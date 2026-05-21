# PORTING NOTES — selection-hook (Node N-API → Flutter FFI)

## Host Platform

| Property | Value |
|----------|-------|
| OS | macOS (Darwin) |
| Arch | arm64 |
| Version | 26.5 (Build 25F71) |
| Flutter | 3.44.0 stable |
| Dart | 3.12.0 |

## API Surface Mapping (from `_upstream/index.d.ts`)

Total public methods in `index.d.ts`: **18 methods + 1 constructor**

### Lifecycle Methods

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| 1 | `start()` | `start(config?) → bool` | ✅ MVP | Core lifecycle |
| 2 | `stop()` | `stop() → bool` | ✅ MVP | Core lifecycle |
| 3 | `isRunning()` | `isRunning() → bool` | ✅ MVP | Status query |
| 4 | `cleanup()` | `cleanup() → void` | ✅ MVP | Maps to `sh_destroy` |

### Selection Methods

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| 5 | `getCurrentSelection()` | `() → TextSelectionData \| null` | ✅ MVP | Core feature |
| 6 | `setSelectionPassiveMode()` | `(passive: bool) → bool` | Deferred | Phase 7 |

### Mouse Tracking

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| 7 | `enableMouseMoveEvent()` | `() → bool` | Deferred | Phase 7 |
| 8 | `disableMouseMoveEvent()` | `() → bool` | Deferred | Phase 7 |

### Clipboard

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| 9 | `enableClipboard()` | `() → bool` | Deferred | Phase 7 |
| 10 | `disableClipboard()` | `() → bool` | Deferred | Phase 7 |
| 11 | `setClipboardMode()` | `(mode, programList?) → bool` | Deferred | Phase 7 |
| 12 | `writeToClipboard()` | `(text: string) → bool` | Deferred | Phase 7 |
| 13 | `readFromClipboard()` | `() → string \| null` | Deferred | Phase 7 |

### Filtering

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| 14 | `setGlobalFilterMode()` | `(mode, programList?) → bool` | Deferred | Phase 7 |
| 15 | `setFineTunedList()` | `(listType, programList?) → bool` | Deferred | Phase 7 |

### Platform-Specific

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| 16 | `macIsProcessTrusted()` | `() → bool` | Deferred | Called inline in `sh_start` |
| 17 | `macRequestProcessTrust()` | `() → bool` | Deferred | Called inline in `sh_start` |
| 18 | `linuxGetEnvInfo()` | `() → LinuxEnvInfo \| null` | Deferred | Phase 6 |

### Constructor

| # | Method | Signature | In MVP | Reason |
|---|--------|-----------|--------|--------|
| — | `new SelectionHook()` | constructor | N/A | Dart `SelectionHook()` creates handle internally |

### Summary

| Category | Count |
|----------|-------|
| In MVP | 5 (start, stop, isRunning, getCurrentSelection, cleanup) |
| Deferred (Phase 7) | 13 |
| N/A (constructor) | 1 |
| **Total** | **19** (18 methods + 1 constructor) |

Match check: index.d.ts has 18 methods + constructor. Table covers all 18. ✅

## Events (from `index.d.ts`)

| # | Event | Payload Type | In MVP | Reason |
|---|-------|-------------|--------|--------|
| 1 | `text-selection` | `TextSelectionData` | ✅ MVP | Core event |
| 2 | `mouse-up` | `MouseEventData` | Deferred | Phase 7 |
| 3 | `mouse-down` | `MouseEventData` | Deferred | Phase 7 |
| 4 | `mouse-move` | `MouseEventData` | Deferred | Phase 7 |
| 5 | `mouse-wheel` | `MouseWheelEventData` | Deferred | Phase 7 |
| 6 | `key-down` | `KeyboardEventData` | Deferred | Phase 7 |
| 7 | `key-up` | `KeyboardEventData` | Deferred | Phase 7 |
| 8 | `status` | `string` | Deferred | Phase 7 |
| 9 | `error` | `Error` | Deferred | Phase 7 |

## Static Constants

| Name | Values | In MVP |
|------|--------|--------|
| `SelectionMethod` | NONE=0, UIA=1, FOCUSCTL=2(depr), ACCESSIBLE=3, AXAPI=11, ATSPI=21(reserved), PRIMARY=22, CLIPBOARD=99 | ✅ (used in TextSelectionData) |
| `PositionLevel` | NONE=0, MOUSE_SINGLE=1, MOUSE_DUAL=2, SEL_FULL=3, SEL_DETAILED=4 | ✅ (used in TextSelectionData) |
| `FilterMode` | DEFAULT=0, INCLUDE_LIST=1, EXCLUDE_LIST=2 | Deferred |
| `FineTunedListType` | EXCLUDE_CLIPBOARD_CURSOR_DETECT=0, INCLUDE_CLIPBOARD_DELAY_READ=1 | Deferred |
| `INVALID_COORDINATE` | -99999 | ✅ (used in TextSelectionData) |
| `DisplayProtocol` | UNKNOWN=0, X11=1, WAYLAND=2 | Deferred |
| `CompositorType` | UNKNOWN=0, KWIN=1, MUTTER=2, HYPRLAND=3, SWAY=4, WLROOTS=5, COSMIC_COMP=6 | Deferred |

## ThreadSafeFunction / Cross-Thread Callback Locations

All `Napi::ThreadSafeFunction` instances that will be replaced with `NativeCallable.listener` callbacks:

### Windows (`_upstream/src/windows/selection_hook.cc`)

