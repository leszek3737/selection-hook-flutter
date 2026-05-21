# Platform Notes

## macOS (host platform — fully ported)

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

### Architecture
- CGEventTap captures mouse/keyboard events on a dedicated CFRunLoop thread.
- Events dispatched to main dispatch queue via `dispatch_async`.
- AXAPI queries run on main thread (safe for AppKit/NSWorkspace).
- Selection detection: drag (≥8px), double-click (≤500ms, ≤3px), shift+click.

### Known limitations
- Clipboard fallback delays up to 100ms on main thread (rare, only when AXAPI fails).
- I-beam cursor detection uses fixed hotspot coordinates — may break on future
  macOS cursor theme changes.

---

## Linux

### Status
**Stub only.** Helper sources copied from upstream (`src/linux/lib/`,
`src/linux/protocols/`), but the main `selection_hook.cc` has not been ported.

### What needs to happen
1. Port `_upstream/src/linux/selection_hook.cc` to `src/linux/selection_hook_core.{h,cc}`
   — strip N-API, replace ThreadSafeFunction with SHSelectionCallback.
2. Write `src/bridge/c_api_linux.cc` bridge (copy pattern from `c_api_mac.mm`).
3. Update `src/CMakeLists.txt` `elseif(LINUX)` block with real sources.

### Dependencies (deferred)
- X11: `libX11`, `libXfixes`, `libXi`, `libXtst`
- Wayland: `wayland-client`, `wayland-cursor`, `wayland-protocols` (for
  `ext-data-control-v1` and `wlr-data-control-unstable-v1`)

### Build notes
- Wayland protocol `.c` files generated from `.xml` are checked into
  `src/linux/protocols/wayland/`. No `wayland-scanner` needed at build time.
- Linux Wayland has coordinate limitations: `startTop`/`startBottom`/
  `endTop`/`endBottom` always `INVALID_COORDINATE` (-99999). Mouse coordinates
  may also be unavailable.

### Display protocol detection
- Upstream detects: X11 (DISPLAY env), Wayland (WAYLAND_DISPLAY env).
- Compositor type detected via XDG_CURRENT_DESKTOP and compositor-specific env vars
  (HYPRLAND_INSTANCE_SIGNATURE, SWAYSOCK).

---

## Windows

### Status
**Stub only.** Helper sources copied from upstream (`src/windows/lib/`),
but the main `selection_hook.cc` has not been ported.

### What needs to happen
1. Port `_upstream/src/windows/selection_hook.cc` to `src/windows/selection_hook_core.{h,cc}`
   — strip N-API, replace ThreadSafeFunction with SHSelectionCallback.
2. Write `src/bridge/c_api_win.cc` bridge (copy pattern from `c_api_mac.mm`).
3. Update `src/CMakeLists.txt` `elseif(WIN32)` block with real sources.

### Dependencies (deferred)
- `ole32`, `oleaut32` (COM for UI Automation)  
- `user32`, `gdi32` (window management)

### Build notes
- Requires Visual Studio with CMake 3.14+.
- UI Automation requires COM initialization (`CoInitializeEx` with
  `COINIT_MULTITHREADED`).

### Known issues
- Windows clipboard fallback uses Ctrl+C injection — may interfere with
  user clipboard if not careful. Upstream has clipboard backup/restore
  logic to mitigate this.
