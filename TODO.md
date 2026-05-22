# Remaining Work: Windows Port

## Current state

| Platform | Status |
|----------|--------|
| macOS | Fully ported — AXAPI + CGEventTap + clipboard fallback |
| Linux | Fully ported — X11 (XRecord + XFixes), gesture detection |
| Windows | Stub — helpers copied, real port pending |

Windows stub compiles via `src/bridge/c_api.cc` (synthetic test events only).

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

## Windows checklist

- [ ] Core class: strip N-API, add `SHSelectionCallback`/`SHMouseCallback`/`SHKeyboardCallback` members
- [ ] Core class: `start()`/`stop()`/`isRunning()`/`getCurrentSelection()`
- [ ] Core class: `dispatchSelection()` → fills `SHSelectionData` + calls callback
- [ ] Core class: `dispatchMouseEvent()` / `dispatchKeyboardEvent()`
- [ ] Core class: `processMouseEvent()` — drag/double-click/shift-click selection detection
- [ ] Bridge: `SelectionHook` struct with core pointer + callback pointers + error string
- [ ] Bridge: all `extern "C"` functions from `c_api.h`
- [ ] Bridge: `sh_get_current_selection` returns heap-allocated `SHSelectionData` with `strdup`
- [ ] Bridge: `running` guard for use-after-free safety
- [ ] Bridge: `is_processing` uses `exchange(true)` (atomic check-and-set)
- [ ] CMakeLists.txt: platform block with real sources + framework/lib links
- [ ] `flutter build` succeeds
- [ ] Smoke test passes
- [ ] Manual test: 3 different apps (Notepad, browser, VS Code)
- [ ] Commit: `feat: windows port`
