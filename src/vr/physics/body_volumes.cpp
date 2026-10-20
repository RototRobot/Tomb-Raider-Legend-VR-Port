#include "body_volumes.h"

#include <cmath>

namespace trlvr
{
    namespace
    {
        // Sizes in metres (a woman of ~1.75 m); every one is scaled by the
        // `metre` passed to body_volumes_build. Half extents for boxes
        // (side, front-to-back, up), radii for capsules. Tune here.
        namespace dim
        {
            const float kPelvisHalf[3]      = { 0.17f, 0.11f, 0.10f };
            const float kAbdomenHalf[3]     = { 0.15f, 0.10f, 0.0f };     // up: from the bones
            const float kAbdomenPad         = 0.02f;                       // up: added to half the span
            const float kChestHalf[3]       = { 0.165f, 0.11f, 0.0f };    // 0.32 wide, 0.22 deep
            const float kChestPad           = 0.04f;
            const float kUpperChestHalf[3]  = { 0.17f, 0.10f, 0.08f };
            // The bones run through the spine, which sits behind the body's
            // middle: the boxes are shifted forward of them.
            const float kPelvisForward      = 0.01f;
            const float kAbdomenForward     = 0.03f;
            const float kChestForward       = 0.03f;
            const float kUpperChestForward  = 0.02f;
            const float kHeadRadius         = 0.095f;
            const float kNeckRadius         = 0.050f;
            const float kClavicleRadius     = 0.050f;
            const float kUpperArmRadius     = 0.050f;
            const float kForearmRadius      = 0.042f;
            const float kHandRadius         = 0.050f;
            const float kHandMinLength      = 0.08f;    // wrist to hand centre when no fingers
            const float kThighRadius        = 0.085f;
            const float kShinRadius         = 0.060f;
            const float kFootRadius         = 0.045f;
            // A soft part: a small box in front of the chest.
            const float kSoftHalf[3]        = { 0.07f, 0.08f, 0.07f };
            const float kSoftForward        = 0.06f;    // in front of the joint, no tip given
        }

