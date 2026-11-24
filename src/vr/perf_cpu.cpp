// CPU time per frame by part; see perf_cpu.h.

#include "perf_cpu.h"

#include "../common/log.h"

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace trlvr
{
    namespace
    {
        // Sampling profiler for the render thread. A helper thread suspends
        // it every millisecond or so, reads the instruction pointer and
        // counts it by module and 16-byte block, plus a per-module total.
        // Nothing that can take a lock runs while the thread is suspended:
        // the module lookup is done after the resume.
        struct Sample { HMODULE module; DWORD rva; unsigned count; };
        constexpr int kSampleSlots = 4096;
        constexpr int kModuleSlots = 64;
        struct ModuleCount { HMODULE module; unsigned count; };
        Sample g_samples[kSampleSlots]{};
        ModuleCount g_modules[kModuleSlots]{};
        int g_sample_used = 0, g_module_used = 0;
        unsigned g_sample_total = 0, g_sample_dropped = 0;
        // Initialised at load: the first report can come before the first
        // draw has started the sampler.
        struct SampleLock
        {
            CRITICAL_SECTION cs;
            SampleLock() { InitializeCriticalSection(&cs); }
        } g_sample_lock_holder;
        CRITICAL_SECTION& g_sample_lock = g_sample_lock_holder.cs;
        HANDLE g_render_thread = nullptr;
        bool g_sampler_checked = false;
        bool g_sampler_on = false;

        // Callers of system-module samples: (leaf, caller) pairs.
        struct CallerSample { HMODULE leaf; DWORD leaf_rva; HMODULE caller; DWORD caller_rva; unsigned count; };
        constexpr int kCallerSlots = 2048;
        constexpr int kStackWords = 512;
        CallerSample g_callers[kCallerSlots]{};
        int g_caller_used = 0;
        unsigned g_caller_system = 0;
        // Samples whose first caller is DXVK: (leaf, dxvk caller, outer caller).
        struct WaitSample { HMODULE leaf; DWORD leaf_rva; DWORD dxvk_rva; HMODULE outer; DWORD outer_rva; unsigned count; };
        constexpr int kWaitSlots = 2048;
        WaitSample g_waits[kWaitSlots]{};
        int g_wait_used = 0;
        unsigned g_wait_total = 0;
        HMODULE g_dxvk_module = nullptr;
        HMODULE g_system_modules[16]{};
        int g_system_count = 0;

        void cache_system_modules()
        {
            static const wchar_t* const names[] = {
                L"ntdll.dll", L"win32u.dll", L"kernelbase.dll", L"kernel32.dll",
                L"user32.dll", L"gdi32.dll", L"gdi32full.dll", L"wow64.dll",
                L"wow64cpu.dll", L"wow64win.dll" };
            for (const wchar_t* n : names)
            {
                HMODULE h = GetModuleHandleW(n);
                if (h && g_system_count < 16)
                    g_system_modules[g_system_count++] = h;
            }
        }

        bool is_system_module(HMODULE m)
        {
            for (int i = 0; i < g_system_count; ++i)
                if (g_system_modules[i] == m)
                    return true;
            return false;
        }

        // PODs only (needed for __try); no calls that can take a lock.
        bool copy_words(const DWORD* from, DWORD* out, int n)
        {
            __try
            {
                for (int i = 0; i < n; ++i)
                    out[i] = from[i];
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool read_bytes(DWORD addr, unsigned char* out, int n)
        {
            __try
            {
                const volatile unsigned char* p = reinterpret_cast<const volatile unsigned char*>(static_cast<uintptr_t>(addr));
                for (int i = 0; i < n; ++i)
                    out[i] = p[i];
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool looks_like_return(DWORD addr)
        {
            unsigned char b[6];
            if (!read_bytes(addr - 6, b, 6))
                return false;
            return b[1] == 0xE8 ||
                   (b[0] == 0xFF && (b[1] & 0x38) == 0x10) ||
                   (b[4] == 0xFF && (b[5] & 0x38) == 0x10) ||
                   (b[3] == 0xFF && (b[4] & 0x38) == 0x10);
        }

        HMODULE module_of(DWORD addr)
        {
            HMODULE m = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &m))
                return m;
            return nullptr;
        }

        // DXVK worker threads, found by name (SetThreadDescription) every ~2 s.
        // Handles and enumeration are sampler-thread only; the counters are
        // shared with the report under g_sample_lock.
        constexpr int kTrackKinds = 3;
        constexpr int kTrackModules = 16;
        const wchar_t* const kTrackNames[kTrackKinds] = { L"dxvk-cs", L"dxvk-submit", L"dxvk-queue" };
        const char* const kTrackLabels[kTrackKinds] = { "dxvk-cs", "dxvk-submit", "dxvk-queue" };
        struct TrackedThread { DWORD tid; HANDLE handle; int kind; };
        TrackedThread g_tracked[16]{};
        int g_tracked_count = 0;
        struct TrackStats
        {
            unsigned total, waiting, module_used;
            ModuleCount modules[kTrackModules];
        };
        TrackStats g_track_stats[kTrackKinds]{};
        HMODULE g_wait_modules[2]{};
        typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR*);
        GetThreadDescriptionFn g_get_description = nullptr;

        void refresh_tracked()
        {
            if (!g_get_description)
                return;
            for (int i = 0; i < g_tracked_count; ++i)
                CloseHandle(g_tracked[i].handle);
            g_tracked_count = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return;
            const DWORD pid = GetCurrentProcessId();
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid || g_tracked_count >= 16)
                    continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h)
                    continue;
                int kind = -1;
                PWSTR desc = nullptr;
                if (SUCCEEDED(g_get_description(h, &desc)) && desc)
                {
                    for (int k = 0; k < kTrackKinds; ++k)
                        if (wcscmp(desc, kTrackNames[k]) == 0)
                            kind = k;
                    LocalFree(desc);
                }
                if (kind < 0)
                {
                    CloseHandle(h);
                    continue;
                }
                g_tracked[g_tracked_count++] = { te.th32ThreadID, h, kind };
            }
            CloseHandle(snap);
        }

        void sample_tracked()
        {
            for (int i = 0; i < g_tracked_count; ++i)
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                bool got = false;
                if (SuspendThread(g_tracked[i].handle) != (DWORD)-1)
                {
                    got = GetThreadContext(g_tracked[i].handle, &ctx) != 0;
                    ResumeThread(g_tracked[i].handle);
                }
                if (!got)
                    continue;
                const HMODULE module = module_of(ctx.Eip);
                bool waiting = false;
                for (HMODULE w : g_wait_modules)
                    if (module && module == w)
                        waiting = true;
                EnterCriticalSection(&g_sample_lock);
                TrackStats& s = g_track_stats[g_tracked[i].kind];
                ++s.total;
                if (waiting)
                    ++s.waiting;
                unsigned m = 0;
                for (; m < s.module_used; ++m)
                    if (s.modules[m].module == module)
                        break;
                if (m < s.module_used)
                    ++s.modules[m].count;
                else if (s.module_used < kTrackModules)
                    s.modules[s.module_used++] = { module, 1 };
                LeaveCriticalSection(&g_sample_lock);
            }
        }

        DWORD WINAPI sampler_main(void*)
        {
            timeBeginPeriod(1);
            cache_system_modules();
            g_wait_modules[0] = GetModuleHandleW(L"ntdll.dll");
            g_wait_modules[1] = GetModuleHandleW(L"wow64cpu.dll");
            {
                HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
                if (!kb)
                    kb = GetModuleHandleW(L"kernel32.dll");
                if (kb)
                    g_get_description = reinterpret_cast<GetThreadDescriptionFn>(
                        reinterpret_cast<void*>(GetProcAddress(kb, "GetThreadDescription")));
            }
            ULONGLONG next_refresh = 0;
            for (;;)
            {
                Sleep(1);
                const ULONGLONG now = GetTickCount64();
                if (now >= next_refresh)
                {
                    refresh_tracked();
                    next_refresh = now + 2000;
                }
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                bool got = false;
                DWORD stack[kStackWords];
                bool have_stack = false;
                if (SuspendThread(g_render_thread) != (DWORD)-1)
                {
                    got = GetThreadContext(g_render_thread, &ctx) != 0;
                    if (got)
                        have_stack = copy_words(reinterpret_cast<const DWORD*>(static_cast<uintptr_t>(ctx.Esp)),
                                                stack, kStackWords);
                    ResumeThread(g_render_thread);
                }
                sample_tracked();
                if (!got)
                    continue;
                HMODULE module = nullptr;
                DWORD rva = 0;
                const void* ip = reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Eip));
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(ip), &module) && module)
                    rva = DWORD(reinterpret_cast<const char*>(ip) - reinterpret_cast<const char*>(module)) & ~15u;
                const bool leaf_system = module && is_system_module(module);
                HMODULE caller = nullptr;
                DWORD caller_rva = 0;
                HMODULE outer = nullptr;
                DWORD outer_rva = 0;
                if (!g_dxvk_module)
                    g_dxvk_module = GetModuleHandleW(L"d3d9_dxvk.dll");
                if (leaf_system && have_stack)
                {
                    for (int w = 0; w < kStackWords; ++w)
                    {
                        HMODULE m = module_of(stack[w]);
                        if (!m || is_system_module(m) || !looks_like_return(stack[w]))
                            continue;
                        caller = m;
                        caller_rva = (stack[w] - DWORD(reinterpret_cast<uintptr_t>(m))) & ~15u;
                        if (m == g_dxvk_module)
                        {
                            for (++w; w < kStackWords; ++w)
                            {
                                HMODULE o = module_of(stack[w]);
                                if (!o || o == g_dxvk_module || is_system_module(o) || !looks_like_return(stack[w]))
                                    continue;
                                outer = o;
                                outer_rva = (stack[w] - DWORD(reinterpret_cast<uintptr_t>(o))) & ~15u;
                                break;
                            }
                        }
                        break;
                    }
                }
                EnterCriticalSection(&g_sample_lock);
                if (leaf_system)
                {
                    ++g_caller_system;
                    if (caller && caller == g_dxvk_module)
                    {
                        ++g_wait_total;
                        int c = 0;
                        for (; c < g_wait_used; ++c)
                            if (g_waits[c].leaf == module && g_waits[c].leaf_rva == rva &&
                                g_waits[c].dxvk_rva == caller_rva && g_waits[c].outer == outer &&
                                g_waits[c].outer_rva == outer_rva)
                                break;
                        if (c < g_wait_used)
                            ++g_waits[c].count;
                        else if (g_wait_used < kWaitSlots)
                            g_waits[g_wait_used++] = { module, rva, caller_rva, outer, outer_rva, 1 };
                    }
                    if (caller)
                    {
                        int c = 0;
                        for (; c < g_caller_used; ++c)
                            if (g_callers[c].leaf == module && g_callers[c].leaf_rva == rva &&
                                g_callers[c].caller == caller && g_callers[c].caller_rva == caller_rva)
                                break;
                        if (c < g_caller_used)
                            ++g_callers[c].count;
                        else if (g_caller_used < kCallerSlots)
                            g_callers[g_caller_used++] = { module, rva, caller, caller_rva, 1 };
                    }
                }
                ++g_sample_total;
                int m = 0;
                for (; m < g_module_used; ++m)
                    if (g_modules[m].module == module)
                        break;
                if (m < g_module_used)
                    ++g_modules[m].count;
                else if (g_module_used < kModuleSlots)
                    g_modules[g_module_used++] = { module, 1 };
                int i = 0;
                for (; i < g_sample_used; ++i)
                    if (g_samples[i].module == module && g_samples[i].rva == rva)
                        break;
                if (i < g_sample_used)
                    ++g_samples[i].count;
                else if (g_sample_used < kSampleSlots)
                    g_samples[g_sample_used++] = { module, rva, 1 };
                else
                    ++g_sample_dropped;
                LeaveCriticalSection(&g_sample_lock);
            }
        }

        // Reads [developer] perf_sampler once, on the first draw (which is
        // on the render thread), and starts the sampler if it is on.
        void sampler_start_if_enabled()
        {
            g_sampler_checked = true;
            wchar_t path[MAX_PATH];
            swprintf_s(path, MAX_PATH, L"%strlvr.ini", exe_dir());
            if (!GetPrivateProfileIntW(L"developer", L"perf_sampler", 0, path))
                return;
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                                 &g_render_thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                 FALSE, 0))
                return;
            HANDLE t = CreateThread(nullptr, 0, sampler_main, nullptr, 0, nullptr);
            if (!t)
                return;
            CloseHandle(t);
            g_sampler_on = true;
        }

        void module_name(HMODULE module, char* out, size_t size)
        {
            wchar_t path[MAX_PATH] = L"?";
            if (module)
                GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            WideCharToMultiByte(CP_UTF8, 0, name, -1, out, int(size), nullptr, nullptr);
            out[size - 1] = 0;
        }

        void sampler_report()
        {
            // Copy under the lock, format outside it.
            std::vector<Sample> samples;
            std::vector<ModuleCount> modules;
            std::vector<CallerSample> callers;
            unsigned total, dropped, system_total, wait_total;
            std::vector<WaitSample> waits;
            EnterCriticalSection(&g_sample_lock);
            samples.assign(g_samples, g_samples + g_sample_used);
            modules.assign(g_modules, g_modules + g_module_used);
            callers.assign(g_callers, g_callers + g_caller_used);
            total = g_sample_total;
            dropped = g_sample_dropped;
            system_total = g_caller_system;
            waits.assign(g_waits, g_waits + g_wait_used);
            wait_total = g_wait_total;
            g_wait_used = 0;
            g_wait_total = 0;
            TrackStats track[kTrackKinds];
            memcpy(track, g_track_stats, sizeof(track));
            memset(g_track_stats, 0, sizeof(g_track_stats));
            g_sample_used = g_module_used = g_caller_used = 0;
            g_sample_total = g_sample_dropped = g_caller_system = 0;
            LeaveCriticalSection(&g_sample_lock);

            for (int k = 0; k < kTrackKinds; ++k)
            {
                TrackStats& s = track[k];
                if (!s.total)
                    continue;
                std::sort(s.modules, s.modules + s.module_used,
                          [](const ModuleCount& a, const ModuleCount& b) { return a.count > b.count; });
                char line[700];
                int n = sprintf_s(line, "perf thread %s: %u samples, busy %.1f%% (ntdll/wait %.1f%%); modules:",
                                  kTrackLabels[k], s.total,
                                  (s.total - s.waiting) * 100.0 / s.total,
                                  s.waiting * 100.0 / s.total);
                for (unsigned i = 0; i < s.module_used && i < 6 && n > 0 && n < 500; ++i)
                {
                    char name[64];
                    module_name(s.modules[i].module, name, sizeof(name));
                    n += sprintf_s(line + n, sizeof(line) - n, " %s %.1f%%;", name,
                                   s.modules[i].count * 100.0 / s.total);
                }
                log("%s", line);
            }

            if (!total)
                return;

            std::sort(modules.begin(), modules.end(),
                      [](const ModuleCount& a, const ModuleCount& b) { return a.count > b.count; });
            char text[1400];
            int len = sprintf_s(text, "perf where modules: %u samples%s;", total,
                                dropped ? " (some dropped)" : "");
            for (size_t i = 0; i < modules.size() && i < 8 && len > 0 && len < 1200; ++i)
            {
                char name[64];
                module_name(modules[i].module, name, sizeof(name));
                len += sprintf_s(text + len, sizeof(text) - len, " %s %.1f%%;", name,
                                 modules[i].count * 100.0 / total);
            }
            log("%s", text);

            std::sort(samples.begin(), samples.end(),
                      [](const Sample& a, const Sample& b) { return a.count > b.count; });
            static char big[8192];
            len = sprintf_s(big, "perf where: %u samples;", total);
            for (size_t i = 0; i < samples.size() && i < 60 && len > 0 && len < 7800; ++i)
            {
                char name[64];
                module_name(samples[i].module, name, sizeof(name));
                len += sprintf_s(big + len, sizeof(big) - len, " %s+0x%X x%u (%.1f%%);", name,
                                 samples[i].rva, samples[i].count,
                                 samples[i].count * 100.0 / total);
            }
            log("%s", big);

            std::sort(callers.begin(), callers.end(),
                      [](const CallerSample& a, const CallerSample& b) { return a.count > b.count; });
            len = sprintf_s(big, "perf where callers: %u system samples;", system_total);
            for (size_t i = 0; i < callers.size() && i < 20 && len > 0 && len < 7800; ++i)
            {
                char leaf[64], caller[64];
                module_name(callers[i].leaf, leaf, sizeof(leaf));
                module_name(callers[i].caller, caller, sizeof(caller));
                len += sprintf_s(big + len, sizeof(big) - len, " %s+0x%X <- %s+0x%X x%u (%.1f%%);",
                                 leaf, callers[i].leaf_rva, caller, callers[i].caller_rva,
                                 callers[i].count, callers[i].count * 100.0 / total);
            }
            log("%s", big);

            std::sort(waits.begin(), waits.end(),
                      [](const WaitSample& a, const WaitSample& b) { return a.count > b.count; });
            len = sprintf_s(big, "perf where dxvk-waits: %u samples;", wait_total);
            for (size_t i = 0; i < waits.size() && i < 25 && len > 0 && len < 7800; ++i)
            {
                char leaf[64], outer[64];
                module_name(waits[i].leaf, leaf, sizeof(leaf));
                module_name(waits[i].outer, outer, sizeof(outer));
                len += sprintf_s(big + len, sizeof(big) - len, " %s+0x%X <- d3d9_dxvk.dll+0x%X <- %s+0x%X x%u (%.1f%%);",
                                 leaf, waits[i].leaf_rva, waits[i].dxvk_rva, outer, waits[i].outer_rva,
                                 waits[i].count, waits[i].count * 100.0 / total);
            }
            log("%s", big);
        }

        LARGE_INTEGER g_frequency{};
        // Bucket times are read with the time-stamp counter (2026-11-22:
        // QueryPerformanceCounter around every draw and eye cost ~0.3 ms a
        // frame at 2800 draws); converted to ms at each report against the
        // QPC clock over the same window.
        double g_total[PerfBucketCount]{};          // TSC ticks until report
        unsigned long long g_started[PerfBucketCount]{};
        unsigned long long g_window_tsc = 0;
        LARGE_INTEGER g_window_qpc{};
        int g_depth[PerfBucketCount]{};
        // Game draw calls by what was running when they were issued
        // (user 2026-11-22: what makes the waterfall view 2500 draws?):
        // inside a BasicDrawable::Draw (effects, sprites, HUD), the terrain
        // hook, the instance hooks (characters, objects), or none of them.
        enum DrawSource { SrcBasic, SrcTerrain, SrcInstance, SrcOther, SrcCount };
        double g_draw_source[SrcCount]{};
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
            g_started[bucket] = __rdtsc();
    }

    void perf_cpu_end(PerfBucket bucket)
    {
        if (g_depth[bucket] <= 0)
            return;
        if (--g_depth[bucket] == 0)
        {
            g_total[bucket] += double(__rdtsc() - g_started[bucket]);
            ++g_calls[bucket];
        }
    }

    void perf_cpu_draw()
    {
        if (g_depth[PerfUiDraw] > 0)
            g_draw_source[SrcBasic] += 1.0;
        else if (g_depth[PerfTerrain] > 0)
            g_draw_source[SrcTerrain] += 1.0;
        else if (g_depth[PerfDrawInstance] > 0 || g_depth[PerfDrawLara] > 0)
            g_draw_source[SrcInstance] += 1.0;
        else
            g_draw_source[SrcOther] += 1.0;
        if (!g_sampler_checked)
            sampler_start_if_enabled();
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
        if (g_sampler_on)
            sampler_report();
        // TSC ticks -> ms over this window (the counter is invariant on
        // every CPU this runs on; QPC gives the window's length).
        {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            const unsigned long long tsc = __rdtsc();
            double ms_per_tick = 0.0;
            if (g_window_tsc && tsc > g_window_tsc && g_window_qpc.QuadPart)
                ms_per_tick = elapsed_ms(g_window_qpc, qpc) /
                              double(tsc - g_window_tsc);
            g_window_tsc = tsc;
            g_window_qpc = qpc;
            for (double& t : g_total)
                t *= ms_per_tick;
        }
        if (!frames)
        {
            for (double& t : g_total)
                t = 0.0;
            return;
        }
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
        log("perf draws by source, per frame: BasicDrawable (effects, "
            "sprites, HUD) %.0f, terrain %.0f, instances %.0f, other %.0f",
            g_draw_source[SrcBasic] / n, g_draw_source[SrcTerrain] / n,
            g_draw_source[SrcInstance] / n, g_draw_source[SrcOther] / n);
        for (double& d : g_draw_source)
            d = 0.0;
        for (int b = 0; b < PerfBucketCount; ++b)
        {
            g_total[b] = 0.0;
            g_calls[b] = 0;
        }
        g_update_ms = g_render_ms = g_present_ms = 0.0;
    }
}
