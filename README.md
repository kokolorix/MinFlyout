# MinFlyout

A system-wide flyout above the **minimize** button – the counterpart to the Snap Layouts
that Windows shows above the maximize button. Pure Win32/C++20, GDI rendering, no third-party
libraries. The flyout previews the monitors with their configured zones; the layouts
live in a JSONC file in the user profile and can be tied to particular screens.

## How it works

```
[hook thread]                 [controller UI thread]
WH_MOUSE_LL ──PostMessage──▶  HandleMouseMove()
(does NOTHING but post)        ├─ WindowFromPoint + GetAncestor(GA_ROOT)
                               ├─ ask:     WM_NCHITTEST == HTMINBUTTON
                               │           then edge search => button rect
                               │           (gated by nothing of ours)
                               ├─ else, and only inside the caption button
                               │  region: compute from the DWM block, the
                               │  title bar strip or the frame
                               ├─ hover delay  ──▶  monitors + layouts + items
                               └─ FlyoutWindow::Show(content, buttonRect, dpi)
```

Key points:

* **No DLL injection, no CBT hooks.** Nothing is loaded into a foreign process. Detection asks
  through `WM_NCHITTEST` via `SendMessageTimeout` (`SMTO_ABORTIFHUNG`), the documented
  interface that Windows itself uses to evaluate the caption buttons, and otherwise reads
  public window data.
* **The button rect** is determined by searching outward from the cursor in all four
  directions for how far `HTMINBUTTON` still applies – first coarsely (4 px steps), then
  refined by binary search. This works regardless of theme, DPI and window border width.
