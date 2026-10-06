// Updater module for nosleep
// Handles checking for updates, downloading, and installing new versions
#ifndef UPDATER_H
#define UPDATER_H

#include <windows.h>
#include "updater_logic.h"

// Check for updates against GitHub releases
// Returns true if check was successful (result in info)
// silent: if true, don't show error notifications
bool updater_check(UpdateInfo* info, HWND hwnd_parent);

// Download a new version and perform the update
// current_exe_path: UTF-16 full path to the current executable
// info: update information from updater_check
// HWND: parent window for any dialogs
// Returns true if update process started successfully
bool updater_download_and_install(UpdateInfo* info, const wchar_t* current_exe_path, HWND hwnd_parent);

// Show the "Update Available" dialog and return user's choice
// Returns true if user wants to download
bool updater_show_prompt_dialog(HWND hwnd_parent, UpdateInfo* info);

#endif // UPDATER_H
