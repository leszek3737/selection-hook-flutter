import 'dart:async';
import 'dart:ffi' as ffi;
import 'dart:io';

import '../selection_hook_flutter_bindings_generated.dart';

/// Dart-side model for a text selection event.
class TextSelectionEvent {
  final String text;
  final String programName;
  final int startX;
  final int startY;
  final int endX;
  final int endY;
  final int method;
  final int posLevel;
  final bool isFullscreen;

  const TextSelectionEvent({
    required this.text,
    required this.programName,
    required this.startX,
    required this.startY,
    required this.endX,
    required this.endY,
    required this.method,
    required this.posLevel,
    required this.isFullscreen,
  });

  @override
  String toString() =>
      'TextSelectionEvent(text="$text", program="$programName", '
      'method=$method, pos=$posLevel)';
}

/// Internal implementation wrapping raw FFI bindings.
///
/// Lifecycle:
/// 1. `registerCallback()` — creates NativeCallable, calls sh_set_selection_callback
/// 2. `start()` — calls sh_start
/// 3. `stop()` — calls sh_stop (blocks until no in-flight callbacks)
/// 4. `dispose()` — sh_stop → close NativeCallable → sh_destroy
///
/// The [onEvent] callback is invoked from the NativeCallable listener isolate
/// on the main isolate (via Zone.current.handleUncaughtError if it throws).
class SelectionHookImpl {
  final SelectionHookFlutterBindings _bindings;
  ffi.Pointer<SelectionHook> _hook;
  ffi.NativeCallable<ffi.Void Function(ffi.Pointer<SHSelectionData>)>?
      _nativeCallable;
  bool _isDisposed = false;

  SelectionHookImpl._(this._bindings, this._hook);

  /// Creates a new native hook. Throws [StateError] on failure.
  static Future<SelectionHookImpl> create() async {
    final dylib = _openLibrary();
    final bindings = SelectionHookFlutterBindings(dylib);
    final hook = bindings.sh_create();
    if (hook == ffi.nullptr) {
      final errPtr = bindings.sh_last_global_error();
      final msg =
          errPtr != ffi.nullptr ? errPtr.toDartString() : 'unknown';
      throw StateError('sh_create failed: $msg');
    }
    return SelectionHookImpl._(bindings, hook);
  }

  static ffi.DynamicLibrary _openLibrary() {
    if (Platform.isMacOS) {
      return ffi.DynamicLibrary.open(
        '$_libName.framework/$_libName',
      );
    }
    if (Platform.isLinux) {
      return ffi.DynamicLibrary.open('lib$_libName.so');
    }
    if (Platform.isWindows) {
      return ffi.DynamicLibrary.open('$_libName.dll');
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }

  static const _libName = 'selection_hook_flutter';

  /// Register the native callback. Must be called before [start].
  ///
  /// [onEvent] is called on the main isolate when text is selected.
  /// Throws [StateError] if already disposed or callback registration fails.
  void registerCallback(void Function(TextSelectionEvent event) onEvent) {
    if (_isDisposed) throw StateError('SelectionHookImpl already disposed');

    final nativeCallable =
        ffi.NativeCallable<ffi.Void Function(ffi.Pointer<SHSelectionData>)>
            .listener((ffi.Pointer<SHSelectionData> data) {
      // Copy all fields before returning — pointer is valid only during callback.
      final ref = data.ref;
      final event = TextSelectionEvent(
        text: ref.text.toDartString(),
        programName: ref.program_name.toDartString(),
        startX: ref.start_top.x,
        startY: ref.start_top.y,
        endX: ref.end_top.x,
        endY: ref.end_top.y,
        method: ref.method,
        posLevel: ref.pos_level,
        isFullscreen: ref.is_fullscreen != 0,
      );
      onEvent(event);
    });

    final result = _bindings.sh_set_selection_callback(
      _hook,
      nativeCallable.nativeFunction,
    );
    if (result != 0) {
      nativeCallable.close();
      throw StateError('sh_set_selection_callback failed: $result');
    }
    _nativeCallable = nativeCallable;
  }

  /// Start monitoring text selections.
  ///
  /// Returns `true` on success. On macOS, may prompt for accessibility
  /// permissions on first run.
  ///
  /// Throws [StateError] if not registered or already disposed.
  bool start() {
    if (_isDisposed) throw StateError('SelectionHookImpl already disposed');
    if (_nativeCallable == null) {
      throw StateError('Call registerCallback() before start()');
    }
    final result = _bindings.sh_start(_hook);
    if (result == SH_ERR_NOT_TRUSTED) {
      throw StateError(
        'Accessibility permission not granted. '
        'Grant permission in System Settings > Privacy > Accessibility, '
        'then restart the app.',
      );
    }
    if (result != 0) {
      final errPtr = _bindings.sh_last_error(_hook);
      final msg =
          errPtr != ffi.nullptr ? errPtr.toDartString() : 'code $result';
      throw StateError('sh_start failed: $msg');
    }
    return true;
  }

  /// Stop monitoring. Blocks until no callbacks are in-flight.
  bool stop() {
    if (_isDisposed) return false;
    return _bindings.sh_stop(_hook) == 0;
  }

  /// Check if the hook is running.
  bool get isRunning {
    if (_isDisposed) return false;
    return _bindings.sh_is_running(_hook) == 1;
  }

  /// Get the current text selection snapshot.
  ///
  /// Returns `null` if no text is selected or the hook is not running.
  TextSelectionEvent? getCurrentSelection() {
    if (_isDisposed) return null;
    final ptr = _bindings.sh_get_current_selection(_hook);
    if (ptr == ffi.nullptr) return null;

    final ref = ptr.ref;
    final event = TextSelectionEvent(
      text: ref.text.toDartString(),
      programName: ref.program_name.toDartString(),
      startX: ref.start_top.x,
      startY: ref.start_top.y,
      endX: ref.end_top.x,
      endY: ref.end_top.y,
      method: ref.method,
      posLevel: ref.pos_level,
      isFullscreen: ref.is_fullscreen != 0,
    );
    _bindings.sh_free_selection_data(_hook, ptr);
    return event;
  }

  /// Stop, close NativeCallable, destroy native hook.
  ///
  /// Idempotent — safe to call multiple times.
  void dispose() {
    if (_isDisposed) return;
    _isDisposed = true;

    // Stop first — blocks until no in-flight callbacks.
    _bindings.sh_stop(_hook);

    // Close the NativeCallable — no more callbacks will fire.
    _nativeCallable?.close();
    _nativeCallable = null;

    // Destroy native resources.
    _bindings.sh_destroy(_hook);
    _hook = ffi.nullptr;
  }
}

/// Minimal Utf8 helper — avoids importing package:ffi for the public API.
/// Reads a NUL-terminated UTF-8 string from a `Pointer<Char>`.
extension Utf8 on ffi.Pointer<ffi.Char> {
  String toDartString() {
    final codeUnits = <int>[];
    var i = 0;
    while (true) {
      final byte = cast<ffi.Uint8>()[i];
      if (byte == 0) break;
      codeUnits.add(byte);
      i++;
    }
    return String.fromCharCodes(codeUnits);
  }
}
