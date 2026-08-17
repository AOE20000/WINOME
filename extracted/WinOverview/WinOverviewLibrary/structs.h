// SPDX-License-Identifier: GPL-2.0-only
// Origin: WinOverview <https://github.com/valinet/WinOverview> (GPL-2.0-or-later)
// Modified for WINOME, 2026-08-16.
#pragma once
#include <Windows.h>
#include <dwmapi.h>
#include <vector>

struct Animation {
    HWND hWnd;
    POINT start;
    POINT end;
    int w;
    int h;
    double scale;
    HTHUMBNAIL thumb;
};

struct AnimationInfo {
    std::vector<Animation> animations;
    UINT type;
    double delay_ms;
    BOOL isOpening;
};

struct MonitorInfo {
    HMONITOR hMonitor;
    RECT rcWork;
    RECT rcMonitor;
    RECT area;
    RECT realArea;
    RECT rcSearch;
    HWND hWallpaperWnd;
    HWND hWnd;
    HWND bkgHWnd;
    HWND focusHWnd;
    AnimationInfo animation;
    HANDLE hAnimMutex;
    HANDLE hAnimThread;
    HTHUMBNAIL hSearchThumb;
    HWND hSearchHWnd;
    SIZE* wallpaperOffset;
};

struct WindowInfo {
    HWND hwnd;
    RECT rect;
};