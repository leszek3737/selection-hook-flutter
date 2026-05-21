# AGENTS.md

## Project identity

Flutter FFI plugin for cross-platform text selection monitoring. Port of
[selection-hook](https://github.com/0xfullex/selection-hook) (Node N-API →
C-ABI for `dart:ffi`). Desktop only (macOS, Windows, Linux). No iOS/Android.

## Key constraints

- **FFI only** — no platform channels (`MethodChannel`/`EventChannel`). All
  native↔Dart communication via `dart:ffi` + `NativeCallable.listener`.
- **`NativeCallable.listener`** for callbacks from OS threads (CGEventTap,
  Windows hooks, X11/Wayland event loops). Never `Pointer.fromFunction`
  for threaded callbacks.
- **C-ABI boundary** — opaque `SelectionHook*` handle + `extern "C"` functions.
  No C++ STL types in signatures. No `std::string`, no `std::function`.
- **`_upstream/` is read-only reference** — gitignored. Never commit.
- **Dart ≥ 3.12** required (`NativeCallable` → 3.1+, `calloc`/`malloc` removed
  from `dart:ffi` in 3.12, use `package:ffi`).
- **C++17** required for native code.

## Platform status

| Platform | Status | Implementation |
|----------|--------|----------------|
| macOS | **Full port** | `src/bridge/c_api_mac.mm` → `src/mac/selection_hook_core.mm` |
| Windows | Stub | `src/bridge/c_api.cc` (synthetic events). Lib helpers in `src/windows/lib/`. |
| Linux | Stub | `src/bridge/c_api.cc` (synthetic events). Lib helpers in `src/linux/`. |

Real Linux/Windows ports need `selection_hook_core.{h,cc}` per platform
(pattern from `src/mac/`), plus `c_api_{linux,win}.cc` bridges. CMakeLists.txt
has `if(LINUX)` / `if(WIN32)` blocks with TODO lists of needed sources.

## Build & test

```bash
# Edit: work in project root (plugin directory), not example/
# Build macOS example app:
cd example && flutter build macos --debug

# Run macOS example app:
cd example && flutter run -d macos

# Smoke test (pure Dart FFI, no Flutter) — macOS only:
dart run tool/smoke_test.dart
```

**Two build systems on macOS** — sources compile via both CMake and CocoaPods:
- `src/CMakeLists.txt` builds `.dylib` for Linux/Windows + macOS fallback.
- `macos/selection_hook_flutter.podspec` → `macos/Classes/selection_hook_flutter.mm`
  forwarder `#include`s sources from `src/` for the real `.app` bundle.
  Podspec sets `CLANG_CXX_LANGUAGE_STANDARD c++17`, `MACOSX_DEPLOYMENT_TARGET 10.14`,
  frameworks `ApplicationServices` + `Cocoa`.

## Regenerating FFI bindings

```bash
# After modifying src/bridge/c_api.h:
dart run ffigen --config ffigen.yaml
```
Output: `lib/selection_hook_flutter_bindings_generated.dart`.

## macOS gotchas

- **Entitlements**: Sandbox must be disabled. `com.apple.security.app-sandbox`
  set to `false` in both `example/macos/Runner/{DebugProfile,Release}.entitlements`.
  CGEventTap does not work inside sandbox.
- **Accessibility**: User must grant in System Settings → Privacy → Accessibility.
  `AXIsProcessTrustedWithOptions(kAXTrustedCheckOptionPrompt=true)` called in
  `start()` flow.
- **Thread safety**: `processMouseEvent` dispatched to main queue via
  `dispatch_async(dispatch_get_main_queue(), ...)`. Guarded against use-after-free
  with `!hook->running` check in the block.
- **Clipboard fallback**: `getTextViaClipboard` polls clipboard on main thread
  (up to 100ms). Only fires when AXAPI fails and clipboard fallback is enabled.

## Key source files

| File | Role |
|------|------|
| `src/bridge/c_api.h` | C-ABI header — canonical API surface |
| `src/bridge/c_api_mac.mm` | macOS bridge (extern "C" implementations) |
| `src/bridge/c_api.cc` | Stub bridge for Linux/Windows |
| `src/mac/selection_hook_core.{h,mm}` | Ported macOS core (no N-API) |
| `src/mac/lib/*.{h,mm}` | Upstream helpers (clipboard, keyboard, utils) |
| `lib/selection_hook_flutter.dart` | Public Dart API (singleton, streams) |
| `lib/src/selection_hook_impl.dart` | FFI adapter (NativeCallable, lifecycle) |
| `lib/selection_hook_flutter_bindings_generated.dart` | ffigen output |
| `example/lib/main.dart` | Demo app |
| `tool/smoke_test.dart` | Pure Dart FFI smoke test |
| `ffigen.yaml` | ffigen config |
| `PORTING_NOTES.md` | API surface mapping, TSFN→callback mapping |
| `PLATFORM_NOTES.md` | Per-platform permissions, build, port status |

## Architecture pattern for adding a platform

1. Copy `_upstream/src/<platform>/lib/*` → `src/<platform>/lib/` (N-API free).
2. Port `_upstream/src/<platform>/selection_hook.{cc,mm}` →
   `src/<platform>/selection_hook_core.{h,cc}` — strip `Napi::*`, replace
   `Napi::ThreadSafeFunction` with `SHSelectionCallback` function pointer,
   wire event loop/taps directly to `dispatchSelection`.
3. Write `src/bridge/c_api_<platform>.{cc,mm}` — `extern "C"` bridge copying
   pattern from `c_api_mac.mm`.
4. Add platform block to `src/CMakeLists.txt` with real sources + framework links.
5. Smoke test → manual test → Flutter app test.
