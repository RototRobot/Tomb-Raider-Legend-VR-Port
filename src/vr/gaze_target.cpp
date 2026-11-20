// Third-person lock-on picks the target nearest the headset's gaze.
//
// Retail chooses combat targets in the player's sense unit (manager
// [0x01117550]) with SenseUnit_PickCandidate (0x0059C5F0, __thiscall, two
// stack args: rule block, origin or null for unit+0x50; returns the chosen
// list node, the wrapper 0x0059D900 adds 8). Disassembly 2026-11-11:
//   unit+0x34 definition (word +4: class-list count; dword +0, flag
//     0x08000000: skip the object the player holds, [player+0x598]);
//   unit+0x38 class lists (stride 0x14), unit+0x3C the last list;
//   node +4 next, +8 candidate (handle +4, point +0xC), +0xC handle;
//   0x004557C0 resolves a handle to an instance;
//   0x00592B50 (eax candidate, edi rule tier, push origin) passes a tier;
//   rule block +0 flags (sign: line of sight from the player, 0x20000000:
//     line of sight required), +4 tier count, +8 tiers;
//   0x00593340 (instance, eye point, candidate, 0) is the line-of-sight test,
//     eye = instance+0x10 with z + [[0x01117050]+0x430];
//   0x00588F90 (out, candidate) is the candidate's target point.
// Same layout as Underworld's Target_PickCandidate, whose gaze pick this
// follows: replay the walk with the game's own tests, keep the candidate
// nearest the gaze ray within the cone, and leave the game's pick in place
// when none qualifies.
//
// Why re-ranking alone followed the controller (disassembly 2026-11-12):
//   The only caller is the wrapper 0x0059D900 (manager thiscall: unit index,
//   rule block, origin), called once, from 0x005B1A93 in the player combat
//   update 0x005B19C0 while PlayerData+0x3E6 == 3 (lock-on held): unit 0,
//   block [[0x01117050]+0x424], origin null. Its result goes to 0x0059C960
//   (set unit 0's target) or 0x0059C920 (clear) every frame, so this pick
//   IS the lock-on target.
//   Tier (edi): +4 rule count, +8 byte mode, +0xC rule pointers. Rule: word
//   type, word test, float limit, +8 vector. A non-zero test passes when the
//   value is <= limit, a zero test when it is >= limit. Types (jump table
//   0x00592F30): 1 3D distance, 2 XY distance, 3 |dz|, 4 3D angle and 5 XY
//   angle between (candidate - origin) and the rule vector turned by the
//   origin's rotation, 6 angle about the retail camera's forward
//   (0x010FC8D0 / position 0x010FC660), or with limit <= [0x00EFDDB0] an
//   on-screen test (0x005A91C0). No class, health or alive tests here.
//   Origin (0x005763E0): +0 instance, +0x30 override flag (then +0x10
//   position, +0x20 rotation); else instance +0x10 / +0x30 (player: z plus
//   eye height). With tier mode != 0 and the stick deflected, the player's
//   yaw becomes the stick heading (0x00564C90). So the retail cone is about
//   Lara's facing or the stick direction, never the head, and the gaze
//   re-rank only saw what that cone let through.
// The gaze pick now tests each tier with its cone rules (4, 5, 6 with a
// limit) removed -- range, screen and exclusion rules stay the game's -- and
// the gaze cone (gaze_target_degrees) takes their place.

