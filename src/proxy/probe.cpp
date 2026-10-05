#include "probe.h"

#include "../common/config.h"
#include "../common/log.h"

#include <d3d9.h>

#include <cmath>
#include <cstring>

namespace trlvr
{
    namespace
    {
        constexpr int kMaxRegisters = 256;   // vs_3_0 float constant file
        constexpr int kMaxBlocks = 8;        // sub-matrices examined per call

        enum Shape
        {
            ShapeProjective = 0,   // no (0,0,0,1) anywhere: carries a projection
            ShapeAffineRows,       // last column is (0,0,0,1)
            ShapeAffineCols,       // last row is (0,0,0,1): the transpose
            ShapeAffine3,          // uploaded as 3 registers: a 4x3 transform
            ShapeCount
        };

        struct RegStat
        {
            unsigned writes_total;
            unsigned writes_this_frame;
            unsigned max_per_frame;
            unsigned frames_seen;
            unsigned shape_count[ShapeCount];
            bool     seen;
            bool     changed;
            float    first[16];
            float    last[16];
        };

        RegStat g_reg[kMaxRegisters];
        unsigned g_frame = 0;
        bool g_reported_once = false;

        // Distinct (start register, count) pairs seen on SetVertexShaderConstantF.
        struct CallSig { unsigned start, count, hits; };
        constexpr int kMaxSigs = 64;
        CallSig g_sig[kMaxSigs];
        int g_sig_count = 0;
        unsigned g_sig_dropped = 0;

        // Distinct viewport / render target sizes seen.
        struct Surface { char what[16]; unsigned w, h, hits; };
        constexpr int kMaxSurfaces = 24;
        Surface g_surface[kMaxSurfaces];
        int g_surface_count = 0;

        // Fixed-function transforms, by D3DTRANSFORMSTATETYPE.
        struct XformStat { unsigned hits; bool seen; float last[16]; };
        XformStat g_world, g_view, g_proj, g_other;
        unsigned g_other_state = 0;

        bool near_zero(float v)  { return fabsf(v) < 1e-6f; }
        bool near_one(float v)   { return fabsf(v - 1.0f) < 1e-6f; }

        Shape classify(const float* m)
        {
            // Row-vector convention: an affine transform's last column is
            // (0,0,0,1). m[3],m[7],m[11] are the w of rows 0..2.
            if (near_zero(m[3]) && near_zero(m[7]) && near_zero(m[11]) && near_one(m[15]))
                return ShapeAffineRows;

            // The same matrix transposed puts that in the last register.
            if (near_zero(m[12]) && near_zero(m[13]) && near_zero(m[14]) && near_one(m[15]))
                return ShapeAffineCols;

            return ShapeProjective;
        }

        const char* shape_name(Shape s)
        {
            switch (s)
            {
            case ShapeAffineRows: return "affine";
            case ShapeAffineCols: return "affine(T)";
            case ShapeAffine3:    return "affine 4x3";
            default:              return "PROJECTIVE";
            }
        }

        void record(UINT reg, const float* m, bool three_row)
        {
            if (reg >= kMaxRegisters)
                return;
            RegStat& r = g_reg[reg];

            if (!r.seen)
            {
                r.seen = true;
                memcpy(r.first, m, sizeof(r.first));
            }
            else if (!r.changed)
            {
                for (int i = 0; i < 16; ++i)
                {
                    if (fabsf(r.first[i] - m[i]) > 1e-4f)
                    {
                        r.changed = true;
                        break;
                    }
                }
            }

            memcpy(r.last, m, sizeof(r.last));
            r.shape_count[three_row ? ShapeAffine3 : classify(m)]++;
            r.writes_total++;
            r.writes_this_frame++;
        }

        // How much this register looks like the view-projection we are after.
        int score(const RegStat& r)
        {
            if (!r.seen || r.writes_total == 0)
                return -1;

            const unsigned proj = r.shape_count[ShapeProjective];
            if (proj * 2 < r.writes_total)
                return -1;                      // mostly affine: a world or view matrix

            const unsigned per_frame = r.max_per_frame;
            if (per_frame == 0)
                return -1;

            int s = 100;
            // A per-object world-view-projection is written far too often.
            if (per_frame > 64)       s -= 80;
            else if (per_frame > 16)  s -= 40;
            else if (per_frame > 8)   s -= 15;

            // The camera moves, so the view-projection must change.
            if (!r.changed)           s -= 50;

            return s;
        }
    }

    void probe_constants(UINT start_register, const float* data, UINT vector4_count)
    {
        if (!data)
            return;

        // A 4x3 affine transform is uploaded as three registers -- the usual
        // shape for a per-object world or world-view matrix. Missing those
        // would leave the picture half-told, so pad one out to 4x4 and record
        // it as its own shape.
        if (vector4_count == 3)
        {
            float m[16];
            memcpy(m, data, 12 * sizeof(float));
            m[12] = m[13] = m[14] = 0.0f;
            m[15] = 1.0f;
            record(start_register, m, true);
            return;
        }

        if (vector4_count < 4)
            return;

        const int blocks = (int)(vector4_count / 4);
        const int limit = blocks < kMaxBlocks ? blocks : kMaxBlocks;
        for (int b = 0; b < limit; ++b)
            record(start_register + b * 4, data + b * 16, false);
    }

    void probe_surface(const char* what, unsigned width, unsigned height)
    {
        if (!width || !height)
            return;
        for (int i = 0; i < g_surface_count; ++i)
        {
            if (g_surface[i].w == width && g_surface[i].h == height &&
                strcmp(g_surface[i].what, what) == 0)
            {
                g_surface[i].hits++;
                return;
            }
        }
        if (g_surface_count >= kMaxSurfaces)
            return;
        Surface& s = g_surface[g_surface_count++];
        strncpy(s.what, what, sizeof(s.what) - 1);
        s.what[sizeof(s.what) - 1] = 0;
        s.w = width; s.h = height; s.hits = 1;
    }

