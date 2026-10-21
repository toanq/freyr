import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:audioplayers/audioplayers.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:record/record.dart';

import '../common/network_errors.dart';
import '../main.dart';
import 'device_detail_screen.dart';

// "Trò chuyện với Synora" -- one continuous voice conversation with the
// device's assistant endpoint (POST /devices/{id}/assistant/turn), each HTTP
// call being one full STT->LLM(+MCP tool calls)->TTS turn server-side.
//
// State machine (mirrors the same listen/process/speak/silence-timeout
// design already proven on this project's firmware/ESP32 side):
//   idle -> listening -> processing -> speaking -> back to listening (loop)
//   ... until stopped -> idle.
//
// Kept as its own small StatefulWidget, shown in a modal bottom sheet from
// DeviceDetailScreen, rather than folded into _DeviceDetailScreenState --
// that screen is already large and this feature's timers/recorder/player
// lifecycle is entirely self-contained.
enum _TalkState { idle, listening, processing, speaking }

class AssistantTalkSheet extends ConsumerStatefulWidget {
  const AssistantTalkSheet({required this.deviceId, super.key});

  final String deviceId;

  @override
  ConsumerState<AssistantTalkSheet> createState() => _AssistantTalkSheetState();
}

class _AssistantTalkSheetState extends ConsumerState<AssistantTalkSheet> {
  static const _wholeSessionSilenceTimeout = Duration(seconds: 10);
  static const _trailingSilenceTimeout = Duration(milliseconds: 1500);
  static const _quietDb = -50.0;
  static const _loudDb = -10.0;
  static const _ballMinScale = 0.7;
  static const _ballMaxScale = 1.3;

  // A fixed absolute dBFS threshold turned out not to work reliably across
  // devices -- confirmed live: on a real phone, speech never crossed a fixed
  // -35 dBFS guess, so _hasSpeechThisTurn stayed false and the turn always
  // silently hit the whole-session timeout instead of ever responding.
  // Calibrating against this device's own ambient noise floor at the start
  // of each listening period, then requiring a fixed margin above THAT, is
  // robust to whatever gain/noise floor a given phone/room actually has.
  static const _calibrationSampleCount = 4;
  static const _speechMarginDb = 12.0;

  final _recorder = AudioRecorder();
  final _player = AudioPlayer();

  _TalkState _state = _TalkState.idle;
  StreamSubscription<Amplitude>? _amplitudeSub;
  StreamSubscription<void>? _playerCompleteSub;
  Timer? _silenceCheckTimer;
  Timer? _idleCloseTimer;

  static const _idleCloseTimeout = Duration(seconds: 10);

  double _ballScale = 1.0;
  bool _hasSpeechThisTurn = false;
  DateTime? _lastSpeechAt;
  DateTime? _listeningStartedAt;

  final List<double> _calibrationSamples = [];
  double? _noiseFloorDb;

  String? _transcript;
  String? _reply;
  bool _shouldEndAfterReply = false;
  String? _replyAudioPath;

  // Simple substring check on the transcript rather than a server round trip
  // -- ending the session on "goodbye" is a client-side UI decision (stop
  // auto-relistening), not something that needs the LLM's involvement. Not
  // exhaustive; add phrases here if a common one gets missed -- confirmed
  // live that "bai bai" (a very common phonetic Vietnamese rendering of "bye
  // bye", distinct from the literal ASCII "bye bye") was missing and caused
  // a real miss (kept listening instead of ending the session).
  static const _goodbyePhrases = [
    'tạm biệt',
    'chào tạm biệt',
    'hẹn gặp lại',
    'goodbye',
    'bye bye',
    'bye',
    'bai bai',
    'bái bai',
    'bai bye',
  ];

  bool _isGoodbye(String? text) {
    if (text == null) return false;
    final lower = text.toLowerCase();
    return _goodbyePhrases.any(lower.contains);
  }

  @override
  void initState() {
    super.initState();
    _start();
  }