        float dot(const float* a, const float* b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        float length(const float* a)
        {
            return std::sqrt(dot(a, a));
        }

        float normalise(float* a)
        {
            const float l = length(a);
            if (l > 1.0e-8f)
                for (int k = 0; k < 3; ++k)
                    a[k] /= l;
            return l;
        }

        void cross(const float* a, const float* b, float* out)
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        struct Builder
        {
            BodyVolumes& out;
            const float* bones;
            int count;
            float metre;

            bool has(int bone) const { return bone >= 0 && bone < count; }
            const float* pos(int bone) const { return bones + bone * 16 + 12; }

            // An oriented box: `up` is its long axis (unit), its front is
            // the body's facing direction made square to it.
            void box(const char* name, const float centre[3], const float up_in[3],
                     float forward_shift, const float half_m[3], int soft)
            {
                if (out.count >= kBodyVolumesMax)
                    return;
                BodyVolume& v = out.volume[out.count];
                float up[3] = { up_in[0], up_in[1], up_in[2] };
                if (normalise(up) < 0.5f)
                    for (int k = 0; k < 3; ++k)
                        up[k] = out.up[k];
                float fwd[3];
                const float along = dot(out.forward, up);
                for (int k = 0; k < 3; ++k)
                    fwd[k] = out.forward[k] - along * up[k];
                if (normalise(fwd) < 0.1f)
                    return;     // facing along her own spine: nothing sensible
                float side[3];
                cross(up, fwd, side);
                normalise(side);
                v.kind = BodyVolumeKind::Box;
                v.name = name;
                v.soft = soft;
                for (int k = 0; k < 3; ++k)
                {
                    v.axis[0][k] = side[k];
                    v.axis[1][k] = fwd[k];
                    v.axis[2][k] = up[k];
                    v.centre[k] = centre[k] + fwd[k] * forward_shift * metre;
                    v.half[k] = half_m[k] * metre;
                }
                ++out.count;
            }

            void capsule(const char* name, const float a[3], const float b[3], float radius_m)
            {
                if (out.count >= kBodyVolumesMax)
                    return;
                BodyVolume& v = out.volume[out.count++];
                v.kind = BodyVolumeKind::Capsule;
                v.name = name;
                v.soft = -1;
                for (int k = 0; k < 3; ++k)
                {
                    v.a[k] = a[k];
                    v.b[k] = b[k];
                }
                v.radius = radius_m * metre;
            }

            void limb(const char* name, int from, int to, float radius_m)
            {
                if (has(from) && has(to))
                    capsule(name, pos(from), pos(to), radius_m);
            }

            // Wrist to the middle of the fingers.
            void hand(const char* name, int wrist, int first, int last, int elbow)
            {
                if (!has(wrist))
                    return;
                float sum[3] = {}, centre[3];
                int n = 0;
                for (int b = first; b <= last && has(b); ++b, ++n)
                    for (int k = 0; k < 3; ++k)
                        sum[k] += pos(b)[k];
                float reach[3] = {};
                if (n > 0)
                    for (int k = 0; k < 3; ++k)
                    {
                        centre[k] = sum[k] / (float)n;
                        reach[k] = centre[k] - pos(wrist)[k];
                    }
                if (n == 0 || length(reach) < 0.02f * metre)
                {
                    // No fingers worth the name: carry on along the forearm.
                    if (!has(elbow))
                        return;
                    for (int k = 0; k < 3; ++k)
                        reach[k] = pos(wrist)[k] - pos(elbow)[k];
                    if (normalise(reach) < 1.0e-6f)
                        return;
                    for (int k = 0; k < 3; ++k)
                        centre[k] = pos(wrist)[k] + reach[k] * dim::kHandMinLength * metre;
                }
                capsule(name, pos(wrist), centre, dim::kHandRadius);
            }
        };

        bool finite_bones(const float* bones, int count)
        {
            for (int i = 0; i < count * 16; ++i)
                if (!std::isfinite(bones[i]))
                    return false;
            return true;
        }

        // Depth, outward normal, surface point of `p` against one volume;
        // `distance` is 0 inside, else the gap. Returns true when inside.
        bool probe(const BodyVolume& v, const float p[3], float* depth,
                   float normal[3], float surface[3], float* distance,
                   const float fallback[3])
        {
            *distance = 0.0f;
            if (v.kind == BodyVolumeKind::Ellipsoid)
            {
                // s = |local / half|: 1 on the surface. The way out is the
                // gradient; the distance to the surface its first-order
                // estimate (s - 1) / |grad s|, exact on a sphere.
                float d[3], l[3], g[3] = {}, n[3] = {};
                for (int k = 0; k < 3; ++k)
                    d[k] = p[k] - v.centre[k];
                float s2 = 0.0f;
                for (int a = 0; a < 3; ++a)
                {
                    l[a] = dot(d, v.axis[a]);
                    s2 += (l[a] / v.half[a]) * (l[a] / v.half[a]);
                    g[a] = l[a] / (v.half[a] * v.half[a]);
                }
                const float s = std::sqrt(s2);
                for (int a = 0; a < 3; ++a)
                    for (int k = 0; k < 3; ++k)
                        n[k] += g[a] * v.axis[a][k];
                const float gl = normalise(n);      // |l / half^2|
                float gap;
                if (gl > 1.0e-9f && s > 1.0e-6f)
                    gap = (s - 1.0f) * s / gl;
                else
                {
                    // At the centre: out along the thinnest axis.
                    int thin = 0;
                    for (int a = 1; a < 3; ++a)
                        if (v.half[a] < v.half[thin])
                            thin = a;
                    for (int k = 0; k < 3; ++k)
                        n[k] = v.axis[thin][k];
                    gap = -v.half[thin];
                }
                for (int k = 0; k < 3; ++k)
                    normal[k] = n[k];
                if (s <= 1.0f)
                {
                    *depth = -gap;
                    for (int k = 0; k < 3; ++k)
                        surface[k] = p[k] + normal[k] * *depth;
                    return true;
                }
                *distance = gap;
                *depth = -gap;
                for (int k = 0; k < 3; ++k)
                    surface[k] = p[k] - normal[k] * gap;
                return false;
            }
            if (v.kind == BodyVolumeKind::Box)
            {
                float d[3], l[3], over[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = p[k] - v.centre[k];
                int worst = 0;
                bool inside = true;
                for (int k = 0; k < 3; ++k)
                {
                    l[k] = dot(d, v.axis[k]);
                    over[k] = std::fabs(l[k]) - v.half[k];
                    if (over[k] > 0.0f)
                        inside = false;
                    if (over[k] > over[worst])
                        worst = k;
                }
                if (inside)
                {
                    // The nearest face: the way out that is shortest.
                    const float sign = l[worst] < 0.0f ? -1.0f : 1.0f;
                    *depth = -over[worst];
                    for (int k = 0; k < 3; ++k)
                        normal[k] = v.axis[worst][k] * sign;
                    for (int k = 0; k < 3; ++k)
                        surface[k] = p[k] + normal[k] * *depth;
                    return true;
                }
                // Outside: out from the nearest point of the box.
                float closest[3] = { v.centre[0], v.centre[1], v.centre[2] };
                for (int a = 0; a < 3; ++a)
                {
                    float c = l[a];
                    c = c > v.half[a] ? v.half[a] : c < -v.half[a] ? -v.half[a] : c;
                    for (int k = 0; k < 3; ++k)
                        closest[k] += v.axis[a][k] * c;
                }
                for (int k = 0; k < 3; ++k)
                    normal[k] = p[k] - closest[k];
                *distance = normalise(normal);
                *depth = -*distance;
                for (int k = 0; k < 3; ++k)
                    surface[k] = closest[k];
                return false;
            }
            float ab[3], ap[3];
            for (int k = 0; k < 3; ++k)
            {
                ab[k] = v.b[k] - v.a[k];
                ap[k] = p[k] - v.a[k];
            }
            const float len2 = dot(ab, ab);
            float t = len2 > 1.0e-12f ? dot(ap, ab) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            float closest[3];
            for (int k = 0; k < 3; ++k)
            {
                closest[k] = v.a[k] + ab[k] * t;
                normal[k] = p[k] - closest[k];
            }
            const float d = normalise(normal);
            if (d < 1.0e-6f)
                for (int k = 0; k < 3; ++k)
                    normal[k] = fallback[k];
            if (d < v.radius)
            {
                *depth = v.radius - d;
                for (int k = 0; k < 3; ++k)
                    surface[k] = closest[k] + normal[k] * v.radius;
                return true;
            }
            *distance = d - v.radius;
            *depth = -*distance;
            for (int k = 0; k < 3; ++k)
                surface[k] = closest[k] + normal[k] * v.radius;
            return false;
        }
    }

    BodyBoneMap body_bone_map_underworld()
    {
        return BodyBoneMap();
    }

    BodyBoneMap body_bone_map_legend()
    {
        BodyBoneMap m;
        m.root = 0;
        m.pelvis = 100;
        m.pelvis2 = -1;
        m.waist = 1;
        m.chest = 2;
        m.upper_chest = 3;
        m.neck = 4;
        m.head = 5;
        const int clavicle[2] = { 58, 79 }, shoulder[2] = { 59, 80 }, elbow[2] = { 60, 81 },
                  wrist[2] = { 61, 82 }, hip[2] = { 101, 105 }, knee[2] = { 102, 106 },
                  ankle[2] = { 103, 107 }, toe[2] = { 104, 108 };
        for (int i = 0; i < 2; ++i)
        {
            m.clavicle[i] = clavicle[i];
            m.shoulder[i] = shoulder[i];
            m.elbow[i] = elbow[i];
            m.wrist[i] = wrist[i];
            m.finger_first[i] = m.finger_last[i] = -1;
            m.hip[i] = hip[i];
            m.knee[i] = knee[i];
            m.ankle[i] = ankle[i];
            m.toe[i] = toe[i];
        }
        m.soft_count = 2;
        m.soft_bone[0] = 54; m.soft_tip[0] = 55;
        m.soft_bone[1] = 56; m.soft_tip[1] = 57;
        return m;
    }

    void body_volumes_build(BodyVolumes& out, const float* bones, int bone_count,
                            float metre, const int* soft_bones, int soft_count,
                            const int* soft_tips)
    {
        BodyBoneMap map = body_bone_map_underworld();
        if (soft_count > kBodySoftMax)
            soft_count = kBodySoftMax;
        map.soft_count = soft_bones && soft_count > 0 ? soft_count : 0;
        for (int i = 0; i < map.soft_count; ++i)
        {
            map.soft_bone[i] = soft_bones[i];
            map.soft_tip[i] = soft_tips ? soft_tips[i] : -1;
        }
        body_volumes_build(out, bones, bone_count, metre, map);
    }

    void body_volumes_build(BodyVolumes& out, const float* bones, int bone_count,
                            float metre, const BodyBoneMap& map)
    {
        out = BodyVolumes();
        out.metre = metre;
        if (!bones || !(metre > 0.0f) || map.root < 0 || map.root >= bone_count ||
            !finite_bones(bones, bone_count))
            return;

        // Her facing and vertical from the root.
        const float* root = bones + map.root * 16;
        const int fr = map.forward_row < 0 ? 0 : map.forward_row > 2 ? 2 : map.forward_row;
        const int ur = map.up_row < 0 ? 0 : map.up_row > 2 ? 2 : map.up_row;
        for (int k = 0; k < 3; ++k)
        {
            out.forward[k] = map.forward_sign * root[fr * 4 + k];
            out.up[k] = root[ur * 4 + k];
        }
        if (normalise(out.forward) < 0.5f || normalise(out.up) < 0.5f)
            return;

        Builder b = { out, bones, bone_count, metre };
        if (!b.has(map.pelvis) || !b.has(map.chest) || !b.has(map.upper_chest) ||
            !b.has(map.neck) || !b.has(map.head))
            return;

        // Soft parts first: a hand in one and in the chest behind it means
        // the soft part.
        int soft_count = map.soft_count > kBodySoftMax ? kBodySoftMax : map.soft_count;
        for (int i = 0; i < soft_count; ++i)
        {
            if (!b.has(map.soft_bone[i]))
                continue;
            const float* joint = b.pos(map.soft_bone[i]);
            float centre[3] = { joint[0], joint[1], joint[2] };
            float up[3] = { out.up[0], out.up[1], out.up[2] };
            float shift = 0.0f;
            if (b.has(map.soft_tip[i]))
            {
                // The mass is at the tip.
                for (int k = 0; k < 3; ++k)
                    centre[k] = b.pos(map.soft_tip[i])[k];
            }
            else
                shift = dim::kSoftForward;
            b.box("soft", centre, up, shift, dim::kSoftHalf, i);
        }

        // Torso: boxes along the line pelvis -> waist -> chest -> neck, each
        // standing along its stretch of it.
        float pelvis[3], up[3];
        for (int k = 0; k < 3; ++k)
            pelvis[k] = b.has(map.pelvis2)
                ? 0.5f * (b.pos(map.pelvis)[k] + b.pos(map.pelvis2)[k])
                : b.pos(map.pelvis)[k];
        const float* chest = b.pos(map.chest);
        const float* upper = b.pos(map.upper_chest);
        const float* neck = b.pos(map.neck);

        for (int k = 0; k < 3; ++k)
            up[k] = (b.has(map.waist) ? b.pos(map.waist)[k] : chest[k]) - pelvis[k];
        b.box("pelvis", pelvis, up, dim::kPelvisForward, dim::kPelvisHalf, -1);

        {
            float centre[3], span[3], half[3] = { dim::kAbdomenHalf[0], dim::kAbdomenHalf[1], 0.0f };
            for (int k = 0; k < 3; ++k)
            {
                centre[k] = 0.5f * (pelvis[k] + chest[k]);
                span[k] = chest[k] - pelvis[k];
            }
            half[2] = 0.5f * length(span) / metre + dim::kAbdomenPad;
            b.box("abdomen", centre, span, dim::kAbdomenForward, half, -1);
        }
        {
            float centre[3], span[3], half[3] = { dim::kChestHalf[0], dim::kChestHalf[1], 0.0f };
            for (int k = 0; k < 3; ++k)
            {
                centre[k] = 0.5f * (chest[k] + upper[k]);
                span[k] = upper[k] - chest[k];
            }
            half[2] = 0.5f * length(span) / metre + dim::kChestPad;
            b.box("chest", centre, span, dim::kChestForward, half, -1);
        }
        {
            float span[3];
            for (int k = 0; k < 3; ++k)
                span[k] = neck[k] - upper[k];
            b.box("upper chest", upper, span, dim::kUpperChestForward, dim::kUpperChestHalf, -1);
        }

        // Neck and head.
        b.limb("neck", map.upper_chest, map.neck, dim::kNeckRadius);
        {
            const float* base = neck;
            const float* top = b.pos(map.head);
            float axis[3];
            for (int k = 0; k < 3; ++k)
                axis[k] = top[k] - base[k];
            const float span = normalise(axis);
            const float r = dim::kHeadRadius * metre;
            // The head's ball: its centre between a radius above the neck
            // and a radius below the top (one point if they cross).
            float a[3], c[3];
            for (int k = 0; k < 3; ++k)
            {
                a[k] = base[k] + axis[k] * r;
                c[k] = top[k] - axis[k] * r;
            }
            if (span < 2.0f * r)
                for (int k = 0; k < 3; ++k)
                    a[k] = c[k] = 0.5f * (base[k] + top[k]);
            b.capsule("head", a, c, dim::kHeadRadius);
        }

        // Arms (A, B) and clavicles.
        static const char* const kClavicle[2] = { "clavicle A", "clavicle B" };
        static const char* const kUpperArm[2] = { "upper arm A", "upper arm B" };
        static const char* const kForearm[2] = { "forearm A", "forearm B" };
        static const char* const kHand[2] = { "hand A", "hand B" };
        static const char* const kThigh[2] = { "thigh A", "thigh B" };
        static const char* const kShin[2] = { "shin A", "shin B" };
        static const char* const kFoot[2] = { "foot A", "foot B" };
        for (int i = 0; i < 2; ++i)
        {
            b.limb(kClavicle[i], map.clavicle[i], map.shoulder[i], dim::kClavicleRadius);
            b.limb(kUpperArm[i], map.shoulder[i], map.elbow[i], dim::kUpperArmRadius);
            b.limb(kForearm[i], map.elbow[i], map.wrist[i], dim::kForearmRadius);
            b.hand(kHand[i], map.wrist[i], map.finger_first[i], map.finger_last[i], map.elbow[i]);
        }

        // Legs.
        for (int i = 0; i < 2; ++i)
        {
            b.limb(kThigh[i], map.hip[i], map.knee[i], dim::kThighRadius);
            b.limb(kShin[i], map.knee[i], map.ankle[i], dim::kShinRadius);
            b.limb(kFoot[i], map.ankle[i], map.toe[i], dim::kFootRadius);
        }

        out.valid = out.count > 0;
    }

    bool body_volumes_test(const BodyVolumes& vols, const float point[3], BodyHit* hit)
    {
        BodyHit none;
        if (hit)
            *hit = none;
        if (!vols.valid || !std::isfinite(point[0]) || !std::isfinite(point[1]) ||
            !std::isfinite(point[2]))
            return false;

        int best = -1, nearest = -1;
        float best_depth = 0.0f, nearest_gap = 0.0f;
        float best_normal[3] = {}, best_surface[3] = {};
        float near_normal[3] = {}, near_surface[3] = {};
        for (int i = 0; i < vols.count; ++i)
        {
            float depth, normal[3], surface[3], gap;
            if (probe(vols.volume[i], point, &depth, normal, surface, &gap, vols.forward))
            {
                // A soft part beats a firm one it sits in front of.
                const bool soft = vols.volume[i].soft >= 0;
                const bool best_soft = best >= 0 && vols.volume[best].soft >= 0;
                if (best < 0 || (soft && !best_soft) ||
                    (soft == best_soft && depth > best_depth))
                {
                    best = i;
                    best_depth = depth;
                    for (int k = 0; k < 3; ++k)
                    {
                        best_normal[k] = normal[k];
                        best_surface[k] = surface[k];
                    }
                }
            }
            else if (nearest < 0 || gap < nearest_gap)
            {
                nearest = i;
                nearest_gap = gap;
                for (int k = 0; k < 3; ++k)
                {
                    near_normal[k] = normal[k];
                    near_surface[k] = surface[k];
                }
            }
        }
        if (best >= 0)
        {
            if (hit)
            {
                hit->inside = true;
                hit->volume = best;
                hit->soft = vols.volume[best].soft;
                hit->depth = best_depth;
                for (int k = 0; k < 3; ++k)
                {
                    hit->normal[k] = best_normal[k];
                    hit->surface[k] = best_surface[k];
                }
            }
            return true;
        }
        if (hit && nearest >= 0)
        {
            hit->volume = nearest;
            hit->soft = vols.volume[nearest].soft;
            hit->depth = -nearest_gap;
            for (int k = 0; k < 3; ++k)
            {
                hit->normal[k] = near_normal[k];
                hit->surface[k] = near_surface[k];
            }
        }
        return false;
    }

    int body_volumes_nearest_soft(const BodyVolumes& vols, const float point[3],
                                  float max_distance)
    {
        int pick = -1;
        float best = max_distance * max_distance;
        for (int i = 0; vols.valid && i < vols.count; ++i)
        {
            const BodyVolume& v = vols.volume[i];
            if (v.soft < 0)
                continue;
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float c = v.kind != BodyVolumeKind::Capsule
                    ? v.centre[k] : 0.5f * (v.a[k] + v.b[k]);
                d2 += (point[k] - c) * (point[k] - c);
            }
            if (d2 < best)
            {
                best = d2;
                pick = v.soft;
            }
        }
        return pick;
    }

