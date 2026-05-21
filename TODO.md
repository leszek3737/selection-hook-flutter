# Remaining Work: Linux & Windows Ports

## Current state

| Platform | Status |
|----------|--------|
| macOS | Fully ported — AXAPI + CGEventTap + clipboard fallback |
| Linux | Stub — helpers copied, real port pending |
| Windows | Stub — helpers copied, real port pending |

Both stubs compile via `src/bridge/c_api.cc` (synthetic test events only).

---

## Linux port

### Step 1: Port core from upstream

- Copy `_upstream/src/linux/selection_hook.cc` (~2100 lines) as starting point.
- Strip all N-API: remove `#include <napi.h>`, `Napi::ObjectWrap`, `Napi::ThreadSafeFunction`, `Napi::Env`, `NODE_API_MODULE`.
- Replace 4 ThreadSafeFunctions (`tsfn`, `mouse_tsfn`, `keyboard_tsfn`, `selection_tsfn`) with `SHSelectionCallback`, `SHMouseCallback`, `SHKeyboardCallback` function pointers.
- Preserve: X11 PRIMARY selection polling, keyboard event handling.
- Wayland support intentionally excluded — X11 only.
- Output: `src/linux/selection_hook_core.{h,cc}` (follow pattern from `src/mac/selection_hook_core.{h,mm}`).

### Step 2: Write bridge

- Create `src/bridge/c_api_linux.cc` — `extern "C"` implementations.
- Copy pattern from `src/bridge/c_api_mac.mm`.
- Struct: wrap `SelectionHookCore*` + callback pointers.
- Wire: `sh_create`, `sh_destroy`, `sh_start`, `sh_stop`, `sh_is_running`, `sh_get_current_selection`, `sh_free_selection_data`, `sh_set_selection_callback`, `sh_set_mouse_callback`, `sh_set_keyboard_callback`, `sh_set_config`, `sh_enable/disable_mouse_move`, `sh_set_passive_mode`, `sh_write/read_clipboard`, `sh_mac_*` → no-op on Linux.

### Step 3: Update CMake

- Replace `elseif(LINUX)` stub block in `src/CMakeLists.txt` with real sources:
  ```
  bridge/c_api_linux.cc
  linux/selection_hook_core.cc
  linux/lib/keyboard.cc
  linux/lib/utils.cc
  linux/protocols/x11.cc
  ```
- Add linker flags: `X11`, `Xfixes`, `Xi`, `Xtst`.

### Step 4: Test

- Build: `cd example && flutter build linux --debug`.
- Smoke test: `dart run tool/smoke_test.dart` → should see real selection events.
- Manual test: select text in terminal, browser, text editor — all must emit events with correct `program_name`.

### Linux-specific concerns

- `DISPLAY` env var required for X11 connection.
- Clipboard read/write not supported on Linux (per upstream). Return `SH_ERR_GENERIC` / `nullptr`.
- `startTop`/`startBottom`/`endTop`/`endBottom` always `INVALID_COORDINATE` (-99999) on Linux per upstream.

---

## Windows port

### Step 1: Port core from upstream

- Copy `_upstream/src/windows/selection_hook.cc` (~2000 lines) as starting point.
- Strip all N-API: remove `#include <napi.h>`, `Napi::ObjectWrap`, `Napi::ThreadSafeFunction`, `Napi::Env`, `NODE_API_MODULE`.
- Replace 3 ThreadSafeFunctions (`tsfn`, `mouse_tsfn`, `keyboard_tsfn`) with `SHSelectionCallback`, `SHMouseCallback`, `SHKeyboardCallback` function pointers.
- Preserve: Windows hook procedures (`SetWindowsHookEx` for mouse/keyboard), UI Automation text selection, clipboard fallback (Ctrl+C).
- Output: `src/windows/selection_hook_core.{h,cc}`.

### Step 2: Write bridge

- Create `src/bridge/c_api_win.cc` — `extern "C"` implementations.
- Copy pattern from `src/bridge/c_api_mac.mm`.
- Struct: wrap `SelectionHookCore*` + callback pointers + `read_clipboard_mutex` + `read_clipboard_buf`.
- Wire all functions from `c_api.h`. `sh_mac_*` → return 0 on Windows.