* **And where nobody answers, the position is computed** instead – from the caption button
  block DWM reports, from the title bar strip, or in the end from the visible frame and the
  system caption height. See [How the button is located](#how-the-button-is-located).
* **Hooks run in their own thread** (`HookThread`). Low-level hooks are processed
  synchronously in the input path; if the same thread sent `SendMessageTimeout` to a hung
  window, the mouse would stutter system-wide and Windows would disable the hook after
  `LowLevelHooksTimeout`. The callbacks therefore do nothing but `PostMessage`.
* **The flyout never activates itself** (`WS_EX_NOACTIVATE` + `MA_NOACTIVATE`); the target
  window keeps the focus.
* **Flush edges.** When a zone is applied, the invisible shadow border from
  `DWMWA_EXTENDED_FRAME_BOUNDS` is subtracted – otherwise a gap opens up between two windows
  placed side by side.
* **The target monitor is explicit.** A zone click carries the `HMONITOR` it was drawn for, so
  moving a window to another screen and sizing it there is a single `SetWindowPos`.

## Configuration

On first start `%APPDATA%\MinFlyout\config.jsonc` is created with a commented template.
The format is JSONC – comments and trailing commas are allowed:

```jsonc
{
  "hoverDelayMs": 350,       // hover delay before opening
  "closeGraceMs": 260,       // grace period before closing
  "showBuiltinItems": true,  // show minimize, notification area, ... below the layouts
  "useWorkArea": true,       // compute percentages without the taskbar
  "showAllMonitors": true,   // offer every screen, not just the current one
  "buttonDetection": "auto", // ask the window, compute when it stays silent

  "layouts": [
    {
      "name": "Halves",
      "zones": [
        { "left":  0, "top": 0, "width": 50, "height": 100 },
        { "left": 50, "top": 0, "width": 50, "height": 100 },
      ],
    },
    {
      "name": "Large + two",
      "monitors": [ "3840x2160" ],   // only offered on the wide screen
      "zones": [
        { "left":  0, "top":  0, "width": 66, "height": 100 },
        { "left": 66, "top":  0, "width": 34, "height":  50 },
        { "left": 66, "top": 50, "width": 34, "height":  50 },
      ],
    },
  ],
}
```

A **layout** is drawn as a miniature of the monitor, each of its **zones** as a tile inside
it. All four zone values are percentages of the monitor the miniature stands for.

| Field | Meaning |
|---|---|
| `name` | caption below the miniature |
| `zones` | the tiles: `left`, `top`, `width`, `height`, each in percent |
| `monitors` | which screens the layout is offered on – omit it for all of them |
| `showAllMonitors` | `true` draws one row per monitor, `false` only the current one |
| `useWorkArea` | `true` leaves the taskbar out of the reference area |

Zones are meant to tile the screen without overlapping, the way the Windows snap layouts do —
but nothing enforces that. If zones do overlap, the smallest one under the cursor wins, so a
small tile drawn on top of a large one stays reachable.

If the `layouts` section is missing entirely, five built-in layouts are used (halves, thirds,
large + two, quarters, full screen). An explicitly empty list `[]` hides the miniature area
and leaves only the text items.

### Writing a zone down without counting pixels

Guessing percentages for a zone is tedious, so there is a hotkey for it. Drag a window to
where it belongs, size it by hand, and press **Ctrl+Alt+F11** — the line

```jsonc
        { "left": 2.03, "top": 65.74, "width": 96.15, "height": 28.7 },
```

is on the clipboard, indented and with the trailing comma already in place, ready to be pasted
between two zones. A balloon shows what was copied. The tray menu entry *Copy zone of the
active window* does the same for the window that was in front when the menu opened, which is
the way out when Ctrl+Alt+F11 already belongs to something else.

Measured is the **foreground window**; if that one cannot be measured — our own flyout has the
focus, the window is minimized — the window under the cursor is used instead. What gets
measured is the *visible* frame, the same rectangle `ApplyZone` positions, so the invisible
shadow border cancels out and pasting the line back reproduces the window exactly. The
reference area follows `useWorkArea`, so the numbers mean the same thing they do everywhere
else in the file.

Values are rounded to two decimals with trailing zeros dropped, and clamped to the ranges the
parser accepts (`left`/`top` 0–99, `width`/`height` 1–100) — a window hanging over the edge of
its screen would otherwise produce a line the next load silently corrects.

### How the button is located

`buttonDetection` chooses how MinFlyout finds the minimize button of a foreign window.

| Value | Meaning |
|---|---|
| `"auto"` | ask the window; compute the position when it stays silent – the default |
| `"hittest"` | only ask – a window with its own title bar is then not detected |
| `"computed"` | never ask, always compute |

**Asking** sends `WM_NCHITTEST` and, when `HTMINBUTTON` comes back, measures the button by
probing outwards. That is exact, because the window itself draws the answer – but it needs a
window that answers. Standard Win32 windows do, and so do most Chromium-based apps, VS Code
among them.

It runs **first and unconditionally**. No assumption of ours stands in front of it: the region
test and the caption arithmetic below rest on how Windows normally lays a title bar out, and an
assumption that is wrong for some app would otherwise hide a window that answers perfectly well.
The region test guards the computed path only.

**Computing** needs nobody's cooperation. The caption buttons are not placed by taste: Windows
puts them in one block flush against the upper right corner of the visible frame, in a fixed
order, each slot equally wide. Three sources feed that, from precise to approximate:

1. `DWMWA_CAPTION_BUTTON_BOUNDS` – DWM reports the whole block, which is then divided into as
   many equal slots as the window styles call for.
2. `GetTitleBarInfo` – an API, not a message, so it also answers for a foreign process. It
   gives the exact title bar strip; only the button width is derived from its height.
3. The visible frame plus the system caption height – the last resort.

Which source produced a rectangle goes into the log on every detection. When none of them
does, the log notes the window class and the `HT` code the window returned, so a window that
stays undetected can be diagnosed instead of guessed about.

The sibling message `WM_GETTITLEBARINFOEX` would hand over the individual button rectangles in
one call, but it carries a pointer into the receiving window's address space and Windows is not
documented to marshal it between processes – so it is deliberately not used.

Which windows qualify for the **computed** path is decided by the styles alone: `WS_MINIMIZEBOX`
or `WS_MAXIMIZEBOX` on a non-child window. Neither `WS_CAPTION` nor `WS_SYSMENU` is required —
an app that draws its own title bar commonly drops both and keeps the boxes (VS Code reports
`0x14C70000`), and those are exactly the windows the computed path exists for. For the hit decision the computed rectangle gets a little slack downwards, because such an
app often draws its buttons a few pixels taller than the system does; sideways there is none,
that space belongs to the maximize button.

The computation assumes the system layout. An app that puts its caption buttons somewhere else
entirely — or sizes them quite differently — stays out of reach.

### When a window stays undetected

Point at the button that does not react and press **Ctrl+Alt+F12**. MinFlyout then walks the
whole decision chain for the window under the cursor and writes the result to the clipboard and
to `minflyout-diagnosis.txt` next to the configuration; a balloon confirms both. The tray menu
entry *Open diagnosis file* does the same and opens the file.

This works in a plain release build. The `WRITE_*_LOG` macros do not: they are compiled in only
with `_DEBUG` or `_RELEASE_WITH_DEBUG_LOG`, so `minflyout.log` stays empty in a release build no
matter what `logToFile` says. The diagnosis reports which of the two cases you are in on its
second line.

The report answers, in order: is the window on the ignore list, do its styles allow a minimize
button at all, is the cursor inside the caption button region, what does `WM_NCHITTEST` return,
what do `GetTitleBarInfo` and DWM report, where would the computed rectangle be — and a plain
verdict naming the step that failed. It also carries the build stamp, so a stale EXE gives
itself away immediately; the same stamp is in the tray tooltip.

`"traceDetection": true` adds one line per gate of the detection — which window is under the
cursor, whether it is on the ignore list, whether the cursor is inside the caption button
region — one line per change, not per movement. That is what answers "where does the mouse
movement die?" when nothing at all reaches the probe.

**When the hook goes quiet.** Two things silence the detection without any error showing up.
Windows removes a low-level hook whose owner did not answer within `LowLevelHooksTimeout` —
which happens to every process that sits at a debugger breakpoint. And the throttle that keeps
at most one movement message in the queue would stay armed forever if that message were ever
lost. A watchdog on the 400 ms poll timer now catches both: if the cursor moves for three ticks
running without the hook reporting anything, MinFlyout clears the flag and reinstalls the hook,
and says so in the log. The diagnosis reports the hook state in its header — how many movements
the callback has seen, how many were posted, whether one is still unacknowledged.

If you want to watch the raw message traffic of a foreign window, use **Spy++** from the Visual
Studio tools — it injects a hook into the target process, which MinFlyout deliberately does not.
Note that Spy++ comes in two bitnesses: `spyxx.exe` only sees 32-bit processes, and on a system
where everything is 64-bit it shows nothing at all. `spyxx_amd64.exe`, next to it in
`Common7\Tools\`, is the one to use — started elevated if the target runs elevated.

### Layouts for one screen

Thirds are useful on an ultrawide panel and cramped on a laptop screen, so a layout can name
the monitors it belongs on. `monitors` takes a single selector or a list of them; without the
key the layout shows up everywhere.

| Selector | Matches |
|---|---|
| `1`, `2`, … | the monitor number the flyout caption shows |
| `"primary"` | the primary screen |
| `"secondary"` | every screen that is not the primary one |
| `"DISPLAY2"` | the device name; in full it needs JSON escaping: `"\\\\.\\DISPLAY2"` |
| `"3840x2160"` | every screen with that resolution |
| `"*"` | every screen – the default |

Entries may be mixed: `"monitors": [1, "secondary"]`. Comparison ignores case and surrounding
blanks, and the resolution accepts `x`, `X` and `×`, so the caption can be pasted straight out
of the flyout.

With more than one screen every row carries that caption — number, device name, resolution and,
on the screen the window is on, *(current)*. Those are exactly the selectors above, so the
configuration can be written from what the flyout shows.

A screen that no layout applies to gets no row at all. That is also how a screen is excluded
entirely: give every layout a list that leaves it out.

### Editing it in VS Code

`res/config.schema.json` describes the whole file. Registered as a schema, VS Code completes
the keys, shows the descriptions on hover and flags typos such as `showAllMonitor`. Add this
to your **user** settings (*Preferences: Open User Settings (JSON)*) — if a `json.schemas`
array already exists, append the entry instead of replacing it:

```json
"json.schemas": [
  {
    "fileMatch": ["**/MinFlyout/config.jsonc"],
    "url": "d:/Projects/MinFlyout/res/config.schema.json"
  }
]
```

Forward slashes work in that path and save the escaping. Alternatively put
`"$schema": "d:/Projects/MinFlyout/res/config.schema.json"` as the first key into
`config.jsonc` — MinFlyout ignores unknown keys, so the file keeps working.

The trailing commas in the template stay marked with a warning: the JSONC mode accepts them,
but VS Code flags them anyway and offers no setting for that single diagnostic. The only
switch is `"json.validate.enable": false`, which would also disable the schema above.

If the file is malformed, the application keeps running with the default values and reports
line and column – in the flyout as a clickable item, on reload as a balloon on the tray icon.

### Saving is enough

The file is watched, so **saving it applies the changes** — no restart, no menu command. A
balloon on the tray icon confirms the reload, or names line and column if the file does not
parse. *Reload configuration* in the tray menu still does the same by hand.

The watch sits on the folder rather than on the file: editors that save by writing a
temporary file and renaming it over the original would otherwise slip through. Entries are
filtered by name (`minflyout.log` lives in the same folder), and 300 ms of silence must pass
before the reload runs, so one save is one reload and not three. `"watchConfig": false` turns
it off — worth knowing if `%APPDATA%` is redirected to a share that reports no changes.

## Structure

| File | Content |
|---|---|
| `src/HookThread.*` | WH_MOUSE_LL / WH_KEYBOARD_LL in a dedicated thread, only posts |
| `src/AppController.*` | state machine Idle → Armed → Open, timers, tray menu |
| `src/CaptionProbe.*` | the detection ladder: hit test, DWM block, title bar strip, estimate |
| `src/CaptionGeometry.*` | where the caption buttons have to be; free of Windows dependencies |
| `src/Diagnostics.*` | Ctrl+Alt+F12: the whole decision chain for one window, in plain text |
| `src/FlyoutWindow.*` | GDI popup: monitor miniatures, zone tiles, text items |
| `src/ItemRegistry.*` | provider registry, merging of the items |
| `src/BuiltinProviders.cpp` | the two providers: core actions and window groups |
| `src/Config.*` | loading/creating `config.jsonc`, schema, default values |
| `src/ConfigWatcher.*` | watches the folder, debounces, posts one message per save |
| `src/Json.*` | JSONC parser (comments, trailing commas), no third-party library |
| `src/Log.*` | `WRITE_*_LOG` macros, debugger output and optional log file |
| `src/Monitors.*` | enumerates the monitors shown in the flyout |
| `src/MonitorSelector.*` | which screens a layout applies to; free of Windows dependencies |
| `src/WindowSizer.*` | zone + monitor → screen coordinates and back, DWM frame correction |
| `src/TrayStash.*` | “minimize to notification area” including restore |
| `tests/JsonTest.cpp` | 40 checks for the parser, without a test framework |
| `tests/SelectorTest.cpp` | 33 checks for the monitor selectors |
| `tests/GeometryTest.cpp` | 57 checks for the caption button arithmetic |
| `docs/` | Doxygen main page, module groups, helper project `docs.vcxproj` |
| `res/` | application icon (`minflyout.ico`, `icon.svg`), resource file, manifest |
| `tools/build_icon.py` | generates the `.ico` from code – change motif or color here |

## Application icon

Motif: the minimize stroke, with the unfolding flyout below it. The `.ico` contains nine
sizes (16–256 px); the small ones are **not** downscaled but drawn separately – two instead
of three menu rows and snapped to whole pixels, so the edges stay crisp at 16 px instead of
becoming blurry.

```
python tools\build_icon.py      # writes out\minflyout.ico + preview
copy out\minflyout.ico res\
```

`res/icon.svg` is the editable template of the large size; colors are at the top of both
files (`#3B82F6` → `#1D4ED8`). The icon is bound in via `res/minflyout.rc` (together with the
version info). From there Windows automatically picks the matching size for Explorer, taskbar
and notification area.

