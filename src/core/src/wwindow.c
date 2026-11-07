#include <raylib.h>
#undef bool

#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <windows.h>

#undef CloseWindow
#undef ShowCursor

#define COBJMACROS
#include <shobjidl.h>

#include <stdio.h>
#include <wchar.h>

#include "wwindow.h"
#include "renderer.h"
#include "apu.h"

static WNDPROC global_old_wnd_proc;
static HMENU global_frame_skip_menu;
static HMENU global_speed_menu;
static HMENU global_file_menu;
static HMENU global_emulator_menu;

extern char* rom_path;

static
void open_rom_file_picker() {
    HRESULT init_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (FAILED(init_result) && init_result != RPC_E_CHANGED_MODE)
        return;

    BOOL should_uninit = SUCCEEDED(init_result);
    IFileOpenDialog* dialog = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&dialog);

    if (SUCCEEDED(hr)) {
        COMDLG_FILTERSPEC filters[] = { { L"Game Boy Advance ROM", L"*.gba;*.zip" } };
        IFileOpenDialog_SetFileTypes(dialog, 1, filters);
        hr = IFileOpenDialog_Show(dialog, (HWND)GetWindowHandle());
        if (SUCCEEDED(hr)) {
            IShellItem* item = NULL;
            if (SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item))) {
                PWSTR path = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
                    if (len > 0) {
                        char* cpath = (char*)malloc(len);
                        if (cpath) {
                            WideCharToMultiByte(CP_UTF8, 0, path, -1, cpath, len, NULL, NULL);
                            rom_path = cpath;
                        }
                    }

                    CoTaskMemFree(path);
                }
                IShellItem_Release(item);
            }
        }

        IFileOpenDialog_Release(dialog);
    }

    if (should_uninit)
        CoUninitialize();
}

static
void menu_handle_frameskip(u32 value) {
    switch (value) {
        case MENU_FRAMESKIP_AUTO: render_set_frame_skip(0); break;
        case MENU_FRAMESKIP_1: render_set_frame_skip(1); break;
        case MENU_FRAMESKIP_2: render_set_frame_skip(2); break;
        case MENU_FRAMESKIP_4: render_set_frame_skip(4); break;
        case MENU_FRAMESKIP_6: render_set_frame_skip(6); break;
        case MENU_FRAMESKIP_8: render_set_frame_skip(8); break;
        case MENU_FRAMESKIP_10: render_set_frame_skip(10); break;
    }
}

static
void menu_handle_speed(u32 value) {
    switch (value) {
        case MENU_SPEED_100: render_set_speed(100); break;
        case MENU_SPEED_200: render_set_speed(200); break;
        case MENU_SPEED_300: render_set_speed(300); break;
        case MENU_SPEED_400: render_set_speed(400); break;
        case MENU_SPEED_500: render_set_speed(500); break;
        case MENU_SPEED_UNLIMITED: render_set_speed(0); break;
    }
}

static
void show_help_menu() {
    MessageBoxA(0,
        "Enter - Start\n"
        "Backspace - Select\n"
        "Arrow keys - D-Pad\n"
        "Z - A\n"
        "X - B\n"
        "Q - L\n"
        "W - R\n",
        "Keybinds",
        MB_OK | MB_ICONINFORMATION
    );
}

static
void show_info_menu() {
    MessageBoxA((HWND)GetWindowHandle(),
        "Yumemi\n"
        "Game Boy Advance emulator\n"
        "\n"
        "Version: 1\n"
        "Built: " __DATE__ "\n"
        "Graphics: raylib " RAYLIB_VERSION "\n"
        "\n"
        "Author: Cactoes\n"
        "github.com/cactoes/yumemi\n"
        "\n"
        "No ROMs or BIOS files are included.",
        "About Yumemi",
        MB_OK | MB_ICONINFORMATION
    );
}

