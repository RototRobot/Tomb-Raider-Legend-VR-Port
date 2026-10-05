// CPU time per frame by part; see perf_cpu.h.

#include "perf_cpu.h"

#include "../common/log.h"

#include <windows.h>

namespace trlvr
{
    namespace
    {
        LARGE_INTEGER g_frequency{};
        double g_total[PerfBucketCount]{};
        LARGE_INTEGER g_started[PerfBucketCount]{};
        int g_depth[PerfBucketCount]{};
        unsigned g_calls[PerfBucketCount]{};

        // Frame phases: update = end of Present to the first draw; render =
        // first draw to the next Present.
        LARGE_INTEGER g_frame_start{};
        LARGE_INTEGER g_first_draw{};
        bool g_drawn = false;
        double g_update_ms = 0.0, g_render_ms = 0.0, g_present_ms = 0.0;
        LARGE_INTEGER g_present_start{};

        double elapsed_ms(const LARGE_INTEGER& from, const LARGE_INTEGER& to)
        {
            if (!g_frequency.QuadPart)
                QueryPerformanceFrequency(&g_frequency);
            return double(to.QuadPart - from.QuadPart) * 1000.0 /
                   double(g_frequency.QuadPart);
        }
    }

    void perf_cpu_begin(PerfBucket bucket)
    {
        // Only the outermost call of a bucket is timed (recursion, nesting).
        if (g_depth[bucket]++ == 0)
            QueryPerformanceCounter(&g_started[bucket]);
    }

    void perf_cpu_end(PerfBucket bucket)
    {
        if (g_depth[bucket] <= 0)
            return;
        if (--g_depth[bucket] == 0)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            g_total[bucket] += elapsed_ms(g_started[bucket], now);
            ++g_calls[bucket];
        }
    }

    void perf_cpu_draw()
    {
        if (!g_drawn)
        {
            g_drawn = true;
            QueryPerformanceCounter(&g_first_draw);
            if (g_frame_start.QuadPart)
                g_update_ms += elapsed_ms(g_frame_start, g_first_draw);
        }
    }

    void perf_cpu_present_begin()
    {
        QueryPerformanceCounter(&g_present_start);
        if (g_drawn)
            g_render_ms += elapsed_ms(g_first_draw, g_present_start);
    }

    void perf_cpu_present_end()
    {
        QueryPerformanceCounter(&g_frame_start);
        if (g_present_start.QuadPart)
            g_present_ms += elapsed_ms(g_present_start, g_frame_start);
        g_drawn = false;
    }

    void perf_cpu_report(unsigned frames)
    {
        if (!frames)
            return;
        const double n = frames;
        log("perf cpu: per frame %.1f ms game update before drawing, %.1f "
            "ms rendering (draw calls %.1f, of it mod matrix math %.1f), "
            "%.1f ms at Present (submit "
            "%.1f, input %.1f, rest is WaitGetPoses and the swap)",
            g_update_ms / n, g_render_ms / n, g_total[PerfDrawCalls] / n,
            g_total[PerfDrawMath] / n,
            g_present_ms / n, g_total[PerfSubmit] / n,
            g_total[PerfInput] / n);
        log("perf cpu: mod hooks incl. the game code they wrap, ms per "
            "frame: camera %.2f (%.0f calls), draw Lara %.2f (%.0f), draw "
            "other instances %.2f (%.0f), "
            "visibility %.2f (%.0f), terrain %.2f (%.0f), line probe %.2f "
            "(%.0f), aim target %.2f (%.0f), movement %.2f (%.0f), UI draw "
            "%.2f (%.0f)",
            g_total[PerfCamera] / n, g_calls[PerfCamera] / n,
            g_total[PerfDrawLara] / n, g_calls[PerfDrawLara] / n,
            g_total[PerfDrawInstance] / n, g_calls[PerfDrawInstance] / n,
            g_total[PerfVisibility] / n, g_calls[PerfVisibility] / n,
            g_total[PerfTerrain] / n, g_calls[PerfTerrain] / n,
            g_total[PerfLineProbe] / n, g_calls[PerfLineProbe] / n,
            g_total[PerfAimTarget] / n, g_calls[PerfAimTarget] / n,
            g_total[PerfMovement] / n, g_calls[PerfMovement] / n,
            g_total[PerfUiDraw] / n, g_calls[PerfUiDraw] / n);
        for (int b = 0; b < PerfBucketCount; ++b)
        {
            g_total[b] = 0.0;
            g_calls[b] = 0;
        }
        g_update_ms = g_render_ms = g_present_ms = 0.0;
    }
}
