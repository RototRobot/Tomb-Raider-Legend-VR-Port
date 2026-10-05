// trlvr_launcher -- starts Tomb Raider: Legend with a 4 GB address space.
//
// Whether a 32-bit process gets 2 GB or 4 GB is decided by one bit in the PE
// header, and the loader reads it from the file at start-up. Nothing inside the
// process can change it afterwards, so it has to be set before launch.
//
// VR needs the room: the headset asks for 2444x2392 per eye, which is about
// 23 MB per render target before depth, and stereo means several of them. A
// 2 GB space runs out during a level load rather than at a convenient moment.
//
// The original trl.exe is never modified. A patched copy is written beside it,
// so Steam's "verify integrity of game files" has nothing to undo.
//
// Usage, from Steam's launch options:
//
//   "...\trlvr_launcher.exe" %command%
//
// Steam expands %command% to the game's own path plus its arguments, so argv[1]
// arrives as the exe to launch. Run without arguments it looks for trl.exe
// beside itself instead.

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>

namespace
{
    const wchar_t* kPatchedName = L"trl_vr.exe";

    FILE* g_log = nullptr;

    void logf(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        if (g_log)
        {
            vfprintf(g_log, fmt, args);
            fputc('\n', g_log);
            fflush(g_log);
        }
        va_end(args);
    }

    void fail_box(const wchar_t* text)
    {
        MessageBoxW(nullptr, text, L"Tomb Raider: Legend VR", MB_ICONERROR | MB_OK);
    }

    // The PE checksum Windows stores in the optional header. It is not verified
    // for ordinary user-mode executables, but leaving a stale one behind in a
    // file we rewrote is the sort of small wrongness that costs an hour later.
    uint32_t pe_checksum(const unsigned char* data, size_t size, size_t checksum_off)
    {
        uint64_t sum = 0;
        for (size_t i = 0; i + 1 < size; i += 2)
        {
            if (i == checksum_off)          // the field reads as zero
                continue;
            sum += data[i] | (uint32_t(data[i + 1]) << 8);
            if (sum > 0xFFFFFFFFull)
                sum = (sum & 0xFFFFFFFFull) + (sum >> 32);
        }
        if (size & 1)
            sum += data[size - 1];

        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);