  @override
  void dispose() {
    _silenceCheckTimer?.cancel();
    _idleCloseTimer?.cancel();
    _amplitudeSub?.cancel();
    _playerCompleteSub?.cancel();
    // Best-effort, fire-and-forget -- this widget is being torn down (e.g.
    // the user swiped the sheet away mid-conversation) so there's no state
    // left to update; the record/audioplayers packages serialize their own
    // calls internally, so stop-then-dispose still runs in order even
    // un-awaited here.
    unawaited(_recorder.stop());
    unawaited(_recorder.dispose());
    unawaited(_player.stop());
    unawaited(_player.dispose());
    _deleteReplyAudio();
    super.dispose();
  }

  void _deleteReplyAudio() {
    final path = _replyAudioPath;
    _replyAudioPath = null;
    if (path != null) _deleteQuietly(path);
  }

  static void _deleteQuietly(String path) {
    unawaited(File(path).delete().then((_) {}, onError: (_) {}));
  }

  Future<void> _start() async {
    _idleCloseTimer?.cancel();
    _idleCloseTimer = null;
    final status = await Permission.microphone.request();
    if (!status.isGranted) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Cần quyền micro để trò chuyện với Synora.')),
        );
      }
      return;
    }
    await _startListening();
  }

  Future<void> _startListening() async {
    if (!mounted) return;
    setState(() {
      _state = _TalkState.listening;
      _hasSpeechThisTurn = false;
      _lastSpeechAt = null;
      _listeningStartedAt = DateTime.now();
      _ballScale = 1.0;
      _calibrationSamples.clear();
      _noiseFloorDb = null;
    });

    // Stop may be pressed during either await below; _stopAll() then sets idle.
    final dir = await getTemporaryDirectory();
    if (!mounted || _state != _TalkState.listening) return;
    final path = '${dir.path}/synora_talk_${DateTime.now().millisecondsSinceEpoch}.wav';

    try {
      await _recorder.start(
        const RecordConfig(encoder: AudioEncoder.wav, sampleRate: 16000, numChannels: 1),
        path: path,
      );
    } catch (e) {
      await _stopAll();
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể ghi âm: ${friendlyError(e)}')));
      }
      return;
    }
    if (!mounted || _state != _TalkState.listening) {
      await _stopAll();
      return;
    }

    await _amplitudeSub?.cancel();
    _amplitudeSub = _recorder.onAmplitudeChanged(const Duration(milliseconds: 200)).listen(_onAmplitude);

    _silenceCheckTimer?.cancel();
    _silenceCheckTimer = Timer.periodic(const Duration(milliseconds: 200), (_) => _checkSilence());
  }

  void _onAmplitude(Amplitude amplitude) {
    if (!mounted || _state != _TalkState.listening) return;
    final db = amplitude.current;
    final clamped = db.clamp(_quietDb, _loudDb);
    final t = (clamped - _quietDb) / (_loudDb - _quietDb);

    // First few readings of each listening period calibrate the "quiet"
    // floor for this device/room, rather than assuming a fixed absolute
    // dBFS value works everywhere -- see the class doc comment on
    // _speechMarginDb for why.
    if (_noiseFloorDb == null) {
      _calibrationSamples.add(db);
      if (_calibrationSamples.length >= _calibrationSampleCount) {
        _noiseFloorDb = _calibrationSamples.reduce((a, b) => a + b) / _calibrationSamples.length;
        debugPrint('[AssistantTalk] calibrated noise floor = ${_noiseFloorDb!.toStringAsFixed(1)} dBFS '
            '(speech threshold = ${(_noiseFloorDb! + _speechMarginDb).toStringAsFixed(1)} dBFS)');
      }
      setState(() => _ballScale = _ballMinScale + t * (_ballMaxScale - _ballMinScale));
      return;
    }

    final isSpeech = db > _noiseFloorDb! + _speechMarginDb;
    debugPrint('[AssistantTalk] amplitude=${db.toStringAsFixed(1)} dBFS isSpeech=$isSpeech '
        'hasSpeechThisTurn=$_hasSpeechThisTurn');
    setState(() {
      _ballScale = _ballMinScale + t * (_ballMaxScale - _ballMinScale);
      if (isSpeech) {
        _hasSpeechThisTurn = true;
        _lastSpeechAt = DateTime.now();
      }
    });
  }

  void _checkSilence() {
    if (_state != _TalkState.listening) return;
    final now = DateTime.now();

    if (!_hasSpeechThisTurn) {
      // Whole-session silence timeout -- the user never said anything, end
      // the whole conversation rather than sending an empty utterance.
      final startedAt = _listeningStartedAt;
      if (startedAt != null && now.difference(startedAt) >= _wholeSessionSilenceTimeout) {
        debugPrint('[AssistantTalk] whole-session silence timeout, no speech ever detected -- stopping');
        _stopAll();
      }
      return;
    }

    // End-of-utterance: trailing silence after speech was detected.
    final lastSpeechAt = _lastSpeechAt;
    if (lastSpeechAt != null && now.difference(lastSpeechAt) >= _trailingSilenceTimeout) {
      debugPrint('[AssistantTalk] trailing silence after speech -- ending utterance, sending to server');
      _finishUtterance();
    }
  }

  Future<void> _finishUtterance() async {
    _silenceCheckTimer?.cancel();
    await _amplitudeSub?.cancel();
    final path = await _recorder.stop();
    if (!mounted) return;
    if (path == null) {
      await _stopAll();
      return;
    }
    await _processRecording(path);
  }

  Future<void> _processRecording(String path) async {
    setState(() => _state = _TalkState.processing);
    try {
      final result = await ref.read(apiClientProvider).assistantTurn(widget.deviceId, File(path));
      if (!mounted) return;
      setState(() {
        _transcript = result['transcript'] as String?;
        _reply = result['reply'] as String?;
        _shouldEndAfterReply = _isGoodbye(_transcript);
      });
      await _speak(result['replyAudioWavBase64'] as String?);
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể trò chuyện: ${friendlyError(e)}')));
        setState(() => _state = _TalkState.idle);
      }
    } finally {
      _deleteQuietly(path);
    }
  }

  Future<void> _speak(String? replyAudioBase64) async {
    if (!mounted) return;
    setState(() => _state = _TalkState.speaking);

    if (replyAudioBase64 == null) {
      // TTS failed server-side -- no audio to play, just give the user a
      // moment to read the reply text before looping back to listening.
      await Future.delayed(const Duration(seconds: 2));
      if (!mounted || _state != _TalkState.speaking) return;
      await _continueOrStop();
      return;
    }

    try {
      // BytesSource isn't supported by audioplayers_darwin, so play from a temp file instead.
      final bytes = base64Decode(replyAudioBase64);
      final dir = await getTemporaryDirectory();
      _deleteReplyAudio();
      final replyPath = '${dir.path}/synora_reply_${DateTime.now().millisecondsSinceEpoch}.wav';
      await File(replyPath).writeAsBytes(bytes, flush: true);
      _replyAudioPath = replyPath;
      if (!mounted || _state != _TalkState.speaking) {
        _deleteReplyAudio();
        return;
      }
      await _playerCompleteSub?.cancel();
      _playerCompleteSub = _player.onPlayerComplete.listen((_) {
        _deleteReplyAudio();
        if (!mounted || _state != _TalkState.speaking) return;
        _continueOrStop();
      });
      await _player.play(DeviceFileSource(replyPath, mimeType: 'audio/wav'));
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể phát âm thanh trả lời: ${friendlyError(e)}')));
        if (_state == _TalkState.speaking) await _continueOrStop();
      }
    }
  }

  // After a reply finishes playing (or is skipped): back to listening for
  // the next turn, unless the user just said goodbye -- then end the whole
  // session instead of auto-relistening on top of a farewell.
  Future<void> _continueOrStop() async {
    if (_shouldEndAfterReply) {
      await _stopAll();
      return;
    }
    await _startListening();
  }

  // Stop button -- available in any non-idle state. Cancels any pending
  // timer, stops recording/playback if active, and returns to idle
  // regardless of which state it was pressed from.
  Future<void> _stopAll() async {
    _silenceCheckTimer?.cancel();
    _silenceCheckTimer = null;
    await _amplitudeSub?.cancel();
    _amplitudeSub = null;
    await _playerCompleteSub?.cancel();
    _playerCompleteSub = null;
    try {
      if (await _recorder.isRecording()) {
        final recordedPath = await _recorder.stop();
        if (recordedPath != null) _deleteQuietly(recordedPath);
      }
    } catch (_) {
      // Non-fatal -- already stopped/never started.
    }
    try {
      await _player.stop();
    } catch (_) {
      // Non-fatal.
    }
    _deleteReplyAudio();
    if (mounted) setState(() => _state = _TalkState.idle);

    // Auto-dismiss the sheet itself if the user doesn't do anything within a
    // few seconds of going idle (session ended by timeout/goodbye/explicit
    // stop) -- otherwise it just sits open until manually swiped away.
    // Guarded on _state still being idle when this fires, in case the user
    // taps "Bắt đầu trò chuyện" again in the meantime (_start() also cancels
    // this timer directly, but the guard covers any race regardless).
    _idleCloseTimer?.cancel();
    _idleCloseTimer = Timer(_idleCloseTimeout, () {
      if (mounted && _state == _TalkState.idle) {
        Navigator.of(context).pop();
      }
    });
  }

  String _stateLabel() {
    switch (_state) {
      case _TalkState.idle:
        return 'Nhấn để bắt đầu trò chuyện với Synora.';
      case _TalkState.listening:
        return 'Đang nghe...';
      case _TalkState.processing:
        return 'Đang xử lý...';
      case _TalkState.speaking:
        return 'Đang trả lời...';
    }
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.fromLTRB(20, 12, 20, 24),
      decoration: const BoxDecoration(
        color: DeviceDetailScreen.cardColor,
        borderRadius: BorderRadius.vertical(top: Radius.circular(24)),
      ),
      child: SafeArea(
        top: false,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 40,
              height: 4,
              decoration: BoxDecoration(color: Colors.white24, borderRadius: BorderRadius.circular(2)),
            ),
            const SizedBox(height: 24),
            _buildBall(),
            const SizedBox(height: 16),
            Text(_stateLabel(),
                textAlign: TextAlign.center,
                style: const TextStyle(color: Colors.white, fontSize: 16, fontWeight: FontWeight.w600)),
            if (_transcript != null || _reply != null) ...[
              const SizedBox(height: 16),
              _buildTranscriptLog(),
            ],
            const SizedBox(height: 24),
            if (_state == _TalkState.idle)
              FilledButton.icon(
                style: FilledButton.styleFrom(
                  backgroundColor: DeviceDetailScreen.goodColor,
                  foregroundColor: DeviceDetailScreen.backgroundColor,
                ),
                onPressed: _start,
                icon: const Icon(Icons.mic),
                label: const Text('Bắt đầu trò chuyện'),
              )
            else
              OutlinedButton.icon(
                onPressed: _stopAll,
                icon: const Icon(Icons.stop_circle_outlined),
                label: const Text('Dừng'),
              ),
          ],
        ),
      ),
    );
  }

  Widget _buildBall() {
    const size = 140.0;

    if (_state == _TalkState.processing) {
      return const SizedBox(
        width: size,
        height: size,
        child: Center(
          child: SizedBox(
            width: 48,
            height: 48,
            child: CircularProgressIndicator(color: DeviceDetailScreen.goodColor, strokeWidth: 3),
          ),
        ),
      );
    }

    final scale = _state == _TalkState.listening ? _ballScale : 1.0;
    return AnimatedScale(
      scale: scale,
      duration: const Duration(milliseconds: 120),
      curve: Curves.easeOut,
      child: Container(
        width: size,
        height: size,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: DeviceDetailScreen.goodColor,
          boxShadow: [
            BoxShadow(
              color: DeviceDetailScreen.goodColor.withValues(alpha: _state == _TalkState.idle ? 0.2 : 0.45),
              blurRadius: 30,
              spreadRadius: 6,
            ),
          ],
        ),
        child: Icon(
          _state == _TalkState.speaking ? Icons.volume_up : Icons.mic,
          color: DeviceDetailScreen.backgroundColor,
          size: 48,
        ),
      ),
    );
  }

  Widget _buildTranscriptLog() {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(color: DeviceDetailScreen.backgroundColor, borderRadius: BorderRadius.circular(12)),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (_transcript != null)
            Text('Bạn: $_transcript', style: const TextStyle(color: Colors.white70, fontSize: 13)),
          if (_transcript != null && _reply != null) const SizedBox(height: 6),
          if (_reply != null) Text('Synora: $_reply', style: const TextStyle(color: Colors.white, fontSize: 13)),
        ],
      ),
    );
  }
}