    bool body_volumes_add_box(BodyVolumes& vols, const char* name, const float centre[3],
                              const float axes[3][3], const float half[3], int soft,
                              BodyVolumeKind kind)
    {
        if (!vols.valid || vols.count >= kBodyVolumesMax || kind == BodyVolumeKind::Capsule)
            return false;
        for (int r = 0; r < 3; ++r)
        {
            const float len2 = axes[r][0] * axes[r][0] + axes[r][1] * axes[r][1] + axes[r][2] * axes[r][2];
            if (!(std::fabs(len2 - 1.0f) < 1e-2f) || !(half[r] > 0.0f) || !std::isfinite(centre[r]))
                return false;
        }
        BodyVolume& v = vols.volume[vols.count++];
        v = BodyVolume();
        v.kind = kind;
        v.name = name;
        v.soft = soft;
        for (int r = 0; r < 3; ++r)
        {
            v.centre[r] = centre[r];
            v.half[r] = half[r];
            for (int k = 0; k < 3; ++k)
                v.axis[r][k] = axes[r][k];
        }
        return true;
    }

    bool body_volumes_test_volume(const BodyVolumes& vols, int index, const float point[3],
                                  BodyHit* hit)
    {
        if (hit)
            *hit = BodyHit();
        if (!vols.valid || index < 0 || index >= vols.count || !std::isfinite(point[0]) ||
            !std::isfinite(point[1]) || !std::isfinite(point[2]))
            return false;
        float depth, normal[3], surface[3], gap;
        const bool inside = probe(vols.volume[index], point, &depth, normal, surface, &gap,
                                  vols.forward);
        if (hit)
        {
            hit->inside = inside;
            hit->volume = index;
            hit->soft = vols.volume[index].soft;
            hit->depth = inside ? depth : -gap;
            for (int k = 0; k < 3; ++k)
            {
                hit->normal[k] = normal[k];
                hit->surface[k] = surface[k];
            }
        }
        return inside;
    }
}
