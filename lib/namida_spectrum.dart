// by claude
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'namida_waveform.dart' show WaveformError;
import 'src/bindings.dart' as bindings;

export 'namida_waveform.dart' show WaveformError;

/// The result of one extraction.
class SpectrumData {
  /// [frameCount] rows of [stride] bytes: the level of every band, low to
  /// high, then how hard a beat lands on that frame.
  final Uint8List rows;

  final int frameCount;
  final int bandCount;
  final int framesPerSecond;

  /// Duration reported by the container, [Duration.zero] when it reports none.
  final Duration duration;

  /// Decoder output sample rate.
  final int sampleRate;

  /// [WaveformError.none] on success. [rows] may still hold a partial result
  /// when decoding stopped early.
  final WaveformError error;

  const SpectrumData({
    required this.rows,
    required this.frameCount,
    required this.bandCount,
    required this.framesPerSecond,
    required this.duration,
    required this.sampleRate,
    required this.error,
  });

  static final empty = SpectrumData(rows: Uint8List(0), frameCount: 0, bandCount: 0, framesPerSecond: 0, duration: Duration.zero, sampleRate: 0, error: WaveformError.unknown);

  int get stride => bandCount + 1;

  bool get isEmpty => frameCount == 0;
  bool get isNotEmpty => frameCount != 0;

  @override
  String toString() =>
      'SpectrumData(frames: $frameCount, bands: $bandCount, fps: $framesPerSecond, '
      'duration: $duration, sampleRate: $sampleRate, error: $error)';
}

abstract final class NamidaSpectrum {
  /// Decodes [path] and reduces it to [framesPerSecond] rows of [bandCount]
  /// log spaced frequency band levels, each followed by a beat strength.
  ///
  /// Frame `i` is centered on `i / framesPerSecond` seconds, so a row can be
  /// looked up by playback position alone. Levels are relative to the loudest
  /// band of the track rather than to full scale.
  ///
  /// This blocks the calling isolate for the duration of the decode; call it
  /// from a worker isolate.
  static SpectrumData extract(String path, {int framesPerSecond = 30, int bandCount = 16}) {
    final pathPtr = path.toNativeUtf8();
    Pointer<bindings.NSResult> resultPtr = nullptr;
    try {
      resultPtr = bindings.nsExtract(pathPtr.cast(), framesPerSecond, bandCount);
      if (resultPtr == nullptr) return SpectrumData.empty;

      final result = resultPtr.ref;
      final frameCount = result.frameCount;
      final resultBandCount = result.bandCount;
      final length = frameCount * (resultBandCount + 1);
      // Copying detaches the data from the native allocation, which is freed below.
      final rows = length > 0 ? Uint8List.fromList(result.data.asTypedList(length)) : Uint8List(0);

      return SpectrumData(
        rows: rows,
        frameCount: frameCount,
        bandCount: resultBandCount,
        framesPerSecond: result.framesPerSecond,
        duration: Duration(milliseconds: result.durationMs),
        sampleRate: result.sampleRate,
        error: WaveformError.fromCode(result.error),
      );
    } finally {
      if (resultPtr != nullptr) bindings.nsResultFree(resultPtr);
      calloc.free(pathPtr);
    }
  }

  /// Version of the rows the loaded native library produces, bumped whenever
  /// the same input would give different ones. Throws if it cannot be loaded.
  static int get nativeVersion => bindings.nsVersion();
}
