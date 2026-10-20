#pragma once
// Lara's body as volumes, built each frame from her bone world matrices, so
// a hand point can be tested against her. The torso is oriented boxes and
// the head, neck, limbs and clavicles are capsules between joints, sized in
// metres and scaled by `metre` (see the dim table in body_volumes.cpp --
// tune them there).
//
// Pure maths: no config(), no game memory, no Windows calls.
//
// Which bone is which is a BodyBoneMap, so the same code serves both games.
// Bone 0 (map.root) is the root: its axes say which way she faces and which
// way is up, because the torso bones' own axes are not known to line up
// with her front.

#include "../vr_math.h"

namespace trlvr
{
    // Bone indices for body_volumes_build; -1 = absent. A and B are the two
    // chains of each pair (A = her left in the Legend map). Names follow the
    // volumes they make, which body_push_segment_for_volume reads (the last
    // letter picks the chain).
    struct BodyBoneMap
    {
        int root = 0;
        // Which row of the root's matrix is her front (taken with the sign:
        // -1 = the row negated) and which is up.
        int forward_row = 1;
        float forward_sign = -1.0f;
        int up_row = 2;
        int pelvis = 2;            // the pelvis box centre is the midpoint of these two
        int pelvis2 = 1;            // (-1: pelvis alone)
        int waist = 14;             // the pelvis box stands along pelvis -> waist (-1: -> chest)
        int chest = 15;             // abdomen top / chest bottom
        int upper_chest = 16;       // chest top / upper chest centre
        int neck = 17;              // the neck capsule runs upper_chest -> neck
        int head = 18;              // the head's top; the capsule runs neck -> head
        int clavicle[2] = { 52, 77 };
        int shoulder[2] = { 53, 78 };       // upper arm bone (its origin is the joint)
        int elbow[2] = { 54, 79 };          // forearm bone
        int wrist[2] = { 55, 80 };          // hand bone
        int finger_first[2] = { 56, 81 };   // the hand reaches to the middle of
        int finger_last[2] = { 71, 96 };    // these (-1: along the forearm)
        int hip[2] = { 4, 9 };
        int knee[2] = { 5, 10 };
        int ankle[2] = { 6, 11 };
        int toe[2] = { 8, 13 };
        // Soft parts, indexed by soft index (BodyHit::soft). The tip is the
        // mass; -1 puts the part's box in front of the joint.
        int soft_count = 0;
        int soft_bone[8] = {};
        int soft_tip[8] = {};
    };

    // Underworld: pelvis 1-2, waist 14, chest 15, upper chest 16, neck 17,
    // head top 18, clavicles 52 / 77, arms 53-55 / 78-80 (fingers 56-71 /
    // 81-96), legs 4-8 / 9-13. Forward is the root's -y, up its z.
    BodyBoneMap body_bone_map_underworld();

    // Legend: root 0, waist 1, spine 2, chest 3, neck 4, head 5, collar bones
    // 58 / 79, upper arm / forearm / hand 59-61 (A, left) and 80-82 (B,
    // right), pelvis 100 (child of the waist), thigh / shin / foot / toe
    // 101-104 (A) and 105-108 (B); soft parts: breasts 54 and 56, tips 55
    // and 57. In the map the waist is Legend's bone 1, "chest" its spine (2)
    // and "upper_chest" its chest (3). No finger range: hands run along the
    // forearm. Root axes: forward = the root's row 1 negated, up = row 2,
    // as Underworld -- Legend's model axes are z up, +x her left, -y her
    // front, and bone 0's rows are those axes in the world. The game side
    // (camera_head.cpp push_map) builds its map per model from the bones'
    // structure and checks the rows against her body (logged).
    BodyBoneMap body_bone_map_legend();

    constexpr int kBodyBonesNeeded = 153;      // bones read (the skeleton's size)
    constexpr int kBodyVolumesMax = 40;
    constexpr int kBodySoftMax = 8;

    enum class BodyVolumeKind : int { Box = 0, Capsule = 1, Ellipsoid = 2 };

    struct BodyVolume
    {
        BodyVolumeKind kind = BodyVolumeKind::Box;
        const char* name = "";
        int soft = -1;              // index into the soft parts list, -1 firm
        // Box / Ellipsoid: centre, three unit axes (side, forward, up) and half
        // extents (an ellipsoid's semi-axes).
        float centre[3] = {};
        float axis[3][3] = {};
        float half[3] = {};
        // Capsule: segment a..b and radius.
        float a[3] = {};
        float b[3] = {};
        float radius = 0.0f;
    };

    struct BodyVolumes
    {
        bool valid = false;
        int count = 0;
        float metre = 0.0f;
        float forward[3] = {};      // the way she faces, unit
        float up[3] = {};
        BodyVolume volume[kBodyVolumesMax];
    };

    // bones: bone_count x 16 floats (row-vector 4x4 per bone, rows 0..2
    // axes, row 3 position; game space). metre: game units per metre.
    // soft_bones: the soft parts' joint bones, soft_count of them (the
    // order is the soft index); soft_tips: each one's tip bone, or null --
    // the part's volume is then put in front of the joint. A soft part's
    // volume sits in front of the chest and is tested before the firm ones.
    // Leaves `valid` false when the bones are unusable (the map's required
    // bones out of range, non-finite, no root axes). This form overrides the
    // map's soft parts and uses the Underworld map; it is what the tests use.
    void body_volumes_build(BodyVolumes& out, const float* bones, int bone_count,
                            float metre, const int* soft_bones, int soft_count,
                            const int* soft_tips = nullptr);

    // The same with an explicit bone map (soft parts from the map).
    void body_volumes_build(BodyVolumes& out, const float* bones, int bone_count,
                            float metre, const BodyBoneMap& map);

    // inside: the point is in some volume. volume: its index (-1 if none);
    // soft: that volume's soft index (-1 firm); depth: how far inside, i.e.
    // how far to push it out along `normal` (outward unit); surface: the
    // point on the surface that push reaches. When outside, volume is the
    // nearest one, depth is minus the distance to it, and normal / surface
    // describe the nearest surface point. Of the volumes holding the
    // point, a soft one wins, otherwise the deepest.
    struct BodyHit
    {
        bool inside = false;
        int volume = -1;
        int soft = -1;
        float depth = 0.0f;
        float normal[3] = {};
        float surface[3] = {};
    };
    bool body_volumes_test(const BodyVolumes& volumes, const float point[3],
                           BodyHit* hit);

    // Legend's fallback (a palm pushing up from under a breast lands in the
    // chest's box and nothing moved): the soft part whose volume centre is
    // nearest `point` and within max_distance (units), -1 if none.
    int body_volumes_nearest_soft(const BodyVolumes& volumes, const float point[3],
                                  float max_distance);

    // One more soft (or firm, soft = -1) box or ellipsoid after
    // body_volumes_build: the buttock cheeks, placed by the caller (axes:
    // unit and square, rows side, forward, up; half extents / semi-axes in
    // units). False if the set is not valid or full, or the shape is unusable.
    bool body_volumes_add_box(BodyVolumes& volumes, const char* name, const float centre[3],
                              const float axes[3][3], const float half[3], int soft,
                              BodyVolumeKind kind = BodyVolumeKind::Box);

    // body_volumes_test against volume `index` alone (inside / depth /
    // normal / surface as there). False when outside it or the index is bad.
    bool body_volumes_test_volume(const BodyVolumes& volumes, int index, const float point[3],
                                  BodyHit* hit);
}
