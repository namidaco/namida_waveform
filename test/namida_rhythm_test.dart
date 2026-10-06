import 'dart:io';

import 'package:namida_waveform/namida_rhythm.dart';
import 'package:test/test.dart';

/// Points at an audio file to run the analysis tests against.
const _envSample = 'NAMIDA_WAVEFORM_TEST_FILE';

void main() {
  final path = Platform.environment[_envSample];

  test('native library loads', () {
    expect(NamidaRhythm.nativeVersion, greaterThan(0));
  });

  test('missing file reports an error instead of throwing', () {
    final result = NamidaRhythm.analyze('/definitely/not/here.mp3');
    expect(result.error, WaveformError.openInput);
    expect(result.bpm, 0);
    expect(result.key, -1);
  });

  group('analysis', () {
    test('finds a grid within the audible range', () {
      final result = NamidaRhythm.analyze(path!);
      expect(result.error, WaveformError.none);
      expect(result.audibleEndMS, greaterThan(result.audibleStartMS));
      expect(result.fadeOutStartMS, inInclusiveRange(result.audibleStartMS, result.audibleEndMS));
      if (result.bpm > 0) {
        expect(result.bpm, inInclusiveRange(40, 250));
        expect(result.beatOffsetMS, lessThan(60000 / result.bpm));
      }
    });

    test('the hint settles the octave', () {
      final free = NamidaRhythm.analyze(path!);
      if (free.bpm <= 0) return;
      final doubled = NamidaRhythm.analyze(path, bpmHint: free.bpm * 2);
      if (doubled.bpm > 0) expect(doubled.bpm, closeTo(free.bpm * 2, free.bpm * 0.05));
    });
  }, skip: path == null ? 'set $_envSample to an audio file to run' : null);
}
