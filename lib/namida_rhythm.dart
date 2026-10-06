// by claude
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'namida_waveform.dart' show WaveformError;
import 'src/bindings.dart' as bindings;

export 'namida_waveform.dart' show WaveformError;

/// The result of one analysis.
class RhythmData {
  /// Beats per minute of the steady grid fitted over the whole track, `0`
  /// when no steady beat was found.
  final double bpm;

  /// Position of a beat of that grid within the first beat period. Every other
  /// beat sits a whole number of periods away.
  final double beatOffsetMS;

  /// How well one steady grid fits the whole track, `0..1`. Tracks drifting in
  /// tempo, or with no clear pulse, score low.
  final double beatConfidence;

  /// `0..11` for the major keys C to B, `12..23` for the minor keys C to B,
  /// `-1` when the track carries no pitch to tell.
  final int key;

  /// Correlation of the pitch classes of the track with the profile of [key],
  /// `0..1`.
  final double keyConfidence;

  /// Where the audio first rises above, and last falls below, silence.
  final int audibleStartMS;
  final int audibleEndMS;

  /// Where the track settles into its fade out or quiet outro, [audibleEndMS]
  /// when it ends at its usual loudness.
  final int fadeOutStartMS;

  /// Duration reported by the container, [Duration.zero] when it reports none.
  final Duration duration;

  /// Decoder output sample rate.
  final int sampleRate;

  /// [WaveformError.none] on success. The fields above may still describe the
  /// part decoded before the error.
  final WaveformError error;

  const RhythmData({
    required this.bpm,
    required this.beatOffsetMS,
    required this.beatConfidence,
    required this.key,
    required this.keyConfidence,
    required this.audibleStartMS,
    required this.audibleEndMS,
    required this.fadeOutStartMS,
    required this.duration,
    required this.sampleRate,
    required this.error,
  });

  static const empty = RhythmData(
    bpm: 0,
    beatOffsetMS: 0,
    beatConfidence: 0,
    key: -1,
    keyConfidence: 0,
    audibleStartMS: 0,
    audibleEndMS: 0,
    fadeOutStartMS: 0,
    duration: Duration.zero,
    sampleRate: 0,
    error: WaveformError.unknown,
  );

  @override
  String toString() =>
      'RhythmData(bpm: $bpm, beatOffset: ${beatOffsetMS}ms, beatConfidence: $beatConfidence, key: $key, keyConfidence: $keyConfidence, '
      'audible: $audibleStartMS..${audibleEndMS}ms, fadeOutStart: ${fadeOutStartMS}ms, duration: $duration, sampleRate: $sampleRate, error: $error)';
}

abstract final class NamidaRhythm {
  /// Decodes [path] and finds its tempo, beat grid and key.
  ///
  /// [bpmHint] is a tempo known from elsewhere, such as a tag. It settles which
  /// of the tempos a beat implies (half, double) is reported, the exact value
  /// still comes from the audio.
  ///
  /// This blocks the calling isolate for the duration of the decode; call it
  /// from a worker isolate.
  static RhythmData analyze(String path, {double bpmHint = 0}) {
    final pathPtr = path.toNativeUtf8();
    Pointer<bindings.NRResult> resultPtr = nullptr;
    try {
      resultPtr = bindings.nrAnalyze(pathPtr.cast(), bpmHint);
      if (resultPtr == nullptr) return RhythmData.empty;

      final result = resultPtr.ref;
      return RhythmData(
        bpm: result.bpm,
        beatOffsetMS: result.beatOffsetMs,
        beatConfidence: result.beatConfidence,
        key: result.key,
        keyConfidence: result.keyConfidence,
        audibleStartMS: result.audibleStartMs,
        audibleEndMS: result.audibleEndMs,
        fadeOutStartMS: result.fadeOutStartMs,
        duration: Duration(milliseconds: result.durationMs),
        sampleRate: result.sampleRate,
        error: WaveformError.fromCode(result.error),
      );
    } finally {
      if (resultPtr != nullptr) bindings.nrResultFree(resultPtr);
      calloc.free(pathPtr);
    }
  }

  /// Version of the analysis the loaded native library runs, bumped whenever
  /// the same input would give a different result. Throws if it cannot be
  /// loaded, or when the library predates the analysis.
  static int get nativeVersion => bindings.nrVersion();
}
