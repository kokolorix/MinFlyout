/**
 * \file main.cpp
 * \ingroup app
 * \brief Entry point: DPI awareness, single instance, message loop.
 */
#include "AppController.h"
#include "Log.h"

namespace {

// Enable per-monitor DPI awareness v2 dynamically (works without a manifest
// and stays quiet on older systems).
void EnableDpiAwareness() {
    using PFN_SetCtx = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        auto setCtx = reinterpret_cast<PFN_SetCtx>(
            reinterpret_cast<void*>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setCtx && setCtx(reinterpret_cast<HANDLE>(-4))) {  // PER_MONITOR_AWARE_V2
            return;
        }
    }
    ::SetProcessDPIAware();
}

}  // namespace

/**
 * \brief Application entry point.
 *
 * Enables DPI awareness, uses a named mutex to make sure only a single
 * instance runs, and hands off to \ref mfly::AppController.
 *
 * \param instance Module instance.
 * \return Process exit code.
 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();

    HANDLE single = ::CreateMutexW(nullptr, TRUE, L"Local\\MinFlyout.SingleInstance");
    if (!single || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        WRITE_WARNING_LOG(L"Second instance rejected");
        ::MessageBoxW(nullptr, L"MinFlyout is already running.", L"MinFlyout",
                      MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    mfly::AppController& app = mfly::AppController::Instance();
    if (!app.Init(instance)) {
        WRITE_ERROR_LOG(L"Initialization failed, exiting");
        app.Shutdown();
        ::MessageBoxW(nullptr,
                      L"Initialization failed (could not install the mouse hook).",
                      L"MinFlyout", MB_ICONERROR | MB_OK);
        return 1;
    }

    const int result = app.RunMessageLoop();
    app.Shutdown();
    WRITE_INFO_LOG(mfly::log::dformat(L"Exit code {}", result));
    ::ReleaseMutex(single);
    ::CloseHandle(single);
    return result;
}