static
LRESULT CALLBACK MyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case MENU_IMPORT_ROM:
                    open_rom_file_picker();
                    break;
                case MENU_EXIT:
                    render_set_close(true);
                    break;
                case MENU_FRAMESKIP_AUTO:
                case MENU_FRAMESKIP_1:
                case MENU_FRAMESKIP_2:
                case MENU_FRAMESKIP_4:
                case MENU_FRAMESKIP_6:
                case MENU_FRAMESKIP_8:
                case MENU_FRAMESKIP_10: {
                    CheckMenuRadioItem(global_frame_skip_menu, MENU_FRAMESKIP_AUTO, MENU_FRAMESKIP_10, LOWORD(wParam), MF_BYCOMMAND);
                    menu_handle_frameskip(LOWORD(wParam));
                    break;
                }
                case MENU_SPEED_100:
                case MENU_SPEED_200:
                case MENU_SPEED_300:
                case MENU_SPEED_400:
                case MENU_SPEED_500:
                case MENU_SPEED_UNLIMITED: {
                    CheckMenuRadioItem(global_speed_menu, MENU_SPEED_100, MENU_SPEED_UNLIMITED, LOWORD(wParam), MF_BYCOMMAND);
                    menu_handle_speed(LOWORD(wParam));
                    break;
                }
                case MENU_TOGGLE_FPS: {
                    UINT state = GetMenuState(global_emulator_menu, MENU_TOGGLE_FPS, MF_BYCOMMAND);
                    bool enable = !(state & MF_CHECKED);

                    CheckMenuItem(global_emulator_menu, MENU_TOGGLE_FPS, MF_BYCOMMAND | (enable ? MF_CHECKED : MF_UNCHECKED));
                    render_set_enable_fps(enable);
                    return 0;
                }
                case MENU_AUDIO_MUTE: {
                    if (!apu_is_ready()) {
                        MessageBoxA((HWND)GetWindowHandle(), "Please wait until the emulator has finished loading.", "Audio Not Ready", MB_OK | MB_ICONWARNING);
                        return 0;
                    }

                    UINT state = GetMenuState(global_emulator_menu, MENU_AUDIO_MUTE, MF_BYCOMMAND);
                    bool enable = !(state & MF_CHECKED);

                    CheckMenuItem(global_emulator_menu, MENU_AUDIO_MUTE, MF_BYCOMMAND | (enable ? MF_CHECKED : MF_UNCHECKED));
                    apu_set_muted(enable);
                    return 0;
                }
                case MENU_HELP_KEYBINDS:
                    show_help_menu();
                    break;
                case MENU_ABOUT_INFO:
                    show_info_menu();
                    break;
            }

            return 0;
        }
    }

    return CallWindowProc(global_old_wnd_proc, hwnd, msg, wParam, lParam);
}


void install_window_menu() {
    HWND hwnd = (HWND)GetWindowHandle();
    HMENU menu = CreateMenu();
    global_file_menu = CreatePopupMenu();

    AppendMenuA(global_file_menu, MF_STRING, MENU_IMPORT_ROM, "Import ROM");
    AppendMenuA(global_file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(global_file_menu, MF_STRING, MENU_EXIT, "Exit");

    AppendMenuA(menu, MF_POPUP, (UINT_PTR)global_file_menu, "File");

    global_emulator_menu = CreatePopupMenu();
    global_frame_skip_menu = CreatePopupMenu();
  
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_AUTO, "Auto");
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_1,  "1");
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_2,  "2");
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_4,  "4");
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_6,  "6");
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_8,  "8");
    AppendMenuA(global_frame_skip_menu, MF_STRING, MENU_FRAMESKIP_10, "10");

    CheckMenuRadioItem(global_frame_skip_menu, MENU_FRAMESKIP_AUTO, MENU_FRAMESKIP_10, MENU_FRAMESKIP_AUTO, MF_BYCOMMAND);

    global_speed_menu = CreatePopupMenu();
    AppendMenuA(global_speed_menu, MF_STRING, MENU_SPEED_100, "100%");
    AppendMenuA(global_speed_menu, MF_STRING, MENU_SPEED_200, "200%");
    AppendMenuA(global_speed_menu, MF_STRING, MENU_SPEED_300, "300%");
    AppendMenuA(global_speed_menu, MF_STRING, MENU_SPEED_400, "400%");
    AppendMenuA(global_speed_menu, MF_STRING, MENU_SPEED_500, "500%");
    AppendMenuA(global_speed_menu, MF_STRING, MENU_SPEED_UNLIMITED, "Unlimited");

    CheckMenuRadioItem(global_speed_menu, MENU_SPEED_100, MENU_SPEED_UNLIMITED, MENU_SPEED_100, MF_BYCOMMAND);

    AppendMenuA(global_emulator_menu, MF_POPUP, (UINT_PTR)global_frame_skip_menu, "Frame Skip");
    AppendMenuA(global_emulator_menu, MF_POPUP, (UINT_PTR)global_speed_menu, "Speed");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)global_emulator_menu, "Emulator");

    AppendMenuA(global_emulator_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(global_emulator_menu, MF_STRING, MENU_TOGGLE_FPS, "Show FPS");
    CheckMenuItem(global_emulator_menu, MENU_TOGGLE_FPS, MF_BYCOMMAND);

    AppendMenuA(global_emulator_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(global_emulator_menu, MF_STRING, MENU_AUDIO_MUTE, "Mute Audio");
    CheckMenuItem(global_emulator_menu, MENU_AUDIO_MUTE, MF_BYCOMMAND);

    HMENU help_menu = CreatePopupMenu();
    AppendMenuA(help_menu, MF_STRING, MENU_HELP_KEYBINDS, "Keybinds");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)help_menu, "Help");

    HMENU about_menu = CreatePopupMenu();
    AppendMenuA(about_menu, MF_STRING, MENU_ABOUT_INFO, "Info");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)about_menu, "About");

    RECT client;
    GetClientRect(hwnd, &client);

    SetMenu(hwnd, menu);

    RECT rect = { 0, 0, client.right, client.bottom };
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rect, style, TRUE, ex_style);

    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    DrawMenuBar(hwnd);

    global_old_wnd_proc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)MyWndProc);
}

void disable_import_rom() {
    EnableMenuItem(global_file_menu, MENU_IMPORT_ROM, MF_BYCOMMAND | MF_GRAYED);
    DrawMenuBar((HWND)GetWindowHandle());
}