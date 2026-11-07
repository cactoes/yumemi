//==========================================
/// @file       wwindow.h
/// @brief      windows window stuff
//==========================================

#pragma once

#ifndef WWINDOW_H
#define WWINDOW_H

#define MENU_IMPORT_ROM         1001
#define MENU_EXIT               1003

#define MENU_FRAMESKIP_AUTO     1101
#define MENU_FRAMESKIP_1        1103
#define MENU_FRAMESKIP_2        1104
#define MENU_FRAMESKIP_4        1105
#define MENU_FRAMESKIP_6        1106
#define MENU_FRAMESKIP_8        1107
#define MENU_FRAMESKIP_10       1108

#define MENU_SPEED_100          1201
#define MENU_SPEED_200          1202
#define MENU_SPEED_300          1203
#define MENU_SPEED_400          1204
#define MENU_SPEED_500          1205
#define MENU_SPEED_UNLIMITED    1206

#define MENU_HELP_KEYBINDS      1301
#define MENU_ABOUT_INFO         1401
#define MENU_TOGGLE_FPS         1501
#define MENU_AUDIO_MUTE         1601

#include "typedef.h"

enum {
    WW_DIALOG_TYPE_INFO,
    WW_DIALOG_TYPE_WARNING,
    WW_DIALOG_TYPE_ERROR
};

void install_window_menu();
void disable_import_rom();
void open_dialog(u32 type, const char* title, const char* message);

#endif // WWINDOW_H