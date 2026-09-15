## 1.0.0

* Initial release. Decodes with libav* through Dart FFI and native assets,
  reducing to RMS amplitudes in the `0..100` range.
* Buckets on sample positions instead of decoder frame boundaries, so the
  requested samples-per-second is honoured identically on every codec.
* Android links against the FFmpeg already bundled by ffmpeg-kit.
