#include "backend.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cstdio>

namespace trlvr
{
    namespace
    {
        HMODULE g_backend = nullptr;
        bool g_tried = false;
        wchar_t g_path[MAX_PATH] = L"";

        bool exists(const wchar_t* path)
        {
            const DWORD a = GetFileAttributesW(path);
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }

        void system_d3d9(wchar_t* out)
        {
            wchar_t dir[MAX_PATH]{};
            GetSystemDirectoryW(dir, MAX_PATH);
            swprintf_s(out, MAX_PATH, L"%s\\d3d9.dll", dir);
        }

        // Resolves the configured backend to a concrete path.
        void choose(wchar_t* out)
        {
            const Config& c = config();

            wchar_t dxvk[MAX_PATH]{};
            swprintf_s(dxvk, L"%sd3d9_dxvk.dll", exe_dir());

            if (_wcsicmp(c.backend, L"system") == 0)
            {
                system_d3d9(out);
                return;
            }
            if (_wcsicmp(c.backend, L"dxvk") == 0)
            {
                if (exists(dxvk))
                {
                    wcscpy_s(out, MAX_PATH, dxvk);
                    return;
                }
                log("backend: 'dxvk' requested but d3d9_dxvk.dll is not beside the game; "
                    "falling back to the system d3d9");
                system_d3d9(out);
                return;
            }
            if (_wcsicmp(c.backend, L"auto") == 0)
            {
                if (exists(dxvk))
                {
                    wcscpy_s(out, MAX_PATH, dxvk);
                    return;
                }
                system_d3d9(out);
                return;
            }

            // Anything else is treated as an explicit path.
            if (exists(c.backend))
            {
                wcscpy_s(out, MAX_PATH, c.backend);
                return;
            }
            log("backend: configured path does not exist, using the system d3d9");
            system_d3d9(out);
        }
    }

    HMODULE backend()
    {
        if (g_tried)
            return g_backend;
        g_tried = true;

        choose(g_path);

        // LOAD_WITH_ALTERED_SEARCH_PATH so a backend beside the game resolves
        // its own dependencies from there rather than from our directory.
        g_backend = LoadLibraryExW(g_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_backend)
        {
            log("backend: FAILED to load %S (error %lu)", g_path, GetLastError());
            return nullptr;
        }
        log("backend: %S", g_path);
        return g_backend;
    }

    const wchar_t* backend_path()
    {
        return g_path;
    }

    FARPROC backend_proc(const char* name)
    {
        HMODULE m = backend();
        return m ? GetProcAddress(m, name) : nullptr;
    }
}
