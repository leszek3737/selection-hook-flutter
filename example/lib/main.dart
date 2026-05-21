import 'dart:async';

import 'package:flutter/material.dart';

import 'package:selection_hook_flutter/selection_hook_flutter.dart';

void main() {
  runApp(const SelectionHookExampleApp());
}

class SelectionHookExampleApp extends StatelessWidget {
  const SelectionHookExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Selection Hook Example',
      theme: ThemeData(
        colorSchemeSeed: Colors.blue,
        useMaterial3: true,
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final _hook = SelectionHook.instance;
  final _logController = ScrollController();
  final _log = <String>[];
  bool _isRunning = false;
  StreamSubscription<TextSelectionEvent>? _subscription;

  bool _showMouseEvents = false;
  bool _showKeyboardEvents = false;
  StreamSubscription<MouseEvent>? _mouseSub;
  StreamSubscription<KeyboardEvent>? _keyboardSub;

  @override
  void initState() {
    super.initState();
    _subscription = _hook.onTextSelection.listen((event) {
      setState(() {
        _log.add(
          '[${DateTime.now().toIso8601String().substring(11, 19)}] '
          '[TEXT] '
          '"${event.text.length > 60 ? '${event.text.substring(0, 60)}...' : event.text}" '
          'in ${event.programName}',
        );
      });
      // Auto-scroll to bottom.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_logController.hasClients) {
          _logController.jumpTo(_logController.position.maxScrollExtent);
        }
      });
    });
  }

  @override
  void dispose() {
    _subscription?.cancel();
    _mouseSub?.cancel();
    _keyboardSub?.cancel();
    _hook.dispose();
    _logController.dispose();
    super.dispose();
  }

  Future<void> _toggle() async {
    if (_isRunning) {
      _mouseSub?.cancel();
      _keyboardSub?.cancel();
      _mouseSub = null;
      _keyboardSub = null;
      await _hook.stop();
      setState(() => _isRunning = false);
      _addLog('Stopped');
    } else {
      try {
        await _hook.start();
        setState(() => _isRunning = true);
        _addLog('Started — select text in other apps');
        if (_showMouseEvents) {
          _mouseSub = _hook.onMouseEvent.listen(_onMouse);
        }
        if (_showKeyboardEvents) {
          _keyboardSub = _hook.onKeyboardEvent.listen(_onKey);
        }
      } on StateError catch (e) {
        _addLog('Error: $e');
      }
    }
  }

  void _addLog(String msg) {
    setState(() {
      _log.add('[${DateTime.now().toIso8601String().substring(11, 19)}] $msg');
    });
  }

  void _onMouse(MouseEvent e) {
    _log.add(
      '[${DateTime.now().toIso8601String().substring(11, 19)}] '
      '[MOUSE] ${e.toString()}',
    );
    setState(() {});
  }

  void _onKey(KeyboardEvent e) {
    _log.add(
      '[${DateTime.now().toIso8601String().substring(11, 19)}] '
      '[KEY] key="${e.uniKey}" vk=${e.vkCode}',
    );
    setState(() {});
  }

  void _readClipboard() {
    final text = _hook.readClipboard();
    _addLog(text != null ? '[CLIP] "$text"' : '[CLIP] empty');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Selection Hook'),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 16),
            child: Center(
              child: Chip(
                label: Text(_isRunning ? 'Running' : 'Stopped'),
                backgroundColor:
                    _isRunning ? Colors.green.shade100 : Colors.grey.shade300,
              ),
            ),
          ),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              children: [
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.icon(
                        onPressed: _toggle,
                        icon: Icon(_isRunning ? Icons.stop : Icons.play_arrow),
                        label: Text(_isRunning ? 'Stop' : 'Start'),
                      ),
                    ),
                    const SizedBox(width: 12),
                    IconButton(
                      icon: const Icon(Icons.content_paste),
                      tooltip: 'Read Clipboard',
                      onPressed: _readClipboard,
                    ),
                    const SizedBox(width: 12),
                    OutlinedButton(
                      onPressed: () {
                        setState(() => _log.clear());
                      },
                      child: const Text('Clear'),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    CheckboxMenuButton(
                      value: _showMouseEvents,
                      onChanged: (v) {
                        setState(() => _showMouseEvents = v!);
                        if (_isRunning) {
                          if (_showMouseEvents && _mouseSub == null) {
                            _mouseSub = _hook.onMouseEvent.listen(_onMouse);
                          } else if (!_showMouseEvents) {
                            _mouseSub?.cancel();
                            _mouseSub = null;
                          }
                        }
                      },
                      child: const Text('Mouse events'),
                    ),
                    CheckboxMenuButton(
                      value: _showKeyboardEvents,
                      onChanged: (v) {
                        setState(() => _showKeyboardEvents = v!);
                        if (_isRunning) {
                          if (_showKeyboardEvents && _keyboardSub == null) {
                            _keyboardSub = _hook.onKeyboardEvent.listen(_onKey);
                          } else if (!_showKeyboardEvents) {
                            _keyboardSub?.cancel();
                            _keyboardSub = null;
                          }
                        }
                      },
                      child: const Text('Keyboard events'),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const Divider(height: 1),
          Expanded(
            child: _log.isEmpty
                ? const Center(
                    child: Text(
                      'Press Start, then select text in other apps.',
                      style: TextStyle(color: Colors.grey),
                    ),
                  )
                : ListView.builder(
                    controller: _logController,
                    padding: const EdgeInsets.all(8),
                    itemCount: _log.length,
                    itemBuilder: (context, index) {
                      return Padding(
                        padding: const EdgeInsets.symmetric(vertical: 2),
                        child: Text(
                          _log[index],
                          style: const TextStyle(
                            fontFamily: 'monospace',
                            fontSize: 13,
                          ),
                        ),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
