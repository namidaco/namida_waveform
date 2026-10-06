// by claude
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'namida_waveform.dart' show WaveformError;
import 'src/bindings.dart' as bindings;

export 'namida_waveform.dart' show WaveformError;

/// The result of one check.
class LosslessCheck {
  /// Whether the codec is a lossless one, only those are analyzed.
  final bool isLossless;

  /// Steepest edge over 8kHz the spectrum never climbs back from.
  final double cutoffHz;

  /// How far the level falls across [cutoffHz]: tens of decibels at a lossy
  /// encoder's or a resampler's lowpass, a few where music rolls off on its own.
  final double cutoffDropDB;

  final int sampleRate;

  /// Bits per sample the file stores.
  final int storedBits;

  /// Bits per sample that ever carry anything, 0 when unknown. Fewer than
  /// [storedBits] means the samples were padded.
  final int usedBits;

  /// Duration reported by the container, [Duration.zero] when it reports none.
  final Duration duration;

  final WaveformError error;

  const LosslessCheck({
    required this.isLossless,
    required this.cutoffHz,
    required this.cutoffDropDB,
    required this.sampleRate,
    required this.storedBits,
    required this.usedBits,
    required this.duration,
    required this.error,
  });

  static const empty = LosslessCheck(
    isLossless: false,
    cutoffHz: 0,
    cutoffDropDB: 0,
    sampleRate: 0,
    storedBits: 0,
    usedBits: 0,
    duration: Duration.zero,
    error: WaveformError.unknown,
  );

  @override
  String toString() =>
      'LosslessCheck(isLossless: $isLossless, cutoff: ${cutoffHz}Hz, drop: ${cutoffDropDB}dB, sampleRate: $sampleRate, '
      'bits: $usedBits/$storedBits, duration: $duration, error: $error)';
}

abstract final class NamidaLossless {
  /// Reads slices spread over [path] and measures what a lossless file built
  /// from a lossy, lower rate or lower depth source gives away. Lossy files are
  /// only opened, never decoded.
  ///
  /// This blocks the calling isolate for the duration of the decode; call it
  /// from a worker isolate.
  static LosslessCheck check(String path) {
    final pathPtr = path.toNativeUtf8();
    Pointer<bindings.NLResult> resultPtr = nullptr;
    try {
      resultPtr = bindings.nlCheck(pathPtr.cast());
      if (resultPtr == nullptr) return LosslessCheck.empty;

      final result = resultPtr.ref;
      return LosslessCheck(
        isLossless: result.lossless != 0,
        cutoffHz: result.cutoffHz,
        cutoffDropDB: result.cutoffDropDb,
        sampleRate: result.sampleRate,
        storedBits: result.storedBits,
        usedBits: result.usedBits,
        duration: Duration(milliseconds: result.durationMs),
        error: WaveformError.fromCode(result.error),
      );
    } finally {
      if (resultPtr != nullptr) bindings.nlResultFree(resultPtr);
      calloc.free(pathPtr);
    }
  }

  /// Version of the check the loaded native library runs. Throws if it cannot
  /// be loaded, or when the library predates the check.
  static int get nativeVersion => bindings.nlVersion();
}
