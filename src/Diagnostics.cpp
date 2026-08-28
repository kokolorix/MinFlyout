/**
 * \file Diagnostics.cpp
 * \ingroup detect
 * \brief Implementation of the detection diagnosis.
 */
#include "Diagnostics.h"

#include <shlobj.h>

#include <format>

#include "CaptionProbe.h"
#include "Config.h"
#include "HookThread.h"

namespace mfly {
namespace {

/// One style bit and its name.
struct StyleBit {
    DWORD bit;            ///< The bit itself.
    const wchar_t* name;  ///< Name without the \c WS_ prefix stripped.
};

/// The \c WS_* bits worth naming for a caption.
constexpr StyleBit kStyles[] = {
    {WS_POPUP, L"WS_POPUP"},           {WS_CHILD, L"WS_CHILD"},
    {WS_CAPTION & ~WS_BORDER, L"WS_DLGFRAME"},
    {WS_BORDER, L"WS_BORDER"},         {WS_SYSMENU, L"WS_SYSMENU"},
    {WS_THICKFRAME, L"WS_THICKFRAME"}, {WS_MINIMIZEBOX, L"WS_MINIMIZEBOX"},
    {WS_MAXIMIZEBOX, L"WS_MAXIMIZEBOX"},
    {WS_VISIBLE, L"WS_VISIBLE"},       {WS_DISABLED, L"WS_DISABLED"},
};

/// The \c WS_EX_* bits that matter for the detection.
constexpr StyleBit kExStyles[] = {
    {WS_EX_DLGMODALFRAME, L"WS_EX_DLGMODALFRAME"},
    {WS_EX_TOOLWINDOW, L"WS_EX_TOOLWINDOW"},
    {WS_EX_WINDOWEDGE, L"WS_EX_WINDOWEDGE"},
    {WS_EX_CLIENTEDGE, L"WS_EX_CLIENTEDGE"},
    {WS_EX_CONTEXTHELP, L"WS_EX_CONTEXTHELP"},
    {WS_EX_TOPMOST, L"WS_EX_TOPMOST"},
    {WS_EX_LAYERED, L"WS_EX_LAYERED"},
    {WS_EX_LAYOUTRTL, L"WS_EX_LAYOUTRTL"},
    {WS_EX_NOREDIRECTIONBITMAP, L"WS_EX_NOREDIRECTIONBITMAP"},
    {WS_EX_NOACTIVATE, L"WS_EX_NOACTIVATE"},
};

/**
 * \brief Lists the set bits of a style word by name.
 * \param value Style word.
 * \param table Table of known bits.
 * \param count Number of entries in \p table.
 * \return The names separated by blanks, or \c "-" if nothing is set.
 */
std::wstring NameBits(DWORD value, const StyleBit* table, size_t count) {
    std::wstring out;
    for (size_t i = 0; i < count; ++i) {
        if ((value & table[i].bit) != table[i].bit) continue;
        if (!out.empty()) out += L' ';
        out += table[i].name;
    }
    return out.empty() ? std::wstring(L"-") : out;
}

/**
 * \brief Name of an \c HT* hit-test code.
 * \param code The code as returned by \c WM_NCHITTEST.
 * \return A static name, or \c "HT?" for anything unexpected.
 */
const wchar_t* HitTestName(LRESULT code) {
    switch (code) {
    case HTERROR:       return L"HTERROR";
    case HTTRANSPARENT: return L"HTTRANSPARENT";
    case HTNOWHERE:     return L"HTNOWHERE (or no answer at all)";
    case HTCLIENT:      return L"HTCLIENT";
    case HTCAPTION:     return L"HTCAPTION";
    case HTSYSMENU:     return L"HTSYSMENU";
    case HTGROWBOX:     return L"HTGROWBOX";
    case HTMENU:        return L"HTMENU";
    case HTMINBUTTON:   return L"HTMINBUTTON";
    case HTMAXBUTTON:   return L"HTMAXBUTTON";
    case HTLEFT:        return L"HTLEFT";
    case HTRIGHT:       return L"HTRIGHT";
    case HTTOP:         return L"HTTOP";
    case HTTOPLEFT:     return L"HTTOPLEFT";
    case HTTOPRIGHT:    return L"HTTOPRIGHT";
    case HTBOTTOM:      return L"HTBOTTOM";
    case HTBOTTOMLEFT:  return L"HTBOTTOMLEFT";
    case HTBOTTOMRIGHT: return L"HTBOTTOMRIGHT";
    case HTBORDER:      return L"HTBORDER";
    case HTCLOSE:       return L"HTCLOSE";
    case HTHELP:        return L"HTHELP";
    default:            return L"HT?";
    }
}

/// Formats a rectangle as \c left,top,right,bottom plus its size.
std::wstring Fmt(const RECT& r) {
    return std::format(L"{},{},{},{}  ({}x{})", r.left, r.top, r.right, r.bottom,
                       r.right - r.left, r.bottom - r.top);
}

/// Formats a \ref RectI the same way.
std::wstring Fmt(const RectI& r) {
    return std::format(L"{},{},{},{}  ({}x{})", r.left, r.top, r.right, r.bottom,
                       r.width(), r.height());
}

/**
 * \brief Reads the window title, however long it is.
 * \param window Window to ask.
 * \return The title, or \c "-" when it has none.
 */
std::wstring TitleOf(HWND window) {
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0) return L"-";
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int written = ::GetWindowTextW(window, text.data(), length + 1);
    text.resize(written > 0 ? static_cast<size_t>(written) : 0);
    return text.empty() ? std::wstring(L"-") : text;
}

/**
 * \brief Determines the executable behind a window.
 * \param window Window to look at.
 * \return \c "pid name.exe", with the name replaced by a note if it is not readable.
 */
std::wstring ProcessOf(HWND window) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);

    std::wstring name = L"(not readable)";
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = ARRAYSIZE(path);
        if (::QueryFullProcessImageNameW(process, 0, path, &size)) {
            const wchar_t* leaf = ::wcsrchr(path, L'\\');
            name = leaf ? leaf + 1 : path;
        }
        ::CloseHandle(process);
    }
    return std::format(L"{}  {}", pid, name);
}

}  // namespace