| Line | Variable | Queue | Purpose |
|------|----------|-------|---------|
| 255 | `tsfn` (member) | — | Text selection callback |
| 256 | `mouse_tsfn` (member) | — | Mouse event callback |
| 257 | `keyboard_tsfn` (member) | — | Keyboard event callback |
| 479 | `tsfn = ...New(...)` | 0/1 | TextSelectionCallback creation |
| 486 | `mouse_tsfn = ...New(...)` | DEFAULT | MouseEventCallback creation |
| 492 | `keyboard_tsfn = ...New(...)` | DEFAULT | KeyboardEventCallback creation |

### macOS (`_upstream/src/mac/selection_hook.mm`)

| Line | Variable | Queue | Purpose |
|------|----------|-------|---------|
| 231 | `tsfn` (member) | — | Text selection callback |
| 232 | `mouse_tsfn` (member) | — | Mouse event callback |
| 233 | `keyboard_tsfn` (member) | — | Keyboard event callback |
| 383 | `tsfn = ...New(...)` | 0/1 | TextSelectionCallback creation |
| 387 | `mouse_tsfn = ...New(...)` | — | MouseEventCallback creation |
| 393 | `keyboard_tsfn = ...New(...)` | — | KeyboardEventCallback creation |

### Linux (`_upstream/src/linux/selection_hook.cc`)

| Line | Variable | Queue | Purpose |
|------|----------|-------|---------|
| 342 | `tsfn` (member) | — | Text selection callback |
| 343 | `mouse_tsfn` (member) | — | Mouse event callback |
| 344 | `keyboard_tsfn` (member) | — | Keyboard event callback |
| 345 | `selection_tsfn` (member) | — | Selection event callback (Path A/B) |
| 533 | `tsfn = ...New(...)` | 0/1 | TextSelectionCallback creation |
| 537 | `mouse_tsfn = ...New(...)` | DEFAULT_MOUSE_EVENT_QUEUE_SIZE/1 | MouseEventCallback creation |
| 542 | `keyboard_tsfn = ...New(...)` | DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE/1 | KeyboardEventCallback creation |
| 546 | `selection_tsfn = ...New(...)` | 64/1 | SelectionEventCallback creation |

**Total: 25 matches across 3 files (6 Windows, 6 macOS, 13 Linux).**

### Replacement strategy

Each `Napi::ThreadSafeFunction` becomes a `NativeCallable<Void Function(...)>.listener` + a C function pointer stored in the SelectionHook struct. The native thread calls the C function pointer, which Dart marshals to the Stream.

For MVP:
- Only `tsfn` (text selection) on macOS needs to be wired
- `mouse_tsfn`, `keyboard_tsfn`, `selection_tsfn` (Linux) deferred to Phase 7

## Upstream Source File Inventory

### macOS (`_upstream/src/mac/`)
- `selection_hook.mm` — main platform integration (N-API binding)
- `lib/clipboard.h`, `lib/clipboard.mm` — clipboard utilities
- `lib/keyboard.h`, `lib/keyboard.mm` — keyboard event handling
- `lib/utils.h`, `lib/utils.mm` — shared utilities

### Windows (`_upstream/src/windows/`)
- `selection_hook.cc` — main platform integration (N-API binding)
- `lib/clipboard.cc`, `lib/clipboard.h` — clipboard utilities
- `lib/keyboard.cc`, `lib/keyboard.h` — keyboard event handling
- `lib/string_pool.cc`, `lib/string_pool.h` — string pool (Windows-only)
- `lib/utils.cc`, `lib/utils.h` — shared utilities

### Linux (`_upstream/src/linux/`)
- `selection_hook.cc` — main platform integration (N-API binding)
- `common.h` — shared types
- `lib/keyboard.cc`, `lib/keyboard.h` — keyboard event handling
- `lib/utils.cc`, `lib/utils.h` — shared utilities
- `protocols/x11.cc` — X11 selection protocol
- `protocols/wayland.cc` — Wayland selection protocol
- `protocols/wayland/` — auto-generated Wayland protocol files

### No common/ directory exists
The upstream has no `_upstream/src/common/` directory. Platform-specific code is self-contained per `src/{windows,mac,linux}/`.

## Phase 1 Completion Checklist

- [x] Scaffold generated: `selection_hook_flutter/`
- [x] Upstream cloned: `_upstream/` (in `.gitignore`)
- [x] `index.d.ts` read — canonical API surface
- [x] `docs/API.md` read
- [x] `README.md` read
- [x] API surface table — 18 methods mapped (4 MVP, 13 deferred, 1 N/A)
- [x] Events table — 9 events (1 MVP, 8 deferred)
- [x] ThreadSafeFunction locations — 25 matches across 3 files
- [x] Host platform identified: macOS arm64
- [ ] GATE: user acceptance

## Notes

- Upstream has NO `src/common/` directory. Phase 4 plan says copy `_upstream/src/{common,<host_platform>}/*` — need to adjust: copy `_upstream/src/mac/*` directly (excluding N-API binding portions).
- `_upstream/src/*/selection_hook.{cc,mm}` files are the N-API bindings. We must NOT copy them wholesale — extract the non-N-API logic and wrap it behind `extern "C"` in our new `c_api.cc`.
- Platform-specific `lib/` files (clipboard, keyboard, utils) can be copied as-is (they contain utility functions not tied to N-API).
- macOS entitlements needed: `com.apple.security.cs.disable-library-validation` for unsigned dylib loading in example app.