## Logging

```cpp
WRITE_INFO_LOG(L"Flyout opened");                                 // message only
WRITE_DEBUG_LOG(L"Button probed", probeTime.ElapsedMs());         // + duration
WRITE_WARNING_LOG(L"Config rejected", config.error);              // + detail
WRITE_ERROR_LOG(L"SetWindowPos failed", detail, elapsed.ElapsedMs());
```

The macros expand to a functor carrying level, `__FUNCTION__`, `__FILE__` and `__LINE__`, so
the call site stays short. Every line is tab separated and can be pasted into a spreadsheet:

```
time                     thread  level  duration  message                    function            file(line)          detail
2026-08-19 22:14:07.913  9284    DEBUG  3         Minimize button detected 46x32  mfly::AppController::HandleMouseMove  AppController.cpp(193)  'Untitled - Notepad' [Notepad] pid 5312
```

* Compiled in when `_DEBUG` or `_RELEASE_WITH_DEBUG_LOG` is defined; otherwise the macros
  discard their arguments **without evaluating them**, and the logging code disappears from
  the binary (≈100 KB smaller in the MinGW release build).
* Output always goes to `OutputDebugStringW` (visible in DebugView or the VS output window).
  `"logToFile": true` in the configuration additionally writes
  `%APPDATA%\MinFlyout\minflyout.log`, UTF-8 with BOM, rotated to `.log.1` at 1 MB.
