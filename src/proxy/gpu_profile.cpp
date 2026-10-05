// One-frame GPU profile (F3); see gpu_profile.h.

#include "gpu_profile.h"

#include "../common/log.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace trlvr
{
    namespace
    {
        constexpr int kMaxStamps = 768;

        struct Segment
        {
            char label[128];
            unsigned draws;
            unsigned primitives;
        };

        enum class State { Idle, Recording, Waiting };

        IDirect3DDevice9* g_device = nullptr;
        IDirect3DQuery9* g_stamps[kMaxStamps]{};
        IDirect3DQuery9* g_disjoint = nullptr;
        IDirect3DQuery9* g_frequency = nullptr;
        Segment g_segments[kMaxStamps];
        int g_segment_count = 0;   // segment i runs from stamp i to i + 1
        State g_state = State::Idle;
        bool g_requested = false;
        int g_wait_frames = 0;
        unsigned g_profiles = 0;

        char g_target[64] = "RT ?";
        bool g_key_valid = false;
        void* g_key_caller = nullptr;
        DWORD g_key_blend = 0;

        void release_queries()
        {
            for (IDirect3DQuery9*& q : g_stamps)
                if (q)
                {
                    q->Release();
                    q = nullptr;
                }
            if (g_disjoint)
            {
                g_disjoint->Release();
                g_disjoint = nullptr;
            }
            if (g_frequency)
            {
                g_frequency->Release();
                g_frequency = nullptr;
            }
        }

        bool create_queries()
        {
            if (!g_device)
                return false;
            if (FAILED(g_device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,
                                             &g_disjoint)) ||
                FAILED(g_device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,
                                             &g_frequency)))
            {
                release_queries();
                return false;
            }
            for (IDirect3DQuery9*& q : g_stamps)
                if (FAILED(g_device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)))
                {
                    release_queries();
                    return false;
                }
            return true;
        }

        void describe_surface(IDirect3DSurface9* surface, char* out,
                              size_t size)
        {
            D3DSURFACE_DESC d{};
            if (!surface || FAILED(surface->GetDesc(&d)))
            {
                snprintf(out, size, "?");
                return;
            }
            const int samples = d.MultiSampleType == D3DMULTISAMPLE_NONMASKABLE
                ? (1 << d.MultiSampleQuality) : int(d.MultiSampleType);
            snprintf(out, size, "%ux%u fmt %d%s", d.Width, d.Height,
                     int(d.Format), samples > 1
                         ? (samples == 2 ? " MSAA2" : samples == 4
                                ? " MSAA4" : samples == 8 ? " MSAA8"
                                                          : " MSAA")
                         : "");
        }

        // Ends the current segment and starts one with this label. The
        // last stamp is kept for the frame's end.
        void stamp(const char* label)
        {
            if (g_segment_count >= kMaxStamps - 1)
                return;
            g_stamps[g_segment_count]->Issue(D3DISSUE_END);
            Segment& s = g_segments[g_segment_count++];
            strncpy_s(s.label, label, _TRUNCATE);
            s.draws = 0;
            s.primitives = 0;
        }

        void report()
        {
            UINT64 frequency = 0;
            BOOL disjoint = FALSE;
            g_frequency->GetData(&frequency, sizeof(frequency), 0);
            g_disjoint->GetData(&disjoint, sizeof(disjoint), 0);
            if (!frequency)
            {
                log("gpu profile: no timestamp frequency -- not supported");
                return;
            }
            UINT64 times[kMaxStamps]{};
            for (int i = 0; i <= g_segment_count; ++i)
                g_stamps[i]->GetData(&times[i], sizeof(times[i]), 0);

            struct Group
            {
                const char* label;
                double ms;
                unsigned draws;
                unsigned primitives;
                unsigned pieces;
            };
            static Group groups[kMaxStamps];
            int group_count = 0;
            double total = 0.0;
            for (int i = 0; i < g_segment_count; ++i)
            {
                const double ms = times[i + 1] >= times[i]
                    ? double(times[i + 1] - times[i]) * 1000.0 /
                      double(frequency) : 0.0;
                total += ms;
                int g = 0;
                while (g < group_count &&
                       strcmp(groups[g].label, g_segments[i].label) != 0)
                    ++g;
                if (g == group_count)
                    groups[group_count++] = { g_segments[i].label, 0.0, 0,
                                              0, 0 };
                groups[g].ms += ms;
                groups[g].draws += g_segments[i].draws;
                groups[g].primitives += g_segments[i].primitives;
                ++groups[g].pieces;
            }
            std::sort(groups, groups + group_count,
                      [](const Group& a, const Group& b) {
                          return a.ms > b.ms;
                      });
            log("gpu profile %u: %.2f ms GPU from first draw to VR submit, "
                "%d segments in %d groups%s", g_profiles, total,
                g_segment_count, group_count,
                disjoint ? " (DISJOINT: timings unreliable)" : "");
            for (int g = 0; g < group_count && g < 40; ++g)
            {
                if (groups[g].ms < 0.05 && g >= 10)
                    break;
                log("gpu profile %u: %6.2f ms %4.1f%%  %s  (%u draws, %u "
                    "prims, %u pieces)", g_profiles, groups[g].ms,
                    total > 0.0 ? 100.0 * groups[g].ms / total : 0.0,
                    groups[g].label, groups[g].draws,
                    groups[g].primitives, groups[g].pieces);
            }
        }
    }

    void gpu_profile_request()
    {
        if (g_state == State::Idle)
        {
            g_requested = true;
            log("gpu profile: F3 -- timing the next frame on the GPU");
        }
    }

    void gpu_profile_set_device(IDirect3DDevice9* device)
    {
        if (device == g_device)
            return;
        release_queries();
        g_state = State::Idle;
        g_device = device;
    }

    void gpu_profile_draw(void* caller, unsigned primitives)
    {
        if (g_state != State::Recording || !g_device)
            return;
        DWORD blend = 0;
        g_device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        if (!g_key_valid || caller != g_key_caller || blend != g_key_blend)
        {
            char label[128];
            snprintf(label, sizeof(label), "draw to %s | code %p | %s",
                     g_target, caller, blend ? "blended" : "opaque");
            stamp(label);
            g_key_valid = true;
            g_key_caller = caller;
            g_key_blend = blend;
        }
        if (g_segment_count > 0)
        {
            ++g_segments[g_segment_count - 1].draws;
            g_segments[g_segment_count - 1].primitives += primitives;
        }
    }

    void gpu_profile_render_target(IDirect3DSurface9* surface)
    {
        if (g_state != State::Recording)
            return;
        char d[48];
        describe_surface(surface, d, sizeof(d));
        snprintf(g_target, sizeof(g_target), "RT %s", d);
        g_key_valid = false;
    }

    void gpu_profile_event_begin(const char* what, IDirect3DSurface9* source,
                                 IDirect3DSurface9* destination)
    {
        if (g_state != State::Recording)
            return;
        char s[48] = "", d[48] = "", label[128];
        if (source)
            describe_surface(source, s, sizeof(s));
        describe_surface(destination, d, sizeof(d));
        if (source)
            snprintf(label, sizeof(label), "%s %s -> %s", what, s, d);
        else
            snprintf(label, sizeof(label), "%s %s", what, d);
        stamp(label);
    }

    void gpu_profile_event_end()
    {
        if (g_state != State::Recording)
            return;
        stamp("(between draws)");
        g_key_valid = false;
    }

    void gpu_profile_mark(const char* what)
    {
        if (g_state != State::Recording)
            return;
        stamp(what);
        g_key_valid = false;
    }

    void gpu_profile_frame_end()
    {
        if (!g_device)
            return;
        if (g_state == State::Recording)
        {
            // The closing stamp of the last segment.
            g_stamps[g_segment_count]->Issue(D3DISSUE_END);
            g_disjoint->Issue(D3DISSUE_END);
            g_state = State::Waiting;
            g_wait_frames = 0;
            return;
        }
        if (g_state == State::Waiting)
        {
            const DWORD flush = g_wait_frames == 0 ? D3DGETDATA_FLUSH : 0;
            bool ready = g_disjoint->GetData(nullptr, 0, flush) == S_OK &&
                         g_frequency->GetData(nullptr, 0, flush) == S_OK;
            for (int i = 0; i <= g_segment_count && ready; ++i)
                ready = g_stamps[i]->GetData(nullptr, 0, flush) == S_OK;
            if (ready)
            {
                ++g_profiles;
                report();
                release_queries();
                g_state = State::Idle;
            }
            else if (++g_wait_frames > 120)
            {
                log("gpu profile: results did not arrive -- dropped");
                release_queries();
                g_state = State::Idle;
            }
            return;
        }
        if (!g_requested)
            return;
        g_requested = false;
        if (!create_queries())
        {
            log("gpu profile: timestamp queries unavailable");
            return;
        }
        g_disjoint->Issue(D3DISSUE_BEGIN);
        g_frequency->Issue(D3DISSUE_END);
        g_segment_count = 0;
        g_key_valid = false;
        IDirect3DSurface9* target = nullptr;
        if (SUCCEEDED(g_device->GetRenderTarget(0, &target)) && target)
        {
            char d[48];
            describe_surface(target, d, sizeof(d));
            snprintf(g_target, sizeof(g_target), "RT %s", d);
            target->Release();
        }
        g_state = State::Recording;
        stamp("(frame start)");
    }
}
