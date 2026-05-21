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

  @override
  void initState() {
    super.initState();
    _hook.onTextSelection.listen((event) {
      setState(() {
        _log.add(
          '[${DateTime.now().toIso8601String().substring(11, 19)}] '
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
    _hook.dispose();
    _logController.dispose();
    super.dispose();
  }

  Future<void> _toggle() async {
    if (_isRunning) {
      await _hook.stop();
      setState(() => _isRunning = false);
      _addLog('Stopped');
    } else {
      try {
        await _hook.start();
        setState(() => _isRunning = true);
        _addLog('Started — select text in other apps');
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
            child: Row(
              children: [
                Expanded(
                  child: FilledButton.icon(
                    onPressed: _toggle,
                    icon: Icon(_isRunning ? Icons.stop : Icons.play_arrow),
                    label: Text(_isRunning ? 'Stop' : 'Start'),
                  ),
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