* `mfly::log::Stopwatch` feeds the duration column from the performance counter;
  `mfly::log::Describe(hwnd)` renders a foreign window as `'title' [class] pid N`.
* Nothing is logged from the hook callbacks or from the pre-test that runs on every throttled
  mouse move — that would drown the log and slow down the input path.
* Every line has exactly eight columns and never leaves one empty (`-` stands in), so log
  viewers that read it as a delimiter separated file do not trip over gaps.

### LogViewPlus

Parser `DsvParser`, file name pattern `minflyout.log`, *Is RegEx* off, and these parser
arguments — the separators are real tab characters:

```
%d{yyyy-MM-dd HH:mm:ss.fff}	%t	%p	%s{Duration}	%m	%c	%s{Source}	%S{Detail}
```

LogViewPlus derives the delimiter from the pattern itself: the first non-space character that
is not part of a specifier — here the tab. The eight columns map like this:

| # | Content | Specifier | Why |
|---|---|---|---|
| 1 | `2026-08-19 22:14:07.913` | `%d{yyyy-MM-dd HH:mm:ss.fff}` | `fff` matches exactly three digits |
| 2 | thread id | `%t` | UI thread and hook thread are told apart by this |
| 3 | `DEBUG` `INFO` `WARN` `ERROR` | `%p` | the level names LogViewPlus knows |
| 4 | duration in ms, or `-` | `%s{Duration}` | own column, sortable |
| 5 | message | `%m` | may contain spaces |
| 6 | function | `%c` | mapped to the logger column, so filtering by origin works |
| 7 | `AppController.cpp(193)` | `%s{Source}` | no spaces, own column |
| 8 | detail, or `-` | `%S{Detail}` | window titles contain spaces |

