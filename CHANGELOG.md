## 2.0.2

- `NamidaPalette`: every source pixel is box-averaged into its grid cell
  instead of one pixel per cell being picked, and the grid is capped by height
  (`maxHeight`, was `maxDimension` on the longest side), so populations match a
  filtered downscale of the whole picture.

## 2.0.0

- `NamidaPalette`: cover art palette extraction in the same library. Decodes
  with libavcodec (JPEG at reduced resolution straight out of the DCT), samples
  the picture into a 5-bit histogram and runs the median-cut quantizer of
  Android's Palette, bit-identical to `palette_generator` for the same pixels.
  `extractAsync` runs on a native thread and reports back through a
  `NativeCallable.listener`, so no isolate and no pixel copy is involved.
- Desktop build: the still image decoders and a static zlib join the
  decode-only FFmpeg.

## 1.0.2

- Sums of squares run over four independent accumulators, and integer formats
  accumulate exactly instead of converting every sample: 25-45% faster on PCM
  and lossless input, bit-identical output.
- Every stream but the chosen audio one is discarded before demuxing, so the
  video in a video file is skipped rather than read.
- Bucket size comes from the first decoded frame's sample rate, which raw
  streams (mp3, ac3, dts) do not always carry in their container.
- A packet the decoder refuses with `EAGAIN` is resent instead of dropped.
- Hook: `macOS` resolved to the wrong library file name; ffmpeg-kit libraries
  are extracted into the hook's shared output directory instead of the package
  root.

## 1.0.1

- Hook configuration moved to `hooks.user_defines` in the root pubspec, since
  build hooks never see environment variables.

## 1.0.0

- Initial release. Decodes with libav\* through Dart FFI and native assets,
  reducing to RMS amplitudes in the `0..100` range.
- Buckets on sample positions instead of decoder frame boundaries, so the
  requested samples-per-second is honoured identically on every codec.
- Android links against the FFmpeg already bundled by ffmpeg-kit.
