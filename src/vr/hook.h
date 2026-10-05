#pragma once
// A minimal 5-byte inline hook.
//
// Enough for what this mod needs and nothing more: no library, no dependency,
// and the whole mechanism visible in one file. trl.exe has no ASLR, so every
// target address is fixed and known ahead of time.
//
// Every install checks the bytes it is about to overwrite against a signature
// first. Our function addresses come from remapping a PDB that belongs to a
// different build (see FINDINGS.md), so they are good but not authoritative --
// patching an address that turned out to be something else would corrupt the
// game in a way that looks like anything but a bad hook.

#include <windows.h>

namespace trlvr
{
    // `expect` must match the `len` bytes at `target`, and `len` must land on an
    // instruction boundary and be at least 5. Returns false and patches nothing
    // if the signature does not match.
    //
    // On success `trampoline` receives a callable that runs the original
    // instructions and jumps back, so the detour can call through.
    bool hook_install(void* target, void* detour, void** trampoline,
                      const unsigned char* expect, size_t len,
                      const char* name);

    // The GOG release's crash guard, ported to every copy: in the terrain
    // callback's node matrix builder (0x005C12D0, called from
    // TerrainCallback), a node offset index above 0x100000 skips the node
    // instead of reading far past the 0x0113F880 offset table. Already
    // present in GOG's trl.exe; applied to Steam's at run time.
    void hook_apply_gog_crash_guard();

    // INI overrides of the game's graphics settings ([vr] depth_of_field),
    // installed from DllMain, before the game loads them.
    void hook_apply_settings_overrides();
}
