#pragma once
// Logging to trlvr.log, next to the game executable.
//
// The log is the only way to see what happened inside a 2006 game running
// under a headset, so it records what the mod actually did, not what it was
// configured to do.

namespace trlvr
{
    // Directory containing the running executable, with a trailing backslash.
    const wchar_t* exe_dir();

    void log_open();
    void log_close();
    void log(const char* fmt, ...);

    // Lessons from SiN VR 1.0.1/1.0.2 (2026-10-04):
    // - name every call into SteamVR/DXVK while it runs, so a crash report
    //   says which one it was (a string literal; null = none);
    // - on an unhandled crash, log the fault (code, module + offset, that
    //   call) and write trlvr_crash.dmp, keeping the previous run's log and
    //   dump as .prev. Installed at the first frame so it sits in front of
    //   the game's own crash reporter, which still runs afterwards.
    void log_set_activity(const char* what);
    void log_install_crash_handler();
}
