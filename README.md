# selection_hook_flutter

Cross-platform text selection monitoring for Flutter desktop apps via FFI.

Detects text selections in **any application** on Windows, macOS, and Linux using
platform accessibility APIs (UIAutomation, AXAPI, X11 PRIMARY selection).

Ported from [selection-hook](https://github.com/0xfullex/selection-hook) (Node.js
N-API) to C-ABI for `dart:ffi` — preserving ~95% of the upstream native code.

## Features

- **Text selection detection** with full metadata (coordinates, method, position level)
- **Mouse events** — down, up, move, wheel (wheel direction + per-axis data)
- **Keyboard events** — unified MDN `KeyboardEvent.key` values + platform vk codes
- **Clipboard read/write** (macOS, Windows)
- **Program filtering** — include/exclude lists for selection events
- **Passive mode** — only detect on manual `getCurrentSelection()`
- **macOS fullscreen detection**
- Pure FFI — no platform channels, lower latency for high-frequency events

### Platform Support

| Feature | macOS | Windows | Linux X11 | Linux Wayland |
|---------|-------|---------|-----------|---------------|
| Text selection | ✅ | planned | planned | - |
| Mouse events | ✅ | planned | planned | - |
| Keyboard events | ✅ | planned | planned | - |
| Clipboard | ✅ | planned | — | — |

## Requirements

- Flutter >= 3.3.0 / Dart >= 3.12.0
- macOS 10.14+, Windows 7+, Linux (X11)

### macOS Permissions

The app **must** be granted Accessibility permission:

1. Go to **System Settings > Privacy & Security > Accessibility**
2. Enable your application
3. Restart the application

The first time `start()` is called, macOS may prompt for permission automatically.
If denied or dismissed, you must enable it manually.

**Hot reload warning:** After hot reload, the native hook instance is recreated.
Stop monitoring before reloading, then restart.

## Quick Start

```dart
import 'package:selection_hook_flutter/selection_hook_flutter.dart';

void main() {
  final hook = SelectionHook.instance;

  // Listen for text selections
  hook.onTextSelection.listen((event) {
    print('Selected: "${event.text}" in ${event.programName}');
  });

  // Optionally listen for mouse/keyboard events
  hook.onMouseEvent.listen((event) {
    print('Mouse: ${event.eventType} at (${event.x}, ${event.y})');
  });

  hook.start(); // May prompt for accessibility on macOS
}
```

## API

### SelectionHook (singleton)

| Method | Description |
|--------|-------------|
| `start()` | Start monitoring. May throw `StateError` if accessibility not granted. |
| `stop()` | Stop monitoring. Blocks until no in-flight callbacks. |
| `dispose()` | Stop + release all native resources. Idempotent. |
| `getCurrentSelection()` | Synchronous snapshot of current selection (or null). |
| `configure({...})` | Set config before `start()`. Options: `debug`, `enableMouseMove`, `enableClipboard`, `selectionPassiveMode`. |
| `writeClipboard(text)` | Write text to clipboard (macOS/Windows). |
| `readClipboard()` | Read text from clipboard (macOS/Windows). |
| `enableMouseMove()` | Enable high-CPU mouse move events. Call before `start()`. |
| `disableMouseMove()` | Disable mouse move events (default). |

### Streams

| Stream | Type |
|--------|------|
| `onTextSelection` | `Stream<TextSelectionEvent>` |
| `onMouseEvent` | `Stream<MouseEvent>` |
| `onKeyboardEvent` | `Stream<KeyboardEvent>` |

### TextSelectionEvent

| Field | Type | Description |
|-------|------|-------------|
| `text` | `String` | Selected text (UTF-8) |
| `programName` | `String` | Bundle/executable name of source app |
| `startX`, `startY` | `int` | Selection start coordinates (screen pixels) |
| `endX`, `endY` | `int` | Selection end coordinates |
| `method` | `int` | Selection method (11=AXAPI, 99=Clipboard…) |
| `posLevel` | `int` | Position detail level |
| `isFullscreen` | `bool` | Source window fullscreen (macOS only) |

### MouseEvent

| Field | Type | Description |
|-------|------|-------------|
| `x`, `y` | `int` | Screen coordinates |
| `button` | `int` | Button: -1=None, 0=Left, 1=Middle, 2=Right |
| `eventType` | `int` | 0=down, 1=up, 2=move, 3=wheel |
| `flag` | `int` | Wheel direction: 1=Up/Right, -1=Down/Left |

### KeyboardEvent

| Field | Type | Description |
|-------|------|-------------|
| `uniKey` | `String` | MDN `KeyboardEvent.key` value |
| `vkCode` | `int` | Platform virtual key code |
| `isSys` | `bool` | Modifier key pressed (Alt/Ctrl/Win/Cmd/Fn) |
| `flags` | `int` | Platform flags |

## Architecture

```
Dart (SelectionHook singleton)
  → selection_hook_impl.dart (FFI wrapper)
    → NativeCallable.listener (for thread callbacks)
      → src/bridge/c_api.h (C-ABI)
        → src/bridge/c_api_mac.mm (macOS bridge)
          → src/mac/selection_hook_core.{h,mm} (ported from upstream)
```

No platform channels. Callbacks from native OS threads (CGEventTap,
Windows hook, X11 event loop) are marshalled via `NativeCallable.listener`.

## License

MIT — see [LICENSE](./LICENSE).

This project is a port of [selection-hook](https://github.com/0xfullex/selection-hook)
by 0xfullex, also MIT licensed. Upstream copyright included in LICENSE.