`tools/minflyout-sample.log` holds ten representative lines — one per level and one per
overload — for the *Testing* tab, so the parser can be checked without running the program.

## Building

**Visual Studio 2022** – open `MinFlyout.sln`, pick a configuration, F7.

* Projects: `minflyout` (EXE), `json_tests` (the unit tests for the JSONC parser and the
  monitor selectors and the caption
  geometry, run immediately after building; a failing test breaks the build) and
  `docs` (generates the Doxygen documentation; deliberately not part of the default build).
* Platforms: `x64` and `ARM64`, each Debug/Release.
* Output: `build\<platform>\<configuration>\minflyout.exe`.
* Release links the **static CRT** (`/MT`), so the EXE runs without the VC redist.
* Shared settings live in `MinFlyout.props` – change them there, not in the individual
  projects. `/utf-8` is not optional: the sources contain UTF-8 literals.

**CMake** (alternative, same sources):

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release        # unit tests
cmake --build build --target docs        # documentation
```

**MinGW-w64:**

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Documentation

`doxygen` in the project directory generates `build/doxygen-html/index.html` (or build the
`docs` project in Visual Studio). The configuration is set up for **zero warnings**:
`EXTRACT_ALL` stays off, `WARN_IF_UNDOCUMENTED` and `WARN_NO_PARAMDOC` are on, so missing
comments stand out. Warnings end up in `build/doxygen-warnings.log`.

Graphviz is not required; with `dot` installed, `HAVE_DOT = YES` additionally produces call
and dependency graphs.

## Usage

* Hold the mouse over the minimize button → the flyout unfolds below it.
* The upper part shows one row per monitor: a miniature per layout, in the aspect ratio of
  that screen. Clicking a tile puts the window on **that monitor, into that zone** — one click
  for screen and position together.
* Below the miniatures the text items follow (minimize, notification area, always on top, …).
* Leaving it, clicking elsewhere or `ESC` closes it again.
* Clicking the button itself minimizes as usual.
* A window without a sizing border (`WS_THICKFRAME`) cannot be tiled; for those the miniature
  area is omitted and only the text items appear.
* **Ctrl+Alt+F11** copies the zone of the active window as one ready-to-paste configuration
  line; **Ctrl+Alt+F12** writes the detection diagnosis for the window under the cursor.
* Tray icon: **double click reloads the configuration** (the default action, shown in bold in
  the menu). Right click opens the menu: *Paused*, *Restore all stashed windows*,
  *Open configuration*, *Reload configuration*, *Copy zone of the active window*,
  *Open diagnosis file*, *Exit*.

Items shipped alongside the configured sizes: Minimize · Minimize to notification area ·
Always on top · Minimize all windows of this app · Minimize other windows · Show desktop.
With `showBuiltinItems: false` only the size list remains.

## Known limits

* **UIPI:** windows of processes with a higher integrity level do not answer `WM_NCHITTEST`.
  The computed path still reaches them, because reading styles and frame needs no message –
  but for anything beyond detection (minimizing, sizing) MinFlyout itself has to run elevated
  (switch the manifest to `requireAdministrator` or start it as administrator).
* **Custom title bars:** a window that answers `WM_NCHITTEST` with `HTMINBUTTON` is measured
  exactly; for one that does not, the position is computed. The computation assumes the system
  layout – one block of equally wide buttons flush in the upper right corner – so an app that
  places its buttons somewhere else stays out of reach. `"buttonDetection"` selects which way
  is used, and every failed detection lands in the log with the window class and the `HT` code
  the window returned.
* **Non-resizable windows** (without `WS_THICKFRAME`) show the size items greyed out.
* Hidden foreign windows (`TrayStash`) are restored on `WM_QUERYENDSESSION`, on exit and when
  the process disappears – a crash of MinFlyout, on the other hand, would leave them behind
  invisible.
