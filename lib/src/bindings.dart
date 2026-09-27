// Hand-written bindings for `src/namida_waveform.h`, `src/namida_spectrum.h` and `src/namida_palette.h`.
//
// The surface is a handful of functions and three structs, so these are maintained directly
// rather than regenerated with ffigen. Field order here must match the C struct
// exactly; Dart FFI derives padding from the target ABI.
@DefaultAsset('package:namida_waveform/namida_waveform_bindings_generated.dart')
library;

import 'dart:ffi';

final class NWResult extends Struct {
  external Pointer<Float> values;

  @Int32()
  external int count;

  @Int32()
  external int error;

  @Int64()
  external int durationMs;

  @Int32()
  external int sampleRate;

  @Int32()
  external int channels;
}

@Native<Pointer<NWResult> Function(Pointer<Char>, Int32)>(symbol: 'nw_extract')
external Pointer<NWResult> nwExtract(Pointer<Char> path, int samplesPerSecond);

@Native<Void Function(Pointer<NWResult>)>(symbol: 'nw_result_free')
external void nwResultFree(Pointer<NWResult> result);

@Native<Int32 Function()>(symbol: 'nw_version')
external int nwVersion();

// -- namida_spectrum.h -------------------------------------------------------

final class NSResult extends Struct {
  external Pointer<Uint8> data;

  @Int32()
  external int frameCount;

  @Int32()
  external int bandCount;

  @Int32()
  external int framesPerSecond;

  @Int32()
  external int error;

  @Int64()
  external int durationMs;

  @Int32()
  external int sampleRate;
}

@Native<Pointer<NSResult> Function(Pointer<Char>, Int32, Int32)>(symbol: 'ns_extract')
external Pointer<NSResult> nsExtract(Pointer<Char> path, int framesPerSecond, int bandCount);

@Native<Void Function(Pointer<NSResult>)>(symbol: 'ns_result_free')
external void nsResultFree(Pointer<NSResult> result);

@Native<Int32 Function()>(symbol: 'ns_version')
external int nsVersion();

// -- namida_palette.h --------------------------------------------------------

final class NPResult extends Struct {
  external Pointer<Uint32> colors;

  external Pointer<Uint32> populations;

  @Int32()
  external int count;

  @Int32()
  external int error;

  @Int32()
  external int width;

  @Int32()
  external int height;
}

typedef NPCallback = Void Function(Int64 requestId, Pointer<NPResult> result);

@Native<Pointer<NPResult> Function(Pointer<Char>, Pointer<Uint8>, Int64, Int32, Int32)>(symbol: 'np_extract')
external Pointer<NPResult> npExtract(Pointer<Char> path, Pointer<Uint8> data, int dataSize, int maxColors, int maxHeight);

// Leaf: the call only copies its inputs and starts a thread, which lets a
// Dart-heap `Uint8List` be passed by address without an intermediate copy.
@Native<Int32 Function(Pointer<Char>, Pointer<Uint8>, Int64, Int32, Int32, Int64, Pointer<NativeFunction<NPCallback>>)>(symbol: 'np_extract_async', isLeaf: true)
external int npExtractAsync(Pointer<Char> path, Pointer<Uint8> data, int dataSize, int maxColors, int maxHeight, int requestId, Pointer<NativeFunction<NPCallback>> callback);

@Native<Void Function(Pointer<NPResult>)>(symbol: 'np_result_free')
external void npResultFree(Pointer<NPResult> result);

@Native<Int32 Function()>(symbol: 'np_version')
external int npVersion();