        return uint32_t(sum) + uint32_t(size);
    }

    bool read_file(const wchar_t* path, unsigned char** out, size_t* out_size)
    {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (200 << 20))
        {
            CloseHandle(h);
            return false;
        }

        auto* buf = (unsigned char*)malloc(size_t(size.QuadPart));
        if (!buf)
        {
            CloseHandle(h);
            return false;
        }

        DWORD got = 0;
        const bool ok = ReadFile(h, buf, DWORD(size.QuadPart), &got, nullptr)
                     && got == size.QuadPart;
        CloseHandle(h);

        if (!ok)
        {
            free(buf);
            return false;
        }
        *out = buf;
        *out_size = size_t(size.QuadPart);
        return true;
    }

    // Returns false and leaves a message in `why` if the file is not a 32-bit PE.
    bool patch_laa(unsigned char* d, size_t size, const char** why, bool* already)
    {
        *already = false;
        if (size < 0x40 || d[0] != 'M' || d[1] != 'Z')
        {
            *why = "not an MZ executable";
            return false;
        }

        const uint32_t nt = *(const uint32_t*)(d + 0x3c);
        if (nt + 24 > size || memcmp(d + nt, "PE\0\0", 4) != 0)
        {
            *why = "no PE header";
            return false;
        }

        const uint16_t machine = *(const uint16_t*)(d + nt + 4);
        if (machine != 0x014c)
        {
            *why = "not a 32-bit x86 image (LAA would be pointless)";
            return false;
        }

        uint16_t* chars = (uint16_t*)(d + nt + 22);
        if (*chars & 0x0020)
        {
            *already = true;
            return true;
        }
        *chars |= 0x0020;                    // IMAGE_FILE_LARGE_ADDRESS_AWARE

        const uint16_t optsz = *(const uint16_t*)(d + nt + 20);
        if (optsz >= 68)
        {
            const size_t csum_off = nt + 24 + 64;
            *(uint32_t*)(d + csum_off) = 0;
            *(uint32_t*)(d + csum_off) = pe_checksum(d, size, csum_off);
        }
        return true;
    }

    bool write_file(const wchar_t* path, const unsigned char* d, size_t size)
    {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;

        DWORD wrote = 0;
        const bool ok = WriteFile(h, d, DWORD(size), &wrote, nullptr) && wrote == size;
        CloseHandle(h);
        return ok;
    }

    bool file_time(const wchar_t* path, FILETIME* ft, LARGE_INTEGER* size)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a))
            return false;
        *ft = a.ftLastWriteTime;
        size->HighPart = a.nFileSizeHigh;
        size->LowPart = a.nFileSizeLow;
        return true;
    }

    // Everything after the launcher's own path on the command line, which is
    // what Steam appended for the game.
    const wchar_t* trailing_args(int consumed)
    {
        const wchar_t* cmd = GetCommandLineW();
        int seen = 0;
        bool quoted = false;

        while (*cmd)
        {
            if (*cmd == L'"')
                quoted = !quoted;
            else if (*cmd == L' ' && !quoted)
            {
                while (*cmd == L' ')
                    ++cmd;
                if (++seen == consumed)
                    return cmd;
                continue;
            }
            ++cmd;
        }
        return L"";
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    wchar_t dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\'))
        slash[1] = L'\0';

    wchar_t logpath[MAX_PATH]{};
    swprintf_s(logpath, L"%strlvr_launcher.log", dir);
    // _wfsopen, not _wfopen_s: the secure variants open exclusively, so the
    // log could not be read while the launcher was still waiting on the game --
    // which is exactly when you want to read it.
    g_log = _wfsopen(logpath, L"w", _SH_DENYWR);

    // Steam expands %command% to the game exe plus its arguments, so argv[1] is
    // the thing to launch. Without it, fall back to trl.exe beside us.
    wchar_t source[MAX_PATH]{};
    int consumed = 1;
    if (argc > 1 && GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES)
    {
        wcscpy_s(source, argv[1]);
        consumed = 2;
        logf("game exe from the command line: %S", source);
    }
    else
    {
        swprintf_s(source, L"%strl.exe", dir);
        logf("game exe assumed beside the launcher: %S", source);
    }

    if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES)
    {
        logf("ERROR: cannot find the game executable");
        fail_box(L"Could not find trl.exe.\n\n"
                 L"Put trlvr_launcher.exe in the game folder, or set Steam's\n"
                 L"launch options to:  \"...\\trlvr_launcher.exe\" %command%");
        return 1;
    }

    // The patched copy lives beside the original, which is never touched.
    wchar_t target[MAX_PATH]{};
    wcscpy_s(target, source);
    if (wchar_t* slash = wcsrchr(target, L'\\'))
        swprintf_s(slash + 1, MAX_PATH - (slash + 1 - target), L"%s", kPatchedName);

    // Rebuild whenever the original has moved on -- a Steam update would
    // otherwise leave a stale copy running for ever.
    bool rebuild = true;
    FILETIME ts{}, tt{};
    LARGE_INTEGER ss{}, st{};
    if (file_time(source, &ts, &ss) && file_time(target, &tt, &st))
    {
        rebuild = !(CompareFileTime(&ts, &tt) == 0 && ss.QuadPart == st.QuadPart);
        if (!rebuild)
            logf("patched copy is current");
    }

    if (rebuild)
    {
        unsigned char* data = nullptr;
        size_t size = 0;
        if (!read_file(source, &data, &size))
        {
            logf("ERROR: could not read %S", source);
            fail_box(L"Could not read the game executable.");
            return 1;
        }

        const char* why = "";
        bool already = false;
        if (!patch_laa(data, size, &why, &already))
        {
            logf("ERROR: %s", why);
            free(data);
            fail_box(L"The game executable is not in a form this launcher understands.");
            return 1;
        }

        logf(already ? "the game is already large-address-aware; copying as-is"
                     : "set IMAGE_FILE_LARGE_ADDRESS_AWARE, rewrote the PE checksum");

        if (!write_file(target, data, size))
        {
            logf("ERROR: could not write %S", target);
            free(data);
            fail_box(L"Could not write the patched copy next to the game.\n"
                     L"Check the folder is writable.");
            return 1;
        }
        free(data);

        // Match the timestamp so the staleness check above works next time.
        HANDLE h = CreateFileW(target, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            SetFileTime(h, nullptr, nullptr, &ts);
            CloseHandle(h);
        }
        logf("wrote %S", target);
    }

    // Launch from the game's own directory, or it will not find its data.
    wchar_t gamedir[MAX_PATH]{};
    wcscpy_s(gamedir, source);
    if (wchar_t* slash = wcsrchr(gamedir, L'\\'))
        *slash = L'\0';

    wchar_t cmdline[4096]{};
    swprintf_s(cmdline, L"\"%s\" %s", target, trailing_args(consumed));
    logf("launching: %S", cmdline);

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                        0, nullptr, gamedir, &si, &pi))
    {
        logf("ERROR: CreateProcess failed (%lu)", GetLastError());
        fail_box(L"Could not start the patched game executable.");
        return 1;
    }

    logf("started, pid %lu", pi.dwProcessId);

    // Stay alive until the game exits so Steam keeps counting playtime and the
    // overlay stays attached.
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    logf("game exited with code %lu", code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (argv)
        LocalFree(argv);
    if (g_log)
        fclose(g_log);
    return int(code);
}
