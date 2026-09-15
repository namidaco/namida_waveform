import 'dart:io';
import 'dart:typed_data';

import 'package:namida_waveform/namida_palette.dart';
import 'package:test/test.dart';

/// Points at an image file to run the extraction tests against.
const _envSample = 'NAMIDA_PALETTE_TEST_FILE';

void main() {
  final path = Platform.environment[_envSample];

  test('native library loads', () {
    expect(NamidaPalette.nativeVersion, greaterThan(0));
  });

  test('missing file reports an error instead of throwing', () {
    final result = NamidaPalette.extract(path: '/definitely/not/here.png');
    expect(result.error, PaletteError.openInput);
    expect(result.colors, isEmpty);
  });

  test('garbage bytes report an error instead of throwing', () async {
    final result = await NamidaPalette.extractAsync(bytes: Uint8List.fromList(List.filled(4096, 0x42)));
    expect(result.error, isNot(PaletteError.none));
    expect(result.colors, isEmpty);
  });

  group('extraction', () {
    test('path and bytes produce the same palette', () async {
      final fromPath = NamidaPalette.extract(path: path!, maxColors: 28);
      expect(fromPath.error, PaletteError.none);
      expect(fromPath.colors, isNotEmpty);
      expect(fromPath.colors.length, lessThanOrEqualTo(28));
      expect(fromPath.colors.length, fromPath.populations.length);
      for (final color in fromPath.colors) {
        expect(color >> 24, 0xFF);
      }
      for (int i = 1; i < fromPath.populations.length; i++) {
        expect(fromPath.populations[i], lessThanOrEqualTo(fromPath.populations[i - 1]));
      }

      final fromBytes = await NamidaPalette.extractAsync(bytes: File(path).readAsBytesSync(), maxColors: 28);
      expect(fromBytes.error, PaletteError.none);
      expect(fromBytes.colors, fromPath.colors);
      expect(fromBytes.populations, fromPath.populations);
    });

    test('concurrent async requests each get their own result', () async {
      final results = await Future.wait(List.generate(8, (_) => NamidaPalette.extractAsync(path: path!, maxColors: 8)));
      for (final result in results) {
        expect(result.error, PaletteError.none);
        expect(result.colors, results.first.colors);
      }
    });
  }, skip: path == null ? 'set $_envSample to an image file' : null);
}
