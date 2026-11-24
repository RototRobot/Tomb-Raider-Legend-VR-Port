#include "env_reflect.h"

#include "camera_head.h"
#include "vr_math.h"

#include "../common/log.h"

#include <cmath>
#include <cstring>

namespace trlvr
{
    namespace
    {
        struct Known
        {
            IDirect3DVertexShader9* shader;
            int base;   // the right axis register; the down axis follows
        };
        Known g_known[64];
        int g_known_count = 0;
        int g_current_base = -1;
        int g_applied_base = -1;

        float g_regs[256][4];
        bool g_set[256];

        // Parameter tokens of each shader model 1.x instruction (the
        // length field only exists from 2.0).
        int params_1x(unsigned op)
        {
            switch (op)
            {
            case 0: return 0;                          // nop
            case 1: case 6: case 7: case 14: case 15: case 16: case 19:
            case 0x4E: case 0x4F: return 2;            // mov rcp rsq exp
                                                       // log lit frc
                                                       // expp logp
            case 2: case 3: case 5: case 8: case 9: case 10: case 11:
            case 12: case 13: case 17: case 20: case 21: case 22: case 23:
            case 24: return 3;                         // two sources
            case 4: case 18: return 4;                 // mad lrp
            case 31: return 2;                         // dcl
            case 0x51: return 5;                       // def
            default: return -1;
            }
        }

        int reg_type(DWORD t) { return int(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
        int reg_index(DWORD t) { return int(t & 0x7FF); }

        // dp3 r.x, ..., c[k]; dp3 r.y, ..., c[k+1]; add oT.xy, r, c: the
        // reflection lookup. Returns k, or -1.
        int match(const DWORD* code)
        {
            if (!code || (code[0] & 0xFFFF0000u) != 0xFFFE0000u)
                return -1;
            const unsigned major = (code[0] >> 8) & 0xFF;
            int x_axis[8], y_axis[8], nx = 0, ny = 0;
            bool texcoord_add = false;
            const DWORD* p = code + 1;
            for (int guard = 0; guard < 4096; ++guard)
            {
                const DWORD t = *p;
                if (t == 0x0000FFFF)
                    break;
                const unsigned op = t & 0xFFFF;
                if (op == 0xFFFE)
                {
                    p += 1 + ((t >> 16) & 0x7FFF);
                    continue;
                }
                const int n = major >= 2 ? int((t >> 24) & 0xF)
                                         : params_1x(op);
                if (n < 0)
                    return -1;
                const DWORD* a = p + 1;
                if (op == 8 && n >= 3 && reg_type(a[0]) == 0)
                {
                    const unsigned mask = (a[0] >> 16) & 0xF;
                    for (int s = 1; s <= 2; ++s)
                        if (reg_type(a[s]) == 2)
                        {
                            if (mask == 1 && nx < 8)
                                x_axis[nx++] = reg_index(a[s]);
                            else if (mask == 2 && ny < 8)
                                y_axis[ny++] = reg_index(a[s]);
                        }
                }
                if (op == 2 && n >= 3 && reg_type(a[0]) == 6 &&
                    (reg_type(a[1]) == 2 || reg_type(a[2]) == 2))
                    texcoord_add = true;
                p += 1 + n;
            }
            if (!texcoord_add)
                return -1;
            for (int i = 0; i < nx; ++i)
                for (int j = 0; j < ny; ++j)
                    if (y_axis[j] == x_axis[i] + 1 && x_axis[i] >= 4 &&
                        x_axis[i] < 254)
                        return x_axis[i];
            return -1;
        }
    }

    void env_reflect_shader_created(IDirect3DVertexShader9* shader,
                                    const DWORD* function)
    {
        if (!shader)
            return;
        const int base = match(function);
        for (int i = 0; i < g_known_count; ++i)
            if (g_known[i].shader == shader)
            {
                g_known[i] = g_known[--g_known_count];
                break;
            }
        if (base < 0)
            return;
        if (g_known_count < 64)
            g_known[g_known_count++] = { shader, base };
        log("env reflect: shader %p looks reflections up through c%d/c%d "
            "(camera axes); the head's turn is taken out of them",
            (void*)shader, base, base + 1);
    }

    void env_reflect_shader_set(IDirect3DVertexShader9* shader)
    {
        g_current_base = -1;
        if (!shader)
            return;
        for (int i = 0; i < g_known_count; ++i)
            if (g_known[i].shader == shader)
            {
                g_current_base = g_known[i].base;
                return;
            }
    }

    void env_reflect_constants(UINT start, const float* data, UINT count)
    {
        if (!data)
            return;
        for (UINT i = 0; i < count && start + i < 256; ++i)
        {
            memcpy(g_regs[start + i], data + i * 4, 4 * sizeof(float));
            g_set[start + i] = true;
        }
    }

    bool env_reflect_before_draw(IDirect3DDevice9* device)
    {
        g_applied_base = -1;
        const int k = g_current_base;
        if (!device || k < 0 || !g_set[k] || !g_set[k + 1])
            return false;
        Mat4 head;
        if (!camera_head_view_applied(&head))
            return false;
        // Camera axes in the object's space: right, down, and forward =
        // right x down (the engine's camera frame).
        const float* right = g_regs[k];
        const float* down = g_regs[k + 1];
        const float lr = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                   right[2] * right[2]);
        const float ld = std::sqrt(down[0] * down[0] + down[1] * down[1] +
                                   down[2] * down[2]);
        if (!(lr > 1e-6f) || !(ld > 1e-6f))
            return false;
        float axes[3][3];
        for (int i = 0; i < 3; ++i)
        {
            axes[0][i] = right[i];
            axes[1][i] = down[i];
        }
        axes[2][0] = (right[1] * down[2] - right[2] * down[1]) / ld;
        axes[2][1] = (right[2] * down[0] - right[0] * down[2]) / ld;
        axes[2][2] = (right[0] * down[1] - right[1] * down[0]) / ld;
        // The view is game view * head, so the game camera's axes are the
        // head's rows combined: body_j = sum_k head[j][k] * axis_k.
        float out[2][4];
        for (int j = 0; j < 2; ++j)
        {
            const float lh = std::sqrt(head.m[j][0] * head.m[j][0] +
                                       head.m[j][1] * head.m[j][1] +
                                       head.m[j][2] * head.m[j][2]);
            if (!(lh > 1e-6f))
                return false;
            for (int i = 0; i < 3; ++i)
                out[j][i] = (head.m[j][0] * axes[0][i] +
                             head.m[j][1] * axes[1][i] +
                             head.m[j][2] * axes[2][i]) / lh;
            out[j][3] = g_regs[k + j][3];
            for (int i = 0; i < 4; ++i)
                if (!std::isfinite(out[j][i]))
                    return false;
        }
        if (FAILED(device->SetVertexShaderConstantF(UINT(k), &out[0][0], 2)))
            return false;
        g_applied_base = k;
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            log("env reflect: first reflection draw, axes (%.3f %.3f %.3f) / "
                "(%.3f %.3f %.3f) -> (%.3f %.3f %.3f) / (%.3f %.3f %.3f)",
                right[0], right[1], right[2], down[0], down[1], down[2],
                out[0][0], out[0][1], out[0][2], out[1][0], out[1][1],
                out[1][2]);
        }
        return true;
    }

    void env_reflect_after_draw(IDirect3DDevice9* device)
    {
        const int k = g_applied_base;
        g_applied_base = -1;
        if (device && k >= 0)
            device->SetVertexShaderConstantF(UINT(k), g_regs[k], 2);
    }
}