const wchar_t* BuildStamp() {
    // __DATE__ and __TIME__ are narrow literals; widened once at startup.
    static const std::wstring stamp = [] {
        const char* date = __DATE__;
        const char* time = __TIME__;
        std::wstring out;
        for (const char* p = date; *p; ++p) out += static_cast<wchar_t>(*p);
        out += L' ';
        for (const char* p = time; *p; ++p) out += static_cast<wchar_t>(*p);
        return out;
    }();
    return stamp.c_str();
}

bool LoggingCompiledIn() {
#if defined(_DEBUG) || defined(_RELEASE_WITH_DEBUG_LOG)
    return true;
#else
    return false;
#endif
}

std::wstring DiagnoseWindow(HWND window, POINT pt) {
    std::wstring out;
    out += L"MinFlyout diagnosis\r\n";
    out += std::format(L"  build            {}\r\n", BuildStamp());
    out += std::format(L"  logging          {}\r\n",
                       LoggingCompiledIn()
                           ? L"compiled in"
                           : L"NOT compiled in - this is a release build without "
                             L"_RELEASE_WITH_DEBUG_LOG, so minflyout.log stays empty");

    const Config& config = ConfigStore::Instance().current();
    out += std::format(L"  configuration    {}\r\n",
                       config.path.empty() ? L"(unknown)" : config.path.c_str());
    out += std::format(L"  buttonDetection  {}\r\n", config.buttonDetection);
    out += std::format(L"  logToFile        {}\r\n", config.logToFile ? L"true" : L"false");
    if (config.hasError()) {
        out += std::format(L"  config error     {}\r\n", config.error);
    }
    out += std::format(L"  traceDetection   {}\r\n", config.traceDetection ? L"true" : L"false");

    // The single most important line when nothing at all happens: if the hook
    // stopped firing, no window is ever looked at and every value below is
    // beside the point.
    out += std::format(L"  mouse hook       {},  {} movements seen, {} posted, "
                       L"{} unacknowledged\r\n",
                       HookThread::Installed() ? L"installed" : L"NOT INSTALLED",
                       HookThread::MovesSeen(), HookThread::MovesPosted(),
                       HookThread::MovePending() ? L"one" : L"none");
    out += std::format(L"  cursor           {},{}\r\n\r\n", pt.x, pt.y);

    if (!window || !::IsWindow(window)) {
        out += L"window under cursor\r\n  none - there is no window at this position.\r\n";
        return out;
    }

    wchar_t cls[128] = {};
    ::GetClassNameW(window, cls, ARRAYSIZE(cls));
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE));

    RECT windowRect{};
    ::GetWindowRect(window, &windowRect);
    RECT frame{};
    const bool haveFrame = VisibleFrame(window, frame);
    // Two different numbers, and telling them apart is what explains a whole
    // class of failures: the caption arithmetic works in screen pixels, so it
    // uses the monitor's DPI, while a window that is not per-monitor aware
    // reports 96 whatever the screen does.
    const UINT dpi = DpiForWindowMonitor(window);
    const UINT windowDpi = DpiForWindowCompat(window);

    out += L"window under cursor\r\n";
    out += std::format(L"  hwnd             {:#x}\r\n",
                       reinterpret_cast<unsigned long long>(window));
    out += std::format(L"  class            {}\r\n", cls);
    out += std::format(L"  title            {}\r\n", TitleOf(window));
    out += std::format(L"  process          {}\r\n", ProcessOf(window));
    out += std::format(L"  style            {:#010x}  {}\r\n", style,
                       NameBits(style, kStyles, ARRAYSIZE(kStyles)));
    out += std::format(L"  exstyle          {:#010x}  {}\r\n", exStyle,
                       NameBits(exStyle, kExStyles, ARRAYSIZE(kExStyles)));
    out += std::format(L"  window rect      {}\r\n", Fmt(windowRect));
    out += std::format(L"  visible frame    {}{}\r\n", Fmt(frame),
                       haveFrame ? L"" : L"   (GetWindowRect - DWM had no answer)");
    out += std::format(L"  dpi (screen)     {}\r\n", dpi);
    out += std::format(L"  dpi (window)     {}{}\r\n", windowDpi,
                       windowDpi == dpi
                           ? L""
                           : L"   - the window is not per-monitor DPI aware; everything "
                             L"below is measured in screen pixels");
    out += std::format(L"  state            {}{}{}\r\n",
                       ::IsZoomed(window) ? L"maximized " : L"",
                       ::IsIconic(window) ? L"minimized " : L"",
                       ::IsWindowVisible(window) ? L"visible" : L"hidden");
    DWORD cloaked = 0;
    DwmWindowAttribute(window, kDwmCloaked, &cloaked, sizeof(cloaked));
    out += std::format(L"  cloaked          {:#x}{}\r\n", cloaked,
                       cloaked ? L"   (non-zero puts the window on the ignore list)" : L"");
    out += std::format(L"  ignored          {}\r\n\r\n",
                       IsIgnoredWindow(window) ? L"YES - detection skips this window"
                                               : L"no");

    // --- what the caption arithmetic makes of it ---------------------------
    const CaptionLayout layout = CaptionLayoutOf(window);
    const int titleBar = TitleBarHeight(dpi);

    out += L"caption\r\n";
    out += std::format(L"  slots            {}\r\n", layout.slots());
    int slot = 0;
    const bool hasMinimize = layout.slotOf(CaptionButton::Minimize, slot);
    out += std::format(L"  minimize slot    {}\r\n",
                       hasMinimize ? std::format(L"{}", slot)
                                   : std::wstring(L"none - this window shows no minimize button"));
    out += std::format(L"  title bar height {}   (SM_CYCAPTION {} + SM_CXPADDEDBORDER {} = {})\r\n",
                       titleBar, SystemMetricForDpi(SM_CYCAPTION, dpi),
                       SystemMetricForDpi(SM_CXPADDEDBORDER, dpi),
                       SystemMetricForDpi(SM_CYCAPTION, dpi) +
                           SystemMetricForDpi(SM_CXPADDEDBORDER, dpi));
    out += std::format(L"  button width     {}\r\n", CaptionButtonWidth(titleBar));

    const LRESULT code = HitTestCode(window, pt);
    out += std::format(L"  NCHITTEST        {}  {}\r\n", code, HitTestName(code));

    RECT strip{};
    const bool haveStrip = TitleBarStrip(window, strip);
    out += std::format(L"  GetTitleBarInfo  {}\r\n",
                       haveStrip ? Fmt(strip) : std::wstring(L"no title bar reported"));

    // The strip is the second source of the ladder, and the gate in front of it
    // is easy to misread from the outside: a window with its own title bar keeps
    // a vestigial system caption that is far too flat, and believing it would
    // put the buttons in the wrong place. Showing both the verdict and what the
    // strip would have produced turns "it does not work" into a rectangle that
    // can be compared with where the buttons visibly are.
    if (haveStrip) {
        const int stripHeight = strip.bottom - strip.top;
        const bool trusted = stripHeight * 4 >= titleBar * 3;
        out += std::format(L"  strip trusted    {}\r\n",
                           trusted
                               ? std::format(L"yes - {} px against the expected {}",
                                             stripHeight, titleBar)
                               : std::format(L"no - {} px is under 3/4 of the expected {}, "
                                             L"so it is taken for a vestigial caption",
                                             stripHeight, titleBar));

        RectI fromStrip;
        const RectI bar{strip.left, strip.top, strip.right, strip.bottom};
        if (hasMinimize &&
            CaptionButtonRect(EstimateCaptionBlock(bar, bar.height(), layout), layout,
                              CaptionButton::Minimize, fromStrip)) {
            out += std::format(L"  strip would give {}\r\n", Fmt(fromStrip));
        }
    }

    RECT rawBlock{};
    const bool haveRaw =
        DwmWindowAttribute(window, kDwmCaptionButtonBounds, &rawBlock, sizeof(rawBlock));
    out += std::format(L"  DWM block raw    {}\r\n",
                       haveRaw ? Fmt(rawBlock) : std::wstring(L"DwmGetWindowAttribute failed"));
    RECT block{};
    out += std::format(L"  DWM block used   {}\r\n",
                       CaptionBlockFromDwm(window, block)
                           ? Fmt(block)
                           : std::wstring(L"rejected as not plausible"));

    if (haveFrame) {
        out += std::format(L"  estimated block  {}\r\n",
                           Fmt(EstimateCaptionBlock(RectI{frame.left, frame.top,
                                                          frame.right, frame.bottom},
                                                    titleBar, layout)));
        out += std::format(L"  region           {}\r\n",
                           Fmt(CaptionButtonRegion(RectI{frame.left, frame.top,
                                                         frame.right, frame.bottom},
                                                   titleBar, layout)));
    }
    out += std::format(L"  in region        {}\r\n",
                       MayBeCaptionButton(window, pt) ? L"yes" : L"NO - the pre-filter stops here");

    RECT button{};
    ProbeSource source = ProbeSource::Estimate;
    const bool computed = ComputeMinimizeButton(window, layout, button, source);
    out += std::format(L"  computed button  {}\r\n",
                       computed ? std::format(L"{}   via {}", Fmt(button),
                                              ProbeSourceName(source))
                                : std::wstring(L"none of the three sources answered"));
    if (computed) {
        out += std::format(L"  cursor in button {}\r\n",
                           PtInRectPt(button, pt) ? L"yes" : L"no");
    }

    // --- verdict -----------------------------------------------------------
    out += L"\r\nverdict\r\n  ";
    if (IsIgnoredWindow(window)) {
        out += L"The window is on the ignore list (own window, shell window, cloaked or "
               L"invisible).";
    } else if (!hasMinimize) {
        out += L"The styles say this window has no minimize button, so nothing is looked for. "
               L"Check WS_SYSMENU and WS_MINIMIZEBOX above.";
    } else if (!MayBeCaptionButton(window, pt)) {
        out += L"The cursor is outside the caption button region, so the detection stops "
               L"before it asks anybody. Compare 'region' with 'cursor' - if the real buttons "
               L"are somewhere else, that is the reason.";
        if (windowDpi != dpi) {
            out += L" Note that the two DPI values differ: if 'region' also looks too small "
                   L"next to what DWM reports, the region was measured for the wrong scale.";
        }
    } else if (code == HTMINBUTTON) {
        out += L"The window reports HTMINBUTTON here - detection should work.";
    } else if (computed && PtInRectPt(button, pt)) {
        out += L"The window says nothing useful, but the computed rectangle contains the "
               L"cursor - the computed path should work.";
    } else if (computed) {
        out += L"The window says nothing useful and the cursor is not inside the computed "
               L"rectangle. Compare 'computed button' with 'cursor': the computation puts the "
               L"button somewhere other than the app draws it.";
    } else {
        out += L"Neither the window nor DWM nor the title bar info yields a rectangle.";
    }
    out += L"\r\n";
    return out;
}

