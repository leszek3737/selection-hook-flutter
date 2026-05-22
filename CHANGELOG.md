## 0.1.0

* macOS: full port — AXAPI + CGEventTap + clipboard fallback.
* Windows: full port — UIAutomation + MSAA + clipboard fallback + WH_MOUSE_LL/WH_KEYBOARD_LL hooks.
* Linux X11: substantially ported — XRecord input monitoring + XFixes PRIMARY selection + XConvertSelection text retrieval.
* Linux Wayland: protocol code present but not yet buildable (missing generated headers).
* Public Dart API: `SelectionHook` singleton with `start()`, `stop()`, `dispose()`,
  `getCurrentSelection()`, `configure()`, `writeClipboard()`, `readClipboard()`,
  `enableMouseMove()`, `disableMouseMove()`, `isRunning` getter.
* Streams: `onTextSelection`, `onMouseEvent`, `onKeyboardEvent`.
* C-ABI surface: 20 exported functions, 5 structs, 2 enums, 3 callback typedefs.
* FFI bindings generated via ffigen from `src/bridge/c_api.h`.
* Smoke test (`tool/smoke_test.dart`) — pure Dart FFI, macOS only.
* Example Flutter app (`example/lib/main.dart`) — real-time selection monitor.
