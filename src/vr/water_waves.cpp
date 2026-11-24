#include "water_waves.h"

#include "tune.h"
#include "vr_math.h"
#include "vr_session.h"

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
        // In FXDRAW_WaterFX: call 0x00406640 (cdecl, five arguments), which
        // queues the grid as 0x8C4 quads of four 32-byte vertices: x, y in
        // -1..1, z, colour, u, v. The quad for column c, row r holds (r, c),
        // (r, c + 1), (r + 1, c), (r + 1, c + 1). The submit scales x and y
        // by depth only (0x00406770), so the base u, v is the screen
        // position for both grids.
        const uintptr_t kSubmitCall = 0x004471F6;
        const uintptr_t kSubmit = 0x00406640;
        const unsigned kQuads = 0x8C4;
        const int kCols = 52;
        const int kRows = 45;
        const int kVerts = kCols * kRows;
        const int kTris = (kCols - 1) * (kRows - 1) * 2;

        // The main camera (as camera_head): its camera-to-world transform,
        // row vectors, camera space in game units (X right, Y down, Z
        // forward), the same space as the headset's head frame.
        const uintptr_t kMainCamera = 0x010FC660;
        const unsigned kOffCwTransform = 0x1E0;
        // WaterFX's wobble phases, advanced by frame time (0x00446EBD).
        const uintptr_t kPhaseU = 0x00F146F8;
        const uintptr_t kPhaseV = 0x00F146F4;
        // Depth of our grid in the game's projection: just past its near
        // plane, as the game's own is (Z test LESSEQUAL, no Z write).
        const float kDepth = 0.0005f;

        typedef unsigned (__cdecl* SubmitFn)(unsigned, unsigned,
                                             const unsigned char*, unsigned,
                                             unsigned);

        struct Vertex
        {
            float x, y, z;
            DWORD colour;
            float u, v;
        };
        static_assert(sizeof(Vertex) == 24, "the game's grid stride");

        // The grids submitted since the last layer was drawn, in order.
        // WaterFX runs twice a frame above water: its first call is laid
        // over the reflection, its second (forced onto the under-water
        // parameters, sort key 500 lower) over the screen copy. Capture and
        // draw are on the same thread (log 2026-11-12).
        WaterLayer g_caps[2];
        int g_cap_count = 0;
        bool g_drawn = true;
        bool g_hooked = false;

        // The water Lara is in: PlayerData::work (0x0111713C, as
        // camera_head) +0x804 points at it and +0x808 is its height, or
        // -16384000 out of the water. WaterFX's own under-water test reads
        // the same height. Kept after she climbs out while the camera stays
        // near where it was last seen.
        const uintptr_t kPlayerWork = 0x0111713C;
        const unsigned kOffWaterZ = 0x808;
        const float kWaterKeepRadius = 30.0f;   // metres
        float g_water_z = 0.0f;
        float g_water_at[2] = { 0.0f, 0.0f };
        bool g_water_known = false;

        WORD g_indices[kTris * 3];
        Vertex g_verts[kVerts];
        float g_wx[kVerts], g_wy[kVerts], g_dx[kVerts], g_dy[kVerts];
        bool g_hit[kVerts];

        // Each eye's grid before a layer's strength and colour: both layers
        // of a frame share the rays and the waves.
        struct EyeWaves
        {
            bool valid;
            float key[14];
            Mat4 to_world;
            float clip[4][4];
            Vertex base[kVerts];
            float wave_u[kVerts], wave_v[kVerts];
            int hits;
            float locked;
        };
        EyeWaves g_eye_waves[2];

        bool finite(float f) { return std::isfinite(f) != 0; }

        bool capture(const unsigned char* quads, WaterLayer* out)
        {
            // Least squares of u against x and v against y; what is left is
            // the wobble.
            const int n = int(kQuads) * 4;
            double sx = 0, su = 0, sxx = 0, sxu = 0;
            double sy = 0, sv = 0, syy = 0, syv = 0;
            for (int i = 0; i < n; ++i)
            {
                const float* f = reinterpret_cast<const float*>(quads + i * 32);
                const double x = f[0], y = f[1], u = f[4], v = f[5];
                sx += x; su += u; sxx += x * x; sxu += x * u;
                sy += y; sv += v; syy += y * y; syv += y * v;
            }
            const double vx = sxx - sx * sx / n, vy = syy - sy * sy / n;
            if (!(vx > 1e-6) || !(vy > 1e-6))
                return false;
            const double bu = (sxu - sx * su / n) / vx;
            const double bv = (syv - sy * sv / n) / vy;
            const double au = (su - bu * sx) / n, av = (sv - bv * sy) / n;
            double eu = 0, ev = 0;
            for (int i = 0; i < n; ++i)
            {
                const float* f = reinterpret_cast<const float*>(quads + i * 32);
                const double du = f[4] - (au + bu * f[0]);
                const double dv = f[5] - (av + bv * f[1]);
                eu += du * du;
                ev += dv * dv;
            }
            out->valid = true;
            // The submit swaps red and blue on the way into the vertex
            // buffer (bswap, ror 8 at 0x004067BF).
            const DWORD raw = *reinterpret_cast<const DWORD*>(quads + 12);
            out->colour = (raw & 0xFF00FF00u) | ((raw >> 16) & 0xFFu) |
                          ((raw & 0xFFu) << 16);
            out->u0 = float(au); out->u1 = float(bu);
            out->v0 = float(av); out->v1 = float(bv);
            out->rms_u = float(std::sqrt(eu / n));
            out->rms_v = float(std::sqrt(ev / n));
            // Both grids are laid out by the same code as the screen
            // position (0.5 + 0.5 x): the wobble leaves the fit a little
            // off, which would keep the two layers from sharing each eye's
            // work.
            float* fit[4] = { &out->u0, &out->u1, &out->v0, &out->v1 };
            for (float* f : fit)
                if (std::fabs(*f - 0.5f) < 0.01f)
                    *f = 0.5f;
            return finite(out->u0) && finite(out->u1) && finite(out->v0) &&
                   finite(out->v1) && finite(out->rms_u) &&
                   finite(out->rms_v);
        }

        unsigned __cdecl submit_detour(unsigned a1, unsigned a2,
                                       const unsigned char* quads,
                                       unsigned count, unsigned a5)
        {
            WaterLayer layer;
            if (quads && count == kQuads && capture(quads, &layer))
            {
                if (g_drawn)
                {
                    g_drawn = false;
                    g_cap_count = 0;
                }
                const int slot = g_cap_count;
                if (g_cap_count < 2)
                    g_caps[g_cap_count++] = layer;

                static int reported = 0;
                static DWORD last = 0;
                const DWORD now = GetTickCount();
                if (reported < 2 || now - last > 60000)
                {
                    ++reported;
                    last = now;
                    log("water waves: grid %d: u = %.4f + %.4f x, v = %.4f + "
                        "%.4f y, wobble rms %.5f / %.5f, colour %08lX", slot,
                        layer.u0, layer.u1, layer.v0, layer.v1, layer.rms_u,
                        layer.rms_v, layer.colour);
                }
            }
            return reinterpret_cast<SubmitFn>(kSubmit)(a1, a2, quads, count,
                                                       a5);
        }

        bool redirect_call(uintptr_t at_address, uintptr_t expected,
                           const void* to)
        {
            unsigned char* at = reinterpret_cast<unsigned char*>(at_address);
            int32_t relative = 0;
            memcpy(&relative, at + 1, sizeof(relative));
            if (at[0] != 0xE8 || uintptr_t(at + 5 + relative) != expected)
                return false;
            unsigned char call[5] = { 0xE8 };
            const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(to) -
                                        (at_address + 5));
            memcpy(call + 1, &rel, sizeof(rel));
            DWORD old = 0;
            if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old))
                return false;
            memcpy(at, call, 5);
            VirtualProtect(at, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 5);
            return true;
        }

        bool read_camera(Mat4* to_world)
        {
            memcpy(to_world->m,
                   reinterpret_cast<const void*>(kMainCamera + kOffCwTransform),
                   sizeof(to_world->m));
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    if (!finite(to_world->m[r][c]))
                        return false;
            return true;
        }

        bool read_water_z(float* z)
        {
            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(kPlayerWork);
                if (!player)
                    return false;
                *z = *reinterpret_cast<const float*>(player + kOffWaterZ);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return finite(*z) && *z > -1.0e6f && *z < 1.0e6f;
        }

        float smoothstep(float a, float b, float x)
        {
            float t = (x - a) / (b - a);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return t * t * (3.0f - 2.0f * t);
        }

        // Two crossing ripples per axis. Each sums to about 0.51 root mean
        // square; the caller scales that to the game's.
        void waves(float cx, float cy, float k1, float k2, float t1,
                   float t2, float* fu, float* fv)
        {
            *fu = 0.6f * std::sin(k1 * (0.80f * cx + 0.60f * cy) + t1) +
                  0.4f * std::sin(k2 * (-0.45f * cx + 0.89f * cy) +
                                  1.7f * t2 + 1.3f);
            *fv = 0.6f * std::cos(k1 * (0.60f * cx - 0.80f * cy) + t2) +
                  0.4f * std::cos(k2 * (0.89f * cx + 0.45f * cy) +
                                  1.7f * t1 + 0.4f);
        }

        void transform(const float p[4], const Mat4& m, float out[4])
        {
            for (int c = 0; c < 4; ++c)
                out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] +
                         p[2] * m.m[2][c] + p[3] * m.m[3][c];
        }
    }

    void water_waves_init()
    {
        if (g_hooked || !config().water_effects || !config().water_waves)
            return;
        for (int r = 0, k = 0; r + 1 < kRows; ++r)
            for (int c = 0; c + 1 < kCols; ++c)
            {
                const WORD i0 = WORD(r * kCols + c), i2 = WORD(i0 + kCols);
                g_indices[k++] = i0;
                g_indices[k++] = WORD(i0 + 1);
                g_indices[k++] = i2;
                g_indices[k++] = WORD(i0 + 1);
                g_indices[k++] = WORD(i2 + 1);
                g_indices[k++] = i2;
            }
        if (!redirect_call(kSubmitCall, kSubmit,
                           reinterpret_cast<const void*>(&submit_detour)))
        {
            log("water waves: the grid submit call at %08X is not the "
                "retail one; the game's waves are kept",
                unsigned(kSubmitCall));
            return;
        }
        g_hooked = true;
        log("water waves: grid submit hooked; waves follow the water surface "
            "in each eye (wave size %.2f m)", config().water_wave_size);
    }

    bool water_waves_begin_layer(int kind, WaterLayer* out)
    {
        if (!g_hooked || !out)
            return false;
        int pick = -1;
        if (g_cap_count == 2)
            pick = kind == 0 ? 1 : 0;
        else if (g_cap_count == 1)
            pick = 0;
        if (pick >= 0)
            *out = g_caps[pick];
        g_drawn = true;
        static bool reported[2] = { false, false };
        if (pick >= 0 && (kind == 0 || kind == 1) && !reported[kind])
        {
            reported[kind] = true;
            log("water waves: the %s layer uses grid %d of %d",
                kind ? "reflection" : "screen copy", pick, g_cap_count);
        }
        return pick >= 0 && out->valid;
    }

    bool water_waves_draw(IDirect3DDevice9* device, const WaterLayer& layer,
                          int eye, const float* c0)
    {
        if (!g_hooked || !device || !layer.valid || !c0 ||
            (eye != EyeLeft && eye != EyeRight) || vr_dark_room_active())
            return false;

        // The game's projection: x' = P00 x, y' = P11 y, z' = A z + B, w = z.
        const float p00 = c0[0], p11 = c0[5], a = c0[10], b = c0[11];
        const float z0 = b / (kDepth - a);
        if (!(z0 > 0.0f) || !finite(z0) || std::fabs(p00) < 1e-4f ||
            std::fabs(p11) < 1e-4f)
            return false;

        // This eye: head frame (game units) <-> its clip space.
        float clip[4][4];
        if (!vr_head_clip_matrix(Eye(eye), clip))
            return false;
        Mat4 to_clip, from_clip;
        memcpy(to_clip.m, clip, sizeof(clip));
        if (!invert(to_clip, &from_clip))
            return false;
        Mat4 to_world;
        if (!read_camera(&to_world))
            return false;
        // Where this eye is in the head frame: its offset from the middle
        // of the two eyes. The eye-to-clip matrix only gives the rays'
        // directions: its origin follows the runtime's eye offsets, which
        // the OpenXR simulator reports from the floor (y 1.70 m), while the
        // game camera is already at the head.
        const EyeInfo& left_eye = vr_eye(EyeLeft);
        const EyeInfo& right_eye = vr_eye(EyeRight);
        const EyeInfo& this_eye = vr_eye(Eye(eye));
        const float s = tune_world_scale() * tune_move_scale();
        const float o[4] = {
            s * (this_eye.offset_x -
                 0.5f * (left_eye.offset_x + right_eye.offset_x)),
            -s * (this_eye.offset_y -
                  0.5f * (left_eye.offset_y + right_eye.offset_y)),
            -s * (this_eye.offset_z -
                  0.5f * (left_eye.offset_z + right_eye.offset_z)),
            1.0f };

        // The water's height: Lara's water, or the last one seen nearby.
        const float metre = config().world_scale;
        float lara_water = 0.0f;
        if (read_water_z(&lara_water))
        {
            static int reported = 0;
            if ((!g_water_known || std::fabs(g_water_z - lara_water) > 1.0f) &&
                reported < 20)
            {
                ++reported;
                log("water waves: water surface at height %.1f (camera %.1f)",
                    lara_water, to_world.m[3][2]);
            }
            g_water_z = lara_water;
            g_water_at[0] = to_world.m[3][0];
            g_water_at[1] = to_world.m[3][1];
            g_water_known = true;
        }
        else if (g_water_known &&
                 std::hypot(to_world.m[3][0] - g_water_at[0],
                            to_world.m[3][1] - g_water_at[1]) >
                     kWaterKeepRadius * metre)
        {
            g_water_known = false;
            log("water waves: left the last water seen; waves locked to the "
                "view direction until Lara is in water again");
        }
        const bool plane = g_water_known;

        // The game's wobble is in texture units of its own (narrower)
        // view; keep it the same angle in the headset's.
        const EyeInfo& info = vr_eye(Eye(eye));
        const float dx = info.tan_right - info.tan_left;
        const float dy = info.tan_bottom - info.tan_top;
        if (!(dx > 1e-3f) || !(dy > 1e-3f))
            return false;
        const float scale_u = (2.0f / dx) / std::fabs(p00);
        const float scale_v = (2.0f / dy) / std::fabs(p11);

        float size = config().water_wave_size;
        if (!(size > 0.01f))
            size = 0.6f;
        const float fine = 0.61f * size * metre;
        const float k1 = 6.2831853f / (size * metre);
        const float k2 = 6.2831853f / fine;
        // Direction-locked fallback: about 8 degrees per longer wave, so
        // the shorter one still spans two grid cells (about 2 degrees each
        // across the headset's view).
        const float radius = size * metre / 0.14f;
        const float t1 = *reinterpret_cast<const float*>(kPhaseU);
        const float t2 = *reinterpret_cast<const float*>(kPhaseV);
        // 1000 turns: whole turns for both 1 and 1.7 times the phase.
        const float phase_u = finite(t1) ? std::fmod(t1, 6283.1853f) : 0.0f;
        const float phase_v = finite(t2) ? std::fmod(t2, 6283.1853f) : 0.0f;
        const float gain_u = layer.rms_u / 0.51f * scale_u;
        const float gain_v = layer.rms_v / 0.51f * scale_v;

        EyeWaves& cache = g_eye_waves[eye];
        const float key[14] = { phase_u, phase_v,
                                plane ? g_water_z : -1.0e30f, z0, p00, p11,
                                layer.u0, layer.u1, layer.v0, layer.v1,
                                o[0], o[1], o[2], size };
        const bool same = cache.valid &&
            memcmp(cache.key, key, sizeof(key)) == 0 &&
            memcmp(&cache.to_world, &to_world, sizeof(to_world)) == 0 &&
            memcmp(cache.clip, clip, sizeof(clip)) == 0;
        if (!same)
        {
            int hits = 0;
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                {
                    const int i = r * kCols + c;
                    const float gx = c * (2.0f / (kCols - 1)) - 1.0f;
                    const float gy = r * (2.0f / (kRows - 1)) - 1.0f;
                    const float bu = layer.u0 + layer.u1 * gx;
                    const float bv = layer.v0 + layer.v1 * gy;
                    // The undistorted coordinate is the screen position.
                    const float nx = 2.0f * bu - 1.0f;
                    const float ny = 1.0f - 2.0f * bv;
                    Vertex& vert = cache.base[i];
                    vert.x = nx * z0 / p00;
                    vert.y = ny * z0 / p11;
                    vert.z = z0;
                    vert.colour = 0;
                    vert.u = bu;
                    vert.v = bv;

                    // This eye's ray through that point, in the world.
                    const float pn[4] = { nx, ny, 0.0f, 1.0f };
                    const float pf[4] = { nx, ny, 1.0f, 1.0f };
                    float hn[4], hf[4];
                    transform(pn, from_clip, hn);
                    transform(pf, from_clip, hf);
                    g_hit[i] = false;
                    g_dx[i] = g_dy[i] = 0.0f;
                    if (!(std::fabs(hn[3]) > 1e-12f) ||
                        !(std::fabs(hf[3]) > 1e-12f))
                        continue;
                    const float d[4] = {
                        hf[0] / hf[3] - hn[0] / hn[3],
                        hf[1] / hf[3] - hn[1] / hn[3],
                        hf[2] / hf[3] - hn[2] / hn[3], 0.0f };
                    float wo[4], wd[4];
                    transform(o, to_world, wo);
                    transform(d, to_world, wd);
                    const float len = std::sqrt(
                        wd[0] * wd[0] + wd[1] * wd[1] + wd[2] * wd[2]);
                    if (!(len > 1e-9f))
                        continue;
                    wd[0] /= len; wd[1] /= len; wd[2] /= len;
                    g_dx[i] = radius * (wd[0] + 0.5f * wd[2]);
                    g_dy[i] = radius * (wd[1] - 0.5f * wd[2]);
                    if (!plane)
                        continue;
                    // Water is level: z = height.
                    if (std::fabs(wd[2]) < 1e-4f)
                        continue;
                    const float t = (g_water_z - wo[2]) / wd[2];
                    if (!(t > 0.0f) || !(t < 1.0e6f))
                        continue;
                    g_hit[i] = true;
                    g_wx[i] = wo[0] + t * wd[0];
                    g_wy[i] = wo[1] + t * wd[1];
                    ++hits;
                }

            float weight_sum = 0.0f;
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                {
                    const int i = r * kCols + c;
                    // How far apart neighbouring grid points land on the
                    // water: waves finer than about two spacings would
                    // alias, so there the direction-locked ones take over.
                    float weight = 0.0f;
                    if (g_hit[i])
                    {
                        const int j = c + 1 < kCols ? i + 1 : i - 1;
                        const int k = r + 1 < kRows ? i + kCols : i - kCols;
                        if (g_hit[j] && g_hit[k])
                        {
                            const float sj = std::hypot(g_wx[j] - g_wx[i],
                                                        g_wy[j] - g_wy[i]);
                            const float sk = std::hypot(g_wx[k] - g_wx[i],
                                                        g_wy[k] - g_wy[i]);
                            const float spacing = sj > sk ? sj : sk;
                            if (spacing > 1e-6f)
                                weight = smoothstep(2.0f, 3.5f,
                                                    fine / spacing);
                        }
                    }
                    weight_sum += weight;
                    float su = 0.0f, sv = 0.0f, du = 0.0f, dv = 0.0f;
                    if (weight > 0.0f)
                        waves(g_wx[i], g_wy[i], k1, k2, phase_u,
                              phase_v, &su, &sv);
                    if (weight < 1.0f)
                        waves(g_dx[i], g_dy[i], k1, k2, phase_u,
                              phase_v, &du, &dv);
                    cache.wave_u[i] = weight * su + (1.0f - weight) * du;
                    cache.wave_v[i] = weight * sv + (1.0f - weight) * dv;
                }
            memcpy(cache.key, key, sizeof(key));
            cache.to_world = to_world;
            memcpy(cache.clip, clip, sizeof(clip));
            cache.hits = hits;
            cache.locked = weight_sum / kVerts;
            cache.valid = true;
        }

        for (int i = 0; i < kVerts; ++i)
        {
            Vertex& vert = g_verts[i];
            vert = cache.base[i];
            vert.colour = layer.colour;
            vert.u += gain_u * cache.wave_u[i];
            vert.v += gain_v * cache.wave_v[i];
        }

        static unsigned reused = 0, computed = 0;
        ++(same ? reused : computed);
        static int reported = 0;
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (reported < 2 || (now - last > 10000 && reported < 40))
        {
            ++reported;
            last = now;
            log("water waves: eye %d: game P00 %.3f P11 %.3f, wobble "
                "x%.2f/x%.2f for the headset's view, grid depth %.3f, camera "
                "(%.0f, %.0f, %.0f), plane %s (height %.0f), %d of %d points "
                "on the water, %.0f%% surface-locked (grids worked out %u, "
                "reused %u)", eye, p00, p11, scale_u, scale_v, z0,
                to_world.m[3][0], to_world.m[3][1], to_world.m[3][2],
                plane ? "yes" : "none", g_water_z, cache.hits, kVerts,
                100.0f * cache.locked, computed, reused);
        }

        // Drawing from memory unbinds stream 0 and the indices.
        IDirect3DVertexBuffer9* stream = nullptr;
        UINT offset = 0, stride = 0;
        device->GetStreamSource(0, &stream, &offset, &stride);
        IDirect3DIndexBuffer9* indices = nullptr;
        device->GetIndices(&indices);
        const HRESULT hr = device->DrawIndexedPrimitiveUP(
            D3DPT_TRIANGLELIST, 0, kVerts, kTris, g_indices, D3DFMT_INDEX16,
            g_verts, sizeof(Vertex));
        device->SetStreamSource(0, stream, offset, stride);
        device->SetIndices(indices);
        if (stream)
            stream->Release();
        if (indices)
            indices->Release();
        if (FAILED(hr))
        {
            static bool failed = false;
            if (!failed)
            {
                failed = true;
                log("water waves: drawing the grid failed (%08lX); the "
                    "game's is drawn instead", unsigned long(hr));
            }
            return false;
        }
        return true;
    }
}