### Step 3: Update CMake

- Replace `elseif(WIN32)` stub block in `src/CMakeLists.txt` with real sources:
  ```
  bridge/c_api_win.cc
  windows/selection_hook_core.cc
  windows/lib/clipboard.cc
  windows/lib/keyboard.cc
  windows/lib/string_pool.cc
  windows/lib/utils.cc
  ```
- Add linker flags: `ole32`, `oleaut32`, `user32`, `gdi32`.

### Step 4: Test

- Build on Windows machine: `cd example && flutter build windows --debug`.
- Smoke test: `dart run tool/smoke_test.dart` → should see real selection events.
- Manual test: select text in Notepad, browser, VS Code — all must emit events.
- Test clipboard fallback: AXAPI-failing apps (Electron, UWP) must still work via Ctrl+C injection.

### Windows-specific concerns

- COM initialization: `CoInitializeEx(NULL, COINIT_MULTITHREADED)` needed before UI Automation calls.
- 64-bit process required — Windows hooks may not work from 32-bit.
- Build requires Visual Studio with "Desktop development with C++" workload.
- String pool (`src/windows/lib/string_pool.{h,cc}`) is a Windows-only pattern — manages string lifetimes for UIA callbacks.
- `vkCode` and `scanCode` differ from macOS/Linux — use Windows `VK_*` constants.

---

## Shared checklist (both platforms)

- [ ] Core class: strip N-API, add `SHSelectionCallback`/`SHMouseCallback`/`SHKeyboardCallback` members
- [ ] Core class: `start()`/`stop()`/`isRunning()`/`getCurrentSelection()`
- [ ] Core class: `dispatchSelection()` → fills `SHSelectionData` + calls callback (heap-backed strings, see macOS fix in `50b90dc`)
- [ ] Core class: `dispatchMouseEvent()` / `dispatchKeyboardEvent()` (heap-backed structs, see macOS fix in `789d127`)
- [ ] Core class: `processMouseEvent()` — drag/double-click/shift-click selection detection
- [ ] Bridge: `SelectionHook` struct with core pointer + callback pointers + error string
- [ ] Bridge: all `extern "C"` functions from `c_api.h`
- [ ] Bridge: `sh_get_current_selection` returns heap-allocated `SHSelectionData` with `strdup` (not shared cache — see fix in `72ae894`)
- [ ] Bridge: `dispatch_async`/hook equivalent guarded with `!hook->running` for use-after-free safety
- [ ] Bridge: `is_processing` uses `exchange(true)` (atomic check-and-set, see fix in `72ae894`)
- [ ] CMakeLists.txt: platform block with real sources + framework/lib links
- [ ] `flutter build` succeeds
- [ ] Smoke test passes
- [ ] Manual test: 3 different apps (browser, terminal, text editor)
- [ ] Update `PLATFORM_NOTES.md` with verified dependencies and gotchas
- [ ] Update `README.md` platform support table (planned → ✅)
- [ ] Commit: `feat: linux port` / `feat: windows port`

## Phase 7 API coverage (already in C-ABI, must work on all platforms)

| Function | macOS | Linux target | Windows target |
|----------|-------|-------------|----------------|
| `sh_create`/`sh_destroy` | ✅ | ✅ | ✅ |
| `sh_start`/`sh_stop` | ✅ | ✅ | ✅ |
| `sh_is_running` | ✅ | ✅ | ✅ |
| `sh_get_current_selection` | ✅ | ✅ | ✅ |
| `sh_set_selection_callback` | ✅ | ✅ | ✅ |
| `sh_set_mouse_callback` | ✅ | ✅ | ✅ |
| `sh_set_keyboard_callback` | ✅ | ✅ | ✅ |
| `sh_enable/disable_mouse_move` | ✅ | ✅ | ✅ |
| `sh_set_config` | ✅ | ✅ | ✅ |
| `sh_set_passive_mode` | ✅ | ✅ | ✅ |
| `sh_write/read_clipboard` | ✅ | no-op | ✅ |
| `sh_mac_is_process_trusted` | ✅ | no-op | no-op |
| `sh_mac_request_process_trust` | ✅ | no-op | no-op |