std::wstring DiagnoseWindowUnderCursor() {
    POINT pt{};
    ::GetCursorPos(&pt);

    HWND under = ::WindowFromPoint(pt);
    HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    return DiagnoseWindow(root, pt);
}

bool CopyToClipboard(const std::wstring& text) {
    if (!::OpenClipboard(nullptr)) return false;

    bool ok = false;
    if (::EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* target = ::GlobalLock(handle)) {
                ::memcpy(target, text.c_str(), bytes);
                ::GlobalUnlock(handle);
                ok = ::SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
            }
            if (!ok) ::GlobalFree(handle);  // the clipboard did not take ownership
        }
    }
    ::CloseClipboard();
    return ok;
}

bool AppendDiagnosisFile(const std::wstring& text, std::wstring& path) {
    path = ConfigStore::FilePath();
    if (path.empty()) return false;

    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    path = path.substr(0, slash + 1) + L"minflyout-diagnosis.txt";

    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const std::wstring block = L"\r\n========================================\r\n" + text;
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, block.c_str(),
                                             static_cast<int>(block.size()),
                                             nullptr, 0, nullptr, nullptr);
    bool ok = false;
    if (needed > 0) {
        std::string utf8(static_cast<size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, block.c_str(), static_cast<int>(block.size()),
                              utf8.data(), needed, nullptr, nullptr);
        DWORD written = 0;
        ok = ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()),
                         &written, nullptr) != FALSE;
    }
    ::CloseHandle(file);
    return ok;
}

}  // namespace mfly
