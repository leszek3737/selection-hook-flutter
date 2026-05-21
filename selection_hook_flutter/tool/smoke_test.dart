import 'dart:async';
import 'dart:ffi';
import 'dart:io';

final DynamicLibrary _lib = () {
  final buildDir = Platform.environment['SMOKE_BUILD_DIR'] ??
      'build/macos';
  return DynamicLibrary.open('$buildDir/libselection_hook_flutter.dylib');
}();

final class SHPoint extends Struct {
  @Int32()
  external int x;
  @Int32()
  external int y;
}

final class SHSelectionData extends Struct {
  external Pointer<Int8> text;
  external Pointer<Int8> program_name;
  external SHPoint start_top;
  external SHPoint start_bottom;
  external SHPoint end_top;
  external SHPoint end_bottom;
  external SHPoint mouse_start;
  external SHPoint mouse_end;
  @Int32()
  external int method;
  @Int32()
  external int pos_level;
  @Int32()
  external int is_fullscreen;
}

extension Utf8Pointer on Pointer<Int8> {
  String toDartString() {
    final buff = <int>[];
    var offset = 0;
    while (true) {
      final byte = elementAt(offset).value;
      if (byte == 0) break;
      buff.add(byte);
      offset++;
    }
    return String.fromCharCodes(buff);
  }
}

typedef SHSelectionCallbackNative = Void Function(
  Pointer<SHSelectionData> data,
);
typedef SHSelectionCallbackDart = void Function(
  Pointer<SHSelectionData> data,
);

typedef ShCreateNative = Pointer<Void> Function();
typedef ShCreateDart = Pointer<Void> Function();
typedef ShDestroyNative = Void Function(Pointer<Void> hook);
typedef ShDestroyDart = void Function(Pointer<Void> hook);
typedef ShStartNative = Int Function(Pointer<Void> hook);
typedef ShStartDart = int Function(Pointer<Void> hook);
typedef ShStopNative = Int Function(Pointer<Void> hook);
typedef ShStopDart = int Function(Pointer<Void> hook);
typedef ShIsRunningNative = Int Function(Pointer<Void> hook);
typedef ShIsRunningDart = int Function(Pointer<Void> hook);
typedef ShSetSelectionCallbackNative = Int Function(
  Pointer<Void> hook,
  Pointer<NativeFunction<SHSelectionCallbackNative>> callback,
);
typedef ShSetSelectionCallbackDart = int Function(
  Pointer<Void> hook,
  Pointer<NativeFunction<SHSelectionCallbackNative>> callback,
);
typedef ShGetCurrentSelectionNative = Pointer<SHSelectionData> Function(
  Pointer<Void> hook,
);
typedef ShGetCurrentSelectionDart = Pointer<SHSelectionData> Function(
  Pointer<Void> hook,
);
typedef ShFreeSelectionDataNative = Void Function(
  Pointer<Void> hook,
  Pointer<SHSelectionData> data,
);
typedef ShFreeSelectionDataDart = void Function(
  Pointer<Void> hook,
  Pointer<SHSelectionData> data,
);
typedef ShLastErrorNative = Pointer<Int8> Function(Pointer<Void> hook);
typedef ShLastErrorDart = Pointer<Int8> Function(Pointer<Void> hook);
typedef ShLastGlobalErrorNative = Pointer<Int8> Function();
typedef ShLastGlobalErrorDart = Pointer<Int8> Function();

late final sh_create = _lib
    .lookupFunction<ShCreateNative, ShCreateDart>('sh_create');
late final sh_destroy = _lib
    .lookupFunction<ShDestroyNative, ShDestroyDart>('sh_destroy');
late final sh_start = _lib
    .lookupFunction<ShStartNative, ShStartDart>('sh_start');
late final sh_stop = _lib
    .lookupFunction<ShStopNative, ShStopDart>('sh_stop');
late final sh_is_running = _lib
    .lookupFunction<ShIsRunningNative, ShIsRunningDart>('sh_is_running');
late final sh_set_selection_callback = _lib.lookupFunction<
    ShSetSelectionCallbackNative,
    ShSetSelectionCallbackDart>('sh_set_selection_callback');
late final sh_get_current_selection =
    _lib.lookupFunction<ShGetCurrentSelectionNative,
        ShGetCurrentSelectionDart>('sh_get_current_selection');
late final sh_free_selection_data =
    _lib.lookupFunction<ShFreeSelectionDataNative,
        ShFreeSelectionDataDart>('sh_free_selection_data');
late final sh_last_error = _lib
    .lookupFunction<ShLastErrorNative, ShLastErrorDart>('sh_last_error');
late final sh_last_global_error = _lib
    .lookupFunction<ShLastGlobalErrorNative, ShLastGlobalErrorDart>(
        'sh_last_global_error');

Future<void> main() async {
  print('[smoke] Creating hook...');
  final hook = sh_create();
  if (hook == nullptr) {
    final err = sh_last_global_error();
    if (err != nullptr) {
      print('FAIL: sh_create returned NULL: ${err.toDartString()}');
    } else {
      print('FAIL: sh_create returned NULL (no error)');
    }
    exit(1);
  }
  print('[smoke] sh_create OK');

  final events = <String>[];
  var eventCount = 0;

  final nativeCallable = NativeCallable<
      Void Function(Pointer<SHSelectionData>)>.listener((Pointer<SHSelectionData> data) {
    final ref = data.ref;
    final text = ref.text.toDartString();
    final prog = ref.program_name.toDartString();
    eventCount++;
    final msg = 'event #$eventCount: text="$text" program="$prog" '
        'method=${ref.method} pos_level=${ref.pos_level}';
    print(msg);
    events.add(msg);
  });

  print('[smoke] Registering callback...');
  final setResult = sh_set_selection_callback(
    hook,
    nativeCallable.nativeFunction,
  );
  if (setResult != 0) {
    print('FAIL: sh_set_selection_callback returned $setResult');
    nativeCallable.close();
    sh_destroy(hook);
    exit(1);
  }
  print('[smoke] sh_set_selection_callback OK');

  print('[smoke] Starting hook...');
  final startResult = sh_start(hook);
  if (startResult != 0) {
    final errPtr = sh_last_error(hook);
    final errMsg = errPtr != nullptr ? errPtr.toDartString() : 'unknown';
    print('FAIL: sh_start returned $startResult ($errMsg)');
    if (startResult == -5) {
      print('  macOS accessibility permission NOT granted.');
      print('  Go to System Settings > Privacy > Accessibility and allow Terminal.');
    }
    nativeCallable.close();
    sh_destroy(hook);
    exit(1);
  }
  final isRunning = sh_is_running(hook);
  print('[smoke] sh_start OK (running=$isRunning)');
  print('[smoke] Waiting 15 seconds. Select text in other apps (browser, terminal, editor)...');

  await Future.delayed(const Duration(seconds: 15));

  print('[smoke] Stopping hook...');
  final stopResult = sh_stop(hook);
  if (stopResult != 0) {
    print('FAIL: sh_stop returned $stopResult');
  } else {
    print('[smoke] sh_stop OK');
  }

  print('[smoke] Closing NativeCallable...');
  nativeCallable.close();

  print('[smoke] Destroying hook...');
  sh_destroy(hook);

  print('[smoke] Received $eventCount events from real text selection');
  for (final e in events) {
    print('  $e');
  }

  if (eventCount > 0) {
    print('PASS: Got $eventCount real text selection events');
  } else {
    print('INFO: No events — select text manually in other apps and re-run');
  }

  print('[smoke] TEST DONE');
}