#include "gaze_target.h"
#include "camera_head.h"
#include "hook.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace trlvr
{
    namespace
    {
        constexpr uintptr_t kPickCandidate = 0x0059C5F0;
        constexpr unsigned char kPickSignature[6] =
            { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        constexpr uintptr_t kSenseManager = 0x01117550;
        constexpr uintptr_t kPlayerData = 0x0111713C;
        constexpr uintptr_t kPlayerInstance = 0x010E537C;
        constexpr uintptr_t kPlayerTuning = 0x01117050;

        using PFN_Pick = void*(__thiscall*)(void*, void*, void*);
        PFN_Pick g_original_pick = nullptr;

        float g_eye[3]{};
        float g_dir[3]{};
        DWORD g_ray_time = 0;
        bool g_ray_valid = false;

        void* resolve(void* handle)
        {
            return reinterpret_cast<void*(__cdecl*)(void*)>(0x004557C0)(handle);
        }

        bool tier_passes(void* candidate, void* tier, void* origin)
        {
            bool passed = false;
            __asm
            {
                mov eax, candidate
                mov edi, tier
                push origin
                mov ecx, 0x00592B50
                call ecx
                add esp, 4
                mov passed, al
            }
            return passed;
        }

        bool line_of_sight(void* instance, float* eye, void* candidate)
        {
            return reinterpret_cast<bool(__cdecl*)(void*, float*, void*, int)>(
                0x00593340)(instance, eye, candidate, 0);
        }

        bool candidate_point(void* candidate, float out[3])
        {
            alignas(16) float p[4] = {};
            reinterpret_cast<void(__cdecl*)(float*, void*)>(0x00588F90)(
                p, candidate);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
                !std::isfinite(p[2]) ||
                (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f))
                return false;
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
            return true;
        }

        // A copy of a rule tier without its inclusion cones, in the layout
        // 0x00592B50 reads (+4 count, +8 mode, +0xC rule pointers).
        constexpr unsigned kMaxRules = 16;
        constexpr unsigned kMaxTiers = 8;
        struct RangeTier
        {
            unsigned char header[0xC];
            void* rules[kMaxRules];
        };

        bool cone_rule(const unsigned char* rule)
        {
            const unsigned short type =
                *reinterpret_cast<const unsigned short*>(rule);
            const unsigned short test =
                *reinterpret_cast<const unsigned short*>(rule + 2);
            const float limit = *reinterpret_cast<const float*>(rule + 4);
            const float screen =
                *reinterpret_cast<const float*>(0x00EFDDB0);
            return test != 0 && (type == 4 || type == 5 ||
                                 (type == 6 && limit > screen));
        }

        // False when the tier cannot be copied (the caller then uses the
        // original). *had_cone says whether any rule was removed.
        bool make_range_tier(const unsigned char* tier, RangeTier* out,
                             bool* had_cone)
        {
            *had_cone = false;
            if (!tier)
                return false;
            const unsigned count = *reinterpret_cast<const unsigned*>(
                tier + 4);
            if (count > kMaxRules)
                return false;
            memcpy(out->header, tier, sizeof(out->header));
            unsigned kept = 0;
            for (unsigned i = 0; i < count; ++i)
            {
                unsigned char* rule = *reinterpret_cast<unsigned char* const*>(
                    tier + 0xC + i * 4);
                if (rule && cone_rule(rule))
                {
                    *had_cone = true;
                    continue;
                }
                out->rules[kept++] = rule;
            }
            *reinterpret_cast<unsigned*>(out->header + 4) = kept;
            return true;
        }

        // Logs the lock-on block's rules once per block, for the headset
        // check (which tiers have cones, and whether mode picks the stick).
        void report_block(const unsigned char* block)
        {
            static const void* reported = nullptr;
            if (block == reported)
                return;
            reported = block;
            const unsigned tiers = *(block + 4);
            log("aim: lock-on rule block %p, flags 0x%08X, %u tiers", block,
                *reinterpret_cast<const unsigned*>(block), tiers);
            for (unsigned t = 0; t < tiers && t < kMaxTiers; ++t)
            {
                const unsigned char* tier =
                    *reinterpret_cast<const unsigned char* const*>(
                        block + 8 + t * 4);
                if (!tier)
                    continue;
                const unsigned rules = *reinterpret_cast<const unsigned*>(
                    tier + 4);
                for (unsigned r = 0; r < rules && r < kMaxRules; ++r)
                {
                    const unsigned char* rule =
                        *reinterpret_cast<const unsigned char* const*>(
                            tier + 0xC + r * 4);
                    if (!rule)
                        continue;
                    log("aim:   tier %u (mode %u) rule %u -- type %u test %u "
                        "limit %.2f%s", t, tier[8], r,
                        *reinterpret_cast<const unsigned short*>(rule),
                        *reinterpret_cast<const unsigned short*>(rule + 2),
                        *reinterpret_cast<const float*>(rule + 4),
                        cone_rule(rule) ? " (cone: gaze replaces it)" : "");
                }
            }
        }

        bool player_unit(void* unit)
        {
            void* const* units = *reinterpret_cast<void* const* const*>(
                kSenseManager);
            if (!units)
                return false;
            for (int i = 0; i < 8; ++i)
                if (units[i] == unit)
                    return true;
            return false;
        }

        struct PickCounts
        {
            int walked = 0;   // live candidates (not self, not held)
            int in_range = 0; // passed a tier with its cones removed
            int retail = 0;   // passed a full tier (the game's cone too)
            int in_gaze = 0;  // in range and within the gaze cone
        };

        // The replayed walk. Returns the chosen node, or null to let the
        // game pick.
        void* gaze_pick(unsigned char* unit, unsigned char* block,
                        unsigned char* origin, PickCounts* counts,
                        float* best_degrees)
        {
            if (!origin)
                origin = unit + 0x50;
            const unsigned char* def =
                *reinterpret_cast<unsigned char* const*>(unit + 0x34);
            if (!def || !block)
                return nullptr;
            const unsigned lists = *(def + 4);
            void* held = nullptr;
            if (*reinterpret_cast<const unsigned*>(def) & 0x08000000u)
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(kPlayerData);
                unsigned char* object = player
                    ? *reinterpret_cast<unsigned char* const*>(player + 0x598)
                    : nullptr;
                if (object)
                    held = (*reinterpret_cast<const unsigned*>(object + 0xA4) &
                            0x400000u)
                        ? *reinterpret_cast<void* const*>(object + 0xF0)
                        : object;
            }
            const unsigned flags = *reinterpret_cast<const unsigned*>(block);
            const unsigned tiers = *(block + 4);
            void* self = *reinterpret_cast<void* const*>(origin);
            if (tiers > kMaxTiers)
                return nullptr;
            report_block(block);

            // Each tier without its cone rules; a tier that cannot be
            // copied keeps its own rules.
            RangeTier range_tiers[kMaxTiers];
            void* full_tiers[kMaxTiers];
            void* test_tiers[kMaxTiers];
            for (unsigned t = 0; t < tiers; ++t)
            {
                full_tiers[t] = *reinterpret_cast<void**>(block + 8 + t * 4);
                bool had_cone = false;
                test_tiers[t] = make_range_tier(
                    static_cast<const unsigned char*>(full_tiers[t]),
                    &range_tiers[t], &had_cone) && had_cone
                    ? static_cast<void*>(&range_tiers[t])
                    : full_tiers[t];
            }

            // Line-of-sight eye, as retail builds it.
            unsigned char* looker = (flags & 0x80000000u)
                ? *reinterpret_cast<unsigned char* const*>(kPlayerInstance)
                : static_cast<unsigned char*>(self);
            alignas(16) float eye[4] = {};
            if (looker)
            {
                const float* p = reinterpret_cast<const float*>(looker + 0x10);
                eye[0] = p[0]; eye[1] = p[1]; eye[2] = p[2]; eye[3] = p[3];
                const unsigned char* tuning =
                    *reinterpret_cast<unsigned char* const*>(kPlayerTuning);
                if (tuning)
                    eye[2] += *reinterpret_cast<const float*>(tuning + 0x430);
            }

            const float limit = cosf(config().gaze_target_degrees *
                                     3.14159265f / 180.0f);
            float best_cos = limit;
            void* best = nullptr;
            for (unsigned list = 0; list <= lists; ++list)
            {
                unsigned char* head = list == lists
                    ? unit + 0x3C
                    : *reinterpret_cast<unsigned char**>(unit + 0x38) +
                          0x14 * list;
                for (unsigned char* node = *reinterpret_cast<unsigned char**>(head);
                     node; node = *reinterpret_cast<unsigned char**>(node + 4))
                {
                    void* instance = resolve(node + 0xC);
                    if (!instance || instance == self || instance == held)
                        continue;
                    void* candidate = node + 8;
                    ++counts->walked;
                    bool passes = false;
                    for (unsigned t = 0; t < tiers && !passes; ++t)
                        passes = tier_passes(candidate, test_tiers[t], origin);
                    if (!passes)
                        continue;
                    ++counts->in_range;
                    bool retail_passes = false;
                    for (unsigned t = 0; t < tiers && !retail_passes; ++t)
                        retail_passes = tier_passes(candidate, full_tiers[t],
                                                    origin);
                    counts->retail += retail_passes ? 1 : 0;
                    float point[3];
                    if (!candidate_point(candidate, point))
                        continue;
                    float to[3] = { point[0] - g_eye[0], point[1] - g_eye[1],
                                    point[2] - g_eye[2] };
                    const float length = sqrtf(to[0] * to[0] + to[1] * to[1] +
                                               to[2] * to[2]);
                    if (!(length > 1.0f))
                        continue;
                    const float c = (to[0] * g_dir[0] + to[1] * g_dir[1] +
                                     to[2] * g_dir[2]) / length;
                    if (c <= limit)
                        continue;
                    ++counts->in_gaze;
                    if (c <= best_cos)
                        continue;
                    if ((flags & 0x20000000u) && looker &&
                        !line_of_sight(looker, eye, candidate))
                        continue;
                    best_cos = c;
                    best = node;
                }
            }
            if (best)
                *best_degrees = acosf(fminf(1.0f, best_cos)) * 180.0f /
                                3.14159265f;
            return best;
        }

        void* __fastcall pick_detour(void* unit, void*, void* block,
                                     void* origin)
        {
            const bool use = config().gaze_target && g_ray_valid &&
                             !camera_first_person_active() &&
                             GetTickCount() - g_ray_time <= 200;
            if (use)
            {
                void* node = nullptr;
                PickCounts counts;
                float degrees = 0.0f;
                bool ok = true;
                __try
                {
                    if (player_unit(unit))
                        node = gaze_pick(static_cast<unsigned char*>(unit),
                                         static_cast<unsigned char*>(block),
                                         static_cast<unsigned char*>(origin),
                                         &counts, &degrees);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    ok = false;
                    node = nullptr;
                }
                if (node)
                {
                    static DWORD last_report = 0;
                    const DWORD now = GetTickCount();
                    if (now - last_report > 2000)
                    {
                        last_report = now;
                        log("aim: gaze target %.1f degrees off gaze (%d "
                            "live, %d in range, %d in the game's cone, %d "
                            "in gaze cone)", degrees, counts.walked,
                            counts.in_range, counts.retail, counts.in_gaze);
                    }
                    return node;
                }
                void* retail = g_original_pick(unit, block, origin);
                // Reported when the walk faulted, or when the game found a
                // target the walk did not count in range (a walk that
                // disagrees with retail), throttled.
                static DWORD last_miss = 0;
                const DWORD now = GetTickCount();
                if ((!ok || (retail && counts.in_range == 0)) &&
                    now - last_miss > 2000)
                {
                    last_miss = now;
                    log("aim: gaze walk %s, game's pick %p used",
                        ok ? "saw no candidates" : "faulted", retail);
                }
                else if (retail && now - last_miss > 5000)
                {
                    last_miss = now;
                    log("aim: no candidate within %.0f degrees of gaze (%d "
                        "live, %d in range, %d in the game's cone); game's "
                        "pick used", config().gaze_target_degrees,
                        counts.walked, counts.in_range, counts.retail);
                }
                return retail;
            }
            return g_original_pick(unit, block, origin);
        }
    }

    void gaze_target_set_ray(const float eye[3], const float direction[3])
    {
        const float length = sqrtf(direction[0] * direction[0] +
                                   direction[1] * direction[1] +
                                   direction[2] * direction[2]);
        if (!std::isfinite(length) || length < 1e-4f ||
            !std::isfinite(eye[0]) || !std::isfinite(eye[1]) ||
            !std::isfinite(eye[2]))
        {
            g_ray_valid = false;
            return;
        }
        for (int i = 0; i < 3; ++i)
        {
            g_eye[i] = eye[i];
            g_dir[i] = direction[i] / length;
        }
        g_ray_time = GetTickCount();
        g_ray_valid = true;
    }

    void gaze_target_init()
    {
        void* trampoline = nullptr;
        if (hook_install((void*)kPickCandidate, (void*)&pick_detour,
                         &trampoline, kPickSignature, sizeof(kPickSignature),
                         "SenseUnit_PickCandidate (gaze lock-on)"))
            g_original_pick = reinterpret_cast<PFN_Pick>(trampoline);
    }
}
