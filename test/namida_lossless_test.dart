import 'dart:io';

import 'package:namida_waveform/namida_lossless.dart';
import 'package:test/test.dart';

/// Points at an audio file to run the check against.
const _envSample = 'NAMIDA_WAVEFORM_TEST_FILE';

void main() {
  final path = Platform.environment[_envSample];

  test('native library loads', () {
    expect(NamidaLossless.nativeVersion, greaterThan(0));
  });

  test('missing file reports an error instead of throwing', () {
    final result = NamidaLossless.check('/definitely/not/here.flac');
    expect(result.error, WaveformError.openInput);
    expect(result.isLossless, isFalse);
  });

  test('measures within the band of the file', () {
    final result = NamidaLossless.check(path!);
    expect(result.error, WaveformError.none);
    if (!result.isLossless) return;
    expect(result.cutoffHz, inInclusiveRange(0, result.sampleRate / 2));
    expect(result.usedBits, lessThanOrEqualTo(32));
  }, skip: path == null ? 'set $_envSample to an audio file to run' : null);
}
