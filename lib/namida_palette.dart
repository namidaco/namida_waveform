import 'dart:async';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'src/bindings.dart' as bindings;

/// Why an extraction produced no palette.
enum PaletteError {
  none(0),
  allocation(1),
  openInput(2),
  noVideoStream(3),
  noDecoder(4),
  decoderOpen(5),
  decode(6),
  unsupportedFormat(7),
  noOutput(8),
  thread(9),
  unknown(-1);

  const PaletteError(this.code);

  final int code;

  static PaletteError fromCode(int code) {
    for (final value in values) {
      if (value.code == code) return value;
    }
    return PaletteError.unknown;
  }
}

/// The result of one extraction.
class PaletteData {
  /// `0xAARRGGBB` colors with alpha `0xFF`, most populous first.
  final Uint32List colors;

  /// How many sampled pixels fell into each entry of [colors].
  final Uint32List populations;

  /// Size of the decoded picture the palette was sampled from. JPEGs decode at
  /// a power-of-two fraction of the file's size when that still covers the
  /// sampling grid.
  final int width;
  final int height;

  /// [PaletteError.none] on success, in which case [colors] is not empty.
  final PaletteError error;

  const PaletteData({required this.colors, required this.populations, required this.width, required this.height, required this.error});

  PaletteData.failed(this.error) : colors = Uint32List(0), populations = Uint32List(0), width = 0, height = 0;

  bool get isEmpty => colors.isEmpty;
  bool get isNotEmpty => colors.isNotEmpty;

  @override
  String toString() => 'PaletteData(count: ${colors.length}, size: ${width}x$height, error: $error)';
}

abstract final class NamidaPalette {
  /// Decodes one picture and reduces it to at most [maxColors] colors with the
  /// median-cut quantizer Android's Palette uses. The picture is box-averaged
  /// onto a grid at most [maxHeight] rows tall, its width following the aspect
  /// ratio, so every source pixel weighs in on the histogram.
  ///
  /// The picture comes from [bytes] when given, otherwise from [path]. Every
  /// still image format the bundled libavcodec carries is accepted; the
  /// container is detected from the bytes, never from the file name.
  ///
  /// This blocks the calling isolate for the duration of the decode; prefer
  /// [extractAsync], which never touches an isolate.
  static PaletteData extract({String? path, Uint8List? bytes, int maxColors = 16, int maxHeight = 240}) {
    assert(path != null || bytes != null, 'a path or bytes must be provided');
    final pathPtr = path == null ? nullptr : path.toNativeUtf8().cast<Char>();
    final dataPtr = bytes == null || bytes.isEmpty ? nullptr : calloc<Uint8>(bytes.length);
    if (dataPtr != nullptr) dataPtr.asTypedList(bytes!.length).setAll(0, bytes);
    Pointer<bindings.NPResult> resultPtr = nullptr;
    try {
      resultPtr = bindings.npExtract(pathPtr, dataPtr, dataPtr == nullptr ? 0 : bytes!.length, maxColors, maxHeight);
      return _read(resultPtr);
    } finally {
      if (resultPtr != nullptr) bindings.npResultFree(resultPtr);
      if (dataPtr != nullptr) calloc.free(dataPtr);
      if (pathPtr != nullptr) calloc.free(pathPtr);
    }
  }

  /// [extract] on a native thread of its own: nothing runs on a Dart isolate
  /// but the completion, and [bytes] is read in place rather than copied
  /// through an isolate message.
  static Future<PaletteData> extractAsync({String? path, Uint8List? bytes, int maxColors = 16, int maxHeight = 240}) {
    assert(path != null || bytes != null, 'a path or bytes must be provided');
    final listener = _listener ??= NativeCallable<bindings.NPCallback>.listener(_onResult)..keepIsolateAlive = false;
    final requestId = ++_lastRequestId;
    final completer = Completer<PaletteData>();
    _pending[requestId] = completer;

    final pathPtr = path == null ? nullptr : path.toNativeUtf8().cast<Char>();
    final int status;
    try {
      status = bytes != null && bytes.isNotEmpty
          ? bindings.npExtractAsync(pathPtr, bytes.address, bytes.length, maxColors, maxHeight, requestId, listener.nativeFunction)
          : bindings.npExtractAsync(pathPtr, nullptr, 0, maxColors, maxHeight, requestId, listener.nativeFunction);
    } finally {
      if (pathPtr != nullptr) calloc.free(pathPtr);
    }

    if (status != 0) {
      _pending.remove(requestId);
      return Future.value(PaletteData.failed(PaletteError.fromCode(status)));
    }
    return completer.future;
  }

  /// Version of the palette API in the loaded native library. Throws if it
  /// cannot be loaded.
  static int get nativeVersion => bindings.npVersion();

  static NativeCallable<bindings.NPCallback>? _listener;
  static int _lastRequestId = 0;
  static final _pending = <int, Completer<PaletteData>>{};

  static void _onResult(int requestId, Pointer<bindings.NPResult> resultPtr) {
    final data = _read(resultPtr);
    if (resultPtr != nullptr) bindings.npResultFree(resultPtr);
    _pending.remove(requestId)?.complete(data);
  }

  static PaletteData _read(Pointer<bindings.NPResult> resultPtr) {
    if (resultPtr == nullptr) return PaletteData.failed(PaletteError.allocation);
    final result = resultPtr.ref;
    final count = result.count;
    // Copying detaches the data from the native allocation, which the caller
    // frees; a few dozen colors are cheaper to copy than to keep alive behind
    // a finalizer.
    return PaletteData(
      colors: count > 0 ? Uint32List.fromList(result.colors.asTypedList(count)) : Uint32List(0),
      populations: count > 0 ? Uint32List.fromList(result.populations.asTypedList(count)) : Uint32List(0),
      width: result.width,
      height: result.height,
      error: PaletteError.fromCode(result.error),
    );
  }
}
