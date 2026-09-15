import 'dart:ffi';
import 'dart:isolate';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'src/bindings.dart' as bindings;

/// Why an extraction produced no usable data.
enum WaveformError {
  none(0),
  allocation(1),
  openInput(2),
  streamInfo(3),
  noAudioStream(4),
  noDecoder(5),
  decoderOpen(6),
  unsupportedFormat(7),
  noOutput(8),
  unknown(-1);

  const WaveformError(this.code);

  final int code;

  static WaveformError fromCode(int code) {
    for (final value in values) {
      if (value.code == code) return value;
    }
    return WaveformError.unknown;
  }
}

/// The result of one extraction.
class WaveformData {
  /// RMS amplitudes in the `0..100` range, [samplesPerSecond] entries per
  /// second of audio.
  final Float32List values;

  /// Duration reported by the container, [Duration.zero] when it reports none.
  final Duration duration;

  /// Decoder output sample rate.
  final int sampleRate;

  /// Decoder output channel count.
  final int channels;

  /// [WaveformError.none] on success. [values] may still hold a partial result
  /// when decoding stopped early.
  final WaveformError error;

  const WaveformData({
    required this.values,
    required this.duration,
    required this.sampleRate,
    required this.channels,
    required this.error,
  });

  static final empty = WaveformData(
    values: Float32List(0),
    duration: Duration.zero,
    sampleRate: 0,
    channels: 0,
    error: WaveformError.unknown,
  );

  bool get isEmpty => values.isEmpty;
  bool get isNotEmpty => values.isNotEmpty;

  @override
  String toString() => 'WaveformData(count: ${values.length}, duration: $duration, '
      'sampleRate: $sampleRate, channels: $channels, error: $error)';
}

abstract final class NamidaWaveform {
  /// Decodes [path] and reduces it to [samplesPerSecond] RMS amplitudes per
  /// second of audio.
  ///
  /// Buckets are cut on sample positions rather than decoder frame boundaries,
  /// so the output rate is honoured exactly for every codec and container.
  ///
  /// This blocks the calling isolate for the duration of the decode; call it
  /// from a worker isolate, or use [extractInIsolate].
  static WaveformData extract(String path, {int samplesPerSecond = 100}) {
    final pathPtr = path.toNativeUtf8();
    Pointer<bindings.NWResult> resultPtr = nullptr;
    try {
      resultPtr = bindings.nwExtract(pathPtr.cast(), samplesPerSecond);
      if (resultPtr == nullptr) return WaveformData.empty;

      final result = resultPtr.ref;
      final count = result.count;
      // Copying detaches the data from the native allocation, which is freed
      // below. At these sizes the copy is far cheaper than keeping the
      // allocation alive behind a finalizer.
      final values = count > 0 ? Float32List.fromList(result.values.asTypedList(count)) : Float32List(0);

      return WaveformData(
        values: values,
        duration: Duration(milliseconds: result.durationMs),
        sampleRate: result.sampleRate,
        channels: result.channels,
        error: WaveformError.fromCode(result.error),
      );
    } finally {
      if (resultPtr != nullptr) bindings.nwResultFree(resultPtr);
      calloc.free(pathPtr);
    }
  }

  /// [extract] on a throwaway isolate, for callers without a worker of their own.
  static Future<WaveformData> extractInIsolate(String path, {int samplesPerSecond = 100}) {
    return Isolate.run(() => extract(path, samplesPerSecond: samplesPerSecond));
  }

  /// Version of the loaded native library. Throws if it cannot be loaded.
  static int get nativeVersion => bindings.nwVersion();

  /// Returns a sample rate inversely proportional to [audioDuration], so that
  /// long files do not produce needlessly large waveforms.
  ///
  /// With a [scaleFactor] of `0.4`:
  /// - 1 minute audio => 315 samples.
  /// - 2 minute audio => 248 samples.
  /// - 10 minute audio => 36 samples.
  static int sampleRateForDuration({
    required Duration audioDuration,
    int maxSampleRate = 400,
    double scaleFactor = 0.4,
  }) {
    final scaledDuration = scaleFactor * audioDuration.inSeconds;
    final scaledSampleRate = maxSampleRate * math.exp(-scaledDuration / 100);
    return scaledSampleRate.clamp(1, maxSampleRate).round();
  }
}
