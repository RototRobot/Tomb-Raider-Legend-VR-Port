#include "overlay.h"
#include "window.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../vr/vr_session.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <cstdio>
#include <windows.h>
#include <xinput.h>     // the XINPUT_GAMEPAD_* bits only; nothing linked

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace trlvr
{
    namespace
    {
        const UINT kToggleKey = VK_INSERT;

        IDirect3DDevice9* g_device = nullptr;   // referenced by the backend
        HWND g_hwnd = nullptr;
        WNDPROC g_game_proc = nullptr;
        HHOOK g_hook = nullptr;
        bool g_unicode = false;
        bool g_ready = false;
        bool g_failed = false;
        bool g_open = false;

        // --- the panel in the headset (overlay_vr_texture) ----------------
        //
        // While VR is up the window is laid out in a square of this many
        // pixels (io.DisplaySize), filling it, and rendered into a texture
        // of that size, which vr_submit.cpp puts into both eyes as a panel
        // in the room. The same draw data then goes to the desktop mirror
        // at window pixels, so the mouse still lines up with it.
        const unsigned kPanelSize = 768;
        IDirect3DTexture9* g_panel = nullptr;
        IDirect3DSurface9* g_panel_surface = nullptr;
        bool g_panel_failed = false;
        bool g_panel_logged = false;
        unsigned g_open_count = 0;

        // This Present's ImGui frame has been built (for the headset), so
        // overlay_frame only draws it to the mirror; the window's client
        // size at the time, which the mirror scales from.
        bool g_frame_built = false;
        ImVec2 g_client_size(0.0f, 0.0f);
        // Set at the open edge: the frame after gives the controllers a
        // visible, focused first item to start from.
        bool g_just_opened = false;

        // The controllers (vr_input.cpp, overlay_set_pad) as ImGui sees them
        // this frame; g_pad_in is the latest given, used once.
        OverlayPad g_pad_in{};
        bool g_pad_fresh = false;
        OverlayPad g_pad{};
        // Buttons held when the overlay opened: only a fresh press counts,
        // so the press that was in flight (the held menu button that opened
        // it) neither activates the first item nor closes it again.
        WORD g_held_at_open = 0;
        WORD g_prev_buttons = 0;
        // The left stick as a D-pad (hysteresis, so a stick resting near the
        // threshold does not stutter): ImGui moves its focus with the D-pad
        // only; its left stick scrolls.
        WORD g_stick_dpad = 0;

        LRESULT call_game(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            return g_unicode ? CallWindowProcW(g_game_proc, hwnd, msg, wp, lp)
                             : CallWindowProcA(g_game_proc, hwnd, msg, wp, lp);
        }

        // In front of the game's window procedure. Closed, it only watches
        // for the toggle key. Open, the mouse and keyboard stop here: the
        // game recentres the cursor on every mouse move it sees, and would
        // act on every key typed into a field.
        //
        // Two places, because the game has two: its message loop reads the
        // queue itself (PeekMessage) and may act before the window
        // procedure is ever called, so queued input is taken in a
        // WH_GETMESSAGE hook, which sees each message first and can blank
        // it. The window procedure is still replaced for what is sent, not
        // queued, and as the fallback if the hook cannot be installed.
        bool is_mouse(UINT msg)
        {
            return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        }

        bool is_key(UINT msg)
        {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                   msg == WM_INPUT;
        }

        void set_open(bool open)
        {
            g_open = open;
            if (g_ready)
                ImGui::GetIO().MouseDrawCursor = g_open;
            window_block_game_mouse(g_open);
            log("overlay: %s", g_open ? "opened" : "closed");
        }

        // True if the message was the overlay's and the game must not see it.
        bool take_input(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_KEYDOWN && wp == kToggleKey)
            {
                if (!(lp & 0x40000000))
                    set_open(!g_open);
                return true;
            }
            if (!g_open || !g_ready || (!is_mouse(msg) && !is_key(msg)))
                return false;
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            // Key releases go on to the game as well: a key held when the
            // overlay opened (the controllers' keys, which vr_input lets go
            // of as it opens, or a real one) would otherwise stay down in
            // the game until pressed again. A release never acts.
            // Fire is the left mouse button: its release goes on too.
            if (msg == WM_KEYUP || msg == WM_SYSKEYUP ||
                msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
                msg == WM_MBUTTONUP || msg == WM_XBUTTONUP)
                return false;
            return true;
        }

        LRESULT CALLBACK overlay_get_message(int code, WPARAM wp, LPARAM lp)
        {
            MSG* msg = reinterpret_cast<MSG*>(lp);
            if (code >= 0 && wp == PM_REMOVE && msg && g_hwnd &&
                (msg->hwnd == g_hwnd || IsChild(g_hwnd, msg->hwnd)))
            {
                // Typed characters come from TranslateMessage, which the
                // game's loop will not reach for a blanked key.
                if (g_open && g_ready && msg->message == WM_KEYDOWN &&
                    msg->wParam != kToggleKey)
                    TranslateMessage(msg);
                if (take_input(msg->hwnd, msg->message, msg->wParam,
                               msg->lParam))
                    msg->message = WM_NULL;
            }
            return CallNextHookEx(g_hook, code, wp, lp);
        }

        LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (take_input(hwnd, msg, wp, lp))
                return 0;
            if (g_open && g_ready)
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            return call_game(hwnd, msg, wp, lp);
        }

        bool init(IDirect3DSwapChain9* chain)
        {
            if (g_ready)
                return true;
            if (g_failed || !chain)
                return false;

            IDirect3DDevice9* device = nullptr;
            if (FAILED(chain->GetDevice(&device)) || !device)
                return false;

            HWND hwnd = window_handle();
            if (!hwnd)
            {
                D3DDEVICE_CREATION_PARAMETERS params{};
                if (SUCCEEDED(device->GetCreationParameters(&params)))
                    hwnd = params.hFocusWindow;
            }
            if (!hwnd || !IsWindow(hwnd))
            {
                device->Release();
                return false;       // no window yet; try again next frame
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            // Window positions are not kept: another imgui.ini beside the
            // game would be shared with any other ImGui tool loaded there.
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;

            // Something more legible than the built-in bitmap font, if
            // Windows has it where it always has.
            wchar_t windir[MAX_PATH]{};
            char font[MAX_PATH * 3]{};
            if (GetWindowsDirectoryW(windir, MAX_PATH))
            {
                wchar_t wide[MAX_PATH]{};
                swprintf_s(wide, L"%s\\Fonts\\segoeui.ttf", windir);
                if (GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES)
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, font,
                                        (int)sizeof(font), nullptr, nullptr);
            }
            if (!font[0] || !io.Fonts->AddFontFromFileTTF(font, 18.0f))
                io.Fonts->AddFontDefault();
            ImGui::StyleColorsDark();

            if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX9_Init(device))
            {
                log("overlay: Dear ImGui would not start; no settings overlay");
                ImGui::DestroyContext();
                device->Release();
                g_failed = true;
                return false;
            }

            // The VR controllers navigate it (feed_gamepad). The Win32
            // backend's own XInput polling is compiled out (build.bat
            // IMGUI_IMPL_WIN32_DISABLE_GAMEPAD): it would read only a real
            // pad, and fight this feed over the same keys.
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

            g_unicode = IsWindowUnicode(hwnd) != FALSE;
            g_game_proc = reinterpret_cast<WNDPROC>(g_unicode
                ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                    reinterpret_cast<LONG_PTR>(&overlay_proc))
                : SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                    reinterpret_cast<LONG_PTR>(&overlay_proc)));
            if (!g_game_proc)
            {
                log("overlay: could not get in front of the game's window "
                    "procedure; no settings overlay");
                ImGui_ImplDX9_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                device->Release();
                g_failed = true;
                return false;
            }

            g_device = device;      // the backend took its own reference
            device->Release();
            g_hwnd = hwnd;
            g_hook = SetWindowsHookExW(WH_GETMESSAGE, &overlay_get_message,
                nullptr, GetWindowThreadProcessId(hwnd, nullptr));
            if (!g_hook)
                log("overlay: no message hook (error %lu); input is taken at "
                    "the window procedure only", GetLastError());
            g_ready = true;
            log("overlay: settings overlay ready -- Insert opens it on the "
                "desktop window; in the headset, hold the menu button %.1f s",
                (double)config().overlay_vr_open_hold);
            return true;
        }

        void release_panel()
        {
            if (g_panel_surface)
                g_panel_surface->Release();
            if (g_panel)
                g_panel->Release();
            g_panel_surface = nullptr;
            g_panel = nullptr;
        }

        bool ensure_panel()
        {
            if (g_panel_surface)
                return true;
            if (g_panel_failed || !g_device)
                return false;
            const HRESULT hr = g_device->CreateTexture(kPanelSize, kPanelSize,
                1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                &g_panel, nullptr);
            if (FAILED(hr) || !g_panel ||
                FAILED(g_panel->GetSurfaceLevel(0, &g_panel_surface)))
            {
                log("overlay: could not create the %ux%u VR panel target "
                    "(0x%08lX); the settings overlay stays on the desktop "
                    "only", kPanelSize, kPanelSize, (unsigned long)hr);
                g_panel_failed = true;
                release_panel();
                return false;
            }
            return true;
        }

        // The open and close edges, seen once a frame by whichever of
        // overlay_vr_texture / overlay_frame runs first. set_open() may run
        // from the window procedure at any time; this is where the rest of
        // the overlay hears of it.
        void sync_open()
        {
            static bool was_open = false;
            if (g_open == was_open)
                return;
            was_open = g_open;
            if (g_open)
            {
                g_just_opened = true;
                ++g_open_count;             // the panel goes in front again
                g_held_at_open = 0xFFFF;    // everything held until let go
                g_prev_buttons = 0;
                g_stick_dpad = 0;
            }
            else
            {
                // Closed with the window's own close button too: give the
                // mouse back.
                ImGui::GetIO().MouseDrawCursor = false;
                window_block_game_mouse(false);
                g_frame_built = false;
            }
        }

        // Once a frame while open: the controllers' pad, with presses that
        // began before the overlay opened masked until released. The menu
        // (Start) button closes it. False if it just closed.
        bool poll_pad()
        {
            OverlayPad pad{};
            if (g_pad_fresh)
                pad = g_pad_in;
            g_pad_fresh = false;
            g_held_at_open &= pad.buttons;
            pad.buttons &= ~g_held_at_open;
            const WORD pressed = pad.buttons & ~g_prev_buttons;
            g_prev_buttons = pad.buttons;
            if (pressed & XINPUT_GAMEPAD_START)
            {
                log("overlay: menu button pressed -- closing");
                set_open(false);
                sync_open();
                return false;
            }

            // The left stick as a D-pad: in at 0.5, out at 0.3.
            const struct { WORD bit; float v; } dirs[] = {
                { XINPUT_GAMEPAD_DPAD_LEFT, -pad.lx },
                { XINPUT_GAMEPAD_DPAD_RIGHT, pad.lx },
                { XINPUT_GAMEPAD_DPAD_DOWN, -pad.ly },
                { XINPUT_GAMEPAD_DPAD_UP, pad.ly },
            };
            for (const auto& d : dirs)
            {
                if (d.v > 0.5f)
                    g_stick_dpad |= d.bit;
                else if (d.v < 0.3f)
                    g_stick_dpad &= ~d.bit;
            }
            g_pad = pad;
            return true;
        }

        // g_pad into ImGui. ImGui 1.91 moves its focus with the D-pad (and
        // tweaks an active slider with it) and scrolls with its left stick;
        // the Quest's controllers have no D-pad, so the left stick feeds the
        // D-pad keys (poll_pad) and the right stick ImGui's left-stick keys
        // (scrolling), as well as its own.
        void feed_gamepad(ImGuiIO& io)
        {
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            const WORD buttons = (WORD)(g_pad.buttons | g_stick_dpad);
            const struct { ImGuiKey key; WORD bit; } map[] = {
                { ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A },
                { ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B },
                { ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X },
                { ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y },
                { ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER },
                { ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT },
                { ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT },
                { ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP },
                { ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN },
                { ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK },
            };
            for (const auto& m : map)
                io.AddKeyEvent(m.key, (buttons & m.bit) != 0);

            // Past XInput's own right-stick dead zone, 0..1 (as the Win32
            // backend does).
            const auto analog = [&io](ImGuiKey key, float v) {
                const float dead = (float)XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE /
                                   32767.0f;
                float n = (v - dead) / (1.0f - dead);
                n = n < 0.0f ? 0.0f : n > 1.0f ? 1.0f : n;
                io.AddKeyAnalogEvent(key, n > 0.1f, n);
            };
            const float rx = g_pad.rx, ry = g_pad.ry;
            analog(ImGuiKey_GamepadLStickLeft, -rx);
            analog(ImGuiKey_GamepadLStickRight, rx);
            analog(ImGuiKey_GamepadLStickUp, ry);
            analog(ImGuiKey_GamepadLStickDown, -ry);
            analog(ImGuiKey_GamepadRStickLeft, -rx);
            analog(ImGuiKey_GamepadRStickRight, rx);
            analog(ImGuiKey_GamepadRStickUp, ry);
            analog(ImGuiKey_GamepadRStickDown, -ry);
        }

        // --- widgets: each edits the live value and saves it ---------------

        void save_float(const wchar_t* section, const wchar_t* key, float v)
        {
            wchar_t text[32]{};
            swprintf_s(text, L"%.4g", v);
            config_save_setting(section, key, text);
        }

        void help(const char* text)
        {
            if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s", text);
        }

        // Saved when the drag ends, not on every frame of it.
        bool slider(const char* label, float* value, float low, float high,
                    const char* format, const wchar_t* section,
                    const wchar_t* key, const char* tip)
        {
            const bool changed = ImGui::SliderFloat(label, value, low, high,
                format, ImGuiSliderFlags_AlwaysClamp);
            if (key && ImGui::IsItemDeactivatedAfterEdit())
                save_float(section, key, *value);
            help(tip);
            return changed;
        }

        bool toggle(const char* label, bool* value, const wchar_t* section,
                    const wchar_t* key, const char* tip)
        {
            const bool changed = ImGui::Checkbox(label, value);
            if (changed && key)
                config_save_setting(section, key, *value ? L"1" : L"0");
            help(tip);
            return changed;
        }

        void scale_section(Config& c)
        {
            slider("World scale", &c.world_scale, 100.0f, 600.0f, "%.0f",
                   L"vr", L"world_scale",
                   "Game units per metre. Lara is about 510 units tall, so "
                   "291 makes her 1.75 m. Higher makes the world smaller.");
            slider("Head movement scale", &c.head_position_scale, 0.0f, 3.0f,
                   "%.2f", L"vr", L"head_position_scale",
                   "How far the view moves for your head's movement. 1 is "
                   "one to one.");
            slider("Head rotation scale", &c.head_rotation_scale, 0.5f, 2.0f,
                   "%.2f", L"vr", L"head_rotation_scale",
                   "How far the view turns for your head's turn. Anything "
                   "but 1 is uncomfortable for most people.");
            slider("First-person eye height", &c.first_person_eye_height,
                   -100.0f, 180.0f, "%.0f units", L"vr",
                   L"first_person_eye_height",
                   "Eye height above Lara's neck joint, about 3 units per "
                   "cm. Lower it if the view sits too high on her body.");
            slider("Roomscale lean deadzone", &c.roomscale_deadzone, 0.02f,
                   0.6f, "%.2f m", L"vr", L"roomscale_deadzone",
                   "First person: leaning this far moves only your head; "
                   "beyond it Lara walks to you.");
            slider("Menu and HUD scale", &c.ui_scale, 0.1f, 2.0f, "%.2f",
                   L"vr", L"ui_scale", "Size of menus and the HUD in the "
                   "headset.");
        }

        void camera_section(Config& c)
        {
            slider("View switch glide", &c.view_transition_seconds, 0.0f, 2.0f,
                   "%.2f s", L"vr", L"view_transition_seconds",
                   "How long a view switch takes to glide from one view to "
                   "the next. 0 cuts instantly.");
            ImGui::SeparatorText("Shoulder camera");
            slider("Distance behind", &c.shoulder_distance, 0.3f, 6.0f,
                   "%.2f m", L"vr", L"shoulder_distance", nullptr);
            slider("Height", &c.shoulder_height, -1.0f, 2.0f, "%.2f m",
                   L"vr", L"shoulder_height", nullptr);
            slider("Side (right)", &c.shoulder_side, -1.5f, 1.5f, "%.2f m",
                   L"vr", L"shoulder_side", nullptr);
            ImGui::SeparatorText("Board camera");
            slider("Lara's height", &c.board_lara_height_cm, 3.0f, 100.0f,
                   "%.0f cm", L"vr", L"board_lara_height_cm",
                   "How tall Lara appears on the tabletop.");
            slider("Seat distance", &c.board_distance, 0.0f, 3.0f, "%.2f m",
                   L"vr", L"board_distance", "How far back you sit from her.");
            slider("Seat height", &c.board_height, 0.0f, 3.0f, "%.2f m",
                   L"vr", L"board_height", "How far above her you sit.");
        }

        // Soft-part presets. High is the tuning the user settled on
        // (2026-10-14); the others step evenly either side of it.
        struct SoftPreset
        {
            const char* name;
            bool on;
            float angle, sway_hz, damping, stretch, ripple;
        };
        const SoftPreset kSoftPresets[] = {
            { "Off",    false, 21.8f, 2.6f, 0.47f, 0.50f, 0.40f },
            { "Low",    true,   7.0f, 3.4f, 0.60f, 0.20f, 0.20f },
            { "Medium", true,  14.0f, 3.0f, 0.53f, 0.35f, 0.30f },
            { "High",   true,  21.8f, 2.6f, 0.47f, 0.50f, 0.40f },
            { "Max",    true,  30.0f, 2.2f, 0.40f, 0.70f, 0.60f },
        };

        void soft_presets(Config& c)
        {
            ImGui::TextUnformatted("Preset");
            for (const SoftPreset& preset : kSoftPresets)
            {
                ImGui::SameLine();
                if (!ImGui::Button(preset.name))
                    continue;
                c.body_soft = preset.on;
                config_save_setting(L"vr", L"body_soft",
                                    preset.on ? L"1" : L"0");
                if (!preset.on)
                    continue;       // Off keeps the tuning for next time
                c.body_soft_angle = preset.angle;
                c.body_soft_sway_hz = preset.sway_hz;
                c.body_soft_damping = preset.damping;
                c.body_soft_stretch = preset.stretch;
                c.body_soft_ripple = preset.ripple;
                save_float(L"vr", L"body_soft_angle", preset.angle);
                save_float(L"vr", L"body_soft_sway_hz", preset.sway_hz);
                save_float(L"vr", L"body_soft_damping", preset.damping);
                save_float(L"vr", L"body_soft_stretch", preset.stretch);
                save_float(L"vr", L"body_soft_ripple", preset.ripple);
            }
            help("Sets the soft parts' limit, sideways looseness and "
                 "damping together. The sliders below still fine-tune.");
        }

        void body_section(Config& c)
        {
            soft_presets(c);
            ImGui::Spacing();
            toggle("Chest spring", &c.body_spring, L"vr", L"body_spring",
                   "First-person full body: her chest sways a little on a "
                   "spring.");
            slider("Chest spring limit", &c.body_spring_angle, 0.0f, 15.0f,
                   "%.1f deg", L"vr", L"body_spring_angle", nullptr);
            toggle("Soft parts", &c.body_soft, L"vr", L"body_soft",
                   "Her soft parts bounce, in first and third person; in first" 
                   "person they also give under your hands.");
            slider("Soft parts limit", &c.body_soft_angle, 0.0f, 30.0f,
                   "%.1f deg", L"vr", L"body_soft_angle", nullptr);
            slider("Squash and stretch", &c.body_soft_stretch, 0.0f, 1.0f,
                   "%.2f", L"vr", L"body_soft_stretch",
                   "Shape change as well as swing: longer and narrower "
                   "pulled out, shorter and wider pushed in, same volume.");
            slider("Ripple", &c.body_soft_ripple, 0.0f, 1.0f, "%.2f", L"vr",
                   L"body_soft_ripple",
                   "A lighter second motion on top of the swing: the outer "
                   "part catching up and overrunning.");
            ImGui::SeparatorText("Third person");
            slider("Sideways looseness", &c.body_soft_sway_hz, 0.8f, 4.0f,
                   "%.1f Hz", L"vr", L"body_soft_sway_hz",
                   "Side-to-side spring rate. Lower swings wider and slower "
                   "and is carried by her stride; 3 is the old, stiff value.");
            slider("Damping", &c.body_soft_damping, 0.03f, 0.6f, "%.2f",
                   L"vr", L"body_soft_damping",
                   "How quickly a swing dies away. Lower rings longer; 0.22 "
                   "is the old value.");

            ImGui::SeparatorText("Push (god mode)");
            toggle("Whole-body push", &c.body_push, L"vr", L"body_push",
                   "A hand leaning on her tilts head, chest, waist, arms, "
                   "hips and legs a few degrees; board view with god hands.");
            slider("Push strength", &c.body_push_strength, 0.0f, 3.0f,
                   "%.2f", L"vr", L"body_push_strength", nullptr);
            slider("Push speed", &c.body_push_hz, 0.5f, 6.0f, "%.1f Hz",
                   L"vr", L"body_push_hz", "Spring frequency of the tilt.");
            slider("Push damping", &c.body_push_damping, 0.2f, 1.5f, "%.2f",
                   L"vr", L"body_push_damping",
                   "Near 1 = no swing past rest; lower swings a little.");
            slider("Head tilt", &c.body_push_head_degrees, 0.0f, 20.0f,
                   "%.1f deg", L"vr", L"body_push_head_degrees", nullptr);
            slider("Chest tilt", &c.body_push_chest_degrees, 0.0f, 15.0f,
                   "%.1f deg", L"vr", L"body_push_chest_degrees", nullptr);
            slider("Waist tilt", &c.body_push_waist_degrees, 0.0f, 15.0f,
                   "%.1f deg", L"vr", L"body_push_waist_degrees", nullptr);
            slider("Arm swing", &c.body_push_arm_degrees, 0.0f, 25.0f,
                   "%.1f deg", L"vr", L"body_push_arm_degrees", nullptr);
            slider("Hips tilt", &c.body_push_hips_degrees, 0.0f, 15.0f,
                   "%.1f deg", L"vr", L"body_push_hips_degrees", nullptr);
            slider("Leg give", &c.body_push_leg_degrees, 0.0f, 8.0f,
                   "%.1f deg", L"vr", L"body_push_leg_degrees", nullptr);

            ImGui::SeparatorText("Rear");
            toggle("Rear soft body", &c.body_rear, L"vr", L"body_rear",
                   "Her buttocks are masses on springs that lag and settle "
                   "as she moves.");
            slider("Rear speed", &c.body_rear_hz, 0.5f, 8.0f, "%.1f Hz",
                   L"vr", L"body_rear_hz",
                   "How fast a cheek bounces. Lower is softer and slower.");
            slider("Rear damping", &c.body_rear_damping, 0.05f, 1.5f, "%.2f",
                   L"vr", L"body_rear_damping",
                   "How quickly a bounce dies away.");
            slider("Rear gain", &c.body_rear_gain, 0.0f, 3.0f, "%.2f", L"vr",
                   L"body_rear_gain",
                   "The overall amount of movement: all of it scales with "
                   "this (0 = none). Use this, not the size, to move less.");
            slider("Rear max lag", &c.body_rear_max_cm, 0.0f, 8.0f, "%.1f cm",
                   L"vr", L"body_rear_max_cm",
                   "The most a cheek lags behind, at her size.");
            slider("Rear absolute max", &c.body_rear_limit_cm, 1.0f, 40.0f,
                   "%.0f cm", L"vr", L"body_rear_limit_cm",
                   "The most any point of the cheeks ever moves from rest, "
                   "however hard a hand pulls or high the gain is.");
            slider("Rear fullness", &c.body_rear_fullness_cm, -2.0f, 5.0f,
                   "%.1f cm", L"vr", L"body_rear_fullness_cm",
                   "How far the cheeks stand out at rest (her real size): "
                   "the baseline size. Minus = slimmer.");
            slider("Rear height", &c.body_rear_lift_cm, -12.0f, 12.0f,
                   "%.1f cm", L"vr", L"body_rear_lift_cm",
                   "Moves the moving regions up (+) or down (-): their "
                   "centre, at her real size.");
            slider("Rear spread", &c.body_rear_spread_cm, 0.0f, 16.0f,
                   "%.1f cm", L"vr", L"body_rear_spread_cm",
                   "8 = as fitted to her mesh; more moves each region out "
                   "from her middle.");
            slider("Rear size", &c.body_rear_size, 0.5f, 2.0f, "%.2f", L"vr",
                   L"body_rear_size",
                   "Size of the region that moves (not the shape itself; "
                   "see Rear fullness).");
            slider("Rear width", &c.body_rear_width, 0.5f, 2.0f, "%.2f",
                   L"vr", L"body_rear_width", "The region's width, times "
                   "its size.");
            slider("Rear tallness", &c.body_rear_height, 0.5f, 2.0f, "%.2f",
                   L"vr", L"body_rear_height", "The region's height, times "
                   "its size.");
            slider("Rear core", &c.body_rear_core, 0.0f, 0.9f, "%.2f",
                   L"vr", L"body_rear_core", "How much of a region moves "
                   "fully before it eases out.");
            slider("Rear dent", &c.body_rear_dent, 0.0f, 2.0f, "%.2f", L"vr",
                   L"body_rear_dent",
                   "God mode: a fingertip dents her where it presses, as a "
                   "multiple of a fingertip. 0 = no dent.");
            toggle("Rear debug lines", &c.body_rear_debug, L"vr",
                   L"body_rear_debug",
                   "Tuning aid: draws the regions as boxes (yellow = moves, "
                   "green = moves fully, magenta = the cleft, cyan = where "
                   "your hand touches, white cross = the pelvis origin).");
        }

        void god_section(Config& c)
        {
            ImGui::TextWrapped("Board view with god hands only.");
            ImGui::SeparatorText("Board leash");
            slider("Leash radius", &c.board_leash, 0.0f, 2.0f, "%.2f m",
                   L"vr", L"board_leash",
                   "Lara runs free this far (table metres) before the board "
                   "follows her. 0 = it follows exactly.");
            slider("Leash height", &c.board_leash_height, 0.0f, 1.0f,
                   "%.2f m", L"vr", L"board_leash_height",
                   "Same for up and down. 0 = height follows at once.");
            slider("View switch hold", &c.view_switch_hold, 0.15f, 2.0f,
                   "%.2f s", L"vr", L"view_switch_hold",
                   "Hold the right stick's click this long to switch view. "
                   "A quick click resets the angle.");
            slider("Zoom speed", &c.board_zoom_speed, 0.0f, 5.0f, "%.2f",
                   L"vr", L"board_zoom_speed",
                   "Board view: right stick up/down zooms to Lara (up = "
                   "closer); left/right turns round her. 0 = off.");
            slider("World scale speed", &c.board_scale_speed, 0.0f, 5.0f,
                   "%.2f", L"vr", L"board_scale_speed",
                   "Board view: hold the right stick in and push up/down to "
                   "scale Lara (up = bigger). 0 = off.");
            slider("Shoulder height speed", &c.stick_height_speed, 0.0f,
                   3.0f, "%.2f m/s", L"vr", L"stick_height_speed",
                   "Shoulder view: hold the right stick in and push up/down "
                   "to raise and lower the seat. 0 = off.");
            ImGui::SeparatorText("Pick-up");
            toggle("Grab with hand pose", &c.god_grab_pose, L"vr",
                   L"god_grab_pose",
                   "Pinch (trigger + thumb down) or fist (trigger + grip) "
                   "grabs; grip alone only points and presses. Needs finger "
                   "tracking. Off = the grip button grabs.");
            slider("Head zone", &c.god_head_zone, 0.05f, 1.0f, "%.2f m",
                   L"vr", L"god_head_zone",
                   "Grip zone round her head, metres at her size.");
            slider("Claim zone", &c.god_claim_zone, 0.0f, 3.0f, "%.2f m",
                   L"vr", L"god_claim_zone",
                   "A hand this close to her keeps its grip and trigger "
                   "from the game.");
            toggle("Drag by hand (experimental)", &c.god_drag_hand, L"vr",
                   L"god_drag_hand",
                   "Grab her hand or arm and drag her up like a ragdoll, "
                   "hanging by IK. Off: the head grab and the dangle "
                   "animation.");
            if (c.god_drag_hand)
            {
                slider("Arm zone", &c.god_arm_zone, 0.0f, 0.5f, "%.2f m",
                       L"vr", L"god_arm_zone",
                       "A grip this close to an arm holds that arm. 0 = no "
                       "arm grab.");
                toggle("Arm leads", &c.god_arm_lead, L"vr", L"god_arm_lead",
                       "Holding an arm, only the arm follows until you pull "
                       "past her reach.");
                toggle("Limp arms", &c.god_arms_limp, L"vr",
                       L"god_arms_limp",
                       "Her arms hang limp while she is held by the head.");
            }
            ImGui::SeparatorText("Throw and drop");
            toggle("Throwing", &c.god_throw, L"vr", L"god_throw",
                   "Let go mid-swing and she flies off your hand and lands "
                   "where she hits the level.");
            slider("Throw strength", &c.god_throw_strength, 0.2f, 3.0f,
                   "%.2f", L"vr", L"god_throw_strength",
                   "Your hand's speed times this.");
            slider("Throw gravity", &c.god_throw_gravity, 1.0f, 30.0f,
                   "%.2f m/s2", L"vr", L"god_throw_gravity",
                   "9.81 = a doll thrown on your table; lower floats more.");
            ImGui::SliderInt("Landing animation", &c.god_land_anim, -1, 4000,
                             "%d", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                wchar_t text[16]{};
                swprintf_s(text, L"%d", c.god_land_anim);
                config_save_setting(L"vr", L"god_land_anim", text);
            }
            help("Animation (game's own, by number) a thrown Lara plays on "
                 "landing. -1 = none.");
            slider("Drop height", &c.god_drop_height, 0.0f, 5.0f, "%.2f m",
                   L"vr", L"god_drop_height",
                   "Let go over a floor and she falls from no higher than "
                   "this. 0 = set straight down.");
        }

        void theatre_section(Config& c)
        {
            int mode = c.gameplay_theatre && c.cutscene_theatre ? 2
                     : c.cutscene_theatre ? 1 : 0;
            if (ImGui::Combo("Theatre window", &mode,
                             "Off\0Cutscenes\0Cutscenes and gameplay\0"))
                config_set_theatre(mode);
            help("Cutscenes (and optionally third-person play) seen through "
                 "a window fixed in a dark room, still in 3D.");

            float width = c.cutscene_screen_width;
            float height = c.cutscene_screen_height;
            bool wide = height <= 0.0f;
            bool size_changed = false, size_done = false;
            size_changed |= ImGui::SliderFloat("Screen width", &width, 0.5f,
                8.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            size_done |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox("16:9 height from the width", &wide))
            {
                height = wide ? 0.0f : width * 9.0f / 16.0f;
                size_changed = size_done = true;
            }
            if (!wide)
            {
                size_changed |= ImGui::SliderFloat("Screen height", &height,
                    0.1f, 8.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                size_done |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (size_changed || size_done)
                config_set_theatre_size(width, height, size_done);

            slider("Screen distance", &c.cutscene_screen_distance, 0.5f, 8.0f,
                   "%.2f m", L"vr", L"cutscene_screen_distance", nullptr);

            bool automatic = c.cutscene_scale <= 0.0f;
            if (ImGui::Checkbox("Scene scale: automatic", &automatic))
            {
                c.cutscene_scale = automatic ? 0.0f : 1.0f;
                if (automatic)
                    config_save_setting(L"vr", L"cutscene_scale", L"auto");
                else
                    save_float(L"vr", L"cutscene_scale", c.cutscene_scale);
            }
            help("Automatic scales every shot so Lara sits at the screen, "
                 "like a 3D film.");
            if (!automatic)
                slider("Scene scale", &c.cutscene_scale, 0.1f, 5.0f, "%.2f",
                       L"vr", L"cutscene_scale", "1 is life size.");

            float colour[3] = {
                ((c.theatre_backdrop >> 16) & 0xFF) / 255.0f,
                ((c.theatre_backdrop >> 8) & 0xFF) / 255.0f,
                (c.theatre_backdrop & 0xFF) / 255.0f };
            if (ImGui::ColorEdit3("Room colour", colour))
                c.theatre_backdrop =
                    ((unsigned)(colour[0] * 255.0f + 0.5f) << 16) |
                    ((unsigned)(colour[1] * 255.0f + 0.5f) << 8) |
                    (unsigned)(colour[2] * 255.0f + 0.5f);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                wchar_t text[16]{};
                swprintf_s(text, L"%06X", c.theatre_backdrop & 0xFFFFFFu);
                config_save_setting(L"vr", L"theatre_backdrop", text);
            }
            help("The room round the window. A chroma-key colour here lets "
                 "a passthrough headset replace it with your real room.");
        }

        void diorama_section(Config& c)
        {
            toggle("Board mode as a diorama", &c.board_diorama, L"vr",
                   L"board_diorama",
                   "The tabletop world inside a box open at the front and "
                   "top, the rest of the room dark.");
            slider("Box width", &c.board_diorama_width, 0.1f, 3.0f, "%.2f m",
                   L"vr", L"board_diorama_width", nullptr);
            slider("Box depth", &c.board_diorama_depth, 0.1f, 3.0f, "%.2f m",
                   L"vr", L"board_diorama_depth", nullptr);
            slider("Box height", &c.board_diorama_height, 0.1f, 3.0f,
                   "%.2f m", L"vr", L"board_diorama_height", nullptr);
            slider("Depth below the floor", &c.board_diorama_below, 0.0f,
                   1.0f, "%.2f m", L"vr", L"board_diorama_below",
                   "How far the box reaches under Lara's floor.");
        }

        void controls_section(Config& c)
        {
            toggle("Immersive controls", &c.immersive_controls, L"controls",
                   L"immersive_controls",
                   "Body-relative gestures: holsters, belt items, the chest "
                   "light, grabbing ledges. Put drawn weapons away before "
                   "switching this.");
            int menu = c.gear_menu_radial ? 1 : 0;
            if (ImGui::Combo("Gear menu", &menu, "Cross by the hip\0Radial\0"))
            {
                c.gear_menu_radial = menu == 1;
                config_save_setting(L"controls", L"gear_menu",
                                    c.gear_menu_radial ? L"radial" : L"cross");
            }
            help("Immersive third person: the 3D gear cross by your hip, or "
                 "hold the left stick in and push it toward the item. "
                 "Without immersive controls the radial menu is always used.");
            toggle("Finger tracking", &c.finger_tracking, L"controls",
                   L"finger_tracking",
                   "Lara's fingers follow yours from the controller's touch "
                   "sensors, trigger and grip.");
            slider("Relaxed finger curl", &c.finger_rest_curl, 0.0f, 0.6f,
                   "%.2f", L"controls", L"finger_rest_curl",
                   "How curled her fingers are with your hand open.");
        }

        void overlay_section(Config& c)
        {
            slider("Panel distance", &c.overlay_vr_distance, 0.3f, 5.0f,
                   "%.2f m", L"vr", L"overlay_vr_distance",
                   "How far in front of you this window floats in the "
                   "headset, in metres.");
            slider("Panel width", &c.overlay_vr_width_degrees, 10.0f, 120.0f,
                   "%.0f deg", L"vr", L"overlay_vr_width_degrees",
                   "How wide this window looks in the headset, in degrees.");
            slider("Panel to the left", &c.overlay_vr_yaw_degrees, -90.0f,
                   90.0f, "%.0f deg", L"vr", L"overlay_vr_yaw_degrees",
                   "How far to the left of straight ahead this window floats, "
                   "in degrees, round your head (negative = to the right). "
                   "It keeps facing you.");
            slider("Panel up", &c.overlay_vr_pitch_degrees, -60.0f, 60.0f,
                   "%.0f deg", L"vr", L"overlay_vr_pitch_degrees",
                   "How far above straight ahead this window floats, in "
                   "degrees (negative = below).");
            slider("Hold menu to open", &c.overlay_vr_open_hold, 0.0f, 3.0f,
                   "%.1f s", L"vr", L"overlay_vr_open_hold",
                   "Hold the controller menu (pause) button this long to open "
                   "this window; a quicker press still pauses the game. 0 = "
                   "off. Touch and Reverb controllers only for now.");
        }

        // `vr`: laid out for the headset panel (the window fills the
        // panel's square and stays put), else for the desktop window.
        void build_ui(bool vr)
        {
            Config& c = config_edit();
            ImGuiWindowFlags flags = 0;
            if (vr)
            {
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize,
                                         ImGuiCond_Always);
                flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse;
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f),
                                        ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(470.0f, 640.0f),
                                         ImGuiCond_FirstUseEver);
            }
            // Focused on opening, so the controllers start on its first item
            // (the Recentre button) without a click.
            if (g_just_opened)
                ImGui::SetNextWindowFocus();
            if (!ImGui::Begin("Tomb Raider: Legend VR settings", &g_open,
                              flags))
            {
                ImGui::End();
                return;
            }
            ImGui::TextWrapped("Changes apply at once and are saved to "
                               "trlvr.ini. Insert closes this window; the "
                               "game takes no mouse, keys or controllers "
                               "while it is open.");
            if (vr)
                ImGui::TextWrapped("Controllers: left stick moves, A selects, "
                                   "B goes back, right stick scrolls. On a "
                                   "slider: A, then left/right (hold LB / RB "
                                   "for slower / faster), then A. The menu "
                                   "button closes this.");
            if (ImGui::Button("Recentre"))
                vr_recenter();
            if (g_just_opened)
                ImGui::SetItemDefaultFocus();
            help("Recentre the view on where you are looking now (the same "
                 "as a double click of the left stick). This window is put "
                 "in front of you again too.");
            ImGui::Spacing();
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::CollapsingHeader("Scale, height and distance",
                                        ImGuiTreeNodeFlags_DefaultOpen))
                scale_section(c);
            if (ImGui::CollapsingHeader("In the headset"))
                overlay_section(c);
            if (ImGui::CollapsingHeader("Picture"))
            {
                slider("Brightness", &c.picture_brightness, -0.5f, 0.5f,
                       "%+.2f", L"vr", L"picture_brightness",
                       "Added to the whole image in the headset.");
                slider("Contrast", &c.picture_contrast, 0.5f, 2.0f, "%.2f",
                       L"vr", L"picture_contrast",
                       "Spread about mid grey. Above 1 deepens a washed-out "
                       "picture.");
                toggle("Use the game's brightness / contrast",
                       &c.picture_follow_game, L"vr", L"picture_follow_game",
                       "The game's own sliders only work fullscreen; this "
                       "applies what they set to the headset image instead.");
            }
            if (ImGui::CollapsingHeader("Third-person cameras"))
                camera_section(c);
            if (ImGui::CollapsingHeader("Body physics"))
                body_section(c);
            if (ImGui::CollapsingHeader("God mode"))
                god_section(c);
            if (ImGui::CollapsingHeader("Cutscene theatre"))
                theatre_section(c);
            if (ImGui::CollapsingHeader("Board diorama"))
                diorama_section(c);
            if (ImGui::CollapsingHeader("Controls"))
                controls_section(c);
            ImGui::PopItemWidth();
            ImGui::End();
        }
    }

    bool overlay_open()
    {
        return g_ready && g_open;
    }

    void overlay_before_reset()
    {
        // The panel target is in D3DPOOL_DEFAULT: made again on next use.
        release_panel();
        g_panel_failed = false;
        g_frame_built = false;
        if (g_ready)
            ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    void overlay_device_closing(IDirect3DDevice9* real_device)
    {
        if (!g_ready || real_device != g_device)
            return;
        // The window outlives the device (the game makes a new one when
        // anti-aliasing is switched), so hand its procedure back and start
        // again on the next device.
        if (g_hwnd && IsWindow(g_hwnd) && g_game_proc)
        {
            if (g_unicode)
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(g_game_proc));
            else
                SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(g_game_proc));
        }
        if (g_hook)
            UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        window_block_game_mouse(false);
        release_panel();
        g_panel_failed = false;
        g_frame_built = false;
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_device = nullptr;
        g_hwnd = nullptr;
        g_game_proc = nullptr;
        g_ready = false;
        g_open = false;
    }

    namespace
    {
        // One ImGui frame: the controllers in, the window laid out, drawn
        // into draw data. False if the menu button closed it instead.
        bool build_frame(bool vr)
        {
            if (!poll_pad())
                return false;
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            g_client_size = io.DisplaySize;     // the Win32 backend's: client pixels
            if (vr)
                io.DisplaySize = ImVec2((float)kPanelSize, (float)kPanelSize);
            feed_gamepad(io);
            ImGui::NewFrame();
            // The focus ring on the first item from the start: by default
            // ImGui shows it only after a directional press.
            if (g_just_opened)
                ImGui::SetNavCursorVisible(true);
            build_ui(vr);
            g_just_opened = false;
            ImGui::Render();
            g_frame_built = true;
            return true;
        }

        // The draw data onto `target`, with the device state restored
        // around it (the backend restores what it changes, but not the
        // render target, which the game may have left on anything). `clear`
        // first clears it to transparent (the panel: ImGui's blending then
        // leaves it premultiplied); the game's depth buffer is left out
        // (ImGui does not depth test, and it may not fit).
        void render_to(IDirect3DSurface9* target, ImDrawData* data, bool clear)
        {
            IDirect3DSurface9* old_target = nullptr;
            IDirect3DSurface9* old_depth = nullptr;
            D3DVIEWPORT9 old_viewport{};
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetDepthStencilSurface(&old_depth);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, target)))
            {
                g_device->SetDepthStencilSurface(nullptr);
                if (clear)
                    g_device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                    D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    ImGui_ImplDX9_RenderDrawData(data);
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                g_device->SetViewport(&old_viewport);
                old_target->Release();
            }
            g_device->SetDepthStencilSurface(old_depth);
            if (old_depth)
                old_depth->Release();
        }
    }

    bool overlay_request_open(bool open)
    {
        if (!g_ready)
            return false;
        if (open != g_open)
            set_open(open);                 // the same path as Insert
        return true;
    }

    unsigned overlay_open_count()
    {
        return g_open_count;
    }

    void overlay_set_pad(const OverlayPad& pad)
    {
        g_pad_in = pad;
        g_pad_fresh = true;
    }

    IDirect3DTexture9* overlay_vr_texture()
    {
        // Nothing at all while closed (or before the overlay is up: it
        // starts from the first overlay_frame).
        if (!g_ready || !g_open)
            return nullptr;
        sync_open();
        if (!g_open || !ensure_panel() || !build_frame(true))
            return nullptr;
        ImDrawData* data = ImGui::GetDrawData();
        if (!data || data->DisplaySize.x <= 0.0f ||
            data->DisplaySize.y <= 0.0f)
            return nullptr;
        render_to(g_panel_surface, data, true);
        if (!g_panel_logged)
        {
            g_panel_logged = true;
            log("overlay: shown in the headset as a %ux%u panel, %.2f m away, "
                "%.0f deg wide", kPanelSize, kPanelSize,
                (double)config().overlay_vr_distance,
                (double)config().overlay_vr_width_degrees);
        }
        return g_panel;
    }

    void overlay_frame(IDirect3DSwapChain9* chain)
    {
        // Initialised even while closed: the toggle key is seen by the
        // window procedure this installs.
        if (!init(chain))
            return;
        sync_open();
        if (!g_open)
        {
            g_frame_built = false;
            return;
        }

        IDirect3DSurface9* back = nullptr;
        D3DSURFACE_DESC desc{};
        if (FAILED(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) ||
            !back)
            return;
        if (FAILED(back->GetDesc(&desc)) || !desc.Width || !desc.Height)
        {
            back->Release();
            return;
        }

        // Already built for the headset this frame (overlay_vr_texture): the
        // same draw data, not a second frame.
        const bool built = g_frame_built;
        g_frame_built = false;
        if (!built && !build_frame(false))
        {
            back->Release();
            return;
        }

        // ImGui laid this out in the window's client pixels (for the
        // headset, in the panel's square at the client's top left). The back
        // buffer is the two-eye image, usually far larger and of another
        // shape, and is squeezed into the window when presented -- so the
        // draw data is stretched to the back buffer here, and comes out at
        // its intended size and shape on the monitor, under the mouse.
        ImDrawData* data = ImGui::GetDrawData();
        const ImVec2 client = g_client_size.x > 0.0f && g_client_size.y > 0.0f
            ? g_client_size
            : (data ? data->DisplaySize : ImVec2(0.0f, 0.0f));
        if (data && client.x > 0.0f && client.y > 0.0f)
        {
            const ImVec2 scale((float)desc.Width / client.x,
                               (float)desc.Height / client.y);
            for (int n = 0; n < data->CmdListsCount; ++n)
            {
                ImDrawList* list = data->CmdLists[n];
                for (int i = 0; i < list->VtxBuffer.Size; ++i)
                {
                    list->VtxBuffer[i].pos.x *= scale.x;
                    list->VtxBuffer[i].pos.y *= scale.y;
                }
            }
            data->ScaleClipRects(scale);
            data->DisplayPos.x *= scale.x;
            data->DisplayPos.y *= scale.y;
            data->DisplaySize = ImVec2((float)desc.Width, (float)desc.Height);
            render_to(back, data, false);
        }
        back->Release();
    }
}
