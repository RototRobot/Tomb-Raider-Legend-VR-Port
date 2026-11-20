#include "overlay.h"
#include "window.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../vr/vr_session.h"
#include "../vr/vr_backend.h"
#include "../vr/tr7_menu_hook.h"
#include "../vr/camera_head.h"
#include "../vr/ui_space.h"

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

        // The headset pointer (overlay_set_pointer): latest given, used once
        // by the next headset frame; when it last drove the panel, so the
        // desktop mouse is not mixed in meanwhile.
        OverlayPointer g_pointer_in{};
        bool g_pointer_fresh = false;
        ULONGLONG g_pointer_at = 0;
        // F8 presses we post for TR7-Menu-Hook, let past take_input.
        int g_pass_f8 = 0;
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
            if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_F8 &&
                g_pass_f8 > 0)
            {
                --g_pass_f8;
                return false;
            }
            if (!g_open || !g_ready || (!is_mouse(msg) && !is_key(msg)))
                return false;
            // The headset pointer drives the panel; the desktop mouse is
            // not mixed in meanwhile.
            if (is_mouse(msg) && GetTickCount64() - g_pointer_at < 250)
                return true;
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

        // Show advanced settings (System tab), as in the Underworld mod. Off:
        // only the settings a player who just wants to play would touch;
        // the hidden ones keep their values. Set at the top of build_ui.
        bool g_adv = false;

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
        }

        // Everything that only matters from Lara's eyes. Each control here
        // is read every frame, so it applies at once.
        void first_person_section(Config& c)
        {
            if (!c.hands_on_controllers)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Hands on controllers is off (Controls "
                                   "tab): her hands play the game's animation, "
                                   "so the Hands settings do nothing.");
            ImGui::SeparatorText("Body");
            int body = c.first_person_full_body ? 1 : 0;
            if (ImGui::Combo("What you see of Lara", &body,
                             "Hands only\0Full body\0"))
                config_set_full_body(body == 1);
            help("Hands only draws just her hands. Full body draws all of her "
                 "except the head, with her arms bent to reach your hands.");
            slider("Eye height", &c.first_person_eye_height,
                   -100.0f, 180.0f, "%.0f units", L"vr",
                   L"first_person_eye_height",
                   "Eye height above Lara's neck joint, about 3 units per "
                   "cm. Lower it if the view sits too high on her body.");

            ImGui::SeparatorText("Hands");
            toggle("Hands follow your controllers",
                   &c.first_person_tracked_hands, L"vr",
                   L"first_person_tracked_hands",
                   "First person only. Off: her arms keep the game's "
                   "animation. Needs Hands on controllers.");
            if (g_adv)
            {
                ImGui::BeginDisabled(!c.first_person_full_body);
                slider("Arm stretch", &c.first_person_arm_stretch,
                       1.0f, 2.0f, "x%.2f", L"vr", L"first_person_arm_stretch",
                       "Full body only: how much her arms may lengthen so her "
                       "hands reach your controllers without stretching at the "
                       "wrist. 1 = off.");
                ImGui::EndDisabled();
            }

            ImGui::SeparatorText("Moving your body");
            toggle("Roomscale", &c.roomscale, L"vr", L"roomscale",
                   "Walking around your room moves Lara, stopped by walls.");
            ImGui::BeginDisabled(!c.roomscale);
            if (g_adv)
                slider("Lean deadzone", &c.roomscale_deadzone, 0.02f,
                       0.6f, "%.2f m", L"vr", L"roomscale_deadzone",
                       "Leaning this far moves only your head; beyond it "
                       "Lara moves to you.");
            toggle("Walk with her animations", &c.roomscale_walk, L"vr",
                   L"roomscale_walk",
                   "A real step of more than 0.4 m is walked by the game "
                   "itself, so Lara turns and walks normally.");
            ImGui::EndDisabled();
            toggle("Crouch for real", &c.physical_crouch, L"vr",
                   L"physical_crouch",
                   "Crouching about 35 cm down makes Lara crouch.");

            ImGui::SeparatorText("View");
            bool hardcore = c.first_person_hardcore;
            if (ImGui::Checkbox("Hardcore view", &hardcore))
                config_set_first_person_hardcore(hardcore);
            help("The view turns with Lara through flips, rolls and spins "
                 "instead of staying level.");
            if (g_adv)
            {
                ImGui::BeginDisabled(!c.first_person_hardcore);
                slider("Hardcore threshold",
                       &c.first_person_hardcore_threshold, 0.0f, 60.0f,
                       "%.0f deg", L"vr", L"first_person_hardcore_threshold",
                       "Tilts smaller than this stay level (walking); bigger "
                       "ones are followed.");
                ImGui::EndDisabled();
                toggle("Keep the view up on a ledge pull-up",
                       &c.first_person_mantle_smoothing, L"vr",
                       L"first_person_mantle_smoothing",
                       "Holds the view at standing height while Lara kneels "
                       "at the top of a ledge. Off follows the game's "
                       "animation.");
            }

            ImGui::SeparatorText("Play");
            int secure = c.auto_secure_catch;
            if (ImGui::Combo("One-hand catches", &secure,
                             "Grab to secure\0Bars secure themselves\0"
                             "Bars and ledges secure themselves\0"))
            {
                c.auto_secure_catch = secure;
                config_save_setting(L"vr", L"auto_secure_catch",
                    secure == 1 ? L"bars" : secure == 2 ? L"all" : L"off");
            }
            help("The game's quick-grab prompt when Lara catches something "
                 "with one hand.");
            toggle("No bullet spread", &c.first_person_no_spread, L"vr",
                   L"first_person_no_spread",
                   "Shots go exactly where the gun points. Off keeps the "
                   "game's random spread.");
        }

        void hud_section(Config& c)
        {
            slider("Menu scale", &c.ui_scale, 0.1f, 2.0f, "%.2f",
                   L"vr", L"ui_scale", "Size of menus, loading screens and "
                   "movies in the headset.");
            slider("HUD scale", &c.hud_scale, 0.1f, 2.0f, "%.2f",
                   L"vr", L"hud_scale", "Size of the gameplay HUD (health, "
                   "ammo, prompts).");
            if (g_adv)
                slider("Binocular scale", &c.binocular_scale, 0.1f, 2.0f,
                       "%.2f", L"vr", L"binocular_scale", "Size of the "
                       "binocular viewfinder. Separate from the HUD and "
                       "menus.");
            slider("HUD follow", &c.hud_follow_degrees, 0.0f, 90.0f,
                   "%.0f deg", L"vr", L"hud_follow_degrees",
                   "The gameplay HUD stays where it is while you look within "
                   "this many degrees of it, then glides back in front of "
                   "you. 0 = stuck to your head. The binoculars always stay "
                   "fixed to your head.");
            if (g_adv)
                toggle("Pause menu fixed in the room", &c.pause_world_locked,
                       L"vr", L"pause_world_locked",
                       "On: the pause menu stays level where you were looking "
                       "when it opened, like the main menu. Off: it follows "
                       "the headset. The gameplay HUD has its own HUD follow "
                       "above.");
            toggle("Hide the gameplay HUD", &c.hide_gameplay_hud, L"vr",
                   L"hide_gameplay_hud",
                   "Turns off the head-locked gameplay HUD (health, ammo, "
                   "prompts) completely. World markers, the binocular view "
                   "and menus are not affected.");
        }

        void view_section(Config& c)
        {
            static const char* const kThird[3] = { "Classic", "Shoulder",
                                                   "Board" };
            const int third = camera_third_person_mode();
            ImGui::Text("Now: %s", camera_view_first_person()
                ? "First person" : kThird[third >= 0 && third <= 2 ? third : 0]);
            ImGui::SameLine();
            ImGui::BeginDisabled(ui_menu_active() ||
                                 ui_loading_screen_active());
            if (ImGui::Button("Switch view"))
                camera_toggle_view("settings menu");
            ImGui::EndDisabled();
            help("The same as the view switch on the controller. Not during "
                 "the game's menus or loading.");
            int mode = c.view_cycle_all ? 3 : c.third_person_mode;
            if (ImGui::Combo("View switch goes to", &mode,
                             "First person and Classic\0"
                             "First person and Shoulder\0"
                             "First person and Board\0"
                             "All four in turn\0"))
            {
                config_set_third_person_mode(mode);
                camera_set_third_person_mode(mode);
            }
            help("All four in turn: Classic, Shoulder, Board, First person. "
                 "The game's VR Settings page sets this too.");
            if (!g_adv)
                return;
            slider("Glide between views", &c.view_transition_seconds, 0.0f,
                   2.0f, "%.2f s", L"vr", L"view_transition_seconds",
                   "How long a view switch takes to glide from one view to "
                   "the next. 0 cuts instantly.");
            slider("View switch hold", &c.view_switch_hold, 0.15f, 2.0f,
                   "%.2f s", L"vr", L"view_switch_hold",
                   "Controller scheme: hold the right stick's click this long "
                   "to switch view. A quick click recentres.");
        }

        void camera_section(Config& c)
        {
            ImGui::SeparatorText("Moving the camera");
            slider("Camera speed", &c.camera_speed, 0.1f, 3.0f, "x%.2f",
                   L"vr", L"camera_speed",
                   "Third-person views: how fast the stick moves the camera "
                   "-- orbit, zoom, world scale and seat height all at once. "
                   "1 = normal.");
            slider("Camera smoothing", &c.third_person_smoothing, 0.0f, 0.5f,
                   "%.2f s", L"vr", L"third_person_smoothing",
                   "How softly the camera follows Lara's moves. 0 = rigid.");
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

        // Rear presets. High is the tuning the user settled on (2026-10-20);
        // the others step either side of it, keeping the max lag x gain
        // about 1.4 x the absolute max so a hard swing reaches into the
        // eased part of the limit without piling up against it. Fullness
        // stays at or above 1 cm: lower, and her stock mesh creases.
        struct RearPreset
        {
            const char* name;
            bool on;
            float hz, damping, gain, max_cm, limit_cm, fullness_cm, dent;
        };
        const RearPreset kRearPresets[] = {
            { "Off",    false, 3.5f, 0.39f, 0.66f, 4.4f, 2.0f, 2.0f,  0.62f },
            { "Low",    true,  4.5f, 0.55f, 0.45f, 3.0f, 1.0f, 1.0f,  0.40f },
            { "Medium", true,  4.0f, 0.47f, 0.55f, 3.7f, 1.5f, 1.25f, 0.50f },
            { "High",   true,  3.5f, 0.39f, 0.66f, 4.4f, 2.0f, 1.5f,  0.62f },
            { "Max",    true,  3.0f, 0.31f, 0.80f, 5.2f, 3.0f, 2.0f,  0.80f },
        };

        void rear_presets(Config& c)
        {
            ImGui::PushID("rear");
            ImGui::TextUnformatted("Preset");
            for (const RearPreset& preset : kRearPresets)
            {
                ImGui::SameLine();
                if (!ImGui::Button(preset.name))
                    continue;
                c.body_rear = preset.on;
                config_save_setting(L"vr", L"body_rear",
                                    preset.on ? L"1" : L"0");
                if (!preset.on)
                    continue;       // Off keeps the tuning for next time
                c.body_rear_hz = preset.hz;
                c.body_rear_damping = preset.damping;
                c.body_rear_gain = preset.gain;
                c.body_rear_max_cm = preset.max_cm;
                c.body_rear_limit_cm = preset.limit_cm;
                c.body_rear_fullness_cm = preset.fullness_cm;
                c.body_rear_dent = preset.dent;
                save_float(L"vr", L"body_rear_hz", preset.hz);
                save_float(L"vr", L"body_rear_damping", preset.damping);
                save_float(L"vr", L"body_rear_gain", preset.gain);
                save_float(L"vr", L"body_rear_max_cm", preset.max_cm);
                save_float(L"vr", L"body_rear_limit_cm", preset.limit_cm);
                save_float(L"vr", L"body_rear_fullness_cm",
                           preset.fullness_cm);
                save_float(L"vr", L"body_rear_dent", preset.dent);
            }
            ImGui::PopID();
            help("Sets the rear's speed, damping, gain, max lag, absolute "
                 "max, fullness and dent together. Off shows her stock "
                 "shape and keeps the tuning. The sliders below still "
                 "fine-tune.");
        }

        void body_section(Config& c)
        {
            soft_presets(c);
            ImGui::Spacing();
            toggle("Chest spring", &c.body_spring, L"vr", L"body_spring",
                   "First-person full body: her chest sways a little on a "
                   "spring.");
            if (g_adv)
                slider("Chest spring limit", &c.body_spring_angle, 0.0f,
                       15.0f, "%.1f deg", L"vr", L"body_spring_angle",
                       nullptr);
            toggle("Soft parts", &c.body_soft, L"vr", L"body_soft",
                   "Her soft parts bounce, in first and third person; in first "
                   "person they also give under your hands.");
            if (g_adv)
            {
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
            }

            ImGui::SeparatorText("Push (god mode)");
            toggle("Whole-body push", &c.body_push, L"vr", L"body_push",
                   "A hand leaning on her tilts head, chest, waist, arms, "
                   "hips and legs a few degrees; board view with god hands.");
            if (g_adv)
            {
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
            }

            ImGui::SeparatorText("Rear");
            rear_presets(c);
            toggle("Rear soft body", &c.body_rear, L"vr", L"body_rear",
                   "Her buttocks are masses on springs that lag and settle "
                   "as she moves.");
            if (!g_adv)
                return;
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
            slider("Rear absolute max", &c.body_rear_limit_cm, 0.5f, 8.0f,
                   "%.1f cm", L"vr", L"body_rear_limit_cm",
                   "The most any point of the cheeks ever moves from rest, "
                   "however hard a hand pulls or high the gain is. Motion "
                   "eases toward it rather than stopping at it.");
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
            ImGui::SeparatorText("Board");
            if (g_adv)
            {
                slider("Leash radius", &c.board_leash, 0.0f, 2.0f, "%.2f m",
                       L"vr", L"board_leash",
                       "Lara runs free this far (table metres) before the board "
                       "follows her. 0 = the old 12 cm leash.");
                slider("Leash height", &c.board_leash_height, 0.0f, 1.0f,
                       "%.2f m", L"vr", L"board_leash_height",
                       "Same for up and down. 0 = height follows at once.");
                toggle("Camera collision", &c.board_camera_collision, L"vr",
                       L"board_camera_collision",
                       "On: the board camera stops short of walls, floor and "
                       "rock. Off: it passes through them (classic and "
                       "shoulder keep their own collision).");
            }
            toggle("Board turns with Lara", &c.board_follow_lara, L"vr",
                   L"board_follow_lara",
                   "Off (as Underworld): the board keeps its heading -- the "
                   "right stick and a recentre turn it -- and the move stick "
                   "follows your head. On: the board turns with the game's "
                   "camera, which swings round behind Lara.");
            if (g_adv)
            {
                slider("Zoom speed", &c.board_zoom_speed, 0.0f, 5.0f, "%.2f",
                       L"vr", L"board_zoom_speed",
                       "Controller scheme: right stick up/down zooms to Lara (up "
                       "= closer). 0 = off.");
                slider("World scale speed", &c.board_scale_speed, 0.0f, 5.0f,
                       "%.2f", L"vr", L"board_scale_speed",
                       "Controller scheme, board view: hold the right stick in "
                       "and push up/down to scale Lara. 0 = off.");
                slider("Shoulder height speed", &c.stick_height_speed, 0.0f,
                       3.0f, "%.2f m/s", L"vr", L"stick_height_speed",
                       "Controller scheme, shoulder view: hold the right stick in "
                       "and push up/down to raise and lower the seat. 0 = off.");
            }
            ImGui::SeparatorText("Arm stretch");
            slider("Arm stretch", &c.god_stretch_range, 0.0f, 5.0f, "%.2f m",
                   L"vr", L"god_stretch_range",
                   "Board view: reach further than your arm. Past the start "
                   "below, the hand moves out faster, this many metres "
                   "further with the arm fully out. 0 = off.");
            if (g_adv)
                slider("Arm stretch starts at", &c.god_stretch_start, 15.0f,
                       80.0f, "%.0f cm", L"vr", L"god_stretch_start",
                       "How far from your chest the hand stays on the "
                       "controller before it starts to stretch.");
            ImGui::SeparatorText("Pick-up");
            toggle("Grab with hand pose",&c.god_grab_pose, L"vr",
                   L"god_grab_pose",
                   "Pinch (trigger + thumb down) or fist (trigger + grip) "
                   "grabs; grip alone only points and presses. Needs finger "
                   "tracking. Off = the grip button grabs.");
            if (g_adv)
            {
                slider("Head zone", &c.god_head_zone, 0.05f, 1.0f, "%.2f m",
                       L"vr", L"god_head_zone",
                       "Grip zone round her head, metres at her size. Off "
                       "Drag by hand, a grab there snaps her arms up into the "
                       "hang pose.");
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
                    toggle("Two hands", &c.god_two_hands, L"vr", L"god_two_hands",
                           "Holding her by one arm, grab her other arm with your "
                           "other hand: she hangs below both hands. Let go of one "
                           "and she hangs by the other.");
                    toggle("Limp arms", &c.god_arms_limp, L"vr",
                           L"god_arms_limp",
                           "Her arms hang limp while she is held by the head.");
                }
            }
            ImGui::SeparatorText("Throw and drop");
            toggle("Throwing", &c.god_throw, L"vr", L"god_throw",
                   "Let go mid-swing and she flies off your hand and lands "
                   "where she hits the level.");
            if (g_adv)
            {
                slider("Throw strength", &c.god_throw_strength, 0.2f, 3.0f,
                       "%.2f", L"vr", L"god_throw_strength",
                       "Your hand's speed times this.");
                slider("Throw gravity", &c.god_throw_gravity, 1.0f, 30.0f,
                       "%.2f m/s2", L"vr", L"god_throw_gravity",
                       "9.81 = a doll thrown on your table; lower floats more.");
                toggle("Throw: averaged speed", &c.god_throw_velocity, L"vr",
                       L"god_throw_velocity",
                       "On: a throw takes your hand's fastest 5-frame average (a "
                       "jitter or the backswing is ignored). Off: the old peak.");
                slider("Throw bounce", &c.god_throw_bounce, 0.0f, 1.0f, "%.2f",
                       L"vr", L"god_throw_bounce",
                       "Off a wall or ceiling she keeps this much of her speed "
                       "into it. 0 = stops dead.");
                slider("Throw friction", &c.god_throw_friction, 0.0f, 1.0f,
                       "%.2f", L"vr", L"god_throw_friction",
                       "... and loses this much of her speed along it.");
                slider("Throw radius", &c.god_throw_radius, 0.3f, 2.0f, "%.2fx",
                       L"vr", L"god_throw_radius",
                       "The ball round a thrown Lara's middle that hits the "
                       "level (1 = a third of her height).");
                slider("Ragdoll", &c.god_ragdoll, 0.0f, 2.0f, "%.2fx", L"vr",
                       L"god_ragdoll",
                       "How loosely her limbs swing, held or thrown. 0 = they "
                       "turn with her body.");
                slider("Knee bend", &c.god_knee_bend, 0.0f, 45.0f, "%.0f deg",
                       L"vr", L"god_knee_bend",
                       "How far her knees bend while held or thrown.");
                slider("Throw spin", &c.god_throw_spin, 0.0f, 4.0f, "%.2f",
                       L"vr", L"god_throw_spin",
                       "The tumble a throw starts, rad/s per m/s across. 0 = "
                       "she keeps only the swing she had.");
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
                slider("Drop minimum", &c.god_drop_min, 0.0f, 1.0f, "%.2f m",
                       L"vr", L"god_drop_min",
                       "Let go and she is left at least this high above the "
                       "floor, so even a light let-go is the game's own fall.");
            }
            ImGui::SeparatorText("Torso grab (fist)");
            toggle("Torso grab", &c.god_torso, L"vr", L"god_torso",
                   "Make a fist with the palm toward her waist, back or "
                   "belly and hold her like a doll: she follows your hand, "
                   "her limbs hang. Let go with a swing to toss her. Needs "
                   "'Grab with hand pose'.");
            if (!g_adv)
                return;
            slider("Torso max height", &c.god_torso_max_height, 0.05f, 3.0f,
                   "%.2f m", L"vr", L"god_torso_max_height",
                   "Only while she looks no taller than this to you.");
            slider("Torso facing", &c.god_torso_facing, 0.0f, 1.0f, "%.2f",
                   L"vr", L"god_torso_facing",
                   "How squarely the palm must face her spine "
                   "(0 = any, 1 = dead on).");
            slider("Torso zone radius", &c.god_torso_radius, 0.05f, 1.0f,
                   "%.2f m", L"vr", L"god_torso_radius",
                   "Round her spine, metres at her size.");
            slider("Torso hand margin", &c.god_torso_margin, 0.0f, 0.3f,
                   "%.2f m", L"vr", L"god_torso_margin",
                   "Plus this margin, metres at her size (the zones keep "
                   "their size against her at any world scale).");
            slider("Torso zone lower", &c.god_torso_lower, 0.0f, 0.5f,
                   "%.2f m", L"vr", L"god_torso_lower",
                   "Slide the zone down her spine toward her hips.");
            slider("Torso grab grace", &c.god_torso_grace, 0.0f, 1.0f,
                   "%.2f s", L"vr", L"god_torso_grace",
                   "The fist may close this long after the palm was aimed.");
            toggle("Fist beats chest pinch", &c.god_torso_fist_first, L"vr",
                   L"god_torso_fist_first",
                   "While she is small, a squeezed fist never holds a soft "
                   "part (chest, rear): only a pinch does.");
            slider("Fist block height", &c.god_fist_block_height, 0.0f, 3.0f,
                   "%.2f m", L"vr", L"god_fist_block_height",
                   "A fist never holds a soft part while she looks smaller "
                   "than this. 0 = never blocks.");
            slider("Torso toss strength", &c.god_torso_throw_scale, 0.0f,
                   3.0f, "%.2fx", L"vr", L"god_torso_throw_scale",
                   "Toss speed = your hand's speed times this. 0 = always "
                   "put down.");
            slider("Torso toss spin", &c.god_torso_spin, 0.0f, 2.0f, "%.2f",
                   L"vr", L"god_torso_spin",
                   "The tumble, as a fraction of your hand's turn.");
            toggle("Show grab zones", &c.god_zone_debug, L"vr",
                   L"god_zone_debug",
                   "Wireframes in the headset: torso green, head magenta, "
                   "arms orange, your hand's points white. Board view only "
                   "(the god hand does not run in classic or shoulder).");
            slider("Grab zone size", &c.god_zone_scale, 0.25f, 4.0f, "%.2fx",
                   L"vr", L"god_zone_scale",
                   "Multiplies the head, arm and torso grab zones.");
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
            if (g_adv && ImGui::Checkbox("16:9 height from the width", &wide))
            {
                height = wide ? 0.0f : width * 9.0f / 16.0f;
                size_changed = size_done = true;
            }
            if (g_adv && !wide)
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
            if (g_adv && ImGui::Checkbox("Scene scale: automatic", &automatic))
            {
                c.cutscene_scale = automatic ? 0.0f : 1.0f;
                if (automatic)
                    config_save_setting(L"vr", L"cutscene_scale", L"auto");
                else
                    save_float(L"vr", L"cutscene_scale", c.cutscene_scale);
            }
            help("Automatic scales every shot so Lara sits at the screen, "
                 "like a 3D film.");
            if (g_adv && !automatic)
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
            if (!g_adv)
                return;
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
            // The scheme is a preset over these three: "Custom" says so when
            // Gear menu or scale control were changed after choosing it.
            const bool preset_controller = c.control_scheme == 0 &&
                c.gear_menu_radial && c.tp_scale_control == 1;
            const bool preset_interaction = c.control_scheme == 1 &&
                !c.gear_menu_radial && c.tp_scale_control == 2;
            const bool preset_underworld = c.control_scheme == 2 &&
                c.gear_menu_radial && c.tp_scale_control == 1;
            int scheme = preset_controller ? 0 : preset_interaction ? 1
                : preset_underworld ? 2 : 3;
            if (ImGui::Combo("Control scheme", &scheme,
                             "Controller (sticks)\0Interaction (hands)\0"
                             "Alternate\0Custom\0") && scheme != 3)
                config_set_control_scheme(scheme);
            help("Controller: right stick click recentres, hold switches "
                 "view, up/down zooms, click-drag sets height or scale; radial "
                 "gear menu. Interaction: right stick click switches weapon, "
                 "long press switches view; the 3D gear cross and palm "
                 "sliders. Alternate: left "
                 "trigger locks on, right grip interacts, X recentres (double "
                 "click: PDA), the radial gear menu, right stick click "
                 "is accurate aim, both stick clicks recentre; in the board "
                 "view, right stick held in and pushed: up/down = seat "
                 "height, left/right = world scale. Custom: you "
                 "changed Gear menu or Scale and zoom "
                 "below; choose a scheme to reset them.");
            static const bool hands_at_start = c.hands_on_controllers;
            static const bool gestures_at_start = c.gesture_controls;
            toggle("Hands on controllers", &c.hands_on_controllers,
                   L"controls", L"hands_on_controllers",
                   "In every view, Lara's hands and what they hold follow "
                   "your controllers. Off: her hands play the game's "
                   "animation, and god mode, body push and the motorbike "
                   "handlebars are off too.");
            toggle("Gesture controls", &c.gesture_controls, L"controls",
                   L"gesture_controls",
                   "First person: draw from holsters, take belt and pouch "
                   "items, chest + left grip for the light, grab ledges and "
                   "poles. Off: those are on buttons, as a normal gamepad "
                   "game. Put drawn weapons away before switching this.");
            if ((c.hands_on_controllers && !hands_at_start) ||
                (c.gesture_controls && !gestures_at_start))
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Restart the game for this to apply fully.");
            ImGui::SeparatorText("Fingers");
            toggle("Finger tracking", &c.finger_tracking, L"controls",
                   L"finger_tracking",
                   "Lara's fingers follow yours from the controller's touch "
                   "sensors, trigger and grip.");
            if (!g_adv)
                return;
            slider("Relaxed finger curl", &c.finger_rest_curl, 0.0f, 0.6f,
                   "%.2f", L"controls", L"finger_rest_curl",
                   "How curled her fingers are with your hand open.");
            ImGui::SeparatorText("Fine-tuning the scheme");
            int menu = c.gear_menu_radial ? 1 : 0;
            if (ImGui::Combo("Gear menu", &menu, "Cross by the hip\0Radial\0"))
            {
                c.gear_menu_radial = menu == 1;
                config_save_setting(L"controls", L"gear_menu",
                                    c.gear_menu_radial ? L"radial" : L"cross");
            }
            help("Third person: the 3D gear cross by your hip, or hold the "
                 "left stick in and push it toward the item. First person "
                 "uses the radial menu and gestures.");
            {
                int control = c.tp_scale_control;
                if (ImGui::Combo("Scale and zoom", &control,
                                 "Both\0Thumbstick\0Palm sliders\0"))
                    config_set_tp_scale_control(control);
                help("Third person: how scale and zoom are changed: the right "
                     "thumbstick, the sliders on the off hand's upturned "
                     "palm, or both.");
            }
        }

        void weapons_section(Config& c)
        {
            bool lefty = c.left_handed;
            if (ImGui::Checkbox("Left-handed", &lefty))
                config_set_left_handed(lefty);
            help("Long guns and aiming follow the left controller; the left "
                 "trigger fires and the right does Action/aim.");
            toggle("Two-handed long guns", &c.two_handed_guns, L"controls",
                   L"two_handed_guns",
                   "Grip with the free hand near a long gun's front to hold "
                   "it two-handed; the gun and aim pivot toward that hand.");
            int aim = c.aim_mode;
            if (g_adv && ImGui::Combo("Aim with", &aim,
                             "Main hand\0Both hands\0Hands and head\0"))
            {
                c.aim_mode = aim;
                config_save_setting(L"controls", L"aim_mode",
                    aim == 2 ? L"hands_head" : aim == 1 ? L"hands" : L"hand");
            }
            if (g_adv)
                help("Both hands: with two pistols drawn, aim between them. Hands "
                 "and head: averaged with where the headset looks.");
            toggle("Aim along the controller", &c.controller_aim, L"controls",
                   L"controller_aim",
                   "First person: weapons aim along the controller. Off: "
                   "along the centre of the headset's view.");
            toggle("3D crosshair", &c.vr_crosshair, L"controls",
                   L"vr_crosshair",
                   "A crosshair at the point the shot will hit, instead of "
                   "the game's flat reticles.");
            ImGui::BeginDisabled(!c.gesture_controls);
            int pistols = c.single_pistols ? 1 : 0;
            if (ImGui::Combo("Pistols from the hips", &pistols,
                             "Either hip draws both\0Each hip draws its own\0"))
            {
                c.single_pistols = pistols == 1;
                config_save_setting(L"controls", L"pistol_mode",
                                    pistols == 1 ? L"single" : L"akimbo");
            }
            help("Needs Gesture controls. Each hip: a pistol is fired by its "
                 "own hand's trigger; one pistol fires at half the rate.");
            ImGui::EndDisabled();
        }

        void turning_section(Config& c)
        {
            int snap = c.snap_turn ? 1 : 0;
            if (ImGui::Combo("First-person turning", &snap, "Smooth\0Snap\0"))
            {
                c.snap_turn = snap == 1;
                config_save_setting(L"controls", L"turn_mode",
                                    snap == 1 ? L"snap" : L"smooth");
            }
            help("How the right stick turns you in first person.");
            if (c.snap_turn)
                slider("Snap angle", &c.snap_turn_degrees, 5.0f, 180.0f,
                       "%.0f deg", L"controls", L"snap_turn_degrees", nullptr);
            else
                slider("Turn speed", &c.smooth_turn_speed, 30.0f, 360.0f,
                       "%.0f deg/s", L"controls", L"smooth_turn_speed",
                       "At full stick.");
            int vignette = c.comfort_vignette;
            if (ImGui::Combo("Comfort vignette", &vignette,
                             "Off\0Jumps and rolls\0All movement\0"))
            {
                c.comfort_vignette = vignette;
                config_save_setting(L"vr", L"comfort_vignette",
                    vignette == 2 ? L"full"
                    : vignette == 1 ? L"jumps" : L"off");
            }
            help("Darkens the edge of your view while Lara moves.");
        }

        void bike_section(Config& c)
        {
            ImGui::BeginDisabled(!c.hands_on_controllers);
            int bars = c.bike_handlebars;
            if (ImGui::Combo("Handlebars", &bars,
                             "Off\0Grab for show\0Hands steer\0"
                             "Hands steer, grip twists throttle\0"))
            {
                static const wchar_t* const kValues[4] = { L"0", L"1", L"2",
                                                           L"3" };
                c.bike_handlebars = bars;
                config_save_setting(L"controls", L"bike_handlebars",
                                    kValues[bars]);
            }
            help("Needs Hands on controllers. Grab for show: the stick still "
                 "rides.");
            if (g_adv)
            {
                slider("Steering travel", &c.bike_steer_range, 0.03f, 0.4f,
                       "%.2f m", L"controls", L"bike_steer_range",
                       "Hand travel for full steering lock.");
                slider("Throttle twist", &c.bike_twist_range, 10.0f, 90.0f,
                       "%.0f deg", L"controls", L"bike_twist_range",
                       "Twist for full throttle.");
                toggle("Show grab points", &c.bike_handlebar_markers, L"controls",
                       L"bike_handlebar_markers", "Rings on the handlebars.");
            }
            ImGui::EndDisabled();
        }

        // OpenXR / Quest settings. The runtime is chosen once at startup, so
        // that one needs a restart; the rest apply at once.
        void runtime_section(Config& c)
        {
            static const int started_with = c.vr_runtime;
            const VrBackend running = vr_backend();
            ImGui::Text("Running on: %s",
                        running == VrBackendOpenXr ? "OpenXR"
                        : running == VrBackendOpenVr ? "OpenVR / SteamVR"
                                                     : "none");
            int runtime = c.vr_runtime;
            if (ImGui::Combo("VR runtime", &runtime,
                             "Auto (OpenXR, then SteamVR)\0OpenXR only\0"
                             "OpenVR / SteamVR only\0"))
            {
                c.vr_runtime = runtime;
                config_save_setting(L"vr", L"vr_runtime",
                    runtime == 1 ? L"openxr"
                    : runtime == 2 ? L"openvr" : L"auto");
            }
            help("Which VR system the game starts on. OpenXR only never "
                 "starts SteamVR behind your back: if OpenXR fails, the log "
                 "says why. Takes effect after restarting the game.");
            if (c.vr_runtime != started_with)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Restart the game to apply this.");
            if (!g_adv)
                return;
            toggle("Match SteamVR grip pose", &c.xr_grip_match_openvr, L"vr",
                   L"xr_grip_match_openvr",
                   "Off: hands use OpenXR's own grip pose (Quest over Virtual "
                   "Desktop). On: the pose is converted to SteamVR's, which "
                   "the built-in hand tuning was made against.");
            toggle("Wait for the GPU copy", &c.xr_copy_wait, L"vr",
                   L"xr_copy_wait",
                   "Wait for each frame's copy to finish before handing it "
                   "to the headset. Leave on unless a runtime stutters.");
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
            if (!g_adv)
                return;
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
        // TR7-Menu-Hook (1.1.0, user 2026-10-27: "lets not have a sub
        // window, lets just take the options from the TR7 mod and format
        // them into the VR mod"). Its useful items, done here directly
        // (tr7_menu_hook.cpp): player, time and display through the game's
        // own functions and switches, its saved options through its
        // registry values. Its developer windows (instance viewer, material
        // editor, script debug) stay behind F8, on the off-hand panel.
        void tr7_check(const char* label, Tr7Flag flag, const char* tip)
        {
            bool on = false;
            if (!tr7_flag(flag, &on))
                return;
            if (ImGui::Checkbox(label, &on))
                tr7_set_flag(flag, on);
            help(tip);
        }

        void tr7_saved_check(const char* label, const char* name,
                             bool fallback, const char* tip)
        {
            bool on = tr7_option_bool(name, fallback);
            if (ImGui::Checkbox(label, &on))
                tr7_set_option_bool(name, on);
            help(tip);
        }

        void tr7_saved_slider(const char* label, const char* name,
                              float fallback, float lo, float hi,
                              const char* format)
        {
            static float editing[8];
            static const char* editing_name[8] = {};
            int slot = 0;
            while (slot < 7 && editing_name[slot] && editing_name[slot] != name)
                ++slot;
            if (editing_name[slot] != name)
            {
                editing_name[slot] = name;
                editing[slot] = tr7_option_float(name, fallback);
            }
            ImGui::SliderFloat(label, &editing[slot], lo, hi, format);
            // Saved once let go, not every frame of a drag.
            if (ImGui::IsItemDeactivatedAfterEdit())
                tr7_set_option_float(name, editing[slot]);
        }

        void mod_menu_section()
        {
            ImGui::SeparatorText("Player");
            if (ImGui::Button("Refill health"))
                tr7_refill_health();
            ImGui::SameLine();
            if (ImGui::Button("Next outfit"))
                tr7_next_outfit();
            help("Next outfit cycles Lara's outfits (with a model mod, its "
                 "models too).");
            if (float* time = tr7_time_multiplier())
            {
                ImGui::SliderFloat("Game speed", time, 0.1f, 4.0f, "%.2fx");
                ImGui::SameLine();
                if (ImGui::Button("Normal"))
                    *time = 1.0f;
            }

            if (g_adv)
            {
                ImGui::SeparatorText("Display");
                tr7_check("Wireframe", Tr7Wireframe,
                          "Draws the world as wireframe.");
                tr7_check("Hide the game's interface", Tr7HideUi,
                          "Hides the game's own menus and HUD.");
                tr7_check("Debug keypad", Tr7DebugKeypad,
                          "The game's developer keypad.");
            }

            ImGui::SeparatorText("Its saved settings (next game start)");
            {
                int skip = tr7_option_int("IntroSkip", 1);
                if (ImGui::Combo("Legal screen and intros", &skip,
                                 "Don't skip\0Skip legal\0Skip legal and intros\0"))
                    tr7_set_option_int("IntroSkip", skip);
            }
            tr7_saved_check("No cinematic bars", "NoCinematicBars", true,
                            "Removes the black bars in cutscenes.");
            tr7_saved_check("No motion blur", "NoMotionBlur", false,
                            "Turns off the game's motion blur.");
            tr7_saved_check("Hide its main-menu watermark", "NoWatermark", false,
                            "TR7-Menu-Hook's name on the main menu.");
            if (!g_adv)
                return;
            tr7_saved_slider("Free camera slow speed", "CameraSlowSpeed", 20.0f,
                             0.0f, 1000.0f, "%.0f");
            tr7_saved_slider("Free camera speed", "CameraNormalSpeed", 80.0f,
                             0.0f, 1000.0f, "%.0f");
            tr7_saved_slider("Free camera fast speed", "CameraFastSpeed", 200.0f,
                             0.0f, 1000.0f, "%.0f");

            ImGui::SeparatorText("Developer tools");
            if (ImGui::Button("Open its full menu (F8)") && g_hwnd)
            {
                // Shown on the off-hand panel, which this window hides.
                set_open(false);
                g_pass_f8 = 2;
                const LPARAM scan = (LPARAM)(MapVirtualKeyW(VK_F8, 0) << 16);
                PostMessageW(g_hwnd, WM_KEYDOWN, VK_F8, scan | 1);
                PostMessageW(g_hwnd, WM_KEYUP, VK_F8, scan | 1 | 0xC0000000);
                log("overlay: closed and F8 sent to TR7-Menu-Hook");
            }
            help("Its instance viewer, material editor and debug drawing. "
                 "Opens on your off hand; point with your main hand. F8 or "
                 "this button again closes it.");
        }

        void build_ui(bool vr)
        {
            Config& c = config_edit();
            g_adv = c.overlay_show_advanced;
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
                ImGui::TextWrapped("Controllers: point with your main hand and "
                                   "pull the trigger, or: left stick moves, A "
                                   "selects, B goes back, right stick scrolls. "
                                   "On a slider: A, then left/right (hold LB / "
                                   "RB for slower / faster), then A. The menu "
                                   "button closes this (Reverb G2, WMR, Index: "
                                   "hold both sticks in).");
            if (ImGui::Button("Recentre"))
                vr_reset_view();
            if (g_just_opened)
                ImGui::SetItemDefaultFocus();
            help("Recentre the view on where you are looking now (the same "
                 "as a double click of the left stick). This window is put "
                 "in front of you again too.");
            ImGui::Spacing();
            if (!ImGui::BeginTabBar("##tabs",
                                    ImGuiTabBarFlags_FittingPolicyResizeDown))
            {
                ImGui::End();
                return;
            }
            // Each page scrolls by itself, so the tabs stay in reach.
            const auto begin_tab = [](const char* name) {
                if (!ImGui::BeginTabItem(name))
                    return false;
                ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f),
                                  ImGuiChildFlags_NavFlattened);
                ImGui::PushItemWidth(
                    ImGui::GetContentRegionAvail().x * 0.5f);
                return true;
            };
            const auto end_tab = []() {
                ImGui::PopItemWidth();
                ImGui::EndChild();
                ImGui::EndTabItem();
            };
            if (begin_tab("View"))
            {
                view_section(c);
                if (g_adv)
                {
                    ImGui::SeparatorText("World and head scale");
                    scale_section(c);
                }
                if (ImGui::CollapsingHeader("Third-person cameras"))
                    camera_section(c);
                if (ImGui::CollapsingHeader("Cutscene theatre"))
                    theatre_section(c);
                if (ImGui::CollapsingHeader("Board diorama"))
                    diorama_section(c);
                end_tab();
            }
            if (begin_tab("First person"))
            {
                first_person_section(c);
                if (ImGui::CollapsingHeader("Motorbike"))
                    bike_section(c);
                end_tab();
            }
            if (begin_tab("God mode"))
            {
                god_section(c);
                end_tab();
            }
            if (begin_tab("Body"))
            {
                body_section(c);
                end_tab();
            }
            if (begin_tab("HUD"))
            {
                hud_section(c);
                if (ImGui::CollapsingHeader("This window in the headset"))
                    overlay_section(c);
                end_tab();
            }
            if (begin_tab("Controls"))
            {
                controls_section(c);
                if (ImGui::CollapsingHeader("Weapons and aiming"))
                    weapons_section(c);
                if (ImGui::CollapsingHeader("Turning and comfort"))
                    turning_section(c);
                end_tab();
            }
            if (begin_tab("System"))
            {
                toggle("Show advanced settings", &c.overlay_show_advanced,
                       L"vr", L"overlay_show_advanced",
                       "Shows every setting in every tab: tuning, physics, "
                       "experiments and test tools. Off: only the everyday "
                       "ones; the hidden settings keep their values.");
                ImGui::SeparatorText("Picture");
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
                if (g_adv)
                    toggle("Movies, menus and loading screens at 4:3",
                           &c.movie_widen_4x3, L"vr", L"movie_widen_4x3",
                           "On: movies, menus and loading screens are drawn at "
                           "4:3, as the game made them. Off: they fill the "
                           "headset view and come out about 0.7x too narrow.");
                ImGui::SeparatorText("VR runtime (OpenXR / Quest)");
                runtime_section(c);
                if (tr7_menu_hook_present() &&
                    ImGui::CollapsingHeader("TR7-Menu-Hook"))
                    mod_menu_section();
                end_tab();
            }
            ImGui::EndTabBar();
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
            // Never inside itself: something it calls into that makes the
            // game present again would end this frame under it.
            static bool building = false;
            if (building)
                return false;
            struct Guard { bool& b; ~Guard() { b = false; } } guard{ building };
            building = true;
            if (!poll_pad())
                return false;
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            g_client_size = io.DisplaySize;     // the Win32 backend's: client pixels
            if (vr)
                io.DisplaySize = ImVec2((float)kPanelSize, (float)kPanelSize);
            feed_gamepad(io);
            // After the Win32 backend's own mouse position: the last event
            // in a frame wins.
            if (vr && g_pointer_fresh)
            {
                io.AddMousePosEvent(g_pointer_in.u * (float)kPanelSize,
                                    g_pointer_in.v * (float)kPanelSize);
                io.AddMouseButtonEvent(0, g_pointer_in.down);
                g_pointer_at = GetTickCount64();
            }
            g_pointer_fresh = false;
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

    void overlay_set_pointer(const OverlayPointer& pointer)
    {
        g_pointer_in = pointer;
        g_pointer_fresh = true;
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
