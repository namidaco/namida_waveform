import 'dart:io';

import 'package:namida_waveform/namida_spectrum.dart';
import 'package:test/test.dart';

/// Points at an audio file to run the extraction tests against.
const _envSample = 'NAMIDA_WAVEFORM_TEST_FILE';

void main() {
  final path = Platform.environment[_envSample];

  test('native library loads', () {
    expect(NamidaSpectrum.nativeVersion, greaterThan(0));
  });

  test('missing file reports an error instead of throwing', () {
    final result = NamidaSpectrum.extract('/definitely/not/here.mp3');
    expect(result.error, WaveformError.openInput);
    expect(result.rows, isEmpty);
  });

  group('extraction', () {
    test('honours the requested rate and band count', () {
      final result = NamidaSpectrum.extract(path!, framesPerSecond: 30, bandCount: 16);
      expect(result.error, WaveformError.none);
      expect(result.bandCount, 16);
      expect(result.framesPerSecond, 30);
      expect(result.rows.length, result.frameCount * result.stride);

      final expected = result.duration.inMilliseconds * 30 / 1000;
      expect(result.frameCount, closeTo(expected, expected * 0.01 + 1));
      expect(result.rows.reduce((a, b) => a > b ? a : b), greaterThan(0));
    });

    test('frame count follows the requested rate', () {
      final low = NamidaSpectrum.extract(path!, framesPerSecond: 10, bandCount: 8);
      final high = NamidaSpectrum.extract(path, framesPerSecond: 60, bandCount: 8);
      expect(high.frameCount / low.frameCount, closeTo(6, 0.1));
    });
  }, skip: path == null ? 'set $_envSample to an audio file to run' : null);
}
