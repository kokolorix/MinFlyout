/**
 * \file Common.cpp
 * \ingroup app
 * \brief DPI and theme helpers, resolved dynamically for older systems.
 */
#include "Common.h"

#include <algorithm>

namespace mfly {
namespace {

// Resolved dynamically so that the EXE also starts on older Windows versions
// that do not know GetDpiForWindow yet.
using PFN_GetDpiForWindow  = UINT(WINAPI*)(HWND);
using PFN_GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

PFN_GetDpiForWindow LoadGetDpiForWindow() {
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        return reinterpret_cast<PFN_GetDpiForWindow>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
    }
    return nullptr;
}

PFN_GetDpiForMonitor LoadGetDpiForMonitor() {
    static HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore) {
        return reinterpret_cast<PFN_GetDpiForMonitor>(
            reinterpret_cast<void*>(::GetProcAddress(shcore, "GetDpiForMonitor")));
    }
    return nullptr;
}

UINT DpiFromMonitor(HMONITOR mon) {
    static PFN_GetDpiForMonitor fn = LoadGetDpiForMonitor();
    if (fn && mon) {
        UINT x = 96, y = 96;
        if (SUCCEEDED(fn(mon, /*MDT_EFFECTIVE_DPI*/ 0, &x, &y)) && x != 0) {
            return x;
        }
    }
    HDC dc = ::GetDC(nullptr);
    UINT dpi = dc ? static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX)) : 96u;
    if (dc) ::ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96u;
}

/// Signature of \c DwmGetWindowAttribute.
using PFN_DwmGetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
/// Signature of \c GetSystemMetricsForDpi.
using PFN_GetSystemMetricsForDpi = int(WINAPI*)(int, UINT);

PFN_DwmGetWindowAttribute LoadDwmGetWindowAttribute() {
    static HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        return reinterpret_cast<PFN_DwmGetWindowAttribute>(
            reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmGetWindowAttribute")));
    }
    return nullptr;
}

PFN_GetSystemMetricsForDpi LoadGetSystemMetricsForDpi() {
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        return reinterpret_cast<PFN_GetSystemMetricsForDpi>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetSystemMetricsForDpi")));
    }
    return nullptr;
}

}  // namespace

bool DwmWindowAttribute(HWND window, DWORD attribute, void* value, DWORD size) {
    static PFN_DwmGetWindowAttribute fn = LoadDwmGetWindowAttribute();
    if (!fn || !window || !value) return false;
    return SUCCEEDED(fn(window, attribute, value, size));
}

bool VisibleFrame(HWND window, RECT& frame) {
    if (!window || !::IsWindow(window)) return false;

    RECT dwmFrame{};
    if (DwmWindowAttribute(window, kDwmExtendedFrameBounds, &dwmFrame, sizeof(dwmFrame)) &&
        dwmFrame.right > dwmFrame.left && dwmFrame.bottom > dwmFrame.top) {
        frame = dwmFrame;
        return true;
    }
    return ::GetWindowRect(window, &frame) != FALSE;
}

int SystemMetricForDpi(int index, UINT dpi) {
    static PFN_GetSystemMetricsForDpi fn = LoadGetSystemMetricsForDpi();
    if (fn) {
        const int value = fn(index, dpi ? dpi : 96u);
        if (value != 0) return value;
    }
    // Older systems: the metric comes back at the system DPI and is scaled.
    const int atSystemDpi = ::GetSystemMetrics(index);
    const UINT systemDpi = DpiFromMonitor(nullptr);
    return ::MulDiv(atSystemDpi, static_cast<int>(dpi ? dpi : 96u),
                    static_cast<int>(systemDpi ? systemDpi : 96u));
}

int TitleBarHeight(UINT dpi) {
    const int caption = SystemMetricForDpi(SM_CYCAPTION, dpi);
    const int padded = SystemMetricForDpi(SM_CXPADDEDBORDER, dpi);

    // Windows 11 documents the standard title bar as 32 pixels at 100 %, and
    // the classic metrics can come out below that. The floor matters: the
    // estimated button block is three button widths wide and right-aligned, so
    // too small a height pushes the minimize slot to the right - onto the
    // maximize button. Too large a height only pushes it left into empty
    // caption space, which is harmless.
    return std::max(caption + padded, Scale(32, dpi));
}

UINT DpiForWindowCompat(HWND hwnd) {
    static PFN_GetDpiForWindow fn = LoadGetDpiForWindow();
    if (fn && hwnd) {
        UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    return DpiFromMonitor(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

UINT DpiForPoint(POINT pt) {
    return DpiFromMonitor(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

bool SystemUsesDarkTheme() {
    DWORD value = 1, size = sizeof(value);
    LSTATUS st = ::RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (st != ERROR_SUCCESS) return false;
    return value == 0;
}

}  // namespace mfly
