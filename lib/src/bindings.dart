// Hand-written bindings for `src/namida_waveform.h`.
//
// The surface is two functions and one struct, so these are maintained directly
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
