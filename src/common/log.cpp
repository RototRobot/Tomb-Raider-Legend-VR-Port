#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstdint>
#include <cstring>

namespace trlvr
{
    namespace
    {
        CRITICAL_SECTION g_lock;
        bool g_lock_ready = false;
        FILE* g_file = nullptr;
        wchar_t g_dir[MAX_PATH] = L"";
        LARGE_INTEGER g_freq{}, g_start{};
        const char* volatile g_activity = nullptr;
        LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
        volatile LONG g_crashed = 0;

        // Keep the previous run's file: players relaunch to reproduce a
        // crash, which used to erase the evidence.
        void keep_previous(const wchar_t* name)
        {
            wchar_t path[MAX_PATH]{}, prev[MAX_PATH]{};
            swprintf_s(path, L"%s%s", exe_dir(), name);
            swprintf_s(prev, L"%s%s.prev", exe_dir(), name);
            MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
        }

        // One line from the crash path: never block on the log lock (the
        // faulting thread may hold it).
        void crash_line(const char* fmt, ...)
        {
            if (!g_file)
                return;
            const bool locked = g_lock_ready &&
                                TryEnterCriticalSection(&g_lock) != FALSE;
            fprintf(g_file, "[  CRASH ] ");
            va_list args;
            va_start(args, fmt);
            vfprintf(g_file, fmt, args);
            va_end(args);
            fputc('\n', g_file);
            fflush(g_file);
            if (locked)
                LeaveCriticalSection(&g_lock);
        }

        LONG WINAPI crash_filter(EXCEPTION_POINTERS* info)
        {
            if (InterlockedExchange(&g_crashed, 1) == 0 && info &&
                info->ExceptionRecord)
            {
                const EXCEPTION_RECORD* r = info->ExceptionRecord;
                HMODULE module = nullptr;
                char name[MAX_PATH] = "?";
                GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(r->ExceptionAddress), &module);
                if (module)
                {
                    char full[MAX_PATH]{};
                    if (GetModuleFileNameA(module, full, MAX_PATH))
                    {
                        const char* slash = strrchr(full, '\\');
                        strcpy_s(name, slash ? slash + 1 : full);
                    }
                }
                const uintptr_t offset = module
                    ? reinterpret_cast<uintptr_t>(r->ExceptionAddress) -
                      reinterpret_cast<uintptr_t>(module) : 0;
                const char* activity = g_activity;
                crash_line("unhandled exception 0x%08lX at %p (%s+0x%IX), "
                           "thread %lu, during: %s", r->ExceptionCode,
                           r->ExceptionAddress, name, offset,
                           GetCurrentThreadId(),
                           activity ? activity : "game code");
                if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                    r->NumberParameters >= 2)
                    crash_line("  %s of address %p",
                               r->ExceptionInformation[0] == 0 ? "read"
                               : r->ExceptionInformation[0] == 1 ? "write"
                                                                 : "execute",
                               reinterpret_cast<void*>(
                                   r->ExceptionInformation[1]));

                // Minidump (dbghelp loaded on demand). The dump, not the
                // log, is what found the SiN crash.
                typedef BOOL(WINAPI* PFN_MiniDumpWriteDump)(HANDLE, DWORD,
                    HANDLE, int, void*, void*, void*);
                struct ExceptionParam
                {
                    DWORD thread;
                    EXCEPTION_POINTERS* pointers;
                    BOOL client;
                };
                HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
                auto write_dump = dbghelp
                    ? reinterpret_cast<PFN_MiniDumpWriteDump>(
                          GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                    : nullptr;
                bool written = false;
                if (write_dump)
                {
                    keep_previous(L"trlvr_crash.dmp");
                    wchar_t path[MAX_PATH]{};
                    swprintf_s(path, L"%strlvr_crash.dmp", exe_dir());
                    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
                    if (file != INVALID_HANDLE_VALUE)
                    {
                        ExceptionParam param{ GetCurrentThreadId(), info,
                                              FALSE };
                        // Normal | WithIndirectlyReferencedMemory |
                        // WithThreadInfo: stacks plus what they point at.
                        written = write_dump(GetCurrentProcess(),
                            GetCurrentProcessId(), file, 0x0040 | 0x1000,
                            &param, nullptr, nullptr) != FALSE;
                        CloseHandle(file);
                    }
                }
                crash_line("minidump %s -- please send trlvr.log and "
                           "trlvr_crash.dmp", written ? "written to "
                           "trlvr_crash.dmp" : "could not be written");
            }
            return g_previous_filter ? g_previous_filter(info)
                                     : EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void log_set_activity(const char* what)
    {
        g_activity = what;
    }

    void log_install_crash_handler()
    {
        static bool installed = false;
        if (installed)
            return;
        installed = true;
        g_previous_filter = SetUnhandledExceptionFilter(&crash_filter);
        log("crash handler installed (minidump to trlvr_crash.dmp; the "
            "game's own handler %s)", g_previous_filter ? "still runs after"
                                                        : "was not set");
    }

    const wchar_t* exe_dir()
    {
        if (g_dir[0])
            return g_dir;

        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
            return L"";

        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash)
            return L"";
        slash[1] = L'\0';
        wcscpy_s(g_dir, path);
        return g_dir;
    }

    void log_open()
    {
        if (!g_lock_ready)
        {
            InitializeCriticalSection(&g_lock);
            g_lock_ready = true;
        }
        if (g_file)
            return;

        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);

        keep_previous(L"trlvr.log");
        wchar_t path[MAX_PATH]{};
        swprintf_s(path, L"%strlvr.log", exe_dir());

        // _wfsopen, not _wfopen_s: the secure variants open exclusively, which
        // makes the log unreadable while the game is running -- exactly when
        // you want to read it. _SH_DENYWR lets anything open it for reading.
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
        if (!g_file)
            return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        fprintf(g_file, "TRL VR proxy log  %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_file);
    }

    void log_close()
    {
        if (!g_lock_ready)
            return;
        EnterCriticalSection(&g_lock);
        if (g_file)
        {
            fclose(g_file);
            g_file = nullptr;
        }
        LeaveCriticalSection(&g_lock);
    }

    void log(const char* fmt, ...)
    {
        if (!g_file || !g_lock_ready)
            return;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double t = g_freq.QuadPart
            ? double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart)
            : 0.0;

        EnterCriticalSection(&g_lock);
        fprintf(g_file, "[%8.3f] ", t);
        va_list args;
        va_start(args, fmt);
        vfprintf(g_file, fmt, args);
        va_end(args);
        fputc('\n', g_file);
        fflush(g_file);            // a crash must not cost us the last line
        LeaveCriticalSection(&g_lock);
    }
}