    void probe_call(UINT start_register, UINT vector4_count)
    {
        for (int i = 0; i < g_sig_count; ++i)
        {
            if (g_sig[i].start == start_register && g_sig[i].count == vector4_count)
            {
                g_sig[i].hits++;
                return;
            }
        }
        if (g_sig_count < kMaxSigs)
        {
            g_sig[g_sig_count].start = start_register;
            g_sig[g_sig_count].count = vector4_count;
            g_sig[g_sig_count].hits = 1;
            g_sig_count++;
        }
        else
        {
            g_sig_dropped++;
        }
    }

    void probe_transform(unsigned state, const _D3DMATRIX* matrix)
    {
        XformStat* slot = nullptr;
        switch (state)
        {
        case D3DTS_WORLD:      slot = &g_world; break;
        case D3DTS_VIEW:       slot = &g_view;  break;
        case D3DTS_PROJECTION: slot = &g_proj;  break;
        default:               slot = &g_other; g_other_state = state; break;
        }
        slot->hits++;
        if (matrix)
        {
            slot->seen = true;
            memcpy(slot->last, matrix, sizeof(slot->last));
        }
    }

    void probe_end_frame()
    {
        for (int i = 0; i < kMaxRegisters; ++i)
        {
            RegStat& r = g_reg[i];
            if (r.writes_this_frame)
            {
                if (r.writes_this_frame > r.max_per_frame)
                    r.max_per_frame = r.writes_this_frame;
                r.frames_seen++;
                r.writes_this_frame = 0;
            }
        }

        g_frame++;
        const int every = config().probe_every_frames;
        if (g_frame % (unsigned)every == 0)
            probe_report("periodic");
    }

    void probe_report(const char* reason)
    {
        log("---- vertex shader constant probe (%s, frame %u) ----", reason, g_frame);

        int ranked[kMaxRegisters];
        int count = 0;
        for (int i = 0; i < kMaxRegisters; ++i)
            if (g_reg[i].seen)
                ranked[count++] = i;

        // simple insertion sort by score, highest first
        for (int i = 1; i < count; ++i)
        {
            const int key = ranked[i];
            int j = i - 1;
            while (j >= 0 && score(g_reg[ranked[j]]) < score(g_reg[key]))
            {
                ranked[j + 1] = ranked[j];
                --j;
            }
            ranked[j + 1] = key;
        }

        if (count == 0)
        {
            log("  nothing uploaded yet");
            return;
        }

        log("   reg   writes  per-frame  changes  shape        score");
        const int show = count < 14 ? count : 14;
        for (int k = 0; k < show; ++k)
        {
            const int i = ranked[k];
            const RegStat& r = g_reg[i];
            Shape dominant = ShapeProjective;
            for (int s = 1; s < ShapeCount; ++s)
                if (r.shape_count[s] > r.shape_count[dominant])
                    dominant = (Shape)s;

            log("  c%-4d %7u  %9u  %-7s  %-11s %5d",
                i, r.writes_total, r.max_per_frame,
                r.changed ? "yes" : "no", shape_name(dominant), score(r));
        }

        // Dump the actual matrices for the leading registers whether or not
        // anything scored well. When nothing scores, the values are the only
        // way to work out what the engine is actually uploading.
        const int dump = count < 3 ? count : 3;
        for (int k = 0; k < dump; ++k)
        {
            const int i = ranked[k];
            const float* m = g_reg[i].last;
            log("  c%d -- last value (score %d)", i, score(g_reg[i]));
            for (int row = 0; row < 4; ++row)
                log("    [ %12.4f %12.4f %12.4f %12.4f ]",
                    m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]);
        }

        log("  -- SetVertexShaderConstantF call shapes (register x count) --");
        if (g_sig_count == 0)
            log("    none");
        for (int i = 0; i < g_sig_count; ++i)
            log("    c%-4u x%-3u  %10u calls", g_sig[i].start, g_sig[i].count, g_sig[i].hits);
        if (g_sig_dropped)
            log("    (%u further shapes not tracked)", g_sig_dropped);

        log("  -- viewports and render targets (aspect vs the 8/7 the projection implies) --");
        if (g_surface_count == 0)
            log("    none seen");
        for (int i = 0; i < g_surface_count; ++i)
            log("    %-10s %5ux%-5u  aspect %.4f  %8u times",
                g_surface[i].what, g_surface[i].w, g_surface[i].h,
                (double)g_surface[i].w / (double)g_surface[i].h, g_surface[i].hits);

        log("  -- SetTransform (fixed function) --");
        log("    world=%u view=%u projection=%u other(state %u)=%u",
            g_world.hits, g_view.hits, g_proj.hits, g_other_state, g_other.hits);
        {
            const XformStat* named[3] = { &g_view, &g_proj, &g_world };
            const char* names[3] = { "VIEW", "PROJECTION", "WORLD" };
            for (int k = 0; k < 3; ++k)
            {
                if (!named[k]->seen)
                    continue;
                const float* m = named[k]->last;
                log("    %s -- last value", names[k]);
                for (int row = 0; row < 4; ++row)
                    log("      [ %12.4f %12.4f %12.4f %12.4f ]",
                        m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]);
            }
        }

        if (score(g_reg[ranked[0]]) <= 0)
            log("  nothing here looks like a per-view projection -- the engine may only"
                " ever upload a per-object world-view-projection");
        else if (!g_reported_once)
        {
            g_reported_once = true;
            log("  (set probe/constants = 0 in trlvr.ini once this is settled)");
        }
    }
}
