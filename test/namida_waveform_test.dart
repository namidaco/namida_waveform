import 'dart:io';

import 'package:namida_waveform/namida_waveform.dart';
import 'package:test/test.dart';

/// Points at an audio file to run the extraction tests against.
const _envSample = 'NAMIDA_WAVEFORM_TEST_FILE';

void main() {
  final path = Platform.environment[_envSample];

  test('native library loads', () {
    expect(NamidaWaveform.nativeVersion, greaterThan(0));
  });

  test('missing file reports an error instead of throwing', () {
    final result = NamidaWaveform.extract('/definitely/not/here.mp3');
    expect(result.error, WaveformError.openInput);
    expect(result.values, isEmpty);
  });

  test('sampleRateForDuration falls off with duration', () {
    expect(NamidaWaveform.sampleRateForDuration(audioDuration: const Duration(minutes: 1)), 315);
    expect(NamidaWaveform.sampleRateForDuration(audioDuration: const Duration(minutes: 2)), 248);
    expect(NamidaWaveform.sampleRateForDuration(audioDuration: const Duration(minutes: 10)), 36);
  });

  group('extraction', () {
    test('honours the requested rate and stays in range', () {
      final result = NamidaWaveform.extract(path!, samplesPerSecond: 100);
      expect(result.error, WaveformError.none);
      expect(result.duration, greaterThan(Duration.zero));
      expect(result.sampleRate, greaterThan(0));

      final expected = result.duration.inMilliseconds * 100 / 1000;
      expect(result.values.length, closeTo(expected, expected * 0.01));

      for (final value in result.values) {
        expect(value, inInclusiveRange(0, 100));
      }
      expect(result.values.reduce((a, b) => a > b ? a : b), greaterThan(0));
    });

    test('rate is independent of the requested resolution', () {
      final low = NamidaWaveform.extract(path!, samplesPerSecond: 20);
      final high = NamidaWaveform.extract(path, samplesPerSecond: 200);
      expect(high.values.length / low.values.length, closeTo(10, 0.1));
    });
  }, skip: path == null ? 'set $_envSample to an audio file to run' : null);
}
