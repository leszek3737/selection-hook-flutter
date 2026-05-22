# AGENTS.md

## Project identity

Flutter FFI plugin for cross-platform text selection monitoring. Port of
[selection-hook](https://github.com/0xfullex/selection-hook) (Node N-API →
C-ABI for `dart:ffi`). Desktop only (macOS, Windows, Linux). No iOS/Android.

## Key constraints

- **FFI only** — no platform channels (`MethodChannel`/`EventChannel`). All
  native↔Dart communication via `dart:ffi` + `NativeCallable.listener`.
- **`NativeCallable.listener`** for callbacks from OS threads (CGEventTap,
  Windows hooks, X11 event loops). Never `Pointer.fromFunction`
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
| Windows | **Full port** | `src/bridge/c_api_win.cc` → `src/windows/selection_hook_core.cc` |
| Linux X11 | **Substantially ported** | `src/bridge/c_api_linux.cc` → `src/linux/selection_hook_core.cc` + `src/linux/protocols/x11.cc` |
| Linux Wayland | **Code present, not buildable** | `src/linux/protocols/wayland.cc` (missing generated headers + CMake integration) |

### Per-platform remaining work

- **Windows**: Needs Flutter plugin `windows/CMakeLists.txt` scaffold + testing on Windows hardware.
- **Linux X11**: Functional, needs testing on Linux.
- **Linux Wayland**: Missing generated protocol headers (`wayland/ext-data-control-v1-client.h`,
  `wlr-data-control-unstable-v1-client.h`), CMake integration for `wayland.cc`,
  and display protocol auto-detection implementation.

## Build & test

```bash
# Build macOS example app:
cd example && flutter build macos --debug

# Run macOS example app:
cd example && flutter run -d macos

# Smoke test (pure Dart FFI, no Flutter) — macOS only:
dart run tool/smoke_test.dart
```

**Two build systems on macOS** — sources compile via both CMake and CocoaPods:
- `src/CMakeLists.txt` builds shared library (`.dylib`/`.so`/`.dll`) per platform.
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
| `src/bridge/c_api.h` | C-ABI header — canonical API surface (20 functions, 5 structs, 2 enums) |
| `src/bridge/c_api_mac.mm` | macOS bridge (extern "C" implementations) |
| `src/bridge/c_api_win.cc` | Windows bridge (extern "C" implementations) |
| `src/bridge/c_api_linux.cc` | Linux bridge (extern "C" implementations) |
| `src/bridge/c_api.cc` | Stub fallback bridge (synthetic events, unused on real platforms) |
| `src/mac/selection_hook_core.{h,mm}` | macOS core — CGEventTap, AXAPI, clipboard fallback |
| `src/windows/selection_hook_core.{h,cc}` | Windows core — WH_MOUSE_LL/WH_KEYBOARD_LL, UIAutomation, MSAA |
| `src/linux/selection_hook_core.{h,cc}` | Linux core — XRecord, XFixes selection detection |
| `src/linux/common.h` | Linux shared types, ProtocolBase abstraction |
| `src/linux/protocols/x11.cc` | X11 protocol implementation |
| `src/linux/protocols/wayland.cc` | Wayland protocol (incomplete — missing generated headers) |
| `src/{mac,windows,linux}/lib/*` | Platform helpers (clipboard, keyboard, utils) |
| `lib/selection_hook_flutter.dart` | Public Dart API (singleton, streams) |
| `lib/src/selection_hook_impl.dart` | FFI adapter (NativeCallable, lifecycle) |
| `lib/selection_hook_flutter_bindings_generated.dart` | ffigen output (725 lines) |
| `example/lib/main.dart` | Demo app |
| `tool/smoke_test.dart` | Pure Dart FFI smoke test (macOS only) |
| `ffigen.yaml` | ffigen config |
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

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **selection-hook-flutter** (1824 symbols, 3340 relationships, 34 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> If any GitNexus tool warns the index is stale, run `npx gitnexus analyze` in terminal first.

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `gitnexus_impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `gitnexus_detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `gitnexus_query({query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `gitnexus_context({name: "symbolName"})`.

## Never Do

- NEVER edit a function, class, or method without first running `gitnexus_impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `gitnexus_rename` which understands the call graph.
- NEVER commit changes without running `gitnexus_detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/selection-hook-flutter/context` | Codebase overview, check index freshness |
| `gitnexus://repo/selection-hook-flutter/clusters` | All functional areas |
| `gitnexus://repo/selection-hook-flutter/processes` | All execution flows |
| `gitnexus://repo/selection-hook-flutter/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
