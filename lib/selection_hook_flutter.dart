/// Flutter FFI plugin for cross-platform text selection monitoring.
///
/// Monitors text selections across applications on macOS, Windows, and Linux
/// using native accessibility APIs (AXAPI, UIA, AT-SPI/PRIMARY).
///
/// Usage:
/// ```dart
/// final hook = SelectionHook.instance;
/// await hook.start();
/// hook.onTextSelection.listen((event) {
///   print('Selected: ${event.text} in ${event.programName}');
/// });
/// // ... later
/// await hook.stop();
/// ``

library;

import 'dart:async';

import 'src/selection_hook_impl.dart';

export 'src/selection_hook_impl.dart'
    show TextSelectionEvent, MouseEvent, KeyboardEvent;

/// Text selection monitoring hook.
///
/// Singleton — call `SelectionHook.instance` to get the shared instance.
/// Designed for hot-reload safety: the instance survives widget rebuilds.
///
/// Lifecycle:
/// 1. `await start()` — begin monitoring (registers native callback)
/// 2. Listen to `onTextSelection` stream
/// 3. `await stop()` — stop monitoring
/// 4. `dispose()` — release all native resources (idempotent)
///
/// After `dispose()`, the instance can be re-created by calling `start()`
/// again — a new native hook will be allocated automatically.
class SelectionHook {
  SelectionHook._();

  static SelectionHook? _instance;

  /// The shared singleton instance.
  static SelectionHook get instance => _instance ??= SelectionHook._();

  SelectionHookImpl? _impl;
  final StreamController<TextSelectionEvent> _controller =
      StreamController<TextSelectionEvent>.broadcast();
  StreamController<MouseEvent>? _mouseController;
  StreamController<KeyboardEvent>? _keyboardController;

  bool _isStarted = false;
  bool _isDisposed = false;

  /// Stream of text selection events.
  ///
  /// Broadcast stream — multiple listeners allowed. Events are delivered
  /// on the main isolate.
  Stream<TextSelectionEvent> get onTextSelection => _controller.stream;

  /// Mouse events (down, up, move, wheel).
  Stream<MouseEvent> get onMouseEvent {
    _mouseController ??= StreamController<MouseEvent>.broadcast();
    return _mouseController!.stream;
  }

  /// Keyboard events (down, up).
  Stream<KeyboardEvent> get onKeyboardEvent {
    _keyboardController ??= StreamController<KeyboardEvent>.broadcast();
    return _keyboardController!.stream;
  }

  /// Whether the hook is currently monitoring.
  bool get isRunning => _impl?.isRunning ?? false;

  /// Start monitoring text selections.
  ///
  /// On macOS, may prompt for accessibility permissions on first run.
  /// Throws [StateError] if permission is not granted.
  ///
  /// Safe to call multiple times (no-op if already started).
  Future<void> start() async {
    if (_isDisposed) {
      // Re-create after previous dispose.
      _isDisposed = false;
      _instance = SelectionHook._();
      return _instance!.start();
    }
    if (_isStarted) return;

    _impl = await SelectionHookImpl.create();
    _impl!.registerCallback((event) {
      if (!_controller.isClosed) {
        _controller.add(event);
      }
    });
    if (_mouseController != null && _mouseController!.hasListener) {
      _impl!.registerMouseCallback((event) {
        if (!_mouseController!.isClosed) _mouseController!.add(event);
      });
    }
    if (_keyboardController != null && _keyboardController!.hasListener) {
      _impl!.registerKeyboardCallback((event) {
        if (!_keyboardController!.isClosed) _keyboardController!.add(event);
      });
    }
    _impl!.start();
    _isStarted = true;
  }

  /// Stop monitoring text selections.
  ///
  /// Blocks until no native callbacks are in-flight.
  /// Safe to call multiple times (no-op if not started).
  Future<void> stop() async {
    if (!_isStarted || _impl == null) return;
    _impl!.stop();
    _isStarted = false;
  }

  /// Get the current text selection snapshot.
  ///
  /// Returns `null` if no text is selected or the hook is not running.
  Future<TextSelectionEvent?> getCurrentSelection() async {
    if (_impl == null) return null;
    return _impl!.getCurrentSelection();
  }

  /// Apply configuration before [start].
  void configure({
    bool debug = false,
    bool enableMouseMove = false,
    bool enableClipboard = true,
    bool selectionPassiveMode = false,
  }) {
    _impl?.setConfig(
      debug: debug,
      enableMouseMove: enableMouseMove,
      enableClipboard: enableClipboard,
      selectionPassiveMode: selectionPassiveMode,
    );
  }

  /// Write text to clipboard. macOS/Windows only, returns false on Linux.
  bool writeClipboard(String text) => _impl?.writeClipboard(text) ?? false;

  /// Read text from clipboard. macOS/Windows only, returns null on Linux.
  String? readClipboard() => _impl?.readClipboard();

  /// Enable high-CPU mouse move events. Call before [start].
  void enableMouseMove() => _impl?.enableMouseMove();

  /// Disable mouse move events (default).
  void disableMouseMove() => _impl?.disableMouseMove();

  /// Release all native resources.
  ///
  /// Idempotent — safe to call multiple times. After dispose, the
  /// singleton can be re-created by calling [start] again.
  void dispose() {
    if (_isDisposed) return;
    _isDisposed = true;
    _isStarted = false;
    _mouseController?.close();
    _mouseController = null;
    _keyboardController?.close();
    _keyboardController = null;
    _impl?.dispose();
    _impl = null;
  }
}
