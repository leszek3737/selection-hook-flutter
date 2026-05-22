# Platform Notes

## macOS (fully ported)

### Permissions
- **Accessibility**: Required for AXAPI text selection. App must be listed in
  System Settings → Privacy & Security → Accessibility.
- **Input Monitoring**: Not required. CGEventTap uses `kCGEventTapOptionListenOnly`
  (passive monitoring), not interception.
- **Sandbox**: Must be disabled (`com.apple.security.app-sandbox = false`).
  CGEventTap does not function inside an App Sandbox.

### Build
- **Deployment target**: macOS 10.14+
- **Frameworks**: ApplicationServices, Cocoa
- **C++ standard**: C++17 with libc++
- **Entitlements**: `example/macos/Runner/DebugProfile.entitlements` and
  `Release.entitlements` — set `com.apple.security.app-sandbox` to `false`.
- **Library validation**: Must be disabled (`com.apple.security.cs.disable-library-validation = true`)
  in Release entitlements for FFI dylib loading.

### Architecture
- CGEventTap captures mouse/keyboard events on a dedicated CFRunLoop thread.
- Events dispatched to main dispatch queue via `dispatch_async`.
- AXAPI queries run on main thread (safe for AppKit/NSWorkspace).
- Selection detection: drag (>=8px), double-click (<=500ms, <=3px), shift+click.
- I-beam cursor validation at mouse-down and mouse-up.

### Known limitations
- Clipboard fallback delays up to 100ms on main thread (rare, only when AXAPI fails).
- I-beam cursor detection uses fixed hotspot coordinates — may break on future
  macOS cursor theme changes. macOS 26 hotspot `{12,11}` already accounted for.

---

## Windows (fully ported)

### Status
**Fully ported.** All native code ported from upstream — `selection_hook_core.{h,cc}`
(1666 lines), `c_api_win.cc` bridge (287 lines), all 4 lib helpers complete.
CMakeLists.txt has real sources and link libraries.

### Remaining work
- Flutter plugin Windows CMakeLists.txt scaffold (the `windows/` directory at plugin root).
- Testing on actual Windows hardware.

### Architecture
- `WH_MOUSE_LL` + `WH_KEYBOARD_LL` hooks on dedicated thread via `SetWindowsHookEx`.
- Custom window messages (`WM_SH_MOUSE_EVENT`, `WM_SH_KEYBOARD_EVENT`) posted via
  `PostThreadMessage` for safe processing on the hook thread.
- Three text extraction strategies:
  1. **UIAutomation** — `IUIAutomation` with TextPattern + `IsSelectionActivePropertyId`
     + `LegacyIAccessible` fallback.
  2. **MSAA/Accessible** — direct `AccessibleObjectFromWindow`.
  3. **Clipboard fallback** — backup/restore with Ctrl+Insert then Ctrl+C, delay-read
     list support, keyboard interrupt detection.
- Selection detection: drag (>=8px), double-click (uses `GetDoubleClickTime()`), shift+click.
- DPI awareness: dynamic `SetProcessDpiAwareness` via Shcore.dll with fallback.
- Fullscreen detection: `SHQueryUserNotificationState` checked every 10 seconds.

### Build
- **Toolchain**: Visual Studio with C++ workload, CMake >= 3.14.
- **Link libraries**: `ole32`, `oleaut32`, `user32`, `gdi32`, `oleacc`,
  `UIAutomationCore`, `shell32`.
- **COM**: `CoInitializeEx` with `COINIT_MULTITHREADED`.
- **String conversion**: `StringPool` class with pooled buffers for WideToUtf8/Utf8ToWide.

### Known issues
- Clipboard fallback uses Ctrl+C injection — clipboard backup/restore logic mitigates
  interference but is not instantaneous.

---

## Linux X11 (substantially ported)

### Status
**X11 fully ported.** `selection_hook_core.{h,cc}` (120+732 lines), `c_api_linux.cc`
bridge (260 lines), X11 protocol (865 lines), keyboard and utils helpers complete.
CMakeLists.txt has X11 sources and link libraries.

### Wayland status
Wayland protocol code exists (`src/linux/protocols/wayland.cc`, 2181 lines) with
support for `ext-data-control-v1` and `wlr-data-control-unstable-v1` v2+, but:
- Generated Wayland protocol headers (`wayland/ext-data-control-v1-client.h`,
  `wlr-data-control-unstable-v1-client.h`) are **missing** from disk.
- `wayland.cc` is **not listed** in CMakeLists.txt.
- Link libraries (`wayland-client`, `libevdev`) not in CMakeLists.txt.
- `selection_hook_core.cc` hardcodes `CreateX11Protocol()` — display protocol
  auto-detection (`DetectDisplayProtocol()`) is declared but not implemented.

### Architecture (X11)
- `XRecord` extension for mouse/keyboard input monitoring on dedicated thread.
- `XFixes` extension for PRIMARY selection change detection on separate thread.
- `XGetWindowProperty` / `WM_CLASS` for active window and program name.
- `XConvertSelection` for reading PRIMARY selection text.
- Modifier state tracking for keyboard events.
- Selection detection: drag (>=8px), double-click (<=500ms, <=3px), shift+click.
- Selection event correlation window: 500ms.

### Build (X11)
- **Link libraries**: `X11`, `Xfixes`, `Xtst`.
- **Wayland (when enabled)**: `wayland-client`, `libevdev`, `libdbus-1`.

### Dependencies
```
# Debian/Ubuntu
sudo apt install libx11-dev libxfixes-dev libxtst-dev

# Wayland (when enabled)
sudo apt install libwayland-dev libevdev-dev libdbus-1-dev wayland-protocols
```

### Build notes
- Wayland protocol `.c` headers should be generated from XML via `wayland-scanner`.
- Linux Wayland has coordinate limitations: `startTop`/`startBottom`/
  `endTop`/`endBottom` always `INVALID_COORDINATE` (-99999). Mouse coordinates
  may also be unavailable depending on compositor.

### Display protocol detection
- X11 detected via `DISPLAY` env var.
- Wayland detected via `WAYLAND_DISPLAY` env var.
- Compositor type via `XDG_CURRENT_DESKTOP` and compositor-specific env vars
  (`HYPRLAND_INSTANCE_SIGNATURE`, `SWAYSOCK`).

### Clipboard
- `sh_write_clipboard` / `sh_read_clipboard` return errors on Linux (not implemented).
