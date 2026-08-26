/**
 * \file AppController.cpp
 * \ingroup app
 * \brief Implementation of the state machine \ref mfly::AppController.
 */
#include "AppController.h"

#include <algorithm>

#include <shellapi.h>

#include "Config.h"
#include "Diagnostics.h"
#include "ItemRegistry.h"
#include "Log.h"
#include "Monitors.h"
#include "TrayStash.h"
#include "WindowSizer.h"
#include "resource.h"

namespace mfly {
namespace {

constexpr UINT kAppTrayId = 0;       ///< ID of the application tray icon (windows start at 1).
constexpr UINT kMenuPause = 100;     ///< Menu command: pause detection.
constexpr UINT kMenuRestoreAll = 101;///< Menu command: restore stashed windows.
constexpr UINT kMenuOpenConfig = 102;///< Menu command: open configuration.
constexpr UINT kMenuReloadConfig = 103;  ///< Menu command: reload configuration.
constexpr UINT kMenuOpenDiagnosis = 105; ///< Menu command: open the diagnosis file.
constexpr UINT kMenuExit = 104;      ///< Menu command: exit.

/// ID of the diagnosis hotkey (Ctrl+Alt+F12).
constexpr int kHotkeyDiagnose = 1;

/// Poll ticks with cursor movement but no hook report before reviving the hook.
constexpr int kWatchdogStrikes = 3;

}  // namespace

AppController& AppController::Instance() {
    static AppController instance;
    return instance;
}

bool AppController::Init(HINSTANCE instance) {
    instance_ = instance;

    // Pick the matching size from the icon resource: large for Alt-Tab and the
    // taskbar, small for the notification area. LoadImage selects the image
    // variant that belongs to the requested size from the .ico.
    appIcon_ = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    trayIcon_ = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AppController::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hIcon = appIcon_;
    wc.hIconSm = trayIcon_;
    if (!::RegisterClassExW(&wc)) {
        WRITE_ERROR_LOG(L"RegisterClassEx failed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"MinFlyout",
                              WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, instance, this);
    if (!hwnd_) {
        WRITE_ERROR_LOG(L"CreateWindowEx failed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }

    if (!flyout_.Create(instance, hwnd_)) {
        WRITE_ERROR_LOG(L"Flyout window could not be created");
        return false;
    }

    TrayStash::Instance().Init(hwnd_);

    ConfigStore::Instance().Reload();  // creates the template on first run
    const Config& config = ConfigStore::Instance().current();
    log::SetFileLogging(config.logToFile);
    ParseProbeMode(config.buttonDetection, probeMode_);
    traceDetection_ = config.traceDetection;
    if (config.hasError()) {
        WRITE_WARNING_LOG(L"Configuration not usable, running on defaults", config.error);
    } else {
        WRITE_INFO_LOG(log::dformat(L"Configuration loaded, {} layouts", config.layouts.size()),
                       config.path);
    }

    RegisterBuiltinProviders();
    AddAppTrayIcon();

    if (!hooks_.Start(hwnd_)) {
        WRITE_ERROR_LOG(L"Mouse hook could not be installed",
                        log::dformat(L"error {}", ::GetLastError()));
        return false;
    }

    ::SetTimer(hwnd_, kTimerPoll, 400, nullptr);

    // Diagnosis hotkey. If something else already owns the combination the
    // application still runs - only the hotkey is then unavailable.
    hotkeyOk_ = ::RegisterHotKey(hwnd_, kHotkeyDiagnose,
                                 MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12) != FALSE;
    if (!hotkeyOk_) {
        WRITE_WARNING_LOG(L"Ctrl+Alt+F12 is taken, the diagnosis hotkey is unavailable",
                          log::dformat(L"error {}", ::GetLastError()));
    }

    WRITE_INFO_LOG(log::dformat(L"MinFlyout started, build {}", BuildStamp()),
                   log::dformat(L"pid {}", ::GetCurrentProcessId()));
    return true;
}

void AppController::Shutdown() {
    WRITE_INFO_LOG(L"Shutting down");
    if (hotkeyOk_) {
        ::UnregisterHotKey(hwnd_, kHotkeyDiagnose);
        hotkeyOk_ = false;
    }
    CloseFlyout(false);
    hooks_.Stop();

    // Hidden foreign windows MUST come back.
    TrayStash::Instance().RestoreAll();
    RemoveAppTrayIcon();
    flyout_.Destroy();

    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }

    // Icons loaded with LoadImage belong to us (no LR_SHARED).
    if (trayIcon_) { ::DestroyIcon(trayIcon_); trayIcon_ = nullptr; }
    if (appIcon_)  { ::DestroyIcon(appIcon_);  appIcon_ = nullptr; }
}

int AppController::RunMessageLoop() {
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// --- State machine ---------------------------------------------------------

void AppController::HandleMouseMove() {
    HookThread::AckMouseMove();  // the next move may be posted again
    if (paused_) return;

    POINT pt{};
    if (!::GetCursorPos(&pt)) return;

    const RECT hotZone = InflateCopy(hit_.buttonRect, 2, 2);

    if (state_ == State::Open) {
        const RECT flyRect = flyout_.ScreenRect();
        // Corridor between button and flyout, so the mouse may travel there.
        RECT bridge{std::min(flyRect.left, hotZone.left),
                    std::min(flyRect.top, hotZone.top),
                    std::max(flyRect.right, hotZone.right),
                    std::max(flyRect.bottom, hotZone.bottom)};

        if (PtInRectPt(flyRect, pt) || PtInRectPt(hotZone, pt) || PtInRectPt(bridge, pt)) {
            if (graceRunning_) {
                ::KillTimer(hwnd_, kTimerGrace);
                graceRunning_ = false;
            }
        } else if (!graceRunning_) {
            ::SetTimer(hwnd_, kTimerGrace,
                       ConfigStore::Instance().current().closeGraceMs, nullptr);
            graceRunning_ = true;
        }
        return;
    }

    if (state_ == State::Armed) {
        if (PtInRectPt(hotZone, pt)) return;  // still on the button: keep waiting
        ::KillTimer(hwnd_, kTimerHover);
        state_ = State::Idle;
    }

    if (suppressUntilLeave_) {
        if (PtInRectPt(hotZone, pt)) return;
        suppressUntilLeave_ = false;
    }

    // Throttling: hit-tests go to foreign windows as a message.
    const ULONGLONG now = ::GetTickCount64();
    if (now - lastProbeTick_ < kProbeThrottle) return;
    if (pt.x == lastProbePt_.x && pt.y == lastProbePt_.y) return;
    lastProbeTick_ = now;
    lastProbePt_ = pt;

    // The hit test comes first and is gated by nothing of ours. It is one
    // message, it is what the detection was originally built on, and it works
    // for every window that answers - including the ones with their own title
    // bar. Putting the computed geometry in front of it made the reliable path
    // depend on the speculative one; it does not any more.
    HWND under = ::WindowFromPoint(pt);
    HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;

    if (!root) {
        TraceGate(nullptr, pt, L"WindowFromPoint found nothing");
        return;
    }
    if (IsIgnoredWindow(root)) {
        TraceGate(root, pt, L"window is on the ignore list");
        return;
    }

    const log::Stopwatch probeTime;
    HitInfo info{};
    if (!ProbeMinimizeButton(pt, probeMode_, info)) {
        // Only complain where a button could plausibly be - otherwise every
        // sweep across the desktop would write a line.
        if (MayBeCaptionButton(root, pt)) {
            LogDetectionMiss(root, pt);
        } else {
            TraceGate(root, pt, L"cursor outside the caption button region");
        }
        return;
    }

    lastMissWindow_ = nullptr;  // the window works, forget the complaint
    hit_ = info;
    state_ = State::Armed;
    WRITE_DEBUG_LOG(log::dformat(L"Minimize button detected {}x{} via {}",
                                 info.buttonRect.right - info.buttonRect.left,
                                 info.buttonRect.bottom - info.buttonRect.top,
                                 ProbeSourceName(info.source)),
                    log::Describe(info.window), probeTime.ElapsedMs());
    ::SetTimer(hwnd_, kTimerHover,
               ConfigStore::Instance().current().hoverDelayMs, nullptr);
}

void AppController::LogDetectionMiss(HWND window, POINT pt) {
    const LRESULT code = HitTestCode(window, pt);
    if (window == lastMissWindow_ && code == lastMissHit_) return;  // already said

    lastMissWindow_ = window;
    lastMissHit_ = code;

    wchar_t cls[64] = {};
    ::GetClassNameW(window, cls, ARRAYSIZE(cls));

    const CaptionLayout layout = CaptionLayoutOf(window);
    RECT frame{};
    VisibleFrame(window, frame);

    RECT dwmBlock{};
    const bool haveBlock =
        DwmWindowAttribute(window, kDwmCaptionButtonBounds, &dwmBlock, sizeof(dwmBlock));

    WRITE_DEBUG_LOG(
        log::dformat(L"In the caption button region, but no button: {} answered NCHITTEST {}",
                     cls, code),
        log::dformat(L"cursor {},{}  frame {},{},{},{}  style {:#x} exstyle {:#x}  slots {}  "
                     L"DWM block {}",
                     pt.x, pt.y, frame.left, frame.top, frame.right, frame.bottom,
                     static_cast<unsigned long long>(::GetWindowLongPtrW(window, GWL_STYLE)),
                     static_cast<unsigned long long>(::GetWindowLongPtrW(window, GWL_EXSTYLE)),
                     layout.slots(),
                     haveBlock ? log::dformat(L"{},{},{},{}", dwmBlock.left, dwmBlock.top,
                                              dwmBlock.right, dwmBlock.bottom)
                               : std::wstring(L"none")));
}

void AppController::HandleMouseDown() {
    if (state_ == State::Idle) return;

    POINT pt{};
    ::GetCursorPos(&pt);
    if (state_ == State::Open && PtInRectPt(flyout_.ScreenRect(), pt)) {
        return;  // click on an item - the flyout reports it itself
    }
    // Click on the button itself or anywhere else: cancel.
    if (state_ == State::Armed) ::KillTimer(hwnd_, kTimerHover);
    CloseFlyout(/*suppressUntilLeave=*/true);
}

void AppController::WriteDiagnosis() {
    const std::wstring report = DiagnoseWindowUnderCursor();

    std::wstring path;
    const bool wrote = AppendDiagnosisFile(report, path);
    const bool copied = CopyToClipboard(report);

    // The balloon has to work in a plain release build too, so it says what
    // happened instead of relying on the log.
    std::wstring text;
    text += copied ? L"Copied to the clipboard." : L"Clipboard was not available.";
    if (wrote) {
        text += L"\nAppended to ";
        text += path;
    } else if (!path.empty()) {
        text += L"\nCould not write ";
        text += path;
    }
    ShowTrayBalloon(L"MinFlyout diagnosis", text, !copied && !wrote);

    WRITE_INFO_LOG(L"Diagnosis written", report);
}

void AppController::OpenDiagnosisFile() {
    // Writing an entry first guarantees the file exists and shows the current
    // state even if the hotkey has never been pressed.
    std::wstring path;
    if (!AppendDiagnosisFile(DiagnoseWindowUnderCursor(), path) || path.empty()) {
        ShowTrayBalloon(L"MinFlyout", L"The diagnosis file could not be written.", true);
        return;
    }
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppController::TraceGate(HWND window, POINT pt, const wchar_t* gate) {
    // Without the trace flag every window still gets one line the first time it
    // is rejected. That is bounded by the number of windows on the desktop and
    // is what answers "why does this app never even get probed?" without
    // anybody having to switch anything on first.
    if (!traceDetection_) {
        for (const auto& seen : tracedWindows_) {
            if (seen.first == window && seen.second == gate) return;
        }
        tracedWindows_[tracedNext_] = {window, gate};
        tracedNext_ = (tracedNext_ + 1) % tracedWindows_.size();
    } else {
        // One line per change, not per mouse movement - otherwise a single
        // sweep across the desktop buries everything else.
        if (window == lastTraceWindow_ && gate == lastTraceGate_) return;
        lastTraceWindow_ = window;
        lastTraceGate_ = gate;
    }

    wchar_t cls[64] = L"-";
    if (window) ::GetClassNameW(window, cls, ARRAYSIZE(cls));

    const CaptionLayout layout = CaptionLayoutOf(window);
    RECT frame{};
    VisibleFrame(window, frame);

    WRITE_DEBUG_LOG(log::dformat(L"Trace: {}  ({})", gate, cls),
                    log::dformat(L"cursor {},{}  frame {},{},{},{}  slots {}",
                                 pt.x, pt.y, frame.left, frame.top,
                                 frame.right, frame.bottom, layout.slots()));
}

void AppController::CheckHookAlive() {
    POINT pt{};
    if (!::GetCursorPos(&pt)) return;

    const bool cursorMoved = (pt.x != watchdogPt_.x || pt.y != watchdogPt_.y);
    watchdogPt_ = pt;

    const unsigned long long seen = HookThread::MovesSeen();
    const bool hookFired = (seen != watchdogMoves_);
    watchdogMoves_ = seen;

    // The cursor moved but the hook did not report it: either Windows removed
    // the hook after a LowLevelHooksTimeout - which is what a debugger
    // breakpoint does to any process - or the throttle flag is stuck because a
    // posted message went missing. Both are silent and both are permanent
    // without this.
    if (!cursorMoved || hookFired || paused_) {
        watchdogStrikes_ = 0;
        return;
    }

    if (++watchdogStrikes_ < kWatchdogStrikes) return;
    watchdogStrikes_ = 0;

    WRITE_WARNING_LOG(
        L"Cursor moves but the hook reports nothing - reviving",
        log::dformat(L"moves seen {}, posted {}, pending {}, installed {}",
                     HookThread::MovesSeen(), HookThread::MovesPosted(),
                     HookThread::MovePending() ? 1 : 0,
                     HookThread::Installed() ? 1 : 0));
    hooks_.Revive();
}

void AppController::OpenFlyout() {
    if (!::IsWindow(hit_.window) || ::IsIconic(hit_.window)) {
        state_ = State::Idle;
        return;
    }

    POINT pt{};
    ::GetCursorPos(&pt);
    if (!PtInRectPt(InflateCopy(hit_.buttonRect, 2, 2), pt)) {
        state_ = State::Idle;
        return;
    }

    ctx_ = Context{};
    ctx_.targetWindow = hit_.window;
    ctx_.targetThreadId = ::GetWindowThreadProcessId(hit_.window, &ctx_.targetProcessId);
    ctx_.buttonRect = hit_.buttonRect;
    ctx_.windowRect = hit_.windowRect;
    ctx_.dpi = DpiForWindowCompat(hit_.window);

    const Config& config = ConfigStore::Instance().current();
    const log::Stopwatch collectTime;

    FlyoutContent content;
    content.useWorkArea = config.useWorkArea;

    // A window without a sizing border cannot be moved into a zone - it gets
    // the text items only, so the monitor rows are not built at all.
    const std::vector<MonitorEntry> monitors = EnumerateMonitors();
    const size_t current = IndexOfMonitorFor(monitors, hit_.window);
    size_t rowOfCurrent = monitors.size();  // "not among the rows"

    if (IsResizable(hit_.window)) {
        for (size_t m = 0; m < monitors.size(); ++m) {
            // Which screens to offer: all of them, or only the one the window
            // is on. And of those, only the ones a layout applies to.
            if (!config.showAllMonitors && m != current) continue;

            MonitorRow row;
            row.monitor = monitors[m];
            row.layouts = LayoutsForMonitor(config.layouts, monitors[m]);
            if (row.layouts.empty()) continue;

            if (m == current) rowOfCurrent = content.rows.size();
            content.rows.push_back(std::move(row));
        }
    }
    content.currentRow = std::min(rowOfCurrent, content.rows.size());

    ItemList list = Registry::Instance().Collect(ctx_);
    content.items = std::move(list.items());

    if (content.items.empty() && content.rows.empty()) {
        WRITE_WARNING_LOG(L"Nothing to show", log::Describe(hit_.window));
        state_ = State::Idle;
        return;
    }

    const size_t itemCount = content.items.size();
    const size_t rowCount = content.rows.size();
    flyout_.Show(std::move(content), hit_.buttonRect, ctx_.dpi);
    state_ = State::Open;
    hooks_.SetKeyboardHookEnabled(true);
    WRITE_INFO_LOG(log::dformat(L"Flyout opened: {} zones on {} of {} monitors, {} items, {} dpi",
                                flyout_.hotspots().size(), rowCount, monitors.size(),
                                itemCount, ctx_.dpi),
                   log::Describe(ctx_.targetWindow), collectTime.ElapsedMs());
}

void AppController::CloseFlyout(bool suppressUntilLeave) {
    if (graceRunning_) {
        ::KillTimer(hwnd_, kTimerGrace);
        graceRunning_ = false;
    }
    if (state_ == State::Armed) ::KillTimer(hwnd_, kTimerHover);

    flyout_.Hide();
    hooks_.SetKeyboardHookEnabled(false);
    state_ = State::Idle;
    suppressUntilLeave_ = suppressUntilLeave;
}

void AppController::InvokeItem(size_t index) {
    if (state_ != State::Open) return;
    const std::vector<Item>& items = flyout_.items();
    if (index >= items.size()) return;

    // Copy: the flyout is closed immediately afterwards.
    std::function<void(const Context&)> action = items[index].action;
    const std::wstring label = items[index].text;
    const Context ctx = ctx_;

    CloseFlyout(/*suppressUntilLeave=*/true);
    if (!action) return;

    const log::Stopwatch actionTime;
    action(ctx);
    WRITE_INFO_LOG(log::dformat(L"Item invoked: {}", label),
                   log::Describe(ctx.targetWindow), actionTime.ElapsedMs());
}

void AppController::InvokeZone(size_t index) {
    if (state_ != State::Open) return;
    const std::vector<ZoneHotspot>& spots = flyout_.hotspots();
    if (index >= spots.size()) return;

    const ZoneHotspot spot = spots[index];
    const Context ctx = ctx_;
    const bool useWorkArea = ConfigStore::Instance().current().useWorkArea;

    CloseFlyout(/*suppressUntilLeave=*/true);

    const log::Stopwatch actionTime;
    ApplyZone(ctx.targetWindow, spot.zone, spot.monitor, useWorkArea);
    WRITE_INFO_LOG(log::dformat(L"Zone invoked: {:.0f},{:.0f} {:.0f}x{:.0f} %",
                                spot.zone.left, spot.zone.top,
                                spot.zone.width, spot.zone.height),
                   log::Describe(ctx.targetWindow), actionTime.ElapsedMs());
}

// --- Tray icon of the application ------------------------------------------

void AppController::AddAppTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kAppTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_MFLY_TRAY;
    nid.hIcon = trayIcon_ ? trayIcon_ : ::LoadIconW(nullptr, IDI_APPLICATION);
    // The tooltip is the only place the double click and the hotkey can be
    // discovered - and the build stamp answers "am I running what I just built?"
    // without opening a single file.
    wchar_t tip[128] = {};
    ::swprintf(tip, ARRAYSIZE(tip),
               L"MinFlyout  %s\nDouble click reloads the configuration"
               L"\nCtrl+Alt+F12: diagnose window",
               BuildStamp());
    ::lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    ::Shell_NotifyIconW(NIM_ADD, &nid);
}

void AppController::RemoveAppTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kAppTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

void AppController::SetPaused(bool paused) {
    paused_ = paused;
    hooks_.SetPaused(paused);
    if (paused_) CloseFlyout(false);
    WRITE_INFO_LOG(paused ? L"Detection paused" : L"Detection resumed");
}

void AppController::ReloadConfig() {
    const bool ok = ConfigStore::Instance().Reload();
    const Config& config = ConfigStore::Instance().current();
    log::SetFileLogging(config.logToFile);
    probeMode_ = ProbeMode::Auto;
    ParseProbeMode(config.buttonDetection, probeMode_);
    traceDetection_ = config.traceDetection;

    if (ok) {
        wchar_t text[128] = {};
        ::swprintf(text, ARRAYSIZE(text), L"Loaded %zu layouts.",
                   config.layouts.size());
        ShowTrayBalloon(L"MinFlyout", text, false);
        WRITE_INFO_LOG(log::dformat(L"Configuration reloaded, {} layouts",
                                    config.layouts.size()), config.path);
    } else {
        ShowTrayBalloon(L"Configuration error", config.error, true);
        WRITE_WARNING_LOG(L"Configuration reload failed", config.error);
    }
}

void AppController::ShowTrayBalloon(const wchar_t* title, const std::wstring& text,
                                    bool error) const {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kAppTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    ::lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    ::lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AppController::ShowTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kMenuPause, L"Paused");
    ::AppendMenuW(menu, MF_STRING | (TrayStash::Instance().empty() ? MF_GRAYED : 0),
                  kMenuRestoreAll, L"Restore all stashed windows");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"Open configuration");
    ::AppendMenuW(menu, MF_STRING, kMenuReloadConfig, L"Reload configuration");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuOpenDiagnosis,
                  L"Open diagnosis file\t Ctrl+Alt+F12 writes it");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    // Drawn in bold, and it is what a double click on the icon triggers.
    ::SetMenuDefaultItem(menu, kMenuOpenConfig, FALSE);

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(hwnd_);  // so that the menu closes correctly
    ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
}

// --- Window procedure ------------------------------------------------------

LRESULT CALLBACK AppController::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<AppController*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_NCCREATE) self->hwnd_ = hwnd;
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT AppController::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MFLY_MOUSEMOVE:
        HandleMouseMove();
        return 0;

    case WM_MFLY_MOUSEDOWN:
        HandleMouseDown();
        return 0;

    case WM_MFLY_CANCEL:
        CloseFlyout(/*suppressUntilLeave=*/true);
        return 0;

    case WM_MFLY_INVOKE:
        InvokeItem(static_cast<size_t>(wParam));
        return 0;

    case WM_MFLY_ZONE:
        InvokeZone(static_cast<size_t>(wParam));
        return 0;

    case WM_MFLY_CLOSED:
        CloseFlyout(false);
        return 0;

    case WM_MFLY_TRAY:
        if (static_cast<UINT>(wParam) == kAppTrayId) {
            const UINT event = static_cast<UINT>(LOWORD(lParam));
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                ShowTrayMenu();
            } else if (event == WM_LBUTTONDBLCLK) {
                // Double click is the default action of the icon - the same
                // command the menu marks as default.
                ConfigStore::Instance().OpenInEditor();
            }
        } else {
            TrayStash::Instance().OnTrayMessage(wParam, lParam);
        }
        return 0;

    case WM_HOTKEY:
        if (static_cast<int>(wParam) == kHotkeyDiagnose) WriteDiagnosis();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuPause:        SetPaused(!paused_); return 0;
        case kMenuRestoreAll:   TrayStash::Instance().RestoreAll(); return 0;
        case kMenuOpenConfig:   ConfigStore::Instance().OpenInEditor(); return 0;
        case kMenuReloadConfig: ReloadConfig(); return 0;
        case kMenuOpenDiagnosis: OpenDiagnosisFile(); return 0;
        case kMenuExit:         ::PostQuitMessage(0); return 0;
        default: break;
        }
        break;

    case WM_TIMER:
        switch (wParam) {
        case kTimerHover:
            ::KillTimer(hwnd_, kTimerHover);
            if (state_ == State::Armed) OpenFlyout();
            return 0;
        case kTimerGrace:
            ::KillTimer(hwnd_, kTimerGrace);
            graceRunning_ = false;
            CloseFlyout(false);
            return 0;
        case kTimerPoll:
            TrayStash::Instance().DropDeadWindows();
            CheckHookAlive();
            if (state_ == State::Open &&
                (!::IsWindow(hit_.window) || ::IsIconic(hit_.window))) {
                CloseFlyout(false);
            }
            return 0;
        default:
            break;
        }
        break;

    case WM_QUERYENDSESSION:
        TrayStash::Instance().RestoreAll();
        return TRUE;

    case WM_ENDSESSION:
        TrayStash::Instance().RestoreAll();
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace mfly
