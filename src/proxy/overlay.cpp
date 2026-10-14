#include "overlay.h"
#include "window.h"

#include "../common/config.h"
#include "../common/log.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <cstdio>
#include <windows.h>

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
                "desktop window");
            return true;
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
            slider("Mission Prep backdrop scale",
                   &c.mission_prep_backdrop_scale, 0.3f, 6.0f, "%.2f", L"vr",
                   L"mission_prep_backdrop_scale",
                   "Backdrop behind the Mission Prep menu, relative to the "
                   "view. 1 exactly fills it.");
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

        void build_ui()
        {
            Config& c = config_edit();
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(470.0f, 640.0f),
                                     ImGuiCond_FirstUseEver);
            if (!ImGui::Begin("Tomb Raider: Legend VR settings", &g_open))
            {
                ImGui::End();
                return;
            }
            ImGui::TextWrapped("Changes apply at once and are saved to "
                               "trlvr.ini. Insert closes this window; the "
                               "game takes no mouse or keys while it is "
                               "open.");
            ImGui::Spacing();
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::CollapsingHeader("Scale, height and distance",
                                        ImGuiTreeNodeFlags_DefaultOpen))
                scale_section(c);
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
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_device = nullptr;
        g_hwnd = nullptr;
        g_game_proc = nullptr;
        g_ready = false;
    }

    void overlay_frame(IDirect3DSwapChain9* chain)
    {
        // Initialised even while closed: the toggle key is seen by the
        // window procedure this installs.
        static bool was_open = false;
        if (!init(chain))
            return;
        // Closed with the window's own close button: give the mouse back.
        if (was_open && !g_open)
        {
            ImGui::GetIO().MouseDrawCursor = false;
            window_block_game_mouse(false);
        }
        was_open = g_open;
        if (!g_open)
            return;

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

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        build_ui();
        ImGui::Render();

        // ImGui laid this out in the window's client pixels. The back
        // buffer is the two-eye image, usually far larger and of another
        // shape, and is squeezed into the window when presented -- so the
        // draw data is stretched to the back buffer here, and comes out at
        // its intended size and shape on the monitor.
        ImDrawData* data = ImGui::GetDrawData();
        if (data && data->DisplaySize.x > 0.0f && data->DisplaySize.y > 0.0f)
        {
            const ImVec2 scale((float)desc.Width / data->DisplaySize.x,
                               (float)desc.Height / data->DisplaySize.y);
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

            // The backend restores the device state it changes, but not the
            // render target, which the game may have left on anything.
            IDirect3DSurface9* old_target = nullptr;
            D3DVIEWPORT9 old_viewport{};
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)) &&
                SUCCEEDED(g_device->BeginScene()))
            {
                ImGui_ImplDX9_RenderDrawData(data);
                g_device->EndScene();
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                g_device->SetViewport(&old_viewport);
                old_target->Release();
            }
        }
        back->Release();
    }
}
