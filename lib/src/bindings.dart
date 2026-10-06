// Hand-written bindings for `src/namida_waveform.h`, `src/namida_spectrum.h`, `src/namida_rhythm.h`, `src/namida_lossless.h` and `src/namida_palette.h`.
//
// The surface is a handful of functions and five structs, so these are maintained directly
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

// -- namida_rhythm.h ---------------------------------------------------------

final class NRResult extends Struct {
  @Double()
  external double bpm;

  @Double()
  external double beatOffsetMs;

  @Float()
  external double beatConfidence;

  @Int32()
  external int key;

  @Float()
  external double keyConfidence;

  @Int32()
  external int error;

  @Int64()
  external int audibleStartMs;

  @Int64()
  external int audibleEndMs;

  @Int64()
  external int fadeOutStartMs;

  @Int64()
  external int durationMs;

  @Int32()
  external int sampleRate;
}

@Native<Pointer<NRResult> Function(Pointer<Char>, Double)>(symbol: 'nr_analyze')
external Pointer<NRResult> nrAnalyze(Pointer<Char> path, double bpmHint);

@Native<Void Function(Pointer<NRResult>)>(symbol: 'nr_result_free')
external void nrResultFree(Pointer<NRResult> result);

@Native<Int32 Function()>(symbol: 'nr_version')
external int nrVersion();

// -- namida_lossless.h -------------------------------------------------------

final class NLResult extends Struct {
  @Float()
  external double cutoffHz;

  @Float()
  external double cutoffDropDb;

  @Int32()
  external int lossless;

  @Int32()
  external int sampleRate;

  @Int32()
  external int storedBits;

  @Int32()
  external int usedBits;

  @Int32()
  external int error;

  @Int64()
  external int durationMs;
}

@Native<Pointer<NLResult> Function(Pointer<Char>)>(symbol: 'nl_check')
external Pointer<NLResult> nlCheck(Pointer<Char> path);

@Native<Void Function(Pointer<NLResult>)>(symbol: 'nl_result_free')
external void nlResultFree(Pointer<NLResult> result);

@Native<Int32 Function()>(symbol: 'nl_version')
external int nlVersion();

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
