// The game camera, driven by the headset.
//
// Everything the engine decides about visibility is built from its camera:
// the object culling frustum through cwTransform2f, and room visibility by
// projecting doorways from the camera position into a fixed 512x448 screen.
// A planar projection cannot represent what is behind it, so no amount of
// widening reaches rooms behind the player -- measured at 0 of 538 doorways
// kept. The camera has to move and turn, and then all of it follows.
//
// The whole head pose lives here: rotation and position together.
//
// It did not always. The first version put yaw on the camera and left pitch
// and roll to the projection, with a residual to cover the frames where the
// camera had not been rebuilt. Every piece of that was individually correct
// and the result was still wrong, because applying pitch in view space after
// the camera has already turned is not the same rotation as turning and
// pitching together. The error is zero looking straight ahead and grows with
// both angles, which is exactly how it felt: fine facing forward, wonky when
// looking up while turned. Splitting a rotation across two places is the bug,
// not any particular sign in it.
//
// CAMERA_CalculateWCTransform (0x0048E100) builds the world-to-camera
// transform from the camera position and its Euler angles, through a builder
// that applies them Z, then Y, then X. Converting a head rotation into that
// convention would mean matching an order and three signs; instead the matrix
// is taken as it comes out and the head rotation multiplied onto it, which
// needs no convention at all. See apply_head_pose.
//
// Gameplay still reads the engine Euler angles, which are restored before
// this returns in third person. Experimental first person keeps headset yaw
// in the camera heading. On the ground, first-person maps left-stick intent
// through that heading into Lara's movement yaw while keeping the view fixed.
// Free-fire is the other deliberate exception: the visual centre
// ray is placed in Camera::targetFocusPoint before the game's own targeting
// update, so its weapon-range and collision code remains authoritative.
#include "camera_head.h"
#include "perf_cpu.h"
#include "cull.h"


#include "hook.h"
#include "tune.h"
#include "ui_space.h"
#include "vr_math.h"
#include "vr_input.h"
#include "vr_session.h"

#include "../common/config.h"
#include "../common/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cfloat>
#include <vector>
#include <intrin.h>

namespace trlvr
{
    namespace
    {
        typedef void(__cdecl* PFN_CalcWC)(void*);
        typedef void(__cdecl* PFN_DrawInstance)(
            void*, int, float, int, int, void*, float, void*, void*);
        typedef void(__cdecl* PFN_GetBoneVB)(void**, int*, unsigned);
        typedef void(__cdecl* PFN_SetIndicesPtr)(void*, void*, float, float);
        typedef int(__cdecl* PFN_GetHMarkerTransform)(
            void*, int, Mat4*, bool);
        typedef void(__cdecl* PFN_EndCombat)();
        typedef void(__thiscall* PFN_FilteredInputUpdate)(
            void*, float, float);
        typedef int(__cdecl* PFN_ProcessMovement)(void*);
        typedef void(__thiscall* PFN_HPoleDesiredDirection)(void*);
        typedef void(__thiscall* PFN_HPoleFastTraverseEntry)(void*, void*, int);
        typedef void*(__thiscall* PFN_StateMessageHandler)(void*, void*,
                                                           void*);
        typedef void(__thiscall* PFN_MarkupNormal)(void*, float*);
        typedef float(__cdecl* PFN_AngleFromVector)(const float*);

        const uintptr_t kCalculateWCTransform = 0x0048E100;
        const uintptr_t kDrawInstance = 0x0040A400;
        const uintptr_t kGetBoneVB = 0x004142E0;
        const unsigned char kGetBoneVBSignature[6] =
            { 0x8B, 0x0D, 0x0C, 0x25, 0x00, 0x01 };
        const uintptr_t kSetIndicesPtr = 0x004142A0;
        const unsigned char kSetIndicesPtrSignature[6] =
            { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
        const uintptr_t kGetHMarkerTransform = 0x00425AD0;
        const uintptr_t kPlayerInvEndCombatMode = 0x005AC4B0;
        const uintptr_t kFilteredInputUpdate = 0x0058A4D0;
        const uintptr_t kProcessMovement = 0x005B90E0;
        const uintptr_t kHPoleDesiredDirection = 0x0055B8C0;
        const uintptr_t kPlayerMovementUpdateReturn = 0x005B9163;
        const uintptr_t kFilteredInputRoot = 0x01117554;
        const unsigned char kEndCombatSignature[5] =
            { 0xA1, 0x3C, 0x71, 0x11, 0x01 };
        // UIFadeGroupTrigger(UIFadeGroupType) 0x004EB1D0: mov eax,[0x010C0A58]
        // (absolute, relocation-free). It turns a HUD fade group on; the
        // group alphas live at 0x011137B8 + 4 * type.
        const uintptr_t kUIFadeGroupTrigger = 0x004EB1D0;
        const unsigned char kFadeTriggerSignature[5] =
            { 0xA1, 0x58, 0x0A, 0x0C, 0x01 };
        // fld dword ptr [esp+4] / push esi: complete, relocation-free
        // instructions covering the five bytes required by hook_install.
        const unsigned char kFilteredInputUpdateSignature[5] =
            { 0xD9, 0x44, 0x24, 0x04, 0x56 };
        PFN_EndCombat g_end_combat = nullptr;
        PFN_FilteredInputUpdate g_filtered_input_update = nullptr;
        PFN_ProcessMovement g_process_movement = nullptr;
        PFN_HPoleDesiredDirection g_hpole_desired_direction = nullptr;
        PFN_HPoleFastTraverseEntry g_hpole_fast_traverse_entry = nullptr;
        // WallVertPoleAttached::MessageHandler: the vine jump decision.
        const uintptr_t kWallVertPoleMessage = 0x0056BD10;
        PFN_StateMessageHandler g_wall_vert_pole_message = nullptr;
        const uintptr_t kMarkupNormal = 0x0045F5B0;
        const uintptr_t kAngleFromVector = 0x0045FB70;
        // HPoleFastTraverse::Entry. mov eax,[esp+4] / push esi: complete,
        // relocation-free instructions covering the five hooked bytes.
        const uintptr_t kHPoleFastTraverseEntry = 0x0056B430;
        const unsigned char kHPoleFastTraverseEntrySignature[5] =
            { 0x8B, 0x44, 0x24, 0x04, 0x56 };

        // push ebp / mov ebp,esp / and esp,0xfffffff0
        const unsigned char kPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };

        // CameraCore::Rotation, a cdc::Euler of three floats in radians. The
        // builder consumes them Z, Y, X, so index 2 is the heading.
        // CameraCore::Position is the first three floats. Terrain and room
        // visibility consult it independently of the view matrices.
        const unsigned kOffPosition      = 0x000;

        // The three world-to-camera transforms the builder writes, and the
        // camera-to-world one that only SetProjDistance2 refreshes.
        const unsigned kOffWcTransform    = 0x120;   // wcTransformf
        const unsigned kOffWcNoShake      = 0x160;   // wcTransformNoShakef
        const unsigned kOffWcTransform2   = 0x1A0;   // wcTransform2f
        const unsigned kOffCwTransform    = 0x1E0;   // cwTransform2f, its inverse

        // Euler angles, still used for levelling the horizon: (pitch, roll,
        // heading), established from the log rather than assumed.
        const unsigned kOffRotation = 0x270;
        const unsigned kOffMode = 0x314;
        const unsigned kOffFocusPoint = 0x330;
        const unsigned kOffFocusDistance = 0x340;
        const unsigned kOffTargetPos = 0x5C0;
        const unsigned kOffTargetFocusPoint = 0x5E0;

        // First-person remains contained here and is entered only after all
        // of these retail-layout checks succeed. Camera::core begins at zero,
        // so the global main Camera can safely be inspected beyond CameraCore.
        const unsigned kOffFocusInstance = 0x488;
        const uintptr_t kPlayerInstance = 0x010E537C;
        const uintptr_t kPlayerProp = 0x01117050;

        const unsigned kOffInstanceMatrices = 0x80;
        // Verified against a live Lara instance and retail ProcessMovement:
        // +0x38 itself is the yaw float. It is not the start of an Euler array.
        const unsigned kOffInstanceHeading = 0x38;
        const unsigned kOffInstanceObject = 0x94;
        const unsigned kOffCurrentRenderModel = 0xB0;
        const unsigned kOffCurrentBaseModel = 0xB1;
        const unsigned kOffNoDrawGroups = 0xDC;
        const unsigned kOffFxNoDrawGroups = 0xE0;

        const unsigned kOffObjectNumModels = 0x18;
        const unsigned kOffObjectModelList = 0x20;
        const unsigned kOffObjectName = 0x48;

        const unsigned kOffModelNumSegments = 0x04;
        // Weighted joint vertices (knuckles, wrist) use virtual segments.
        // They follow the real ones in the segment list, and their blended
        // matrices follow the real ones in the instance matrix array: the
        // retail renderer at 0x00409B55 walks numSegments + numVirtSegments.
        const unsigned kOffModelNumVirtSegments = 0x08;
        const unsigned kOffModelSegments = 0x0C;
        const unsigned kOffModelNumVertices = 0x20;
        const unsigned kOffModelVertices = 0x24;
        const unsigned kOffModelTextureStrips = 0x58;
        const unsigned kOffModelDrawgroupCenters = 0x78;
        // Lara's logged centre table has eight entries. Reading entries
        // beyond seven exposed unrelated allocation data.
        const int kLaraDrawGroups = 8;

        // PlayerProp::oldData begins at +0x30 and
        // OldRazielData::HeadSegment is +0x200.
        const unsigned kOffPlayerPropHeadSegment = 0x230;
        const unsigned kOffPlayerPropLeftShoulder = 0x238;
        const unsigned kOffPlayerPropLeftElbow = 0x23C;
        const unsigned kOffPlayerPropLeftWrist = 0x240;
        const unsigned kOffPlayerPropRightShoulder = 0x24C;
        const unsigned kOffPlayerPropRightElbow = 0x250;
        const unsigned kOffPlayerPropRightWrist = 0x254;

        struct ModelSegment
        {
            float minimum[4];
            float maximum[4];
            float pivot[4];
            int flags;
            short first_vertex;
            short last_vertex;
            int parent;
            void* hierarchy;
        };

        // Retail VirtSegment: blends matrices `index` and `weight_index`.
        struct ModelVirtualSegment
        {
            float minimum[4];
            float maximum[4];
            float pivot[4];
            int flags;
            short first_vertex;
            short last_vertex;
            short index;
            short weight_index;
            float weight;
        };

        struct ShortVector
        {
            short x, y, z, pad;
        };

        static_assert(sizeof(ModelSegment) == 64, "retail Segment layout");
        static_assert(sizeof(ModelVirtualSegment) == 64,
                      "retail VirtSegment layout");
        static_assert(sizeof(ShortVector) == 8, "retail SVector layout");

        // playerUpdateTargetPos is the last owner of the free-fire point used
        // by the weapons. Its mode-0x20 path copies Camera::targetFocusPoint
        // as a direction, extends it to the weapon's range, collision-tests it,
        // and writes the result to PlayerData+0x760. Feed our ray into that
        // calculation; never replace its collision-tested result.
        using PFN_PlayerUpdateTargetPos = void(__cdecl*)(bool);
        const uintptr_t kPlayerUpdateTargetPos = 0x005A7CE0;
        const unsigned char kPlayerUpdateTargetPosSignature[6] =
            { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        PFN_PlayerUpdateTargetPos g_player_update_target = nullptr;

        // The target calculated above is later copied into a per-shot block.
        // Verify and, if necessary, refresh that final input at the weapon
        // boundary so a stale animation/auto-aim point cannot steer the shot.
        using PFN_WeaponFireSingle = void(__cdecl*)(void*, void*);
        const uintptr_t kWeaponFireSingle = 0x0054D6D0;
        const unsigned char kWeaponFireSingleSignature[6] =
            { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        PFN_WeaponFireSingle g_weapon_fire_single = nullptr;
        using PFN_LegacyLineProbe = void(__cdecl*)(uint32_t, void*, void*);
        const PFN_LegacyLineProbe g_legacy_line_probe =
            reinterpret_cast<PFN_LegacyLineProbe>(0x005563A0);
        PFN_LegacyLineProbe g_retail_line_probe = nullptr;
        DWORD g_weapon_fire_thread = 0;
        float g_requested_shot_target[3] = {};

        // Lara's long gun, moved onto the controller in the game's own
        // transforms (G2Instance_BuildTransforms), not only at draw time.
        // The muzzle flash, tracers and bullet origin all come from the
        // gameplay copy; while only the drawn copy moved they appeared at
        // Lara's hidden animated hand (headset test 2026-09-30).
        using PFN_BuildTransforms = void(__cdecl*)(void*);
        const uintptr_t kG2InstanceBuildTransforms = 0x004DFFD0;
        const unsigned char kBuildTransformsSignature[6] =
            { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        PFN_BuildTransforms g_build_transforms = nullptr;
        struct GameplayGun
        {
            void* instance = nullptr;
            float* matrices = nullptr;   // buffer the moved pose was written to
            int count = 0;
            float original[8][16]{};     // retail (animated) pose
            float moved0[16]{};          // segment 0 as written, to detect rebuilds
            Mat4 root = Mat4::identity();// retail world -> controller, for points
            DWORD tick = 0;
            char name[40]{};
        };
        GameplayGun g_gameplay_gun;
        // The two pistols' gameplay copies (left, right), moved the same
        // way while drawn so their muzzle flashes leave the held guns; an
        // undrawn pistol that retail put in Lara's hand (single-pistol
        // mode) is sunk far below the world so its flash is never seen.
        GameplayGun g_gameplay_pistol[2];

        GameplayGun* gameplay_gun_for(void* instance)
        {
            if (!instance)
                return nullptr;
            if (g_gameplay_gun.instance == instance)
                return &g_gameplay_gun;
            for (int side = 0; side < 2; ++side)
                if (g_gameplay_pistol[side].instance == instance)
                    return &g_gameplay_pistol[side];
            return nullptr;
        }

        // Synthetic recoil. Retail's recoil is Lara's arm animation, which
        // the controller-driven hands cancel, and the MP5 model has a single
        // segment, so nothing kicked. Each of Lara's shots now tips the
        // firing controller's hand and gun muzzle-up about the palm and
        // pushes it back along the barrel, decaying in ~0.25 s. Visual only:
        // the aim ray reads the controller pose directly, so the crosshair
        // and bullets are unaffected.
        struct Recoil
        {
            DWORD start = 0;
            float amp = 0.0f;
            float angle = 0.0f;      // radians at amp 1
            float kick = 0.0f;       // game units at amp 1
            float axis[3]{};         // world rotation axis (muzzle rises)
            float back[3]{};         // world direction of the push-back
            bool rotate = false;
        };
        Recoil g_recoil[2];
        bool first_person_controller_world_pose(bool left, Mat4* world);
        bool weapon_is_pistol(const char* name);
        bool weapon_is_long_gun(const char* name);
        void recoil_register_shot(void* weapon);
        bool g_recoil_bypass = false;

        // Two-handed hold of the long gun (the SiN VR mod's design): a fixed
        // foregrip point on the barrel line, engage / release radii with
        // hysteresis, and auto / toggle / hold modes. While held, the
        // trigger hand's pose turns about its palm so the barrel points at
        // the free hand, and Lara's free hand is pinned to the foregrip.
        // Hands, gun, flash and the aim ray all read those poses.
        struct TwoHand
        {
            bool engaged = false;
            bool grip_was = false;
            DWORD zone_tick = 0;     // last time the free hand was in reach
        };
        TwoHand g_two_hand;
        void two_hand_adjust(bool left, const Mat4& camera_to_world,
                             Mat4* world);

        const void* const kMainCamera = reinterpret_cast<const void*>(0x010FC660);
        short g_last_mode = -32768;
        bool g_accurate_aim_active = false;
        int g_forward_candidate = -1;
        bool g_forward_candidate_verified = false;
        float g_head_aim_target[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float g_head_aim_eye[3] = { 0.0f, 0.0f, 0.0f };
        float g_head_aim_direction[3] = { 0.0f, 0.0f, 0.0f };
        bool g_head_aim_target_valid = false;
        DWORD g_head_aim_target_time = 0;
        // g_head_aim_eye/direction came from the right controller this frame.
        bool g_aim_from_controller = false;
        // Where the current shot would land, for the VR crosshair.
        float g_aim_point[3] = { 0.0f, 0.0f, 0.0f };
        bool g_aim_point_hit = false;
        // The probed hit is an enemy by the retail reticle's own test.
        bool g_aim_point_enemy = false;
        // CSenseManager::IsTargetTypeActive(BaseInstance*, uint), thiscall
        // on the sense manager at [0x01117550]. playerDrawCombatLock draws
        // its enemy sprite for types 0, 1, 6 and 7.
        using PFN_IsTargetTypeActive = bool(__thiscall*)(void*, void*,
                                                         unsigned);
        const uintptr_t kIsTargetTypeActive = 0x00563300;
        // Drawn-pistol visibility evidence (goal 2): per-frame draw counts
        // through the linked-weapon path, and the pistol instance's own
        // retail position, which is not moved to the controller.
        unsigned g_pistol_draws[2] = { 0, 0 };
        unsigned g_pistol_prev_draws[2] = { 1, 1 };
        float g_pistol_instance_pos[2][3]{};
        unsigned g_pistol_reports = 0;
        unsigned g_pistol_last_mark = 0;
        DWORD g_aim_point_time = 0;
        // playerDrawCombatLock is the retail combat-reticle entry point. It
        // only draws, so skipping it removes every flat reticle; the time it
        // was last asked to draw tells the VR crosshair when to show.
        using PFN_DrawCombatLock = void(__cdecl*)();
        const uintptr_t kDrawCombatLock = 0x005ABEC0;
        PFN_DrawCombatLock g_draw_combat_lock = nullptr;
        // playerDrawCombatReticle (0x005A9860): the lock-on rings only.
        // playerDrawCombatLock also draws the caution icon and the other
        // sense-target sprites (playerDrawSpriteWorld types 2/3), so first
        // person now blocks just this instead of the whole function.
        using PFN_DrawCombatReticle =
            void(__cdecl*)(const void*, bool, bool, float, float);
        const uintptr_t kDrawCombatReticle = 0x005A9860;
        PFN_DrawCombatReticle g_draw_combat_reticle = nullptr;
        bool g_block_combat_reticle = false;
        DWORD g_combat_reticle_time = 0;
        bool g_combat_reticle_accurate = false;
        // The world point retail would have put its lock-on reticle on
        // (playerDrawCombatReticle's Vector3, which playerFindTargetPosition
        // projects to the screen): the auto-target, for the third-person VR
        // crosshair.
        float g_lock_target[3] = { 0.0f, 0.0f, 0.0f };
        DWORD g_lock_target_time = 0;
        // Third person: the game has a target (crosshair red, else white).
        bool g_lock_on_target = false;

        // Environmental targets (user 2026-10-06: Lara aims at them, no
        // crosshair). playerDrawCombatLock draws its reticle for the combat
        // target only in lock states (PlayerData+0x3E6 == 3 or +0x440 &
        // 0x3000000), so they never reach playerDrawCombatReticle. Fall back
        // to the target Lara is actually aiming at: playerGetCombatTarget
        // (0x005A54A0) and getTargetPosition (0x00588F90, Vector3& out).
        // Guns out in retail terms: playerInvEnterIndicatorMode
        // (0x005AC310) sets PlayerData+0x440 bit 0x01000000, lock mode
        // 0x02000000; playerInvEndCombatMode (0x005AC4B0) clears both. The
        // combat "state" read by retail_combat_state is never zero in
        // third person (log 2026-10-06: 0x01..0x2D while walking), so it
        // cannot say whether the guns are drawn.
        bool retail_weapons_out()
        {
            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                return player &&
                    (*reinterpret_cast<const unsigned*>(player + 0x440) &
                     0x03000000u) != 0;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // PlayerData+0x760: where the weapon will fire (playerUpdateTarget-
        // Pos's result). Zero until a target has been chosen.
        bool weapon_target_position(float out[3])
        {
            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!player)
                    return false;
                const float* p =
                    reinterpret_cast<const float*>(player + 0x760);
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
                    !std::isfinite(p[2]) ||
                    (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f))
                    return false;
                out[0] = p[0];
                out[1] = p[1];
                out[2] = p[2];
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool combat_target_position(float out[3])
        {
            __try
            {
                void* target = reinterpret_cast<void*(__cdecl*)()>(
                    0x005A54A0)();
                if (!target)
                    return false;
                alignas(16) float position[4] = {};
                reinterpret_cast<void(__cdecl*)(float*, void*)>(
                    0x00588F90)(position, target);
                if (!std::isfinite(position[0]) ||
                    !std::isfinite(position[1]) ||
                    !std::isfinite(position[2]))
                    return false;
                out[0] = position[0];
                out[1] = position[1];
                out[2] = position[2];
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
        unsigned g_gameplay_target_updates = 0;

        bool g_first_person_active = false;
        void* g_first_person_instance = nullptr;
        void* g_first_person_model = nullptr;
        uint32_t g_first_person_head_mask = 0;
        uint32_t g_first_person_saved_normal = 0;
        uint32_t g_first_person_saved_fx = 0;
        bool g_first_person_head_hidden = false;
        int g_first_person_head_segment = -1;
        int g_first_person_shoulder_segment[2] = { -1, -1 };
        int g_first_person_elbow_segment[2] = { -1, -1 };
        int g_first_person_wrist_segment[2] = { -1, -1 };
        struct HandStripCache
        {
            unsigned char* data = nullptr;
            unsigned nodes = 0;
            unsigned triangles = 0;
        };
        HandStripCache g_first_person_hand_strips[2];
        // Full-body mode: every triangle except those touching the head
        // subtree (the camera sits inside the head).
        HandStripCache g_first_person_body_strips;
        // How a virtual segment blends its two bones, measured on first use:
        // 0 = (1-w)*index + w*weight_index, 1 = w*index + (1-w)*weight_index.
        int g_virtual_blend_mode = -1;
        bool g_full_body_reported = false;
        void* g_first_person_strip_source = nullptr;
        bool g_first_person_strip_tri_list = false;
        bool g_first_person_hands_reported = false;
        bool g_first_person_hand_unavailable_reported = false;
        bool g_first_person_camera_position_reported = false;
        bool g_first_person_hand_calibrated[2] = { false, false };
        // The hand alignment is a user-facing calibration.  A cinematic
        // temporarily leaves first person, but it must not throw away the
        // calibration and recapture an animated wrist pose when gameplay
        // resumes.  Keep the calibration associated with Lara's model and
        // only invalidate it when the model itself changes.
        void* g_first_person_hand_calibration_model = nullptr;
        Mat4 g_first_person_hand_calibration[2] = {
            Mat4::identity(), Mat4::identity()
        };

        bool g_first_person_anchor_valid = false;
        bool g_first_person_ground_anchor_valid = false;
        float g_first_person_anchor[3] = { 0.0f, 0.0f, 0.0f };
        float g_first_person_last_eye[3] = { 0.0f, 0.0f, 0.0f };
        bool g_first_person_anchor_warned = false;
        // A cinematic can end while Lara is still in a low transition pose.
        // Keep the last standing height for this exact actor/model so re-entry
        // cannot permanently latch that pose as the new eye level.
        void* g_first_person_standing_instance = nullptr;
        void* g_first_person_standing_model = nullptr;
        float g_first_person_standing_eye_z = 0.0f;
        bool g_first_person_standing_eye_valid = false;
        enum FirstPersonTraversal { TraversalGround, TraversalSwim,
                                    TraversalClimb };
        FirstPersonTraversal g_first_person_traversal = TraversalGround;
        // Smoothed view drop for Lara's crouch (game units) and whether her
        // animation is currently crouched.
        float g_first_person_crouch_drop = 0.0f;
        bool g_first_person_crouched = false;
        bool g_first_person_ledge_hanging = false;
        bool g_first_person_bar_hanging = false;
        bool g_first_person_bar_axis_valid = false;
        float g_first_person_bar_axis[2] = { 0.0f, 0.0f };
        float g_first_person_bar_span = 0.0f;
        DWORD g_first_person_bar_entered_at = 0;
        bool g_first_person_ledge_climbing = false;
        bool g_first_person_vine_climbing = false;
        // A one-handed "secure your grip" catch after a long jump: the
        // retail air-attach state is showing its button prompt.
        bool g_first_person_grip_precarious = false;
        // The retail secure-grip prompt object while a precarious catch
        // shows it (LedgeAirAttach +0x18 / HPoleAirAttach +0x1C), its Object
        // (rejects a recycled instance slot) and its first-seen row length.
        void* g_grip_prompt_instance = nullptr;
        void* g_grip_prompt_object = nullptr;
        void* g_grip_prompt_scaled = nullptr;
        float g_grip_prompt_base_row = 0.0f;
        // Current retail player state vtable (0x00F05CEC = JumpState).
        uintptr_t g_first_person_state_vtable = 0;
        // Eye-height diagnostics after a climb ends and during jumps.
        DWORD g_height_diag_until = 0;
        DWORD g_first_person_secure_requested_at = 0;
        DWORD g_auto_secure_at = 0;
        int g_auto_secure_pulses = 0;
        // Horizontal direction of the current ledge/bar handhold box,
        // refreshed by the grab-zone code each frame.
        float g_first_person_hold_axis[2] = { 1.0f, 0.0f };
        bool g_first_person_hold_axis_valid = false;
        uint64_t g_first_person_gpu_draw_count = 0;
        uint64_t g_first_person_frame_gpu_begin = 0;
        uint64_t g_first_person_hand_gpu_first = 0;
        uint64_t g_first_person_hand_gpu_last = 0;
        unsigned g_last_ledge_visibility_mark[2] = { ~0u, ~0u };
        struct HandDrawableTag
        {
            void* drawable;
            int hand;
        };
        HandDrawableTag g_hand_drawables[512]{};
        unsigned g_hand_drawable_count = 0;
        unsigned g_hand_drawable_hits[2]{};
        unsigned g_hand_gpu_draws[2]{};
        unsigned g_hand_cpu_allocations[2]{};
        unsigned g_hand_vb_allocation_reports = 0;
        int g_current_hand_cpu_draw = -1;
        int g_current_gpu_hand_draw = -1;
        unsigned g_hand_vb_report_frames = 0;
        unsigned g_hand_vb_last_report_mark = ~0u;
        // The retail CPU renderer writes 24-byte BONEVERTEX records. Capture
        // its actual transformed output, not just the tracked wrist pose.
        const unsigned char* g_hand_cpu_vertex_buffer = nullptr;
        unsigned g_hand_cpu_vertex_expected_count = 0;
        unsigned g_hand_vertex_trace_mark[2] = { ~0u, ~0u };
        unsigned g_hand_vertex_trace_count[2] = { 0, 0 };
        unsigned g_hand_vertex_trace_total[2] = { 0, 0 };
        DWORD g_hand_vertex_trace_time[2] = { 0, 0 };
        bool g_hand_vertex_ground_reported[2] = { false, false };
        unsigned g_hand_render_state_last_mark = ~0u;
        bool g_ground_hand_render_state_reported = false;
        unsigned g_hand_render_state_trace_remaining = 0;
        int g_hand_render_state_trace_sample = -1;
        bool g_first_person_horizontal_ledge = false;
        struct LedgeHandGrip
        {
            bool was_down = false;
            bool pending = false;
            bool held = false;
            bool in_grab_zone = false;
            DWORD pressed_at = 0;
            DWORD grabbed_at = 0;
            unsigned serial = 0;
            unsigned misses_reported = 0;
            Mat4 world_pose = Mat4::identity();
            float tangent[2] = { 1.0f, 0.0f };
        };
        LedgeHandGrip g_ledge_hand_grip[2];
        unsigned g_ledge_grip_serial = 0;
        bool g_camera_head_initialized = false;
        struct GrabZoneDebug
        {
            bool valid = false;
            float corners[8][3]{};
        };
        GrabZoneDebug g_grab_zone_debug;
        // Centre of the shared grab zone in the head frame (game units),
        // where the mod's own secure-grip badge is drawn (vr_submit).
        bool g_grab_zone_centre_valid = false;
        float g_grab_zone_centre[3]{};
        struct HandholdBox
        {
            float centre[3]{};
            float along[2]{};
            float depth[2]{};
            float half_along = 0.0f;
            float half_depth = 0.0f;
            float half_up = 0.0f;
            bool valid = false;
        };

        bool handhold_box_contains(const HandholdBox& box,
                                   const Mat4& controller, float margin,
                                   float components[3])
        {
            if (!box.valid)
                return false;
            const float dx = controller.m[3][0] - box.centre[0];
            const float dy = controller.m[3][1] - box.centre[1];
            components[0] = dx * box.along[0] + dy * box.along[1];
            components[1] = dx * box.depth[0] + dy * box.depth[1];
            components[2] = controller.m[3][2] - box.centre[2];
            return std::isfinite(components[0]) &&
                std::isfinite(components[1]) &&
                std::isfinite(components[2]) &&
                fabsf(components[0]) <= box.half_along + margin &&
                fabsf(components[1]) <= box.half_depth + margin &&
                fabsf(components[2]) <= box.half_up + margin;
        }
        int g_first_person_vine_pull_hand = -1;

        // The binoculars and grapple are still gameplay-linked to Lara.
        // Their most recent player-facing draw poses let a controller pick up
        // the actual visible objects; a captured rotation follows that hand
        // until the gesture returns the item to its original belt marker.
        Mat4 g_first_person_gear_pose[2] = {
            Mat4::identity(), Mat4::identity()
        };
        Mat4 g_first_person_gear_grip_rotation[2] = {
            Mat4::identity(), Mat4::identity()
        };
        Mat4 g_first_person_gear_belt_local[2] = {
            Mat4::identity(), Mat4::identity()
        };
        DWORD g_first_person_gear_seen[2]{};
        bool g_first_person_gear_valid[2]{};
        bool g_first_person_gear_belt_local_valid[2]{};
        bool g_first_person_gear_held[2]{};
        int g_first_person_gear_hand[2] = { -1, -1 };

        bool g_first_person_turn_ready = false;
        // The headset heading that drives Lara's facing and movement in
        // first person: steady_yaw_of, which stays put looking straight
        // down or up (review 2026-10-02). The rendered view keeps the full
        // head matrix.
        float first_person_hmd_heading()
        {
            return steady_yaw_of(vr_head_rotation());
        }

        float g_first_person_turn_sign = 1.0f;
        // Forward-pitch change per radian of camera Euler pitch (rot[0]),
        // measured from the engine builder; 0 until measured.
        float g_first_person_pitch_scale = 0.0f;

        // Elevation (radians, world Z up) of a row-vector world-to-camera
        // matrix's forward, its column 2.
        float view_pitch_of(const Mat4& m)
        {
            const float f[3] = { m.m[0][2], m.m[1][2], m.m[2][2] };
            const float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            return len > 1e-4f
                ? asinf(fmaxf(-1.0f, fminf(1.0f, f[2] / len))) : 0.0f;
        }
        float g_first_person_base_heading = 0.0f;
        float g_first_person_stick_start = 0.0f;
        float g_first_person_actor_camera_offset = 0.0f;
        bool g_first_person_actor_offset_valid = false;
        // Keep this association through temporary cinematic fallback. The
        // player instance may be recreated while the Lara model stays loaded.
        void* g_first_person_actor_offset_model = nullptr;
        bool g_first_person_actor_reported = false;
        bool g_first_person_movement_reported[4] = { false, false, false, false };
        bool g_first_person_motion_reported[4] = { false, false, false, false };
        bool g_first_person_move_driven = false;
        // Retail combat owns Lara's facing; see first_person_combat_locomotion.
        bool g_first_person_combat_locomotion = false;
        float g_first_person_move_heading = 0.0f;
        DWORD g_first_person_move_report_time = 0;
        unsigned g_first_person_move_reports = 0;
        int g_first_person_probe_sector = -1;
        DWORD g_first_person_probe_start = 0;
        float g_first_person_probe_position[3] = { 0.0f, 0.0f, 0.0f };

        // Both streamed and resident cinematics can keep Lara as the focus
        // instance. Camera mode alone is therefore insufficient to identify
        // an authored cutscene.
        typedef bool(__cdecl* PFN_CinematicPlaying)();
        const uintptr_t kCinematicPlaying = 0x0041E6B0;
        const uintptr_t kResidentCinematicPlaying = 0x00464740;

        PFN_DrawInstance g_draw_instance = nullptr;
        // Third-person terrain camera position swap (see core_position_restore).
        bool g_core_swapped = false;
        float g_core_saved[3]{};
        float g_core_written[3]{};
        // The game's own camera position this frame (the sky dome is pinned
        // to it), read at the main camera build before we write ours --
        // first person writes the eye there every frame, so "the current
        // value" was ours, not the game's (sky slid in first-person aim,
        // 2026-10-03).
        float g_game_camera[3]{};
        float g_core_last_write[3]{};
        bool g_core_last_write_valid = false;
        PFN_GetBoneVB g_get_bone_vb = nullptr;
        PFN_SetIndicesPtr g_set_indices_ptr = nullptr;

        void __cdecl detour_get_bone_vb(void** vertices, int* base,
                                         unsigned count)
        {
            if (!g_get_bone_vb)
                return;
            g_get_bone_vb(vertices, base, count);
            if (g_current_hand_cpu_draw >= 0 &&
                g_current_hand_cpu_draw < 2 && base && vertices &&
                *vertices && *base >= 0 && count > 0 && count <= 65535)
            {
                ++g_hand_cpu_allocations[g_current_hand_cpu_draw];
                if (count == g_hand_cpu_vertex_expected_count &&
                    !g_hand_cpu_vertex_buffer)
                    g_hand_cpu_vertex_buffer =
                        static_cast<const unsigned char*>(*vertices);
                if (g_first_person_ledge_hanging &&
                    g_hand_vb_allocation_reports < 8)
                {
                    log("render: %s hand CPU VB allocation base=%d count=%u",
                        g_current_hand_cpu_draw == 0 ? "left" : "right",
                        *base, count);
                    ++g_hand_vb_allocation_reports;
                }
            }
        }

        void __cdecl detour_set_indices_ptr(void* drawable, void* indices,
                                            float a3, float a4)
        {
            if (!g_set_indices_ptr)
                return;
            g_set_indices_ptr(drawable, indices, a3, a4);
            if (!drawable)
                return;
            // This drawable may be reused by the retail renderer. Its most
            // recent CPU submission decides whether it belongs to a hand.
            for (unsigned i = 0; i < g_hand_drawable_count; ++i)
            {
                if (g_hand_drawables[i].drawable == drawable)
                {
                    g_hand_drawables[i].hand = g_current_hand_cpu_draw;
                    return;
                }
            }
            if (g_current_hand_cpu_draw >= 0 &&
                g_hand_drawable_count >= 512)
                g_hand_drawable_count = 0; // stale pool: start over
            if (g_current_hand_cpu_draw >= 0 &&
                g_hand_drawable_count < 512)
                g_hand_drawables[g_hand_drawable_count++] =
                    { drawable, g_current_hand_cpu_draw };
        }

        // CAMERA_CalcAutoCenterData(Camera*, float* multiplier, float* maxSpeed)
        // hands GenericCameraProcess -- its only caller -- the two values that
        // swing the camera back behind Lara as she moves. The PDB names them
        // followAutoCenterMultiplier and followAutoCenterMaxSpeed, at
        // Camera+0x3D8 and +0x3DC.
        //
        // Zeroing them at this boundary stops the camera chasing the player
        // without touching the struct, the mouse, or anything the camera does
        // when you actually ask it to turn. In a headset a view that swings on
        // its own while you are walking is motion you did not make, which is
        // the kind that makes people ill.
        typedef void(__cdecl* PFN_AutoCenter)(void*, float*, float*);

        const uintptr_t kCalcAutoCenterData = 0x004807A0;

        // sub esp,0x14 / mov eax,[0x010C0A58] -- eight bytes, ending on an
        // instruction boundary.
        const unsigned char kAutoCenterPrologue[8] =
            { 0x83, 0xEC, 0x14, 0xA1, 0x58, 0x0A, 0x0C, 0x01 };

        // Camera shake -- a boulder landing, an explosion -- moves the view
        // without the player moving their head. On a monitor that is weight;
        // in a headset it is the world lurching while your inner ear says
        // nothing happened, which is about the most direct way there is to
        // make someone ill.
        //
        // CAMERA_SetShakeNoPadshock is the choke point: every route to shake
        // arrives here, including CAMERA_SetShake and CAMERA_SetShakeRamp,
        // which both call it. Refusing it removes all of them at once.
        //
        // Usefully, the *pad* rumble is triggered by CAMERA_SetShake before it
        // delegates here -- so hooking the inner function keeps the controller
        // shaking while the picture stays still, which is exactly the split
        // you want. The boulder still feels like a boulder.
        typedef void(__cdecl* PFN_SetShake)(void*, int, int);

        const uintptr_t kSetShakeNoPadshock = 0x0047E410;

        // push ecx / fild dword ptr [esp+0x10] -- five bytes exactly, and an
        // instruction boundary.
        const unsigned char kShakePrologue[5] =
            { 0x51, 0xDB, 0x44, 0x24, 0x10 };

        PFN_SetShake g_setshake = nullptr;
        unsigned g_shakes_refused = 0;

        PFN_AutoCenter g_autocenter = nullptr;

        PFN_CalcWC g_original = nullptr;

        unsigned g_calls = 0;
        unsigned g_last_report = 0;

        float normalise3(float* v)
        {
            const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (n > 0.0f)
                for (int i = 0; i < 3; ++i) v[i] /= n;
            return n;
        }

        float dot3(const float* a, const float* b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        void direction_candidate(const Mat4& m, int candidate, float* out)
        {
            const int axis = (candidate / 2) % 3;
            const bool row = candidate >= 6;
            const float sign = (candidate & 1) ? -1.0f : 1.0f;
            for (int i = 0; i < 3; ++i)
                out[i] = sign * (row ? m.m[axis][i] : m.m[i][axis]);
        }

        void aim_angles(const float* direction, float* elevation, float* heading)
        {
            float d[3] = { direction[0], direction[1], direction[2] };
            normalise3(d);
            if (d[2] < -1.0f) d[2] = -1.0f;
            if (d[2] >  1.0f) d[2] =  1.0f;
            *elevation = acosf(d[2]);
            *heading = atan2f(d[1], d[0]) - 1.57079633f;
            const float two_pi = 6.28318531f;
            while (*heading < 0.0f) *heading += two_pi;
            while (*heading >= two_pi) *heading -= two_pi;
        }

        bool laras_long_gun_held();
        int retail_combat_state();
        void climb_anim_watch(void* lara, uintptr_t vtable, bool precarious,
                              bool first_person = true);

        // Both pistols are in hand (akimbo, or single mode with both drawn).
        bool both_pistols_drawn()
        {
            if (!g_first_person_active || laras_long_gun_held())
                return false;
            if (config().immersive_controls)
                return vr_input_holster_hand_drawn(true) &&
                       vr_input_holster_hand_drawn(false);
            return retail_combat_state() != 0;
        }

        // The hand the aim ray (and crosshair) follows: the handedness
        // setting, except in single-pistol mode with exactly one pistol
        // drawn, where it is that pistol's hand.
        bool aim_hand_left()
        {
            if (config().single_pistols && config().immersive_controls &&
                g_first_person_active && !laras_long_gun_held())
            {
                const bool l = vr_input_holster_hand_drawn(true);
                const bool r = vr_input_holster_hand_drawn(false);
                if (l != r)
                    return l;
            }
            return config().left_handed;
        }

        // The grip-model aim ray, converted once from the original right-hand
        // aim (raw pose) so the tuned direction carries over:
        // d_grip = d_raw * inv(rot(O)).
        bool first_person_grip_aim_ready()
        {
            if (tune_grip_aim(nullptr, nullptr))
                return vr_controller_grip_offset(aim_hand_left(), nullptr);
            Mat4 offset;
            if (!vr_controller_grip_offset(false, &offset))
                return false;
            // The original RIGHT-hand values, whatever the handedness now.
            float pitch = 0.0f, yaw = 0.0f;
            tune_aim_offset_hand(false, &pitch, &yaw);
            pitch *= 0.0174532925f;
            yaw *= 0.0174532925f;
            const float raw[3] = { sinf(yaw) * cosf(pitch), -sinf(pitch),
                                   cosf(yaw) * cosf(pitch) };
            float grip[3];
            for (int j = 0; j < 3; ++j)
                grip[j] = raw[0] * offset.m[j][0] + raw[1] * offset.m[j][1] +
                          raw[2] * offset.m[j][2];
            const float new_yaw = atan2f(grip[0], grip[2]) * 57.2957795f;
            const float new_pitch =
                asinf(fmaxf(-1.0f, fminf(1.0f, -grip[1]))) * 57.2957795f;
            if (!tune_set_grip_aim(new_pitch, new_yaw))
                return false;
            log("aim: grip-pose aim converted from the original right-hand "
                "aim: pitch %+.1f, yaw %+.1f degrees", new_pitch, new_yaw);
            return vr_controller_grip_offset(aim_hand_left(), nullptr);
        }

        void publish_head_aim(void* cam, const Mat4& reference,
                              const Mat4& visual)
        {
            if (!config().hmd_aim || cam != kMainCamera)
                return;
            const short mode = *reinterpret_cast<const short*>(
                (const unsigned char*)cam + kOffMode);
            if (mode != 13 && !camera_first_person_active())
                return;

            // The view is a row-vector world-to-camera matrix. Its +Z column
            // is the world-space centre ray. The retail focus ray below is a
            // gameplay target, not a camera basis reference: matching it
            // selected row +Z, which kept the shot heading near 180 degrees
            // even as the HMD camera turned.
            const float* origin = reinterpret_cast<const float*>(
                (const unsigned char*)cam + kOffTargetPos);
            const float* old_target = reinterpret_cast<const float*>(
                (const unsigned char*)cam + kOffTargetFocusPoint);
            float source[3] = {
                old_target[0] - origin[0],
                old_target[1] - origin[1],
                old_target[2] - origin[2]
            };
            float target_distance = normalise3(source);
            if (target_distance < 0.001f && mode == 13)
                return;
            if (target_distance < 0.001f)
                target_distance = 2000.0f;

            const bool first_person_aim = camera_first_person_active();
            float match = -2.0f;
            if (!first_person_aim && mode == 13 && !g_forward_candidate_verified)
            {
                int best = 4;
                for (int candidate = 0; candidate < 12; ++candidate)
                {
                    float direction[3];
                    direction_candidate(reference, candidate, direction);
                    normalise3(direction);
                    const float score = dot3(direction, source);
                    if (score > match)
                    {
                        match = score;
                        best = candidate;
                    }
                }
                // A rigid camera basis must provide a close match. Refuse to
                // steer gameplay if the executable's layout is not what the
                // verified symbols and disassembly say it is.
                if (match < 0.95f)
                {
                    static bool warned = false;
                    if (!warned)
                    {
                        warned = true;
                        log("aim: target-ray basis did not verify (best dot %.3f); "
                            "gameplay target left alone", match);
                    }
                    return;
                }
                g_forward_candidate = best;
                g_forward_candidate_verified = true;
                log("aim: target ray locked to camera basis %s %c%d (dot %.4f)",
                    best >= 6 ? "row" : "column", (best & 1) ? '-' : '+',
                    (best / 2) % 3, match);
            }
            float direction[3], elevation = 0.0f, heading = 0.0f;
            const int forward_basis = first_person_aim ? 4 : g_forward_candidate;
            Mat4 camera_to_world{};
            if (!invert(visual, &camera_to_world))
                return;
            if (first_person_aim)
            {
                // The inverse's +Z row is the exact world-space centre ray,
                // even if a camera rebuild left small scale/shear in the view.
                for (int i = 0; i < 3; ++i)
                    direction[i] = camera_to_world.m[2][i];
            }
            else
                direction_candidate(visual, forward_basis, direction);
            float aim_origin[3] = {
                camera_to_world.m[3][0], camera_to_world.m[3][1],
                camera_to_world.m[3][2]
            };
            // The headset centre ray, kept for aim_mode hands_head.
            float head_direction[3], head_origin[3];
            for (int i = 0; i < 3; ++i)
            {
                head_direction[i] = direction[i];
                head_origin[i] = aim_origin[i];
            }
            g_aim_from_controller = false;
            Mat4 controller_to_head;
            const bool grip_aim = first_person_aim &&
                config().controller_aim && config().hand_model == 2 &&
                first_person_grip_aim_ready();
            if (first_person_aim && config().controller_aim &&
                (grip_aim
                    ? vr_controller_grip_head_pose(aim_hand_left(),
                                                   &controller_to_head)
                    : vr_controller_head_pose(aim_hand_left(),
                                              &controller_to_head)))
            {
                // Right controller, raw tracked pose (no hand offset), so the
                // ray does not move when the hand model is re-tuned. Row 2 is
                // the controller's forward axis in the engine convention, the
                // same convention the camera's row 2 uses for its centre ray.
                // aim_pitch/aim_yaw rotate it in controller space.
                Mat4 controller = controller_to_head * camera_to_world;
                two_hand_adjust(config().left_handed, camera_to_world,
                                &controller);
                float pitch = 0.0f, yaw = 0.0f;
                if (grip_aim)
                {
                    tune_grip_aim(&pitch, &yaw);
                    if (aim_hand_left())
                        yaw = -yaw; // the left grip is the mirror image
                }
                else
                    tune_aim_offset(&pitch, &yaw);
                pitch *= 0.0174532925f;
                yaw *= 0.0174532925f;
                // Engine local axes: X right, Y down, Z forward.
                const float local[3] = {
                    sinf(yaw) * cosf(pitch), -sinf(pitch),
                    cosf(yaw) * cosf(pitch)
                };
                float controller_direction[3]{};
                for (int i = 0; i < 3; ++i)
                    controller_direction[i] =
                        local[0] * controller.m[0][i] +
                        local[1] * controller.m[1][i] +
                        local[2] * controller.m[2][i];
                bool valid = normalise3(controller_direction) > 0.5f;
                for (int i = 0; i < 3; ++i)
                    valid = valid && std::isfinite(controller.m[3][i]) &&
                            std::isfinite(controller_direction[i]);
                if (valid)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        aim_origin[i] = controller.m[3][i];
                        direction[i] = controller_direction[i];
                    }
                    g_aim_from_controller = true;
                }
                // aim_mode: blend the other pistol hand and/or the headset
                // into the ray (equal weights, unit directions averaged).
                const int aim_mode = config().aim_mode;
                if (valid && aim_mode != 0)
                {
                    float sum_dir[3], sum_origin[3];
                    int rays = 1;
                    for (int i = 0; i < 3; ++i)
                    {
                        sum_dir[i] = direction[i];
                        sum_origin[i] = aim_origin[i];
                    }
                    const bool main_left = aim_hand_left();
                    Mat4 other_to_head;
                    if (both_pistols_drawn() &&
                        (grip_aim
                            ? vr_controller_grip_head_pose(!main_left,
                                                           &other_to_head)
                            : vr_controller_head_pose(!main_left,
                                                      &other_to_head)))
                    {
                        const Mat4 other = other_to_head * camera_to_world;
                        float op = 0.0f, oy = 0.0f;
                        if (grip_aim)
                        {
                            tune_grip_aim(&op, &oy);
                            if (!main_left)
                                oy = -oy; // the other hand is the left one
                        }
                        else
                            tune_aim_offset_hand(!main_left, &op, &oy);
                        op *= 0.0174532925f;
                        oy *= 0.0174532925f;
                        const float ol[3] = {
                            sinf(oy) * cosf(op), -sinf(op),
                            cosf(oy) * cosf(op)
                        };
                        float od[3];
                        for (int i = 0; i < 3; ++i)
                            od[i] = ol[0] * other.m[0][i] +
                                    ol[1] * other.m[1][i] +
                                    ol[2] * other.m[2][i];
                        bool ok = normalise3(od) > 0.5f;
                        for (int i = 0; i < 3; ++i)
                            ok = ok && std::isfinite(od[i]) &&
                                 std::isfinite(other.m[3][i]);
                        if (ok)
                        {
                            for (int i = 0; i < 3; ++i)
                            {
                                sum_dir[i] += od[i];
                                sum_origin[i] += other.m[3][i];
                            }
                            ++rays;
                        }
                    }
                    if (aim_mode == 2)
                    {
                        float hd[3] = { head_direction[0], head_direction[1],
                                        head_direction[2] };
                        if (normalise3(hd) > 0.5f)
                        {
                            for (int i = 0; i < 3; ++i)
                            {
                                sum_dir[i] += hd[i];
                                sum_origin[i] += head_origin[i];
                            }
                            ++rays;
                        }
                    }
                    if (rays > 1 && normalise3(sum_dir) > 1.0e-3f)
                        for (int i = 0; i < 3; ++i)
                        {
                            direction[i] = sum_dir[i];
                            aim_origin[i] = sum_origin[i] / rays;
                        }
                    static int reported_rays = 0;
                    if (rays != reported_rays)
                    {
                        reported_rays = rays;
                        log("aim: aim_mode %s blends %d ray%s", aim_mode == 2
                            ? "hands_head" : "hands", rays,
                            rays == 1 ? "" : "s");
                    }
                }
            }
            normalise3(direction);
            aim_angles(direction, &elevation, &heading);
            for (int i = 0; i < 3; ++i)
            {
                g_head_aim_eye[i] = aim_origin[i];
                g_head_aim_direction[i] = direction[i];
            }

            float target[4] = {
                origin[0], origin[1], origin[2], old_target[3]
            };
            for (int i = 0; i < 3; ++i)
                target[i] = (g_aim_from_controller ? aim_origin[i]
                                                   : origin[i]) +
                            direction[i] * target_distance;

            memcpy(g_head_aim_target, target, sizeof(g_head_aim_target));
            g_head_aim_target_valid = true;
            g_head_aim_target_time = GetTickCount();

            const float degrees = 57.2957795f;
            static DWORD last = 0;
            const DWORD now = GetTickCount();
            if (!last || now - last >= 5000)
            {
                last = now;
                if (first_person_aim)
                    log("aim: %s target published -> "
                        "elevation %.1f heading %.1f, range %.1f",
                        g_aim_from_controller
                            ? (aim_hand_left() ? "left-controller"
                                                    : "right-controller")
                                              : "headset centre-ray",
                        elevation * degrees, heading * degrees,
                        target_distance);
                else
                    log("aim: headset target published (basis %s %c%d) -> "
                        "elevation %.1f heading %.1f, range %.1f",
                        forward_basis >= 6 ? "row" : "column",
                        (forward_basis & 1) ? '-' : '+',
                        (forward_basis / 2) % 3,
                        elevation * degrees, heading * degrees,
                        target_distance);
            }
        }

        float aim_vertical_slope(const float* point);
        float aim_vertical_slope_at(const float* point, unsigned view_offset);

        float aim_weapon_range()
        {
            float range = 12000.0f;
            __try
            {
                const unsigned char* weapon_info =
                    *reinterpret_cast<unsigned char* const*>(0x01117050);
                if (weapon_info)
                    range = *reinterpret_cast<const float*>(
                        weapon_info + 0x814);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                range = 12000.0f;
            }
            return std::isfinite(range) && range >= 100.0f &&
                   range <= 50000.0f ? range : 12000.0f;
        }

        bool aim_instance_is_enemy(void* instance)
        {
            if (!instance)
                return false;
            bool enemy = false;
            const char* name = nullptr;
            __try
            {
                void* senses =
                    *reinterpret_cast<void* const*>(0x01117550);
                const PFN_IsTargetTypeActive active =
                    reinterpret_cast<PFN_IsTargetTypeActive>(
                        kIsTargetTypeActive);
                if (senses)
                {
                    const unsigned types[4] = { 0, 1, 6, 7 };
                    for (unsigned type : types)
                        enemy = enemy || active(senses, instance, type);
                }
                const unsigned char* object =
                    *reinterpret_cast<unsigned char* const*>(
                        static_cast<unsigned char*>(instance) +
                        kOffInstanceObject);
                if (object)
                    name = *reinterpret_cast<const char* const*>(
                        object + kOffObjectName);
                if (name && IsBadStringPtrA(name, 64))
                    name = nullptr;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                enemy = false;
                name = nullptr;
            }
            // Bounded evidence: which instances the controller ray meets and
            // how the retail target test classifies them.
            static void* seen[24]{};
            static unsigned seen_count = 0;
            bool known = false;
            for (unsigned i = 0; i < seen_count; ++i)
                known = known || seen[i] == instance;
            if (!known && seen_count < 24)
            {
                seen[seen_count++] = instance;
                log("aim: controller ray hit instance %p \"%s\" -> %s",
                    instance, name ? name : "?",
                    enemy ? "enemy (red crosshair)" : "not a target");
            }
            return enemy;
        }

        // The same retail MULTIBODY_LegacyLineProbe call (flags, PCollideInfo
        // layout, Lara ignored) the accurate-aim close-surface probe uses.
        // Writes the first hit, or the end of the ray at weapon range.
        bool aim_probe(const float* origin, const float* direction,
                       float range, float* point, void** hit_instance)
        {
            if (hit_instance)
                *hit_instance = nullptr;
            for (int i = 0; i < 3; ++i)
                point[i] = origin[i] + direction[i] * range;
            bool hit = false;
            __try
            {
                const void* owner =
                    *reinterpret_cast<void* const*>(kPlayerInstance);
                alignas(16) unsigned char probe[0x60] = {};
                float* start = reinterpret_cast<float*>(probe + 0x30);
                float* end = reinterpret_cast<float*>(probe + 0x40);
                for (int i = 0; i < 3; ++i)
                {
                    start[i] = origin[i];
                    end[i] = point[i];
                }
                start[3] = end[3] = 1.0f;
                *reinterpret_cast<uint32_t*>(probe + 0x50) = 0xF;
                *reinterpret_cast<const void**>(probe + 0x54) = owner;
                g_legacy_line_probe(0x45AC1983, probe,
                                    const_cast<void*>(owner));
                if (*reinterpret_cast<const int*>(probe + 0x10) != 0)
                {
                    float offset[3]{};
                    for (int i = 0; i < 3; ++i)
                        offset[i] = end[i] - origin[i];
                    const float distance = sqrtf(dot3(offset, offset));
                    hit = std::isfinite(distance) && distance > 4.0f &&
                          distance <= range + 1.0f &&
                          std::isfinite(end[0]) && std::isfinite(end[1]) &&
                          std::isfinite(end[2]);
                    if (hit)
                        for (int i = 0; i < 3; ++i)
                            point[i] = end[i];
                    // PCollideInfo::inst (+0x1C, PDB).
                    if (hit && hit_instance)
                        *hit_instance =
                            *reinterpret_cast<void**>(probe + 0x1C);
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                hit = false;
            }
            return hit;
        }

        void __cdecl detour_player_update_target_body(bool immediate);
        void __cdecl detour_player_update_target(bool immediate)
        {
            perf_cpu_begin(PerfAimTarget);
            detour_player_update_target_body(immediate);
            perf_cpu_end(PerfAimTarget);
        }

        void __cdecl detour_player_update_target_body(bool immediate)
        {
            bool applied = false;
            bool applied_range_ray = false;
            float saved_camera_target[4] = {};
            alignas(16) float saved_near[4] = {};
            alignas(16) float saved_far[4] = {};
            float prior_ray_dot = 0.0f;
            float prior_near_offset = 0.0f;
            if (config().hmd_aim &&
                (g_accurate_aim_active || camera_first_person_active()) &&
                g_head_aim_target_valid &&
                GetTickCount() - g_head_aim_target_time <= 100)
            {
                __try
                {
                    memcpy(saved_camera_target,
                           (const unsigned char*)kMainCamera +
                               kOffTargetFocusPoint,
                           sizeof(saved_camera_target));
                    memcpy((unsigned char*)kMainCamera + kOffTargetFocusPoint,
                           g_head_aim_target, sizeof(g_head_aim_target));
                    applied = true;
                    if (camera_first_person_active())
                    {
                        // The retail free-aim path at 005A8058 builds its
                        // collision ray from these aligned near/far points,
                        // bypassing Camera::targetFocusPoint entirely.
                        float* near_point = reinterpret_cast<float*>(
                            0x010FCC20);
                        float* far_point = reinterpret_cast<float*>(
                            0x010FCC40);
                        memcpy(saved_near, near_point, sizeof(saved_near));
                        memcpy(saved_far, far_point, sizeof(saved_far));
                        float old_direction[3] = {
                            saved_far[0] - saved_near[0],
                            saved_far[1] - saved_near[1],
                            saved_far[2] - saved_near[2]
                        };
                        const float ray_length = normalise3(old_direction);
                        if (std::isfinite(ray_length) &&
                            ray_length > 1.0f && ray_length < 100000.0f)
                        {
                            prior_ray_dot = dot3(old_direction,
                                                 g_head_aim_direction);
                            float offset[3] = {
                                saved_near[0] - g_head_aim_eye[0],
                                saved_near[1] - g_head_aim_eye[1],
                                saved_near[2] - g_head_aim_eye[2]
                            };
                            prior_near_offset = sqrtf(dot3(offset, offset));
                            for (int i = 0; i < 3; ++i)
                            {
                                near_point[i] = g_head_aim_eye[i];
                                far_point[i] = g_head_aim_eye[i] +
                                    g_head_aim_direction[i] * ray_length;
                            }
                            applied_range_ray = true;
                        }
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    g_head_aim_target_valid = false;
                }
            }

            // Let retail update its gameplay state before selecting the
            // first-person firing point. It may use an actor-heading and
            // height-bias branch instead of the near/far collision ray.
            g_player_update_target(immediate);

            if (applied && camera_first_person_active())
            {
                __try
                {
                    unsigned char* player = *reinterpret_cast<unsigned char**>(
                        0x0111713C);
                    float* result = reinterpret_cast<float*>(player + 0x760);
                    float from_eye[3] = {
                        result[0] - g_head_aim_eye[0],
                        result[1] - g_head_aim_eye[1],
                        result[2] - g_head_aim_eye[2]
                    };
                    float depth = dot3(from_eye, g_head_aim_direction);
                    if (g_aim_from_controller)
                    {
                        // The retail target's depth belongs to its own ray
                        // (lock-on or view), not the gun's. Probe the
                        // controller ray and aim at what it actually meets.
                        float point[3]{};
                        void* hit_instance = nullptr;
                        g_aim_point_hit = aim_probe(g_head_aim_eye,
                            g_head_aim_direction, aim_weapon_range(), point,
                            &hit_instance);
                        g_aim_point_enemy = aim_instance_is_enemy(
                            hit_instance);
                        for (int i = 0; i < 3; ++i)
                            result[i] = g_aim_point[i] = point[i];
                        g_aim_point_time = GetTickCount();
                        static DWORD last_report = 0;
                        const DWORD now = GetTickCount();
                        if (!last_report || now - last_report >= 5000)
                        {
                            last_report = now;
                            float offset[3]{};
                            for (int i = 0; i < 3; ++i)
                                offset[i] = point[i] - g_head_aim_eye[i];
                            log("aim: controller ray %s at %.1f units "
                                "(retail target depth was %.1f)",
                                g_aim_point_hit ? "hit" : "clear to range",
                                sqrtf(dot3(offset, offset)), depth);
                        }
                    }
                    else if (std::isfinite(depth))
                    {
                        if (depth < 100.0f) depth = 2000.0f;
                        if (depth > 20000.0f) depth = 20000.0f;
                        const float before = aim_vertical_slope(result);
                        for (int i = 0; i < 3; ++i)
                            result[i] = g_aim_point[i] = g_head_aim_eye[i] +
                                        g_head_aim_direction[i] * depth;
                        g_aim_point_hit = false;
                        g_aim_point_enemy = false;
                        g_aim_point_time = GetTickCount();
                        static DWORD last_alignment_report = 0;
                        const DWORD now = GetTickCount();
                        if (!last_alignment_report ||
                            now - last_alignment_report >= 5000)
                        {
                            last_alignment_report = now;
                            log("aim: final gameplay target centred, "
                                "unscaled view Y/Z %.3f -> %.3f, scaled "
                                "view %.3f at depth %.1f",
                                before, aim_vertical_slope(result),
                                aim_vertical_slope_at(result,
                                                      kOffWcTransform), depth);
                        }
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    g_head_aim_target_valid = false;
                }
            }
            else if (applied && config().vr_crosshair)
            {
                // Third-person precision aim (2026-10-03): the retail centre
                // crosshair is a flat HUD sprite, dragged by hud_follow, so
                // it sat beside the hits whenever the head was off centre
                // (user screenshot). Put the VR crosshair on the target
                // retail just chose -- where the shot goes -- and colour it
                // by what the eye-to-target ray meets.
                __try
                {
                    const unsigned char* player =
                        *reinterpret_cast<unsigned char* const*>(0x0111713C);
                    const float* result =
                        reinterpret_cast<const float*>(player + 0x760);
                    float ray[3] = { result[0] - g_head_aim_eye[0],
                                     result[1] - g_head_aim_eye[1],
                                     result[2] - g_head_aim_eye[2] };
                    const float reach = normalise3(ray);
                    if (std::isfinite(reach) && reach > 1.0f)
                    {
                        float point[3]{};
                        void* hit_instance = nullptr;
                        g_aim_point_hit = aim_probe(g_head_aim_eye, ray,
                            reach + 50.0f, point, &hit_instance);
                        g_aim_point_enemy = g_aim_point_hit &&
                            aim_instance_is_enemy(hit_instance);
                        for (int i = 0; i < 3; ++i)
                            g_aim_point[i] = result[i];
                        g_aim_point_time = GetTickCount();
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }

            // targetFocusPoint also belongs to the accurate-aim camera. Leaving
            // the headset ray there made its focus distance collapse from about
            // 1850 to the first collision (319.6 in the confirming log), which
            // changed the camera geometry even though the shot was right. The
            // gameplay routine only needs the value for the duration of this
            // call, so put the camera's own target straight back afterwards.
            if (applied)
            {
                __try
                {
                    memcpy((unsigned char*)kMainCamera + kOffTargetFocusPoint,
                           saved_camera_target, sizeof(saved_camera_target));
                    if (applied_range_ray)
                    {
                        memcpy(reinterpret_cast<void*>(0x010FCC20),
                               saved_near, sizeof(saved_near));
                        memcpy(reinterpret_cast<void*>(0x010FCC40),
                               saved_far, sizeof(saved_far));
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    g_head_aim_target_valid = false;
                }
            }

            if (applied)
            {
                ++g_gameplay_target_updates;
                static DWORD last = 0;
                const DWORD now = GetTickCount();
                if (!last || now - last >= 5000)
                {
                    last = now;
                    log("aim: headset ray fed through %u gameplay range/collision "
                        "updates", g_gameplay_target_updates);
                    if (camera_first_person_active())
                        log("aim: retail ray vs headset dot %.3f, near-eye "
                            "offset %.1f units, range-ray override %d, "
                            "camera mode %d",
                            prior_ray_dot, prior_near_offset,
                            applied_range_ray ? 1 : 0,
                            g_accurate_aim_active ? 13 : 0);
                    g_gameplay_target_updates = 0;
                }
            }
        }

        float aim_vertical_slope_at(const float* point, unsigned view_offset)
        {
            Mat4 view;
            memcpy(&view.m[0][0],
                   static_cast<const unsigned char*>(kMainCamera) +
                       view_offset, sizeof(view.m));
            const float y = point[0] * view.m[0][1] +
                            point[1] * view.m[1][1] +
                            point[2] * view.m[2][1] + view.m[3][1];
            const float z = point[0] * view.m[0][2] +
                            point[1] * view.m[1][2] +
                            point[2] * view.m[2][2] + view.m[3][2];
            return z > 1.0f ? y / z : 999.0f;
        }

        float aim_vertical_slope(const float* point)
        {
            // wcTransformf has viewport scales baked into its basis. The
            // unscaled wcTransform2f is the physical camera pose used for the
            // first-person ray and for this diagnostic projection.
            return aim_vertical_slope_at(point, kOffWcTransform2);
        }

        void __cdecl detour_legacy_line_probe(uint32_t flags, void* info,
                                               void* ignored_instance)
        {
            PerfCpuScope perf_scope(PerfLineProbe);
            const bool weapon_probe = g_weapon_fire_thread ==
                    GetCurrentThreadId() && flags == 0x472C19A3 &&
                    ignored_instance ==
                        *reinterpret_cast<void* const*>(kPlayerInstance);
            float start[3] = {}, end[3] = {};
            if (weapon_probe)
            {
                memcpy(start, static_cast<unsigned char*>(info) + 0x30,
                       sizeof(start));
                memcpy(end, static_cast<unsigned char*>(info) + 0x40,
                       sizeof(end));
            }
            g_retail_line_probe(flags, info, ignored_instance);
            if (weapon_probe)
            {
                static unsigned reports = 0;
                if (reports++ < 48)
                {
                    const float* impact = reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(info) + 0x40);
                    const int hit_type = *reinterpret_cast<const int*>(
                        static_cast<const unsigned char*>(info) + 0x10);
                    float to_eye[3], end_error[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        to_eye[i] = start[i] - g_head_aim_eye[i];
                        end_error[i] = end[i] - g_requested_shot_target[i];
                    }
                    log("aim: retail bullet ray start-eye %.1f, end-target "
                        "%.1f, end view Y/Z %.3f, hit type %d view Y/Z %.3f",
                        sqrtf(dot3(to_eye, to_eye)),
                        sqrtf(dot3(end_error, end_error)),
                        aim_vertical_slope(end), hit_type,
                        aim_vertical_slope(impact));
                }
            }
        }

        // Which hand a Lara-linked pistol belongs to. Holstered, the left
        // pistol is on hip marker 4 and the right on 6 (the holster draw
        // relies on that); drawn, retail moves them to hand markers 5 / 7,
        // whose pairing was guessed wrong once (headset test: the wrong
        // pistol fired in single mode). So each instance's side is learnt
        // from its hip marker and remembered by identity.
        void* g_pistol_instance[2] = { nullptr, nullptr };

        int pistol_side(void* weapon)
        {
            __try
            {
                unsigned char* inst = static_cast<unsigned char*>(weapon);
                if (*reinterpret_cast<void**>(inst + 0xB8) !=
                    g_first_person_instance || !g_first_person_instance)
                    return -1;
                const unsigned char* object =
                    *reinterpret_cast<unsigned char* const*>(
                        inst + kOffInstanceObject);
                const char* name = object
                    ? *reinterpret_cast<const char* const*>(
                          object + kOffObjectName)
                    : nullptr;
                if (!name || strncmp(name, "handgun", 7) != 0)
                    return -1;
                const short marker = *reinterpret_cast<short*>(inst + 0xBE);
                if (marker == 4 || marker == 6)
                {
                    const int side = marker == 4 ? 0 : 1;
                    if (g_pistol_instance[side] != weapon)
                    {
                        g_pistol_instance[side] = weapon;
                        if (g_pistol_instance[1 - side] == weapon)
                            g_pistol_instance[1 - side] = nullptr;
                        log("first-person: %s pistol is instance %p (hip "
                            "marker %d)", side == 0 ? "left" : "right",
                            weapon, marker);
                    }
                    return side;
                }
                if (weapon == g_pistol_instance[0])
                    return 0;
                if (weapon == g_pistol_instance[1])
                    return 1;
                return -1;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        void __cdecl detour_weapon_fire_single(void* weapon, void* fire_data)
        {
            // Single-pistol mode. Retail alternates the pistols, but only
            // after a real shot: with one pistol drawn it asked the
            // holstered one to fire every time (log 2026-09-30: only the
            // undrawn side's requests, ~0.2 s apart, and the drawn pistol
            // never asked). So every other such request is fired from the
            // drawn pistol instead and the rest are dropped -- half the
            // akimbo rate, from the gun in hand.
            // With both drawn, retail still picks the pistol, not the
            // trigger (log: four right-pistol shots in a row while the
            // player expected the other gun), so a pistol only fires while
            // it is drawn AND its own trigger is held; with one such pistol
            // every other request fires from it (half rate), with two the
            // retail alternation stands.
            if (config().single_pistols && config().immersive_controls &&
                g_first_person_active)
            {
                const int side = pistol_side(weapon);
                bool active[2];
                for (int s = 0; s < 2; ++s)
                    active[s] = g_pistol_instance[s] &&
                        vr_input_holster_hand_drawn(s == 0) &&
                        vr_input_trigger_held(s == 0);
                if (side >= 0 && !(active[0] && active[1]))
                {
                    static bool fire_next = true;
                    const int shooter = active[0] ? 0 : active[1] ? 1 : -1;
                    const bool fire = fire_next && shooter >= 0;
                    fire_next = !fire_next;
                    static unsigned reports = 0;
                    if (reports++ < 8)
                        log("aim: single pistol -- %s pistol's request %s",
                            side == 0 ? "left" : "right",
                            fire ? (shooter == side ? "fired"
                                    : "fired from the other (triggered) "
                                      "pistol")
                                 : "dropped");
                    if (!fire)
                        return;
                    void* drawn = g_pistol_instance[shooter];
                    if (drawn == weapon)
                        goto single_pistol_done;
                    weapon = drawn;
                    // Retail played the fire animation (the muzzle flash) on
                    // the requested pistol before FireSingle (weapon message
                    // handler 0x0054DF6D..0x0054DFB4); replay it on the drawn
                    // one: anim = data+0xC [+3] (or [+0x54] for alternate
                    // fire), SetMode(inst,0,1), SetAnimation(inst,0,anim,0,0),
                    // clear state bit 0x100000, state+0x130 = anim.
                    __try
                    {
                        unsigned char* w = static_cast<unsigned char*>(drawn);
                        unsigned char* state =
                            *reinterpret_cast<unsigned char**>(w + 0x23C);
                        const unsigned char* data =
                            *reinterpret_cast<unsigned char* const*>(
                                w + 0x1C0);
                        const bool alternate = static_cast<const unsigned
                            char*>(fire_data)[2] != 0;
                        if (state && data)
                        {
                            const signed char anim = static_cast<signed char>(
                                data[0xC + (alternate ? 0x54 : 3)]);
                            if (anim != -1)
                            {
                                reinterpret_cast<void(__cdecl*)(void*, int,
                                    int)>(0x004DED90)(drawn, 0, 1);
                                reinterpret_cast<void(__cdecl*)(void*, int,
                                    int, int, int)>(0x004DEC30)(
                                        drawn, 0, anim, 0, 0);
                                *reinterpret_cast<uint32_t*>(state) &=
                                    ~0x100000u;
                                state[0x130] =
                                    static_cast<unsigned char>(anim);
                            }
                        }
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER) {}
                single_pistol_done:;
                }
            }
            alignas(16) float old_target[4] = {};
            float original_slope = 999.0f, headset_slope = 999.0f;
            float separation = 0.0f;
            float centre_hit_distance = 0.0f;
            bool centre_hit = false;
            bool redirected = false;
            if (config().hmd_aim && camera_first_person_active() &&
                g_head_aim_target_valid &&
                GetTickCount() - g_head_aim_target_time <= 100)
            {
                __try
                {
                    const unsigned char* state = *reinterpret_cast<unsigned char**>(
                        static_cast<unsigned char*>(weapon) + 0x23C);
                    const void* owner = *reinterpret_cast<void* const*>(state + 0x100);
                    if (owner == *reinterpret_cast<void* const*>(0x010E537C))
                    {
                        float* shot_target = reinterpret_cast<float*>(
                            static_cast<unsigned char*>(fire_data) + 0x10);
                        const unsigned char* player = *reinterpret_cast<unsigned char* const*>(
                            0x0111713C);
                        const float* headset_target = reinterpret_cast<const float*>(
                            player + 0x760);
                        bool valid = true;
                        for (int i = 0; i < 3; ++i)
                            valid = valid && std::isfinite(headset_target[i]) &&
                                    std::isfinite(shot_target[i]);
                        if (valid)
                        {
                            memcpy(old_target, shot_target, sizeof(old_target));
                            float delta[3] = {
                                shot_target[0] - headset_target[0],
                                shot_target[1] - headset_target[1],
                                shot_target[2] - headset_target[2]
                            };
                            separation = sqrtf(dot3(delta, delta));
                            original_slope = aim_vertical_slope(shot_target);
                            headset_slope = aim_vertical_slope(headset_target);
                            if (std::isfinite(separation) && separation > 2.0f)
                            {
                                memcpy(shot_target, headset_target,
                                       sizeof(old_target));
                                redirected = true;
                            }

                            if (g_accurate_aim_active)
                            {
                                // At close range a target far down the HMD
                                // ray leaves a visible eye-to-muzzle parallax.
                                // Probe the centre ray once per actual shot
                                // and converge the weapon on its first hit.
                                alignas(16) unsigned char probe[0x60] = {};
                                float* start = reinterpret_cast<float*>(
                                    probe + 0x30);
                                float* end = reinterpret_cast<float*>(
                                    probe + 0x40);
                                const unsigned char* weapon_info =
                                    *reinterpret_cast<unsigned char* const*>(
                                        0x01117050);
                                float range = *reinterpret_cast<const float*>(
                                    weapon_info + 0x814);
                                if (!std::isfinite(range) || range < 100.0f ||
                                    range > 50000.0f)
                                    range = 12000.0f;
                                for (int i = 0; i < 3; ++i)
                                {
                                    start[i] = g_head_aim_eye[i];
                                    end[i] = start[i] +
                                             g_head_aim_direction[i] * range;
                                }
                                start[3] = end[3] = 1.0f;
                                *reinterpret_cast<uint32_t*>(probe + 0x50) = 0xF;
                                *reinterpret_cast<const void**>(probe + 0x54) =
                                    owner;
                                g_legacy_line_probe(0x45AC1983, probe,
                                    const_cast<void*>(owner));
                                const int hit_type =
                                    *reinterpret_cast<const int*>(probe + 0x10);
                                if (hit_type != 0)
                                {
                                    float* hit = reinterpret_cast<float*>(
                                        probe + 0x40);
                                    float offset[3] = {
                                        hit[0] - start[0],
                                        hit[1] - start[1],
                                        hit[2] - start[2]
                                    };
                                    centre_hit_distance =
                                        sqrtf(dot3(offset, offset));
                                    if (std::isfinite(centre_hit_distance) &&
                                        centre_hit_distance > 4.0f &&
                                        centre_hit_distance <= range + 1.0f &&
                                        std::isfinite(hit[0]) &&
                                        std::isfinite(hit[1]) &&
                                        std::isfinite(hit[2]))
                                    {
                                        memcpy(shot_target, hit,
                                               sizeof(float) * 3);
                                        redirected = true;
                                        centre_hit = true;
                                    }
                                }
                            }
                        }
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    redirected = false;
                }
            }

            if (original_slope != 999.0f)
            {
                const float* requested = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(fire_data) + 0x10);
                memcpy(g_requested_shot_target, requested,
                       sizeof(g_requested_shot_target));
                g_weapon_fire_thread = GetCurrentThreadId();
            }

            // WEAPON_FireSingle starts the bullet ray, and centres the random
            // spread cone, at the weapon instance position (+0x10), which for
            // Lara's long gun is her hidden animated hand. Fire from the gun
            // the player holds instead; restore afterwards.
            alignas(16) float saved_origin[4] = {};
            bool origin_moved = false;
            float origin_shift = 0.0f;
            const GameplayGun* moved_copy = gameplay_gun_for(weapon);
            if (g_first_person_active && moved_copy &&
                GetTickCount() - moved_copy->tick <= 250)
            {
                __try
                {
                    float* position = reinterpret_cast<float*>(
                        static_cast<unsigned char*>(weapon) + 0x10);
                    memcpy(saved_origin, position, sizeof(saved_origin));
                    const Mat4& root = moved_copy->root;
                    float moved[3];
                    bool finite = true;
                    for (int c = 0; c < 3; ++c)
                    {
                        moved[c] = saved_origin[0] * root.m[0][c] +
                                   saved_origin[1] * root.m[1][c] +
                                   saved_origin[2] * root.m[2][c] +
                                   root.m[3][c];
                        finite = finite && std::isfinite(moved[c]);
                    }
                    if (finite)
                    {
                        float d[3] = { moved[0] - saved_origin[0],
                                       moved[1] - saved_origin[1],
                                       moved[2] - saved_origin[2] };
                        origin_shift = sqrtf(dot3(d, d));
                        memcpy(position, moved, sizeof(moved));
                        origin_moved = true;
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    origin_moved = false;
                }
            }
            // First person has no auto-aim, so accurate shooting is
            // rewarded: the random spread (fire data +0x20, the cone
            // WEAPON_FireSingle passes to 0x0054CC50, which skips the
            // scatter when it is 0) is zeroed for the shot (user,
            // 2026-10-04).
            float saved_spread = 0.0f;
            bool spread_zeroed = false;
            if (g_first_person_active && config().first_person_no_spread)
            {
                __try
                {
                    float* spread = reinterpret_cast<float*>(
                        static_cast<unsigned char*>(fire_data) + 0x20);
                    saved_spread = *spread;
                    if (saved_spread != 0.0f)
                    {
                        *spread = 0.0f;
                        spread_zeroed = true;
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    spread_zeroed = false;
                }
            }
            g_weapon_fire_single(weapon, fire_data);
            g_weapon_fire_thread = 0;
            if (spread_zeroed)
            {
                __try
                {
                    *reinterpret_cast<float*>(
                        static_cast<unsigned char*>(fire_data) + 0x20) =
                        saved_spread;
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
                static unsigned spread_reports = 0;
                if (spread_reports++ < 12)
                    log("aim: first-person shot without spread (game "
                        "spread %.2f)", saved_spread);
            }
            recoil_register_shot(weapon);
            if (origin_moved)
            {
                __try
                {
                    memcpy(static_cast<unsigned char*>(weapon) + 0x10,
                           saved_origin, sizeof(saved_origin));
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
                static unsigned gun_reports = 0;
                if (gun_reports++ < 24)
                {
                    float spread = 0.0f;
                    __try
                    {
                        spread = *reinterpret_cast<const float*>(
                            static_cast<const unsigned char*>(fire_data) +
                            0x20);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER) {}
                    log("aim: long-gun shot fired from the held gun (origin "
                        "moved %.1f units); retail spread value %.4f",
                        origin_shift, spread);
                }
            }

            if (redirected)
            {
                __try
                {
                    memcpy(static_cast<unsigned char*>(fire_data) + 0x10,
                           old_target, sizeof(old_target));
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (original_slope != 999.0f)
            {
                static unsigned reports = 0;
                if (reports++ < 20)
                    log("aim: fired shot target delta %.1f units, view Y/Z "
                        "original %.3f headset %.3f, target sync %d, "
                        "accurate centre hit %d at %.1f units",
                        separation, original_slope, headset_slope,
                        redirected ? 1 : 0, centre_hit ? 1 : 0,
                        centre_hit_distance);
            }
        }

        void first_person_restore_head()
        {
            if (g_first_person_head_hidden && g_first_person_instance &&
                g_first_person_head_mask)
            {
                __try
                {
                    unsigned char* instance =
                        static_cast<unsigned char*>(g_first_person_instance);
                    uint32_t& normal = *reinterpret_cast<uint32_t*>(
                        instance + kOffNoDrawGroups);
                    uint32_t& fx = *reinterpret_cast<uint32_t*>(
                        instance + kOffFxNoDrawGroups);
                    normal = (normal & ~g_first_person_head_mask) |
                             g_first_person_saved_normal;
                    fx = (fx & ~g_first_person_head_mask) |
                         g_first_person_saved_fx;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    // The level can destroy an instance before the camera
                    // observes the transition. There is then nothing left to
                    // restore; most importantly, do not follow a stale pointer.
                }
            }

            g_first_person_head_hidden = false;
            g_first_person_head_mask = 0;
            g_first_person_saved_normal = 0;
            g_first_person_saved_fx = 0;
        }

        void first_person_leave()
        {
            g_first_person_combat_locomotion = false;
            first_person_restore_head();
            vr_set_head_position_origin(nullptr, false);
            for (int item = 0; item < 2; ++item)
            {
                g_first_person_gear_valid[item] = false;
                g_first_person_gear_belt_local_valid[item] = false;
                g_first_person_gear_held[item] = false;
                g_first_person_gear_hand[item] = -1;
            }
            g_first_person_active = false;
            g_first_person_instance = nullptr;
            g_first_person_model = nullptr;
            g_first_person_head_segment = -1;
            g_first_person_shoulder_segment[0] = -1;
            g_first_person_shoulder_segment[1] = -1;
            g_first_person_elbow_segment[0] = -1;
            g_first_person_elbow_segment[1] = -1;
            g_first_person_wrist_segment[0] = -1;
            g_first_person_wrist_segment[1] = -1;
            for (HandStripCache& cache : g_first_person_hand_strips)
            {
                if (cache.data)
                    HeapFree(GetProcessHeap(), 0, cache.data);
                cache = HandStripCache{};
            }
            if (g_first_person_body_strips.data)
                HeapFree(GetProcessHeap(), 0, g_first_person_body_strips.data);
            g_first_person_body_strips = HandStripCache{};
            g_first_person_strip_source = nullptr;
            g_first_person_hands_reported = false;
            g_first_person_hand_unavailable_reported = false;
            g_first_person_anchor_valid = false;
            g_first_person_ground_anchor_valid = false;
            g_first_person_anchor_warned = false;
            g_first_person_traversal = TraversalGround;
            g_first_person_ledge_hanging = false;
            g_first_person_bar_hanging = false;
            g_first_person_bar_axis_valid = false;
            g_first_person_bar_entered_at = 0;
            g_first_person_ledge_climbing = false;
            g_first_person_vine_climbing = false;
            g_last_ledge_visibility_mark[0] = ~0u;
            g_last_ledge_visibility_mark[1] = ~0u;
            g_current_hand_cpu_draw = -1;
            g_current_gpu_hand_draw = -1;
            g_hand_drawable_count = 0;
            g_hand_render_state_last_mark = ~0u;
            g_ground_hand_render_state_reported = false;
            g_hand_render_state_trace_remaining = 0;
            g_first_person_horizontal_ledge = false;
            g_first_person_vine_pull_hand = -1;
            for (LedgeHandGrip& grip : g_ledge_hand_grip)
            {
                grip.pending = false;
                grip.held = false;
                grip.in_grab_zone = false;
            }
            g_grab_zone_debug.valid = false;
            g_grab_zone_centre_valid = false;
            g_first_person_turn_ready = false;
            g_first_person_base_heading = 0.0f;
            g_first_person_stick_start = 0.0f;
            // A cutscene uses an unrelated camera heading while Lara keeps
            // the same actor. Preserve their original model-facing offset;
            // recalibrating it from the cinematic exit broke body rotation.
            g_first_person_actor_reported = false;
            for (bool& reported : g_first_person_movement_reported)
                reported = false;
            for (bool& reported : g_first_person_motion_reported)
                reported = false;
            g_first_person_move_driven = false;
            g_first_person_move_report_time = 0;
            g_first_person_move_reports = 0;
            g_first_person_probe_sector = -1;
        }

        bool segment_is_in_head_subtree(const ModelSegment* segments,
                                         int count, int segment, int head);

        bool bind_joint_position(const ModelSegment* segments, int count,
                                 int segment, float* position)
        {
            if (segment < 0 || segment >= count)
                return false;
            position[0] = position[1] = position[2] = 0.0f;
            for (int guard = 0; guard < count; ++guard)
            {
                for (int axis = 0; axis < 3; ++axis)
                    position[axis] += segments[segment].pivot[axis];
                const int parent = segments[segment].parent;
                if (parent < 0)
                    return true;
                if (parent >= count || parent == segment)
                    return false;
                segment = parent;
            }
            return false;
        }

        float point_distance_sq(const float* a, const float* b)
        {
            const float x = a[0] - b[0];
            const float y = a[1] - b[1];
            const float z = a[2] - b[2];
            return x*x + y*y + z*z;
        }

        float line_distance_sq(const float* point, const float* a,
                               const float* b)
        {
            const float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
            const float ap[3] = { point[0]-a[0], point[1]-a[1],
                                  point[2]-a[2] };
            const float length_sq = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
            float t = length_sq > 0.001f ?
                (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / length_sq : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const float nearest[3] = { a[0]+t*ab[0], a[1]+t*ab[1],
                                       a[2]+t*ab[2] };
            return point_distance_sq(point, nearest);
        }


        uint32_t first_person_find_head_group(void* model, int head_segment)
        {
            uint32_t mask = 0;
            __try
            {
                unsigned char* m = static_cast<unsigned char*>(model);
                const int count = *reinterpret_cast<int*>(
                    m + kOffModelNumSegments);
                ModelSegment* segments = *reinterpret_cast<ModelSegment**>(
                    m + kOffModelSegments);
                ShortVector* centres = *reinterpret_cast<ShortVector**>(
                    m + kOffModelDrawgroupCenters);
                if (!segments || !centres || count <= 0 || count > 512 ||
                    head_segment < 0 || head_segment >= count)
                    return 0;

                // Segment pivots are parent-relative and stored parents-first.
                // Accumulate the head joint into model space, the same space
                // used by Model::drawgroupCenterList.
                float head[3] = { 0.0f, 0.0f, 0.0f };
                int segment = head_segment;
                for (int guard = 0; guard < count; ++guard)
                {
                    head[0] += segments[segment].pivot[0];
                    head[1] += segments[segment].pivot[1];
                    head[2] += segments[segment].pivot[2];
                    const int parent = segments[segment].parent;
                    if (parent < 0)
                        break;
                    if (parent >= count || parent == segment)
                        return 0;
                    segment = parent;
                }

                int best = -1;
                float best_distance_sq = FLT_MAX;
                for (int group = 0; group < kLaraDrawGroups; ++group)
                {
                    const float dx = static_cast<float>(centres[group].x) - head[0];
                    const float dy = static_cast<float>(centres[group].y) - head[1];
                    const float dz = static_cast<float>(centres[group].z) - head[2];
                    const float distance_sq = dx * dx + dy * dy + dz * dz;
                    if (distance_sq < best_distance_sq)
                    {
                        best_distance_sq = distance_sq;
                        best = group;
                    }
                }

                // A missing/empty centre list reads as zeroes. Lara's head is
                // roughly 480 units above her root, so refuse a remote match
                // instead of hiding an arbitrary body group.
                if (best < 0 || best_distance_sq > 180.0f * 180.0f)
                {
                    log("first-person: no trustworthy head draw group "
                        "(nearest=%d distance=%.1f); model left intact",
                        best, best >= 0 ? sqrtf(best_distance_sq) : -1.0f);
                    return 0;
                }

                mask = uint32_t(1) << best;

                // The last headset log showed only eight real draw-group
                // centres. Group 0 is Lara's broad body mesh; groups 4..7
                // form a tight cluster around the known head group 5.
                // Mask the cluster, excluding group 0, so the remaining
                // face/hair sections cannot enter the headset.
                const float skull_centre[3] = {
                    static_cast<float>(centres[best].x),
                    static_cast<float>(centres[best].y),
                    static_cast<float>(centres[best].z)
                };
                for (int group = 1; group < kLaraDrawGroups; ++group)
                {
                    if (group == best ||
                        (!centres[group].x && !centres[group].y &&
                         !centres[group].z))
                        continue;
                    const float centre[3] = {
                        static_cast<float>(centres[group].x),
                        static_cast<float>(centres[group].y),
                        static_cast<float>(centres[group].z)
                    };
                    if (point_distance_sq(centre, skull_centre) >
                        65.0f * 65.0f)
                        continue;
                    mask |= uint32_t(1) << group;
                    log("first-person: clustered head draw group %d hidden",
                        group);
                }

                // Bone collapse made mixed head/body triangles stretch into
                // the camera. Classify whole draw groups by the segments of
                // their actual vertex indices, then hide only groups whose
                // geometry is overwhelmingly in the head hierarchy.
                const int vertex_count = *reinterpret_cast<int*>(
                    m + kOffModelNumVertices);
                unsigned char* vertices = *reinterpret_cast<unsigned char**>(
                    m + kOffModelVertices);
                unsigned char* strip = *reinterpret_cast<unsigned char**>(
                    m + kOffModelTextureStrips);
                unsigned char* first_strip = strip;
                unsigned total[kLaraDrawGroups]{};
                unsigned in_head[kLaraDrawGroups]{};
                unsigned in_arm[2][kLaraDrawGroups]{};
                unsigned in_hand[2][kLaraDrawGroups]{};
                bool head_bone[512]{};
                bool arm_bone[2][512]{};
                bool hand_bone[2][512]{};
                for (int bone = 0; bone < count; ++bone)
                {
                    head_bone[bone] = segment_is_in_head_subtree(
                        segments, count, bone, head_segment);
                    for (int hand = 0; hand < 2; ++hand)
                    {
                        const int shoulder =
                            g_first_person_shoulder_segment[hand];
                        const int wrist =
                            g_first_person_wrist_segment[hand];
                        arm_bone[hand][bone] = shoulder >= 0 &&
                            segment_is_in_head_subtree(
                                segments, count, bone, shoulder);
                        hand_bone[hand][bone] = wrist >= 0 &&
                            segment_is_in_head_subtree(
                                segments, count, bone, wrist);
                    }
                }
                const char* strip_issue = nullptr;
                bool valid_strips = vertices && strip && vertex_count > 0 &&
                                    vertex_count <= 65535;
                if (!valid_strips)
                    strip_issue = !strip ? "no texture strips" :
                        !vertices ? "no source vertices" :
                        "invalid source vertex count";
                int strips_seen = 0;
                while (valid_strips && strip && strips_seen++ < 4096)
                {
                    const int n = *reinterpret_cast<short*>(strip);
                    if (n == 0)
                    {
                        strip = nullptr; // Renderer uses this sentinel too.
                        break;
                    }
                    const int group = *reinterpret_cast<short*>(strip + 2);
                    if (n < 3 || group < 0 ||
                        group >= kLaraDrawGroups)
                    {
                        strip_issue = "invalid strip header";
                        log("first-person: strip %d header count %d group %d "
                            "at %p", strips_seen, n, group, strip);
                        valid_strips = false;
                        break;
                    }
                    const uint16_t* indices = reinterpret_cast<const uint16_t*>(
                        strip + 0x14);
                    for (int i = 0; i < n; ++i)
                    {
                        const unsigned index = indices[i];
                        if (index >= static_cast<unsigned>(vertex_count))
                        {
                            strip_issue = "vertex index outside source mesh";
                            valid_strips = false;
                            break;
                        }
                        const int bone = *reinterpret_cast<short*>(
                            vertices + index * 16 + 10);
                        if (bone < 0 || bone >= count)
                            continue;
                        ++total[group];
                        if (head_bone[bone])
                            ++in_head[group];
                        for (int hand = 0; hand < 2; ++hand)
                        {
                            if (arm_bone[hand][bone])
                                ++in_arm[hand][group];
                            if (hand_bone[hand][bone])
                                ++in_hand[hand][group];
                        }
                    }
                    unsigned char* next =
                        *reinterpret_cast<unsigned char**>(strip + 0x10);
                    if (next == strip)
                    {
                        strip_issue = "self-linked strip";
                        valid_strips = false;
                        break;
                    }
                    strip = next == first_strip ? nullptr : next;
                }
                if (strip)
                {
                    if (!strip_issue)
                        strip_issue = "truncated or cyclic strip list";
                    valid_strips = false; // truncated or cyclic list
                }
                if (valid_strips)
                {
                    for (int group = 0; group < kLaraDrawGroups; ++group)
                    {
                        if (total[group] >= 6 &&
                            (in_head[group] || in_arm[0][group] ||
                             in_arm[1][group]))
                            log("first-person: group %d vertices %u: "
                                "head %u, left arm/hand %u/%u, "
                                "right arm/hand %u/%u",
                                group, total[group], in_head[group],
                                in_arm[0][group], in_hand[0][group],
                                in_arm[1][group], in_hand[1][group]);
                        if (total[group] < 6 || in_head[group] * 4 <
                                                 total[group] * 3)
                            continue;
                        const float dx = static_cast<float>(centres[group].x) - head[0];
                        const float dy = static_cast<float>(centres[group].y) - head[1];
                        const float dz = static_cast<float>(centres[group].z) - head[2];
                        if (dx*dx + dy*dy + dz*dz > 300.0f*300.0f)
                            continue;
                        mask |= uint32_t(1) << group;
                        log("first-person: head draw group %d has %u/%u "
                            "head-linked vertices", group, in_head[group],
                            total[group]);
                    }
                }
                else
                {
                    log("first-person: strip census unavailable (%s; source "
                        "vertices %d, strips %d); using head cluster mask",
                        strip_issue ? strip_issue : "unknown", vertex_count,
                        strips_seen);
                    // Render data can outlive the source strips. Capture the
                    // compact centre table so the next headset run can tell
                    // which head and wrist groups remain without guessing.
                    for (int group = 0; group < kLaraDrawGroups; ++group)
                    {
                        const float dx = static_cast<float>(centres[group].x) - head[0];
                        const float dy = static_cast<float>(centres[group].y) - head[1];
                        const float dz = static_cast<float>(centres[group].z) - head[2];
                        if (centres[group].x || centres[group].y ||
                            centres[group].z)
                            log("first-person: group %d centre (%d,%d,%d) "
                                "head distance %.1f", group,
                                centres[group].x, centres[group].y,
                                centres[group].z, sqrtf(dx*dx + dy*dy + dz*dz));
                    }
                    log("first-person: source mesh ptr %p, strip ptr %p, "
                        "render mesh ptr %p", vertices, first_strip,
                        *reinterpret_cast<void**>(m + 0x90));
                }

                log("first-person: HeadSegment %d head draw mask 0x%08X "
                    "(nearest group %d, %.1f units)", head_segment, mask,
                    best, sqrtf(best_distance_sq));
                return mask;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                log("first-person: model strip metadata could not be read; "
                    "head mask falls back to 0x%08X", mask);
                return mask;
            }
        }

        struct TraversalProbe
        {
            FirstPersonTraversal kind = TraversalGround;
            uintptr_t state_vtable = 0;
            int state_id = -1;
            unsigned swim_status = 0;
            bool water = false;
            bool attached = false;
            bool rope = false;
            bool ledge_hanging = false;
            bool bar_hanging = false;
            bool vine_climbing = false;
            bool precarious = false;
            void* prompt = nullptr;
        };

        // owner: the instance whose states are read; null = the first-
        // person Lara (third person passes Lara for the animation log).
        TraversalProbe first_person_traversal_probe(void* owner = nullptr)
        {
            TraversalProbe result;
            if (!owner)
                owner = g_first_person_instance;
            __try
            {
                // 0x0111713C points to PlayerData::work. PDB layout puts
                // CharacterState 0x70 bytes before it and its active StateObj
                // at +4. Verify the owning instance before following states.
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!player || !owner)
                    return result;
                result.water =
                    *reinterpret_cast<void* const*>(player + 0x804) != nullptr;
                result.swim_status =
                    *reinterpret_cast<const unsigned*>(player + 0x81C);
                const bool underwater =
                    *reinterpret_cast<const unsigned char*>(player + 0xCB2)
                        != 0;
                result.attached =
                    *reinterpret_cast<void* const*>(player + 0xC00) != nullptr;
                result.rope =
                    *reinterpret_cast<void* const*>(player + 0xF5C) != nullptr;

                const unsigned char* data = player - 0x70;
                if (*reinterpret_cast<void* const*>(data) == owner)
                {
                    const unsigned char* state =
                        *reinterpret_cast<unsigned char* const*>(data + 4);
                    for (int depth = 0; depth < 5 && state; ++depth)
                    {
                        const uintptr_t vtable =
                            *reinterpret_cast<const uintptr_t*>(state);
                        if (!depth)
                        {
                            result.state_vtable = vtable;
                            result.state_id =
                                *reinterpret_cast<const int*>(state + 4);
                        }
                        // Verified against the retail executable's vtable
                        // slots. The earlier PDB addresses are from a
                        // different link and never matched this game build.
                        // Air-attach remains active while hanging in the
                        // current headset log, so accept it as well as the
                        // idle, shimmy and corner states.
                        if (vtable == 0x00F057B4 || // LedgeIdle
                            vtable == 0x00F057C8 || // LedgeShimmy
                            vtable == 0x00F057DC || // LedgeConcaveTurn
                            vtable == 0x00F057F0 || // LedgeConvexTurn
                            vtable == 0x00F05C94)   // LedgeAirAttach
                        {
                            // LedgeAirAttach: +0x50/+0x54 == 1 marks the
                            // precarious catch; its ButtonPrompt instance is
                            // at +0x18 while shown. Message 0x8000007C
                            // (Action, LT = E) secures it.
                            if (vtable == 0x00F05C94)
                                result.prompt =
                                    *reinterpret_cast<void* const*>(
                                        state + 0x18);
                            if (vtable == 0x00F05C94)
                                result.precarious =
                                    *reinterpret_cast<void* const*>(
                                        state + 0x18) != nullptr &&
                                    (*reinterpret_cast<const int*>(
                                         state + 0x50) == 1 ||
                                     *reinterpret_cast<const int*>(
                                         state + 0x54) == 1);
                            result.kind = TraversalClimb;
                            result.ledge_hanging = true;
                            return result;
                        }
                        // A horizontal swing bar has its own sustained
                        // hang, swing and traverse states after air attach.
                        // Keep the hand anchor through those transitions.
                        if (vtable == 0x00F05CA8 || // HPoleAirAttach
                            vtable == 0x00F05818 || // HPoleHangIdle
                            vtable == 0x00F0582C || // HPoleHangTurn
                            vtable == 0x00F05840 || // HPoleSwingSwinging
                            vtable == 0x00F05854 || // HPoleSwingStop
                            vtable == 0x00F05868 || // HPoleSwingTurn
                            vtable == 0x00F0587C || // HPoleTraverse
                            vtable == 0x00F05890)   // HPoleFastTraverse
                        {
                            // HPoleAirAttach: +0x20 == 1 with its prompt
                            // at +0x1C; also secured by 0x8000007C.
                            if (vtable == 0x00F05CA8)
                                result.prompt =
                                    *reinterpret_cast<void* const*>(
                                        state + 0x1C);
                            if (vtable == 0x00F05CA8)
                                result.precarious =
                                    *reinterpret_cast<void* const*>(
                                        state + 0x1C) != nullptr &&
                                    *reinterpret_cast<const int*>(
                                        state + 0x20) == 1;
                            result.kind = TraversalClimb;
                            result.bar_hanging = true;
                            return result;
                        }
                        // Retail vtables verified by their Entry and
                        // MessageHandler slots in trl.exe. Climbable vines
                        // use the wall-vertical-pole family; include ladder
                        // and free vertical pole attachment as the same
                        // hand-over-hand class, but never rope or zipline.
                        if (vtable == 0x00F0578C || // LadderAirAttach
                            vtable == 0x00F05C80 || // LadderAttached
                            vtable == 0x00F058E8 || // VertPoleAirAttach
                            vtable == 0x00F058D4 || // VertPoleAttached
                            vtable == 0x00F05924 || // WallVertPoleAirAttach
                            vtable == 0x00F05910)   // WallVertPoleAttached
                        {
                            result.kind = TraversalClimb;
                            result.vine_climbing = true;
                            return result;
                        }
                        // The remaining broad traversal test retains its
                        // guarded attachment fallback for other surfaces.
                        if (vtable == 0x00F06A80 ||
                            (vtable >= 0x00F06C7C &&
                             vtable <= 0x00F06CCC &&
                             (vtable - 0x00F06C7C) % 0x14 == 0))
                        {
                            result.kind = TraversalSwim;
                            return result;
                        }
                        const bool ledge_or_pole =
                            vtable >= 0x00F0652C &&
                            vtable <= 0x00F06658 &&
                            (vtable - 0x00F0652C) % 0x14 == 0 &&
                            vtable != 0x00F065B8;
                        const bool vertical_pole =
                            vtable >= 0x00F0668C &&
                            vtable <= 0x00F066F0 &&
                            (vtable - 0x00F0668C) % 0x14 == 0;
                        const bool rope =
                            vtable >= 0x00F067B4 &&
                            vtable <= 0x00F0687C &&
                            (vtable - 0x00F067B4) % 0x14 == 0;
                        const bool zipline =
                            vtable >= 0x00F068E4 &&
                            vtable <= 0x00F0690C &&
                            (vtable - 0x00F068E4) % 0x14 == 0;
                        if (ledge_or_pole || vertical_pole || rope || zipline ||
                            vtable == 0x00F06AD0 ||
                            vtable == 0x00F06AE4 ||
                            vtable == 0x00F06AF8 ||
                            vtable == 0x00F06EAC ||
                            vtable == 0x00F06EC8 ||
                            vtable == 0x00F06EDC ||
                            vtable == 0x00F06EF0 ||
                            vtable == 0x00F06F04 ||
                            vtable == 0x00F06F18 ||
                            vtable == 0x00F06F54 ||
                            vtable == 0x00F06F68)
                        {
                            result.kind = TraversalClimb;
                            return result;
                        }
                        const unsigned char* super =
                            *reinterpret_cast<unsigned char* const*>(
                                state + 8);
                        if (super == state)
                            break;
                        state = super;
                    }
                }
                if (result.water &&
                    (result.swim_status != 0 || underwater))
                    result.kind = TraversalSwim;
                else if (result.attached || result.rope)
                    result.kind = TraversalClimb;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                // A changing state pointer must not interrupt the camera.
            }
            return result;
        }

        // While hanging from a ledge or bar the eye follows Lara's animated
        // head and root. The fast shimmy / fast traverse clips sway much
        // more than the normal ones, which moved the player's view off the
        // hold mid-grab. Smooth the eye in world space along three axes:
        // lightly along the hold (keeps the travel speed, ~0.1 s lag) and
        // heavily across it and vertically (the sway; the body does not
        // truly travel there while hanging). Large jumps snap.
        // Evidence for "low after a pull-up" and "jumps feel low": for 3 s
        // after climb -> ground, and throughout JumpState, log the eye
        // against Lara's origin, root segment and animated head.
        void first_person_height_diag(const char* path, bool use_root,
                                      float root_distance, const float* raw,
                                      const float* root, const float* eye)
        {
            const DWORD now = GetTickCount();
            const bool jumping = g_first_person_state_vtable == 0x00F05CEC;
            if (!jumping && (!g_height_diag_until ||
                             now > g_height_diag_until))
                return;
            static DWORD last = 0;
            static unsigned reports = 0;
            if (reports >= 400 || now - last < 200)
                return;
            last = now;
            ++reports;
            const float* origin = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(g_first_person_instance) +
                0x10);
            const float scale = tune_world_scale();
            const float m = (std::isfinite(scale) && scale > 0.0f)
                ? 1.0f / scale : 1.0f;
            // The floor under the eye, from the retail line probe (Lara
            // ignored): separates "eye low relative to Lara" from "Lara's
            // origin low relative to the floor".
            const float down[3] = { 0.0f, 0.0f, -1.0f };
            float floor_point[3] = { 0.0f, 0.0f, 0.0f };
            const bool floor_hit = aim_probe(eye, down,
                3.0f / (m > 0.0f ? m : 1.0f), floor_point, nullptr);
            log("first-person: height %s [%s] state %p root %s (head-root "
                "%.0f u); above origin m: eye %+.2f, head %+.2f, root %+.2f; "
                "above floor m: eye %s%.2f, origin %+.2f; origin z %.1f",
                path, jumping ? "jump" : "after climb",
                reinterpret_cast<void*>(g_first_person_state_vtable),
                use_root ? "ok" : "REJECTED", root_distance,
                (eye[2] - origin[2]) * m, (raw[2] - origin[2]) * m,
                (root[14] - origin[2]) * m,
                floor_hit ? "" : ">", (eye[2] - floor_point[2]) * m,
                (origin[2] - floor_point[2]) * m, origin[2]);
        }

        // The retail mantle (LedgeClimb, 00F05804) ends with Lara kneeling on
        // the ledge top before she stands; the eye follows her animated head
        // there, so the player sat ~0.6 m above the floor for 1.5 s (log
        // 2026-09-30 05:06, 49.2-50.8 s) until the stand-up or a step. Once
        // the eye is over the top surface, keep it at least a standing eye
        // height above that floor, rising at up to 2.5 m/s (no pop).
        void first_person_mantle_eye_floor(float* eye)
        {
            static bool active = false;
            static float held_z = 0.0f;
            static LARGE_INTEGER last{};
            static LARGE_INTEGER frequency{};
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            const float scale = tune_world_scale();
            if (!config().first_person_mantle_smoothing ||
                !g_first_person_ledge_climbing || !g_first_person_anchor_valid ||
                !std::isfinite(scale) || scale <= 0.0f)
            {
                if (active)
                    log("first-person: mantle eye floor released");
                active = false;
                return;
            }
            const float standing = g_first_person_anchor[2];
            if (!std::isfinite(standing) || standing < 0.8f * scale ||
                standing > 2.5f * scale)
                return;
            const float down[3] = { 0.0f, 0.0f, -1.0f };
            float floor_point[3];
            if (!aim_probe(eye, down, standing, floor_point, nullptr))
            {
                // Not over the top surface yet (still rising from the hang,
                // or the floor is further than a standing height below).
                if (active)
                    held_z = eye[2];
                return;
            }
            const float target = floor_point[2] + standing;
            if (!active)
            {
                active = true;
                held_z = eye[2];
                last = now;
                log("first-person: mantle eye floor engaged (eye %.2f m above "
                    "the ledge top, standing %.2f m)",
                    (eye[2] - floor_point[2]) / scale, standing / scale);
            }
            float dt = frequency.QuadPart
                ? float(now.QuadPart - last.QuadPart) /
                  float(frequency.QuadPart)
                : 0.011f;
            last = now;
            dt = fminf(fmaxf(dt, 0.0f), 0.1f);
            // Rise toward the standing floor height at a bounded speed; never
            // hold the eye below the animated one.
            const float rise = 2.5f * scale * dt;
            if (held_z < target)
                held_z = fminf(target, held_z + rise);
            else
                held_z = target;
            if (eye[2] < held_z)
                eye[2] = held_z;
        }

        void first_person_smooth_hanging_eye(float* eye)
        {
            static bool valid = false;
            static float smoothed[3] = { 0.0f, 0.0f, 0.0f };
            static LARGE_INTEGER last{};
            static LARGE_INTEGER frequency{};
            const bool hanging =
                (g_first_person_ledge_hanging || g_first_person_bar_hanging) &&
                !g_first_person_ledge_climbing && g_first_person_hold_axis_valid;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            if (!hanging)
            {
                valid = false;
                return;
            }
            float delta[3];
            float distance_sq = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                delta[axis] = eye[axis] - smoothed[axis];
                distance_sq += delta[axis] * delta[axis];
            }
            const float scale = tune_world_scale();
            const float snap = (std::isfinite(scale) && scale > 0.0f)
                ? 1.0f * scale : 300.0f;
            if (!valid || !std::isfinite(distance_sq) ||
                distance_sq > snap * snap)
            {
                memcpy(smoothed, eye, sizeof(smoothed));
                last = now;
                valid = true;
                return;
            }
            float dt = frequency.QuadPart
                ? float(now.QuadPart - last.QuadPart) /
                  float(frequency.QuadPart)
                : 0.011f;
            last = now;
            dt = fminf(fmaxf(dt, 0.0f), 0.1f);
            const float along_rate = 1.0f - expf(-dt / 0.10f);
            const float across_rate = 1.0f - expf(-dt / 0.40f);
            const float ax = g_first_person_hold_axis[0];
            const float ay = g_first_person_hold_axis[1];
            const float along = delta[0] * ax + delta[1] * ay;
            const float across = -delta[0] * ay + delta[1] * ax;
            smoothed[0] += along * along_rate * ax -
                           across * across_rate * ay;
            smoothed[1] += along * along_rate * ay +
                           across * across_rate * ax;
            smoothed[2] += delta[2] * across_rate;
            memcpy(eye, smoothed, sizeof(smoothed));
        }

        // A held eye (root transform rejected) rides on Lara's origin, and
        // the return to the anchored eye is blended. The forward roll
        // (state 26, animation 56) curls the head onto the root for 4-7
        // frames: the view froze while she rolled on, then jumped 0.8-0.9 m
        // (user: "almost freezes for a second", log 2026-10-04).
        bool  g_eye_hold_active = false;
        float g_eye_hold_origin[3]{};
        float g_eye_hold_eye[3]{};
        float g_eye_return[3]{};
        unsigned g_eye_hold_reports = 0;

        bool first_person_origin(float* out)
        {
            __try
            {
                const float* o = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(g_first_person_instance) +
                    0x10);
                for (int k = 0; k < 3; ++k)
                    out[k] = o[k];
                return std::isfinite(out[0]) && std::isfinite(out[1]) &&
                       std::isfinite(out[2]);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Decay of the blended return, per call (~0.12 s).
        void first_person_eye_return(float* filtered)
        {
            static LARGE_INTEGER last{};
            static LARGE_INTEGER frequency{};
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            float dt = last.QuadPart && frequency.QuadPart
                ? float(now.QuadPart - last.QuadPart) /
                  float(frequency.QuadPart) : 0.0f;
            last = now;
            dt = fminf(fmaxf(dt, 0.0f), 0.1f);
            const float keep = expf(-dt / 0.12f);
            for (int k = 0; k < 3; ++k)
            {
                g_eye_return[k] *= keep;
                if (!std::isfinite(g_eye_return[k]))
                    g_eye_return[k] = 0.0f;
                filtered[k] += g_eye_return[k];
            }
        }

        void first_person_filter_anchor(const float* raw, const float* root,
                                        FirstPersonTraversal traversal,
                                        float* filtered)
        {
            Mat4 root_to_world = Mat4::identity();
            memcpy(&root_to_world.m[0][0], root, sizeof(root_to_world.m));
            const float root_distance = sqrtf(
                (raw[0]-root[12])*(raw[0]-root[12]) +
                (raw[1]-root[13])*(raw[1]-root[13]) +
                (raw[2]-root[14])*(raw[2]-root[14]));
            bool use_root = std::isfinite(root_distance) &&
                            root_distance > 100.0f && root_distance < 2000.0f;
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    use_root = use_root &&
                        std::isfinite(root_to_world.m[row][column]);
            for (int row = 0; row < 3; ++row)
            {
                float length_sq = 0.0f;
                for (int column = 0; column < 3; ++column)
                    length_sq += root_to_world.m[row][column] *
                                 root_to_world.m[row][column];
                use_root = use_root && fabsf(length_sq - 1.0f) < 0.1f;
                for (int other = row + 1; other < 3; ++other)
                {
                    float dot = 0.0f;
                    for (int column = 0; column < 3; ++column)
                        dot += root_to_world.m[row][column] *
                               root_to_world.m[other][column];
                    use_root = use_root && fabsf(dot) < 0.1f;
                }
            }
            if (!use_root)
            {
                if (traversal != TraversalGround)
                {
                    float animated[3] = {
                        raw[0], raw[1],
                        raw[2] + config().first_person_eye_height
                    };
                    float jump_sq = 0.0f;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float delta =
                            animated[axis] - g_first_person_last_eye[axis];
                        jump_sq += delta * delta;
                    }
                    if (!g_first_person_anchor_valid ||
                        jump_sq < 600.0f*600.0f)
                    {
                        memcpy(filtered, animated, 3 * sizeof(float));
                        memcpy(g_first_person_last_eye, filtered,
                               sizeof(g_first_person_last_eye));
                        first_person_height_diag("animated", false,
                                                 root_distance, raw, root,
                                                 filtered);
                        return;
                    }
                }
                // A transient bad root matrix is common during traversal.
                // Keep the last good eye point and *retain* the calibration;
                // recalibrating from an animated head caused a large drop
                // into Lara's collarbone in the previous headset log.
                memcpy(filtered,
                       g_first_person_anchor_valid ? g_first_person_last_eye
                                                   : raw,
                       3 * sizeof(float));
                float origin[3];
                if (g_first_person_anchor_valid &&
                    first_person_origin(origin))
                {
                    if (!g_eye_hold_active)
                    {
                        g_eye_hold_active = true;
                        memcpy(g_eye_hold_origin, origin, sizeof(origin));
                        memcpy(g_eye_hold_eye, g_first_person_last_eye,
                               sizeof(g_eye_hold_eye));
                        if (g_eye_hold_reports++ < 20)
                            log("first-person: root rejected (head-root "
                                "%.0f u); eye rides on Lara's origin",
                                root_distance);
                    }
                    for (int k = 0; k < 3; ++k)
                        filtered[k] = g_eye_hold_eye[k] +
                                      origin[k] - g_eye_hold_origin[k];
                    memcpy(g_first_person_last_eye, filtered,
                           sizeof(g_first_person_last_eye));
                }
                if (!g_first_person_anchor_warned)
                {
                    g_first_person_anchor_warned = true;
                    log("first-person: root transform invalid; holding last "
                        "calibrated eye until it recovers");
                }
                first_person_height_diag("held last eye", false,
                                         root_distance, raw, root, filtered);
                return;
            }
            const Mat4 world_to_root = rigid_inverse(root_to_world);
            float relative[3];
            for (int column = 0; column < 3; ++column)
            {
                relative[column] = world_to_root.m[3][column];
                for (int row = 0; row < 3; ++row)
                    relative[column] += raw[row] *
                                        world_to_root.m[row][column];
            }
            // Latch the anatomical eye height at entry. Lara's idle, running
            // and breathing clips move HeadSegment independently of the
            // player's body; following them was the visible camera drift.
            // Root translation and rotation still pass through every frame,
            // while physical HMD translation/rotation is composed afterwards.
            if (!g_first_person_anchor_valid ||
                (traversal == TraversalGround &&
                 !g_first_person_ground_anchor_valid))
            {
                memcpy(g_first_person_anchor, relative,
                       sizeof(g_first_person_anchor));
                g_first_person_anchor[2] +=
                    config().first_person_eye_height;
                if (traversal == TraversalGround)
                {
                    const bool same_actor =
                        g_first_person_standing_eye_valid &&
                        g_first_person_standing_instance ==
                            g_first_person_instance &&
                        g_first_person_standing_model ==
                            g_first_person_model;
                    // About 100 game units of breathing/transition motion is
                    // plausible. A 300-unit drop after a cinematic is not a
                    // new standing height; retain the prior anatomical one.
                    if (same_actor &&
                        g_first_person_anchor[2] <
                            g_first_person_standing_eye_z - 100.0f)
                    {
                        log("first-person: rejected low ground eye anchor "
                            "%.1f after traversal/cinematic; restored %.1f",
                            g_first_person_anchor[2],
                            g_first_person_standing_eye_z);
                        g_first_person_anchor[2] =
                            g_first_person_standing_eye_z;
                    }
                    else if (!same_actor ||
                             g_first_person_anchor[2] >
                                 g_first_person_standing_eye_z)
                    {
                        g_first_person_standing_instance =
                            g_first_person_instance;
                        g_first_person_standing_model =
                            g_first_person_model;
                        g_first_person_standing_eye_z =
                            g_first_person_anchor[2];
                        g_first_person_standing_eye_valid = true;
                    }
                }
                g_first_person_ground_anchor_valid =
                    traversal == TraversalGround;
                log("first-person: fixed root-local eye anchor (%.1f, %.1f, "
                    "%.1f), %.1f above HeadSegment; %s calibration",
                    g_first_person_anchor[0], g_first_person_anchor[1],
                    g_first_person_anchor[2],
                    config().first_person_eye_height,
                    g_first_person_ground_anchor_valid ? "ground" : "temporary");
            }

            g_first_person_anchor_valid = true;
            float animated_eye[3] = {
                relative[0], relative[1],
                relative[2] + config().first_person_eye_height
            };
            const float* local_eye = g_first_person_anchor;
            // Crouch: the anchored eye ignored Lara's crouch, so the view
            // stayed standing. When her animated eye drops more than 0.20 m
            // below the anchor, ease the view down with it (0.12 s). The
            // headset's own drop is already in the camera, so the anchor is
            // only lowered by what the physical crouch does not already
            // cover: the view goes down by the deeper of the two, never the
            // sum (user requirement: button + physical must not double).
            float crouched_eye[3];
            {
                const float scale = tune_world_scale();
                const float drop = g_first_person_anchor[2] - animated_eye[2];
                const float target = traversal == TraversalGround &&
                    scale > 0.0f && drop > 0.20f * scale &&
                    drop < 1.5f * scale ? drop : 0.0f;
                // The performance counter, not GetTickCount: its ~16 ms
                // steps made the 0.12 s ease advance in uneven jumps.
                static LARGE_INTEGER last{};
                static LARGE_INTEGER frequency{};
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                if (!frequency.QuadPart)
                    QueryPerformanceFrequency(&frequency);
                float dt = last.QuadPart && frequency.QuadPart
                    ? float(now.QuadPart - last.QuadPart) /
                      float(frequency.QuadPart)
                    : 0.0f;
                last = now;
                dt = fminf(fmaxf(dt, 0.0f), 0.1f);
                g_first_person_crouch_drop += (target -
                    g_first_person_crouch_drop) * (1.0f - expf(-dt / 0.12f));
                if (!std::isfinite(g_first_person_crouch_drop))
                    g_first_person_crouch_drop = 0.0f;
                g_first_person_crouched = target > 0.0f;
                const float physical = vr_head_drop();
                // A crouch the player's own body started belongs to the
                // headset: standing back up, the view held at Lara's
                // crouched eye until her stand clip caught up (a jerky
                // ascent, user test 2026-10-03). Until she is fully up
                // again the view follows only the real head; the full body
                // squashes or stretches to match (first_person_draw_full_body).
                static bool physical_owns = false;
                if (vr_input_physical_crouch_held())
                    physical_owns = true;
                else if (!g_first_person_crouched &&
                         g_first_person_crouch_drop < 0.02f * scale)
                    physical_owns = false;
                const float lower = physical_owns ? 0.0f
                    : physical > 0.0f
                    ? fmaxf(g_first_person_crouch_drop - physical, 0.0f)
                    : g_first_person_crouch_drop;
                if (traversal == TraversalGround && lower > 0.5f)
                {
                    memcpy(crouched_eye, g_first_person_anchor,
                           sizeof(crouched_eye));
                    crouched_eye[2] -= lower;
                    local_eye = crouched_eye;
                }
                static bool was_crouched = false;
                if (g_first_person_crouched != was_crouched)
                {
                    was_crouched = g_first_person_crouched;
                    log("first-person: Lara %s (animated eye %.2f m below "
                        "standing; headset %.2f m below its start)",
                        was_crouched ? "crouched -- view follows"
                                     : "stood up",
                        scale > 0.0f ? drop / scale : 0.0f,
                        scale > 0.0f ? physical / scale : 0.0f);
                }
            }
            if (traversal != TraversalGround)
            {
                const float dx = animated_eye[0] - g_first_person_anchor[0];
                const float dy = animated_eye[1] - g_first_person_anchor[1];
                const float dz = animated_eye[2] - g_first_person_anchor[2];
                if (dx*dx + dy*dy + dz*dz < 900.0f*900.0f)
                    local_eye = animated_eye;
            }
            for (int column = 0; column < 3; ++column)
            {
                filtered[column] = root_to_world.m[3][column];
                for (int row = 0; row < 3; ++row)
                    filtered[column] += local_eye[row] *
                                        root_to_world.m[row][column];
            }
            if (traversal != TraversalGround)
            {
                first_person_smooth_hanging_eye(filtered);
                first_person_mantle_eye_floor(filtered);
            }
            if (g_eye_hold_active)
            {
                // Back from a held eye: ease in from where the view is.
                g_eye_hold_active = false;
                float gap_sq = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    g_eye_return[k] = g_first_person_last_eye[k] - filtered[k];
                    gap_sq += g_eye_return[k] * g_eye_return[k];
                }
                const float scale = tune_world_scale() > 0.0f
                    ? tune_world_scale() : 1.0f;
                if (!std::isfinite(gap_sq) || gap_sq > 2.0f * 2.0f * scale *
                                                       scale)
                    memset(g_eye_return, 0, sizeof(g_eye_return));
                if (g_eye_hold_reports < 40)
                {
                    ++g_eye_hold_reports;
                    log("first-person: root back; view eases %.2f m to the "
                        "anchored eye", sqrtf(gap_sq) / scale);
                }
            }
            first_person_eye_return(filtered);
            memcpy(g_first_person_last_eye, filtered,
                   sizeof(g_first_person_last_eye));
            first_person_height_diag(
                local_eye != animated_eye ? "anchored"
                                                   : "animated (root-local)",
                true, root_distance, raw, root, filtered);
        }

        bool first_person_resolve(void* cam, short mode, float* head_position)
        {
            // Modes 4, 11 and 12 are authored cameras. They often follow an
            // actor but are authored shots, not playable viewpoints. Menus
            // likewise retain their existing camera; pause is the exception
            // because its frozen world should remain where play stopped.
            bool cinematic = false;
            __try
            {
                cinematic =
                    reinterpret_cast<PFN_CinematicPlaying>(
                        kCinematicPlaying)() ||
                    reinterpret_cast<PFN_CinematicPlaying>(
                        kResidentCinematicPlaying)();
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                cinematic = false;
            }
            if (cinematic || mode == 4 || mode == 11 || mode == 12 ||
                (ui_menu_active() && !ui_pause_menu_active()))
            {
                if (g_first_person_active &&
                    (cinematic || mode == 4 || mode == 11 || mode == 12))
                    log("first-person: cinematic fallback to third person "
                        "(camera mode %d, cinematic %d)",
                        mode, cinematic ? 1 : 0);
                first_person_leave();
                return false;
            }

            __try
            {
                unsigned char* camera = static_cast<unsigned char*>(cam);
                void* focus = *reinterpret_cast<void**>(
                    camera + kOffFocusInstance);
                void* player = *reinterpret_cast<void**>(kPlayerInstance);
                if (!focus || focus != player)
                {
                    first_person_leave();
                    return false;
                }

                unsigned char* instance = static_cast<unsigned char*>(focus);
                void* object = *reinterpret_cast<void**>(
                    instance + kOffInstanceObject);
                if (!object)
                {
                    first_person_leave();
                    return false;
                }

                unsigned char* obj = static_cast<unsigned char*>(object);
                const char* name = *reinterpret_cast<const char**>(
                    obj + kOffObjectName);
                // "lara" or an outfit object "lara_*" (lara_classic,
                // lara_biker, lara_young, ...: trl.exe strings 0x00F009FC..
                // 0x00F00C30). Exact "lara" only put every alternate skin
                // in third person (user report 2026-10-01). cine_/title_/
                // costume_lara do not match; the player check above stays.
                // The two Amanda costumes are the player objects
                // "amanda_player" / "amanda_player_alt" (strings 0x00F009D8,
                // 0x00F009EC). An enemy Amanda is never the player instance,
                // which the focus == player check above already requires.
                if (!name || (strcmp(name, "lara") != 0 &&
                              strncmp(name, "lara_", 5) != 0 &&
                              strncmp(name, "amanda_player", 13) != 0))
                {
                    static const void* reported = nullptr;
                    if (name && reported != object)
                    {
                        reported = object;
                        log("first-person: player object \"%s\" is not Lara; "
                            "third person", name);
                    }
                    first_person_leave();
                    return false;
                }

                const int num_models = *reinterpret_cast<short*>(
                    obj + kOffObjectNumModels);
                void** models = *reinterpret_cast<void***>(
                    obj + kOffObjectModelList);
                int model_index = *reinterpret_cast<signed char*>(
                    instance + kOffCurrentRenderModel);
                if (model_index < 0 || model_index >= num_models)
                    model_index = *reinterpret_cast<signed char*>(
                        instance + kOffCurrentBaseModel);
                if (!models || num_models <= 0 || num_models > 32 ||
                    model_index < 0 || model_index >= num_models ||
                    !models[model_index])
                {
                    first_person_leave();
                    return false;
                }
                void* model = models[model_index];

                void* player_prop = *reinterpret_cast<void**>(kPlayerProp);
                if (!player_prop)
                {
                    first_person_leave();
                    return false;
                }
                const int head_segment = *reinterpret_cast<int*>(
                    static_cast<unsigned char*>(player_prop) +
                    kOffPlayerPropHeadSegment);
                const int segment_count = *reinterpret_cast<int*>(
                    static_cast<unsigned char*>(model) +
                    kOffModelNumSegments);
                float* matrices = *reinterpret_cast<float**>(
                    instance + kOffInstanceMatrices);
                if (!matrices || head_segment < 0 ||
                    head_segment >= segment_count || segment_count > 512)
                {
                    first_person_leave();
                    return false;
                }

                const float* head = matrices + head_segment * 16 + 12;
                float raw_head[3] = { 0.0f, 0.0f, 0.0f };
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!std::isfinite(head[axis]) || fabsf(head[axis]) > 1.0e7f)
                    {
                        first_person_leave();
                        return false;
                    }
                    raw_head[axis] = head[axis];
                }

                if (focus != g_first_person_instance ||
                    model != g_first_person_model)
                {
                    first_person_leave();
                    if (model != g_first_person_hand_calibration_model)
                    {
                        g_first_person_hand_calibration_model = model;
                        g_first_person_hand_calibrated[0] = false;
                        g_first_person_hand_calibrated[1] = false;
                        log("first-person: hand calibration reset for new Lara model");
                    }
                    else
                    {
                        log("first-person: hand calibration preserved across camera transition");
                    }
                    g_first_person_instance = focus;
                    g_first_person_model = model;
                    g_first_person_head_segment = head_segment;
                    const unsigned char* prop =
                        static_cast<const unsigned char*>(player_prop);
                    const int shoulders[2] = {
                        *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropLeftShoulder),
                        *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropRightShoulder)
                    };
                    const int elbows[2] = {
                        *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropLeftElbow),
                        *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropRightElbow)
                    };
                    const int left_wrist = *reinterpret_cast<const int*>(
                        prop + kOffPlayerPropLeftWrist);
                    const int right_wrist = *reinterpret_cast<const int*>(
                        prop + kOffPlayerPropRightWrist);
                    for (int hand = 0; hand < 2; ++hand)
                    {
                        g_first_person_shoulder_segment[hand] =
                            shoulders[hand] >= 0 &&
                            shoulders[hand] < segment_count
                                ? shoulders[hand] : -1;
                        g_first_person_elbow_segment[hand] =
                            elbows[hand] >= 0 && elbows[hand] < segment_count
                                ? elbows[hand] : -1;
                    }
                    g_first_person_wrist_segment[0] =
                        left_wrist >= 0 && left_wrist < segment_count
                            ? left_wrist : -1;
                    g_first_person_wrist_segment[1] =
                        right_wrist >= 0 && right_wrist < segment_count
                            ? right_wrist : -1;
                    g_first_person_head_mask =
                        first_person_find_head_group(model, head_segment);

                    float hmd[3];
                    vr_head_position(hmd);
                    vr_set_head_position_origin(hmd, true);

                    if (g_first_person_head_mask)
                    {
                        uint32_t& normal = *reinterpret_cast<uint32_t*>(
                            instance + kOffNoDrawGroups);
                        uint32_t& fx = *reinterpret_cast<uint32_t*>(
                            instance + kOffFxNoDrawGroups);
                        g_first_person_saved_normal =
                            normal & g_first_person_head_mask;
                        g_first_person_saved_fx = fx & g_first_person_head_mask;
                        normal |= g_first_person_head_mask;
                        fx |= g_first_person_head_mask;
                        g_first_person_head_hidden = true;
                    }

                    log("first-person: active on Lara model %d, "
                        "HeadSegment %d at (%.1f, %.1f, %.1f)%s",
                        model_index, head_segment,
                        raw_head[0], raw_head[1], raw_head[2],
                        g_first_person_head_mask ? "" :
                        " [head draw group unresolved]");
                    log("first-person: arm joints left %d/%d/%d, "
                        "right %d/%d/%d (shoulder/elbow/wrist)",
                        g_first_person_shoulder_segment[0],
                        g_first_person_elbow_segment[0],
                        g_first_person_wrist_segment[0],
                        g_first_person_shoulder_segment[1],
                        g_first_person_elbow_segment[1],
                        g_first_person_wrist_segment[1]);
                }
                else if (g_first_person_head_hidden)
                {
                    // Game events can rewrite the mask. Reassert only our one
                    // bit and leave every other draw-group decision untouched.
                    *reinterpret_cast<uint32_t*>(
                        instance + kOffNoDrawGroups) |= g_first_person_head_mask;
                    *reinterpret_cast<uint32_t*>(
                        instance + kOffFxNoDrawGroups) |= g_first_person_head_mask;
                }

                const TraversalProbe traversal =
                    first_person_traversal_probe();
                if (traversal.kind != g_first_person_traversal)
                {
                    const char* labels[] = { "ground", "swim", "climb" };
                    log("first-person: traversal %s -> %s "
                        "(state %d, vtable %p, water %d, swim 0x%X, "
                        "markup %d, rope %d, head Z %.1f)",
                        labels[g_first_person_traversal],
                        labels[traversal.kind],
                        traversal.state_id,
                        reinterpret_cast<void*>(traversal.state_vtable),
                        traversal.water ? 1 : 0,
                        traversal.swim_status,
                        traversal.attached ? 1 : 0,
                        traversal.rope ? 1 : 0,
                        raw_head[2]);
                    if (g_first_person_traversal == TraversalClimb &&
                        traversal.kind == TraversalGround)
                        g_height_diag_until = GetTickCount() + 6000;
                    g_first_person_traversal = traversal.kind;
                }
                if ((g_first_person_ledge_hanging !=
                     traversal.ledge_hanging ||
                     g_first_person_bar_hanging !=
                     traversal.bar_hanging ||
                     g_first_person_vine_climbing !=
                     traversal.vine_climbing) &&
                    (g_first_person_ledge_hanging ||
                     g_first_person_bar_hanging ||
                     g_first_person_vine_climbing))
                {
                    for (LedgeHandGrip& grip : g_ledge_hand_grip)
                    {
                        grip.pending = false;
                        grip.held = false;
                        grip.in_grab_zone = false;
                    }
                    g_grab_zone_debug.valid = false;
                    g_first_person_vine_pull_hand = -1;
                    log("first-person: traversal hand holds released on state change");
                }
                if (g_first_person_ledge_hanging !=
                    traversal.ledge_hanging)
                    log("first-person: ledge hang %s (state %d, vtable %p)",
                        traversal.ledge_hanging ? "entered" : "left",
                        traversal.state_id,
                        reinterpret_cast<void*>(traversal.state_vtable));
                g_first_person_ledge_hanging = traversal.ledge_hanging;
                if (g_first_person_bar_hanging != traversal.bar_hanging)
                {
                    log("first-person: swing bar %s (state %d, vtable %p)",
                        traversal.bar_hanging ? "entered" : "left",
                        traversal.state_id,
                        reinterpret_cast<void*>(traversal.state_vtable));
                    g_first_person_bar_axis_valid = false;
                    g_first_person_bar_entered_at = traversal.bar_hanging
                        ? GetTickCount() : 0;
                }
                g_first_person_bar_hanging = traversal.bar_hanging;
                g_first_person_state_vtable = traversal.state_vtable;
                climb_anim_watch(focus, traversal.state_vtable,
                                 traversal.precarious);
                // auto_secure_catch: 0 off, 1 swing bars, 2 everything.
                const bool auto_secure =
                    config().auto_secure_catch == 2 ||
                    (config().auto_secure_catch == 1 &&
                     traversal.bar_hanging);
                if (traversal.precarious != g_first_person_grip_precarious)
                {
                    log("first-person: precarious one-hand catch %s",
                        !traversal.precarious ? "ended"
                        : auto_secure
                            ? "-- securing it automatically (auto_secure_catch)"
                            : "-- grab the hold to secure");
                    g_auto_secure_at = traversal.precarious ? GetTickCount() : 0;
                    g_auto_secure_pulses = 0;
                }
                g_first_person_grip_precarious = traversal.precarious;
                // auto_secure_catch: the Action pulse a grab would send,
                // 0.2 s after the catch (a press on its first frame may not
                // count), again every 0.6 s while it is still precarious.
                if (traversal.precarious && auto_secure &&
                    g_auto_secure_at && g_auto_secure_pulses < 4)
                {
                    const DWORD due = g_auto_secure_at + 200 +
                                      600 * g_auto_secure_pulses;
                    if (GetTickCount() >= due)
                    {
                        g_first_person_secure_requested_at = GetTickCount();
                        ++g_auto_secure_pulses;
                    }
                }
                if (traversal.prompt && config().immersive_controls)
                {
                    // The retail prompt (a giant glyph at the player's face
                    // that no draw hook reaches) is replaced by the mod's
                    // badge on the grab zone (vr_submit). Hide it with the
                    // flags ENEMY_Hide (0x004B36B0) sets -- +0xA4 |= 0x800,
                    // +0xA8 |= 0x20000000 -- and every draw group (+0xE0).
                    // ButtonPrompt::Hide kills it as usual.
                    __try
                    {
                        unsigned char* inst =
                            static_cast<unsigned char*>(traversal.prompt);
                        *reinterpret_cast<unsigned*>(inst + 0xA4) |= 0x800u;
                        *reinterpret_cast<unsigned*>(inst + 0xA8) |=
                            0x20000000u;
                        *reinterpret_cast<unsigned*>(inst + 0xE0) =
                            0xFFFFFFFFu;
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                }
                if (traversal.prompt != g_grip_prompt_instance)
                {
                    g_grip_prompt_instance = traversal.prompt;
                    g_grip_prompt_object = nullptr;
                    __try
                    {
                        if (traversal.prompt)
                            g_grip_prompt_object =
                                *reinterpret_cast<void* const*>(
                                    static_cast<unsigned char*>(
                                        traversal.prompt) +
                                    kOffInstanceObject);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        g_grip_prompt_instance = nullptr;
                    }
                    if (g_grip_prompt_instance)
                        log("first-person: secure-grip prompt object %p "
                            "tracked for the left hand",
                            g_grip_prompt_instance);
                }
                // In the tested pull-up logs, vtable 00F05804 is the retail
                // LedgeClimb state between leaving the hang and grounding.
                g_first_person_ledge_climbing =
                    traversal.state_vtable == 0x00F05804;
                if (g_first_person_vine_climbing != traversal.vine_climbing)
                    log("first-person: vertical handhold %s (state %d, vtable %p)",
                        traversal.vine_climbing ? "entered" : "left",
                        traversal.state_id,
                        reinterpret_cast<void*>(traversal.state_vtable));
                g_first_person_vine_climbing = traversal.vine_climbing;
                g_first_person_horizontal_ledge =
                    traversal.ledge_hanging;
                first_person_filter_anchor(raw_head, matrices,
                                           traversal.kind, head_position);
                g_first_person_active = true;
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                first_person_leave();
                return false;
            }
        }

        bool segment_is_in_head_subtree(const ModelSegment* segments,
                                        int count, int segment, int head)
        {
            for (int guard = 0; guard < count && segment >= 0; ++guard)
            {
                if (segment == head)
                    return true;
                if (segment >= count)
                    return false;
                const int parent = segments[segment].parent;
                if (parent == segment)
                    return false;
                segment = parent;
            }
            return false;
        }

        Mat4 matrix_from_floats(const float* values)
        {
            Mat4 result;
            memcpy(&result.m[0][0], values, sizeof(result.m));
            return result;
        }

        Mat4 rotation_only(const Mat4& value)
        {
            Mat4 result = value;
            result.m[3][0] = 0.0f;
            result.m[3][1] = 0.0f;
            result.m[3][2] = 0.0f;
            result.m[0][3] = 0.0f;
            result.m[1][3] = 0.0f;
            result.m[2][3] = 0.0f;
            result.m[3][3] = 1.0f;
            return result;
        }

        // ------------------------------------------------------------------
        // Hand model 2. The wrist hangs off SteamVR's grip pose (origin in
        // the palm, same axes for every controller, left and right mirror
        // images). For the right hand, in engine row-vector form:
        //
        //   wrist = T(-palm) * R_cal * Q(grip_hand_rot) * T(grip_hand_pos) * Grip
        //
        // palm   : Lara's palm point in wrist-local space (midway to the
        //          knuckle bones), so rotations pivot in the palm;
        // R_cal  : base rotation, converted once from the original model so
        //          the hand keeps its tuned orientation;
        // Q, pos : tuning, in the grip's own axes.
        //
        // The left hand is the mirror of that: F * H_right * S, with F a flip
        // of Lara's lateral wrist axis (verified from her palm bones) and S
        // a flip of the grip's x. Guns and pistols ride on the same wrist
        // targets, so every weapon lines up the same way in either hand.
        // ------------------------------------------------------------------
        void* g_grip_palm_model = nullptr;
        bool g_grip_palm_valid[2] = { false, false };
        float g_grip_palm[2][3]{};
        int g_grip_mirror_axis = -1;
        bool g_grip_mirror_reported = false;
        bool g_grip_model_reported[2] = { false, false };

        bool first_person_palm_local(int hand, float out[3])
        {
            if (g_grip_palm_model != g_first_person_model)
            {
                g_grip_palm_model = g_first_person_model;
                g_grip_palm_valid[0] = g_grip_palm_valid[1] = false;
                g_grip_mirror_axis = -1;
                g_grip_mirror_reported = false;
            }
            if (!g_grip_palm_valid[hand])
            {
                __try
                {
                    unsigned char* model =
                        static_cast<unsigned char*>(g_first_person_model);
                    const int count = model ? *reinterpret_cast<int*>(
                        model + kOffModelNumSegments) : 0;
                    ModelSegment* segments = model
                        ? *reinterpret_cast<ModelSegment**>(
                              model + kOffModelSegments)
                        : nullptr;
                    const float* matrices = g_first_person_instance
                        ? *reinterpret_cast<float* const*>(
                              static_cast<unsigned char*>(
                                  g_first_person_instance) +
                              kOffInstanceMatrices)
                        : nullptr;
                    const int wrist = g_first_person_wrist_segment[hand];
                    if (!segments || !matrices || count <= 0 ||
                        count > 512 || wrist < 0 || wrist >= count)
                        return false;
                    const Mat4 w = matrix_from_floats(matrices + wrist * 16);
                    float sum[3]{};
                    int children = 0;
                    for (int s = 0; s < count; ++s)
                    {
                        if (s == wrist || segments[s].parent != wrist)
                            continue;
                        float d[3];
                        for (int k = 0; k < 3; ++k)
                            d[k] = matrices[s * 16 + 12 + k] - w.m[3][k];
                        for (int j = 0; j < 3; ++j)
                            sum[j] += d[0] * w.m[j][0] + d[1] * w.m[j][1] +
                                      d[2] * w.m[j][2];
                        ++children;
                    }
                    if (!children)
                        return false;
                    for (int j = 0; j < 3; ++j)
                        g_grip_palm[hand][j] = 0.5f * sum[j] / children;
                    g_grip_palm_valid[hand] = true;
                    log("first-person: %s palm point (%.1f, %.1f, %.1f) in "
                        "wrist space from %d knuckle bones",
                        hand == 0 ? "left" : "right", g_grip_palm[hand][0],
                        g_grip_palm[hand][1], g_grip_palm[hand][2], children);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }
            for (int k = 0; k < 3; ++k)
                out[k] = g_grip_palm[hand][k];
            return true;
        }

        // Lara's lateral wrist axis: the one whose flip maps the right palm
        // onto the left palm. -1 if no single flip fits.
        int first_person_grip_mirror_axis()
        {
            float left[3], right[3];
            if (!first_person_palm_local(0, left) ||
                !first_person_palm_local(1, right))
                return -1;
            if (g_grip_mirror_axis >= 0 || g_grip_mirror_reported)
                return g_grip_mirror_axis;
            const float length = sqrtf(right[0] * right[0] +
                                       right[1] * right[1] +
                                       right[2] * right[2]);
            int best = -1;
            float best_error = 1.0e9f;
            for (int k = 0; k < 3; ++k)
            {
                float e = 0.0f;
                for (int j = 0; j < 3; ++j)
                {
                    const float mirrored = j == k ? -right[j] : right[j];
                    e += (left[j] - mirrored) * (left[j] - mirrored);
                }
                e = sqrtf(e);
                if (e < best_error)
                {
                    best_error = e;
                    best = k;
                }
            }
            g_grip_mirror_reported = true;
            if (length > 1.0f && best_error < 0.3f * length)
                g_grip_mirror_axis = best;
            log("first-person: left hand %s the right (wrist axis %c, palm "
                "mismatch %.1f of %.1f units)",
                g_grip_mirror_axis >= 0 ? "mirrors" : "does NOT mirror",
                "xyz"[best < 0 ? 0 : best], best_error, length);
            return g_grip_mirror_axis;
        }

        // R_cal, converted once from the original right-hand calibration and
        // alignment so the hand keeps its orientation: the old wrist rotation
        // was cal * align * rot(Raw); Grip = O * Raw, so R_cal = cal * align
        // * inv(rot(O)).
        bool first_person_grip_base_rotation(Mat4* out)
        {
            float cal[9]{};
            if (!tune_grip_hand_calibration(cal))
            {
                Mat4 offset;
                if (!vr_controller_grip_offset(false, &offset))
                    return false;
                float old[9]{};
                Mat4 legacy = Mat4::identity();
                if (tune_load_hand_calibration(false, old))
                {
                    for (int r = 0; r < 3; ++r)
                        for (int k = 0; k < 3; ++k)
                            legacy.m[r][k] = old[r * 3 + k];
                }
                else if (g_first_person_hand_calibrated[1])
                    legacy = rotation_only(g_first_person_hand_calibration[1]);
                else
                    return false;
                float align[3]{};
                tune_hand_rotation(false, align);
                const Mat4 base = legacy *
                    hand_alignment_rotation(align[0], align[1], align[2]) *
                    rigid_inverse(rotation_only(offset));
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k)
                        cal[r * 3 + k] = base.m[r][k];
                if (!tune_set_grip_hand_calibration(cal))
                    return false;
                log("first-person: grip hand model: base rotation converted "
                    "from the original right-hand tuning and saved");
            }
            *out = Mat4::identity();
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k)
                    out->m[r][k] = cal[r * 3 + k];
            return true;
        }

        // Units per metre for the tuned grip offset; 0 = tune_world_scale().
        // The third-person hands set it to the unscaled world scale while
        // they are placed: in board mode their projection already scales
        // them by F, and a board-scaled offset was applied twice (hands a
        // hand-length too far forward, user test 2026-10-02).
        float g_grip_offset_metre = 0.0f;

        // H: wrist-local -> grip-local for this hand.
        bool first_person_grip_hand_transform(int hand, Mat4* out)
        {
            if (config().hand_model != 2 || !out)
                return false;
            float palm[3];
            Mat4 base;
            if (!first_person_palm_local(1, palm) ||
                !first_person_grip_base_rotation(&base))
                return false;
            float position[3], rotation[3];
            tune_grip_hand(position, rotation);
            const float scale = g_grip_offset_metre > 0.0f
                ? g_grip_offset_metre : tune_world_scale();
            if (!std::isfinite(scale) || scale <= 0.0f)
                return false;
            const Mat4 right =
                translation(-palm[0], -palm[1], -palm[2]) * base *
                hand_alignment_rotation(rotation[0], rotation[1],
                                        rotation[2]) *
                translation(position[0] * scale, position[1] * scale,
                            position[2] * scale);
            if (hand == 1)
            {
                *out = right;
                return true;
            }
            const int axis = first_person_grip_mirror_axis();
            if (axis < 0)
                return false;
            Mat4 flip_wrist = Mat4::identity();
            flip_wrist.m[axis][axis] = -1.0f;
            Mat4 flip_grip = Mat4::identity();
            flip_grip.m[0][0] = -1.0f;
            *out = flip_wrist * right * flip_grip;
            return true;
        }

        bool first_person_grip_model_ready(int hand)
        {
            Mat4 unused;
            const bool ready = config().hand_model == 2 &&
                vr_controller_grip_offset(hand == 0, nullptr) &&
                first_person_grip_hand_transform(hand, &unused);
            if (ready && !g_grip_model_reported[hand])
            {
                g_grip_model_reported[hand] = true;
                log("first-person: %s hand now on the grip-pose model",
                    hand == 0 ? "left" : "right");
            }
            return ready;
        }

        // Current recoil strength of one controller, 0 when at rest: a 20 ms
        // rise, then an exponential return (70 ms time constant).
        float recoil_factor(const Recoil& r, DWORD now)
        {
            if (!r.start)
                return 0.0f;
            const float t = (now - r.start) / 1000.0f;
            if (t < 0.0f || t > 0.30f)
                return 0.0f;
            return r.amp * (t < 0.02f ? t / 0.02f
                                      : expf(-(t - 0.02f) / 0.07f));
        }

        // Rotate v about unit axis k by angle a (right-handed).
        void rotate_about(const float* k, float a, const float* v,
                          float* out)
        {
            const float c = cosf(a), s = sinf(a);
            const float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
            const float cross[3] = {
                k[1] * v[2] - k[2] * v[1],
                k[2] * v[0] - k[0] * v[2],
                k[0] * v[1] - k[1] * v[0]
            };
            for (int i = 0; i < 3; ++i)
                out[i] = v[i] * c + cross[i] * s + k[i] * kv * (1.0f - c);
        }

        void apply_recoil(bool left, Mat4* world)
        {
            const Recoil& r = g_recoil[left ? 0 : 1];
            const float f = recoil_factor(r, GetTickCount());
            if (f <= 0.0f)
                return;
            // Rows 0-2 are the controller's axes in world space; rotating
            // them pivots the hand and gun about the palm (row 3).
            if (r.rotate)
                for (int row = 0; row < 3; ++row)
                {
                    float rotated[3];
                    rotate_about(r.axis, r.angle * f, world->m[row],
                                 rotated);
                    for (int k = 0; k < 3; ++k)
                        world->m[row][k] = rotated[k];
                }
            for (int k = 0; k < 3; ++k)
                world->m[3][k] += r.back[k] * r.kick * f;
        }

        bool controller_head_pose_for_hands(bool left, Mat4* to_head)
        {
            if (first_person_grip_model_ready(left ? 0 : 1))
                return vr_controller_grip_head_pose(left, to_head);
            if (!vr_controller_head_pose(left, to_head))
                return false;
            float offset[3]{};
            tune_hand_offset(left, offset);
            *to_head = hand_pose_with_head_offset(
                *to_head, offset[0], offset[1], offset[2]);
            return true;
        }

        // Unit barrel direction in world space for a controller world pose:
        // the aim ray's controller-local direction (mirrored for the left
        // grip), exactly as publish_head_aim builds it.
        bool barrel_world(bool left, const Mat4& world, float barrel[3])
        {
            float pitch = 0.0f, yaw = 0.0f;
            if (config().hand_model == 2 && tune_grip_aim(&pitch, &yaw))
            {
                if (left)
                    yaw = -yaw;
            }
            else
                tune_aim_offset(&pitch, &yaw);
            pitch *= 0.0174532925f;
            yaw *= 0.0174532925f;
            const float local[3] = { sinf(yaw) * cosf(pitch), -sinf(pitch),
                                     cosf(yaw) * cosf(pitch) };
            for (int i = 0; i < 3; ++i)
                barrel[i] = local[0] * world.m[0][i] +
                            local[1] * world.m[1][i] +
                            local[2] * world.m[2][i];
            return normalise3(barrel) > 1.0e-4f;
        }

        // Per-weapon foregrip, in metres, in a frame built from the trigger
        // hand: forward along the barrel (aim) direction, right and up
        // perpendicular to it from the hand's own axes. Stored in
        // [developer] foregrip_<weapon>_forward/right/up, <weapon> being the
        // object name before "_rbweapon" (smgmpfive, autorifle, ...).
        // Defaults from the first headset screenshots: the pinned hand sat
        // beside the receiver, right of the gun and short of the fore-end.
        struct Foregrip
        {
            char weapon[40]{};
            float offset[3]{};       // forward, right, up
        };
        Foregrip g_foregrips[12];
        int g_foregrip_count = 0;
        bool g_foregrip_tuning = false;
        bool g_foregrip_tuning_read = false;

        void foregrip_keys(const char* weapon, wchar_t keys[3][96])
        {
            wchar_t wide[40]{};
            for (int i = 0; i < 39 && weapon[i]; ++i)
                wide[i] = static_cast<wchar_t>(weapon[i]);
            const wchar_t* axes[3] = { L"forward", L"right", L"up" };
            for (int a = 0; a < 3; ++a)
                swprintf_s(keys[a], 96, L"foregrip_%s_%s", wide, axes[a]);
        }

        // The left-handed player's fine adjustment, added to every gun's
        // (right-handed) foregrip: one tweak for all two-handed guns rather
        // than a second per-weapon tune (user request after the mirrored
        // right-hand tuning landed slightly off). Stored as
        // foregrip_left_adjust_forward/right/up.
        const char* const kLeftAdjust = "left_adjust";

        void foregrip_defaults(const char* weapon, float out[3])
        {
            if (strcmp(weapon, kLeftAdjust) == 0)
            {
                out[0] = out[1] = out[2] = 0.0f;
                return;
            }
            const bool rifle = strcmp(weapon, "smgmpfive") != 0;
            out[0] = config().two_handed_grip_distance +
                     (rifle ? 0.12f : 0.06f);
            out[1] = -0.07f;
            out[2] = 0.0f;
        }

        Foregrip* foregrip_for(const char* object_name)
        {
            char weapon[40]{};
            strncpy_s(weapon, object_name ? object_name : "gun", _TRUNCATE);
            if (char* suffix = strstr(weapon, "_rbweapon"))
                *suffix = 0;
            for (int i = 0; i < g_foregrip_count; ++i)
                if (strcmp(g_foregrips[i].weapon, weapon) == 0)
                    return &g_foregrips[i];
            Foregrip* grip = g_foregrip_count < 12
                ? &g_foregrips[g_foregrip_count++] : &g_foregrips[11];
            strncpy_s(grip->weapon, weapon, _TRUNCATE);
            float defaults[3] = {};
            foregrip_defaults(weapon, defaults);
            wchar_t path[MAX_PATH]{}, keys[3][96]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            foregrip_keys(weapon, keys);
            for (int a = 0; a < 3; ++a)
            {
                wchar_t text[64]{};
                GetPrivateProfileStringW(L"developer", keys[a], L"", text,
                                         64, path);
                wchar_t* end = nullptr;
                const float value = text[0] ? wcstof(text, &end) : 0.0f;
                grip->offset[a] = text[0] && end != text &&
                    std::isfinite(value) && fabsf(value) <= 1.0f
                    ? value : defaults[a];
            }
            log("first-person: %s foregrip forward %.2f, right %+.2f, up "
                "%+.2f m", weapon, grip->offset[0], grip->offset[1],
                grip->offset[2]);
            return grip;
        }

        // The foregrip as a world-space vector from the trigger palm.
        bool foregrip_world(bool left, const Mat4& world, float out[3])
        {
            float barrel[3], up[3], right[3];
            if (!barrel_world(left, world, barrel))
                return false;
            // Engine local Y is down: -row1 is the hand's up, row0 its right.
            for (int k = 0; k < 3; ++k)
            {
                up[k] = -world.m[1][k];
                right[k] = world.m[0][k];
            }
            const float bu = dot3(barrel, up);
            for (int k = 0; k < 3; ++k)
                up[k] -= barrel[k] * bu;
            if (normalise3(up) < 1.0e-4f)
                return false;
            const float br = dot3(barrel, right), ur = dot3(up, right);
            for (int k = 0; k < 3; ++k)
                right[k] -= barrel[k] * br + up[k] * ur;
            if (normalise3(right) < 1.0e-4f)
                return false;
            const Foregrip* grip = foregrip_for(g_gameplay_gun.name);
            float offset[3] = { grip->offset[0], grip->offset[1],
                                grip->offset[2] };
            if (config().left_handed)
            {
                const Foregrip* adjust = foregrip_for(kLeftAdjust);
                for (int a = 0; a < 3; ++a)
                    offset[a] += adjust->offset[a];
            }
            const float metre = tune_world_scale();
            for (int k = 0; k < 3; ++k)
                out[k] = (barrel[k] * offset[0] + right[k] * offset[1] +
                          up[k] * offset[2]) * metre;
            return true;
        }

        bool foregrip_save(const Foregrip* grip)
        {
            wchar_t path[MAX_PATH]{}, keys[3][96]{}, value[32]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            foregrip_keys(grip->weapon, keys);
            bool ok = true;
            for (int a = 0; a < 3; ++a)
            {
                swprintf_s(value, L"%.2f", grip->offset[a]);
                ok = WritePrivateProfileStringW(L"developer", keys[a], value,
                                                path) != 0 && ok;
            }
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            return ok;
        }

        // [developer] foregrip_tuning_debug = 1: while a long gun is drawn
        // (held two-handed or not), Numpad 4/6 move the foregrip left/right,
        // 8/2 forward/back, 9/3 up/down (1 cm), 5 resets. Every change is
        // saved for this weapon at once; 7 saves again. (Headset test: the
        // first version only listened while held, so a 7 pressed after
        // letting go was ignored and the tuning was lost.)
        void foregrip_tuning_update()
        {
            if (!g_foregrip_tuning_read)
            {
                wchar_t path[MAX_PATH]{};
                swprintf_s(path, L"%strlvr.ini", exe_dir());
                g_foregrip_tuning = GetPrivateProfileIntW(L"developer",
                    L"foregrip_tuning_debug", 0, path) != 0;
                g_foregrip_tuning_read = true;
            }
            if (!g_foregrip_tuning || !g_gameplay_gun.name[0])
                return;
            HWND foreground = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            if (!foreground || pid != GetCurrentProcessId())
                return;
            static bool down[8]{};
            static ULONGLONG repeat_at[8]{};
            const ULONGLONG now = GetTickCount64();
            auto key = [&](int slot, int vk, bool repeat) {
                if (!(GetAsyncKeyState(vk) & 0x8000))
                    return down[slot] = false;
                if (!down[slot])
                {
                    down[slot] = true;
                    repeat_at[slot] = now + 300;
                    return true;
                }
                if (repeat && now >= repeat_at[slot])
                {
                    repeat_at[slot] = now + 80;
                    return true;
                }
                return false;
            };
            // Right-handed: this gun's foregrip. Left-handed: the shared
            // adjustment on top of every gun's right-handed foregrip.
            const bool left = config().left_handed;
            Foregrip* grip = left ? foregrip_for(kLeftAdjust)
                                  : foregrip_for(g_gameplay_gun.name);
            const int keys[6] = { VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD8,
                                  VK_NUMPAD2, VK_NUMPAD9, VK_NUMPAD3 };
            const int axes[6] = { 1, 1, 0, 0, 2, 2 };
            // The left grip frame is mirrored, so its "right" axis points to
            // the player's left: flip 4/6 there so 4 still moves left.
            const float side = left ? -1.0f : 1.0f;
            const float signs[6] = { -side, +side, +1, -1, +1, -1 };
            bool changed = false;
            for (int i = 0; i < 6; ++i)
                if (key(i, keys[i], true))
                {
                    grip->offset[axes[i]] = fmaxf(-1.0f, fminf(1.0f,
                        grip->offset[axes[i]] + signs[i] * 0.01f));
                    changed = true;
                }
            if (key(6, VK_NUMPAD5, false))
            {
                // Back to the built-in defaults (zero for the adjustment).
                foregrip_defaults(grip->weapon, grip->offset);
                changed = true;
            }
            const bool save_key = key(7, VK_NUMPAD7, false);
            if (changed || save_key)
            {
                const bool ok = foregrip_save(grip);
                log("tune: %s foregrip%s forward %+.2f, right %+.2f, up %+.2f "
                    "m %s", left ? "left-handed" : grip->weapon,
                    left ? " adjustment (all guns)" : "",
                    grip->offset[0], grip->offset[1],
                    grip->offset[2],
                    ok ? "saved to trlvr.ini" : "FAILED to save");
            }
        }

        // Two-handed long-gun hold. `world` is the trigger hand's pose built
        // with `camera_to_world`; the free hand's pose is built with the
        // same camera so both are in one frame.
        void two_hand_adjust(bool left, const Mat4& camera_to_world,
                             Mat4* world)
        {
            TwoHand& hold = g_two_hand;
            const DWORD now = GetTickCount();
            const bool gun_drawn = config().two_handed_guns &&
                g_first_person_active && g_gameplay_gun.instance &&
                now - g_gameplay_gun.tick <= 250;
            if (!gun_drawn || left != config().left_handed)
            {
                if (!gun_drawn)
                    hold.engaged = false;
                return;
            }
            foregrip_tuning_update();
            const bool off_left = !left;
            Mat4 off_to_head;
            float barrel[3];
            if (!controller_head_pose_for_hands(off_left, &off_to_head) ||
                !barrel_world(left, *world, barrel))
            {
                hold.engaged = false;
                return;
            }
            const Mat4 off = off_to_head * camera_to_world;
            const float metre = tune_world_scale();
            float foregrip[3];
            if (!(metre > 0.0f) || !foregrip_world(left, *world, foregrip))
                return;
            float reach[3];
            for (int k = 0; k < 3; ++k)
                reach[k] = off.m[3][k] - world->m[3][k];
            const float distance = sqrtf(dot3(reach, reach));
            const float grip_at = sqrtf(dot3(foregrip, foregrip));
            // Free hand's distance from the foregrip. Unheld, the foregrip is
            // fixed on the gun; held, the gun already turns the foregrip
            // toward the free hand, so only the distance along it counts.
            float from_grip = 0.0f;
            if (hold.engaged)
                from_grip = fabsf(distance - grip_at);
            else
            {
                float e[3];
                for (int k = 0; k < 3; ++k)
                    e[k] = reach[k] - foregrip[k];
                from_grip = sqrtf(dot3(e, e));
            }
            const bool in_reach =
                from_grip <= config().two_handed_radius * metre;
            const bool out_of_reach =
                from_grip > config().two_handed_release_radius * metre;
            if (in_reach)
                hold.zone_tick = now ? now : 1;

            const bool grip = vr_input_grip_held(off_left);
            const bool pressed = grip && !hold.grip_was;
            hold.grip_was = grip;
            const int mode = config().two_handed_mode;
            // Toggle and hold still let go if the hand ends up far away.
            const bool far_away = from_grip > 0.50f * metre;
            bool engage = false, release = false;
            if (mode == 1)
            {
                engage = !hold.engaged && pressed && in_reach;
                release = hold.engaged && (pressed || far_away);
            }
            else if (mode == 2)
            {
                engage = !hold.engaged && grip && in_reach;
                release = hold.engaged && (!grip || far_away);
            }
            else
            {
                engage = !hold.engaged && in_reach;
                release = hold.engaged && out_of_reach;
            }
            if (engage)
            {
                hold.engaged = true;
                vr_input_weapon_haptic(off_left, 0.3f);
                static unsigned reports = 0;
                if (reports++ < 8)
                    log("first-person: two-handed hold engaged by the %s "
                        "hand (%s mode), %.2f m from the foregrip",
                        off_left ? "left" : "right",
                        mode == 1 ? "toggle" : mode == 2 ? "hold" : "auto",
                        from_grip / metre);
            }
            else if (release)
            {
                hold.engaged = false;
                vr_input_weapon_haptic(off_left, 0.1f);
                static unsigned reports = 0;
                if (reports++ < 8)
                    log("first-person: two-handed hold released (%.2f m "
                        "from the foregrip)", from_grip / metre);
            }
            if (!hold.engaged || distance < 0.08f * metre)
                return;

            // Turn the trigger hand about its palm so the foregrip points at
            // the free hand.
            float from[3], to[3];
            for (int k = 0; k < 3; ++k)
            {
                from[k] = foregrip[k];
                to[k] = reach[k];
            }
            if (normalise3(from) < 1.0e-4f || normalise3(to) < 1.0e-4f)
                return;
            float axis[3] = {
                from[1] * to[2] - from[2] * to[1],
                from[2] * to[0] - from[0] * to[2],
                from[0] * to[1] - from[1] * to[0]
            };
            const float sine = normalise3(axis);
            if (sine < 1.0e-5f)
                return;
            const float angle = atan2f(sine, dot3(from, to));
            for (int row = 0; row < 3; ++row)
            {
                float rotated[3];
                rotate_about(axis, angle, world->m[row], rotated);
                for (int k = 0; k < 3; ++k)
                    world->m[row][k] = rotated[k];
            }
        }

        // While held, Lara's free hand is pinned to the foregrip on the
        // (steered, recoiling) gun instead of following its controller's
        // position; its orientation still follows the controller.
        void two_hand_pin_off_hand(bool left, Mat4* world);

        bool first_person_controller_world_pose(bool left, Mat4* world)
        {
            Mat4 controller_to_head;
            if (!controller_head_pose_for_hands(left, &controller_to_head))
                return false;

            Mat4 camera_to_world;
            memcpy(&camera_to_world.m[0][0],
                   static_cast<const unsigned char*>(kMainCamera) +
                       kOffCwTransform,
                   sizeof(camera_to_world.m));
            *world = controller_to_head * camera_to_world;
            if (!g_recoil_bypass)
            {
                two_hand_adjust(left, camera_to_world, world);
                apply_recoil(left, world);
                two_hand_pin_off_hand(left, world);
            }
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    if (!std::isfinite(world->m[row][column]))
                        return false;
            return true;
        }

        void two_hand_pin_off_hand(bool left, Mat4* world)
        {
            if (!g_two_hand.engaged || left == config().left_handed ||
                !g_first_person_active || !g_gameplay_gun.instance ||
                GetTickCount() - g_gameplay_gun.tick > 250)
                return;
            Mat4 trigger;
            float foregrip[3];
            const bool trigger_left = config().left_handed;
            if (!first_person_controller_world_pose(trigger_left, &trigger) ||
                !foregrip_world(trigger_left, trigger, foregrip))
                return;
            for (int k = 0; k < 3; ++k)
                world->m[3][k] = trigger.m[3][k] + foregrip[k];
        }

        // Start a recoil kick and a vibration for one of Lara's shots, on
        // the controller that holds the firing gun.
        void recoil_register_shot(void* weapon)
        {
            if (!g_first_person_active || !g_first_person_instance ||
                !weapon)
                return;
            bool left = false, long_gun = false;
            __try
            {
                unsigned char* inst = static_cast<unsigned char*>(weapon);
                const unsigned char* state =
                    *reinterpret_cast<unsigned char* const*>(inst + 0x23C);
                if (!state || *reinterpret_cast<void* const*>(state + 0x100)
                        != *reinterpret_cast<void* const*>(kPlayerInstance))
                    return;
                const unsigned char* object =
                    *reinterpret_cast<unsigned char* const*>(
                        inst + kOffInstanceObject);
                const char* name = object
                    ? *reinterpret_cast<const char* const*>(
                          object + kOffObjectName)
                    : nullptr;
                if (weapon == g_gameplay_gun.instance ||
                    weapon_is_long_gun(name))
                {
                    long_gun = true;
                    left = config().left_handed;
                }
                else if (weapon_is_pistol(name))
                {
                    // Akimbo pistols alternate. The firing pistol's own
                    // position (+0x10, where retail starts its bullet) sits
                    // at one of Lara's animated wrists; the nearer wrist is
                    // the hand. The link marker (4 left, 6 right) was not
                    // enough: headset test showed only one hand kicking.
                    const float* position = reinterpret_cast<const float*>(
                        inst + 0x10);
                    const float* lara_matrices =
                        *reinterpret_cast<float* const*>(
                            static_cast<unsigned char*>(
                                g_first_person_instance) +
                            kOffInstanceMatrices);
                    float d2[2] = { 1.0e30f, 1.0e30f };
                    for (int h = 0; h < 2 && lara_matrices; ++h)
                    {
                        const int w = g_first_person_wrist_segment[h];
                        if (w < 0)
                            continue;
                        d2[h] = 0.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            const float e = position[k] -
                                lara_matrices[w * 16 + 12 + k];
                            d2[h] += e * e;
                        }
                    }
                    const bool linked = *reinterpret_cast<void**>(
                        inst + 0xB8) == g_first_person_instance;
                    const short marker = linked
                        ? *reinterpret_cast<short*>(inst + 0xBE) : -1;
                    const int side = pistol_side(weapon);
                    if (side >= 0)
                        left = side == 0;  // learnt from its hip marker
                    else if (d2[0] < 1.0e30f || d2[1] < 1.0e30f)
                        left = d2[0] < d2[1];
                    else
                        left = marker == 4 ? true
                             : marker == 6 ? false
                             : config().left_handed;
                    static unsigned pistol_reports = 0;
                    if (pistol_reports++ < 12)
                        log("aim: pistol shot from instance %p (marker %d), "
                            "wrist distance left %.1f right %.1f -> %s hand",
                            weapon, marker, sqrtf(d2[0]), sqrtf(d2[1]),
                            left ? "left" : "right");
                }
                else
                    return;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }

            vr_input_weapon_haptic(left, long_gun ? 0.85f : 0.5f);
            const float strength = config().weapon_recoil;
            if (strength <= 0.0f)
                return;

            Mat4 world;
            g_recoil_bypass = true;
            const bool posed = first_person_controller_world_pose(left,
                                                                  &world);
            g_recoil_bypass = false;
            if (!posed)
                return;
            float barrel[3]{};
            if (!barrel_world(left, world, barrel))
                return;

            Recoil& r = g_recoil[left ? 0 : 1];
            const DWORD now = GetTickCount();
            // Rapid fire builds up a little instead of restarting flat.
            const float residual = recoil_factor(r, now);
            // World Z is up; the axis barrel x up tips the muzzle upward.
            float axis[3] = { barrel[1], -barrel[0], 0.0f };
            r.rotate = normalise3(axis) > 0.05f;
            for (int k = 0; k < 3; ++k)
            {
                r.axis[k] = axis[k];
                r.back[k] = -barrel[k];
            }
            r.amp = fminf(1.6f, 1.0f + residual * 0.5f);
            // A two-handed hold steadies the gun.
            if (long_gun && g_two_hand.engaged)
                r.amp *= 0.5f;
            r.angle = (long_gun ? 4.0f : 7.0f) * 0.0174532925f * strength;
            r.kick = (long_gun ? 0.025f : 0.03f) * tune_world_scale() *
                     strength;
            r.start = now ? now : 1;
        }

        Mat4 first_person_hand_delta(int hand, const Mat4& original_wrist,
                                     const Mat4& controller)
        {
            if (first_person_grip_model_ready(hand))
            {
                Mat4 grip_hand;
                if (first_person_grip_hand_transform(hand, &grip_hand))
                    return rigid_inverse(original_wrist) * grip_hand *
                           controller;
            }
            const Mat4 controller_rotation = rotation_only(controller);
            if (!g_first_person_hand_calibrated[hand])
            {
                float saved[9]{};
                if (tune_load_hand_calibration(hand == 0, saved))
                {
                    g_first_person_hand_calibration[hand] = Mat4::identity();
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column)
                            g_first_person_hand_calibration[hand].m[row][column] =
                                saved[row * 3 + column];
                    log("first-person: restored persistent %s hand calibration",
                        hand == 0 ? "left" : "right");
                }
                else
                {
                    g_first_person_hand_calibration[hand] =
                        rotation_only(original_wrist) *
                        rigid_inverse(controller_rotation);
                    float captured[9]{};
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column)
                            captured[row * 3 + column] =
                                g_first_person_hand_calibration[hand].m[row][column];
                    if (tune_save_hand_calibration(hand == 0, captured))
                        log("first-person: persisted initial %s hand calibration",
                            hand == 0 ? "left" : "right");
                }
                g_first_person_hand_calibrated[hand] = true;
            }
            float alignment[3]{};
            tune_hand_rotation(hand == 0, alignment);
            Mat4 target = g_first_person_hand_calibration[hand] *
                          hand_alignment_rotation(
                              alignment[0], alignment[1], alignment[2]) *
                          controller_rotation;
            for (int axis = 0; axis < 3; ++axis)
                target.m[3][axis] = controller.m[3][axis];
            return rigid_inverse(original_wrist) * target;
        }

        void report_hand_cpu_vertex_bounds(int hand,
                                            const unsigned char* source,
                                            int vertex_count,
                                            const bool* selected_bones,
                                            int segment_count,
                                            const Mat4& original_wrist)
        {
            const unsigned mark = tune_ledge_visibility_marker();
            if (!g_first_person_ledge_hanging)
            {
                if (g_hand_vertex_ground_reported[hand])
                    return;
                g_hand_vertex_ground_reported[hand] = true;
            }
            else
            {
                if (mark != g_hand_vertex_trace_mark[hand])
                {
                    g_hand_vertex_trace_mark[hand] = mark;
                    g_hand_vertex_trace_count[hand] = 0;
                    g_hand_vertex_trace_time[hand] = 0;
                }
                const DWORD now = GetTickCount();
                if (g_hand_vertex_trace_count[hand] >= 16 ||
                    g_hand_vertex_trace_total[hand] >= 80 ||
                    (g_hand_vertex_trace_time[hand] &&
                     now - g_hand_vertex_trace_time[hand] < 300))
                    return;
                g_hand_vertex_trace_time[hand] = now;
                ++g_hand_vertex_trace_count[hand];
                ++g_hand_vertex_trace_total[hand];
            }

            // D3D_GetVB's first full-model allocation is indexed by source
            // model vertex. Its retail BONEVERTEX records contain world XYZ
            // in their first three floats (24-byte stride). A separate
            // per-strip allocation may have the same count but a different
            // index order, so detour_get_bone_vb keeps the first one.
            const unsigned char* output = g_hand_cpu_vertex_buffer;
            if (!output || g_hand_cpu_vertex_expected_count !=
                            static_cast<unsigned>(vertex_count))
            {
                log("render: hand CPU vertex sample %u %s unavailable "
                    "(buffer=%p expected=%u source=%d)",
                    mark, hand == 0 ? "left" : "right", output,
                    g_hand_cpu_vertex_expected_count, vertex_count);
                return;
            }

            // BONEVERTEX XYZ is already in view space: the retail character
            // vertex shader applies a pure projection at c0-c3. Applying the
            // camera transform again made the previous diagnostic's signed
            // depth and controller distance meaningless.
            float camera_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
            float camera_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            unsigned selected = 0, finite = 0, behind = 0,
                     near_vertices = 0;
            __try
            {
                for (int vertex = 0; vertex < vertex_count; ++vertex)
                {
                    const int bone = *reinterpret_cast<const short*>(
                        source + vertex * 16 + 10);
                    if (bone < 0 || bone >= segment_count ||
                        !selected_bones[bone])
                        continue;
                    ++selected;
                    const float* xyz = reinterpret_cast<const float*>(
                        output + vertex * 24);
                    if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) ||
                        !std::isfinite(xyz[2]))
                        continue;
                    const float* camera_xyz = xyz;
                    if (!std::isfinite(camera_xyz[0]) ||
                        !std::isfinite(camera_xyz[1]) ||
                        !std::isfinite(camera_xyz[2]))
                        continue;
                    ++finite;
                    if (camera_xyz[2] <= 0.0f)
                        ++behind;
                    if (camera_xyz[2] < 4.0f)
                        ++near_vertices;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        camera_min[axis] = fminf(camera_min[axis],
                                                camera_xyz[axis]);
                        camera_max[axis] = fmaxf(camera_max[axis],
                                                camera_xyz[axis]);
                    }
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                log("render: hand CPU vertex sample %u %s buffer read fault",
                    mark, hand == 0 ? "left" : "right");
                return;
            }
            if (!finite)
            {
                log("render: hand CPU vertex sample %u %s selected=%u "
                    "finite=0", mark, hand == 0 ? "left" : "right",
                    selected);
                return;
            }

            float row_len[3]{};
            float max_row_dot = 0.0f;
            for (int row = 0; row < 3; ++row)
            {
                float length_sq = 0.0f;
                for (int axis = 0; axis < 3; ++axis)
                    length_sq += original_wrist.m[row][axis] *
                                 original_wrist.m[row][axis];
                row_len[row] = sqrtf(length_sq);
                for (int other = row + 1; other < 3; ++other)
                {
                    float dot = 0.0f;
                    for (int axis = 0; axis < 3; ++axis)
                        dot += original_wrist.m[row][axis] *
                               original_wrist.m[other][axis];
                    max_row_dot = fmaxf(max_row_dot, fabsf(dot));
                }
            }
            const float* a = original_wrist.m[0];
            const float* b = original_wrist.m[1];
            const float* c = original_wrist.m[2];
            const float determinant =
                a[0] * (b[1] * c[2] - b[2] * c[1]) -
                a[1] * (b[0] * c[2] - b[2] * c[0]) +
                a[2] * (b[0] * c[1] - b[1] * c[0]);
            const float scale = tune_world_scale();
            const float unit = std::isfinite(scale) && scale > 0.0f
                             ? scale : 1.0f;
            log("render: hand CPU vertices mark=%u %s ledge=%d "
                "selected=%u finite=%u behind=%u near4=%u "
                "view m X=(%+.2f..%+.2f) Y=(%+.2f..%+.2f) "
                "Z=(%+.2f..%+.2f) "
                "wrist rows=(%.3f,%.3f,%.3f) dot=%.3f det=%.3f",
                mark, hand == 0 ? "left" : "right",
                g_first_person_ledge_hanging ? 1 : 0,
                selected, finite, behind, near_vertices,
                camera_min[0] / unit, camera_max[0] / unit,
                camera_min[1] / unit, camera_max[1] / unit,
                camera_min[2] / unit, camera_max[2] / unit,
                row_len[0], row_len[1], row_len[2], max_row_dot,
                determinant);
        }

        int retail_combat_state();

        // Player weapon objects (trl.exe preload table at 0x00F15548):
        // handgun_rbweapon, handgun_rbweapon_upa/upb/upc, handgun_revolver,
        // shotgun_, autorifle_, rpg_, grenadelauncher_, smgmpfive_,
        // excalibur_, mreaver_rbweapon.
        bool weapon_is_pistol(const char* name)
        {
            return name && strncmp(name, "handgun", 7) == 0;
        }

        bool weapon_is_long_gun(const char* name)
        {
            if (!name || weapon_is_pistol(name))
                return false;
            const size_t length = strlen(name);
            return length > 9 &&
                   strcmp(name + length - 9, "_rbweapon") == 0;
        }

        // A weapon is "in hand" when the immersive holster gesture drew it,
        // or -- without immersive controls, where the X button / retail keys
        // draw -- whenever retail combat is active.
        bool weapon_drawn_in_hand(int hand)
        {
            if (config().immersive_controls)
                return vr_input_holster_hand_drawn(hand == 0);
            return retail_combat_state() != 0;
        }

        // Lara's long gun is out (moved onto a controller this frame). The
        // holster state still reads "drawn" then, but the pistols stay at
        // her hips (headset test: they floated at a hip-to-wrist offset
        // from the controllers while the MP5 / rifle was held).
        bool laras_long_gun_held()
        {
            return g_first_person_active && g_gameplay_gun.instance &&
                   GetTickCount() - g_gameplay_gun.tick <= 250;
        }

        bool first_person_draw_holstered_gear(
            void* instance, int a2, float a3, int a4, int a5,
            void* model, float a7, void* colour, void* transform,
            unsigned char* object, const char* name, int link_marker)
        {
            int gear = -1;
            const bool long_gun_out = laras_long_gun_held();
            if (link_marker == 4 &&
                weapon_is_pistol(name) &&
                config().immersive_controls &&
                (!vr_input_holster_hand_drawn(true) || long_gun_out))
                gear = 0;
            else if (link_marker == 6 &&
                     weapon_is_pistol(name) &&
                     config().immersive_controls &&
                     (!vr_input_holster_hand_drawn(false) || long_gun_out))
                gear = 1;
            else if ((link_marker == 9 ||
                      g_first_person_gear_held[1]) &&
                     strcmp(name, "magnetic_grapple") == 0)
                gear = 2;
            else if ((link_marker == 16 ||
                      g_first_person_gear_held[0]) &&
                     strcmp(name, "g_binoculars") == 0)
                gear = 3;
            else if (link_marker == 17 &&
                     strcmp(name, "g_personal_light") == 0)
                gear = 4;
            if (gear < 0 || !g_first_person_turn_ready ||
                !g_first_person_actor_offset_valid ||
                g_first_person_actor_offset_model != g_first_person_model ||
                !g_first_person_anchor_valid || transform)
                return false;

            unsigned char* child = static_cast<unsigned char*>(instance);
            float* child_matrices = nullptr;
            int child_count = 0;
            float root[3]{};
            float actor_yaw = 0.0f;
            Mat4 eye = Mat4::identity();
            __try
            {
                void* draw_model = model;
                if (!draw_model)
                {
                    void** models = *reinterpret_cast<void***>(
                        object + kOffObjectModelList);
                    const int nmodels = *reinterpret_cast<short*>(
                        object + kOffObjectNumModels);
                    int index = *reinterpret_cast<signed char*>(
                        child + kOffCurrentRenderModel);
                    if (index < 0 || index >= nmodels)
                        index = *reinterpret_cast<signed char*>(
                            child + kOffCurrentBaseModel);
                    if (!models || nmodels <= 0 || nmodels > 32 ||
                        index < 0 || index >= nmodels)
                        return false;
                    draw_model = models[index];
                }
                if (!draw_model)
                    return false;
                child_count = *reinterpret_cast<int*>(
                    static_cast<unsigned char*>(draw_model) +
                    kOffModelNumSegments);
                child_matrices = *reinterpret_cast<float**>(
                    child + kOffInstanceMatrices);
                if (!child_matrices || child_count <= 0 || child_count > 16)
                    return false;
                const unsigned char* lara =
                    static_cast<const unsigned char*>(g_first_person_instance);
                for (int axis = 0; axis < 3; ++axis)
                    root[axis] = *reinterpret_cast<const float*>(
                        lara + 0x10 + axis * sizeof(float));
                actor_yaw = *reinterpret_cast<const float*>(
                    lara + kOffInstanceHeading);
                memcpy(&eye.m[0][0],
                       static_cast<const unsigned char*>(kMainCamera) +
                           kOffCwTransform,
                       sizeof(eye.m));
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }

            const float stick_turn = vr_input_first_person_turn() -
                                     g_first_person_stick_start;
            const float player_yaw = wrap_pi(
                g_first_person_base_heading +
                g_first_person_turn_sign * stick_turn -
                g_first_person_turn_sign * first_person_hmd_heading() +
                g_first_person_actor_camera_offset);
            const float yaw_delta = wrap_pi(player_yaw - actor_yaw);
            const float belt_height = tune_first_person_belt_height();
            float eye_delta[3]{};
            float distance_sq = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                eye_delta[axis] = eye.m[3][axis] -
                                  g_first_person_last_eye[axis];
                distance_sq += eye_delta[axis] * eye_delta[axis];
            }
            if (!std::isfinite(yaw_delta) ||
                !std::isfinite(distance_sq) || distance_sq > 1000.0f*1000.0f)
                return false;
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(root[axis]))
                    return false;
            const int held_item = gear == 3 ? 0 : gear == 2 ? 1 : -1;

            // Gear tuning: the item follows a raw controller until its
            // trigger places it; afterwards (and in later sessions) it sits
            // at the saved body-frame point, which is also its grab zone.
            // Only the item's position changes; it keeps its belt rotation.
            float target[3]{};
            bool placed = false;
            const int follow = tune_gear_tuning_hand(gear);
            if (follow >= 0)
            {
                Mat4 controller_to_head;
                if (vr_controller_head_pose(follow == 0,
                                            &controller_to_head))
                {
                    const Mat4 world = controller_to_head * eye;
                    for (int axis = 0; axis < 3; ++axis)
                        target[axis] = world.m[3][axis];
                    placed = true;
                }
            }
            else
            {
                float body[3]{};
                float head[3]{};
                if (tune_gear_position(gear, body) &&
                    vr_body_point_head_position(body[0], body[1], body[2],
                                                head))
                {
                    const Mat4 world =
                        translation(head[0], head[1], head[2]) * eye;
                    for (int axis = 0; axis < 3; ++axis)
                        target[axis] = world.m[3][axis];
                    placed = true;
                }
            }
            for (int axis = 0; axis < 3; ++axis)
                placed = placed && std::isfinite(target[axis]);

            if (held_item < 0 && !placed &&
                fabsf(yaw_delta) < 0.002f && distance_sq < 0.25f &&
                fabsf(belt_height) < 0.01f)
                return false;

            // Legend's world is XY horizontal and Z up. Its heading vector
            // is (sin(yaw), -cos(yaw), 0), so this row-vector Z rotation has
            // the same positive direction as Instance+0x38.
            Mat4 rotation = Mat4::identity();
            const float c = cosf(yaw_delta), s = sinf(yaw_delta);
            rotation.m[0][0] = c;
            rotation.m[0][1] = s;
            rotation.m[1][0] = -s;
            rotation.m[1][1] = c;
            Mat4 belt_delta =
                translation(-root[0], -root[1], -root[2]) *
                rotation *
                translation(root[0] + eye_delta[0],
                            root[1] + eye_delta[1],
                            root[2] + eye_delta[2] + belt_height);
            if (placed)
            {
                const Mat4 belt_pose =
                    matrix_from_floats(child_matrices) * belt_delta;
                belt_delta = belt_delta *
                    translation(target[0] - belt_pose.m[3][0],
                                target[1] - belt_pose.m[3][1],
                                target[2] - belt_pose.m[3][2]);
            }

            Mat4 draw_delta = belt_delta;
            if (held_item >= 0)
            {
                const Mat4 source_pose =
                    matrix_from_floats(child_matrices) * belt_delta;
                Mat4 player_rotation = Mat4::identity();
                const float pc = cosf(player_yaw), ps = sinf(player_yaw);
                player_rotation.m[0][0] = pc;
                player_rotation.m[0][1] = ps;
                player_rotation.m[1][0] = -ps;
                player_rotation.m[1][1] = pc;
                const Mat4 player_transform = player_rotation *
                    translation(root[0] + eye_delta[0],
                                root[1] + eye_delta[1],
                                root[2] + eye_delta[2] + belt_height);
                const bool at_belt_marker =
                    link_marker == (held_item == 0 ? 16 : 9);
                if (at_belt_marker)
                {
                    g_first_person_gear_belt_local[held_item] =
                        source_pose * rigid_inverse(player_transform);
                    g_first_person_gear_belt_local_valid[held_item] = true;
                }
                if (g_first_person_gear_belt_local_valid[held_item])
                {
                    g_first_person_gear_pose[held_item] =
                        g_first_person_gear_belt_local[held_item] *
                        player_transform;
                    g_first_person_gear_seen[held_item] = GetTickCount();
                    g_first_person_gear_valid[held_item] = true;
                }
                if (g_first_person_gear_held[held_item])
                {
                    Mat4 controller;
                    const bool left = g_first_person_gear_hand[held_item] == 0;
                    if (!first_person_controller_world_pose(left, &controller))
                        return true; // Do not flash the item back to the belt.
                    const Mat4 hand_pose =
                        g_first_person_gear_grip_rotation[held_item] *
                        controller;
                    draw_delta = belt_delta * rigid_inverse(source_pose) *
                                 hand_pose;
                }
            }

            Mat4 saved[16];
            for (int segment = 0; segment < child_count; ++segment)
            {
                saved[segment] = matrix_from_floats(
                    child_matrices + segment * 16);
                const Mat4 moved = saved[segment] * draw_delta;
                memcpy(child_matrices + segment * 16,
                       &moved.m[0][0], sizeof(moved.m));
            }
            g_draw_instance(instance, a2, a3, a4, a5, model, a7,
                            colour, transform);
            for (int segment = 0; segment < child_count; ++segment)
                memcpy(child_matrices + segment * 16,
                       &saved[segment].m[0][0], sizeof(saved[segment].m));

            static bool reported[5]{};
            if (!reported[gear])
            {
                reported[gear] = true;
                log("first-person: holstered %s marker %d follows player "
                    "hip (yaw correction %.1f deg, HMD offset %.1f units)",
                    name, link_marker, yaw_delta * 57.2957795f,
                    sqrtf(distance_sq));
            }
            return true;
        }

        // Segment count of an instance's model (the one it renders, else its
        // base model), or 0 when it cannot be read.
        int instance_segment_count(unsigned char* inst, void* model)
        {
            if (!model)
            {
                unsigned char* object = *reinterpret_cast<unsigned char**>(
                    inst + kOffInstanceObject);
                if (!object)
                    return 0;
                void** models = *reinterpret_cast<void***>(
                    object + kOffObjectModelList);
                const int nmodels = *reinterpret_cast<short*>(
                    object + kOffObjectNumModels);
                int index = *reinterpret_cast<signed char*>(
                    inst + kOffCurrentRenderModel);
                if (index < 0 || index >= nmodels)
                    index = *reinterpret_cast<signed char*>(
                        inst + kOffCurrentBaseModel);
                if (!models || index < 0 || index >= nmodels)
                    return 0;
                model = models[index];
            }
            if (!model)
                return 0;
            const int count = *reinterpret_cast<int*>(
                static_cast<unsigned char*>(model) + kOffModelNumSegments);
            return count > 0 && count <= 512 ? count : 0;
        }

        // Transform of Lara's held long gun, retail pose -> controller:
        // moved = original * grip (right-handed) or
        // mirror * original * grip (left-handed; see the draw path).
        bool long_gun_grip(const Mat4& wrist, Mat4* grip, bool* mirrored)
        {
            const bool left = config().left_handed;
            Mat4 controller;
            if (!first_person_controller_world_pose(left, &controller))
                return false;
            *mirrored = left;
            if (left)
            {
                Mat4 mirror = Mat4::identity();
                mirror.m[0][0] = -1.0f;
                *grip = first_person_hand_delta(1, wrist, Mat4::identity()) *
                        mirror * controller;
            }
            else
                *grip = first_person_hand_delta(1, wrist, controller);
            return true;
        }

        // The long gun is the selected weapon with no swap pending
        // (PlayerData+0x3E1 selected slot == +0x3E5 requested slot, see
        // vr_input select_weapon_kind). During a swap retail animates the
        // gun to or from Lara's back while it is still near her wrist;
        // carrying that with the controller swung it through the view
        // (headset test), so it is hidden until the swap settles.
        bool long_gun_selection_settled()
        {
            __try
            {
                const unsigned char* pd =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!pd)
                    return false;
                const int selected = static_cast<signed char>(pd[0x3E1]);
                const int requested = static_cast<signed char>(pd[0x3E5]);
                if (selected < 0 || selected > 10 || requested != selected)
                    return false;
                const unsigned char* inst =
                    *reinterpret_cast<unsigned char* const*>(
                        pd + 0x3E8 + selected * 8);
                const unsigned char* object = inst
                    ? *reinterpret_cast<unsigned char* const*>(
                          inst + kOffInstanceObject)
                    : nullptr;
                const char* name = object
                    ? *reinterpret_cast<const char* const*>(
                          object + kOffObjectName)
                    : nullptr;
                return weapon_is_long_gun(name);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // True when `inst` is Lara's drawn long gun: a long-gun object held
        // within 0.5 m of her animated right wrist (enemies carry the same
        // objects, metres away).
        bool is_laras_held_long_gun(const float* own,
                                    const char* name)
        {
            if (!weapon_is_long_gun(name) || !weapon_drawn_in_hand(1) ||
                !long_gun_selection_settled())
                return false;
            const float* lara_matrices = *reinterpret_cast<float* const*>(
                static_cast<unsigned char*>(g_first_person_instance) +
                kOffInstanceMatrices);
            const int w = g_first_person_wrist_segment[1];
            if (!lara_matrices || !own || w < 0)
                return false;
            float d = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float e = own[12 + k] - lara_matrices[w * 16 + 12 + k];
                d += e * e;
            }
            const float scale = tune_world_scale();
            return std::isfinite(d) && scale > 0.0f &&
                   sqrtf(d) <= 0.5f * scale;
        }

        bool gameplay_gun_match(void* instance, const float* matrices,
                                float shift[3]);

        int pistol_side(void* weapon);
        bool laras_long_gun_held();

        void move_gameplay_pistol(void* instance, unsigned char* inst,
                                  const char* name)
        {
            if (*reinterpret_cast<void**>(inst + 0xB8) !=
                g_first_person_instance)
                return;
            const int side = pistol_side(instance);
            if (side < 0 || g_first_person_wrist_segment[side] < 0)
                return;
            float* matrices = *reinterpret_cast<float**>(
                inst + kOffInstanceMatrices);
            if (!matrices)
                return;
            GameplayGun& gun = g_gameplay_pistol[side];
            float unused_shift[3];
            if (gameplay_gun_match(instance, matrices, unused_shift))
            {
                gun.tick = GetTickCount();
                return;
            }
            const short marker = *reinterpret_cast<short*>(inst + 0xBE);
            if (marker == 4 || marker == 6)
                return; // on its hip: the holster draw owns it
            const bool drawn = vr_input_holster_hand_drawn(side == 0) &&
                               !laras_long_gun_held();
            Mat4 root;
            if (drawn)
            {
                Mat4 controller;
                if (!first_person_controller_world_pose(side == 0,
                                                        &controller))
                    return;
                const float* lara_matrices = *reinterpret_cast<float* const*>(
                    static_cast<unsigned char*>(g_first_person_instance) +
                    kOffInstanceMatrices);
                const Mat4 wrist = matrix_from_floats(
                    lara_matrices + g_first_person_wrist_segment[side] * 16);
                root = first_person_hand_delta(side, wrist, controller);
            }
            else if (config().single_pistols && config().immersive_controls)
                root = translation(0.0f, 0.0f, -100000.0f); // out of sight
            else
                return;
            const int count = instance_segment_count(inst, nullptr);
            if (count <= 0 || count > 8)
                return;
            for (int segment = 0; segment < count; ++segment)
            {
                memcpy(gun.original[segment], matrices + segment * 16,
                       sizeof(gun.original[segment]));
                const Mat4 moved =
                    matrix_from_floats(gun.original[segment]) * root;
                memcpy(matrices + segment * 16, &moved.m[0][0],
                       sizeof(moved.m));
            }
            memcpy(gun.moved0, matrices, sizeof(gun.moved0));
            gun.instance = instance;
            gun.matrices = matrices;
            gun.count = count;
            gun.root = root;
            gun.tick = GetTickCount();
            strncpy_s(gun.name, name, _TRUNCATE);
            static unsigned reports[2] = {};
            if (reports[drawn ? 0 : 1]++ < 2)
                log("first-person: %s pistol's gameplay copy %s", side == 0
                    ? "left" : "right", drawn
                    ? "moved onto its controller (flash follows)"
                    : "sunk out of sight (holstered in single mode)");
        }

        void __cdecl detour_build_transforms(void* instance)
        {
            g_build_transforms(instance);
            if (!g_first_person_active || !g_first_person_instance ||
                !instance || instance == g_first_person_instance ||
                g_first_person_wrist_segment[1] < 0)
                return;
            __try
            {
                unsigned char* inst = static_cast<unsigned char*>(instance);
                unsigned char* object = *reinterpret_cast<unsigned char**>(
                    inst + kOffInstanceObject);
                const char* name = object ? *reinterpret_cast<const char**>(
                    object + kOffObjectName) : nullptr;
                if (!name)
                    return;
                if (weapon_is_pistol(name))
                {
                    move_gameplay_pistol(instance, inst, name);
                    return;
                }
                if (!weapon_is_long_gun(name))
                    return;
                float* matrices = *reinterpret_cast<float**>(
                    inst + kOffInstanceMatrices);
                if (!matrices)
                    return;
                // Retail skipped the rebuild: the buffer still holds our
                // moved pose (possibly slid by Lara's movement); moving it
                // again would compound.
                float unused_shift[3];
                if (gameplay_gun_match(instance, matrices, unused_shift))
                {
                    g_gameplay_gun.tick = GetTickCount();
                    return;
                }
                if (!is_laras_held_long_gun(matrices, name))
                    return;
                const int count = instance_segment_count(inst, nullptr);
                if (count <= 0 || count > 8)
                    return;
                const float* lara_matrices = *reinterpret_cast<float* const*>(
                    static_cast<unsigned char*>(g_first_person_instance) +
                    kOffInstanceMatrices);
                const Mat4 wrist = matrix_from_floats(
                    lara_matrices + g_first_person_wrist_segment[1] * 16);
                Mat4 grip;
                bool mirrored = false;
                if (!long_gun_grip(wrist, &grip, &mirrored))
                    return;
                Mat4 mirror = Mat4::identity();
                mirror.m[0][0] = -1.0f;
                GameplayGun& gun = g_gameplay_gun;
                for (int segment = 0; segment < count; ++segment)
                {
                    memcpy(gun.original[segment], matrices + segment * 16,
                           sizeof(gun.original[segment]));
                    const Mat4 original = matrix_from_floats(
                        gun.original[segment]);
                    const Mat4 moved = mirrored
                        ? mirror * original * grip
                        : original * grip;
                    memcpy(matrices + segment * 16, &moved.m[0][0],
                           sizeof(moved.m));
                }
                memcpy(gun.moved0, matrices, sizeof(gun.moved0));
                gun.instance = instance;
                gun.matrices = matrices;
                gun.count = count;
                gun.root = grip;
                gun.tick = GetTickCount();
                strncpy_s(gun.name, name, _TRUNCATE);
                static unsigned reports = 0;
                if (reports++ < 3)
                    log("first-person: %s moved onto the %s controller in "
                        "the game's own transforms (%d segments); flash, "
                        "tracers and bullets follow it", name,
                        config().left_handed ? "left" : "right", count);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                g_gameplay_gun.instance = nullptr;
            }
        }

        // True when the gameplay copy still carries our move. While Lara
        // walks, retail slides an attached instance's matrices by her step
        // after they were built (headset test: the MP5 left the hand only
        // while walking, because an exact comparison failed and the draw
        // moved it a second time). A pure translation keeps the rotation
        // rows bit-identical; `shift` returns that translation, which the
        // retail pose would have received too.
        bool gameplay_gun_match(void* instance, const float* matrices,
                                float shift[3])
        {
            const GameplayGun* found = gameplay_gun_for(instance);
            if (!found)
                return false;
            const GameplayGun& gun = *found;
            if (gun.matrices != matrices || gun.count <= 0 ||
                GetTickCount() - gun.tick > 250 ||
                memcmp(gun.moved0, matrices, sizeof(float) * 12) != 0)
                return false;
            for (int k = 0; k < 3; ++k)
            {
                shift[k] = matrices[12 + k] - gun.moved0[12 + k];
                if (!std::isfinite(shift[k]))
                    return false;
            }
            return true;
        }

        // Retail pose of one gun segment, including the shift above.
        Mat4 gameplay_gun_original(void* instance, int segment,
                                   const float shift[3])
        {
            const GameplayGun* gun = gameplay_gun_for(instance);
            if (!gun || segment < 0 || segment >= gun->count)
                return Mat4::identity();
            Mat4 original = matrix_from_floats(gun->original[segment]);
            for (int k = 0; k < 3; ++k)
                original.m[3][k] += shift[k];
            return original;
        }

        bool first_person_draw_linked_weapon(void* instance, int a2,
                                              float a3, int a4, int a5,
                                              void* model, float a7,
                                              void* colour, void* transform)
        {
            unsigned char* child = static_cast<unsigned char*>(instance);
            unsigned char* object = nullptr;
            const char* name = nullptr;
            int link_bone = -1;
            int link_marker = -1;
            bool linked_to_lara = false;
            __try
            {
                object = *reinterpret_cast<unsigned char**>(
                    child + kOffInstanceObject);
                name = object ? *reinterpret_cast<const char**>(
                    object + kOffObjectName) : nullptr;
                linked_to_lara =
                    *reinterpret_cast<void**>(child + 0xB8) ==
                    g_first_person_instance;
                if (linked_to_lara)
                {
                    link_bone = *reinterpret_cast<short*>(child + 0xBC);
                    link_marker = *reinterpret_cast<short*>(child + 0xBE);
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            // Linked props include binoculars, the light, grapple and both
            // holstered pistols. Only a pistol explicitly drawn from its
            // holster belongs at the controller pose.
            if (!name)
                return false;
            // Learn each pistol's side while it sits on its hip marker.
            if (linked_to_lara && weapon_is_pistol(name))
                pistol_side(instance);
            if ((link_bone < 0 || g_first_person_gear_held[0] ||
                 g_first_person_gear_held[1]) &&
                first_person_draw_holstered_gear(
                    instance, a2, a3, a4, a5, model, a7, colour,
                    transform, object, name, link_marker))
                return true;
            // Evidence for how each weapon is attached, once per name and
            // drawn state: parent (Lara or not), link bone/marker, and its
            // distance from each animated wrist.
            if (weapon_is_pistol(name) || weapon_is_long_gun(name))
            {
                static char seen_names[24][40]{};
                static int seen_drawn[24]{};
                static unsigned seen_count = 0;
                const int drawn = weapon_drawn_in_hand(1) ? 1 : 0;
                bool seen = false;
                for (unsigned i = 0; i < seen_count; ++i)
                    seen = seen || (strcmp(seen_names[i], name) == 0 &&
                                    seen_drawn[i] == drawn);
                if (!seen && seen_count < 24)
                {
                    strncpy_s(seen_names[seen_count], name, _TRUNCATE);
                    seen_drawn[seen_count++] = drawn;
                    float wrist_m[2] = { -1.0f, -1.0f };
                    __try
                    {
                        const float* lara_matrices =
                            *reinterpret_cast<float* const*>(
                                static_cast<unsigned char*>(
                                    g_first_person_instance) +
                                kOffInstanceMatrices);
                        const float* own = *reinterpret_cast<float* const*>(
                            child + kOffInstanceMatrices);
                        const float scale = tune_world_scale();
                        for (int h = 0; h < 2 && lara_matrices && own &&
                                        scale > 0.0f; ++h)
                        {
                            const int w = g_first_person_wrist_segment[h];
                            if (w < 0)
                                continue;
                            float d = 0.0f;
                            for (int k = 0; k < 3; ++k)
                            {
                                const float e = own[12 + k] -
                                                lara_matrices[w * 16 + 12 + k];
                                d += e * e;
                            }
                            wrist_m[h] = sqrtf(d) / scale;
                        }
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                    log("first-person: weapon %s %s: parent %s, link bone "
                        "%d, marker %d; wrist distance left %.2f m, right "
                        "%.2f m", name, drawn ? "drawn" : "not drawn",
                        linked_to_lara ? "Lara" : "other/none", link_bone,
                        link_marker, wrist_m[0], wrist_m[1]);
                }
            }

            // Two-handed weapons (MP5, assault rifle, shotgun, ...) are held
            // by the right hand in retail. While drawn, carry them with the
            // right hand's wrist-to-controller delta, exactly like a pistol,
            // whatever their link. The left hand stays free for now.
            int hand = -1;
            bool long_gun = false;
            if (weapon_is_long_gun(name))
            {
                if (!weapon_drawn_in_hand(1))
                {
                    if (linked_to_lara)
                        return true; // holstered on Lara's hidden body
                    return false;
                }
                // Enemies carry the same objects (smgmpfive_, autorifle_...)
                // and Lara's long gun is not linked to her either (log
                // 2026-09-30: parent none, bone/marker -1), so moving every
                // instance by name dragged NPC guns onto the controller.
                // Retail holds Lara's at her animated right hand; an enemy's
                // is metres away. Only a gun within 0.5 m of that wrist is
                // hers. A gun the transform hook already moved is measured
                // at its saved retail pose.
                float wrist_distance = 1.0e9f;
                __try
                {
                    const float* own_now = *reinterpret_cast<float* const*>(
                        child + kOffInstanceMatrices);
                    const float* lara_matrices =
                        *reinterpret_cast<float* const*>(
                            static_cast<unsigned char*>(
                                g_first_person_instance) +
                            kOffInstanceMatrices);
                    float shift[3];
                    float retail0[16];
                    const float* own = own_now;
                    if (gameplay_gun_match(instance, own_now, shift))
                    {
                        const Mat4 r = gameplay_gun_original(instance, 0,
                                                             shift);
                        memcpy(retail0, &r.m[0][0], sizeof(retail0));
                        own = retail0;
                    }
                    const int w = g_first_person_wrist_segment[1];
                    if (lara_matrices && own && w >= 0)
                    {
                        float d = 0.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            const float e = own[12 + k] -
                                            lara_matrices[w * 16 + 12 + k];
                            d += e * e;
                        }
                        wrist_distance = sqrtf(d);
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
                const float scale = tune_world_scale();
                if (!std::isfinite(wrist_distance) || scale <= 0.0f ||
                    wrist_distance > 0.5f * scale)
                    return false; // someone else's gun: draw it untouched
                if (!long_gun_selection_settled())
                {
                    static unsigned swap_reports = 0;
                    if (swap_reports++ < 6)
                        log("first-person: %s hidden while a weapon swap is "
                            "in progress", name);
                    return true;
                }
                static unsigned own_reports = 0;
                if (own_reports++ < 8)
                    log("first-person: Lara's %s is %.2f m from her right "
                        "wrist; carried by the %s controller", name,
                        wrist_distance / scale,
                        config().left_handed ? "left" : "right");
                hand = 1;
                long_gun = true;
            }
            else if (!weapon_is_pistol(name))
            {
                // The parent Lara draw is reduced to two isolated hands.
                // Unhandled children of her skeleton (hair and similar
                // accessories) would still be rendered by the ordinary path
                // directly against the HMD, producing large close polygons.
                // Keep the known belt items and grapple on their own paths.
                if (linked_to_lara &&
                    strcmp(name, "g_binoculars") != 0 &&
                    strcmp(name, "g_personal_light") != 0 &&
                    strcmp(name, "magnetic_grapple") != 0)
                {
                    static unsigned suppressed_reports = 0;
                    if (suppressed_reports++ < 12)
                        log("first-person: suppressed Lara-linked %s "
                            "(bone %d, marker %d)",
                            name, link_bone, link_marker);
                    return true;
                }
                return false;
            }
            if (hand < 0 && link_bone < 0 && link_marker < 0)
                return false;

            unsigned char* parent_model =
                static_cast<unsigned char*>(g_first_person_model);
            const int parent_count = *reinterpret_cast<int*>(
                parent_model + kOffModelNumSegments);
            ModelSegment* parent_segments =
                *reinterpret_cast<ModelSegment**>(
                    parent_model + kOffModelSegments);
            if (parent_count <= 0 || parent_count > 512 ||
                !parent_segments)
                return false;
            float* parent_matrices = *reinterpret_cast<float**>(
                static_cast<unsigned char*>(g_first_person_instance) +
                kOffInstanceMatrices);
            if (!parent_matrices)
                return false;
            if (hand >= 0)
            {
                // Long gun: hand already chosen.
            }
            else if (link_bone >= 0)
            {
                for (int candidate = 0; candidate < 2; ++candidate)
                    if (link_bone < parent_count &&
                        segment_is_in_head_subtree(parent_segments,
                            parent_count, link_bone,
                            g_first_person_wrist_segment[candidate]))
                        hand = candidate;
            }
            else if (weapon_is_pistol(name) && pistol_side(instance) >= 0)
            {
                // Marker 4 (left) / 6 (right) on the hip; once drawn the
                // pistol's side comes from its remembered identity.
                hand = pistol_side(instance);
            }
            else if (link_marker >= 0)
            {
                // MiniInstance and full Instance links can use a model marker
                // instead of a segment. Retail's marker lookup resolves its
                // world position; accept only markers close to a wrist so a
                // holster or belt item cannot jump onto a controller.
                alignas(16) Mat4 marker = Mat4::identity();
                bool found = false;
                __try
                {
                    found = reinterpret_cast<PFN_GetHMarkerTransform>(
                        kGetHMarkerTransform)(
                            g_first_person_instance, link_marker,
                            &marker, false) != 0;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    found = false;
                }
                float nearest_sq = 175.0f * 175.0f;
                if (found)
                    for (int candidate = 0; candidate < 2; ++candidate)
                    {
                        const int wrist_index =
                            g_first_person_wrist_segment[candidate];
                        if (wrist_index < 0 || wrist_index >= parent_count)
                            continue;
                        const Mat4 wrist = matrix_from_floats(
                            parent_matrices + wrist_index * 16);
                        const float dx = marker.m[3][0] - wrist.m[3][0];
                        const float dy = marker.m[3][1] - wrist.m[3][1];
                        const float dz = marker.m[3][2] - wrist.m[3][2];
                        const float distance_sq = dx*dx + dy*dy + dz*dz;
                        if (std::isfinite(distance_sq) &&
                            distance_sq < nearest_sq)
                        {
                            nearest_sq = distance_sq;
                            hand = candidate;
                        }
                    }
            }
            if (hand < 0)
            {
                static unsigned unmatched = 0;
                if (unmatched++ < 6)
                    log("first-person: Lara-linked draw at segment %d "
                        "marker %d is outside wrist subtrees",
                        link_bone, link_marker);
                return false;
            }
            if (!weapon_drawn_in_hand(hand))
            {
                // Single-pistol mode: retail still puts the undrawn pistol in
                // Lara's hidden other hand (off its hip marker 4 / 6), where
                // it floated in view. The hip-holster draw would keep that
                // in-hand rotation, so hide it until it is back on the hip.
                if (!long_gun && weapon_is_pistol(name) &&
                    link_marker != 4 && link_marker != 6)
                    return true;
                return false;
            }
            // Pistols while a long gun is held: the holster path above
            // draws them at the hips; if it could not, hide them rather
            // than carry them on a controller.
            if (!long_gun && laras_long_gun_held())
                return true;

            // A left-handed player's long gun keeps retail's right-wrist
            // grip (and the right hand's calibration) but follows the left
            // controller. It is not mirrored: that would invert the mesh.
            const bool left_controller =
                long_gun && config().left_handed ? true : hand == 0;
            Mat4 controller;
            if (!first_person_controller_world_pose(left_controller,
                                                    &controller))
                return true; // Do not expose a gun at the old hand position.
            const Mat4 wrist = matrix_from_floats(
                parent_matrices + g_first_person_wrist_segment[hand] * 16);
            const Mat4 delta = first_person_hand_delta(hand, wrist,
                                                        controller);
            // Left-handed long gun. Reusing the right hand's calibration on
            // the left controller stood the gun on end (headset test): the
            // two controllers' frames are mirror images. The right-hand grip
            // in controller space is  G_rel = M * inv(W_R) * K_R  (K_R the
            // right calibration * alignment rotation), i.e. the right delta
            // for an identity controller. Mirror it across the controller's
            // left/right axis, S = diag(-1, 1, 1):  M' = S * G_rel * S * C_L.
            // The two S keep the determinant +1, so the mesh is not turned
            // inside out -- the gun is held in the mirrored pose.
            const bool mirror_grip = long_gun && config().left_handed;
            Mat4 mirror = Mat4::identity();
            mirror.m[0][0] = -1.0f;
            const Mat4 right_grip = mirror_grip
                ? first_person_hand_delta(hand, wrist, Mat4::identity())
                : Mat4::identity();

            void* draw_model = model;
            if (!draw_model)
            {
                void** models = *reinterpret_cast<void***>(
                    object + kOffObjectModelList);
                const int nmodels = *reinterpret_cast<short*>(
                    object + kOffObjectNumModels);
                int index = *reinterpret_cast<signed char*>(
                    child + kOffCurrentRenderModel);
                if (index < 0 || index >= nmodels)
                    index = *reinterpret_cast<signed char*>(
                        child + kOffCurrentBaseModel);
                if (!models || index < 0 || index >= nmodels)
                    return false;
                draw_model = models[index];
            }
            const int child_count = *reinterpret_cast<int*>(
                static_cast<unsigned char*>(draw_model) +
                kOffModelNumSegments);
            float* child_matrices = *reinterpret_cast<float**>(
                child + kOffInstanceMatrices);
            if (!child_matrices || child_count <= 0 || child_count > 512)
                return false;

            ++g_pistol_draws[hand];
            for (int axis = 0; axis < 3; ++axis)
                g_pistol_instance_pos[hand][axis] = *reinterpret_cast<float*>(
                    child + 0x10 + axis * sizeof(float));

            // Gameplay copy already moved by detour_build_transforms: redo
            // the move from its retail pose with this frame's controller.
            float gameplay_shift[3] = {};
            const bool from_gameplay =
                gameplay_gun_match(instance, child_matrices, gameplay_shift);
            const GameplayGun* gameplay_copy = gameplay_gun_for(instance);
            if (long_gun)
            {
                static unsigned slide_reports = 0, miss_reports = 0;
                const float slide = sqrtf(dot3(gameplay_shift,
                                               gameplay_shift));
                if (from_gameplay && slide > 0.01f && slide_reports < 6)
                    log("first-person: held gun slid %.2f units after its "
                        "transform build; re-derived from the retail pose",
                        slide), ++slide_reports;
                else if (!from_gameplay && instance == g_gameplay_gun.instance
                         && miss_reports < 6)
                    log("first-person: held gun draw did not match the "
                        "gameplay move (buffer %s, age %u ms)",
                        child_matrices == g_gameplay_gun.matrices
                            ? "same" : "different",
                        GetTickCount() - g_gameplay_gun.tick),
                        ++miss_reports;
            }
            float saved[512][16]{};
            for (int segment = 0; segment < child_count; ++segment)
            {
                memcpy(saved[segment], child_matrices + segment * 16,
                       sizeof(saved[segment]));
                const Mat4 original =
                    from_gameplay && gameplay_copy &&
                    segment < gameplay_copy->count
                        ? gameplay_gun_original(instance, segment,
                                                gameplay_shift)
                        : matrix_from_floats(child_matrices + segment * 16);
                const Mat4 moved = mirror_grip
                    ? mirror * original * right_grip * mirror * controller
                    : original * delta;
                memcpy(child_matrices + segment * 16, &moved.m[0][0],
                       sizeof(moved.m));
            }
            g_draw_instance(instance, a2, a3, a4, a5, model, a7,
                            colour, transform);

            for (int segment = 0; segment < child_count; ++segment)
                memcpy(child_matrices + segment * 16, saved[segment],
                       sizeof(saved[segment]));

            static void* reported[16]{};
            static unsigned reported_count = 0;
            bool seen = false;
            for (unsigned i = 0; i < reported_count; ++i)
                seen = seen || reported[i] == instance;
            if (!seen && reported_count < 16)
            {
                reported[reported_count++] = instance;
                log("first-person: drawn weapon %s follows %s controller "
                    "(link bone %d, marker %d, %d segments, "
                    "extra transform %p)",
                    name ? name : "?", hand == 0 ? "left" : "right",
                    link_bone, link_marker, child_count, transform);
            }
            return true;
        }

        bool build_hand_strips(unsigned char* source,
                               const unsigned char* vertices,
                               int vertex_count, int segment_count,
                               const bool* selected_bones, bool triangle_list,
                               HandStripCache* output)
        {
            // Despite the name, each TextureStripInfo holds a triangle LIST:
            // the retail renderer consumes its indices in threes (0x00409FF4
            // and 0x0040A1B0 reverse each triple). The node list is copied
            // unchanged; a triangle is kept only when all three vertices
            // belong to this hand (including its virtual joint segments),
            // and every other triangle collapses to one hand vertex, so it
            // has zero area. Remapping single indices instead turned every
            // mixed triangle into a spike toward that one vertex.
            //
            // The earlier exact-triangle split looked broken only because
            // knuckle vertices on virtual segments were then misclassified
            // as non-hand and their triangles dropped.
            auto selected = [&](uint16_t index)
            {
                const int bone = *reinterpret_cast<const short*>(
                    vertices + index * 16 + 10);
                return bone >= 0 && bone < segment_count &&
                    selected_bones[bone];
            };
            std::vector<unsigned char*> strips;
            size_t bytes = 20; // zero-count terminal strip
            uint16_t wrist_vertex = UINT16_MAX;
            unsigned kept = 0;
            unsigned char* strip = source;
            size_t total_indices = 0;
            bool ended = false;
            for (int node = 0; node < 4096; ++node)
            {
                if (!strip)
                    return false;
                const int n = *reinterpret_cast<const short*>(strip);
                if (n == 0)
                {
                    ended = true;
                    break;
                }
                if (n < 3 || total_indices + n > 262144)
                    return false;
                total_indices += n;
                const uint16_t* indices =
                    reinterpret_cast<const uint16_t*>(strip + 0x14);
                for (int i = 0; i < n; ++i)
                    if (indices[i] >= vertex_count)
                        return false;
                for (int i = 0; i + 2 < n; i += 3)
                {
                    if (selected(indices[i]) && selected(indices[i + 1]) &&
                        selected(indices[i + 2]))
                    {
                        if (wrist_vertex == UINT16_MAX)
                            wrist_vertex = indices[i];
                        ++kept;
                    }
                }
                strips.push_back(strip);
                bytes += (20 + n * 2 + 3) & ~size_t(3);
                if (bytes > 2 * 1024 * 1024)
                    return false;
                unsigned char* next =
                    *reinterpret_cast<unsigned char**>(strip + 0x10);
                if (!next || next == strip)
                    return false;
                strip = next;
            }
            if (!ended || strips.empty() || wrist_vertex == UINT16_MAX ||
                kept < 4)
                return false;
            unsigned char* data = static_cast<unsigned char*>(
                HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
            if (!data)
                return false;

            size_t offset = 0;
            unsigned char* previous = nullptr;
            for (unsigned char* original : strips)
            {
                unsigned char* node = data + offset;
                const int count = *reinterpret_cast<short*>(original);
                memcpy(node, original, 20);
                uint16_t* out = reinterpret_cast<uint16_t*>(node + 0x14);
                const uint16_t* in = reinterpret_cast<const uint16_t*>(
                    original + 0x14);
                int i = 0;
                for (; i + 2 < count; i += 3)
                {
                    const bool keep = selected(in[i]) &&
                        selected(in[i + 1]) && selected(in[i + 2]);
                    for (int corner = 0; corner < 3; ++corner)
                        out[i + corner] = keep ? in[i + corner] : wrist_vertex;
                }
                for (; i < count; ++i)
                    out[i] = wrist_vertex;
                if (previous)
                    *reinterpret_cast<unsigned char**>(previous + 0x10) = node;
                previous = node;
                offset += (20 + count * 2 + 3) & ~size_t(3);
            }
            unsigned char* terminal = data + offset;
            if (previous)
                *reinterpret_cast<unsigned char**>(previous + 0x10) = terminal;
            output->data = data;
            output->nodes = static_cast<unsigned>(strips.size());
            output->triangles = kept;
            (void)triangle_list;
            return true;
        }

        // The retail secure-grip prompt is a real object born at the grab
        // point and linked to Lara -- in first person that is right in the
        // player's face, and huge. At its draw, move it 10 cm above the left
        // controller and scale it to grip_prompt_scale x its retail size
        // (absolute, from the first-seen row length, so it cannot compound).
        void grip_prompt_follow_hand(void* instance, void* model)
        {
            if (!instance || instance != g_grip_prompt_instance)
                return;
            Mat4 hand;
            if (!first_person_controller_world_pose(true, &hand))
                return;
            const float scale = tune_world_scale();
            if (!std::isfinite(scale) || scale <= 0.0f)
                return;
            __try
            {
                unsigned char* inst = static_cast<unsigned char*>(instance);
                if (*reinterpret_cast<void* const*>(inst + kOffInstanceObject)
                        != g_grip_prompt_object)
                {
                    g_grip_prompt_instance = nullptr;
                    return;
                }
                const unsigned char* m = static_cast<const unsigned char*>(model);
                if (!m)
                {
                    const unsigned char* object =
                        *reinterpret_cast<unsigned char* const*>(inst + 0x94);
                    const signed char lod =
                        *reinterpret_cast<const signed char*>(inst + 0xB0);
                    if (!object || lod < 0)
                        return;
                    m = (*reinterpret_cast<unsigned char* const* const*>(
                        object + 0x20))[lod];
                }
                float* matrices = *reinterpret_cast<float**>(
                    inst + kOffInstanceMatrices);
                const int count = m
                    ? *reinterpret_cast<const int*>(m + kOffModelNumSegments)
                    : 0;
                if (!matrices || count <= 0 || count > 64)
                    return;
                const float target[3] = {
                    hand.m[3][0], hand.m[3][1],
                    hand.m[3][2] + 0.10f * scale };
                const float row = sqrtf(matrices[0] * matrices[0] +
                                        matrices[1] * matrices[1] +
                                        matrices[2] * matrices[2]);
                if (g_grip_prompt_scaled != instance)
                {
                    g_grip_prompt_scaled = instance;
                    g_grip_prompt_base_row = row;
                    log("first-person: secure-grip prompt %p drawn; %d "
                        "segments, moved %.2f m to the left hand, scale %.2f",
                        instance, count,
                        sqrtf((target[0] - matrices[12]) *
                              (target[0] - matrices[12]) +
                              (target[1] - matrices[13]) *
                              (target[1] - matrices[13]) +
                              (target[2] - matrices[14]) *
                              (target[2] - matrices[14])) / scale,
                        tune_grip_prompt_scale());
                }
                float factor = 1.0f;
                if (row > 1e-6f && g_grip_prompt_base_row > 1e-6f)
                    factor = tune_grip_prompt_scale() *
                             g_grip_prompt_base_row / row;
                if (!std::isfinite(factor) || factor <= 0.0f)
                    factor = 1.0f;
                const float origin[3] = {
                    matrices[12], matrices[13], matrices[14] };
                for (int seg = 0; seg < count; ++seg)
                {
                    float* M = matrices + seg * 16;
                    for (int r = 0; r < 3; ++r)
                        for (int k = 0; k < 3; ++k)
                            M[r * 4 + k] *= factor;
                    for (int k = 0; k < 3; ++k)
                        M[12 + k] = target[k] +
                                    (M[12 + k] - origin[k]) * factor;
                }
                float* position = reinterpret_cast<float*>(inst + 0x10);
                for (int k = 0; k < 3; ++k)
                    position[k] = target[k];
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                g_grip_prompt_instance = nullptr;
            }
        }

        // Full-body mode: builds (once per strip source) the body triangle
        // list -- every triangle with no vertex in the HeadSegment subtree
        // (a virtual segment counts as head when either bone is).
        bool first_person_body_ready(const ModelSegment* segments, int count,
                                     int virtual_count, unsigned char* strips,
                                     const unsigned char* vertices,
                                     int vertex_count, bool triangle_list)
        {
            if (g_first_person_body_strips.data)
                return true;
            const int head = g_first_person_head_segment;
            if (head < 0 || head >= count)
                return false;
            static bool body_bone[512];
            const int total = count + virtual_count;
            for (int bone = 0; bone < count; ++bone)
                body_bone[bone] = !segment_is_in_head_subtree(
                    segments, count, bone, head);
            __try
            {
                const ModelVirtualSegment* virtuals =
                    reinterpret_cast<const ModelVirtualSegment*>(
                        segments + count);
                for (int v = 0; v < virtual_count; ++v)
                {
                    const int a = virtuals[v].index;
                    const int b = virtuals[v].weight_index;
                    body_bone[count + v] =
                        !(a >= 0 && a < count && !body_bone[a]) &&
                        !(b >= 0 && b < count && !body_bone[b]);
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            const bool built = build_hand_strips(
                strips, vertices, vertex_count, total, body_bone,
                triangle_list, &g_first_person_body_strips);
            log("first-person: full-body triangle list %s (%u nodes, %u "
                "triangles; head subtree of segment %d removed)",
                built ? "built" : "FAILED", g_first_person_body_strips.nodes,
                g_first_person_body_strips.triangles, head);
            return built && g_first_person_body_strips.data;
        }

        // Full-body draw. Each arm is bent so its wrist meets the tracked
        // hand: the elbow comes from a two-bone solve (bone lengths kept,
        // elbow hinted down and outward), the upper arm and forearm
        // subtrees rotate about shoulder and elbow, and the wrist subtree
        // takes the same hand delta the hands-only mode uses. Virtual
        // (blended) segments are not rebuilt by the renderer (0x00409B55
        // only multiplies the stored matrices by the view), so each is moved
        // by the same weighted change as its two bones -- the likely cause
        // of the torn arms in the 2026-09-24 IK attempts. All matrices and
        // the strip pointer are restored after the draw.
        void first_person_draw_full_body(
            void* instance, int a2, float a3, int a4, int a5, void* model,
            float a7, void* colour, void* transform, void* draw_model,
            unsigned char* strips, const ModelSegment* segments, int count,
            int virtual_count, float* matrices, const Mat4 hand_delta[2])
        {
            const int total = count + virtual_count;
            static float saved[512][16];
            memcpy(saved, matrices, sizeof(float) * 16 * total);
            // Per real segment: the world post-transform applied to it
            // (moved = saved * xform), identity when untouched.
            static Mat4 xform[512];
            for (int seg = 0; seg < count; ++seg)
                xform[seg] = Mat4::identity();

            // Upper body follows the headset: the waist subtree (first spine
            // bone below the pelvis on the path to the head) turns about the
            // vertical through the waist to the HMD heading; pelvis and legs
            // keep Lara's movement heading (user request 2026-09-30).
            int waist = -1;
            {
                int chain[64];
                int depth = 0;
                for (int seg = g_first_person_head_segment;
                     seg >= 0 && seg < count && depth < 64;
                     seg = segments[seg].parent)
                    chain[depth++] = seg;
                // chain[depth-1] is the root; the waist is its child on the
                // path to the head.
                if (depth >= 3)
                    waist = chain[depth - 2];
            }
            float torso_turn = 0.0f;
            if (waist >= 0)
            {
                __try
                {
                    const float lara_yaw = *reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(instance) +
                        kOffInstanceHeading);
                    const float* cam = reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(kMainCamera) +
                        kOffCwTransform);
                    // Heading vector convention: (sin(yaw), -cos(yaw), 0).
                    const float camera_yaw = atan2f(cam[8], -cam[9]);
                    if (std::isfinite(lara_yaw) && std::isfinite(camera_yaw) &&
                        cam[8] * cam[8] + cam[9] * cam[9] > 1.0e-4f)
                        torso_turn = wrap_pi(camera_yaw - lara_yaw);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    torso_turn = 0.0f;
                }
            }
            if (waist >= 0 && fabsf(torso_turn) > 1.0e-4f)
            {
                const float* pivot = saved[waist] + 12;
                Mat4 rotation = Mat4::identity();
                const float c = cosf(torso_turn), s = sinf(torso_turn);
                rotation.m[0][0] = c;
                rotation.m[0][1] = s;
                rotation.m[1][0] = -s;
                rotation.m[1][1] = c;
                const Mat4 turn = translation(-pivot[0], -pivot[1], -pivot[2]) *
                                  rotation *
                                  translation(pivot[0], pivot[1], pivot[2]);
                for (int seg = 0; seg < count; ++seg)
                    if (segment_is_in_head_subtree(segments, count, seg,
                                                   waist))
                        xform[seg] = turn;
            }

            // Anchor the body under the view: Lara's walk animation moves her
            // torso around her root (the body lurched ahead of the camera,
            // headset test), and room-scale leaning moves the camera off
            // her. Slide every segment horizontally so the HeadSegment joint
            // (neck/collar) sits under a neck point 0.10 m below and 0.08 m
            // behind the eye in the HEAD's own frame, so nodding down (which
            // swings the eyes forward) no longer pushes the body forward.
            // Vertical motion is left to the animation.
            if (g_first_person_head_segment >= 0 &&
                g_first_person_head_segment < count)
            {
                const int head = g_first_person_head_segment;
                const Mat4 neck = matrix_from_floats(saved[head]) *
                                  xform[head];
                float shift[2] = { 0.0f, 0.0f };
                bool ok = false;
                __try
                {
                    const float* cam = reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(kMainCamera) +
                        kOffCwTransform);
                    // Rows: 0 right, 1 down, 2 forward (engine view axes).
                    float down[3] = { cam[4], cam[5], cam[6] };
                    float fwd[3] = { cam[8], cam[9], cam[10] };
                    const float scale = tune_world_scale();
                    if (normalise3(down) > 1.0e-3f &&
                        normalise3(fwd) > 1.0e-3f)
                    {
                        float anchor[2];
                        for (int k = 0; k < 2; ++k)
                            anchor[k] = cam[12 + k] +
                                down[k] * 0.10f * scale -
                                fwd[k] * 0.08f * scale;
                        shift[0] = anchor[0] - neck.m[3][0];
                        shift[1] = anchor[1] - neck.m[3][1];
                        ok = std::isfinite(shift[0]) &&
                             std::isfinite(shift[1]) &&
                             shift[0] * shift[0] + shift[1] * shift[1] <
                                 powf(1.5f * tune_world_scale(), 2.0f);
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    ok = false;
                }
                if (ok)
                {
                    const Mat4 slide = translation(shift[0], shift[1], 0.0f);
                    for (int seg = 0; seg < count; ++seg)
                        xform[seg] = xform[seg] * slide;
                }
            }

            // The real head owns the height (user design 2026-10-03): the
            // headset leads Lara's crouch and stand clips, which put the view
            // inside her shoulders going down and held it low coming up.
            // Keep the neck joint where the view says it should be (the eye
            // minus first_person_eye_height, its relation at entry): the
            // chest subtree (the shoulders' common parent) moves rigidly
            // with the head, and everything below it -- spine, pelvis, legs
            // -- is squashed (or, while her stand clip lags, stretched)
            // vertically about the feet, which stay planted. The first test
            // squashed only the 0.10 m waist-to-chest spine and could lower
            // her 6 cm of the ~40 needed. 4 cm dead band for breathing and
            // walk bob; stretch only up to her own animated crouch depth.
            if (g_first_person_head_segment >= 0 &&
                g_first_person_head_segment < count &&
                g_first_person_traversal == TraversalGround &&
                !g_first_person_ledge_climbing)
            {
                const float scale = tune_world_scale();
                const int head = g_first_person_head_segment;
                int root = head;
                for (int guard = 0; guard < 64 && segments[root].parent >= 0 &&
                     segments[root].parent < count &&
                     segments[root].parent != root; ++guard)
                    root = segments[root].parent;
                float eye_z = 0.0f;
                bool have_eye = false;
                // The player's neck pivot, not the eye: nodding to look at
                // the body drops the eye ~0.1 m, which lowered her with it
                // and opened the neck hole (user test 2026-10-03). The pivot
                // is the slide's anchor above, 0.10 m below and 0.08 m behind
                // the eye in the head's frame; it barely moves on a nod.
                // eye_z is where the eye would be over that pivot upright.
                __try
                {
                    const float* cam = reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(kMainCamera) +
                        kOffCwTransform);
                    float down[3] = { cam[4], cam[5], cam[6] };
                    float fwd[3] = { cam[8], cam[9], cam[10] };
                    if (normalise3(down) > 1.0e-3f &&
                        normalise3(fwd) > 1.0e-3f)
                    {
                        eye_z = cam[14] + down[2] * 0.10f * scale -
                                fwd[2] * 0.08f * scale + 0.10f * scale;
                        have_eye = std::isfinite(eye_z);
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    have_eye = false;
                }
                // Chest: the first common ancestor of the two shoulders.
                int chest = -1;
                const int ls = g_first_person_shoulder_segment[0];
                const int rs = g_first_person_shoulder_segment[1];
                if (ls >= 0 && rs >= 0 && ls < count && rs < count)
                    for (int seg = segments[ls].parent, guard = 0;
                         seg >= 0 && seg < count && guard < 64;
                         seg = segments[seg].parent, ++guard)
                        if (segment_is_in_head_subtree(segments, count, rs,
                                                       seg))
                        {
                            chest = seg;
                            break;
                        }
                if (chest < 0 || chest == root)
                    chest = waist;
                const float feet = saved[root][14];
                const float neck_z = saved[head][14];
                const float span = chest >= 0 ? saved[chest][14] - feet : 0.0f;
                const float band = 0.04f * scale;
                float drop = have_eye && scale > 0.0f
                    ? neck_z - (eye_z - config().first_person_eye_height)
                    : 0.0f;
                if (!std::isfinite(drop))
                    drop = 0.0f;
                drop = drop > band ? drop - band
                     : drop < -band ? drop + band : 0.0f;
                // Stretch only to undo her own crouch (stand clip lagging).
                drop = fmaxf(drop, -fminf(g_first_person_crouch_drop,
                                          0.3f * span));
                drop = fminf(drop, 0.45f * span);
                static float reported_drop = 0.0f;
                if (chest >= 0 && span > 0.3f * scale && drop != 0.0f)
                {
                    const float k = (span - drop) / span;
                    Mat4 squash = Mat4::identity();
                    squash.m[2][2] = k;
                    squash.m[3][2] = feet * (1.0f - k);
                    const Mat4 lower = translation(0.0f, 0.0f, -drop);
                    for (int seg = 0; seg < count; ++seg)
                        xform[seg] = xform[seg] *
                            (segment_is_in_head_subtree(segments, count, seg,
                                                        chest)
                                 ? lower : squash);
                    if (fabsf(drop) > fabsf(reported_drop) + 0.05f * scale)
                    {
                        reported_drop = drop;
                        log("first-person: body %s %.2f m to the view (root "
                            "%d, chest %d, feet-to-chest %.2f m, x%.2f)",
                            drop > 0.0f ? "squashed" : "stretched",
                            fabsf(drop) / scale, root, chest, span / scale,
                            k);
                    }
                }
                else if (reported_drop != 0.0f)
                {
                    reported_drop = 0.0f;
                    log("first-person: body squash released");
                }
            }

            // Arms, solved on the turned torso.
            bool arm_ok[2] = { false, false };
            Mat4 upper[2], forearm[2];
            for (int hand = 0; hand < 2; ++hand)
            {
                const int s = g_first_person_shoulder_segment[hand];
                const int e = g_first_person_elbow_segment[hand];
                const int w = g_first_person_wrist_segment[hand];
                const int o = g_first_person_shoulder_segment[1 - hand];
                if (s < 0 || e < 0 || w < 0 || s >= count || e >= count ||
                    w >= count)
                    continue;
                auto moved_point = [&](int seg, float* out) {
                    const Mat4 m = matrix_from_floats(saved[seg]) *
                                   xform[seg];
                    for (int k = 0; k < 3; ++k)
                        out[k] = m.m[3][k];
                };
                float S[3], E[3], W[3], target[3], other[3];
                moved_point(s, S);
                moved_point(e, E);
                moved_point(w, W);
                if (o >= 0 && o < count)
                    moved_point(o, other);
                else
                    memcpy(other, S, sizeof(other));
                const Mat4 target_wrist =
                    matrix_from_floats(saved[w]) * hand_delta[hand];
                for (int k = 0; k < 3; ++k)
                    target[k] = target_wrist.m[3][k];
                float upper_v[3], lower_v[3];
                for (int k = 0; k < 3; ++k)
                {
                    upper_v[k] = E[k] - S[k];
                    lower_v[k] = W[k] - E[k];
                }
                const float l1 = sqrtf(dot3(upper_v, upper_v));
                const float l2 = sqrtf(dot3(lower_v, lower_v));
                // Elbow hint: down (world Z is up) and away from the other
                // shoulder, so the elbow does not follow the animation's
                // (often raised, aiming) bend.
                float out[3] = { S[0] - other[0], S[1] - other[1], 0.0f };
                if (normalise3(out) < 1.0e-4f)
                    out[0] = out[1] = 0.0f;
                float hint[3] = { 0.6f * out[0], 0.6f * out[1], -1.0f };
                normalise3(hint);
                float fake_e[3], fake_w[3], solved_e[3], solved_w[3];
                for (int k = 0; k < 3; ++k)
                {
                    fake_e[k] = S[k] + hint[k] * l1;
                    fake_w[k] = fake_e[k] + hint[k] * l2;
                }
                if (!solve_two_bone_arm(S, fake_e, fake_w, target, solved_e,
                                        solved_w))
                    continue;
                float from1[3], to1[3];
                for (int k = 0; k < 3; ++k)
                {
                    from1[k] = E[k] - S[k];
                    to1[k] = solved_e[k] - S[k];
                }
                upper[hand] = translation(-S[0], -S[1], -S[2]) *
                              rotation_between(from1, to1) *
                              translation(S[0], S[1], S[2]);
                float w1[3];
                for (int k = 0; k < 3; ++k)
                    w1[k] = W[0] * upper[hand].m[0][k] +
                            W[1] * upper[hand].m[1][k] +
                            W[2] * upper[hand].m[2][k] + upper[hand].m[3][k];
                float from2[3], to2[3];
                for (int k = 0; k < 3; ++k)
                {
                    from2[k] = w1[k] - solved_e[k];
                    to2[k] = solved_w[k] - solved_e[k];
                }
                forearm[hand] = upper[hand] *
                    translation(-solved_e[0], -solved_e[1], -solved_e[2]) *
                    rotation_between(from2, to2) *
                    translation(solved_e[0], solved_e[1], solved_e[2]);
                arm_ok[hand] = true;
            }

            static bool arm_layout_logged = false;
            for (int seg = 0; seg < count; ++seg)
                for (int hand = 0; hand < 2; ++hand)
                {
                    const int s = g_first_person_shoulder_segment[hand];
                    const int e = g_first_person_elbow_segment[hand];
                    const int w = g_first_person_wrist_segment[hand];
                    if (s < 0 || !segment_is_in_head_subtree(
                            segments, count, seg, s))
                        continue;
                    const bool in_wrist = w >= 0 && segment_is_in_head_subtree(
                        segments, count, seg, w);
                    const bool in_elbow = e >= 0 && segment_is_in_head_subtree(
                        segments, count, seg, e);
                    if (!arm_layout_logged && !in_wrist)
                        log("first-person: %s arm segment %d parent %d -> %s "
                            "at (%.0f, %.0f, %.0f)",
                            hand == 0 ? "left" : "right", seg,
                            segments[seg].parent,
                            in_elbow ? "forearm" : "upper arm",
                            saved[seg][12], saved[seg][13], saved[seg][14]);
                    if (in_wrist)
                        xform[seg] = hand_delta[hand];
                    else if (arm_ok[hand])
                        xform[seg] = xform[seg] *
                            (in_elbow ? forearm[hand] : upper[hand]);
                    break;
                }
            arm_layout_logged = true;

            for (int seg = 0; seg < count; ++seg)
            {
                const Mat4 moved = matrix_from_floats(saved[seg]) * xform[seg];
                memcpy(matrices + seg * 16, &moved.m[0][0], sizeof(moved.m));
            }

            // Virtual (blended) segments: blend their two bones' world
            // post-transforms and apply that to the stored blend. Bones that
            // move together (fingers, knuckles) then move the blend rigidly,
            // exactly as in hands-only mode, whatever retail's blend formula.
            __try
            {
                const ModelVirtualSegment* virtuals =
                    reinterpret_cast<const ModelVirtualSegment*>(
                        segments + count);
                if (g_virtual_blend_mode < 0)
                {
                    double error[2] = { 0.0, 0.0 };
                    for (int v = 0; v < virtual_count; ++v)
                    {
                        const int a = virtuals[v].index;
                        const int b = virtuals[v].weight_index;
                        const float wt = virtuals[v].weight;
                        if (a < 0 || a >= count || b < 0 || b >= count)
                            continue;
                        for (int i = 0; i < 16; ++i)
                        {
                            const float value = saved[count + v][i];
                            error[0] += fabs(value -
                                ((1.0f - wt) * saved[a][i] + wt * saved[b][i]));
                            error[1] += fabs(value -
                                (wt * saved[a][i] + (1.0f - wt) * saved[b][i]));
                        }
                    }
                    g_virtual_blend_mode = error[1] < error[0] ? 1 : 0;
                    log("first-person: virtual segment blend fit error "
                        "%.3f ((1-w)*index + w*weight) vs %.3f (w*index + "
                        "(1-w)*weight) over %d segments; using %d",
                        error[0], error[1], virtual_count,
                        g_virtual_blend_mode);
                }
                for (int v = 0; v < virtual_count; ++v)
                {
                    const int a = virtuals[v].index;
                    const int b = virtuals[v].weight_index;
                    if (a < 0 || a >= count || b < 0 || b >= count)
                        continue;
                    const float wt = virtuals[v].weight;
                    const float wa = g_virtual_blend_mode ? wt : 1.0f - wt;
                    const float wb = 1.0f - wa;
                    Mat4 blend;
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            blend.m[r][c] = wa * xform[a].m[r][c] +
                                            wb * xform[b].m[r][c];
                    const Mat4 moved =
                        matrix_from_floats(saved[count + v]) * blend;
                    memcpy(matrices + (count + v) * 16, &moved.m[0][0],
                           sizeof(moved.m));
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                memcpy(matrices, saved, sizeof(float) * 16 * total);
                return;
            }

            unsigned char* model_bytes = static_cast<unsigned char*>(
                draw_model);
            *reinterpret_cast<unsigned char**>(
                model_bytes + kOffModelTextureStrips) =
                    g_first_person_body_strips.data;
            g_draw_instance(instance, a2, a3, a4, a5, model, a7, colour,
                            transform);
            *reinterpret_cast<unsigned char**>(
                model_bytes + kOffModelTextureStrips) = strips;
            memcpy(matrices, saved, sizeof(float) * 16 * total);

            if (!g_full_body_reported)
            {
                g_full_body_reported = true;
                log("first-person: full-body draw (arms bent to the hands: "
                    "left %s, right %s; waist segment %d, torso turn %.1f "
                    "deg)", arm_ok[0] ? "yes" : "no",
                    arm_ok[1] ? "yes" : "no", waist,
                    torso_turn * 57.2957795f);
            }
        }

        // Board-mode pick-up state (camera_board_pluck_update).
        struct BoardPluck
        {
            int hand = -1;              // hand holding Lara, -1 none
            bool was[2] = { false, false };
            bool inside[2] = { false, false };
            float grab_offset[3]{};     // Lara root - hand at the grab
            float carry[3]{};           // drawn offset while held
            float target[3]{};          // where her root is held
            bool floor_valid = false;
            float floor[3]{};
            bool hover = false;
            float root[3]{};
            // Dangle (pendulum hanging from the grab point, real metres):
            // body tilt angles about world x/y, and lagging limb tilts
            // (0/1 arms, 2/3 legs).
            float hand_pos[3]{};
            float last_hand[3]{};
            float hand_vel[3]{};
            float hand_acc[2]{};
            float tilt[2]{};
            float tilt_vel[2]{};
            float limb_tilt[4][2]{};
            float phase = 0.0f;
            Mat4 body_rotation = Mat4::identity();
            // The pinch point (between thumb and index tips) at true world
            // scale, from the last hand draw, and the hand's world axes.
            float pinch[3]{};
            DWORD pinch_time = 0;
            float hand_axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            Mat4 limb_rotation[4] = { Mat4::identity(), Mat4::identity(),
                                      Mat4::identity(), Mat4::identity() };
            // Palm platform (2026-10-03): Lara set down on an upturned
            // hand stands on it (drawn there, like a carry) until the hand
            // turns over. palm = that hand, -1 none; offset = her feet
            // from the palm point, horizontal, world units.
            int palm = -1;
            float palm_offset[2]{};
            float palm_last_root[2]{};  // her real root last frame
            // Jumping on the palm (2026-10-04): her real instance jumps
            // where it stands; its rise above palm_base_z (her real floor
            // height, tracked while grounded) lifts her off the palm.
            float palm_base_z = 0.0f;
            float last_root_z = 0.0f;
            bool last_root_valid = false;
            DWORD palm_left_at = 0;
        };
        BoardPluck g_pluck;

        // Each third-person hand's palm as last drawn: centre (true world
        // scale) and the palm normal; fresh for 250 ms.
        struct PalmSeen
        {
            float point[3]{};
            float normal[3]{};
            DWORD time = 0;
        };
        PalmSeen g_palm_seen[2];

        // Finger flick (board mode, left hand, hold Y long; 2026-10-03):
        // holding loads it (index curled behind the thumb, the rest in a
        // fist), releasing snaps the index out. amount: 1 loaded, 0 rest,
        // negative overextended. tip: the index tip as last drawn (true
        // world scale), and the one before it, for the swept hit test.
        struct BoardFlick
        {
            int phase = 0;              // 0 idle, 1 loading, 2 snapping
            float amount = 0.0f;
            float fist = 0.0f;
            LARGE_INTEGER started{};
            LARGE_INTEGER last{};
            float tip[3]{};
            float last_tip[3]{};
            DWORD tip_time = 0;
            bool tip_valid = false;
            bool hit_done = false;
            // Knockdown animation playing once: restore her previous
            // (looping) animation when it ends, so it never repeats.
            bool knock_active = false;
            void* knock_keylist = nullptr;
            int knock_restore = -1;
            LARGE_INTEGER knock_until{};
        };
        BoardFlick g_flick;
        int g_flick_anim = -2;          // INI [vr] board_flick_anim, -1 none
        // The user's picks (2026-10-03): 140 when flicked from the front,
        // 139 from behind. INI board_flick_anim_front / _back.
        int g_flick_anim_front = 140;
        int g_flick_anim_back = 139;
        int g_palm_flip = -1;   // INI [vr] board_palm_flip (0/1)

        bool palm_fresh(int hand)
        {
            return hand >= 0 && hand < 2 && g_palm_seen[hand].time &&
                   GetTickCount() - g_palm_seen[hand].time < 250;
        }

        // How much the palm faces up (normal . world up), -2 if unknown.
        float palm_up(int hand)
        {
            return palm_fresh(hand) ? g_palm_seen[hand].normal[2] : -2.0f;
        }

        // Procedural posing on a model's world matrices: per-segment
        // world-space relative transforms (xform), committed as
        // saved * xform, with virtual (knuckle/joint blend) segments
        // recomputed from their two bones as the full-body path does.
        struct PoseWork
        {
            const ModelSegment* segments = nullptr;
            int count = 0;
            int virtual_count = 0;
            float saved[512][16];
            Mat4 xform[512];
            bool changed[512];
        };
        PoseWork g_pose_work;

        bool pose_begin(PoseWork& w, const ModelSegment* segments, int count,
                        int virtual_count, const float* matrices)
        {
            if (!segments || !matrices || count <= 0 || virtual_count < 0 ||
                count + virtual_count > 512)
                return false;
            w.segments = segments;
            w.count = count;
            w.virtual_count = virtual_count;
            memcpy(w.saved, matrices,
                   sizeof(float) * 16 * (count + virtual_count));
            for (int s = 0; s < count + virtual_count; ++s)
            {
                w.xform[s] = Mat4::identity();
                w.changed[s] = false;
            }
            return true;
        }

        void pose_joint(const PoseWork& w, int s, float out[3])
        {
            const float* m = w.saved[s];
            const Mat4& x = w.xform[s];
            for (int k = 0; k < 3; ++k)
                out[k] = m[12] * x.m[0][k] + m[13] * x.m[1][k] +
                         m[14] * x.m[2][k] + x.m[3][k];
        }

        void pose_rotate_subtree(PoseWork& w, int root, const float pivot[3],
                                 const Mat4& rotation)
        {
            const Mat4 about = translation(-pivot[0], -pivot[1], -pivot[2]) *
                rotation * translation(pivot[0], pivot[1], pivot[2]);
            for (int s = 0; s < w.count; ++s)
                if (segment_is_in_head_subtree(w.segments, w.count, s, root))
                {
                    w.xform[s] = w.xform[s] * about;
                    w.changed[s] = true;
                }
        }

        void pose_commit(PoseWork& w, float* matrices)
        {
            for (int s = 0; s < w.count; ++s)
                if (w.changed[s])
                {
                    const Mat4 moved = matrix_from_floats(w.saved[s]) *
                                       w.xform[s];
                    memcpy(matrices + s * 16, &moved.m[0][0],
                           sizeof(moved.m));
                }
            const ModelVirtualSegment* virtuals =
                reinterpret_cast<const ModelVirtualSegment*>(
                    w.segments + w.count);
            const int mode = g_virtual_blend_mode < 0 ? 0
                                                      : g_virtual_blend_mode;
            for (int v = 0; v < w.virtual_count; ++v)
            {
                const int a = virtuals[v].index;
                const int b = virtuals[v].weight_index;
                if (a < 0 || a >= w.count || b < 0 || b >= w.count ||
                    !(w.changed[a] || w.changed[b]))
                    continue;
                const float wt = virtuals[v].weight;
                const float wa = mode ? wt : 1.0f - wt;
                const float wb = 1.0f - wa;
                Mat4 blend;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        blend.m[r][c] = wa * w.xform[a].m[r][c] +
                                        wb * w.xform[b].m[r][c];
                const Mat4 moved =
                    matrix_from_floats(w.saved[w.count + v]) * blend;
                memcpy(matrices + (w.count + v) * 16, &moved.m[0][0],
                       sizeof(moved.m));
            }
        }

        // CCD: rotate each joint of a chain (tip side first) so the chain's
        // extrapolated fingertip moves toward the target.
        void pose_chain_tip(const PoseWork& w, const int* chain, int depth,
                            float tip[3])
        {
            float last[3], before[3];
            pose_joint(w, chain[depth - 1], last);
            pose_joint(w, chain[depth - 2], before);
            for (int k = 0; k < 3; ++k)
                tip[k] = last[k] + (last[k] - before[k]) * 0.9f;
        }

        void pose_ccd(PoseWork& w, const int* chain, int depth,
                      const float target[3])
        {
            for (int iteration = 0; iteration < 4; ++iteration)
                for (int j = depth - 1; j >= 0; --j)
                {
                    float pivot[3], tip[3], from[3], to[3];
                    pose_joint(w, chain[j], pivot);
                    pose_chain_tip(w, chain, depth, tip);
                    for (int k = 0; k < 3; ++k)
                    {
                        from[k] = tip[k] - pivot[k];
                        to[k] = target[k] - pivot[k];
                    }
                    pose_rotate_subtree(w, chain[j], pivot, rotation_fraction(
                        rotation_between(from, to), 0.6f));
                }
        }

        Mat4 axis_rotation(const float axis_in[3], float angle);

        // The holding hand's pinch: thumb and index tips meet, the other
        // three open and fan out. Fingers are the wrist's child chains:
        // thumb = least far along the palm direction, index = nearest the
        // thumb, then middle/ring/pinky by distance from it -- confirmed by
        // the user's finger-debug pass (2026-10-02: chains index, middle,
        // ring, pinky, thumb; segments 62..75 left, 83..96 right). Writes
        // the pinch point (between the two tips) in the drawn hand's space.
        bool pose_pinch(PoseWork& w, int wrist, const float palm_local[3],
                        int hand, float pinch_out[3])
        {
            const int count = w.count;
            int chains[8][5];
            int depth[8];
            int n = 0;
            for (int c = 0; c < count && n < 8; ++c)
            {
                if (w.segments[c].parent != wrist || c == wrist)
                    continue;
                int d = 0;
                int current = c;
                while (current >= 0 && d < 5)
                {
                    chains[n][d++] = current;
                    int next = -1;
                    for (int s = 0; s < count; ++s)
                        if (s != current && w.segments[s].parent == current)
                        {
                            next = s;
                            break;
                        }
                    current = next;
                }
                if (d >= 2)
                    depth[n++] = d;
            }
            if (n < 5)
                return false;
            float wrist_o[3];
            pose_joint(w, wrist, wrist_o);
            float f[3];
            const float* wm = w.saved[wrist];
            for (int k = 0; k < 3; ++k)
                f[k] = palm_local[0] * wm[k] + palm_local[1] * wm[4 + k] +
                       palm_local[2] * wm[8 + k];
            if (normalise3(f) < 1e-4f)
                return false;
            float base[8][3], along[8];
            for (int i = 0; i < n; ++i)
            {
                pose_joint(w, chains[i][0], base[i]);
                along[i] = 0.0f;
                for (int k = 0; k < 3; ++k)
                    along[i] += (base[i][k] - wrist_o[k]) * f[k];
            }
            int thumb = 0;
            for (int i = 1; i < n; ++i)
                if (along[i] < along[thumb])
                    thumb = i;
            auto dist = [&](int a, int b) {
                float d = 0.0f;
                for (int k = 0; k < 3; ++k)
                    d += (base[a][k] - base[b][k]) * (base[a][k] - base[b][k]);
                return d;
            };
            int index = -1;
            for (int i = 0; i < n; ++i)
                if (i != thumb && (index < 0 ||
                                   dist(i, thumb) < dist(index, thumb)))
                    index = i;
            int rest[8];
            int rest_n = 0;
            for (int i = 0; i < n; ++i)
                if (i != thumb && i != index)
                    rest[rest_n++] = i;
            for (int a = 0; a < rest_n; ++a)
                for (int b = a + 1; b < rest_n; ++b)
                    if (dist(rest[b], index) < dist(rest[a], index))
                    {
                        const int t = rest[a];
                        rest[a] = rest[b];
                        rest[b] = t;
                    }
            if (rest_n > 3)
                rest_n = 3;
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                log("board: pinch fingers -- thumb seg %d (along %.1f), "
                    "index %d, others %d %d %d of %d chains", chains[thumb][0],
                    along[thumb], chains[index][0],
                    rest_n > 0 ? chains[rest[0]][0] : -1,
                    rest_n > 1 ? chains[rest[1]][0] : -1,
                    rest_n > 2 ? chains[rest[2]][0] : -1, n);
            }
            // Wrist axes (finger debug): fingers along x, fanned across y
            // (index -y, pinky +y), palm facing along z. Wrist y+ curls the
            // left index toward the thumb; the right hand is the mirror
            // (x flipped), so its rotation axes flip sign.
            const float mirror = hand == 0 ? 1.0f : -1.0f;
            float curl_axis[3], palm_axis[3];
            for (int k = 0; k < 3; ++k)
            {
                curl_axis[k] = wm[4 + k] * mirror;
                palm_axis[k] = wm[8 + k] * mirror;
            }
            // Middle, ring, pinky: open a little and fan out pinky-ward.
            const float spread[3] = { 0.10f, 0.24f, 0.40f };
            for (int r = 0; r < rest_n; ++r)
            {
                const int i = rest[r];
                for (int j = 0; j < depth[i]; ++j)
                {
                    float pivot[3];
                    pose_joint(w, chains[i][j], pivot);
                    pose_rotate_subtree(w, chains[i][j], pivot,
                                        axis_rotation(curl_axis, -0.15f));
                }
                float pivot[3];
                pose_joint(w, chains[i][0], pivot);
                pose_rotate_subtree(w, chains[i][0], pivot,
                                    axis_rotation(palm_axis, spread[r]));
            }
            // Index curls in toward the thumb.
            const float curl[5] = { 0.45f, 0.70f, 0.60f, 0.5f, 0.5f };
            for (int j = 0; j < depth[index]; ++j)
            {
                float pivot[3];
                pose_joint(w, chains[index][j], pivot);
                pose_rotate_subtree(w, chains[index][j], pivot,
                                    axis_rotation(curl_axis, curl[j]));
            }
            // Thumb reaches its tip to the index tip.
            float thumb_tip[3], index_tip[3];
            pose_chain_tip(w, chains[index], depth[index], index_tip);
            pose_ccd(w, chains[thumb], depth[thumb], index_tip);
            pose_chain_tip(w, chains[thumb], depth[thumb], thumb_tip);
            pose_chain_tip(w, chains[index], depth[index], index_tip);
            for (int k = 0; k < 3; ++k)
                pinch_out[k] = 0.5f * (thumb_tip[k] + index_tip[k]);
            return true;
        }

        int find_finger_chains(const PoseWork& w, int wrist, int chains[8][5],
                               int depth[8]);

        // The flick hand: index curled by `amount` (negative overextends),
        // middle/ring/pinky curled into a fist by `fist`, and while loaded
        // the thumb's tip held on the index nail. Finger roles as in
        // pose_pinch. Writes the index tip in the drawn hand's space.
        bool pose_flick(PoseWork& w, int wrist, const float palm_local[3],
                        int hand, float amount, float fist, float tip_out[3])
        {
            int chains[8][5];
            int depth[8];
            const int n = find_finger_chains(w, wrist, chains, depth);
            if (n < 5)
                return false;
            float wrist_o[3];
            pose_joint(w, wrist, wrist_o);
            const float* wm = w.saved[wrist];
            float f[3];
            for (int k = 0; k < 3; ++k)
                f[k] = palm_local[0] * wm[k] + palm_local[1] * wm[4 + k] +
                       palm_local[2] * wm[8 + k];
            if (normalise3(f) < 1e-4f)
                return false;
            float base[8][3], along[8];
            for (int i = 0; i < n; ++i)
            {
                pose_joint(w, chains[i][0], base[i]);
                along[i] = 0.0f;
                for (int k = 0; k < 3; ++k)
                    along[i] += (base[i][k] - wrist_o[k]) * f[k];
            }
            int thumb = 0;
            for (int i = 1; i < n; ++i)
                if (along[i] < along[thumb])
                    thumb = i;
            auto dist = [&](int a, int b) {
                float d = 0.0f;
                for (int k = 0; k < 3; ++k)
                    d += (base[a][k] - base[b][k]) * (base[a][k] - base[b][k]);
                return d;
            };
            int index = -1;
            for (int i = 0; i < n; ++i)
                if (i != thumb && (index < 0 ||
                                   dist(i, thumb) < dist(index, thumb)))
                    index = i;
            const float mirror = hand == 0 ? 1.0f : -1.0f;
            float curl_axis[3];
            for (int k = 0; k < 3; ++k)
                curl_axis[k] = wm[4 + k] * mirror;
            // The other three: a fist.
            for (int i = 0; i < n; ++i)
            {
                if (i == thumb || i == index)
                    continue;
                for (int j = 0; j < depth[i]; ++j)
                {
                    float pivot[3];
                    pose_joint(w, chains[i][j], pivot);
                    pose_rotate_subtree(w, chains[i][j], pivot,
                                        axis_rotation(curl_axis, 1.1f * fist));
                }
            }
            // Index: curled hard when loaded, flung straight past rest.
            const float curl[5] = { 1.0f, 1.3f, 1.1f, 0.8f, 0.8f };
            for (int j = 0; j < depth[index]; ++j)
            {
                float pivot[3];
                pose_joint(w, chains[index][j], pivot);
                pose_rotate_subtree(w, chains[index][j], pivot,
                                    axis_rotation(curl_axis, curl[j] * amount));
            }
            float index_tip[3];
            pose_chain_tip(w, chains[index], depth[index], index_tip);
            // Thumb holds the nail while loaded.
            if (amount > 0.6f)
                pose_ccd(w, chains[thumb], depth[thumb], index_tip);
            pose_chain_tip(w, chains[index], depth[index], index_tip);
            for (int k = 0; k < 3; ++k)
                tip_out[k] = index_tip[k];
            return true;
        }

        // Lara's hips: walk up from her two lowest leaf bones (the feet) to
        // their common ancestor (the pelvis); each hip is the child of it on
        // that path. Cached per model.
        void* g_hip_model = nullptr;
        int g_hip[2] = { -1, -1 };

        void find_hips(const ModelSegment* segments, int count,
                       const float (*saved)[16])
        {
            g_hip[0] = g_hip[1] = -1;
            bool has_child[512] = {};
            for (int s = 0; s < count; ++s)
                if (segments[s].parent >= 0 && segments[s].parent < count)
                    has_child[segments[s].parent] = true;
            int foot[2] = { -1, -1 };
            for (int s = 0; s < count; ++s)
                if (!has_child[s] &&
                    (foot[0] < 0 || saved[s][14] < saved[foot[0]][14]))
                    foot[0] = s;
            const float apart = 0.08f * config().world_scale;
            for (int s = 0; s < count; ++s)
            {
                if (has_child[s] || s == foot[0] || foot[0] < 0)
                    continue;
                const float dx = saved[s][12] - saved[foot[0]][12];
                const float dy = saved[s][13] - saved[foot[0]][13];
                if (dx * dx + dy * dy < apart * apart)
                    continue;
                if (foot[1] < 0 || saved[s][14] < saved[foot[1]][14])
                    foot[1] = s;
            }
            if (foot[0] < 0 || foot[1] < 0)
                return;
            int common = -1;
            for (int a = foot[0], guard = 0; a >= 0 && guard < 64;
                 a = segments[a].parent, ++guard)
                if (segment_is_in_head_subtree(segments, count, foot[1], a))
                {
                    common = a;
                    break;
                }
            if (common < 0)
                return;
            for (int k = 0; k < 2; ++k)
                for (int a = foot[k], guard = 0; a >= 0 && guard < 64;
                     a = segments[a].parent, ++guard)
                    if (segments[a].parent == common)
                    {
                        g_hip[k] = a;
                        break;
                    }
            log("board: dangle legs -- feet %d/%d, pelvis %d, hips %d/%d",
                foot[0], foot[1], common, g_hip[0], g_hip[1]);
        }

        // Rotation of `angle` radians about a unit world axis, row-vector
        // convention (p' = p * R), as the rest of the posing code.
        Mat4 axis_rotation(const float axis_in[3], float angle)
        {
            float k[3] = { axis_in[0], axis_in[1], axis_in[2] };
            if (normalise3(k) < 1e-5f)
                return Mat4::identity();
            const float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
            Mat4 r = Mat4::identity();
            r.m[0][0] = c + t * k[0] * k[0];
            r.m[0][1] = t * k[0] * k[1] + s * k[2];
            r.m[0][2] = t * k[0] * k[2] - s * k[1];
            r.m[1][0] = t * k[0] * k[1] - s * k[2];
            r.m[1][1] = c + t * k[1] * k[1];
            r.m[1][2] = t * k[1] * k[2] + s * k[0];
            r.m[2][0] = t * k[0] * k[2] + s * k[1];
            r.m[2][1] = t * k[1] * k[2] - s * k[0];
            r.m[2][2] = c + t * k[2] * k[2];
            return r;
        }

        // The wrist's finger chains (child chains of depth >= 2).
        int find_finger_chains(const PoseWork& w, int wrist, int chains[8][5],
                               int depth[8])
        {
            int n = 0;
            for (int c = 0; c < w.count && n < 8; ++c)
            {
                if (w.segments[c].parent != wrist || c == wrist)
                    continue;
                int d = 0;
                int current = c;
                while (current >= 0 && d < 5)
                {
                    chains[n][d++] = current;
                    int next = -1;
                    for (int s = 0; s < w.count; ++s)
                        if (s != current && w.segments[s].parent == current)
                        {
                            next = s;
                            break;
                        }
                    current = next;
                }
                if (d >= 2)
                    depth[n++] = d;
            }
            return n;
        }

        // Finger debug: curl every joint of one chain by 0.6 rad about one
        // of the wrist's own axes, so the user can name each chain and the
        // axis/sign that curls it naturally.
        int g_finger_debug_chain = 0;
        bool g_finger_debug_curl = false;
        int g_finger_debug_axis = 0;          // 0..5: x+ x- y+ y- z+ z-
        unsigned g_finger_debug_reported[2] = { ~0u, ~0u };

        bool pose_debug_curl(PoseWork& w, int wrist, int hand)
        {
            int chains[8][5];
            int depth[8];
            const int n = find_finger_chains(w, wrist, chains, depth);
            if (n <= 0)
                return false;
            const int c = ((g_finger_debug_chain % n) + n) % n;
            const float* wm = w.saved[wrist];
            const int row = g_finger_debug_axis / 2;
            const float sign = (g_finger_debug_axis % 2) ? -1.0f : 1.0f;
            const float axis[3] = { wm[row * 4] * sign, wm[row * 4 + 1] * sign,
                                    wm[row * 4 + 2] * sign };
            const unsigned key = (unsigned)(c * 8 + g_finger_debug_axis);
            if (g_finger_debug_reported[hand] != key)
            {
                g_finger_debug_reported[hand] = key;
                float wrist_o[3];
                pose_joint(w, wrist, wrist_o);
                for (int i = 0; i < n; ++i)
                {
                    float b[3], local[3];
                    pose_joint(w, chains[i][0], b);
                    for (int r = 0; r < 3; ++r)
                    {
                        local[r] = 0.0f;
                        for (int k = 0; k < 3; ++k)
                            local[r] += (b[k] - wrist_o[k]) * wm[r * 4 + k];
                    }
                    log("finger debug: %s hand chain %d/%d segs %d..%d "
                        "(depth %d), base in wrist space (%.1f, %.1f, %.1f)%s",
                        hand == 0 ? "left" : "right", i + 1, n,
                        chains[i][0], chains[i][depth[i] - 1], depth[i],
                        local[0], local[1], local[2],
                        i == c ? "  <-- CURLING" : "");
                }
                log("finger debug: %s hand curling chain %d about wrist %c%c",
                    hand == 0 ? "left" : "right", c + 1, "xyz"[row],
                    sign > 0.0f ? '+' : '-');
            }
            const Mat4 bend = axis_rotation(axis, 0.6f);
            for (int j = 0; j < depth[c] - 1; ++j)
            {
                float pivot[3];
                pose_joint(w, chains[c][j], pivot);
                pose_rotate_subtree(w, chains[c][j], pivot, bend);
            }
            return true;
        }

        // Dangle animation (board_dangle_anim): while Lara is held, one of
        // her own animations plays (looped) and she hangs from her higher
        // wrist instead of the grab point. G2EmulationInstanceSetAnimation
        // 0x004DEC30 (inst, section, anim, frame, interp), SetMode
        // 0x004DED90 (2 = loop), G2Instance_GetKeylist 0x004DF1E0; the
        // current keylist of section 0 is AnimProcessor([inst+0xF4]+4)
        // ->sections(+0x120)[0] + 0x24; Object+0x1A = number of animations.
        int g_dangle_anim = -1;
        bool g_dangle_loaded = false;
        bool g_dangle_active = false;
        int g_dangle_original = -1;
        bool g_dangle_force = false;
        int g_pivot_wrist = -1;
        bool g_pivot_valid = false;
        float g_pivot[3]{};

        using PFN_GetKeylist = void*(__cdecl*)(void*, int);
        using PFN_SetAnimation = void(__cdecl*)(void*, int, int, int, int);
        using PFN_SetMode = void(__cdecl*)(void*, int, int);

        int lara_anim_count(void* lara)
        {
            __try
            {
                const unsigned char* object =
                    *reinterpret_cast<unsigned char* const*>(
                        static_cast<unsigned char*>(lara) + kOffInstanceObject);
                const int n = object
                    ? *reinterpret_cast<const short*>(object + 0x1A) : 0;
                return n > 0 && n < 4000 ? n : 0;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        void* lara_keylist(void* lara, int anim)
        {
            __try
            {
                return reinterpret_cast<PFN_GetKeylist>(0x004DF1E0)(lara, anim);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        void* lara_current_keylist(void* lara)
        {
            __try
            {
                const unsigned char* data =
                    *reinterpret_cast<unsigned char* const*>(
                        static_cast<unsigned char*>(lara) + 0xF4);
                const unsigned char* processor = data
                    ? *reinterpret_cast<unsigned char* const*>(data + 4)
                    : nullptr;
                const unsigned char* sections = processor
                    ? *reinterpret_cast<unsigned char* const*>(processor + 0x120)
                    : nullptr;
                return sections
                    ? *reinterpret_cast<void* const*>(sections + 0x24)
                    : nullptr;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        bool lara_play_animation(void* lara, int anim, bool loop)
        {
            __try
            {
                reinterpret_cast<PFN_SetAnimation>(0x004DEC30)(lara, 0, anim,
                                                               0, 0);
                const unsigned char* data =
                    *reinterpret_cast<unsigned char* const*>(
                        static_cast<unsigned char*>(lara) + 0xF4);
                const unsigned char* processor = data
                    ? *reinterpret_cast<unsigned char* const*>(data + 4)
                    : nullptr;
                const int sections = processor ? processor[0x150] : 0;
                if (loop)
                    for (int s = 0; s < sections && s < 8; ++s)
                        reinterpret_cast<PFN_SetMode>(0x004DED90)(lara, s, 2);
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        int lara_anim_index_of(void* lara, void* keylist)
        {
            const int n = lara_anim_count(lara);
            for (int i = 0; i < n && keylist; ++i)
                if (lara_keylist(lara, i) == keylist)
                    return i;
            return -1;
        }

        // Ledge pull-up and one-hand catch animations (user 2026-10-06):
        // retail picks between pull-ups of different lengths, and a one-hand
        // catch drops her after some time. Every animation change during
        // LedgeClimb (0x00F05804) or a precarious catch is logged with its
        // keylist length (u16 keys at +4, s16 ms per key at +6), plus how
        // long the state lasted. [vr] first_person_pullup_anim = N with
        // first_person_pullup_replace = a,b,c swaps any of a,b,c for N when
        // it starts during a first-person pull-up (empty = retail).
        struct ClimbWatch
        {
            bool loaded = false;
            int pullup_anim = -1;
            int replace[8]{};
            int replace_count = 0;
            // One-hand catch: seconds added by restarting its animation.
            float catch_extra = 4.5f;
            int catch_anim = -1;
            DWORD catch_restart_due = 0;
            DWORD catch_grace_end = 0;
            unsigned catch_restarts = 0;
            int kind = 0;               // 0 none, 1 pull-up, 2 one-hand catch
            unsigned number[3]{};
            DWORD start = 0;
            void* keylist = nullptr;
        };
        ClimbWatch g_climb_watch;

        void climb_watch_load()
        {
            ClimbWatch& w = g_climb_watch;
            w.loaded = true;
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            // Test 2026-10-06: tapping Up pulled up with animation 266 in
            // about 1.1 s, holding it with 236 in 2.5 s; first person
            // always gets 266. -1 = retail choice.
            w.pullup_anim = (int)GetPrivateProfileIntW(L"vr",
                L"first_person_pullup_anim", 266, path);
            wchar_t text[128]{};
            GetPrivateProfileStringW(L"vr", L"first_person_pullup_replace",
                                     L"236", text, _countof(text), path);
            // A one-hand catch (animation 257) dropped Lara 1.53 s after
            // it began; restarting the animation before it ends is tried
            // for catch_extra_time more seconds (0 = retail).
            wchar_t extra[32]{};
            GetPrivateProfileStringW(L"vr", L"catch_extra_time", L"4.5",
                                     extra, _countof(extra), path);
            w.catch_extra = (float)_wtof(extra);
            if (!(w.catch_extra >= 0.0f) || w.catch_extra > 10.0f)
                w.catch_extra = 4.5f;
            const wchar_t* p = text;
            while (*p && w.replace_count < 8)
            {
                wchar_t* end = nullptr;
                const long v = wcstol(p, &end, 10);
                if (end == p)
                {
                    ++p;
                    continue;
                }
                w.replace[w.replace_count++] = (int)v;
                p = end;
            }
            if (w.pullup_anim >= 0 && w.replace_count)
                log("first-person: pull-up animation %d replaces %d listed "
                    "alternative(s)", w.pullup_anim, w.replace_count);
        }

        // The secure window: ButtonPrompt::Show (0x00566C00) stores its
        // float argument -- from a per-difficulty table -- at
        // PlayerData+0x6B4, in 30 fps frames (45.00 for the 1.5 s window in
        // the 2026-10-06 test; it reads 0 once the catch ends). Extending the animation
        // alone kept Lara hanging but the window still closed at 1.51 s.
        bool catch_window(float add, float* before, float* after)
        {
            __try
            {
                unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!player)
                    return false;
                float* window = reinterpret_cast<float*>(player + 0x6B4);
                *before = *window;
                if (add > 0.0f && std::isfinite(*window))
                    *window += add;
                *after = *window;
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool keylist_length(void* keylist, unsigned* keys, int* ms)
        {
            __try
            {
                const unsigned char* k =
                    static_cast<const unsigned char*>(keylist);
                *keys = *reinterpret_cast<const unsigned short*>(k + 4);
                *ms = *reinterpret_cast<const short*>(k + 6);
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void climb_anim_watch(void* lara, uintptr_t vtable, bool precarious,
                              bool first_person)
        {
            ClimbWatch& w = g_climb_watch;
            if (!w.loaded)
                climb_watch_load();
            const int kind = vtable == 0x00F05804 ? 1 : precarious ? 2 : 0;
            const DWORD now = GetTickCount();
            const char* names[3] = { "", "ledge pull-up", "one-hand catch" };
            if (kind != w.kind)
            {
                if (w.kind)
                {
                    float before = 0.0f, after = 0.0f;
                    if (w.kind == 2 && catch_window(0.0f, &before, &after))
                        log("anim: %s %u ended after %.2f s (window now "
                            "%.2f)", names[w.kind], w.number[w.kind],
                            (now - w.start) / 1000.0f, after);
                    else
                        log("anim: %s %u ended after %.2f s", names[w.kind],
                            w.number[w.kind], (now - w.start) / 1000.0f);
                }
                w.kind = kind;
                w.keylist = nullptr;
                w.catch_anim = -1;
                if (kind)
                {
                    w.start = now;
                    ++w.number[kind];
                }
                if (kind == 2)
                {
                    const bool bar_hold = vtable == 0x00F05CA8;
                    const bool secured_auto =
                        config().auto_secure_catch == 2 ||
                        (config().auto_secure_catch == 1 && bar_hold);
                    float before = 0.0f, after = 0.0f;
                    if (first_person && !secured_auto &&
                        catch_window(w.catch_extra * 30.0f, &before,
                                     &after))
                        log("anim: one-hand catch %u: secure window %.2f -> "
                            "%.2f (PlayerData+0x6B4)", w.number[2], before,
                            after);
                    w.catch_restart_due = now + 1100;
                    w.catch_grace_end = now + 1100 +
                        (DWORD)(w.catch_extra * 1000.0f);
                    w.catch_restarts = 0;
                }
            }
            if (!kind || !lara)
                return;
            // More time on a one-hand catch the player secures by hand
            // (first person, auto_secure_catch not covering this hold).
            const bool bar = vtable == 0x00F05CA8;
            const bool auto_secure = config().auto_secure_catch == 2 ||
                                     (config().auto_secure_catch == 1 && bar);
            if (kind == 2 && first_person && !auto_secure &&
                w.catch_anim >= 0 && w.catch_extra > 0.0f &&
                (int)(now - w.catch_restart_due) >= 0 &&
                (int)(w.catch_grace_end - now) > 0 &&
                lara_play_animation(lara, w.catch_anim, false))
            {
                ++w.catch_restarts;
                w.catch_restart_due = now + 1100;
                w.keylist = lara_current_keylist(lara);
                log("anim: one-hand catch %u: animation %d restarted (%u) "
                    "for more time", w.number[2], w.catch_anim,
                    w.catch_restarts);
            }
            void* keylist = lara_current_keylist(lara);
            if (!keylist || keylist == w.keylist)
                return;
            w.keylist = keylist;
            const int anim = lara_anim_index_of(lara, keylist);
            if (kind == 2 && w.catch_anim < 0)
                w.catch_anim = anim;
            // (The keylist header guess, keys at +4 / ms at +6, read
            // nonsense in the 2026-10-06 log; durations come from the
            // state timings instead.)
            log("anim: %s %u: animation %d at +%.2f s", names[kind],
                w.number[kind], anim, (now - w.start) / 1000.0f);
            // The swap is first person only (user 2026-10-06); third
            // person just logs.
            if (!first_person || kind != 1 || w.pullup_anim < 0 ||
                anim < 0 || anim == w.pullup_anim)
                return;
            for (int i = 0; i < w.replace_count; ++i)
                if (w.replace[i] == anim && lara_keylist(lara, w.pullup_anim))
                {
                    if (lara_play_animation(lara, w.pullup_anim, false))
                    {
                        w.keylist = lara_keylist(lara, w.pullup_anim);
                        log("anim: pull-up animation %d replaced by %d",
                            anim, w.pullup_anim);
                    }
                    break;
                }
        }

        void dangle_load()
        {
            if (g_dangle_loaded)
                return;
            g_dangle_loaded = true;
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            // 472: the one-handed hang the user picked (2026-10-02).
            g_dangle_anim = (int)GetPrivateProfileIntW(L"vr",
                L"board_dangle_anim", 472, path);
            if (g_dangle_anim < -1)
                g_dangle_anim = -1;
        }

        void dangle_restore(void* lara)
        {
            if (g_dangle_active && lara && g_dangle_original >= 0)
                lara_play_animation(lara, g_dangle_original, true);
            g_dangle_active = false;
            g_pivot_valid = false;
            g_pivot_wrist = -1;
        }

        void dangle_maintain(void* lara)
        {
            dangle_load();
            if (g_dangle_anim < 0)
            {
                if (g_dangle_active)
                    dangle_restore(lara);
                return;
            }
            void* wanted = lara_keylist(lara, g_dangle_anim);
            if (!wanted)
                return;
            if (!g_dangle_active)
                g_dangle_original =
                    lara_anim_index_of(lara, lara_current_keylist(lara));
            if (g_dangle_force || lara_current_keylist(lara) != wanted)
            {
                g_dangle_force = false;
                if (lara_play_animation(lara, g_dangle_anim, true))
                {
                    if (!g_dangle_active)
                        log("board: dangle animation %d playing (was %d)",
                            g_dangle_anim, g_dangle_original);
                    g_dangle_active = true;
                }
            }
        }

        // Third-person immersive replaces the retail gear cross with the 3D
        // one (vr_submit): fade group 5 is the HUD inventory (the d-pad
        // cross -- UIHUDInventoryMenuHandler 0x004F5910 triggers it for each
        // direction) and 0xE its low-light warning (Hud::draw 0x004545BD).
        // Their triggers are dropped, and their alphas held at zero, so the
        // retail cross never shows. Health, ammo and hints are untouched.
        using PFN_FadeTrigger = void(__cdecl*)(int);
        PFN_FadeTrigger g_fade_trigger = nullptr;

        bool third_person_gear_hud_hidden()
        {
            return config().immersive_controls && !g_first_person_active &&
                   !g_first_person_instance;
        }

        // The retail light meter (rounded outline + yellow charge bar) is a
        // UIItemMeter showing game value 0x44; it drew whenever the light
        // was on, outside every fade group (only group 0, always 1.0, was
        // visible -- log 2026-10-04), and its quads use the shared white
        // texture, so neither fade nor texture skipping could reach it.
        // UIItemMeter_Draw (0x004EE4B0, cdecl item, drawInfo) is skipped
        // for that one meter in third-person immersive.
        using PFN_MeterDraw = void(__cdecl*)(void*, void*);
        PFN_MeterDraw g_meter_draw = nullptr;
        const unsigned char kMeterDrawSignature[8] =
            { 0x8B, 0x4C, 0x24, 0x04, 0xF6, 0x41, 0x01, 0x80 };

        void __cdecl detour_meter_draw(void* item, void* info)
        {
            if (item && third_person_gear_hud_hidden())
            {
                short value = 0;
                __try
                {
                    value = *reinterpret_cast<const short*>(
                        static_cast<const unsigned char*>(item) + 0x0C);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
                if (value == 0x44)
                {
                    static bool reported = false;
                    if (!reported)
                    {
                        reported = true;
                        log("third-person: retail light meter (outline and "
                            "charge bar) not drawn; the 3D gear cross shows "
                            "the charge");
                    }
                    return;
                }
            }
            g_meter_draw(item, info);
        }

        // The light meter's rounded outline is a font glyph (the box
        // character in the HUD font atlas) drawn by a sibling item, so
        // skipping the meter left it (user screenshot 2026-10-04). Skip the
        // whole UIItemGroup that holds the light meter when it is a small
        // group (the light's own block). UIItemGroup_Draw (0x004EC260,
        // cdecl item, drawInfo): child count at +0x20, child pointers from
        // +0x24, each child's draw at [[child+4]+0x10].
        const uintptr_t kUIItemMeterDraw = 0x004EE4B0;
        using PFN_GroupDraw = void(__cdecl*)(void*, void*);
        PFN_GroupDraw g_group_draw = nullptr;
        const uintptr_t kUIItemGroupDraw = 0x004EC260;
        const unsigned char kGroupDrawSignature[6] =
            { 0x83, 0xEC, 0x0C, 0x33, 0xC0, 0x56 };

        // 1 = small light group (skip), 2 = larger group holding it, 0 none.
        int light_group_kind(void* group)
        {
            __try
            {
                const unsigned char* g =
                    static_cast<const unsigned char*>(group);
                const int count = *reinterpret_cast<const int*>(g + 0x20);
                if (count <= 0 || count > 64)
                    return 0;
                void* const* children =
                    reinterpret_cast<void* const*>(g + 0x24);
                for (int i = 0; i < count; ++i)
                {
                    const unsigned char* child =
                        static_cast<const unsigned char*>(children[i]);
                    if (!child)
                        continue;
                    const uintptr_t* vtable =
                        *reinterpret_cast<const uintptr_t* const*>(child + 4);
                    if (vtable && vtable[0x10 / 4] == kUIItemMeterDraw &&
                        *reinterpret_cast<const short*>(child + 0x0C) == 0x44)
                        return count <= 4 ? 1 : 2;
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return 0;
        }

        void __cdecl detour_group_draw(void* group, void* info)
        {
            if (group && third_person_gear_hud_hidden())
            {
                const int kind = light_group_kind(group);
                static bool reported[3] = { false, false, false };
                if (kind && !reported[kind])
                {
                    reported[kind] = true;
                    int count = 0;
                    __try
                    {
                        count = *reinterpret_cast<const int*>(
                            static_cast<const unsigned char*>(group) + 0x20);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                    log("third-person: retail light block (group %p, %d "
                        "items) %s", group, count, kind == 1
                        ? "not drawn: outline, icon and bar"
                        : "is a large group -- left drawn, only its meter "
                          "skipped");
                }
                if (kind == 1)
                    return;
            }
            g_group_draw(group, info);
        }

        void __cdecl detour_fade_trigger(int type)
        {
            // UIItemMeter::DrawMeter triggers its item's own fade group as
            // the metered value crosses a threshold (0x004EDF4A, returning
            // to 0x004EDF4F): that is how the retail light meter (rounded
            // outline + yellow bar) appears while the light drains (user
            // screenshot 2026-10-02). In third-person immersive every meter
            // trigger is dropped except health (1) and air (2).
            const uintptr_t caller =
                reinterpret_cast<uintptr_t>(_ReturnAddress());
            const bool meter = caller == 0x004EDF4F;
            const bool hidden = third_person_gear_hud_hidden();
            if (hidden)
            {
                static uintptr_t seen[64][2];
                static unsigned seen_count = 0;
                bool known = false;
                for (unsigned i = 0; i < seen_count; ++i)
                    if (seen[i][0] == (uintptr_t)type && seen[i][1] == caller)
                        known = true;
                if (!known && seen_count < 64)
                {
                    seen[seen_count][0] = (uintptr_t)type;
                    seen[seen_count][1] = caller;
                    ++seen_count;
                    log("third-person: HUD fade group %d triggered from %p%s",
                        type, reinterpret_cast<void*>(caller),
                        meter ? " (meter)" : "");
                }
            }
            if (hidden && meter && type != 1 && type != 2 && type >= 0 &&
                type < 0x12)
            {
                __try
                {
                    reinterpret_cast<float*>(0x011137B8)[type] = 0.0f;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
                return;
            }
            if ((type == 5 || type == 0xE) && third_person_gear_hud_hidden())
            {
                static bool reported[2] = { false, false };
                bool& once = reported[type == 5 ? 0 : 1];
                if (!once)
                {
                    once = true;
                    log("third-person: retail %s hidden (fade group %d); "
                        "the 3D gear cross replaces it", type == 5
                        ? "gear cross" : "low-light warning", type);
                }
                __try
                {
                    reinterpret_cast<float*>(0x011137B8)[type] = 0.0f;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
                return;
            }
            if (g_fade_trigger)
                g_fade_trigger(type);
        }

        // Third-person immersive: Lara is drawn normally, then each hand is
        // drawn a second time on its own (the first-person hand-only strip
        // lists, cut from her mesh) and moved onto its controller, in front
        // of the third-person camera. First person's model/wrist globals are
        // borrowed for the duration of this draw only (the grip-pose helpers
        // read them) and put back, so first_person_leave never sees them.
        HandStripCache g_tp_hand_strips[2];
        void* g_tp_strip_source = nullptr;
        bool g_tp_hands_reported = false;
        bool g_tp_hands_failed_reported = false;
        bool g_tp_hands_on = false;

        void third_person_draw_hands_inner(void* instance, int a2, float a3,
            int a4, int a5, void* model, float a7, void* colour,
            void* transform, void* draw_model, const int wrist[2],
            ModelSegment* segments, unsigned char* vertices,
            unsigned char* strips, float* matrices, int count,
            int virtual_count, int vertex_count);

        void third_person_draw_hands(void* instance, int a2, float a3,
                                     int a4, int a5, void* model, float a7,
                                     void* colour, void* transform)
        {
            if (!config().immersive_controls || g_first_person_active ||
                g_first_person_instance || !instance)
                return;
            bool cinematic = false;
            __try
            {
                cinematic =
                    reinterpret_cast<PFN_CinematicPlaying>(
                        kCinematicPlaying)() ||
                    reinterpret_cast<PFN_CinematicPlaying>(
                        kResidentCinematicPlaying)();
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                cinematic = true;
            }
            const bool on = !cinematic &&
                !(ui_menu_active() && !ui_pause_menu_active());
            if (on != g_tp_hands_on)
            {
                g_tp_hands_on = on;
                log("third-person: controller hands %s", on
                    ? "shown" : "hidden (cinematic or menu)");
            }
            if (!on)
                return;

            void* draw_model = nullptr;
            int wrist[2] = { -1, -1 };
            ModelSegment* segments = nullptr;
            unsigned char* vertices = nullptr;
            unsigned char* strips = nullptr;
            float* matrices = nullptr;
            int count = 0, virtual_count = 0, vertex_count = 0;
            __try
            {
                if (instance != *reinterpret_cast<void**>(kPlayerInstance))
                    return;
                unsigned char* inst = static_cast<unsigned char*>(instance);
                unsigned char* obj = *reinterpret_cast<unsigned char**>(
                    inst + kOffInstanceObject);
                if (!obj)
                    return;
                const char* name = *reinterpret_cast<const char**>(
                    obj + kOffObjectName);
                if (!name || (strcmp(name, "lara") != 0 &&
                              strncmp(name, "lara_", 5) != 0 &&
                              strncmp(name, "amanda_player", 13) != 0))
                    return;
                const int num_models = *reinterpret_cast<short*>(
                    obj + kOffObjectNumModels);
                void** models = *reinterpret_cast<void***>(
                    obj + kOffObjectModelList);
                int index = *reinterpret_cast<signed char*>(
                    inst + kOffCurrentRenderModel);
                if (index < 0 || index >= num_models)
                    index = *reinterpret_cast<signed char*>(
                        inst + kOffCurrentBaseModel);
                if (!models || num_models <= 0 || num_models > 32 ||
                    index < 0 || index >= num_models || !models[index])
                    return;
                draw_model = models[index];
                // Other model variants drawn for the same instance.
                if (model && model != draw_model)
                    return;
                const unsigned char* prop =
                    *reinterpret_cast<unsigned char* const*>(kPlayerProp);
                if (!prop)
                    return;
                wrist[0] = *reinterpret_cast<const int*>(
                    prop + kOffPlayerPropLeftWrist);
                wrist[1] = *reinterpret_cast<const int*>(
                    prop + kOffPlayerPropRightWrist);
                unsigned char* m = static_cast<unsigned char*>(draw_model);
                count = *reinterpret_cast<int*>(m + kOffModelNumSegments);
                virtual_count = *reinterpret_cast<int*>(
                    m + kOffModelNumVirtSegments);
                segments = *reinterpret_cast<ModelSegment**>(
                    m + kOffModelSegments);
                vertex_count = *reinterpret_cast<int*>(
                    m + kOffModelNumVertices);
                vertices = *reinterpret_cast<unsigned char**>(
                    m + kOffModelVertices);
                strips = *reinterpret_cast<unsigned char**>(
                    m + kOffModelTextureStrips);
                matrices = *reinterpret_cast<float**>(
                    inst + kOffInstanceMatrices);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }
            if (!segments || !vertices || !strips || !matrices ||
                count <= 0 || virtual_count < 0 ||
                count + virtual_count > 512 || vertex_count <= 0 ||
                vertex_count > 65535 || wrist[0] < 0 || wrist[0] >= count ||
                wrist[1] < 0 || wrist[1] >= count)
                return;

            // Borrow the first-person model globals (see above).
            g_first_person_instance = instance;
            g_first_person_model = draw_model;
            g_first_person_wrist_segment[0] = wrist[0];
            g_first_person_wrist_segment[1] = wrist[1];
            g_grip_offset_metre = config().world_scale;
            third_person_draw_hands_inner(instance, a2, a3, a4, a5, model,
                a7, colour, transform, draw_model, wrist, segments,
                vertices, strips, matrices, count, virtual_count,
                vertex_count);
            g_grip_offset_metre = 0.0f;
            g_first_person_instance = nullptr;
            g_first_person_model = nullptr;
            g_first_person_wrist_segment[0] = -1;
            g_first_person_wrist_segment[1] = -1;
        }

        void third_person_draw_hands_inner(void* instance, int a2, float a3,
            int a4, int a5, void* model, float a7, void* colour,
            void* transform, void* draw_model, const int wrist[2],
            ModelSegment* segments, unsigned char* vertices,
            unsigned char* strips, float* matrices, int count,
            int virtual_count, int vertex_count)
        {
            Mat4 camera_to_world;
            memcpy(&camera_to_world.m[0][0],
                   static_cast<const unsigned char*>(kMainCamera) +
                       kOffCwTransform, sizeof(camera_to_world.m));
            Mat4 controllers[2];
            for (int hand = 0; hand < 2; ++hand)
            {
                Mat4 to_head;
                if (!controller_head_pose_for_hands(hand == 0, &to_head))
                    return;
                // Board mode shrinks the world, not the player. Scaling the
                // hand's bone matrices also scaled its normals (blown-out
                // lighting, user screenshot 2026-10-02). Instead the hand is
                // drawn at its own size at 1/F of the controller's distance
                // from the head, and its projection scales view space by F
                // about the head (vr_session, tag 2/3): it lands on the
                // controller F times larger, lit as normal.
                const float grow = camera_world_scale_factor();
                if (grow > 1.001f)
                    for (int k = 0; k < 3; ++k)
                        to_head.m[3][k] /= grow;
                controllers[hand] = to_head * camera_to_world;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        if (!std::isfinite(controllers[hand].m[r][c]))
                            return;
            }

            const int total = count + virtual_count;
            bool hand_bone[2][512]{};
            int hand_vertices[2] = { 0, 0 };
            __try
            {
                for (int bone = 0; bone < count; ++bone)
                    for (int hand = 0; hand < 2; ++hand)
                        hand_bone[hand][bone] = segment_is_in_head_subtree(
                            segments, count, bone, wrist[hand]);
                const ModelVirtualSegment* virtuals =
                    reinterpret_cast<const ModelVirtualSegment*>(
                        segments + count);
                for (int v = 0; v < virtual_count; ++v)
                {
                    const int a = virtuals[v].index;
                    const int b = virtuals[v].weight_index;
                    for (int hand = 0; hand < 2; ++hand)
                        hand_bone[hand][count + v] =
                            (a >= 0 && a < count && hand_bone[hand][a]) ||
                            (b >= 0 && b < count && hand_bone[hand][b]);
                }
                for (int i = 0; i < vertex_count; ++i)
                {
                    const int bone = *reinterpret_cast<const short*>(
                        vertices + i * 16 + 10);
                    if (bone < 0 || bone >= total)
                        continue;
                    for (int hand = 0; hand < 2; ++hand)
                        if (hand_bone[hand][bone])
                            ++hand_vertices[hand];
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }

            uint32_t draw_flags = 0;
            memcpy(&draw_flags, &a7, sizeof(draw_flags));
            const bool triangle_list = (draw_flags & 1u) != 0;
            if (g_tp_strip_source != strips)
            {
                for (HandStripCache& cache : g_tp_hand_strips)
                {
                    if (cache.data)
                        HeapFree(GetProcessHeap(), 0, cache.data);
                    cache = HandStripCache{};
                }
                g_tp_strip_source = strips;
                g_tp_hands_failed_reported = false;
                if (hand_vertices[0] >= 12 && hand_vertices[1] >= 12)
                {
                    bool ok = false;
                    __try
                    {
                        ok = build_hand_strips(strips, vertices,
                            vertex_count, total, hand_bone[0],
                            triangle_list, &g_tp_hand_strips[0]) &&
                            build_hand_strips(strips, vertices,
                            vertex_count, total, hand_bone[1],
                            triangle_list, &g_tp_hand_strips[1]);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        ok = false;
                    }
                    (void)ok;
                }
            }
            if (!g_tp_hand_strips[0].data || !g_tp_hand_strips[1].data)
            {
                if (!g_tp_hands_failed_reported)
                {
                    g_tp_hands_failed_reported = true;
                    log("third-person: hand-only mesh unavailable (hand "
                        "vertices %d/%d); no controller hands",
                        hand_vertices[0], hand_vertices[1]);
                }
                return;
            }

            unsigned char* model_bytes =
                static_cast<unsigned char*>(draw_model);
            for (int hand = 0; hand < 2; ++hand)
            {
                const Mat4 original_wrist =
                    matrix_from_floats(matrices + wrist[hand] * 16);
                Mat4 delta = first_person_hand_delta(
                    hand, original_wrist, controllers[hand]);
                // (A board-only slide back along the palm, board_hand_back,
                // was removed 2026-10-07: it dated from a double-scaled grip
                // offset fixed the same day, and with the hand ruler it was
                // the whole 8 cm difference from classic. One hand tuning
                // now fits every view.)
                static short touched[512];
                static float saved[512][16];
                int touched_count = 0;
                __try
                {
                    for (int segment = 0; segment < total; ++segment)
                    {
                        if (!hand_bone[hand][segment])
                            continue;
                        touched[touched_count] = static_cast<short>(segment);
                        memcpy(saved[touched_count], matrices + segment * 16,
                               sizeof(saved[touched_count]));
                        ++touched_count;
                        const Mat4 moved =
                            matrix_from_floats(matrices + segment * 16) *
                            delta;
                        memcpy(matrices + segment * 16, &moved.m[0][0],
                               sizeof(moved.m));
                    }
                    // Finger debug overrides; else the holding hand pinches.
                    float palm_local[3];
                    if (g_finger_debug_curl)
                    {
                        if (pose_begin(g_pose_work, segments, count,
                                       virtual_count, matrices) &&
                            pose_debug_curl(g_pose_work, wrist[hand], hand))
                            pose_commit(g_pose_work, matrices);
                    }
                    else
                    {
                        float pinch[3];
                        if (g_pluck.hand == hand &&
                            first_person_palm_local(hand, palm_local) &&
                            pose_begin(g_pose_work, segments, count,
                                       virtual_count, matrices) &&
                            pose_pinch(g_pose_work, wrist[hand], palm_local,
                                       hand, pinch))
                        {
                            pose_commit(g_pose_work, matrices);
                            // Drawn at 1/F about the head; the projection
                            // scales by F, so the world pinch is there.
                            const float grow = camera_world_scale_factor();
                            for (int k = 0; k < 3; ++k)
                                g_pluck.pinch[k] = camera_to_world.m[3][k] +
                                    (pinch[k] - camera_to_world.m[3][k]) *
                                    grow;
                            g_pluck.pinch_time = GetTickCount();
                        }
                        else if (hand == 0 && g_pluck.hand != hand &&
                                 g_flick.phase != 0 &&
                                 first_person_palm_local(hand, palm_local) &&
                                 pose_begin(g_pose_work, segments, count,
                                            virtual_count, matrices) &&
                                 pose_flick(g_pose_work, wrist[hand],
                                            palm_local, hand, g_flick.amount,
                                            g_flick.fist, pinch))
                        {
                            pose_commit(g_pose_work, matrices);
                            const float grow = camera_world_scale_factor();
                            const bool fresh = g_flick.tip_time &&
                                GetTickCount() - g_flick.tip_time < 100;
                            for (int k = 0; k < 3; ++k)
                            {
                                g_flick.last_tip[k] = fresh
                                    ? g_flick.tip[k]
                                    : camera_to_world.m[3][k] +
                                      (pinch[k] - camera_to_world.m[3][k]) *
                                      grow;
                                g_flick.tip[k] = camera_to_world.m[3][k] +
                                    (pinch[k] - camera_to_world.m[3][k]) *
                                    grow;
                            }
                            g_flick.tip_time = GetTickCount();
                            g_flick.tip_valid = true;
                        }
                    }
                    // Palm centre and normal for the palm platform. The
                    // palm faces along wrist z (sign by hand from the
                    // pinch curl; board_palm_flip = 1 reverses it).
                    if (first_person_palm_local(hand, palm_local))
                    {
                        if (g_palm_flip < 0)
                        {
                            wchar_t path[MAX_PATH]{};
                            swprintf_s(path, L"%strlvr.ini", exe_dir());
                            g_palm_flip = GetPrivateProfileIntW(
                                L"vr", L"board_palm_flip", 0, path) ? 1 : 0;
                        }
                        const float* W = matrices + wrist[hand] * 16;
                        float n[3] = { W[8], W[9], W[10] };
                        // Sign confirmed in the headset 2026-10-03.
                        const float sign = (hand == 0 ? 1.0f : -1.0f) *
                                           (g_palm_flip ? -1.0f : 1.0f);
                        if (normalise3(n) > 1e-4f)
                        {
                            const float grow = camera_world_scale_factor();
                            PalmSeen& seen = g_palm_seen[hand];
                            for (int k = 0; k < 3; ++k)
                            {
                                const float local = W[12 + k] +
                                    palm_local[0] * W[k] +
                                    palm_local[1] * W[4 + k] +
                                    palm_local[2] * W[8 + k];
                                seen.point[k] = camera_to_world.m[3][k] +
                                    (local - camera_to_world.m[3][k]) * grow;
                                seen.normal[k] = n[k] * sign;
                            }
                            seen.time = GetTickCount();
                        }
                    }
                    *reinterpret_cast<unsigned char**>(
                        model_bytes + kOffModelTextureStrips) =
                            g_tp_hand_strips[hand].data;
                    const int previous_cpu_hand = g_current_hand_cpu_draw;
                    g_current_hand_cpu_draw = hand + 2;
                    g_draw_instance(instance, a2, a3, a4, a5, model, a7,
                                    colour, transform);
                    g_current_hand_cpu_draw = previous_cpu_hand;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
                *reinterpret_cast<unsigned char**>(
                    model_bytes + kOffModelTextureStrips) = strips;
                for (int i = 0; i < touched_count; ++i)
                    memcpy(matrices + touched[i] * 16, saved[i],
                           sizeof(saved[i]));
            }
            if (!g_tp_hands_reported)
            {
                g_tp_hands_reported = true;
                log("third-person: controller hands drawn as copies of "
                    "Lara's hands (triangles left %u, right %u; wrists "
                    "%d/%d)", g_tp_hand_strips[0].triangles,
                    g_tp_hand_strips[1].triangles, wrist[0], wrist[1]);
            }
        }


        // Where Lara hangs: the pinch point from the last hand draw (else the
        // controller), plus the tunable board_hang offset (real metres,
        // along the hand's own axes; numpad with board_pose_debug).
        float g_hang_offset[3]{};
        bool g_hang_loaded = false;

        void hang_load()
        {
            if (g_hang_loaded)
                return;
            g_hang_loaded = true;
            wchar_t path[MAX_PATH]{}, value[32]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            const wchar_t* keys[3] = { L"board_hang_x", L"board_hang_y",
                                       L"board_hang_z" };
            // Defaults: the user's tuning of 2026-10-02.
            const wchar_t* defaults[3] = { L"-0.020", L"-0.005", L"0.005" };
            for (int k = 0; k < 3; ++k)
            {
                GetPrivateProfileStringW(L"vr", keys[k], defaults[k], value,
                                         32, path);
                const float v = (float)_wtof(value);
                g_hang_offset[k] = std::isfinite(v) && fabsf(v) < 0.5f
                    ? v : 0.0f;
            }
        }

        void hang_point(float out[3])
        {
            hang_load();
            const bool pinch = g_pluck.pinch_time &&
                GetTickCount() - g_pluck.pinch_time < 250;
            const float table =
                config().world_scale * camera_world_scale_factor();
            for (int k = 0; k < 3; ++k)
            {
                out[k] = pinch ? g_pluck.pinch[k] : g_pluck.hand_pos[k];
                for (int a = 0; a < 3; ++a)
                    out[k] += g_hang_offset[a] * table *
                              g_pluck.hand_axes[a][k];
            }
        }

        // While Lara is held, she and everything linked to her (parent at
        // Instance+0xB8: holstered guns, gear) are drawn translated by the
        // carry offset; the game's own Lara stays put until release.
        struct CarrySave
        {
            float* matrices = nullptr;
            int total = 0;
            float full[512][16];
        };

        bool carry_apply(void* instance, void* model, CarrySave* save)
        {
            save->matrices = nullptr;
            if ((g_pluck.hand < 0 && g_pluck.palm < 0) ||
                g_first_person_active || !instance)
                return false;
            __try
            {
                void* lara = *reinterpret_cast<void**>(kPlayerInstance);
                unsigned char* inst = static_cast<unsigned char*>(instance);
                if (!lara || (instance != lara &&
                    *reinterpret_cast<void**>(inst + 0xB8) != lara))
                    return false;
                unsigned char* m = static_cast<unsigned char*>(model);
                if (!m)
                {
                    unsigned char* object = *reinterpret_cast<unsigned char**>(
                        inst + kOffInstanceObject);
                    const signed char lod =
                        *reinterpret_cast<const signed char*>(inst + 0xB0);
                    if (!object || lod < 0)
                        return false;
                    m = (*reinterpret_cast<unsigned char***>(
                        object + kOffObjectModelList))[lod];
                }
                float* matrices = *reinterpret_cast<float**>(
                    inst + kOffInstanceMatrices);
                if (!m || !matrices)
                    return false;
                const int total =
                    *reinterpret_cast<const int*>(m + kOffModelNumSegments) +
                    *reinterpret_cast<const int*>(m + kOffModelNumVirtSegments);
                if (total <= 0 || total > 512)
                    return false;
                memcpy(save->full, matrices, sizeof(float) * 16 * total);
                save->matrices = matrices;
                save->total = total;
                // Standing on a palm: her own animation, moved there.
                if (g_pluck.hand < 0)
                {
                    const Mat4 lift = translation(g_pluck.carry[0],
                        g_pluck.carry[1], g_pluck.carry[2]);
                    for (int s = 0; s < total; ++s)
                    {
                        const Mat4 moved =
                            matrix_from_floats(save->full[s]) * lift;
                        memcpy(matrices + s * 16, &moved.m[0][0],
                               sizeof(moved.m));
                    }
                    return true;
                }
                // With a dangle animation she hangs from her higher wrist
                // (chosen once per animation), from this draw's pose.
                if (instance == lara && g_dangle_active)
                {
                    const unsigned char* prop =
                        *reinterpret_cast<unsigned char* const*>(kPlayerProp);
                    const int count_now = *reinterpret_cast<const int*>(
                        m + kOffModelNumSegments);
                    int wrists[2] = { -1, -1 };
                    if (prop)
                    {
                        wrists[0] = *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropLeftWrist);
                        wrists[1] = *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropRightWrist);
                    }
                    if (g_pivot_wrist < 0 && wrists[0] >= 0 &&
                        wrists[1] >= 0 && wrists[0] < count_now &&
                        wrists[1] < count_now)
                    {
                        g_pivot_wrist =
                            matrices[wrists[0] * 16 + 14] >=
                            matrices[wrists[1] * 16 + 14]
                            ? wrists[0] : wrists[1];
                        log("board: dangling from her %s wrist (segment %d)",
                            g_pivot_wrist == wrists[0] ? "left" : "right",
                            g_pivot_wrist);
                    }
                    if (g_pivot_wrist >= 0 && g_pivot_wrist < count_now)
                    {
                        for (int k = 0; k < 3; ++k)
                            g_pivot[k] = matrices[g_pivot_wrist * 16 + 12 + k];
                        g_pivot_valid = true;
                    }
                }
                // Hanging from the grab point: Lara's grab point (root -
                // grab_offset) -- or her wrist with a dangle animation --
                // goes to the hand, tilted by the pendulum.
                float grab[3];
                const bool wrist_pivot = g_dangle_active && g_pivot_valid;
                for (int k = 0; k < 3; ++k)
                    grab[k] = wrist_pivot ? g_pivot[k]
                        : g_pluck.root[k] - g_pluck.grab_offset[k];
                float hang[3];
                hang_point(hang);
                const Mat4 body = translation(-grab[0], -grab[1], -grab[2]) *
                    g_pluck.body_rotation *
                    translation(hang[0], hang[1], hang[2]);
                const int count =
                    *reinterpret_cast<const int*>(m + kOffModelNumSegments);
                const ModelSegment* segments =
                    *reinterpret_cast<ModelSegment* const*>(
                        m + kOffModelSegments);
                if (instance == lara && segments &&
                    pose_begin(g_pose_work, segments, count, total - count,
                               matrices))
                {
                    for (int s = 0; s < count; ++s)
                    {
                        g_pose_work.xform[s] = body;
                        g_pose_work.changed[s] = true;
                    }
                    if (g_hip_model != m)
                    {
                        g_hip_model = m;
                        find_hips(segments, count, g_pose_work.saved);
                    }
                    int limb[4] = { -1, -1, g_hip[0], g_hip[1] };
                    const unsigned char* prop =
                        *reinterpret_cast<unsigned char* const*>(kPlayerProp);
                    if (prop)
                    {
                        limb[0] = *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropLeftShoulder);
                        limb[1] = *reinterpret_cast<const int*>(
                            prop + kOffPlayerPropRightShoulder);
                    }
                    for (int l = 0; l < 4; ++l)
                    {
                        if (limb[l] < 0 || limb[l] >= count)
                            continue;
                        float pivot[3];
                        pose_joint(g_pose_work, limb[l], pivot);
                        pose_rotate_subtree(g_pose_work, limb[l], pivot,
                                            g_pluck.limb_rotation[l]);
                    }
                    pose_commit(g_pose_work, matrices);
                }
                else
                {
                    for (int s = 0; s < total; ++s)
                    {
                        const Mat4 moved =
                            matrix_from_floats(save->full[s]) * body;
                        memcpy(matrices + s * 16, &moved.m[0][0],
                               sizeof(moved.m));
                    }
                }
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                save->matrices = nullptr;
                return false;
            }
        }

        void carry_restore(CarrySave* save)
        {
            if (!save->matrices)
                return;
            memcpy(save->matrices, save->full,
                   sizeof(float) * 16 * save->total);
            save->matrices = nullptr;
        }

        // Lara's own model is hidden in first person. The two hand-only strip
        // lists retain source material settings with non-hand indices collapsed.
        // Per-instance CPU cost of DRAW_DrawInstance (2026-10-05: about
        // 15 ms per frame for 70-105 instances in the water area, nearly the
        // same for 67 as for 105, so a few instances dominate). Reported
        // with the 5 s perf line: the costliest instances by name, with the
        // angle between the view and the direction to them (over 90 =
        // behind the player; first person draws those too, its object
        // culling being open).
        struct InstanceCost
        {
            const void* instance;
            long long ticks;
            unsigned calls;
            float angle_sum;
        };
        InstanceCost g_instance_costs[256];
        int g_instance_cost_count = 0;
        bool g_instance_cost_full_reported = false;

        float instance_view_angle(const void* instance)
        {
            __try
            {
                const float* pos = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(instance) + 0x10);
                const float* cw = reinterpret_cast<const float*>(
                    kMainCamera) + kOffCwTransform / sizeof(float);
                const float d[3] = { pos[0] - cw[12], pos[1] - cw[13],
                                     pos[2] - cw[14] };
                const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] +
                                       d[2] * d[2]);
                const float fl = sqrtf(cw[8] * cw[8] + cw[9] * cw[9] +
                                       cw[10] * cw[10]);
                if (!(dl > 1.0f) || !(fl > 1e-4f))
                    return 0.0f;
                const float cosine = (d[0] * cw[8] + d[1] * cw[9] +
                                      d[2] * cw[10]) / (dl * fl);
                return acosf(fmaxf(-1.0f, fminf(1.0f, cosine))) *
                       57.2957795f;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return 0.0f;
            }
        }

        void instance_cost_note(const void* instance, long long ticks)
        {
            if (!instance)
                return;
            for (int i = 0; i < g_instance_cost_count; ++i)
                if (g_instance_costs[i].instance == instance)
                {
                    g_instance_costs[i].ticks += ticks;
                    ++g_instance_costs[i].calls;
                    g_instance_costs[i].angle_sum +=
                        instance_view_angle(instance);
                    return;
                }
            if (g_instance_cost_count < 256)
                g_instance_costs[g_instance_cost_count++] = {
                    instance, ticks, 1, instance_view_angle(instance) };
        }

        const char* instance_name(const void* instance)
        {
            __try
            {
                const unsigned char* object =
                    *reinterpret_cast<const unsigned char* const*>(
                        static_cast<const unsigned char*>(instance) +
                        kOffInstanceObject);
                const char* name = object
                    ? *reinterpret_cast<const char* const*>(
                          object + kOffObjectName) : nullptr;
                return name && !IsBadStringPtrA(name, 64) ? name : "?";
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return "?";
            }
        }

        void __cdecl detour_draw_instance_body(void* instance, int a2, float a3, int a4, int a5, void* model, float a7, void* colour, void* transform);
        void __cdecl detour_draw_instance(void* instance, int a2, float a3, int a4, int a5, void* model, float a7, void* colour, void* transform)
        {
            // Lara (first-person hand/body passes) apart from the rest.
            const PerfBucket bucket = instance && instance == g_first_person_instance
                ? PerfDrawLara : PerfDrawInstance;
            LARGE_INTEGER started{}, ended{};
            QueryPerformanceCounter(&started);
            perf_cpu_begin(bucket);
            detour_draw_instance_body(instance, a2, a3, a4, a5, model, a7, colour, transform);
            perf_cpu_end(bucket);
            QueryPerformanceCounter(&ended);
            instance_cost_note(instance, ended.QuadPart - started.QuadPart);
        }

        void __cdecl detour_draw_instance_body(void* instance, int a2, float a3,
                                           int a4, int a5, void* model,
                                           float a7, void* colour,
                                           void* transform)
        {
            if (!g_draw_instance)
                return;
            if (!g_first_person_active ||
                instance != g_first_person_instance)
            {
                if (g_first_person_active &&
                    first_person_draw_linked_weapon(instance, a2, a3, a4,
                        a5, model, a7, colour, transform))
                    return;
                if (g_first_person_active)
                    grip_prompt_follow_hand(instance, model);
                static CarrySave carry;
                const bool carried = carry_apply(instance, model, &carry);
                g_draw_instance(instance, a2, a3, a4, a5, model, a7,
                                colour, transform);
                if (carried)
                    carry_restore(&carry);
                if (!g_first_person_active)
                    third_person_draw_hands(instance, a2, a3, a4, a5, model,
                                            a7, colour, transform);
                return;
            }
            // Other Lara model variants can share the same instance. They
            // must not bring a second animated head into the headset view.
            if (model && model != g_first_person_model)
                return;

            Mat4 controllers[2];
            if (!first_person_controller_world_pose(true, &controllers[0]) ||
                !first_person_controller_world_pose(false, &controllers[1]))
            {
                for (LedgeHandGrip& grip : g_ledge_hand_grip)
                    grip.in_grab_zone = false;
                g_grab_zone_debug.valid = false;
                return; // No animated body/head while tracking is lost.
            }

            void* draw_model = model ? model : g_first_person_model;
            ModelSegment* segments = nullptr;
            unsigned char* vertices = nullptr;
            unsigned char* strips = nullptr;
            float* matrices = nullptr;
            int count = 0, virtual_count = 0, vertex_count = 0;
            bool valid = false;
            __try
            {
                unsigned char* m = static_cast<unsigned char*>(draw_model);
                count = *reinterpret_cast<int*>(m + kOffModelNumSegments);
                virtual_count = *reinterpret_cast<int*>(
                    m + kOffModelNumVirtSegments);
                segments = *reinterpret_cast<ModelSegment**>(
                    m + kOffModelSegments);
                vertex_count = *reinterpret_cast<int*>(
                    m + kOffModelNumVertices);
                vertices = *reinterpret_cast<unsigned char**>(
                    m + kOffModelVertices);
                strips = *reinterpret_cast<unsigned char**>(
                    m + kOffModelTextureStrips);
                matrices = *reinterpret_cast<float**>(
                    static_cast<unsigned char*>(instance) +
                    kOffInstanceMatrices);
                valid = segments && vertices && strips && matrices &&
                    count > 0 && virtual_count >= 0 &&
                    count + virtual_count <= 512 &&
                    vertex_count > 0 && vertex_count <= 65535;
                for (int hand = 0; hand < 2 && valid; ++hand)
                    valid = g_first_person_wrist_segment[hand] >= 0 &&
                        g_first_person_wrist_segment[hand] < count;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                valid = false;
            }
            if (!valid)
                return;

            bool hand_bone[2][512]{};
            for (int bone = 0; bone < count; ++bone)
                for (int hand = 0; hand < 2; ++hand)
                    hand_bone[hand][bone] = segment_is_in_head_subtree(
                        segments, count, bone,
                        g_first_person_wrist_segment[hand]);
            // A virtual segment joins the hand when either blended bone is
            // in its wrist subtree: knuckles blend two finger bones, the
            // glove cuff blends wrist and forearm. Its matrix then receives
            // the same rigid delta as the real bones, so the blend is kept.
            const int total = count + virtual_count;
            int virtual_hand[2] = { 0, 0 };
            __try
            {
                const ModelVirtualSegment* virtuals =
                    reinterpret_cast<const ModelVirtualSegment*>(
                        segments + count);
                for (int v = 0; v < virtual_count; ++v)
                {
                    const int a = virtuals[v].index;
                    const int b = virtuals[v].weight_index;
                    for (int hand = 0; hand < 2; ++hand)
                    {
                        const bool in_hand =
                            (a >= 0 && a < count && hand_bone[hand][a]) ||
                            (b >= 0 && b < count && hand_bone[hand][b]);
                        hand_bone[hand][count + v] = in_hand;
                        virtual_hand[hand] += in_hand ? 1 : 0;
                    }
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }

            int hand_vertices[2] = { 0, 0 };
            __try
            {
                for (int i = 0; i < vertex_count; ++i)
                {
                    const int bone = *reinterpret_cast<const short*>(
                        vertices + i * 16 + 10);
                    if (bone < 0 || bone >= total)
                        continue;
                    for (int hand = 0; hand < 2; ++hand)
                        if (hand_bone[hand][bone])
                            ++hand_vertices[hand];
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }

            uint32_t draw_flags = 0;
            memcpy(&draw_flags, &a7, sizeof(draw_flags));
            const bool triangle_list = (draw_flags & 1u) != 0;
            if (g_first_person_strip_source != strips ||
                g_first_person_strip_tri_list != triangle_list)
            {
                for (HandStripCache& cache : g_first_person_hand_strips)
                {
                    if (cache.data)
                        HeapFree(GetProcessHeap(), 0, cache.data);
                    cache = HandStripCache{};
                }
                if (g_first_person_body_strips.data)
                    HeapFree(GetProcessHeap(), 0,
                             g_first_person_body_strips.data);
                g_first_person_body_strips = HandStripCache{};
                g_first_person_strip_source = strips;
                g_first_person_strip_tri_list = triangle_list;
            }
            if (!g_first_person_hand_strips[0].data &&
                hand_vertices[0] >= 12 && hand_vertices[1] >= 12)
            {
                valid = build_hand_strips(
                    strips, vertices, vertex_count, total,
                    hand_bone[0], triangle_list,
                    &g_first_person_hand_strips[0]);
                valid = valid && build_hand_strips(
                    strips, vertices, vertex_count, total,
                    hand_bone[1], triangle_list,
                    &g_first_person_hand_strips[1]);
            }
            if (!valid || !g_first_person_hand_strips[0].data ||
                !g_first_person_hand_strips[1].data)
            {
                if (!g_first_person_hand_unavailable_reported)
                {
                    g_first_person_hand_unavailable_reported = true;
                    log("first-person: remapped hand-strip isolation unavailable "
                        "(vertices %d/%d, flags 0x%08X); local model hidden",
                        hand_vertices[0], hand_vertices[1], draw_flags);
                }
                return;
            }

            uint32_t* normal_mask = reinterpret_cast<uint32_t*>(
                static_cast<unsigned char*>(instance) + kOffNoDrawGroups);
            uint32_t* fx_mask = reinterpret_cast<uint32_t*>(
                static_cast<unsigned char*>(instance) + kOffFxNoDrawGroups);
            const uint32_t old_normal = *normal_mask;
            const uint32_t old_fx = *fx_mask;
            // The old head draw-group mask was necessarily broad. Clear only
            // our bits for the isolated hand passes; index filtering now
            // excludes every head vertex regardless of draw group.
            *normal_mask = (old_normal & ~g_first_person_head_mask) |
                           g_first_person_saved_normal;
            *fx_mask = (old_fx & ~g_first_person_head_mask) |
                       g_first_person_saved_fx;

            // A single box spans both animated wrists. Either controller
            // can enter anywhere in it, while each hand keeps its own grip
            // latch and anchored pose for one- or two-handed climbing.
            const float scale = tune_world_scale();
            const float height = (g_first_person_vine_climbing ||
                                  g_first_person_bar_hanging)
                ? 0.0f : tune_ledge_grab_height();
            const Mat4 left_wrist = matrix_from_floats(
                matrices + g_first_person_wrist_segment[0] * 16);
            const Mat4 right_wrist = matrix_from_floats(
                matrices + g_first_person_wrist_segment[1] * 16);
            HandholdBox box{};
            box.valid = config().immersive_controls && scale > 0.0f &&
                (g_first_person_ledge_hanging ||
                 g_first_person_bar_hanging ||
                 g_first_person_vine_climbing);
            for (int axis = 0; axis < 3; ++axis)
            {
                box.centre[axis] = 0.5f *
                    (left_wrist.m[3][axis] + right_wrist.m[3][axis]) +
                    (axis == 2 ? height * scale : 0.0f);
                box.valid = box.valid && std::isfinite(box.centre[axis]);
            }
            float along_x = right_wrist.m[3][0] - left_wrist.m[3][0];
            float along_y = right_wrist.m[3][1] - left_wrist.m[3][1];
            float span = sqrtf(along_x * along_x + along_y * along_y);
            box.valid = box.valid && std::isfinite(span);
            if (g_first_person_bar_hanging &&
                !g_first_person_bar_axis_valid &&
                !g_first_person_grip_precarious &&
                g_first_person_bar_entered_at &&
                GetTickCount() - g_first_person_bar_entered_at >= 500 &&
                span >= 0.12f * scale && span <= 1.2f * scale)
            {
                // The wrist span follows Lara's animation when she rotates
                // into the traverse pose. Capture its bar-aligned direction
                // at attachment so the shared reach zone stays on the pole.
                g_first_person_bar_axis[0] = along_x / span;
                g_first_person_bar_axis[1] = along_y / span;
                g_first_person_bar_span = span;
                g_first_person_bar_axis_valid = true;
                log("first-person: swing bar axis captured (%.2f, %.2f), "
                    "wrist span %.2f m",
                    g_first_person_bar_axis[0],
                    g_first_person_bar_axis[1], span / scale);
            }
            if (g_first_person_bar_hanging &&
                g_first_person_bar_axis_valid)
            {
                along_x = g_first_person_bar_axis[0];
                along_y = g_first_person_bar_axis[1];
                span = g_first_person_bar_span;
            }
            if (span < 0.08f * scale || span > 1.2f * scale)
            {
                const float* camera_basis =
                    reinterpret_cast<const float*>(kMainCamera) +
                    kOffCwTransform / sizeof(float);
                along_x = camera_basis[0];
                along_y = camera_basis[1];
                span = 0.3f * scale;
            }
            const float basis_length = sqrtf(along_x * along_x +
                                             along_y * along_y);
            box.valid = box.valid && std::isfinite(basis_length) &&
                basis_length > 0.0001f;
            if (box.valid)
            {
                box.along[0] = along_x / basis_length;
                box.along[1] = along_y / basis_length;
                g_first_person_hold_axis[0] = box.along[0];
                g_first_person_hold_axis[1] = box.along[1];
                g_first_person_hold_axis_valid =
                    g_first_person_ledge_hanging || g_first_person_bar_hanging;
                box.depth[0] = -box.along[1];
                box.depth[1] = box.along[0];
                if (g_first_person_vine_climbing)
                {
                    // A vine, chain or ladder is a vertical handhold: one
                    // shared box around the animated wrists, tall enough for
                    // either controller to reach well above or below the
                    // hands. 2026-10-05 (user: "expand the height much
                    // more"; log: misses at 0.18-0.29 m off the hold with
                    // 0.17 allowed): 0.40 wide, 1.10 up/down, 0.40 deep,
                    // centred on the wrists (a chain can be held from any
                    // side), with no ledge lip offset.
                    box.half_along = 0.40f * scale;
                    box.half_up = 1.10f * scale;
                }
                else if (g_first_person_bar_hanging)
                {
                    // Centre the shared reach box on the animated bar grip.
                    // Unlike a ledge there is no forward lip or vertical
                    // offset; either hand can grab along its full width.
                    // 2.1 (was 1.5): the user reaches comfortably wider.
                    box.half_along = 2.1f *
                        (0.5f * span + 0.35f * scale);
                    box.half_up = 0.20f * scale;
                }
                else
                {
                    // Tested wide, low ledge box; width 2.6 (2026-10-05,
                    // was 2.1, before that 1.5) because the user reaches
                    // comfortably wider.
                    box.half_along = 2.6f *
                        (0.5f * span + 0.35f * scale);
                    box.half_up = 0.175f * scale;
                    box.centre[2] -= 0.175f * scale;
                }

                // Keep the face beyond the ledge where it was and remove
                // depth from the face nearest Lara. Her actor origin gives
                // a stable side even when the HMD leans across the box.
                const float* lara = static_cast<const float*>(instance);
                float lara_x = lara[4], lara_y = lara[5];
                if (!std::isfinite(lara_x) || !std::isfinite(lara_y))
                {
                    const float* camera =
                        reinterpret_cast<const float*>(kMainCamera) +
                        kOffCwTransform / sizeof(float);
                    lara_x = camera[12];
                    lara_y = camera[13];
                }
                const float lara_side =
                    (lara_x - box.centre[0]) * box.depth[0] +
                    (lara_y - box.centre[1]) * box.depth[1];
                box.valid = box.valid && std::isfinite(lara_side);
                const float near_sign = lara_side >= 0.0f ? 1.0f : -1.0f;
                const bool ledge_box = !g_first_person_bar_hanging &&
                                       !g_first_person_vine_climbing;
                if (ledge_box)
                {
                    box.centre[0] -= near_sign * 0.175f * scale * box.depth[0];
                    box.centre[1] -= near_sign * 0.175f * scale * box.depth[1];
                }
                // Ledge depth 0.27 (2026-10-05, was 0.175): most ledge
                // misses in the user's level log were 0.19-0.27 m deep.
                box.half_depth = g_first_person_vine_climbing ? 0.40f * scale
                    : g_first_person_bar_hanging ? 0.20f * scale
                                                 : 0.27f * scale;
                // Slide the entire new box toward Lara by 10% of its full
                // depth: remove that amount at the far face and restore it
                // at the near face, without changing the box's thickness.
                const float toward_lara = ledge_box
                    ? 0.10f * 2.0f * box.half_depth : 0.0f;
                box.centre[0] += near_sign * toward_lara * box.depth[0];
                box.centre[1] += near_sign * toward_lara * box.depth[1];
            }
            g_grab_zone_debug.valid = false;
            g_grab_zone_centre_valid = false;
            Mat4 world_to_head = Mat4::identity();
            // A one-hand catch on a swing bar (user, 2026-10-05: spinning
            // bars): the free wrist dangles, so a box between the wrists
            // sat below the bar and, with the levelled view, moved around
            // as Lara swung. Until the first grab the zone is fixed to the
            // player's head instead: above it, a little forward, generous.
            const bool head_fixed_catch = box.valid &&
                g_first_person_bar_hanging &&
                g_first_person_grip_precarious &&
                !g_ledge_hand_grip[0].held && !g_ledge_hand_grip[1].held;
            // Head frame: X right, Y down, Z forward (game units).
            const float catch_centre[3] = {
                0.0f, -0.32f * scale, 0.12f * scale };
            const float catch_half[3] = {
                0.45f * scale, 0.32f * scale, 0.38f * scale };
            if (box.valid)
            {
                Mat4 camera_to_world;
                memcpy(&camera_to_world.m[0][0],
                       static_cast<const unsigned char*>(kMainCamera) +
                           kOffCwTransform, sizeof(camera_to_world.m));
                world_to_head = rigid_inverse(camera_to_world);
                g_grab_zone_centre_valid = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    float head = world_to_head.m[3][axis];
                    for (int row = 0; row < 3; ++row)
                        head += box.centre[row] * world_to_head.m[row][axis];
                    g_grab_zone_centre[axis] = head;
                    g_grab_zone_centre_valid =
                        g_grab_zone_centre_valid && std::isfinite(head);
                }
                if (head_fixed_catch)
                    memcpy(g_grab_zone_centre, catch_centre,
                           sizeof(catch_centre));
            }
            // A controller's position in the head frame (game units).
            const auto head_point = [&](const Mat4& world, float out[3]) {
                for (int axis = 0; axis < 3; ++axis)
                {
                    out[axis] = world_to_head.m[3][axis];
                    for (int row = 0; row < 3; ++row)
                        out[axis] += world.m[3][row] *
                                     world_to_head.m[row][axis];
                }
            };
            const auto in_catch_zone = [&](const Mat4& world, float margin,
                                           float components[3]) {
                float p[3];
                head_point(world, p);
                bool inside = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    components[axis] = p[axis] - catch_centre[axis];
                    inside = inside && std::isfinite(components[axis]) &&
                        fabsf(components[axis]) <= catch_half[axis] + margin;
                }
                return inside;
            };
            if (box.valid && tune_ledge_grab_debug_draw() && head_fixed_catch)
            {
                g_grab_zone_debug.valid = true;
                for (int corner = 0; corner < 8; ++corner)
                {
                    g_grab_zone_debug.corners[corner][0] = catch_centre[0] +
                        ((corner & 1) ? 1.0f : -1.0f) * catch_half[0];
                    g_grab_zone_debug.corners[corner][1] = catch_centre[1] +
                        ((corner & 4) ? 1.0f : -1.0f) * catch_half[1];
                    g_grab_zone_debug.corners[corner][2] = catch_centre[2] +
                        ((corner & 2) ? 1.0f : -1.0f) * catch_half[2];
                }
            }
            else if (box.valid && tune_ledge_grab_debug_draw())
            {
                g_grab_zone_debug.valid = true;
                for (int corner = 0; corner < 8; ++corner)
                {
                    const float a = (corner & 1) ? 1.0f : -1.0f;
                    const float d = (corner & 2) ? 1.0f : -1.0f;
                    const float u = (corner & 4) ? 1.0f : -1.0f;
                    const float world[3] = {
                        box.centre[0] + a * box.half_along * box.along[0] +
                            d * box.half_depth * box.depth[0],
                        box.centre[1] + a * box.half_along * box.along[1] +
                            d * box.half_depth * box.depth[1],
                        box.centre[2] + u * box.half_up
                    };
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        float head = world_to_head.m[3][axis];
                        for (int row = 0; row < 3; ++row)
                            head += world[row] * world_to_head.m[row][axis];
                        g_grab_zone_debug.corners[corner][axis] = head;
                        g_grab_zone_debug.valid =
                            g_grab_zone_debug.valid && std::isfinite(head);
                    }
                }
            }

            const bool full_body = config().first_person_full_body &&
                first_person_body_ready(segments, count, virtual_count,
                                        strips, vertices, vertex_count,
                                        triangle_list);
            Mat4 body_hand_delta[2] = { Mat4::identity(), Mat4::identity() };
            for (int hand = 0; hand < 2; ++hand)
            {
                const int wrist = g_first_person_wrist_segment[hand];
                const Mat4 original_wrist =
                    matrix_from_floats(matrices + wrist * 16);
                g_hand_cpu_vertex_buffer = nullptr;
                g_hand_cpu_vertex_expected_count =
                    static_cast<unsigned>(vertex_count);
                LedgeHandGrip& grip = g_ledge_hand_grip[hand];
                float components[3]{};
                const bool inside_grip = head_fixed_catch
                    ? in_catch_zone(controllers[hand], 0.0f, components)
                    : handhold_box_contains(
                          box, controllers[hand], 0.0f, components);
                const bool inside = head_fixed_catch
                    ? in_catch_zone(controllers[hand],
                                    grip.in_grab_zone ? 0.07f * scale : 0.0f,
                                    components)
                    : handhold_box_contains(
                          box, controllers[hand],
                          grip.in_grab_zone ? 0.07f * scale : 0.0f,
                          components);
                if (inside && !grip.in_grab_zone && !grip.held)
                    vr_input_handhold_haptic(hand == 0);
                grip.in_grab_zone = inside;
                if (grip.pending)
                {
                    // Capture the unmodified controller pose. The shared
                    // reach box only decides eligibility; each hand still
                    // anchors independently at its own tracked position.
                    grip.pending = false;
                    if (GetTickCount() - grip.pressed_at <= 250 &&
                        inside_grip)
                    {
                        grip.world_pose = controllers[hand];
                        grip.tangent[0] = box.along[0];
                        grip.tangent[1] = box.along[1];
                        grip.held = true;
                        grip.grabbed_at = GetTickCount();
                        grip.serial = ++g_ledge_grip_serial;
                        if (!grip.serial)
                            grip.serial = ++g_ledge_grip_serial;
                        if (g_first_person_grip_precarious)
                        {
                            g_first_person_secure_requested_at =
                                GetTickCount();
                            log("first-person: grab secures the precarious "
                                "hold (Action pulse)");
                        }
                        log("first-person: %s hand gripped %s "
                            "(shared box, along %+.2f, depth %+.2f, "
                            "vertical %+.2f m)",
                            hand == 0 ? "left" : "right",
                            g_first_person_vine_climbing ? "vertical hold" :
                            g_first_person_bar_hanging ? "swing bar" : "ledge",
                            components[0] / scale,
                            components[1] / scale,
                            components[2] / scale);
                    }
                    else if (head_fixed_catch && grip.misses_reported++ < 12)
                        log("first-person: %s catch grab missed (head-fixed "
                            "zone right %+.2f/%.2f, down %+.2f/%.2f, forward "
                            "%+.2f/%.2f m)", hand == 0 ? "left" : "right",
                            components[0] / scale, catch_half[0] / scale,
                            components[1] / scale, catch_half[1] / scale,
                            components[2] / scale, catch_half[2] / scale);
                    else if (grip.misses_reported++ < 12)
                        log("first-person: %s handhold grip missed "
                            "(shared box along %+.2f/%.2f, depth "
                            "%+.2f/%.2f, vertical %+.2f/%.2f m)",
                            hand == 0 ? "left" : "right",
                            scale > 0.0f ? components[0] / scale : 0.0f,
                            scale > 0.0f ? box.half_along / scale : 0.0f,
                            scale > 0.0f ? components[1] / scale : 0.0f,
                            scale > 0.0f ? box.half_depth / scale : 0.0f,
                            scale > 0.0f ? components[2] / scale : 0.0f,
                            scale > 0.0f ? box.half_up / scale : 0.0f);
                }
                const Mat4 delta = first_person_hand_delta(
                    hand, original_wrist,
                    grip.held ? grip.world_pose : controllers[hand]);
                body_hand_delta[hand] = delta;
                if (full_body)
                    continue; // drawn with the body below

                unsigned char* model_bytes =
                    static_cast<unsigned char*>(draw_model);
                *reinterpret_cast<unsigned char**>(
                    model_bytes + kOffModelTextureStrips) =
                        g_first_person_hand_strips[hand].data;

                short touched[512]{};
                float saved[512][16]{};
                int touched_count = 0;
                for (int segment = 0; segment < total; ++segment)
                {
                    if (!hand_bone[hand][segment])
                        continue;
                    touched[touched_count] = static_cast<short>(segment);
                    memcpy(saved[touched_count], matrices + segment * 16,
                           sizeof(saved[touched_count]));
                    ++touched_count;
                    const Mat4 moved =
                        matrix_from_floats(matrices + segment * 16) * delta;
                    memcpy(matrices + segment * 16, &moved.m[0][0],
                           sizeof(moved.m));
                }

                const uint64_t gpu_before = g_first_person_gpu_draw_count;
                const int previous_cpu_hand = g_current_hand_cpu_draw;
                g_current_hand_cpu_draw = hand;
                g_draw_instance(instance, a2, a3, a4, a5, model, a7,
                                colour, transform);
                g_current_hand_cpu_draw = previous_cpu_hand;
                report_hand_cpu_vertex_bounds(
                    hand, vertices, vertex_count, hand_bone[hand], total,
                    original_wrist);
                g_hand_cpu_vertex_buffer = nullptr;
                g_hand_cpu_vertex_expected_count = 0;
                const uint64_t gpu_inside =
                    g_first_person_gpu_draw_count - gpu_before;
                const unsigned mark = tune_ledge_visibility_marker();
                if (g_first_person_ledge_hanging &&
                    mark != g_last_ledge_visibility_mark[hand])
                {
                    g_last_ledge_visibility_mark[hand] = mark;
                    const float* eye = reinterpret_cast<const float*>(
                        kMainCamera) + kOffCwTransform / sizeof(float);
                    float relative[3]{};
                    for (int axis = 0; axis < 3; ++axis)
                        relative[axis] = scale > 0.0f
                            ? (controllers[hand].m[3][axis] -
                               original_wrist.m[3][axis]) / scale : 0.0f;
                    const float dx = controllers[hand].m[3][0] - eye[12];
                    const float dy = controllers[hand].m[3][1] - eye[13];
                    const float dz = controllers[hand].m[3][2] - eye[14];
                    const float eye_distance = scale > 0.0f
                        ? sqrtf(dx * dx + dy * dy + dz * dz) / scale : 0.0f;
                    float view[3]{};
                    if (scale > 0.0f)
                        for (int axis = 0; axis < 3; ++axis)
                            view[axis] = (dx * eye[axis * 4] +
                                          dy * eye[axis * 4 + 1] +
                                          dz * eye[axis * 4 + 2]) / scale;
                    log("render: ledge visibility sample %u %s hand: "
                        "held=%d controller-wrist=(%+.2f,%+.2f,%+.2f)m "
                        "eye=(%+.2f,%+.2f,%+.2f)m distance=%.2fm "
                        "GPU draws total=%llu inside hand CPU pass=%llu",
                        mark, hand == 0 ? "left" : "right",
                        grip.held ? 1 : 0, relative[0], relative[1],
                        relative[2], view[0], view[1], view[2],
                        eye_distance,
                        static_cast<unsigned long long>(
                            g_first_person_gpu_draw_count),
                        static_cast<unsigned long long>(gpu_inside));
                }

                for (int i = 0; i < touched_count; ++i)
                    memcpy(matrices + touched[i] * 16, saved[i],
                           sizeof(saved[i]));
                *reinterpret_cast<unsigned char**>(
                    static_cast<unsigned char*>(draw_model) +
                    kOffModelTextureStrips) = strips;
            }

            if (full_body)
                first_person_draw_full_body(
                    instance, a2, a3, a4, a5, model, a7, colour, transform,
                    draw_model, strips, segments, count, virtual_count,
                    matrices, body_hand_delta);

            *normal_mask = old_normal;
            *fx_mask = old_fx;
            if (!g_first_person_hands_reported)
            {
                g_first_person_hands_reported = true;
                log("first-person: local body/head suppressed; whole-triangle "
                    "hand lists left %u nodes/%u triangles, right %u/%u "
                    "(vertices %d/%d, virtual joint segments %d/%d of %d, "
                    "draw flags 0x%08X)",
                    g_first_person_hand_strips[0].nodes,
                    g_first_person_hand_strips[0].triangles,
                    g_first_person_hand_strips[1].nodes,
                    g_first_person_hand_strips[1].triangles,
                    hand_vertices[0], hand_vertices[1],
                    virtual_hand[0], virtual_hand[1], virtual_count,
                    draw_flags);
            }
        }

        // Used by the first-person heading calibration below. Definitions
        // remain beside the head-pose composition they support.
        Mat4 load_matrix(const void* cam, unsigned off);
        void store_matrix(void* cam, unsigned off, const Mat4& m);

        void set_view_position(Mat4* view, const float* position)
        {
            // Row-vector world-to-camera transform:
            // translation = -camera_position * rotation.
            for (int column = 0; column < 3; ++column)
            {
                view->m[3][column] =
                    -(position[0] * view->m[0][column] +
                      position[1] * view->m[1][column] +
                      position[2] * view->m[2][column]);
            }
            view->m[3][3] = 1.0f;
        }

        void set_camera_position(void* cam, const float* position)
        {
            if (!cam || !position)
                return;
            memcpy((unsigned char*)cam + kOffPosition,
                   position, sizeof(float) * 3);
        }

        float rotation_error(const Mat4& a, const Mat4& b)
        {
            float error = 0.0f;
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                {
                    const float d = a.m[row][column] - b.m[row][column];
                    error += d * d;
                }
            return error;
        }

        float wrap_heading(float heading)
        {
            const float two_pi = 6.28318531f;
            while (heading < 0.0f) heading += two_pi;
            while (heading >= two_pi) heading -= two_pi;
            return heading;
        }

        // Lara's retail combat state, [[[player]+0x23C]+4]+4: zero outside
        // combat, 0x20 in accurate aim. The same value gates the retail
        // reticle in playerDrawCombatLock.
        int retail_combat_state()
        {
            int state = 0;
            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(kPlayerInstance);
                const unsigned char* a =
                    *reinterpret_cast<unsigned char* const*>(player + 0x23C);
                const unsigned char* b =
                    *reinterpret_cast<unsigned char* const*>(a + 4);
                state = *reinterpret_cast<const int*>(b + 4);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                state = 0;
            }
            return state;
        }

        // In combat retail locomotion owns Lara's facing: ProcessMovement
        // steering mode 4 turns her toward the lock target (player+0x7E0),
        // mode 13 toward the stick bearing snapped to quadrants, and the
        // combat states pick strafe/backstep from the stick relative to her.
        // Forcing her yaw to the travel direction fought that (back travel
        // measured 12 units per 500 ms against 437 forward). Her facing is
        // invisible in first person -- guns follow the controllers and the
        // aim ray is ours -- so combat is left entirely to retail.
        bool first_person_combat_locomotion()
        {
            return vr_input_holster_combat_held() ||
                   retail_combat_state() != 0;
        }

        // The VR view heading in the CAMERA Euler convention, which is what
        // Camera::lagZ holds: at first-person entry lagZ read 180.0 with the
        // camera heading at 180.0 and Lara at 0.0, and retail then produced
        // zDir = lagZ + stick direction + 180 deg on every verified call.
        // (Feeding the actor-convention heading reversed all directions.)
        bool first_person_view_camera_heading(float* heading)
        {
            if (!g_first_person_turn_ready)
                return false;
            const float stick_turn =
                vr_input_first_person_turn() - g_first_person_stick_start;
            const float view = wrap_heading(
                g_first_person_base_heading +
                g_first_person_turn_sign * stick_turn -
                g_first_person_turn_sign * first_person_hmd_heading());
            if (!std::isfinite(view))
                return false;
            *heading = view;
            return true;
        }

        int __cdecl detour_process_movement_body(void* instance);
        int __cdecl detour_process_movement(void* instance)
        {
            perf_cpu_begin(PerfMovement);
            const auto result = detour_process_movement_body(instance);
            perf_cpu_end(PerfMovement);
            return result;
        }

        int __cdecl detour_process_movement_body(void* instance)
        {
            if (g_first_person_active && instance == g_first_person_instance)
            {
                g_first_person_move_driven = false;
                const bool combat = first_person_combat_locomotion();
                if (combat != g_first_person_combat_locomotion)
                {
                    g_first_person_combat_locomotion = combat;
                    log("first-person: %s locomotion (%s)",
                        combat ? "native retail combat" : "VR ground travel",
                        combat ? "Lara's facing and strafe left to the game; "
                                 "camera-relative input follows the view"
                               : "instant turn to stick direction");
                }
                float right = 0.0f, forward = 0.0f;
                if (!combat &&
                    g_first_person_traversal == TraversalGround &&
                    g_first_person_turn_ready &&
                    g_first_person_actor_offset_valid &&
                    g_first_person_actor_offset_model == g_first_person_model &&
                    vr_input_first_person_move(&right, &forward))
                {
                    __try
                    {
                        unsigned char* root =
                            *reinterpret_cast<unsigned char**>(kFilteredInputRoot);
                        unsigned char* player =
                            *reinterpret_cast<unsigned char**>(0x0111713C);
                        const int control_mode = root
                            ? int(*reinterpret_cast<signed char*>(
                                root + 0x60 + 0x70)) : -1;
                        const int steering = player
                            ? *reinterpret_cast<int*>(player + 0x74) : -1;
                        if (control_mode == 3 &&
                            (steering == 0 || steering == 1))
                        {
                            const float stick_turn =
                                vr_input_first_person_turn() -
                                g_first_person_stick_start;
                            const float view_heading = wrap_heading(
                                g_first_person_base_heading +
                                g_first_person_turn_sign * stick_turn -
                                g_first_person_turn_sign *
                                first_person_hmd_heading());
                            const float move_offset = atan2f(-right, forward);
                            const float target = wrap_heading(
                                view_heading +
                                g_first_person_actor_camera_offset +
                                move_offset);
                            if (std::isfinite(target))
                            {
                                *reinterpret_cast<float*>(
                                    static_cast<unsigned char*>(instance) +
                                    kOffInstanceHeading) = target;
                                g_first_person_move_heading = target;
                                g_first_person_move_driven = true;
                            }
                        }
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        g_first_person_move_driven = false;
                    }
                }
            }

            const int result = g_process_movement(instance);

            if (g_first_person_move_driven &&
                instance == g_first_person_instance)
            {
                // Retail steering can rewrite yaw during ProcessMovement.
                // Keep the same player-requested travel direction for root
                // motion later in the frame, without touching position or
                // calling the physics rotation setter.
                __try
                {
                    *reinterpret_cast<float*>(
                        static_cast<unsigned char*>(instance) +
                        kOffInstanceHeading) = g_first_person_move_heading;
                    const DWORD now = GetTickCount();
                    if (g_first_person_move_reports < 30 &&
                        (g_first_person_move_report_time == 0 ||
                         now - g_first_person_move_report_time >= 2000))
                    {
                        g_first_person_move_report_time = now;
                        ++g_first_person_move_reports;
                        log("first-person: VR travel target %.1f deg; "
                            "Lara yaw %.1f deg after movement",
                            g_first_person_move_heading * 57.2957795f,
                            *reinterpret_cast<float*>(
                                static_cast<unsigned char*>(instance) +
                                kOffInstanceHeading) * 57.2957795f);
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    g_first_person_move_driven = false;
                }
            }
            return result;
        }

        // HorizPoleStateImpl::ComputeDesiredDirection (0x0055B8C0) runs every
        // frame from HorizPoleStateImpl::Process and writes the requested
        // HPoleDirection to +0x50. ComputeCurrentDirection (0x0055BA00)
        // quantises Lara's facing into +0x51 on the hang states' Entry.
        // HPoleHangIdle::Process then: desired != current -> HangTurn to
        // face desired; desired == current and 1/3 -> Traverse (forward along
        // her facing); 0/2 -> swing. So retail traverse is "turn to face
        // along the bar, then go". The enum is pole-relative, not
        // Lara-relative: with a = MATH3D_AngleFromVector(PlayerData+0xC20)
        // and the engine's CCW quaternion rotation, 3 is facing +C20 and 1 is
        // facing -C20 (C20 is the bar's horizontal direction); 0/2 face
        // across it.
        //
        // A sideways hand pull is measured along the captured bar axis
        // (grip tangent); the body should travel toward +tangent for a
        // positive pull. Set desired to the enum for that WORLD direction.
        // It stays fixed while Lara turns, so the retail turn completes
        // and traverse follows. (Requesting a Lara-relative side instead
        // re-aimed the request every time she turned: she spun in place.)
        void __fastcall detour_hpole_desired_direction(void* pole, void*)
        {
            if (!g_hpole_desired_direction)
                return;
            g_hpole_desired_direction(pole);
            if (!config().immersive_controls || !g_first_person_active ||
                !g_first_person_bar_hanging ||
                !g_first_person_bar_axis_valid ||
                !(g_ledge_hand_grip[0].held || g_ledge_hand_grip[1].held))
                return;

            float right = 0.0f, forward = 0.0f;
            if (!vr_input_first_person_move(&right, &forward) ||
                fabsf(right) < 0.5f || fabsf(forward) >= 0.5f)
                return;

            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!player)
                    return;
                const float* c20 =
                    reinterpret_cast<const float*>(player + 0xC20);
                const float cl = sqrtf(c20[0] * c20[0] + c20[1] * c20[1]);
                if (!std::isfinite(cl) || cl < 1e-4f)
                    return;
                const float side = right > 0.0f ? 1.0f : -1.0f;
                const float travel[2] = {
                    side * g_first_person_bar_axis[0],
                    side * g_first_person_bar_axis[1] };
                const float along =
                    (travel[0] * c20[0] + travel[1] * c20[1]) / cl;
                unsigned char* state = static_cast<unsigned char*>(pole);
                const int native = state[0x50];
                const int current = state[0x51];
                // The captured wrist axis must lie along C20. If it does
                // not, the enum reasoning above does not hold for this bar;
                // leave the retail result rather than guess.
                const bool aligned = fabsf(along) >= 0.7f;
                const int wanted = along > 0.0f ? 3 : 1;
                if (aligned)
                    state[0x50] = static_cast<unsigned char>(wanted);

                // Retail fast traverse: HPoleTraverse's message handler
                // (0x0056B370) stores HorizPoleStateImpl+0x20 (150.0, set on
                // Entry) in PlayerData+0x6DC when the player presses in the
                // stroke's frame window; HPoleTraverse::Process then enters
                // HPoleFastTraverse at its own switch window, and each fast
                // stroke continues while +0x6DC is non-zero (Traverse and
                // FastTraverse Entry clear it). Hand-over-hand keeps it set
                // while she is already travelling the wanted way.
                static bool fast_reported = false;
                const bool fast = aligned && current == wanted &&
                    vr_input_bar_fast_traverse();
                if (fast)
                {
                    const float speed =
                        *reinterpret_cast<const float*>(state + 0x20);
                    if (std::isfinite(speed) && speed > 0.0f)
                        *reinterpret_cast<float*>(
                            const_cast<unsigned char*>(player) + 0x6DC) =
                            speed;
                    if (!fast_reported)
                        log("first-person: bar fast traverse requested "
                            "(PlayerData+0x6DC = %.1f)", speed);
                }
                fast_reported = fast;

                static int last_wanted = -1, last_current = -1;
                static unsigned reports = 0;
                static DWORD last_report_at = 0;
                const DWORD now = GetTickCount();
                if (reports < 60 &&
                    (wanted != last_wanted || current != last_current ||
                     now - last_report_at >= 2000))
                {
                    log("first-person: bar pull %s -> world (%.2f, %.2f); "
                        "pole C20 (%.2f, %.2f), along %+.2f; desired native "
                        "%d -> %d, current %d%s",
                        side > 0.0f ? "+axis" : "-axis",
                        travel[0], travel[1], c20[0] / cl, c20[1] / cl,
                        along, native, aligned ? wanted : native, current,
                        aligned ? "" : " [axis not along C20: retail kept]");
                    ++reports;
                    last_wanted = wanted;
                    last_current = current;
                    last_report_at = now;
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        // WallVertPoleAttached::MessageHandler on the jump message
        // 0x80000056 writes the jump heading to PlayerData+0xFF0:
        //   a = AngleFromVector(-markupNormal)       (facing the vine)
        //   no stick                -> a + pi       (away from the wall)
        //   stick within 45 deg of a +/- pi/2       -> that side
        //   otherwise               -> a + pi
        // where the stick angle comes from FilteredInput in retail camera
        // terms. In VR that angle (and the old view/actor sign patches
        // that tried to predict it) is unreliable, which gave inverted
        // jumps. Decide in world space instead: the intent is the raw left
        // stick taken relative to the rendered view (camera-to-world rows),
        // so "face the vine and push sideways" and "look sideways and push
        // forward" are the same gesture. Pick the retail option whose
        // direction (sin h, -cos h) best matches, with a 60-degree side
        // cone, and overwrite +0xFF0 after the retail handler.
        void* __fastcall detour_wall_vert_pole_message(void* self, void*,
                                                       void* player,
                                                       void* message)
        {
            void* result = g_wall_vert_pole_message(self, player, message);
            if (!config().immersive_controls || !g_first_person_active)
                return result;
            __try
            {
                if (!message ||
                    *static_cast<const unsigned*>(message) != 0x80000056u)
                    return result;
                unsigned char* data =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!data)
                    return result;
                float* heading = reinterpret_cast<float*>(data + 0xFF0);
                const float native = *heading;
                float sx = 0.0f, sy = 0.0f;
                vr_input_first_person_stick(&sx, &sy);
                const float magnitude = sqrtf(sx * sx + sy * sy);
                if (magnitude < 0.35f)
                {
                    log("first-person: vine jump with no stick -> retail "
                        "back jump (heading %.1f deg)", native * 57.2957795f);
                    return result;
                }
                void* markup = *reinterpret_cast<void* const*>(data + 0xC00);
                if (!markup)
                    return result;
                __declspec(align(16)) float normal[4] = { 0, 0, 0, 0 };
                reinterpret_cast<PFN_MarkupNormal>(kMarkupNormal)(markup,
                                                                  normal);
                __declspec(align(16)) float into[4] = {
                    -normal[0], -normal[1], -normal[2], 0.0f };
                const float a = reinterpret_cast<PFN_AngleFromVector>(
                    kAngleFromVector)(into);
                if (!std::isfinite(a))
                    return result;

                // World intent from the rendered view: cwTransform2f rows
                // are the camera's right (0) and forward (2) in world.
                const float* cw = reinterpret_cast<const float*>(kMainCamera) +
                                  kOffCwTransform / sizeof(float);
                float right[2] = { cw[0], cw[1] };
                float forward[2] = { cw[8], cw[9] };
                const float rl = sqrtf(right[0] * right[0] +
                                       right[1] * right[1]);
                const float fl = sqrtf(forward[0] * forward[0] +
                                       forward[1] * forward[1]);
                if (rl < 1e-4f || fl < 1e-4f)
                    return result;
                float intent[2] = {
                    right[0] / rl * sx + forward[0] / fl * sy,
                    right[1] / rl * sx + forward[1] / fl * sy };
                const float il = sqrtf(intent[0] * intent[0] +
                                       intent[1] * intent[1]);
                if (il < 1e-4f)
                    return result;
                intent[0] /= il;
                intent[1] /= il;

                const float pi = 3.14159265f;
                const float options[3] = { a + 0.5f * pi, a - 0.5f * pi,
                                           a + pi };
                const char* names[3] = { "side (a+90)", "side (a-90)", "back" };
                int choice = 2;
                float best = cosf(60.0f * pi / 180.0f);
                for (int i = 0; i < 2; ++i)
                {
                    const float dot = intent[0] * sinf(options[i]) -
                                      intent[1] * cosf(options[i]);
                    if (dot > best)
                    {
                        best = dot;
                        choice = i;
                    }
                }
                float chosen = fmodf(options[choice], 2.0f * pi);
                if (chosen < 0.0f)
                    chosen += 2.0f * pi;
                *heading = chosen;
                log("first-person: vine jump stick (%.2f, %.2f) -> world "
                    "(%.2f, %.2f); wall %.1f deg; retail %.1f -> %s %.1f deg",
                    sx, sy, intent[0], intent[1], a * 57.2957795f,
                    native * 57.2957795f, names[choice],
                    chosen * 57.2957795f);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return result;
        }

        // Jumping off a free-standing vertical pole or chain (user,
        // 2026-10-05: "too easy to jump the wrong direction"). Its jump
        // message only sets jump type 0x10 (VertPoleAttached::Message-
        // Handler 0x0055C3C0); the heading comes from CalcJumpFacingDir
        // (0x005784F0), whose type-0x10 case is FilteredInput::GetZDir --
        // the stick in the retail camera's terms -- or, with no stick,
        // straight back from Lara's facing. Neither follows the headset.
        // In first person the jump now goes where the player looks: the
        // view's horizontal forward, or with the stick pushed, the stick
        // taken relative to the view. Latched for the jump's tests.
        typedef float (__cdecl* PFN_CalcJumpFacingDir)();
        PFN_CalcJumpFacingDir g_calc_jump_facing = nullptr;
        float g_pole_jump_heading = 0.0f;
        DWORD g_pole_jump_at = 0;

        float __cdecl detour_calc_jump_facing()
        {
            const float native = g_calc_jump_facing();
            if (!config().immersive_controls || !g_first_person_active)
                return native;
            __try
            {
                const unsigned char* data =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!data || *reinterpret_cast<const int*>(data + 0x28) !=
                                 0x10)
                    return native;
                const DWORD now = GetTickCount();
                if (g_pole_jump_at && now - g_pole_jump_at <= 1200)
                    return g_pole_jump_heading;

                const float* cw = reinterpret_cast<const float*>(kMainCamera) +
                                  kOffCwTransform / sizeof(float);
                float right[2] = { cw[0], cw[1] };
                float forward[2] = { cw[8], cw[9] };
                const float rl = sqrtf(right[0] * right[0] +
                                       right[1] * right[1]);
                const float fl = sqrtf(forward[0] * forward[0] +
                                       forward[1] * forward[1]);
                if (!(rl > 1e-4f) || !(fl > 1e-4f))
                    return native;
                float sx = 0.0f, sy = 0.0f;
                vr_input_first_person_stick(&sx, &sy);
                const bool stick = sqrtf(sx * sx + sy * sy) >= 0.35f;
                float intent[2] = { forward[0] / fl, forward[1] / fl };
                if (stick)
                {
                    intent[0] = right[0] / rl * sx + forward[0] / fl * sy;
                    intent[1] = right[1] / rl * sx + forward[1] / fl * sy;
                }
                const float il = sqrtf(intent[0] * intent[0] +
                                       intent[1] * intent[1]);
                if (!(il > 1e-4f))
                    return native;
                // Heading h points along (sin h, -cos h), as for vines.
                const float two_pi = 6.28318531f;
                float heading = atan2f(intent[0] / il, -intent[1] / il);
                if (heading < 0.0f)
                    heading += two_pi;
                if (!std::isfinite(heading))
                    return native;
                g_pole_jump_heading = heading;
                g_pole_jump_at = now;
                static unsigned reports = 0;
                if (reports++ < 24)
                    log("first-person: pole jump %s -> world (%.2f, %.2f), "
                        "heading %.1f deg (retail %.1f)",
                        stick ? "stick relative to the view" : "view forward",
                        intent[0] / il, intent[1] / il, heading * 57.2957795f,
                        native * 57.2957795f);
                return heading;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return native;
            }
        }

        // Proof from the game itself that fast traverse happened: retail
        // enters HPoleFastTraverse once per fast stroke. Hard to see in the
        // headset, so each entry also gives both controllers a short buzz.
        void __fastcall detour_hpole_fast_traverse_entry(void* self, void*,
                                                         void* player,
                                                         int arg)
        {
            g_hpole_fast_traverse_entry(self, player, arg);
            static unsigned strokes = 0;
            ++strokes;
            if (strokes <= 40 || strokes % 50 == 0)
                log("first-person: retail HPoleFastTraverse stroke %u",
                    strokes);
            if (g_first_person_active)
            {
                vr_input_handhold_haptic(true);
                vr_input_handhold_haptic(false);
            }
        }

        void __fastcall detour_filtered_input_update(
            void* filtered, void*, float x, float y)
        {
            // Must stay the hook itself: a wrapper changes this caller
            // (2026-10-05, a timing wrapper broke strafing).
            const uintptr_t caller =
                reinterpret_cast<uintptr_t>(_ReturnAddress());

            // Retail direction code (0x00579830) builds world travel as
            // zDir = Camera::lagZ (0x010FC980) + stick direction, and
            // zDelta = zDir relative to Lara. In first person the third-person
            // camera never updates lagZ to the VR view, so every camera-
            // relative movement used a stale heading. Supply the view heading
            // for this call only; retail then produces native directions in
            // every ground state, combat strafing included.
            float* const lag_z = reinterpret_cast<float*>(0x010FC980);
            float saved_lag_z = 0.0f;
            float view_heading = 0.0f;
            bool lag_set = false;
            if (g_first_person_active && g_first_person_instance &&
                caller == kPlayerMovementUpdateReturn &&
                // Swimming is camera-relative too (2026-10-01: it steered
                // from the stale third-person lagZ, which felt like Lara's
                // own facing rather than the view).
                (g_first_person_traversal == TraversalGround ||
                 g_first_person_traversal == TraversalSwim) &&
                first_person_view_camera_heading(&view_heading))
            {
                __try
                {
                    saved_lag_z = *lag_z;
                    *lag_z = view_heading;
                    lag_set = true;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    lag_set = false;
                }
            }
            g_filtered_input_update(filtered, x, y);
            if (lag_set)
            {
                __try
                {
                    *lag_z = saved_lag_z;
                    const float* stick = static_cast<const float*>(filtered);
                    static unsigned checks = 0;
                    static float last_direction = -100.0f;
                    static bool swim_checks_reset = false;
                    if (g_first_person_traversal == TraversalSwim &&
                        !swim_checks_reset)
                    {
                        swim_checks_reset = true; // verify swim separately
                        checks = 0;
                    }
                    if (stick[5] >= 0.25f && checks < 16 &&
                        fabsf(wrap_pi(stick[3] - last_direction)) > 0.2f)
                    {
                        // Verify the model: retail zDir should equal the view
                        // heading plus its own stick direction plus 180 deg
                        // (lagZ is camera-convention, travel is actor yaw).
                        ++checks;
                        last_direction = stick[3];
                        log("first-person: camera-relative input lagZ %.1f "
                            "(was %.1f), stick dir %.1f -> retail zDir %.1f, "
                            "expected %.1f, bearing %.1f deg%s",
                            view_heading * 57.2957795f,
                            saved_lag_z * 57.2957795f,
                            stick[3] * 57.2957795f, stick[2] * 57.2957795f,
                            wrap_heading(view_heading + stick[3] + 3.14159265f) *
                                57.2957795f,
                            stick[4] * 57.2957795f,
                            g_first_person_traversal == TraversalSwim
                                ? " [swim]" :
                            g_first_person_combat_locomotion
                                ? " [combat]" : "");
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
            }

            // Let retail filter the keys and determine magnitude. For normal
            // ground travel, replace only its camera-relative direction with
            // the movement heading already computed from the current VR view.
            if (!g_first_person_active || !g_first_person_instance ||
                caller != kPlayerMovementUpdateReturn)
                return;
            __try
            {
                unsigned char* root =
                    *reinterpret_cast<unsigned char**>(kFilteredInputRoot);
                if (!root || filtered != root + 0x60)
                    return;
                float* stick = reinterpret_cast<float*>(filtered);
                const float direction = stick[3];  // Stick::stickZDir
                float bearing = stick[4];         // Stick::zDelta
                const float magnitude = stick[5]; // Stick::mag
                if (!std::isfinite(direction) ||
                    !std::isfinite(bearing) || !std::isfinite(magnitude))
                    return;
                if (magnitude < 0.25f)
                {
                    g_first_person_probe_sector = -1;
                    return;
                }
                const int sector = fabsf(stick[0]) > fabsf(stick[1])
                    ? (stick[0] < 0.0f ? 1 : 3)
                    : (stick[1] < 0.0f ? 0 : 2);
                unsigned char* player =
                    *reinterpret_cast<unsigned char**>(0x0111713C);
                const int control_mode = int(*reinterpret_cast<signed char*>(
                    static_cast<unsigned char*>(filtered) + 0x70));
                int steering = player
                    ? *reinterpret_cast<int*>(player + 0x74) : -1;
                if (g_first_person_move_driven && control_mode == 3)
                {
                    stick[2] = g_first_person_move_heading;
                    stick[4] = 0.0f; // Actor already faces travel: move forward.
                    bearing = 0.0f;
                    if (!g_first_person_motion_reported[sector])
                    {
                        g_first_person_motion_reported[sector] = true;
                        const char* labels[4] = {
                            "forward", "left", "back", "right" };
                        log("first-person: player-driven %s travel at "
                            "%.1f deg (steering %d)", labels[sector],
                            g_first_person_move_heading * 57.2957795f,
                            steering);
                    }
                }
                if (g_first_person_traversal == TraversalClimb &&
                    !g_first_person_vine_climbing &&
                    g_first_person_turn_ready &&
                    g_first_person_actor_offset_valid &&
                    vr_input_first_person_jump_held())
                {
                    float right = 0.0f, forward = 0.0f;
                    if (vr_input_first_person_move(&right, &forward))
                    {
                        const float stick_turn =
                            vr_input_first_person_turn() -
                            g_first_person_stick_start;
                        const float jump_view = wrap_heading(
                            g_first_person_base_heading +
                            g_first_person_turn_sign * stick_turn -
                            g_first_person_turn_sign *
                            first_person_hmd_heading());
                        const float travel = wrap_heading(
                            jump_view +
                            g_first_person_actor_camera_offset +
                            atan2f(-right, forward));
                        const float actor_heading =
                            *reinterpret_cast<float*>(
                                static_cast<unsigned char*>(
                                    g_first_person_instance) +
                                kOffInstanceHeading);
                        if (std::isfinite(travel) &&
                            std::isfinite(actor_heading))
                        {
                            const float requested_bearing =
                                wrap_pi(travel - actor_heading);
                            // Vines are decided in
                            // detour_wall_vert_pole_message instead.
                            const float jump_bearing = requested_bearing;
                            stick[2] = wrap_heading(
                                actor_heading + jump_bearing);
                            stick[4] = jump_bearing;
                            static unsigned jump_reports = 0;
                            if (jump_reports++ < 16)
                                log("first-person: attached jump input "
                                    "view %.1f, travel %.1f, actor %.1f, "
                                    "bearing %.1f deg (mode %d)",
                                    jump_view * 57.2957795f,
                                    stick[2] * 57.2957795f,
                                    actor_heading * 57.2957795f,
                                    stick[4] * 57.2957795f,
                                    control_mode);
                        }
                    }
                }
                float* pos = reinterpret_cast<float*>(
                    static_cast<unsigned char*>(g_first_person_instance) + 0x10);
                if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) ||
                    !std::isfinite(pos[2]))
                    return;
                const DWORD now = GetTickCount();
                if (sector != g_first_person_probe_sector)
                {
                    g_first_person_probe_sector = sector;
                    g_first_person_probe_start = now;
                    for (int axis = 0; axis < 3; ++axis)
                        g_first_person_probe_position[axis] = pos[axis];
                }
                if (!g_first_person_movement_reported[sector] &&
                    now - g_first_person_probe_start >= 500)
                {
                    g_first_person_movement_reported[sector] = true;
                    const char* labels[4] = { "forward", "left", "back", "right" };
                    log("first-person: %s hold %.0f ms: filtered (%.2f, %.2f), "
                        "stick dir %.1f, bearing %.1f, zDir %.1f, "
                        "upAngle %.1f deg, control mode %d, steering %d, "
                        "actor %.1f deg, displacement (%.1f, %.1f, %.1f)",
                        labels[sector], float(now - g_first_person_probe_start),
                        stick[0], stick[1], direction * 57.2957795f,
                        bearing * 57.2957795f, stick[2] * 57.2957795f,
                        *reinterpret_cast<float*>(
                            static_cast<unsigned char*>(filtered) + 0x11C) *
                            57.2957795f,
                        control_mode, steering,
                        *reinterpret_cast<float*>(
                            static_cast<unsigned char*>(g_first_person_instance) +
                            kOffInstanceHeading) * 57.2957795f,
                        pos[0] - g_first_person_probe_position[0],
                        pos[1] - g_first_person_probe_position[1],
                        pos[2] - g_first_person_probe_position[2]);
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                // A stale retail input pointer must not interrupt gameplay.
            }
        }

        void first_person_face_lara(float camera_heading)
        {
            if (!g_first_person_instance ||
                g_first_person_traversal != TraversalGround ||
                !std::isfinite(camera_heading))
                return;

            __try
            {
                float* actor_heading = reinterpret_cast<float*>(
                    static_cast<unsigned char*>(g_first_person_instance) +
                    kOffInstanceHeading);
                const float current = *actor_heading;
                if (!std::isfinite(current))
                    return;

                const bool retained_offset =
                    g_first_person_actor_offset_valid &&
                    g_first_person_actor_offset_model == g_first_person_model;
                if (!retained_offset)
                {
                    // Lara's model forward is opposite the camera in the
                    // measured startup scene. Capture the actual relationship
                    // for this entry so another level or model can differ.
                    g_first_person_actor_camera_offset = wrap_pi(
                        current - g_first_person_base_heading);
                    g_first_person_actor_offset_valid = true;
                    g_first_person_actor_offset_model = g_first_person_model;
                    log("first-person: actor-to-camera yaw offset %.1f deg "
                        "(actor %.1f, camera %.1f)",
                        g_first_person_actor_camera_offset * 57.2957795f,
                        current * 57.2957795f,
                        g_first_person_base_heading * 57.2957795f);
                }

                const float wanted = wrap_heading(
                    camera_heading + g_first_person_actor_camera_offset);
                if (!g_first_person_move_driven &&
                    !g_first_person_combat_locomotion &&
                    fabsf(wrap_pi(current - wanted)) >= 0.002f)
                    *actor_heading = wanted;

                if (!g_first_person_actor_reported)
                {
                    g_first_person_actor_reported = true;
                    if (retained_offset)
                        log("first-person: retained actor-to-camera offset "
                            "%.1f deg across camera transition",
                            g_first_person_actor_camera_offset * 57.2957795f);
                    log("first-person: Lara yaw at Instance+0x38 follows "
                        "HMD/right-stick heading (%.1f deg)",
                        wanted * 57.2957795f);
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                // First-person resolve will reject a stale actor next frame.
            }
        }

        void first_person_drive_game_heading(void* cam, float* rotation,
                                             const float* saved, bool level,
                                             Mat4* base_camera)
        {
            if (!g_first_person_turn_ready)
            {
                // Establish the sign empirically from the engine's own camera
                // builder. A tiny positive heading is compared with positive
                // and negative HMD-yaw compositions, avoiding another guessed
                // Euler convention. This costs two extra rebuilds once per
                // Lara/model entry, not per frame.
                const float epsilon = 0.01f;
                // Same for pitch: the retail target choice (sense rule 6,
                // 0x00592E37) uses a cone about the camera Euler, so first
                // person writes the headset pitch there too.
                rotation[1] = 0.0f;
                rotation[2] = saved[2];
                rotation[0] = 0.1f;
                g_original(cam);
                const float pitch_up =
                    view_pitch_of(load_matrix(cam, kOffWcTransform));
                rotation[0] = -0.1f;
                g_original(cam);
                const float pitch_down =
                    view_pitch_of(load_matrix(cam, kOffWcTransform));
                g_first_person_pitch_scale = (pitch_up - pitch_down) / 0.2f;
                log("first-person: camera Euler pitch moves the view %.2f "
                    "rad per rad", g_first_person_pitch_scale);
                rotation[0] = level ? 0.0f : saved[0];
                rotation[1] = level ? 0.0f : saved[1];
                rotation[2] = wrap_heading(saved[2] + epsilon);
                g_original(cam);
                const Mat4 plus = load_matrix(cam, kOffWcTransform);

                rotation[2] = saved[2];
                g_original(cam);
                *base_camera = load_matrix(cam, kOffWcTransform);

                const Mat4 wanted_positive =
                    *base_camera * yaw_rotation(epsilon);
                const Mat4 wanted_negative =
                    *base_camera * yaw_rotation(-epsilon);
                g_first_person_turn_sign =
                    rotation_error(plus, wanted_positive) <=
                    rotation_error(plus, wanted_negative) ? 1.0f : -1.0f;

                g_first_person_base_heading = saved[2];
                if (g_first_person_actor_offset_valid &&
                    g_first_person_actor_offset_model == g_first_person_model &&
                    g_first_person_instance)
                {
                    // On return from a cinematic, the third-person camera
                    // Euler is unrelated to Lara's forward direction. Start
                    // the new first-person view from her current heading.
                    __try
                    {
                        const float actor_heading = *reinterpret_cast<float*>(
                            static_cast<unsigned char*>(g_first_person_instance) +
                            kOffInstanceHeading);
                        if (std::isfinite(actor_heading))
                        {
                            g_first_person_base_heading = wrap_heading(
                                actor_heading - g_first_person_actor_camera_offset);
                            log("first-person: camera base restored from Lara "
                                "heading %.1f deg (cinematic camera %.1f deg)",
                                g_first_person_base_heading * 57.2957795f,
                                saved[2] * 57.2957795f);
                        }
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        // Keep the retail camera angle if the actor is stale.
                    }
                }
                g_first_person_stick_start =
                    vr_input_first_person_turn();
                g_first_person_turn_ready = true;
                log("first-person: headset and right-stick own camera yaw "
                    "(engine sign %+.0f); movement steering cannot turn view",
                    g_first_person_turn_sign);
            }

            const float hmd_heading = first_person_hmd_heading();
            // Lara's steering and jump animations can rewrite the engine
            // heading by more than 180 degrees. First person keeps its own
            // reference and accepts only physical HMD and right-stick turns.
            const float stick_heading =
                vr_input_first_person_turn() - g_first_person_stick_start;
            const float body_heading = wrap_heading(
                g_first_person_base_heading +
                g_first_person_turn_sign * stick_heading);
            // yaw_of() reports the camera's viewing direction, while the
            // engine builder writes a world-to-camera rotation. A positive
            // viewing yaw is therefore the opposite Euler change from the
            // positive right-stick Euler turn calibrated above. Matching the
            // HMD view here also gives Lara and gameplay the same heading.
            const float wanted = wrap_heading(
                body_heading - g_first_person_turn_sign * hmd_heading);

            // Build the visible camera at the stick/mouse-controlled body
            // heading. The already-proven full HMD matrix is composed later,
            // giving exactly one copy of physical yaw. Persist the combined
            // heading only after the matrices are built so gameplay receives
            // the headset direction without doubling the rendered view.
            rotation[0] = level ? 0.0f : saved[0];
            rotation[1] = level ? 0.0f : saved[1];
            rotation[2] = body_heading;
            g_original(cam);
            *base_camera = load_matrix(cam, kOffWcTransform);

            rotation[0] = saved[0];
            rotation[1] = saved[1];
            rotation[2] = wanted; // persistent camera heading
            first_person_face_lara(wanted);
        }

        // The head pose, composed onto the transform the engine just built.
        //
        // CAMERA_CalculateWCTransform produces a world-to-camera matrix from the
        // camera position and its Euler angles. Rather than convert the head
        // rotation into that Euler convention -- which would mean matching the
        // engine's Z, then Y, then X order and three sign choices, every one of
        // them a chance to be subtly wrong -- the matrix is taken as it comes and
        // the head rotation multiplied onto it.
        //
        // For a world-to-camera transform  wc = [Rc | 0; -p*Rc | 1]  and a head
        // rotation R,
        //
        //     wc * R = [Rc*R | 0; -p*(Rc*R) | 1]
        //
        // which is the same camera, at the same place, rotated by the head. The
        // displacement then only has to move the translation row, and the term
        // works out as head_position * R -- no camera rotation in it at all, so
        // there is nothing to get out of step.
        //
        // This replaces a design that put yaw on the camera and pitch and roll in
        // the projection. Both halves were right; the composition was not.
        Mat4 load_matrix(const void* cam, unsigned off)
        {
            Mat4 m;
            memcpy(&m.m[0][0], (const unsigned char*)cam + off, sizeof(float) * 16);
            return m;
        }

        void store_matrix(void* cam, unsigned off, const Mat4& m)
        {
            memcpy((unsigned char*)cam + off, &m.m[0][0], sizeof(float) * 16);
        }

        // Retail 0x0048DCA0 finishes wcTransformf by scaling its view X and Y
        // columns by screenXRatio (Camera+0x2C0) and 448/384 * screenYRatio
        // (+0x2C4): a flat-monitor aspect prescale matched to the fixed 8/7
        // projection. wcTransformNoShakef copies that rotation. With the head
        // pose multiplied on afterwards the view became R*S*H, which scales
        // the world in the BODY frame: the centre of view showed world slope
        // tan(pitch)/Sy while every gameplay ray (built from the unscaled
        // wcTransform2f) used tan(pitch) -- shots high looking up, low looking
        // down, exact only when level. Moving S after H (R*H*S) instead puts
        // a fixed anisotropic stretch in the headset. A VR view has no place
        // for S at all, and our per-eye projection never compensated for it,
        // so divide it out: each column's length is exactly its scale, since
        // the rotation underneath is orthonormal.
        bool remove_view_prescale(Mat4* m, float* scale_x, float* scale_y)
        {
            float scale[2]{};
            for (int column = 0; column < 2; ++column)
            {
                scale[column] = sqrtf(
                    m->m[0][column] * m->m[0][column] +
                    m->m[1][column] * m->m[1][column] +
                    m->m[2][column] * m->m[2][column]);
                if (!std::isfinite(scale[column]) || scale[column] < 0.25f ||
                    scale[column] > 4.0f)
                    return false;
            }
            for (int column = 0; column < 2; ++column)
                for (int row = 0; row < 4; ++row)
                    m->m[row][column] /= scale[column];
            if (scale_x) *scale_x = scale[0];
            if (scale_y) *scale_y = scale[1];
            return true;
        }

        float g_view_prescale[2] = { 1.0f, 1.0f };

        void apply_head_pose(void* cam, const float* camera_position)
        {
            Mat4 head_view = vr_head_camera_view();
            const bool unscale = tune_view_prescale_fix();

            // All three, because the engine builds three and different parts of it
            // read different ones. Leaving any behind would put the picture and
            // the culling in different places.
            const unsigned targets[3] = { kOffWcTransform, kOffWcNoShake, kOffWcTransform2 };
            for (int t = 0; t < 3; ++t)
            {
                Mat4 m = load_matrix(cam, targets[t]);
                float sx = 1.0f, sy = 1.0f;
                if (unscale && targets[t] != kOffWcTransform2 &&
                    remove_view_prescale(&m, &sx, &sy) &&
                    cam == kMainCamera && targets[t] == kOffWcTransform)
                {
                    g_view_prescale[0] = sx;
                    g_view_prescale[1] = sy;
                    static float reported[2] = { 0.0f, 0.0f };
                    if (fabsf(sx - reported[0]) > 0.001f ||
                        fabsf(sy - reported[1]) > 0.001f)
                    {
                        reported[0] = sx;
                        reported[1] = sy;
                        log("view: removed flat-screen prescale x %.4f, "
                            "y %.4f (screenRatio %.4f/%.4f) before the head "
                            "pose", sx, sy,
                            *reinterpret_cast<const float*>(
                                static_cast<const unsigned char*>(cam) + 0x2C0),
                            *reinterpret_cast<const float*>(
                                static_cast<const unsigned char*>(cam) + 0x2C4));
                    }
                }
                if (camera_position)
                    set_view_position(&m, camera_position);
                m = m * head_view;
                store_matrix(cam, targets[t], m);
            }

            // The engine only rebuilds the camera-to-world transform inside
            // SetProjDistance2, and this function is reached from seven places. Left
            // alone it would describe a camera that is no longer where the picture
            // is drawn from -- and the object culling frustum is built through it.
            store_matrix(cam, kOffCwTransform,
                         rigid_inverse(load_matrix(cam, kOffWcTransform2)));
        }
        // Third-person camera presets (third_person_mode). The game's camera
        // keeps its heading -- right stick and auto-centre -- so retail
        // camera-relative movement still matches the view; only where the
        // camera stands is replaced, and smoothed:
        //   classic  the game's own position, smoothed (it lurches with
        //            Lara's root motion);
        //   shoulder about shoulder_distance behind the game's focus point,
        //            a little above and to the right; never further back
        //            than the game's own (collision-checked) camera;
        //   board    the world scaled down (camera_world_scale_factor), the
        //            player seated at a tabletop anchor that drifts after
        //            Lara only once she walks away from it.
        struct ThirdPersonCamera
        {
            bool valid = false;
            float position[3]{};
            bool anchor_valid = false;
            float anchor[3]{};
            int mode = -1;
            LARGE_INTEGER last{};
        };
        ThirdPersonCamera g_tp_camera;
        bool g_tp_board_scale = false;
        // How far the third-person camera stands from the retail one.
        float g_tp_retail_distance = 0.0f;
        // Main camera to its focus (Lara), world units, after the head pose:
        // the depth world markers are converged at.
        float g_marker_depth = 0.0f;
        // Third-person views drawn from somewhere other than the retail
        // camera: CameraCore::Position (+0x00) is set to the drawn eye for
        // the frame's rendering -- terrain visibility reads it, as first
        // person found (2026-09-25) -- and the game's own value is put back
        // before its next camera build and at Present, so the retail camera
        // logic never sees ours. Board rocks came and went with the
        // viewpoint (user, 2026-10-03).
        // (g_core_swapped / g_core_saved / g_core_written: declared with
        // g_draw_instance, where the sky probe reads them.)

        void core_position_restore(void* cam)
        {
            if (!g_core_swapped || !cam)
                return;
            float* core = reinterpret_cast<float*>(
                static_cast<unsigned char*>(cam) + kOffPosition);
            // Only if it still holds ours: the game may have moved it.
            if (memcmp(core, g_core_written, sizeof(g_core_written)) == 0)
                memcpy(core, g_core_saved, sizeof(g_core_saved));
            g_core_swapped = false;
        }

        bool horizontal_unit(float x, float y, float out[2])
        {
            const float length = sqrtf(x * x + y * y);
            if (!(length > 1e-5f))
                return false;
            out[0] = x / length;
            out[1] = y / length;
            return true;
        }

        void* lara_instance()
        {
            __try
            {
                return *reinterpret_cast<void* const*>(kPlayerInstance);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        bool third_person_camera_position(void* cam, const Mat4& retail,
                                          float out[3])
        {
            // Pull-up / one-hand catch animation log in third person too.
            {
                void* lara = lara_instance();
                const TraversalProbe probe =
                    first_person_traversal_probe(lara);
                climb_anim_watch(lara, probe.state_vtable,
                                 probe.precarious, false);
            }
            const int mode = camera_third_person_mode();
            Mat4 unscaled = retail;
            remove_view_prescale(&unscaled, nullptr, nullptr);
            const Mat4 to_world = rigid_inverse(unscaled);
            float forward[2], right[2];
            if (!horizontal_unit(to_world.m[2][0], to_world.m[2][1],
                                 forward) ||
                !horizontal_unit(to_world.m[0][0], to_world.m[0][1], right))
                return false;
            const float* focus = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(cam) + kOffFocusPoint);
            const float retail_position[3] = {
                to_world.m[3][0], to_world.m[3][1], to_world.m[3][2] };
            for (int k = 0; k < 3; ++k)
                if (!std::isfinite(focus[k]) ||
                    !std::isfinite(retail_position[k]))
                    return false;
            const float metre = config().world_scale;
            float target[3] = { retail_position[0], retail_position[1],
                                retail_position[2] };
            if (mode == 1)
            {
                const float dx = retail_position[0] - focus[0];
                const float dy = retail_position[1] - focus[1];
                const float dz = retail_position[2] - focus[2];
                const float retail_back = sqrtf(dx * dx + dy * dy + dz * dz);
                float back = config().shoulder_distance * metre;
                // The game's camera is pulled in by walls; never stand
                // further back than it does.
                if (retail_back > 0.3f * metre && back > retail_back)
                    back = retail_back;
                const float side = config().shoulder_side * metre;
                // Along the game camera's own direction from Lara, so the
                // look stick's up/down orbit raises and lowers the shoulder
                // view too (it stayed at a fixed height, user 2026-10-03).
                float away[3] = { -forward[0], -forward[1], 0.0f };
                if (retail_back > 0.3f * metre)
                {
                    away[0] = dx / retail_back;
                    away[1] = dy / retail_back;
                    away[2] = dz / retail_back;
                }
                target[0] = focus[0] + away[0] * back + right[0] * side;
                target[1] = focus[1] + away[1] * back + right[1] * side;
                target[2] = focus[2] + away[2] * back +
                            config().shoulder_height * metre;
            }
            else if (mode == 2)
            {
                const float table = metre * camera_world_scale_factor();
                if (!g_tp_camera.anchor_valid)
                {
                    for (int k = 0; k < 3; ++k)
                        g_tp_camera.anchor[k] = focus[k];
                    g_tp_camera.anchor_valid = true;
                }
                // Lara may wander 12 table-cm from the anchor before it
                // follows; height follows at once (stairs, ledges).
                const float ax = focus[0] - g_tp_camera.anchor[0];
                const float ay = focus[1] - g_tp_camera.anchor[1];
                const float away = sqrtf(ax * ax + ay * ay);
                const float leash = 0.12f * table;
                if (away > leash)
                {
                    const float pull = (away - leash) / away;
                    g_tp_camera.anchor[0] += ax * pull;
                    g_tp_camera.anchor[1] += ay * pull;
                }
                g_tp_camera.anchor[2] = focus[2];
                target[0] = g_tp_camera.anchor[0] -
                    forward[0] * config().board_distance * table;
                target[1] = g_tp_camera.anchor[1] -
                    forward[1] * config().board_distance * table;
                target[2] = g_tp_camera.anchor[2] +
                    config().board_height * table;
                // Collision (user, 2026-10-03): turning the view must not
                // swing the board camera into rock. Probe from just above
                // Lara to the seat; on a hit, sit 0.3 m (her scale) short
                // of it, on her side.
                const float from[3] = { focus[0], focus[1],
                                        focus[2] + 0.3f * metre };
                float ray[3] = { target[0] - from[0], target[1] - from[1],
                                 target[2] - from[2] };
                const float reach = sqrtf(ray[0] * ray[0] + ray[1] * ray[1] +
                                          ray[2] * ray[2]);
                if (reach > 1.0f && config().camera_collision)
                {
                    for (float& r : ray)
                        r /= reach;
                    float hit[3]{};
                    if (aim_probe(from, ray, reach, hit, nullptr))
                    {
                        const float hx = hit[0] - from[0];
                        const float hy = hit[1] - from[1];
                        const float hz = hit[2] - from[2];
                        const float keep = fmaxf(0.0f,
                            sqrtf(hx * hx + hy * hy + hz * hz) - 0.3f * metre);
                        for (int k = 0; k < 3; ++k)
                            target[k] = from[k] + ray[k] * keep;
                        static DWORD last_report = 0;
                        if (GetTickCount() - last_report > 3000)
                        {
                            last_report = GetTickCount();
                            log("board: camera pulled in by geometry to "
                                "%.2f of its %.2f table-m", keep / table,
                                reach / table);
                        }
                    }
                }
            }

            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const double dt = g_tp_camera.last.QuadPart && frequency.QuadPart
                ? (double)(now.QuadPart - g_tp_camera.last.QuadPart) /
                  (double)frequency.QuadPart : 1.0;
            g_tp_camera.last = now;
            float jump = 0.0f;
            if (g_tp_camera.valid)
                for (int k = 0; k < 3; ++k)
                    jump += (target[k] - g_tp_camera.position[k]) *
                            (target[k] - g_tp_camera.position[k]);
            const float snap = 4.0f * metre * camera_world_scale_factor();
            if (!g_tp_camera.valid || g_tp_camera.mode != mode ||
                dt > 0.5 || jump > snap * snap)
            {
                for (int k = 0; k < 3; ++k)
                    g_tp_camera.position[k] = target[k];
                if (g_tp_camera.mode != mode)
                    log("third-person: camera %s (smoothing %.2f s)",
                        mode == 1 ? "shoulder" : mode == 2 ? "board game"
                                  : "classic",
                        config().third_person_smoothing);
                g_tp_camera.valid = true;
                g_tp_camera.mode = mode;
            }
            else
            {
                const float tau = config().third_person_smoothing;
                const float follow = tau > 0.001f
                    ? 1.0f - expf(-(float)dt / tau) : 1.0f;
                for (int k = 0; k < 3; ++k)
                    g_tp_camera.position[k] +=
                        (target[k] - g_tp_camera.position[k]) * follow;
            }
            for (int k = 0; k < 3; ++k)
                out[k] = g_tp_camera.position[k];
            {
                const float dx = out[0] - retail_position[0];
                const float dy = out[1] - retail_position[1];
                const float dz = out[2] - retail_position[2];
                g_tp_retail_distance = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            return true;
        }

        // Eye collision for the third-person views (user, 2026-10-03): the
        // seat is already kept on Lara's side of geometry, but the head
        // moves the eye from it -- scaled by the board factor, so a 10 cm
        // lean moved the eye ~1.2 m of her scale into rock. Work out where
        // apply_head_pose will put the eye, probe from the seat to it, and
        // on a hit stop it 0.2 m (her scale) short by moving the seat back
        // by the overshoot. Head rotation is untouched.
        void third_person_eye_collision(const Mat4& base, float seat[3])
        {
            Mat4 m = base;
            if (tune_view_prescale_fix())
                remove_view_prescale(&m, nullptr, nullptr);
            set_view_position(&m, seat);
            m = m * vr_head_camera_view();
            const Mat4 to_world = rigid_inverse(m);
            const float eye[3] = { to_world.m[3][0], to_world.m[3][1],
                                   to_world.m[3][2] };
            float ray[3] = { eye[0] - seat[0], eye[1] - seat[1],
                             eye[2] - seat[2] };
            const float reach = sqrtf(ray[0] * ray[0] + ray[1] * ray[1] +
                                      ray[2] * ray[2]);
            if (!std::isfinite(reach) || reach < 1.0f)
                return;
            for (float& r : ray)
                r /= reach;
            const float margin = 0.2f * config().world_scale;
            float hit[3]{};
            if (!aim_probe(seat, ray, reach + margin, hit, nullptr))
                return;
            const float hx = hit[0] - seat[0], hy = hit[1] - seat[1],
                        hz = hit[2] - seat[2];
            const float keep = fmaxf(0.0f,
                sqrtf(hx * hx + hy * hy + hz * hz) - margin);
            if (keep >= reach)
                return;
            const float back = reach - keep;
            for (int k = 0; k < 3; ++k)
                seat[k] -= ray[k] * back;
            static DWORD last_report = 0;
            if (GetTickCount() - last_report > 3000)
            {
                last_report = GetTickCount();
                log("third-person: head moved the eye into geometry; held "
                    "%.2f m (her scale) back of %.2f", back /
                    config().world_scale, reach / config().world_scale);
            }
        }

        // First-person target cone. The player's sense units (manager
        // [0x01117550], unit i at +i*4) choose targets with rule blocks at
        // unit+0x34: block +4 tier count, +8 tier pointers; tier +4 rule
        // count, +0xC rule pointers; rule word type, word test, float limit
        // (0x00592B50). Types 4-6 are angles (ACos, limit * [0x00F056E0]);
        // a non-zero test passes when the angle is within the limit. Type 6
        // at or below [0x00EFDDB0] is a different (screen) test. First
        // person widens those cones -- the player aims by hand anyway
        // (user, 2026-10-04) -- and restores them on leaving.
        struct ConePatch { float* limit; float original; float widened; };
        std::vector<ConePatch> g_cone_patches;
        void* g_cone_blocks[8]{};
        bool g_cone_widened = false;
        bool g_cone_reported = false;

        void target_cone_restore()
        {
            for (const ConePatch& p : g_cone_patches)
            {
                __try
                {
                    if (*p.limit == p.widened)
                        *p.limit = p.original;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            g_cone_patches.clear();
            g_cone_widened = false;
        }

        void target_cone_update(bool first_person)
        {
            void* blocks[8]{};
            __try
            {
                void** senses = *reinterpret_cast<void** const*>(0x01117550);
                for (int u = 0; senses && u < 8; ++u)
                    if (senses[u])
                        blocks[u] = *reinterpret_cast<void* const*>(
                            static_cast<unsigned char*>(senses[u]) + 0x34);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }
            const bool changed =
                memcmp(blocks, g_cone_blocks, sizeof(blocks)) != 0;
            if (changed)
            {
                // A reloaded rule set: the old addresses may be gone.
                g_cone_patches.clear();
                g_cone_widened = false;
                memcpy(g_cone_blocks, blocks, sizeof(blocks));
            }
            if (!first_person)
            {
                if (g_cone_widened)
                {
                    target_cone_restore();
                    log("targeting: first-person cone widening restored");
                }
                return;
            }
            if (g_cone_widened)
                return;
            g_cone_widened = true;
            const float factor = 2.0f;
            __try
            {
                const float to_rad = *reinterpret_cast<const float*>(0x00F056E0);
                const float screen = *reinterpret_cast<const float*>(0x00EFDDB0);
                const float cap = to_rad > 1e-6f ? 1.40f / to_rad : 0.0f;
                if (!g_cone_reported)
                    log("targeting: rule angle unit %.5f rad, screen-test limit "
                        "%.2f", to_rad, screen);
                for (int u = 0; u < 8; ++u)
                {
                    const unsigned char* block =
                        static_cast<const unsigned char*>(blocks[u]);
                    if (!block)
                        continue;
                    bool seen = false;
                    for (int k = 0; k < u; ++k)
                        seen = seen || blocks[k] == blocks[u];
                    if (seen)
                        continue;
                    const unsigned tiers = *reinterpret_cast<const unsigned*>(
                        block + 4);
                    for (unsigned i = 0; i < tiers && i < 8; ++i)
                    {
                        const unsigned char* tier =
                            *reinterpret_cast<const unsigned char* const*>(
                                block + 8 + i * 4);
                        if (!tier)
                            continue;
                        const unsigned rules =
                            *reinterpret_cast<const unsigned*>(tier + 4);
                        for (unsigned j = 0; j < rules && j < 16; ++j)
                        {
                            unsigned char* rule =
                                *reinterpret_cast<unsigned char* const*>(
                                    tier + 0xC + j * 4);
                            if (!rule)
                                continue;
                            const unsigned short type =
                                *reinterpret_cast<unsigned short*>(rule);
                            const unsigned short test =
                                *reinterpret_cast<unsigned short*>(rule + 2);
                            float* limit = reinterpret_cast<float*>(rule + 4);
                            const float original = *limit;
                            const bool angle = type >= 4 && type <= 6 &&
                                test != 0 && cap > 0.0f &&
                                !(type == 6 && original <= screen);
                            float widened = original;
                            if (angle && std::isfinite(original) &&
                                original > 0.0f)
                                widened = fminf(original * factor,
                                                fmaxf(cap, original));
                            if (!g_cone_reported)
                                log("targeting: unit %d tier %u rule %u -- type "
                                    "%u test %u limit %.2f%s", u, i, j, type,
                                    test, original, widened != original
                                    ? " (widened in first person)" : "");
                            if (widened != original)
                            {
                                *limit = widened;
                                g_cone_patches.push_back(
                                    { limit, original, widened });
                            }
                        }
                    }
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            g_cone_reported = true;
            log("targeting: first person widened %u target cone%s x%.1f",
                (unsigned)g_cone_patches.size(),
                g_cone_patches.size() == 1 ? "" : "s", factor);
        }

        void __cdecl detour_body(void* cam);
        void __cdecl detour(void* cam)
        {
            perf_cpu_begin(PerfCamera);
            detour_body(cam);
            perf_cpu_end(PerfCamera);
        }

        void __cdecl detour_body(void* cam)
        {
            const bool drive = config().hmd_drives_camera
                            && vr_ready() && vr_pose_valid();

            // Levelling still works on the Euler angles, because it is removing
            // the camera's own pitch and roll rather than adding the head's --
            // and zeroing two numbers needs no convention at all.
            // Mission Prep is shown as a pinned flat picture (vr_session):
            // the camera keeps the game's own composed shot -- no head pose,
            // no levelling -- and the projection maps it onto a world-fixed
            // panel like the intro videos.
            const bool flat_screen = cam == kMainCamera &&
                                     ui_mission_prep_active();
            const bool level = tune_level_horizon() && !flat_screen;
            float* rot = (float*)((unsigned char*)cam + kOffRotation);
            const float saved[3] = { rot[0], rot[1], rot[2] };

            const bool main_camera = cam == kMainCamera;
            if (main_camera)
            {
                core_position_restore(cam);
                const float* core = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(cam) + kOffPosition);
                if (!g_core_last_write_valid ||
                    memcmp(core, g_core_last_write,
                           sizeof(g_core_last_write)) != 0)
                    memcpy(g_game_camera, core, sizeof(g_game_camera));
            }
            const short mode = main_camera
                ? *reinterpret_cast<const short*>((const unsigned char*)cam + kOffMode)
                : (short)-1;
            if (main_camera)
            {
                g_accurate_aim_active = mode == 13;
                if (!g_accurate_aim_active && !camera_first_person_active())
                    g_head_aim_target_valid = false;
            }

            // On the first accurate-aim frame, let the engine build one
            // unlevelled reference transform. That is the transform whose
            // centre ray exactly matches targetFocusPoint-targetPos, allowing
            // us to identify the engine's forward basis without an axis guess.
            Mat4 aim_reference = Mat4::identity();
            bool have_aim_reference = false;
            if (drive && config().hmd_aim && main_camera && mode == 13 &&
                level && !g_forward_candidate_verified)
            {
                g_original(cam);
                aim_reference = load_matrix(cam, kOffWcTransform);
                have_aim_reference = true;
            }

            // First person is resolved before anything is built: it builds
            // its camera itself, once, at the body heading
            // (first_person_drive_game_heading), and the levelled retail
            // build below was thrown away there every frame (review
            // 2026-10-02). Resolving reads only Lara, never these matrices.
            float first_person_position[3] = { 0.0f, 0.0f, 0.0f };
            bool first_person = false;
            if (main_camera)
            {
                if (camera_view_first_person() && drive)
                    first_person = first_person_resolve(
                        cam, mode, first_person_position);
                else if (g_first_person_active || g_first_person_instance)
                    first_person_leave();
            }

            Mat4 base_camera = Mat4::identity();
            if (!first_person)
            {
                if (level)
                {
                    rot[0] = 0.0f;
                    rot[1] = 0.0f;
                }

                g_original(cam);

                base_camera = load_matrix(cam, kOffWcTransform);

                rot[0] = saved[0];
                rot[1] = saved[1];
                rot[2] = saved[2];
            }

            if (first_person)
            {
                // The renderer uses the matrices below, but terrain and room
                // visibility also consult CameraCore::Position directly. If
                // it remains at the retail third-person chase-camera point,
                // nearby geometry can disappear as the HMD turns or moves
                // toward it. Keep both camera representations together.
                set_camera_position(cam, first_person_position);
                memcpy(g_core_last_write, first_person_position,
                       sizeof(g_core_last_write));
                g_core_last_write_valid = true;
                if (!g_first_person_camera_position_reported)
                {
                    g_first_person_camera_position_reported = true;
                    log("first-person: terrain camera position follows HMD "
                        "eye (%.1f, %.1f, %.1f)",
                        first_person_position[0], first_person_position[1],
                        first_person_position[2]);
                }

                // Put headset yaw into the game's persistent camera heading,
                // not merely the rendered matrix. The camera turns with the
                // player, and the actor yaw is updated below in the same
                // HMD-relative frame.
                first_person_drive_game_heading(
                    cam, rot, saved, level, &base_camera);
            }

            if (!have_aim_reference)
                aim_reference = base_camera;

            // Third-person presets: gameplay only (no authored cameras,
            // cutscenes or menus other than pause).
            float tp_position[3]{};
            bool tp_override = false;
            if (main_camera && drive && !first_person && !flat_screen &&
                mode != 4 && mode != 11 && mode != 12 &&
                !camera_cinematic_playing() &&
                !(ui_menu_active() && !ui_pause_menu_active()))
            {
                g_tp_board_scale = camera_third_person_mode() == 2;
                tp_override = third_person_camera_position(
                    cam, base_camera, tp_position);
            }
            else if (main_camera)
            {
                g_tp_board_scale = false;
                g_tp_camera.valid = false;
                g_tp_camera.anchor_valid = false;
            }
            if (main_camera && !tp_override)
                g_tp_board_scale = false;
            if (tp_override && drive && !flat_screen &&
                config().camera_collision)
                third_person_eye_collision(base_camera, tp_position);

            // The whole head pose, on the matrices the engine just built. Nothing
            // is left for the projection, and nothing has to be restored: these
            // are outputs, rebuilt from scratch on every call.
            if (drive && !flat_screen)
            {
                apply_head_pose(cam,
                    first_person ? first_person_position
                    : tp_override ? tp_position : nullptr);
                cull_rebuild_headset_volume(cam, first_person);
                if (main_camera)
                {
                    vr_note_main_camera_head_applied();
                    const float* cw = reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(cam) +
                        kOffCwTransform);
                    const float* f = reinterpret_cast<const float*>(
                        static_cast<const unsigned char*>(cam) +
                        kOffFocusPoint);
                    const float dx = f[0] - cw[12], dy = f[1] - cw[13],
                                dz = f[2] - cw[14];
                    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    g_marker_depth = std::isfinite(d) ? d : 0.0f;
                    if (tp_override && std::isfinite(cw[12]) &&
                        std::isfinite(cw[13]) && std::isfinite(cw[14]))
                    {
                        float* core = reinterpret_cast<float*>(
                            static_cast<unsigned char*>(cam) + kOffPosition);
                        memcpy(g_core_saved, core, sizeof(g_core_saved));
                        memcpy(g_core_written, cw + 12, sizeof(g_core_written));
                        memcpy(core, g_core_written, sizeof(g_core_written));
                        g_core_swapped = true;
                        memcpy(g_core_last_write, g_core_written,
                               sizeof(g_core_last_write));
                        g_core_last_write_valid = true;
                        static bool reported = false;
                        if (!reported)
                        {
                            reported = true;
                            log("third-person: terrain camera position "
                                "follows the drawn view for rendering (%.0f "
                                "units from the retail camera); restored "
                                "before each camera build and at Present",
                                sqrtf((cw[12] - g_core_saved[0]) *
                                      (cw[12] - g_core_saved[0]) +
                                      (cw[13] - g_core_saved[1]) *
                                      (cw[13] - g_core_saved[1]) +
                                      (cw[14] - g_core_saved[2]) *
                                      (cw[14] - g_core_saved[2])));
                        }
                    }
                }
                publish_head_aim(cam, aim_reference,
                                 load_matrix(cam, first_person
                                     ? kOffWcTransform2 : kOffWcTransform));

                // First person: the camera Euler heading already follows the
                // headset, but its pitch stayed the game's own, and retail
                // picks combat targets in a cone about that Euler -- enemies
                // above or below it went untargeted (user, 2026-10-04: no
                // target mid-combat, so the grapple would not throw). Give
                // gameplay the headset pitch. Only with a levelled build,
                // which ignores this pitch when drawing.
                const float scale = g_first_person_pitch_scale;
                if (main_camera && first_person && level &&
                    fabsf(scale) > 0.5f && fabsf(scale) < 2.0f)
                {
                    const float view = view_pitch_of(
                        load_matrix(cam, kOffWcTransform2));
                    const float wanted = view / scale;
                    if (std::isfinite(wanted))
                        rot[0] = fmaxf(-1.3f, fminf(1.3f, wanted));
                }
                if (main_camera)
                {
                    target_cone_update(first_person);
                }
            }

            if (main_camera)
            {
                if (mode != g_last_mode)
                {
                    const float* p = reinterpret_cast<const float*>(cam);
                    const float* f = reinterpret_cast<const float*>(
                        (const unsigned char*)cam + kOffFocusPoint);
                    const float dx = f[0] - p[0], dy = f[1] - p[1], dz = f[2] - p[2];
                    log("camera-mode: %d -> %d  focusDistance %.1f  "
                        "camera-to-focus %.1f  projection near %.1f",
                        (int)g_last_mode, (int)mode,
                        *reinterpret_cast<const float*>((const unsigned char*)cam +
                                                        kOffFocusDistance),
                        sqrtf(dx * dx + dy * dy + dz * dz),
                        *reinterpret_cast<const float*>((const unsigned char*)cam + 0x2D0));
                    g_last_mode = mode;
                }
            }

            ++g_calls;
            const unsigned now = GetTickCount();
            if (g_last_report == 0)
                g_last_report = now;
            if (now - g_last_report >= 5000)
            {
                float head[3];
                vr_head_position(head);
                const bool report_main_camera = cam == kMainCamera;
                const short report_mode = report_main_camera
                    ? *reinterpret_cast<const short*>((const unsigned char*)cam + kOffMode)
                    : (short)-1;
                const float focus = report_main_camera
                    ? *reinterpret_cast<const float*>((const unsigned char*)cam +
                                                       kOffFocusDistance)
                    : 0.0f;
                log("camera: %.0f rebuilds/s mode %d focus %.1f  head yaw %+.1f deg  "
                    "head at (%+.1f, %+.1f, %+.1f) units  turn x%.2f move x%.2f sep x%.2f%s%s",
                    g_calls * 1000.0f / (float)(now - g_last_report),
                    (int)report_mode, focus,
                    vr_head_yaw() * 57.29578f,
                    head[0], head[1], head[2],
                    tune_turn_scale(), tune_move_scale(), tune_ipd_scale(),
                     drive ? "" : "  [not driving the camera]",
                     level ? "  [horizon levelled]" : "");
                if (first_person && report_main_camera)
                {
                    log("first-person: heading engine %.1f, stick base %.1f, "
                        "HMD %.1f, combined %.1f deg",
                        saved[2] * 57.2957795f,
                        wrap_heading(g_first_person_base_heading +
                            g_first_person_turn_sign *
                            (vr_input_first_person_turn() -
                             g_first_person_stick_start)) * 57.2957795f,
                        first_person_hmd_heading() * 57.2957795f,
                        rot[2] * 57.2957795f);
                    __try
                    {
                        const unsigned char* player =
                            *reinterpret_cast<unsigned char* const*>(
                                0x0111713C);
                        const unsigned char* actor =
                            static_cast<const unsigned char*>(
                                g_first_person_instance);
                        if (player && actor)
                            log("first-person: movement steering mode %d, "
                                "magnitude %.2f, bearing %.1f deg, "
                                "actor heading %.1f deg",
                                *reinterpret_cast<const int*>(
                                    player + 0x74),
                                *reinterpret_cast<const float*>(
                                    player + 0x64),
                                *reinterpret_cast<const float*>(
                                    player + 0x6C) * 57.2957795f,
                                *reinterpret_cast<const float*>(
                                    actor + 0x38) * 57.2957795f);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        // Diagnostics cannot interrupt gameplay.
                    }
                }
                g_calls = 0;
                g_last_report = now;
            }
        }
        void __cdecl detour_setshake(void* cam, int amount, int frames)
        {
            if (tune_camera_shake())
            {
                g_setshake(cam, amount, frames);
                return;
            }

            // Counted rather than silently dropped, so the log can say
            // whether an effect that looked wrong was one of these.
            if ((++g_shakes_refused % 25u) == 1u)
                log("camera: %u screen shakes refused (latest %d over %d frames)",
                    g_shakes_refused, amount, frames);
        }

        // Retail playerDrawCombatLock returns without drawing when Lara's
        // combat state [[[player]+0x23C]+4]+4 is zero; 0x20 is accurate aim
        // (its centre crosshair), anything else draws lock-on reticles. The
        // game places those flat sprites from its own camera, and the HUD
        // policy then moves them again with the head, so in first person they
        // are replaced by the 3D crosshair drawn at the real impact point.
        // Every view uses the VR crosshair now (user 2026-10-06): first
        // person aims directly (controller ray impact), third person shows
        // the game's auto-target, captured from the lock-on rings retail is
        // no longer allowed to draw.
        void __cdecl detour_draw_combat_lock()
        {
            const int state = config().vr_crosshair
                ? retail_combat_state() : 0;
            if (!config().vr_crosshair)
            {
                g_draw_combat_lock();
                return;
            }
            static int state_was = -1;
            static unsigned state_reports = 0;
            if (state != state_was && state_reports < 60)
            {
                ++state_reports;
                log("aim: retail combat state 0x%02X (%s)", state,
                    camera_first_person_active() ? "first person"
                                                 : "third person");
            }
            state_was = state;
            if (state == 0)
            {
                // Out of combat retail draws no reticle; third person still
                // runs it for its other sense sprites.
                if (!camera_first_person_active())
                    g_draw_combat_lock();
                return;
            }
            // 0x30 is the caution indicator (danger sensed, guns away):
            // retail draws only the warning sprite. No crosshair -- it
            // showed whenever danger was near (user 2026-10-06).
            if (state == 0x30)
            {
                g_draw_combat_lock();
                return;
            }
            g_combat_reticle_time = GetTickCount();
            g_combat_reticle_accurate = state == 0x20;
            // First person: the retail function still runs for the caution
            // icon (missing in first person, user screenshots 2026-10-04);
            // only its lock-on rings are blocked. Precision aim draws just
            // its centre crosshair, which the VR crosshair replaces.
            if (state != 0x20 && g_draw_combat_reticle)
            {
                const DWORD captured_before = g_lock_target_time;
                g_block_combat_reticle = true;
                g_draw_combat_lock();
                g_block_combat_reticle = false;
                // Third person: the crosshair goes where the shot goes --
                // PlayerData+0x760, the weapon target playerUpdateTargetPos
                // just chose. The reticle retail draws is for its combat
                // lock target, which can be a different enemy than the
                // rocks Lara is actually aiming at (user screenshot
                // 2026-10-06). Failing that, the captured reticle, then
                // the combat target (environment targets draw none).
                float target[3];
                if (!camera_first_person_active())
                {
                    float unused[3];
                    g_lock_on_target =
                        g_lock_target_time != captured_before ||
                        combat_target_position(unused);
                }
                if (!camera_first_person_active() &&
                    weapon_target_position(target))
                {
                    memcpy(g_lock_target, target, sizeof(g_lock_target));
                    g_lock_target_time = GetTickCount();
                }
                else if (!camera_first_person_active() &&
                    g_lock_target_time == captured_before &&
                    combat_target_position(target))
                {
                    memcpy(g_lock_target, target, sizeof(g_lock_target));
                    g_lock_target_time = GetTickCount();
                    static bool reported = false;
                    if (!reported)
                    {
                        reported = true;
                        log("aim: third-person crosshair on a combat target "
                            "retail drew no reticle for (environment target)");
                    }
                }
            }
            static bool reported[2] = { false, false };
            const int view = camera_first_person_active() ? 0 : 1;
            if (!reported[view])
            {
                reported[view] = true;
                if (view == 0)
                    log("aim: retail combat reticles suppressed; VR crosshair "
                        "drawn at the %s impact point",
                        config().controller_aim ? "right-controller"
                                                : "headset");
                else
                    log("aim: third person -- retail lock-on reticles "
                        "replaced by the VR crosshair on the auto-target");
            }
        }

        void __cdecl detour_draw_combat_reticle(const void* position,
                                                bool a, bool b, float c,
                                                float d)
        {
            if (g_block_combat_reticle)
            {
                __try
                {
                    const float* p = static_cast<const float*>(position);
                    if (p && std::isfinite(p[0]) && std::isfinite(p[1]) &&
                        std::isfinite(p[2]))
                    {
                        g_lock_target[0] = p[0];
                        g_lock_target[1] = p[1];
                        g_lock_target[2] = p[2];
                        g_lock_target_time = GetTickCount();
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
                return;
            }
            g_draw_combat_reticle(position, a, b, c, d);
        }

        void __cdecl detour_end_combat()
        {
            // Retail playerInvProcess calls from the first two sites when its
            // ordinary input/idle timer would put weapons away. The third
            // site is its target/aim update: entering accurate aim calls
            // EndCombatMode there, which unequips the VR pistols and stops
            // fire. Keep that call from undoing an explicit VR holster draw.
            // Other callers (including the actual grip holster and scripted
            // transitions) still reach the original.
            const uintptr_t caller = reinterpret_cast<uintptr_t>(
                _ReturnAddress());
            if (g_end_combat && camera_first_person_active() &&
                vr_input_holster_combat_held() &&
                (caller == 0x005B1B68 || caller == 0x005B1D49 ||
                 caller == 0x005ACDD6))
            {
                static unsigned suppressed = 0;
                if (suppressed++ < 12)
                    log("controls: kept holster-drawn combat active "
                        "(retail end call %p)",
                        reinterpret_cast<void*>(caller));
                return;
            }
            if (g_end_combat)
            {
                g_end_combat();
                vr_input_pistol_combat_ended(caller);
            }
        }

        void __cdecl detour_autocenter(void* cam, float* multiplier,
                                       float* max_speed)
        {
            g_autocenter(cam, multiplier, max_speed);

            // The one place the mod is handed a whole Camera rather than just
            // its core, so the one place that can see what it is following.
            // Classic and shoulder follow camera_auto_center (shoulder used
            // to always auto-centre behind Lara; user 2026-10-06: rotate it
            // like classic); the board camera never swings on its own.
            const int tp_mode = camera_third_person_mode();
            if (!camera_first_person_active() && tp_mode != 2 &&
                tune_camera_auto_center())
                return;
            if (camera_first_person_active() && tune_camera_auto_center())
                return;

            if (multiplier) *multiplier = 0.0f;
            if (max_speed)  *max_speed  = 0.0f;
        }
    }

    float camera_stereo_scale()
    {
        return 1.0f;
    }

    bool camera_accurate_aim_active()
    {
        return g_accurate_aim_active;
    }

    void camera_view_prescale(float* x, float* y)
    {
        if (x) *x = g_view_prescale[0];
        if (y) *y = g_view_prescale[1];
    }

    bool camera_frame_positions(float retail[3], float eye[3],
                                float focus[3])
    {
        __try
        {
            const unsigned char* cam =
                static_cast<const unsigned char*>(kMainCamera);
            const float* cw = reinterpret_cast<const float*>(
                cam + kOffCwTransform);
            const float* f = reinterpret_cast<const float*>(
                cam + kOffFocusPoint);
            for (int k = 0; k < 3; ++k)
            {
                eye[k] = cw[12 + k];
                focus[k] = f[k];
                retail[k] = g_game_camera[k];
            }
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Whether retail currently has a target for a thrown item (its own
    // playerGetTarget, 0x005A8C00). With a gun out, the grapple is only
    // thrown when it does (log 2026-10-04).
    bool camera_grapple_has_target()
    {
        __try
        {
            return reinterpret_cast<void*(__cdecl*)()>(0x005A8C00)() !=
                   nullptr;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void camera_core_position_restore()
    {
        core_position_restore(const_cast<void*>(kMainCamera));
    }

    float camera_marker_depth()
    {
        return g_marker_depth;
    }

    float camera_world_scale_factor()
    {
        if (!g_tp_board_scale || g_first_person_active)
            return 1.0f;
        // Lara is about 1.75 m at world_scale; shown board_lara_height_cm.
        const float factor = 175.0f / config().board_lara_height_cm;
        return std::isfinite(factor) && factor > 0.5f ? factor : 1.0f;
    }

    namespace
    {
        // Let Lara go at a point: she is moved there (retail teleport,
        // which snaps to ground only within ~96 units) and the game's own
        // physics drops her from it (user request 2026-10-03: fall, do not
        // teleport to the floor). Only over a floor -- never into a void.
        bool board_put_down(void* lara, const float* over)
        {
            const float metre = config().world_scale;
            const float origin[3] = { over[0], over[1], over[2] + 0.5f * metre };
            const float down[3] = { 0.0f, 0.0f, -1.0f };
            float hit[3]{};
            if (!aim_probe(origin, down, 60.0f * metre, hit, nullptr))
                return false;
            alignas(16) float place[4] = { over[0], over[1],
                                           fmaxf(over[2], hit[2]), 1.0f };
            using PFN_SetPositionFindGround =
                void(__cdecl*)(void*, const float*);
            __try
            {
                reinterpret_cast<PFN_SetPositionFindGround>(0x004574C0)(
                    lara, place);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }
    }

    namespace
    {
        // Lara airborne: retail JumpState (vtable 0x00F05CEC) or FallState
        // (0x00F05D00) at the top of her state chain.
        bool lara_airborne(void* lara)
        {
            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!player || !lara)
                    return false;
                const unsigned char* data = player - 0x70;
                if (*reinterpret_cast<void* const*>(data) != lara)
                    return false;
                const unsigned char* state =
                    *reinterpret_cast<unsigned char* const*>(data + 4);
                if (!state)
                    return false;
                const uintptr_t vtable =
                    *reinterpret_cast<const uintptr_t*>(state);
                return vtable == 0x00F05CEC || vtable == 0x00F05D00;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Put her real instance on the floor under (x, y): used when she
        // lands on a palm from a height, so the real body does not finish
        // a long fall (and its damage) out of sight.
        void board_ground_real(void* lara, const float* at)
        {
            const float metre = config().world_scale;
            const float origin[3] = { at[0], at[1], at[2] + 0.5f * metre };
            const float down[3] = { 0.0f, 0.0f, -1.0f };
            float hit[3]{};
            if (aim_probe(origin, down, 60.0f * metre, hit, nullptr))
                board_put_down(lara, hit);
        }
    }

    void camera_board_pluck_update(bool active, const bool grip[2],
                                   bool claimed[2])
    {
        claimed[0] = claimed[1] = false;
        const float factor = camera_world_scale_factor();
        void* lara = nullptr;
        bool ok = active && factor > 1.001f && !g_first_person_active &&
                  config().immersive_controls && config().board_god_hand;
        __try
        {
            lara = *reinterpret_cast<void**>(kPlayerInstance);
            if (lara)
                memcpy(g_pluck.root, static_cast<unsigned char*>(lara) + 0x10,
                       sizeof(g_pluck.root));
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            lara = nullptr;
        }
        for (int k = 0; k < 3; ++k)
            ok = ok && std::isfinite(g_pluck.root[k]);
        ok = ok && lara;
        if (!ok)
        {
            if (g_pluck.hand >= 0)
            {
                log("board: pick-up dropped (left board gameplay); Lara "
                    "stays where she was");
                dangle_restore(lara);
            }
            if (g_pluck.palm >= 0)
                log("board: palm platform ended (left board gameplay)");
            g_pluck.palm = -1;
            g_pluck.hand = -1;
            g_pluck.hover = false;
            for (int h = 0; h < 2; ++h)
            {
                g_pluck.was[h] = grip[h];
                g_pluck.inside[h] = false;
            }
            for (float& c : g_pluck.carry)
                c = 0.0f;
            return;
        }
        Mat4 camera_to_world;
        memcpy(&camera_to_world.m[0][0],
               static_cast<const unsigned char*>(kMainCamera) +
                   kOffCwTransform, sizeof(camera_to_world.m));
        const float metre = config().world_scale;
        const float table = metre * factor;
        const float radius = 0.30f * metre + 0.04f * table;
        // Palm platform upkeep: stand on the palm while it faces up
        // (hysteresis 0.55); turned over, she drops to the floor below.
        if (g_pluck.palm >= 0 && g_pluck.hand < 0)
        {
            const int ph = g_pluck.palm;
            const float up = palm_up(ph);
            float stand[3];
            const PalmSeen& seen = g_palm_seen[ph];
            // She may walk (her real instance walks where she was picked
            // up from); her steps move her across the palm, and past its
            // edge she steps off and falls.
            g_pluck.palm_offset[0] += g_pluck.root[0] - g_pluck.palm_last_root[0];
            g_pluck.palm_offset[1] += g_pluck.root[1] - g_pluck.palm_last_root[1];
            g_pluck.palm_last_root[0] = g_pluck.root[0];
            g_pluck.palm_last_root[1] = g_pluck.root[1];
            const float edge = 0.055f * table;
            const bool off_edge =
                g_pluck.palm_offset[0] * g_pluck.palm_offset[0] +
                g_pluck.palm_offset[1] * g_pluck.palm_offset[1] > edge * edge;
            // Jump/crouch on the palm: crouch is her own animation; a jump
            // lifts her by her real rise above the floor she jumped from.
            const bool airborne = lara_airborne(lara);
            if (!airborne)
                g_pluck.palm_base_z = g_pluck.root[2];
            const float lift = airborne
                ? fmaxf(0.0f, g_pluck.root[2] - g_pluck.palm_base_z) : 0.0f;
            stand[0] = seen.point[0] + g_pluck.palm_offset[0];
            stand[1] = seen.point[1] + g_pluck.palm_offset[1];
            stand[2] = seen.point[2] + 0.004f * table + lift;
            if (up > 0.55f && !off_edge)
            {
                for (int k = 0; k < 3; ++k)
                    g_pluck.carry[k] = stand[k] - g_pluck.root[k];
            }
            else
            {
                // Where she stands now (the palm, or her last drawn spot
                // when the hand is gone).
                if (up < -1.5f)
                    for (int k = 0; k < 3; ++k)
                        stand[k] = g_pluck.root[k] + g_pluck.carry[k];
                const bool placed = board_put_down(lara, stand);
                log("board: %s (up %.2f) -- Lara %s",
                    off_edge ? (airborne ? "jumped off the palm"
                                         : "walked off the palm")
                             : "palm turned over",
                    up, placed ? "falls from there"
                               : "stays where she was (no floor)");
                vr_input_weapon_haptic(ph == 0, 0.3f);
                g_pluck.palm = -1;
                g_pluck.palm_left_at = GetTickCount();
                for (float& c : g_pluck.carry)
                    c = 0.0f;
            }
        }
        // Onto the palm unaided: an upturned palm she walks onto (its top
        // within a step of her feet), or lands on from above.
        if (g_pluck.palm < 0 && g_pluck.hand < 0 &&
            GetTickCount() - g_pluck.palm_left_at > 1000)
        {
            const bool airborne = lara_airborne(lara);
            for (int h = 0; h < 2; ++h)
            {
                if (palm_up(h) <= 0.7f)
                    continue;
                const PalmSeen& seen = g_palm_seen[h];
                const float ox = g_pluck.root[0] - seen.point[0];
                const float oy = g_pluck.root[1] - seen.point[1];
                const float reach = 0.05f * table;
                if (ox * ox + oy * oy > reach * reach)
                    continue;
                const float top = seen.point[2] + 0.004f * table;
                const float rise = top - g_pluck.root[2];
                const bool step = !airborne && rise > -0.15f * metre &&
                                  rise < 0.40f * metre;
                const bool land = airborne && g_pluck.last_root_valid &&
                    g_pluck.last_root_z >= top &&
                    g_pluck.root[2] < top + 0.05f * metre;
                if (!step && !land)
                    continue;
                if (land)
                    board_ground_real(lara, g_pluck.root);
                g_pluck.palm = h;
                g_pluck.palm_offset[0] = ox;
                g_pluck.palm_offset[1] = oy;
                g_pluck.palm_last_root[0] = g_pluck.root[0];
                g_pluck.palm_last_root[1] = g_pluck.root[1];
                g_pluck.palm_base_z = g_pluck.root[2];
                for (int k = 0; k < 2; ++k)
                    g_pluck.carry[k] = seen.point[k] +
                        g_pluck.palm_offset[k] - g_pluck.root[k];
                g_pluck.carry[2] = top - g_pluck.root[2];
                vr_input_weapon_haptic(h == 0, 0.4f);
                log("board: Lara %s the %s palm by herself",
                    land ? "landed on" : "stepped onto",
                    h == 0 ? "left" : "right");
                break;
            }
        }
        g_pluck.last_root_z = g_pluck.root[2];
        g_pluck.last_root_valid = true;
        float zone[3];
        for (int k = 0; k < 3; ++k)
            zone[k] = g_pluck.root[k] +
                (g_pluck.palm >= 0 ? g_pluck.carry[k] : 0.0f);
        const float bottom = zone[2] - 0.03f * table;
        const float top = zone[2] + 1.8f * metre + 0.04f * table;
        g_pluck.hover = false;
        for (int h = 0; h < 2; ++h)
        {
            const bool left = h == 0;
            const bool pressed = grip[h] && !g_pluck.was[h];
            const bool released = !grip[h] && g_pluck.was[h];
            g_pluck.was[h] = grip[h];
            Mat4 to_head;
            if (!vr_controller_head_pose(left, &to_head))
            {
                g_pluck.inside[h] = false;
                continue;
            }
            const Mat4 world = to_head * camera_to_world;
            const float* p = world.m[3];
            const float dx = p[0] - zone[0];
            const float dy = p[1] - zone[1];
            // The palm she stands on cannot also pinch her.
            const bool inside = dx * dx + dy * dy <= radius * radius &&
                                p[2] >= bottom && p[2] <= top &&
                                g_pluck.palm != h;
            if (inside && !g_pluck.inside[h] && g_pluck.hand < 0)
                vr_input_handhold_haptic(left);
            g_pluck.inside[h] = inside;
            if (inside && g_pluck.hand < 0)
                g_pluck.hover = true;
            if (g_pluck.hand < 0 && pressed && inside)
            {
                if (g_pluck.palm >= 0)
                    log("board: Lara picked up off the %s palm",
                        g_pluck.palm == 0 ? "left" : "right");
                g_pluck.palm = -1;
                g_pluck.hand = h;
                for (int k = 0; k < 3; ++k)
                {
                    g_pluck.grab_offset[k] = g_pluck.root[k] - p[k];
                    g_pluck.last_hand[k] = p[k];
                    g_pluck.hand_vel[k] = 0.0f;
                }
                for (int k = 0; k < 2; ++k)
                {
                    g_pluck.tilt[k] = g_pluck.tilt_vel[k] = 0.0f;
                    g_pluck.hand_acc[k] = 0.0f;
                    for (auto& limb : g_pluck.limb_tilt)
                        limb[k] = 0.0f;
                }
                vr_input_weapon_haptic(left, 0.5f);
                log("board: %s hand picked Lara up at (%.0f, %.0f, %.0f)",
                    left ? "left" : "right", g_pluck.root[0],
                    g_pluck.root[1], g_pluck.root[2]);
            }
            if (g_pluck.hand != h)
                continue;
            claimed[h] = true;
            dangle_maintain(lara);
            const bool wrist_pivot = g_dangle_active && g_pivot_valid;
            for (int a = 0; a < 3; ++a)
            {
                float axis[3] = { world.m[a][0], world.m[a][1],
                                  world.m[a][2] };
                normalise3(axis);
                for (int k = 0; k < 3; ++k)
                    g_pluck.hand_axes[a][k] = axis[k];
            }
            for (int k = 0; k < 3; ++k)
                g_pluck.hand_pos[k] = p[k];
            float hang_at[3];
            hang_point(hang_at);
            for (int k = 0; k < 3; ++k)
            {
                g_pluck.target[k] = hang_at[k] + (wrist_pivot
                    ? g_pluck.root[k] - g_pivot[k] : g_pluck.grab_offset[k]);
                g_pluck.carry[k] = g_pluck.target[k] - g_pluck.root[k];
            }
            // Dangle: a pendulum of a doll-sized length hanging from the
            // grab point, driven by the hand's horizontal acceleration in
            // real metres; the limbs follow on slower, lagging pendulums
            // with a little idle sway.
            {
                static LARGE_INTEGER last{};
                LARGE_INTEGER now{}, frequency{};
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&frequency);
                float dt = last.QuadPart && frequency.QuadPart
                    ? (float)((double)(now.QuadPart - last.QuadPart) /
                              (double)frequency.QuadPart) : 0.0f;
                last = now;
                if (!(dt > 0.0f) || dt > 0.1f)
                    dt = 0.0f;
                if (dt > 0.0f)
                {
                    const float to_metres = 1.0f / table;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float v = (p[k] - g_pluck.last_hand[k]) / dt *
                                        to_metres;
                        if (k < 2)
                        {
                            const float a = (v - g_pluck.hand_vel[k]) / dt;
                            const float smooth = dt / (dt + 0.03f);
                            g_pluck.hand_acc[k] += (a - g_pluck.hand_acc[k]) *
                                                   smooth;
                        }
                        g_pluck.hand_vel[k] = v;
                        g_pluck.last_hand[k] = p[k];
                    }
                    const float length = 0.09f;   // metres, doll scale
                    const float gravity = 9.81f;
                    for (int k = 0; k < 2; ++k)
                    {
                        const float accel = -gravity / length *
                            sinf(g_pluck.tilt[k]) -
                            3.0f * g_pluck.tilt_vel[k] -
                            g_pluck.hand_acc[k] / length;
                        g_pluck.tilt_vel[k] += accel * dt;
                        g_pluck.tilt[k] += g_pluck.tilt_vel[k] * dt;
                        if (g_pluck.tilt[k] > 1.2f) g_pluck.tilt[k] = 1.2f;
                        if (g_pluck.tilt[k] < -1.2f) g_pluck.tilt[k] = -1.2f;
                    }
                    g_pluck.phase += dt * 6.2831853f * 1.3f;
                    const float lag[4] = { 0.16f, 0.16f, 0.28f, 0.28f };
                    for (int l = 0; l < 4; ++l)
                        for (int k = 0; k < 2; ++k)
                        {
                            const float follow = dt / (dt + lag[l]);
                            g_pluck.limb_tilt[l][k] += (g_pluck.tilt[k] -
                                g_pluck.limb_tilt[l][k]) * follow;
                        }
                }
                auto hang = [](float tx, float ty, float out[3]) {
                    out[0] = sinf(tx);
                    out[1] = sinf(ty);
                    const float rest = 1.0f - out[0] * out[0] -
                                       out[1] * out[1];
                    out[2] = -sqrtf(rest > 0.05f ? rest : 0.05f);
                };
                const float down[3] = { 0.0f, 0.0f, -1.0f };
                float body[3];
                hang(g_pluck.tilt[0], g_pluck.tilt[1], body);
                g_pluck.body_rotation = rotation_between(down, body);
                for (int l = 0; l < 4; ++l)
                {
                    const float idle = 0.06f * sinf(g_pluck.phase + 1.7f * l);
                    float limb[3];
                    hang(g_pluck.limb_tilt[l][0] + idle,
                         g_pluck.limb_tilt[l][1] - 0.5f * idle, limb);
                    g_pluck.limb_rotation[l] = rotation_between(body, limb);
                }
            }
            // Floor under the held Lara: probe straight down from a little
            // above her feet (she may be pushed slightly into a surface).
            const float origin[3] = { g_pluck.target[0], g_pluck.target[1],
                                      g_pluck.target[2] + 0.5f * metre };
            const float down[3] = { 0.0f, 0.0f, -1.0f };
            float hit[3]{};
            g_pluck.floor_valid = aim_probe(origin, down, 60.0f * metre,
                                            hit, nullptr);
            if (g_pluck.floor_valid)
                for (int k = 0; k < 3; ++k)
                    g_pluck.floor[k] = hit[k];
            if (!released)
                continue;
            {
                const int other = 1 - h;
                const float up = palm_up(other);
                const PalmSeen& seen = g_palm_seen[other];
                // Her feet: the carried root.
                const float fx = g_pluck.root[0] + g_pluck.carry[0];
                const float fy = g_pluck.root[1] + g_pluck.carry[1];
                const float fz = g_pluck.root[2] + g_pluck.carry[2];
                const float ox = fx - seen.point[0];
                const float oy = fy - seen.point[1];
                const float reach = 0.07f * table;
                static int reported = 0;
                if (up > -1.5f && reported < 6)
                {
                    ++reported;
                    log("board: release near the %s palm: up %.2f, %.2f "
                        "table-m across, feet %.2f table-m above it",
                        other == 0 ? "left" : "right", up,
                        sqrtf(ox * ox + oy * oy) / table,
                        (fz - seen.point[2]) / table);
                }
                if (up > 0.7f && ox * ox + oy * oy <= reach * reach &&
                    fz > seen.point[2] - 0.05f * table &&
                    fz < seen.point[2] + 0.25f * table)
                {
                    g_pluck.palm = other;
                    g_pluck.palm_last_root[0] = g_pluck.root[0];
                    g_pluck.palm_last_root[1] = g_pluck.root[1];
                    g_pluck.palm_base_z = g_pluck.root[2];
                    // Keep her near the middle of the hand.
                    const float keep = 0.03f * table;
                    const float d = sqrtf(ox * ox + oy * oy);
                    const float s = d > keep ? keep / d : 1.0f;
                    g_pluck.palm_offset[0] = ox * s;
                    g_pluck.palm_offset[1] = oy * s;
                    for (int k = 0; k < 2; ++k)
                        g_pluck.carry[k] = seen.point[k] +
                            g_pluck.palm_offset[k] - g_pluck.root[k];
                    g_pluck.carry[2] = seen.point[2] + 0.004f * table -
                                       g_pluck.root[2];
                    vr_input_weapon_haptic(other == 0, 0.4f);
                    log("board: Lara set down on the %s palm",
                        other == 0 ? "left" : "right");
                    dangle_restore(lara);
                    g_pluck.hand = -1;
                    continue;
                }
            }
            if (g_pluck.floor_valid)
            {
                // Dropped from her feet as drawn (the carried root).
                if (board_put_down(lara, g_pluck.target))
                    log("board: Lara dropped from %.2f table-m above the "
                        "floor, %.2f table-m from where she was picked up",
                        (g_pluck.target[2] - g_pluck.floor[2]) / table,
                        sqrtf((g_pluck.target[0] - g_pluck.root[0]) *
                              (g_pluck.target[0] - g_pluck.root[0]) +
                              (g_pluck.target[1] - g_pluck.root[1]) *
                              (g_pluck.target[1] - g_pluck.root[1])) / table);
                else
                    log("board: drop failed; Lara stays");
            }
            else
                log("board: released over no floor; Lara stays where she "
                    "was");
            vr_input_weapon_haptic(left, 0.3f);
            dangle_restore(lara);
            g_pluck.hand = -1;
            for (float& c : g_pluck.carry)
                c = 0.0f;
        }
    }

    // Board mode finger flick (left hand, Y long press held = loaded,
    // released = snap). A snap whose index tip sweeps through mini Lara
    // knocks her away along the flick (she lands and falls as from a
    // drop); with board_flick_anim set, that animation plays as well.
    void camera_board_flick_update(bool active, bool held)
    {
        if (g_flick_anim == -2)
        {
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            g_flick_anim = (int)GetPrivateProfileIntW(L"vr",
                L"board_flick_anim", -1, path);
            if (g_flick_anim < -1)
                g_flick_anim = -1;
            g_flick_anim_front = (int)GetPrivateProfileIntW(L"vr",
                L"board_flick_anim_front", 140, path);
            g_flick_anim_back = (int)GetPrivateProfileIntW(L"vr",
                L"board_flick_anim_back", 139, path);
        }
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        float dt = g_flick.last.QuadPart && frequency.QuadPart
            ? (float)((double)(now.QuadPart - g_flick.last.QuadPart) /
                      (double)frequency.QuadPart) : 0.0f;
        g_flick.last = now;
        dt = fminf(fmaxf(dt, 0.0f), 0.1f);
        // A knockdown animation runs once: at its end, back to the
        // animation she had (unless her own state already moved on).
        if (g_flick.knock_active && now.QuadPart >= g_flick.knock_until.QuadPart)
        {
            g_flick.knock_active = false;
            __try
            {
                void* lara = *reinterpret_cast<void**>(kPlayerInstance);
                void* current = lara ? lara_current_keylist(lara) : nullptr;
                if (lara && g_flick.knock_restore >= 0 &&
                    current == g_flick.knock_keylist)
                {
                    lara_play_animation(lara, g_flick.knock_restore, true);
                    log("board: knockdown finished; animation %d restored",
                        g_flick.knock_restore);
                }
                else if (lara)
                    log("board: knockdown ended by her own state (animation "
                        "%d now); nothing restored",
                        lara_anim_index_of(lara, current));
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        const bool board = active && camera_world_scale_factor() > 1.001f &&
            !g_first_person_active && config().immersive_controls &&
            config().board_god_hand && g_pluck.hand != 0;
        if (!board)
        {
            g_flick.phase = 0;
            g_flick.amount = g_flick.fist = 0.0f;
            return;
        }
        if (g_flick.phase == 0 && held)
        {
            g_flick.phase = 1;
            g_flick.tip_valid = false;
        }
        if (g_flick.phase == 1)
        {
            const float follow = 1.0f - expf(-dt / 0.08f);
            g_flick.amount += (1.0f - g_flick.amount) * follow;
            g_flick.fist += (1.0f - g_flick.fist) * follow;
            if (!held)
            {
                g_flick.phase = 2;
                g_flick.started = now;
                g_flick.hit_done = false;
                vr_input_weapon_haptic(true, 0.15f);
            }
            return;
        }
        if (g_flick.phase != 2)
            return;
        const float e = frequency.QuadPart
            ? (float)((double)(now.QuadPart - g_flick.started.QuadPart) /
                      (double)frequency.QuadPart) : 1.0f;
        // Snap straight in 40 ms, hold the flicked finger 1.5 s, then relax
        // back to the idle hand over 0.3 s (user, 2026-10-03). Holding Y
        // again after the hit window loads another flick.
        const float straight = -0.15f;
        if (e < 0.04f)
        {
            g_flick.amount = 1.0f + (straight - 1.0f) * (e / 0.04f);
            g_flick.fist = 1.0f;
        }
        else if (e < 1.5f)
        {
            g_flick.amount = straight;
            g_flick.fist = 1.0f;
        }
        else
        {
            const float w = fmaxf(0.0f, 1.0f - (e - 1.5f) / 0.3f);
            g_flick.amount = straight * w;
            g_flick.fist = w;
        }
        if (e >= 1.8f)
        {
            g_flick.phase = 0;
            g_flick.amount = g_flick.fist = 0.0f;
        }
        else if (held && e > 0.15f)
        {
            g_flick.phase = 1;
            g_flick.tip_valid = false;
            return;
        }
        if (g_flick.hit_done || e > 0.15f || !g_flick.tip_valid ||
            GetTickCount() - g_flick.tip_time > 100)
            return;

        void* lara = nullptr;
        float feet[3]{};
        __try
        {
            lara = *reinterpret_cast<void**>(kPlayerInstance);
            if (lara)
                memcpy(feet, static_cast<unsigned char*>(lara) + 0x10,
                       sizeof(feet));
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            lara = nullptr;
        }
        if (!lara)
            return;
        if (g_pluck.palm >= 0)
            for (int k = 0; k < 3; ++k)
                feet[k] += g_pluck.carry[k];
        const float metre = config().world_scale;
        const float table = metre * camera_world_scale_factor();
        // Her body: a vertical capsule 1.8 m of her own scale tall,
        // widened a little at table scale for a fingertip.
        const float radius = 0.30f * metre + 0.012f * table;
        float closest = 1e30f;
        float hit_point[3]{};
        for (int s = 0; s <= 8; ++s)
        {
            const float u = s / 8.0f;
            float q[3];
            for (int k = 0; k < 3; ++k)
                q[k] = g_flick.last_tip[k] +
                       (g_flick.tip[k] - g_flick.last_tip[k]) * u;
            if (q[2] < feet[2] - 0.01f * table ||
                q[2] > feet[2] + 1.8f * metre + 0.01f * table)
                continue;
            const float dx = q[0] - feet[0], dy = q[1] - feet[1];
            const float d = sqrtf(dx * dx + dy * dy);
            if (d < closest)
            {
                closest = d;
                memcpy(hit_point, q, sizeof(q));
            }
        }
        if (!(closest <= radius))
            return;
        g_flick.hit_done = true;
        // Direction: the tip's horizontal motion, else away from the tip.
        float dir[2] = { g_flick.tip[0] - g_flick.last_tip[0],
                         g_flick.tip[1] - g_flick.last_tip[1] };
        float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        if (len < 1e-3f)
        {
            dir[0] = feet[0] - hit_point[0];
            dir[1] = feet[1] - hit_point[1];
            len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        }
        if (len < 1e-3f)
        {
            dir[0] = 1.0f;
            dir[1] = 0.0f;
            len = 1.0f;
        }
        dir[0] /= len;
        dir[1] /= len;
        // Knocked 1.5 m (her scale) along it, short of any wall, and
        // lifted 0.4 m so she drops (no lift with a chosen animation).
        // Front or back: the flick against or along her facing (heading
        // vector (sin yaw, -cos yaw)).
        int anim = g_flick_anim_front;
        bool from_behind = false;
        __try
        {
            const float yaw = *reinterpret_cast<const float*>(
                static_cast<unsigned char*>(lara) + kOffInstanceHeading);
            if (std::isfinite(yaw))
                // Confirmed in the headset (2026-10-03, after one swap
                // each way).
                from_behind = dir[0] * sinf(yaw) - dir[1] * cosf(yaw) > 0.0f;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (from_behind)
            anim = g_flick_anim_back;
        const bool animated = anim >= 0;
        // The knockdown clips carry their own travel (user: they moved her
        // a lot on top of the shove), so with one she is not shoved.
        float push = animated ? 0.0f : 1.5f * metre;
        if (push > 0.0f)
        {
            const float from[3] = { feet[0], feet[1], feet[2] + 0.5f * metre };
            const float ray[3] = { dir[0], dir[1], 0.0f };
            float wall[3]{};
            if (aim_probe(from, ray, push + 0.3f * metre, wall, nullptr))
            {
                const float wx = wall[0] - from[0], wy = wall[1] - from[1];
                push = fmaxf(0.0f, sqrtf(wx * wx + wy * wy) - 0.3f * metre);
            }
        }
        const float to[3] = { feet[0] + dir[0] * push,
                              feet[1] + dir[1] * push,
                              feet[2] + (animated ? 0.0f : 0.4f * metre) };
        if (g_pluck.palm >= 0)
        {
            g_pluck.palm = -1;
            for (float& c : g_pluck.carry)
                c = 0.0f;
        }
        const bool moved = !animated || push > 0.0f
            ? board_put_down(lara, to) : true;
        if (animated && moved)
        {
            const int previous =
                lara_anim_index_of(lara, lara_current_keylist(lara));
            void* keylist = lara_keylist(lara, anim);
            // Played once (G2 emulation mode 1 = no looping; 2 loops, 0
            // pauses), holding its last frame; her previous animation
            // returns after 0.65 s (the user's tuning, 2026-10-03).
            const float length = 0.65f;
            const bool played = lara_play_animation(lara, anim, false);
            if (played)
            {
                __try
                {
                    const unsigned char* data =
                        *reinterpret_cast<unsigned char* const*>(
                            static_cast<unsigned char*>(lara) + 0xF4);
                    const unsigned char* processor = data
                        ? *reinterpret_cast<unsigned char* const*>(data + 4)
                        : nullptr;
                    const int sections = processor ? processor[0x150] : 0;
                    for (int s = 0; s < sections && s < 8; ++s)
                        reinterpret_cast<PFN_SetMode>(0x004DED90)(lara, s, 1);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            if (played && keylist && previous >= 0 && previous != anim)
            {
                g_flick.knock_active = true;
                g_flick.knock_keylist = keylist;
                g_flick.knock_restore = previous;
                g_flick.knock_until.QuadPart = now.QuadPart +
                    (LONGLONG)(length * (double)frequency.QuadPart);
            }
            log("board: knockdown %d once (no loop), animation %d back "
                "after %.1f s", anim, previous, length);
        }
        vr_input_weapon_haptic(true, 0.7f);
        log("board: flick hit Lara from %s (tip %.2f table-m from her "
            "axis) -- knocked %.2f m (her scale), animation %d %s",
            from_behind ? "behind" : "the front", closest / table,
            push / metre, anim,
            animated ? (moved ? "played" : "skipped") : "none");
    }

    bool camera_board_pluck_held()
    {
        return g_pluck.hand >= 0 || g_pluck.palm >= 0;
    }

    bool camera_board_pluck_carried()
    {
        return g_pluck.hand >= 0;
    }

    void camera_pose_debug_key(int key)
    {
        // 20/21: next/previous flick animation, previewed on Lara and saved
        // as board_flick_anim (-1 = none: the knock is a shove and fall).
        if (key == 20 || key == 21)
        {
            camera_board_flick_update(false, false);   // loads the INI value
            void* lara = nullptr;
            __try
            {
                lara = *reinterpret_cast<void**>(kPlayerInstance);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                lara = nullptr;
            }
            const int n = lara ? lara_anim_count(lara) : 0;
            if (n <= 0)
                return;
            int next = g_flick_anim;
            for (int tries = 0; tries < n; ++tries)
            {
                next = key == 20 ? next + 1 : next - 1;
                if (next >= n)
                    next = -1;
                if (next < -1)
                    next = n - 1;
                if (next == -1 || lara_keylist(lara, next))
                    break;
            }
            g_flick_anim = next;
            if (next >= 0)
                lara_play_animation(lara, next, false);
            wchar_t path[MAX_PATH]{}, value[16]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            swprintf_s(value, L"%d", next);
            WritePrivateProfileStringW(L"vr", L"board_flick_anim", value, path);
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("board: flick animation %d of %d%s; saved", next, n,
                next < 0 ? " (none: shove and fall)" : " previewed");
            return;
        }
        // 10..15: hang offset -x +x -y +y -z +z (5 mm), 16: reset.
        if (key >= 10 && key <= 16)
        {
            hang_load();
            if (key == 16)
                g_hang_offset[0] = g_hang_offset[1] = g_hang_offset[2] = 0.0f;
            else
            {
                const int axis = (key - 10) / 2;
                // Engine axes: x right, y DOWN, z forward -- numpad 8 is up.
                float step = (key % 2) ? 0.005f : -0.005f;
                if (axis == 1)
                    step = -step;
                g_hang_offset[axis] += step;
            }
            wchar_t path[MAX_PATH]{}, value[32]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            const wchar_t* keys[3] = { L"board_hang_x", L"board_hang_y",
                                       L"board_hang_z" };
            for (int k = 0; k < 3; ++k)
            {
                swprintf_s(value, L"%.3f", g_hang_offset[k]);
                WritePrivateProfileStringW(L"vr", keys[k], value, path);
            }
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("board: hang offset (%+.3f, %+.3f, %+.3f) m along the hand "
                "(right, down, forward); saved", g_hang_offset[0], g_hang_offset[1],
                g_hang_offset[2]);
            return;
        }
        if (key == 0)
        {
            ++g_finger_debug_chain;
            g_finger_debug_curl = true;
            log("finger debug: next chain (%d)", g_finger_debug_chain % 8 + 1);
        }
        else if (key == 1)
        {
            g_finger_debug_curl = !g_finger_debug_curl;
            g_finger_debug_reported[0] = g_finger_debug_reported[1] = ~0u;
            log("finger debug: curl %s", g_finger_debug_curl ? "ON" : "OFF");
        }
        else if (key == 2)
        {
            g_finger_debug_axis = (g_finger_debug_axis + 1) % 6;
            g_finger_debug_curl = true;
            log("finger debug: axis %c%c", "xyz"[g_finger_debug_axis / 2],
                g_finger_debug_axis % 2 ? '-' : '+');
        }
        else if (key == 3 || key == 4)
        {
            dangle_load();
            void* lara = nullptr;
            __try
            {
                lara = *reinterpret_cast<void**>(kPlayerInstance);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                lara = nullptr;
            }
            const int n = lara ? lara_anim_count(lara) : 0;
            if (n <= 0)
            {
                log("board: dangle animation -- Lara's animation count is "
                    "unavailable");
                return;
            }
            int next = g_dangle_anim;
            for (int tries = 0; tries < n; ++tries)
            {
                next = key == 3 ? next + 1 : next - 1;
                if (next >= n)
                    next = -1;
                if (next < -1)
                    next = n - 1;
                if (next == -1 || lara_keylist(lara, next))
                    break;
            }
            g_dangle_anim = next;
            g_dangle_force = true;
            g_pivot_wrist = -1;
            g_pivot_valid = false;
            if (next == -1 && g_dangle_active)
                dangle_restore(lara);
            wchar_t path[MAX_PATH]{}, value[16]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            swprintf_s(value, L"%d", next);
            WritePrivateProfileStringW(L"vr", L"board_dangle_anim", value,
                                       path);
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("board: dangle animation %d of %d%s (saved as "
                "board_dangle_anim)", next, n,
                next < 0 ? " = none (her own animation)" : "");
        }
    }

    int camera_board_pluck_ring(float points[24][3])
    {
        int state = 0;
        float centre[3]{};
        float radius = 0.0f;
        const float metre = config().world_scale;
        if (g_pluck.hand >= 0)
        {
            state = g_pluck.floor_valid ? 2 : 3;
            for (int k = 0; k < 3; ++k)
                centre[k] = g_pluck.floor_valid ? g_pluck.floor[k]
                                                : g_pluck.target[k];
            radius = 0.25f * metre;
        }
        else if (g_pluck.hover)
        {
            state = 1;
            for (int k = 0; k < 3; ++k)
                centre[k] = g_pluck.root[k] +
                    (g_pluck.palm >= 0 ? g_pluck.carry[k] : 0.0f);
            radius = 0.30f * metre;
        }
        if (!state || !points)
            return 0;
        Mat4 camera_to_world;
        memcpy(&camera_to_world.m[0][0],
               static_cast<const unsigned char*>(kMainCamera) +
                   kOffCwTransform, sizeof(camera_to_world.m));
        const Mat4 world_to_head = rigid_inverse(camera_to_world);
        for (int i = 0; i < 24; ++i)
        {
            const float a = 6.2831853f * i / 24.0f;
            const float w[3] = { centre[0] + radius * cosf(a),
                                 centre[1] + radius * sinf(a),
                                 centre[2] + 2.0f };
            for (int axis = 0; axis < 3; ++axis)
            {
                float v = world_to_head.m[3][axis];
                for (int r = 0; r < 3; ++r)
                    v += w[r] * world_to_head.m[r][axis];
                points[i][axis] = v;
            }
        }
        return state;
    }

    bool camera_cinematic_playing()
    {
        __try
        {
            return reinterpret_cast<PFN_CinematicPlaying>(
                       kCinematicPlaying)() ||
                   reinterpret_cast<PFN_CinematicPlaying>(
                       kResidentCinematicPlaying)();
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool camera_first_person_active()
    {
        return g_first_person_active;
    }

    namespace
    {
        int g_view_first_person = -1;   // -1: not read from config yet
        int g_tp_mode = -1;             // -1: not read from config yet
    }

    int camera_third_person_mode()
    {
        if (g_tp_mode < 0)
            g_tp_mode = config().third_person_mode;
        return g_tp_mode;
    }

    bool camera_view_first_person()
    {
        if (g_view_first_person < 0)
            g_view_first_person = config().first_person ? 1 : 0;
        return g_view_first_person != 0;
    }

    void camera_toggle_view(const char* why)
    {
        // third_person_mode = all: classic -> shoulder -> board -> first
        // person -> classic. Otherwise first person <-> the set preset.
        bool first = !camera_view_first_person();
        if (config().view_cycle_all)
        {
            const int mode = camera_third_person_mode();
            if (camera_view_first_person())
            {
                first = false;
                g_tp_mode = 0;
            }
            else if (mode < 2)
            {
                first = false;
                g_tp_mode = mode + 1;
            }
            else
                first = true;
        }
        g_view_first_person = first ? 1 : 0;
        if (!first)
            log("view: third-person preset %s",
                camera_third_person_mode() == 1 ? "shoulder"
                : camera_third_person_mode() == 2 ? "board" : "classic");
        // Leaving first person: the next camera build sees the flag and
        // calls first_person_leave (head, hands, gear restored); entering
        // it resolves the first-person eye on the next build.
        log("view: switched to %s (%s)", first ? "first person"
                                               : "third person",
            why ? why : "?");
    }

    float camera_comfort_vignette()
    {
        static float strength = 0.0f;
        static float last_pos[3] = {};
        static float last_turn = 0.0f;
        static LARGE_INTEGER last{};
        static bool have_last = false;
        const int mode = config().comfort_vignette;
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        float dt = last.QuadPart && frequency.QuadPart
            ? (float)((double)(now.QuadPart - last.QuadPart) /
                      (double)frequency.QuadPart)
            : 0.0f;
        last = now;
        if (mode == 0 || !g_first_person_active || !g_first_person_instance)
        {
            strength = 0.0f;
            have_last = false;
            return 0.0f;
        }
        float pos[3]{};
        __try
        {
            const float* p = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(g_first_person_instance) +
                0x10);
            for (int k = 0; k < 3; ++k)
                pos[k] = p[k];
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return strength;
        }
        const float turn = vr_input_first_person_turn();
        float target = 0.0f;
        const float scale = tune_world_scale();
        if (have_last && dt > 0.0f && dt < 0.2f && scale > 0.0f)
        {
            const float dx = pos[0] - last_pos[0];
            const float dy = pos[1] - last_pos[1];
            const float dz = pos[2] - last_pos[2];
            const float speed = sqrtf(dx * dx + dy * dy) / scale / dt;
            const float climb = fabsf(dz) / scale / dt;
            const float turn_rate = fabsf(wrap_pi(turn - last_turn)) / dt;
            const bool airborne =
                g_first_person_state_vtable == 0x00F05CEC; // JumpState
            const bool rolling = g_first_person_crouched && speed > 1.2f;
            if (std::isfinite(speed) && std::isfinite(climb))
            {
                if (airborne || rolling)
                    target = 1.0f;
                else if (mode == 2)
                {
                    // Walk ~1.5 m/s, run ~4 m/s; shimmy/climb move the root
                    // too. Smooth turning counts; snap turns are instant.
                    const float move = fmaxf(speed, climb);
                    target = fminf(1.0f, fmaxf(0.0f, (move - 0.25f) / 1.5f));
                    if (!config().snap_turn && std::isfinite(turn_rate))
                        target = fmaxf(target,
                                       fminf(1.0f, turn_rate / 1.6f));
                }
            }
        }
        memcpy(last_pos, pos, sizeof(last_pos));
        last_turn = turn;
        have_last = true;
        dt = fminf(fmaxf(dt, 0.0f), 0.1f);
        // Close in quickly (0.12 s), open slowly (0.35 s).
        const float tau = target > strength ? 0.12f : 0.35f;
        strength += (target - strength) * (1.0f - expf(-dt / tau));
        if (!std::isfinite(strength))
            strength = 0.0f;
        return strength;
    }

    bool camera_first_person_grounded()
    {
        return g_first_person_active &&
               g_first_person_traversal == TraversalGround;
    }

    bool camera_first_person_crouched()
    {
        return g_first_person_active && g_first_person_crouched;
    }

    bool camera_first_person_ledge_hanging()
    {
        return g_first_person_active && g_first_person_ledge_hanging;
    }

    bool camera_two_hand_engaged()
    {
        // Also true while foregrip tuning owns the numpad (long gun drawn).
        return g_first_person_active &&
            (g_two_hand.engaged ||
             (g_foregrip_tuning && g_gameplay_gun.instance &&
              GetTickCount() - g_gameplay_gun.tick <= 250));
    }

    bool camera_two_hand_grip_claims(bool left)
    {
        if (!config().two_handed_guns || !g_first_person_active ||
            left == config().left_handed || !g_gameplay_gun.instance)
            return false;
        const DWORD now = GetTickCount();
        if (now - g_gameplay_gun.tick > 250)
            return false;
        // Auto mode needs no button, but a grip pressed while holding or
        // reaching for the foregrip must still not holster or grab gear.
        return g_two_hand.engaged ||
               (g_two_hand.zone_tick && now - g_two_hand.zone_tick <= 150);
    }

    bool camera_first_person_secure_grip_pulse()
    {
        return g_first_person_secure_requested_at &&
            GetTickCount() - g_first_person_secure_requested_at <= 150;
    }

    bool camera_first_person_bar_hanging()
    {
        return g_first_person_active && g_first_person_bar_hanging;
    }

    bool camera_first_person_ledge_climbing()
    {
        return g_first_person_active && g_first_person_ledge_climbing;
    }

    bool camera_first_person_vine_climbing()
    {
        return g_first_person_active && g_first_person_vine_climbing;
    }

    bool camera_first_person_vine_look_angle(float* radians)
    {
        if (!radians || !camera_first_person_vine_climbing() ||
            !g_first_person_instance || !g_first_person_actor_offset_valid ||
            g_first_person_actor_offset_model != g_first_person_model)
            return false;

        float view_heading = 0.0f;
        if (!first_person_view_camera_heading(&view_heading))
            return false;
        __try
        {
            const float actor_heading = *reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(g_first_person_instance) +
                kOffInstanceHeading);
            if (!std::isfinite(actor_heading))
                return false;
            // The actor/camera offset is calibrated on the ground, then Lara's
            // actor yaw stays with the hold while the headset/right stick turns.
            // Reverse the engine camera sign so rightward looking is positive.
            const float look = -g_first_person_turn_sign * wrap_pi(
                view_heading + g_first_person_actor_camera_offset -
                actor_heading);
            if (!std::isfinite(look))
                return false;
            *radians = look;
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int camera_first_person_grapple_state()
    {
        if (!g_first_person_active || !g_first_person_instance)
            return 0;
        __try
        {
            const unsigned char* player =
                *reinterpret_cast<const unsigned char* const*>(0x0111713C);
            if (!player ||
                *reinterpret_cast<void* const*>(player - 0x70) !=
                    g_first_person_instance)
                return 0;
            // The global points at PlayerData::work (+0x70), while the
            // retail getter takes PlayerData*. Passing work made it read a
            // different handle and report "stowed" for every thrown hook.
            const unsigned char* player_data = player - 0x70;
            using PFN_GrappleHook = void*(__cdecl*)(const void*);
            if (!reinterpret_cast<PFN_GrappleHook>(0x0055A530)(player_data))
                return 0;
            // Retail GetGrapplingHookStateTargetInstance checks
            // PlayerData+0x440 bit 0x800 before returning the hooked target
            // (CHUCKABLE query 0xD7). An in-flight hook remains state 1.
            using PFN_GrappleTarget = void*(__cdecl*)();
            return reinterpret_cast<PFN_GrappleTarget>(0x0055A590)() ? 2 : 1;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool camera_aim_crosshair(float head_point[3], int* style)
    {
        if (!head_point || !config().vr_crosshair ||
            vr_hand_calibration_active())
            return false;
        const bool first_person = camera_first_person_active();
        // Retail combat state can outlive the holster (it only gates its
        // own reticle). With immersive holsters, the drawn guns decide.
        if (first_person && config().immersive_controls &&
            !vr_input_holster_combat_held())
            return false;
        const DWORD now = GetTickCount();
        if (!g_combat_reticle_time || now - g_combat_reticle_time > 200)
            return false;
        // Third person outside precision aim: on the game's auto-target,
        // where its lock-on rings would have been. No target, no crosshair
        // (as retail).
        const bool on_lock = !first_person && !g_combat_reticle_accurate;
        // Third person: only with the guns drawn (precision aim aside).
        if (on_lock && !retail_weapons_out())
            return false;
        if (on_lock && (!g_lock_target_time ||
                        now - g_lock_target_time > 200))
            return false;
        if (!on_lock && (!g_aim_point_time || now - g_aim_point_time > 200))
            return false;
        const float* world_point = on_lock ? g_lock_target : g_aim_point;
        Mat4 camera_to_world;
        memcpy(&camera_to_world.m[0][0],
               static_cast<const unsigned char*>(kMainCamera) +
                   kOffCwTransform, sizeof(camera_to_world.m));
        const Mat4 world_to_head = rigid_inverse(camera_to_world);
        for (int axis = 0; axis < 3; ++axis)
        {
            float value = world_to_head.m[3][axis];
            for (int row = 0; row < 3; ++row)
                value += world_point[row] * world_to_head.m[row][axis];
            if (!std::isfinite(value))
                return false;
            head_point[axis] = value;
        }
        if (style)
            *style = on_lock ? (g_lock_on_target ? (2 | 4) : 0) :
                     (g_combat_reticle_accurate ? 1 : 0) |
                     (g_aim_point_hit ? 2 : 0) |
                     (g_aim_point_enemy ? 4 : 0);
        return head_point[2] > 1.0f;
    }

    bool camera_aim_debug_ray(float head_origin[3], float head_end[3],
                              bool* hit)
    {
        if (!head_origin || !head_end || !tune_aim_debug_enabled() ||
            !camera_first_person_active() || !g_aim_from_controller)
            return false;
        const DWORD now = GetTickCount();
        if (!g_head_aim_target_time || now - g_head_aim_target_time > 200)
            return false;
        // Out of combat there is no probed point; show 20 m of ray so the
        // barrel can be lined up with guns holstered or drawn.
        const bool fresh = g_aim_point_time && now - g_aim_point_time <= 200;
        float world_end[3]{};
        for (int i = 0; i < 3; ++i)
            world_end[i] = fresh ? g_aim_point[i] :
                g_head_aim_eye[i] +
                g_head_aim_direction[i] * 20.0f * tune_world_scale();
        Mat4 camera_to_world;
        memcpy(&camera_to_world.m[0][0],
               static_cast<const unsigned char*>(kMainCamera) +
                   kOffCwTransform, sizeof(camera_to_world.m));
        const Mat4 world_to_head = rigid_inverse(camera_to_world);
        for (int axis = 0; axis < 3; ++axis)
        {
            float a = world_to_head.m[3][axis];
            float b = world_to_head.m[3][axis];
            for (int row = 0; row < 3; ++row)
            {
                a += g_head_aim_eye[row] * world_to_head.m[row][axis];
                b += world_end[row] * world_to_head.m[row][axis];
            }
            if (!std::isfinite(a) || !std::isfinite(b))
                return false;
            head_origin[axis] = a;
            head_end[axis] = b;
        }
        if (hit)
            *hit = fresh && g_aim_point_hit;
        return true;
    }

    bool camera_first_person_secure_grab_point(float centre[3])
    {
        if (!centre || !g_first_person_active ||
            !config().immersive_controls || !g_first_person_grip_precarious ||
            !g_grab_zone_centre_valid ||
            g_ledge_hand_grip[0].held || g_ledge_hand_grip[1].held)
            return false;
        memcpy(centre, g_grab_zone_centre, sizeof(g_grab_zone_centre));
        return true;
    }

    bool camera_first_person_grab_box(float corners[8][3], int* state)
    {
        if (!corners || !state || !g_first_person_active ||
            !config().immersive_controls ||
            !(g_first_person_ledge_hanging || g_first_person_bar_hanging ||
              g_first_person_vine_climbing))
            return false;
        if (!g_grab_zone_debug.valid)
            return false;
        memcpy(corners, g_grab_zone_debug.corners,
               sizeof(g_grab_zone_debug.corners));
        *state = g_ledge_hand_grip[0].held || g_ledge_hand_grip[1].held
            ? 2 : g_ledge_hand_grip[0].in_grab_zone ||
                  g_ledge_hand_grip[1].in_grab_zone ? 1 : 0;
        return true;
    }

    void camera_first_person_note_gpu_draw()
    {
        ++g_first_person_gpu_draw_count;
        if (g_current_gpu_hand_draw >= 0 &&
            g_current_gpu_hand_draw < 2)
        {
            ++g_hand_gpu_draws[g_current_gpu_hand_draw];
            if (!g_first_person_hand_gpu_first)
                g_first_person_hand_gpu_first = g_first_person_gpu_draw_count;
            g_first_person_hand_gpu_last = g_first_person_gpu_draw_count;
        }
    }

    int camera_first_person_drawable_hand(void* drawable)
    {
        if (!drawable)
            return -1;
        for (unsigned i = 0; i < g_hand_drawable_count; ++i)
        {
            const HandDrawableTag& tag = g_hand_drawables[i];
            if (tag.drawable != drawable)
                continue;
            if (g_first_person_active && tag.hand >= 0 && tag.hand < 2)
            {
                ++g_hand_drawable_hits[tag.hand];
                return tag.hand;
            }
            // Third-person controller hands (camera_third_person_hand_scale).
            if (!g_first_person_active && (tag.hand == 2 || tag.hand == 3))
                return tag.hand;
            return -1;
        }
        return -1;
    }

    int camera_first_person_gpu_hand_draw(int hand)
    {
        const int previous = g_current_gpu_hand_draw;
        g_current_gpu_hand_draw = hand;
        return previous;
    }

    void camera_first_person_gpu_hand_draw_end(int previous)
    {
        g_current_gpu_hand_draw = previous;
    }

    int camera_first_person_current_gpu_hand()
    {
        return g_current_gpu_hand_draw;
    }

    int camera_first_person_hand_render_state_mark()
    {
        if (!g_first_person_active || g_current_gpu_hand_draw < 0)
            return -1;
        if (!g_first_person_ledge_hanging)
        {
            if (!g_ground_hand_render_state_reported)
            {
                g_ground_hand_render_state_reported = true;
                g_hand_render_state_trace_sample = -2;
                g_hand_render_state_trace_remaining = 24;
            }
        }
        else
        {
            const unsigned mark = tune_ledge_visibility_marker();
            if (mark != g_hand_render_state_last_mark)
            {
                g_hand_render_state_last_mark = mark;
                g_hand_render_state_trace_sample = static_cast<int>(mark);
                g_hand_render_state_trace_remaining = 24;
            }
        }
        if (!g_hand_render_state_trace_remaining)
            return -1;
        --g_hand_render_state_trace_remaining;
        return g_hand_render_state_trace_sample;
    }

    void camera_first_person_gpu_frame_end()
    {
        // The D3D proxy also runs in its standalone load test. That process
        // has no retail camera or screen-stack globals to inspect.
        if (!g_camera_head_initialized)
            return;
        const unsigned mark = tune_ledge_visibility_marker();
        // Menu-Lara tilt evidence: the pitch of the main camera matrices as
        // the frame is finished, against the pitch our levelled rebuild gave
        // them. A menu camera pitched ~19 deg down but shown as a level view
        // makes upright figures lean toward the viewer.
        if (ui_menu_active())
        {
            static DWORD last = 0;
            static unsigned reports = 0;
            const DWORD now = GetTickCount();
            if (reports < 30 && (!last || now - last >= 1000))
            {
                last = now;
                ++reports;
                const Mat4 wc = load_matrix(kMainCamera, kOffWcTransform);
                const Mat4 wc2 = load_matrix(kMainCamera, kOffWcTransform2);
                const Mat4 head = vr_head_rotation();
                // World forward of a row-vector world-to-camera matrix is its
                // column 2; world Z is up. Head pitch from the head view.
                auto pitch_of = [](const Mat4& m)
                {
                    const float f[3] = { m.m[0][2], m.m[1][2], m.m[2][2] };
                    const float len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
                    return len > 1e-4f
                        ? asinf(fmaxf(-1.0f, fminf(1.0f, f[2] / len))) *
                              57.2957795f
                        : 0.0f;
                };
                const float* rot = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(kMainCamera) +
                    kOffRotation);
                log("camera: menu frame %u: wcTransformf pitch %+.1f, "
                    "wcTransform2f pitch %+.1f, head pitch %+.1f, Euler "
                    "(%.1f, %.1f, %.1f) deg",
                    reports, pitch_of(wc), pitch_of(wc2),
                    asinf(fmaxf(-1.0f, fminf(1.0f, -head.m[2][1]))) *
                        57.2957795f,
                    rot[0] * 57.2957795f, rot[1] * 57.2957795f,
                    rot[2] * 57.2957795f);
            }
        }
        // Goal 2 discriminator. A drawn pistol that stops reaching the
        // linked-weapon draw was culled before DRAW_DrawInstance; one that is
        // still drawn while invisible is lost later (projection/near plane).
        // Log every drop/return and every F8 mark while the guns are drawn.
        if (camera_first_person_active() && vr_input_holster_combat_held())
        {
            const bool marked = mark != g_pistol_last_mark;
            for (int hand = 0; hand < 2; ++hand)
            {
                const bool changed =
                    (g_pistol_draws[hand] == 0) !=
                    (g_pistol_prev_draws[hand] == 0);
                if ((changed || marked) && g_pistol_reports < 60)
                {
                    ++g_pistol_reports;
                    const Mat4 head = vr_head_rotation();
                    const float pitch = asinf(fmaxf(-1.0f, fminf(1.0f,
                        -head.m[2][1]))) * 57.2957795f;
                    Mat4 camera_to_world;
                    memcpy(&camera_to_world.m[0][0],
                           static_cast<const unsigned char*>(kMainCamera) +
                               kOffCwTransform, sizeof(camera_to_world.m));
                    const Mat4 world_to_head = rigid_inverse(camera_to_world);
                    float local[3]{};
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        local[axis] = world_to_head.m[3][axis];
                        for (int row = 0; row < 3; ++row)
                            local[axis] += g_pistol_instance_pos[hand][row] *
                                           world_to_head.m[row][axis];
                    }
                    log("render: %s pistol %s%s: %u draws this frame, "
                        "head pitch %+.0f deg, retail instance position in "
                        "head space (%.0f, %.0f, %.0f) units (Y down, Z "
                        "forward)",
                        hand == 0 ? "left" : "right",
                        marked ? "F8 mark" : g_pistol_draws[hand]
                            ? "drawn again" : "NOT drawn",
                        marked ? "" : " (draw-path change)",
                        g_pistol_draws[hand], pitch,
                        local[0], local[1], local[2]);
                }
                g_pistol_prev_draws[hand] = g_pistol_draws[hand];
            }
            g_pistol_last_mark = mark;
        }
        g_pistol_draws[0] = g_pistol_draws[1] = 0;
        if (g_first_person_ledge_hanging &&
            (g_hand_vb_report_frames < 8 ||
             mark != g_hand_vb_last_report_mark))
        {
            log("render: ledge frame CPU allocations L/R=%u/%u "
                "tagged drawables=%u matched drawables L/R=%u/%u "
                "GPU draws L/R=%u/%u F8 marker=%u "
                "hand draw positions=%llu..%llu/%llu (total=%llu)",
                g_hand_cpu_allocations[0], g_hand_cpu_allocations[1],
                g_hand_drawable_count,
                g_hand_drawable_hits[0], g_hand_drawable_hits[1],
                g_hand_gpu_draws[0], g_hand_gpu_draws[1],
                mark,
                static_cast<unsigned long long>(
                    g_first_person_hand_gpu_first ?
                    g_first_person_hand_gpu_first -
                    g_first_person_frame_gpu_begin : 0),
                static_cast<unsigned long long>(
                    g_first_person_hand_gpu_last ?
                    g_first_person_hand_gpu_last -
                    g_first_person_frame_gpu_begin : 0),
                static_cast<unsigned long long>(
                    g_first_person_gpu_draw_count -
                    g_first_person_frame_gpu_begin),
                static_cast<unsigned long long>(
                    g_first_person_gpu_draw_count));
            ++g_hand_vb_report_frames;
            g_hand_vb_last_report_mark = mark;
        }
        g_hand_drawable_count = 0;
        g_hand_drawable_hits[0] = g_hand_drawable_hits[1] = 0;
        g_hand_gpu_draws[0] = g_hand_gpu_draws[1] = 0;
        g_hand_cpu_allocations[0] = g_hand_cpu_allocations[1] = 0;
        g_first_person_hand_gpu_first = 0;
        g_first_person_hand_gpu_last = 0;
        g_first_person_frame_gpu_begin = g_first_person_gpu_draw_count;
    }

    void camera_perf_instance_report(unsigned frames)
    {
        if (!frames || !g_instance_cost_count)
            return;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double n = frames;
        double behind_ms = 0.0, total_ms = 0.0;
        unsigned behind_count = 0;
        for (int i = 0; i < g_instance_cost_count; ++i)
        {
            const InstanceCost& c = g_instance_costs[i];
            const double ms = double(c.ticks) * 1000.0 /
                              double(frequency.QuadPart) / n;
            total_ms += ms;
            if (c.calls && c.angle_sum / c.calls > 90.0f)
            {
                behind_ms += ms;
                ++behind_count;
            }
        }
        std::sort(g_instance_costs, g_instance_costs + g_instance_cost_count,
                  [](const InstanceCost& a, const InstanceCost& b) {
                      return a.ticks > b.ticks;
                  });
        char line[1024];
        int used = snprintf(line, sizeof(line), "perf instances: %d drawn, "
                            "%.2f ms/frame; behind the view (>90 deg) %u "
                            "costing %.2f ms; costliest:",
                            g_instance_cost_count, total_ms, behind_count,
                            behind_ms);
        for (int i = 0; i < g_instance_cost_count && i < 10 &&
                        used > 0 && used < (int)sizeof(line) - 80; ++i)
        {
            const InstanceCost& c = g_instance_costs[i];
            used += snprintf(line + used, sizeof(line) - used,
                " %s %.2f ms x%.1f at %.0f deg;",
                instance_name(c.instance),
                double(c.ticks) * 1000.0 / double(frequency.QuadPart) / n,
                c.calls / n, c.calls ? c.angle_sum / c.calls : 0.0f);
        }
        log("%s", line);
        g_instance_cost_count = 0;
    }

    bool camera_first_person_free_pole()
    {
        return g_first_person_active && g_first_person_vine_climbing &&
               (g_first_person_state_vtable == 0x00F058D4 ||  // attached
                g_first_person_state_vtable == 0x00F058E8);   // air attach
    }

    bool camera_first_person_ledge_pull(float* right_metres,
                                        float* up_metres,
                                        HandholdPullSample hands[2])
    {
        if (!right_metres || !up_metres || !hands)
            return false;
        *right_metres = 0.0f;
        *up_metres = 0.0f;
        hands[0] = HandholdPullSample{};
        hands[1] = HandholdPullSample{};
        if (!camera_first_person_ledge_hanging() &&
            !camera_first_person_bar_hanging() &&
            !camera_first_person_vine_climbing())
            return false;
        const float scale = tune_world_scale();
        if (!std::isfinite(scale) || scale <= 0.0f)
            return false;

        float total = 0.0f;
        float weakest_up = 1.0e6f;
        float vine_up[2]{};
        bool vine_tracked[2]{};
        int tracked = 0;
        bool held = false;
        // Ledge and bar pulls (2026-10-05, user: shimmy did not always
        // trigger, corners were hard, "a bit more of a cone"):
        // - measured along the hold's current direction, not the one
        //   captured at the grab, so the pull follows Lara round a corner;
        // - a pull within 50 degrees of that direction counts with its full
        //   length (a natural arm arc dips and swings toward the wall);
        // - the newest grip decides, not the average of both hands: a hand
        //   still holding where it grabbed halved every stroke before.
        const bool lateral_hold = !g_first_person_vine_climbing;
        float axis[2] = { 0.0f, 0.0f };
        if (lateral_hold && g_first_person_hold_axis_valid)
        {
            axis[0] = g_first_person_hold_axis[0];
            axis[1] = g_first_person_hold_axis[1];
        }
        unsigned newest_serial = 0;
        float newest_pull = 0.0f;
        for (int hand = 0; hand < 2; ++hand)
        {
            const LedgeHandGrip& grip = g_ledge_hand_grip[hand];
            if (!grip.held)
                continue;
            held = true;
            Mat4 controller;
            if (!first_person_controller_world_pose(hand == 0,
                                                    &controller))
                continue;
            const float dx = grip.world_pose.m[3][0] -
                             controller.m[3][0];
            const float dy = grip.world_pose.m[3][1] -
                             controller.m[3][1];
            const float dz = grip.world_pose.m[3][2] -
                             controller.m[3][2];
            // The live direction starts as the grab-time one and follows
            // the hold's axis frame to frame, keeping its sign by
            // continuity (round a 90-degree corner the grab-time direction
            // cannot tell the sign).
            static unsigned live_serial[2] = { 0, 0 };
            static float live[2][2] = { { 1.0f, 0.0f }, { 1.0f, 0.0f } };
            if (live_serial[hand] != grip.serial)
            {
                live_serial[hand] = grip.serial;
                live[hand][0] = grip.tangent[0];
                live[hand][1] = grip.tangent[1];
            }
            if (lateral_hold && (axis[0] != 0.0f || axis[1] != 0.0f))
            {
                const float same = axis[0] * live[hand][0] +
                                   axis[1] * live[hand][1];
                live[hand][0] = same < 0.0f ? -axis[0] : axis[0];
                live[hand][1] = same < 0.0f ? -axis[1] : axis[1];
            }
            const float tx = live[hand][0], ty = live[hand][1];
            float pull = (dx * tx + dy * ty) / scale;
            if (lateral_hold)
            {
                const float length =
                    sqrtf(dx * dx + dy * dy + dz * dz) / scale;
                if (std::isfinite(length) && length > 0.0f &&
                    fabsf(pull) >= 0.64f * length)
                    pull = pull < 0.0f ? -length : length;
            }
            if (!std::isfinite(pull) || !std::isfinite(dz))
                continue;
            total += pull;
            if (grip.serial >= newest_serial)
            {
                newest_serial = grip.serial;
                newest_pull = pull;
            }
            hands[hand].right_metres = pull;
            hands[hand].grip_serial = grip.serial;
            hands[hand].grabbed_at_ms = grip.grabbed_at;
            hands[hand].up_metres = dz / scale;
            weakest_up = fminf(weakest_up, dz / scale);
            vine_up[hand] = dz / scale;
            vine_tracked[hand] = true;
            ++tracked;
        }
        // Suppress stick shimmy even if controller tracking is briefly lost.
        // With two held hands, their average is the attempted body movement.
        if (tracked)
        {
            *right_metres = lateral_hold ? newest_pull : total / tracked;
            // On a vertical hold, the first hand to move far enough becomes
            // the driving hand until it releases. This lets either hand
            // start a pull even when both are anchored, without the other
            // stationary hand reversing the climb as Lara's body moves.
            if (g_first_person_vine_climbing)
            {
                if (g_first_person_vine_pull_hand >= 0 &&
                    !vine_tracked[g_first_person_vine_pull_hand])
                    g_first_person_vine_pull_hand = -1;
                if (g_first_person_vine_pull_hand < 0)
                {
                    int candidate = -1;
                    for (int hand = 0; hand < 2; ++hand)
                        if (vine_tracked[hand] &&
                            fabsf(vine_up[hand]) > 0.12f &&
                            (candidate < 0 ||
                             fabsf(vine_up[hand]) >
                             fabsf(vine_up[candidate])))
                            candidate = hand;
                    if (candidate >= 0)
                    {
                        g_first_person_vine_pull_hand = candidate;
                        log("first-person: %s hand drives vertical climb",
                            candidate == 0 ? "left" : "right");
                    }
                }
                if (g_first_person_vine_pull_hand >= 0)
                    *up_metres =
                        vine_up[g_first_person_vine_pull_hand];
            }
            else if (tracked == 2 && g_first_person_horizontal_ledge)
                *up_metres = weakest_up;
        }
        return held;
    }

    void camera_first_person_ledge_grip_input(bool left, bool down,
                                               bool enabled)
    {
        const int hand = left ? 0 : 1;
        LedgeHandGrip& grip = g_ledge_hand_grip[hand];
        if (!enabled ||
            (!camera_first_person_ledge_hanging() &&
             !camera_first_person_bar_hanging() &&
             !camera_first_person_vine_climbing()) || !down)
        {
            if (grip.held)
                log("first-person: %s hand released handhold",
                    left ? "left" : "right");
            grip.pending = false;
            grip.held = false;
            if (g_first_person_vine_pull_hand == hand)
                g_first_person_vine_pull_hand = -1;
        }
        else if (!grip.was_down)
        {
            grip.pending = true;
            grip.pressed_at = GetTickCount();
        }
        // Refresh even outside ledge mode: a grip already held when Lara
        // catches a ledge cannot create an accidental hand attachment.
        grip.was_down = down;
    }

    bool camera_first_person_hand_calibration(int hand, float out[9])
    {
        if (!out || hand < 0 || hand >= 2 ||
            !g_first_person_hand_calibrated[hand])
            return false;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                out[row * 3 + column] =
                    g_first_person_hand_calibration[hand].m[row][column];
        return true;
    }

    bool camera_first_person_gear_belt_distance(int item, bool left,
                                                 float* distance_metres)
    {
        if (distance_metres)
            *distance_metres = FLT_MAX;
        if (!g_first_person_active || item < 0 || item >= 2 ||
            !g_first_person_gear_valid[item] ||
            GetTickCount() - g_first_person_gear_seen[item] > 500 ||
            !distance_metres)
            return false;

        Mat4 controller;
        if (!first_person_controller_world_pose(left, &controller))
            return false;
        const Mat4& gear = g_first_person_gear_pose[item];
        float distance_sq = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float difference =
                gear.m[3][axis] - controller.m[3][axis];
            distance_sq += difference * difference;
        }
        const float scale = tune_world_scale();
        if (!std::isfinite(distance_sq) || !std::isfinite(scale) ||
            scale <= 0.0f)
            return false;
        const float distance = sqrtf(distance_sq) / scale;
        *distance_metres = distance;
        return true;
    }

    bool camera_first_person_gear_grab(int item, bool left,
                                      float radius_metres,
                                      float* distance_metres)
    {
        float distance = FLT_MAX;
        if (item < 0 || item >= 2 || g_first_person_gear_held[item] ||
            !std::isfinite(radius_metres) || radius_metres <= 0.0f ||
            !camera_first_person_gear_belt_distance(
                item, left, &distance))
            return false;
        if (distance_metres)
            *distance_metres = distance;
        if (distance > radius_metres)
            return false;

        Mat4 controller;
        if (!first_person_controller_world_pose(left, &controller))
            return false;
        const Mat4& gear = g_first_person_gear_pose[item];
        // Capture orientation at pickup while snapping the item's origin to
        // the visible hand. It then rotates rigidly with the controller.
        g_first_person_gear_grip_rotation[item] =
            rotation_only(gear) *
            rigid_inverse(rotation_only(controller));
        g_first_person_gear_held[item] = true;
        g_first_person_gear_hand[item] = left ? 0 : 1;
        return true;
    }

    void camera_first_person_gear_release(int item)
    {
        if (item >= 0 && item < 2)
        {
            g_first_person_gear_held[item] = false;
            g_first_person_gear_hand[item] = -1;
        }
    }

    int camera_first_person_visibility_class(void* instance)
    {
        if (!g_first_person_active || !g_first_person_instance || !instance)
            return 0;
        if (instance == g_first_person_instance)
            return 1;
        __try
        {
            return *reinterpret_cast<void**>(
                static_cast<unsigned char*>(instance) + 0xB8) ==
                g_first_person_instance ? 2 : 0;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // DRAW_DrawInstance draw flag 0x20 selects wcTransformNoShakef and scales
    // its view X/Y columns by 512 / projection distance
    // (0x0040A587: fld [0x00EFDD1C] = 512.0; 0x0040A592: fdiv [0x00F11D0C],
    // the global TRANS_SetProjectionDistance writes). That keeps such models
    // at a fixed field of view on a monitor. Our VR projection ignores the
    // game's projection distance, so in the main menu (distance != 512) the
    // menu Lara got a view-space X/Y magnification the world did not, and
    // looked tilted toward the viewer while the scenery stayed level.
    // Point the divisor at the 512.0 constant: the factor becomes exactly 1.
    static void neutralise_fixed_fov_model_scale()
    {
        unsigned char* operand = reinterpret_cast<unsigned char*>(0x0040A594);
        const unsigned char expected[6] =
            { 0xD8, 0x35, 0x0C, 0x1D, 0xF1, 0x00 };  // fdiv [0x00F11D0C]
        const unsigned char replacement[4] =
            { 0x1C, 0xDD, 0xEF, 0x00 };              // -> [0x00EFDD1C]
        if (memcmp(operand - 2, expected, sizeof(expected)) != 0)
        {
            log("camera: fixed-FOV model scale left alone (unexpected bytes "
                "at 0x0040A592)");
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect(operand, sizeof(replacement),
                            PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(operand, replacement, sizeof(replacement));
        VirtualProtect(operand, sizeof(replacement), old, &old);
        FlushInstructionCache(GetCurrentProcess(), operand,
                              sizeof(replacement));
        log("camera: fixed-FOV model draws (flag 0x20, e.g. the menu Lara) use "
            "the world's scale in VR");
    }

    void camera_head_init()
    {
        g_camera_head_initialized = true;
        if (config().hmd_drives_camera)
            neutralise_fixed_fov_model_scale();
        // Installed whatever the settings say: levelling the horizon runs
        // through the same detour and can be switched on from the keypad at
        // any point, so the hook has to already be there.
        if (!config().hmd_drives_camera)
            log("camera: the headset is not driving the camera "
                "(hmd_drives_camera = 0)");

        if (config().hmd_aim)
            log("aim: head-directed free-fire target enabled (camera mode 13)");

        log("view: starting in %s (right stick long press or \\ switches "
            "live)", camera_view_first_person() ? "first person"
                                                : "third person");
        if (camera_view_first_person())
        {
            log("first-person: EXPERIMENTAL mode requested; gameplay view will "
                "use a fixed root-local eye anchor, with safe menu/cinematic fallback");
            if (!config().first_person_tracked_hands)
                log("first-person: isolated hand rendering disabled; "
                    "Lara's animated arms are preserved");
        }

        void* tramp = nullptr;
        if (hook_install((void*)kCalculateWCTransform, (void*)&detour, &tramp,
                         kPrologue, sizeof(kPrologue),
                         "CAMERA_CalculateWCTransform"))
        {
            g_original = (PFN_CalcWC)tramp;
        }

        // Third-person immersive draws controller hands from this hook too.
        if (config().first_person_tracked_hands ||
            config().immersive_controls)
        {
            tramp = nullptr;
            if (hook_install((void*)kDrawInstance,
                             (void*)&detour_draw_instance, &tramp,
                             kPrologue, sizeof(kPrologue),
                             "DRAW_DrawInstance (isolated tracked hands)"))
            {
                g_draw_instance = (PFN_DrawInstance)tramp;
            }
            tramp = nullptr;
            if (hook_install((void*)kGetBoneVB,
                             (void*)&detour_get_bone_vb, &tramp,
                             kGetBoneVBSignature,
                             sizeof(kGetBoneVBSignature),
                             "D3D_GetVB (hand GPU range diagnostic)"))
                g_get_bone_vb = (PFN_GetBoneVB)tramp;
            tramp = nullptr;
            if (hook_install((void*)kSetIndicesPtr,
                             (void*)&detour_set_indices_ptr, &tramp,
                             kSetIndicesPtrSignature,
                             sizeof(kSetIndicesPtrSignature),
                             "D3D_SetIndicesPtr (hand drawable tag)"))
                g_set_indices_ptr = (PFN_SetIndicesPtr)tramp;
        }

        // First-person hooks: installed in either starting view so the view
        // can switch live; each passes straight through unless first person
        // is active (g_first_person_active).
        {
            tramp = nullptr;
            if (hook_install((void*)kFilteredInputUpdate,
                             (void*)&detour_filtered_input_update, &tramp,
                             kFilteredInputUpdateSignature,
                             sizeof(kFilteredInputUpdateSignature),
                             "FilteredInput::Update (VR travel heading)"))
                g_filtered_input_update =
                    reinterpret_cast<PFN_FilteredInputUpdate>(tramp);

            tramp = nullptr;
            if (hook_install((void*)kProcessMovement,
                             (void*)&detour_process_movement, &tramp,
                             kPrologue, sizeof(kPrologue),
                             "ProcessMovement (VR travel intent)"))
                g_process_movement =
                    reinterpret_cast<PFN_ProcessMovement>(tramp);

            if (config().immersive_controls)
            {
                tramp = nullptr;
                if (hook_install((void*)kHPoleDesiredDirection,
                                 (void*)&detour_hpole_desired_direction, &tramp,
                                 kPrologue, sizeof(kPrologue),
                                 "HorizPole desired direction (VR hand traverse)"))
                    g_hpole_desired_direction =
                        reinterpret_cast<PFN_HPoleDesiredDirection>(tramp);

                tramp = nullptr;
                if (hook_install((void*)kWallVertPoleMessage,
                                 (void*)&detour_wall_vert_pole_message,
                                 &tramp, kPrologue, sizeof(kPrologue),
                                 "WallVertPoleAttached jump (VR vine jump)"))
                    g_wall_vert_pole_message =
                        reinterpret_cast<PFN_StateMessageHandler>(tramp);

                // push ecx / mov edx, [0x0111713C] -- two whole instructions.
                const unsigned char kCalcJumpFacingSignature[7] =
                    { 0x51, 0x8B, 0x15, 0x3C, 0x71, 0x11, 0x01 };
                tramp = nullptr;
                if (hook_install((void*)0x005784F0,
                                 (void*)&detour_calc_jump_facing, &tramp,
                                 kCalcJumpFacingSignature,
                                 sizeof(kCalcJumpFacingSignature),
                                 "CalcJumpFacingDir (VR chain/pole jump)"))
                    g_calc_jump_facing =
                        reinterpret_cast<PFN_CalcJumpFacingDir>(tramp);

                tramp = nullptr;
                if (hook_install((void*)kHPoleFastTraverseEntry,
                                 (void*)&detour_hpole_fast_traverse_entry,
                                 &tramp, kHPoleFastTraverseEntrySignature,
                                 sizeof(kHPoleFastTraverseEntrySignature),
                                 "HPoleFastTraverse entry (confirmation)"))
                    g_hpole_fast_traverse_entry =
                        reinterpret_cast<PFN_HPoleFastTraverseEntry>(tramp);
            }
        }

        // Both views: third-person precision aim uses the VR crosshair too
        // (2026-10-04; installed in first person only, it never ran there).
        if (config().vr_crosshair)
        {
            tramp = nullptr;
            if (hook_install((void*)kDrawCombatLock,
                             (void*)&detour_draw_combat_lock, &tramp,
                             kPrologue, sizeof(kPrologue),
                             "playerDrawCombatLock (VR crosshair)"))
                g_draw_combat_lock =
                    reinterpret_cast<PFN_DrawCombatLock>(tramp);
            tramp = nullptr;
            if (g_draw_combat_lock &&
                hook_install((void*)kDrawCombatReticle,
                             (void*)&detour_draw_combat_reticle, &tramp,
                             kPrologue, sizeof(kPrologue),
                             "playerDrawCombatReticle (first-person rings)"))
                g_draw_combat_reticle =
                    reinterpret_cast<PFN_DrawCombatReticle>(tramp);
        }

        if (config().immersive_controls)
        {
            tramp = nullptr;
            if (hook_install((void*)kPlayerInvEndCombatMode,
                             (void*)&detour_end_combat, &tramp,
                             kEndCombatSignature,
                             sizeof(kEndCombatSignature),
                             "playerInvEndCombatMode (holster hold)"))
                g_end_combat = reinterpret_cast<PFN_EndCombat>(tramp);
        }

        if (config().immersive_controls)
        {
            tramp = nullptr;
            if (hook_install((void*)kUIFadeGroupTrigger,
                             (void*)&detour_fade_trigger, &tramp,
                             kFadeTriggerSignature,
                             sizeof(kFadeTriggerSignature),
                             "UIFadeGroupTrigger (third-person gear HUD)"))
                g_fade_trigger = reinterpret_cast<PFN_FadeTrigger>(tramp);
            tramp = nullptr;
            if (hook_install((void*)kUIItemMeterDraw,
                             (void*)&detour_meter_draw, &tramp,
                             kMeterDrawSignature,
                             sizeof(kMeterDrawSignature),
                             "UIItemMeter_Draw (third-person light meter)"))
                g_meter_draw = reinterpret_cast<PFN_MeterDraw>(tramp);
            tramp = nullptr;
            if (hook_install((void*)kUIItemGroupDraw,
                             (void*)&detour_group_draw, &tramp,
                             kGroupDrawSignature,
                             sizeof(kGroupDrawSignature),
                             "UIItemGroup_Draw (third-person light block)"))
                g_group_draw = reinterpret_cast<PFN_GroupDraw>(tramp);
        }

        {
            tramp = nullptr;
            if (hook_install((void*)kG2InstanceBuildTransforms,
                             (void*)&detour_build_transforms, &tramp,
                             kBuildTransformsSignature,
                             sizeof(kBuildTransformsSignature),
                             "G2Instance_BuildTransforms (held long gun)"))
                g_build_transforms =
                    reinterpret_cast<PFN_BuildTransforms>(tramp);
        }

        if (config().hmd_aim)
        {
            tramp = nullptr;
            if (hook_install((void*)g_legacy_line_probe,
                             (void*)&detour_legacy_line_probe, &tramp,
                             kPrologue, sizeof(kPrologue),
                             "MULTIBODY_LegacyLineProbe (bullet ray trace)"))
                g_retail_line_probe =
                    reinterpret_cast<PFN_LegacyLineProbe>(tramp);
            tramp = nullptr;
            if (hook_install((void*)kPlayerUpdateTargetPos,
                             (void*)&detour_player_update_target, &tramp,
                             kPlayerUpdateTargetPosSignature,
                             sizeof(kPlayerUpdateTargetPosSignature),
                             "playerUpdateTargetPos (head aim)"))
            {
                g_player_update_target =
                    reinterpret_cast<PFN_PlayerUpdateTargetPos>(tramp);
            }
            tramp = nullptr;
            if (hook_install((void*)kWeaponFireSingle,
                             (void*)&detour_weapon_fire_single, &tramp,
                             kWeaponFireSingleSignature,
                             sizeof(kWeaponFireSingleSignature),
                             "WEAPON_FireSingle (headset shot target)"))
                g_weapon_fire_single =
                    reinterpret_cast<PFN_WeaponFireSingle>(tramp);
        }

        tramp = nullptr;
        if (hook_install((void*)kCalcAutoCenterData, (void*)&detour_autocenter,
                         &tramp, kAutoCenterPrologue, sizeof(kAutoCenterPrologue),
                         "CAMERA_CalcAutoCenterData"))
        {
            g_autocenter = (PFN_AutoCenter)tramp;
        }

        tramp = nullptr;
        if (hook_install((void*)kSetShakeNoPadshock, (void*)&detour_setshake,
                         &tramp, kShakePrologue, sizeof(kShakePrologue),
                         "CAMERA_SetShakeNoPadshock"))
        {
            g_setshake = (PFN_SetShake)tramp;
        }
    }
}
