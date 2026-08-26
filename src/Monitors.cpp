/**
 * \file Monitors.cpp
 * \ingroup ui
 * \brief Implementation of the monitor enumeration and of the layout filter.
 */
#include "Monitors.h"

#include <algorithm>

#include "MonitorSelector.h"

namespace mfly {
namespace {

/// Collector for \c EnumDisplayMonitors.
BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lp);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(handle, &info)) return TRUE;

    MonitorEntry entry;
    entry.handle = handle;
    entry.rect = info.rcMonitor;
    entry.work = info.rcWork;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    // szDevice is not guaranteed to be terminated when it fills the buffer.
    size_t length = 0;
    while (length < ARRAYSIZE(info.szDevice) && info.szDevice[length] != L'\0') ++length;
    entry.device.assign(info.szDevice, length);
    out->push_back(entry);
    return TRUE;
}

}  // namespace

std::wstring MonitorEntry::shortDevice() const {
    const size_t slash = device.find_last_of(L'\\');
    return slash == std::wstring::npos ? device : device.substr(slash + 1);
}

std::vector<MonitorEntry> EnumerateMonitors() {
    std::vector<MonitorEntry> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor,
                          reinterpret_cast<LPARAM>(&monitors));

    // Primary first, then left to right, then top to bottom - that is how
    // people read their own desk.
    std::sort(monitors.begin(), monitors.end(),
              [](const MonitorEntry& a, const MonitorEntry& b) {
                  if (a.primary != b.primary) return a.primary;
                  if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
                  return a.rect.top < b.rect.top;
              });

    for (size_t i = 0; i < monitors.size(); ++i) {
        monitors[i].index = static_cast<int>(i) + 1;
    }
    return monitors;
}

size_t IndexOfMonitorFor(const std::vector<MonitorEntry>& monitors, HWND window) {
    if (monitors.empty()) return 0;

    HMONITOR handle = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (monitors[i].handle == handle) return i;
    }
    return 0;
}

bool MonitorMatchesSelector(const MonitorEntry& monitor, const std::wstring& selector) {
    MonitorFacts facts;
    facts.index = monitor.index;
    facts.primary = monitor.primary;
    facts.device = monitor.device;
    facts.width = monitor.rect.right - monitor.rect.left;
    facts.height = monitor.rect.bottom - monitor.rect.top;
    return SelectorMatches(facts, selector);
}

bool LayoutAppliesTo(const Layout& layout, const MonitorEntry& monitor) {
    if (layout.everywhere()) return true;
    for (const std::wstring& selector : layout.monitors) {
        if (MonitorMatchesSelector(monitor, selector)) return true;
    }
    return false;
}

std::vector<Layout> LayoutsForMonitor(const std::vector<Layout>& layouts,
                                      const MonitorEntry& monitor) {
    std::vector<Layout> picked;
    for (const Layout& layout : layouts) {
        if (LayoutAppliesTo(layout, monitor)) picked.push_back(layout);
    }
    return picked;
}

}  // namespace mfly
