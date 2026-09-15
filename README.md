# namida_waveform

Cross-platform audio waveform extraction for Namida. Decodes with `libavformat`
/ `libavcodec` through Dart FFI and native assets, and reduces the stream to RMS
amplitudes in the `0..100` range.

```dart
final data = await NamidaWaveform.extractInIsolate(path, samplesPerSecond: 100);
print(data.values); // Float32List, 100 entries per second of audio
```

`extract` blocks the calling isolate; call it from your own worker when you have
one, or use `extractInIsolate`.

## Output

One RMS value per bucket, where a bucket is `sampleRate / samplesPerSecond`
source samples, summed across every channel. Buckets are cut on sample positions
rather than decoder frame boundaries, so the requested rate is honoured exactly
and the same audio produces the same number of values in every container.

## Building

| Target | FFmpeg source |
| --- | --- |
| Android | Links against the FFmpeg shipped by `ffmpeg_kit_flutter` (already in the APK). Headers for n6.0 are vendored in `src/ffmpeg/include`. |
| Linux / macOS | `pkg-config` (`libavformat`, `libavcodec`, `libavutil`). |
| Windows | The `ffmpeg_dir` user-define below, pointing at an install with `include/` and `lib/`. |

Release builds should not link against whatever the build machine has
installed. Configuration comes from the root package's `pubspec.yaml` — build
hooks run in a semi-hermetic environment, so environment variables do not reach
them:

```yaml
hooks:
  user_defines:
    namida_waveform:
      prebuilt_dir: external/ffmpeg_build
      ffmpeg_dir: external/ffmpeg_build/install/usr/local
```

| Key | Purpose |
| --- | --- |
| `prebuilt_dir` | Bundle an already built library instead of compiling. Looked up as `<os>/<arch>/<library>`, `<os>/<library>`, then `<library>`. |
| `ffmpeg_dir` | FFmpeg install (`include/` + `lib/`) to compile against. Links statically when the prefix only carries `.a` archives. |
| `ffmpeg_kit_aar` | Override the ffmpeg-kit `.aar` the Android build links against. |

Paths resolve against the directory of the `pubspec.yaml` declaring them. Each
is optional and falls through to the next option when it holds nothing for the
target, so a checkout that has not produced the artifacts yet still builds
against a system FFmpeg.

## Credits

Written by Claude (Opus 5).
