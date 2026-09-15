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

| Target | FFmpeg |
| --- | --- |
| Android | Links the FFmpeg `ffmpeg_kit_flutter` already ships inside the APK, so this package adds no decoder of its own. Headers for n6.0 are vendored in `src/ffmpeg/include` to match its ABI. |
| Linux / Windows | Links a static FFmpeg into the library, since a desktop machine is not guaranteed to have one. |

For a release, build the desktop library with `build_waveform.sh` in namida's
`external/ffmpeg_build`, and point `prebuilt_dir` at its output. That script
builds a decode-only FFmpeg -- every demuxer, audio decoders and parsers only,
no network and no external libraries -- so the library ends up around 5.5MB and
imports nothing but libc/libm (Linux) or KERNEL32 and the UCRT (Windows).

> Build it on the oldest glibc you support. `verify_linux.sh` fails a library
> built on a newer one, exactly as it does for `ffmpeg`/`ffprobe`.

Without `prebuilt_dir` the hook compiles from source against a system FFmpeg
found through `pkg-config`, which is fine for development but not for shipping.

## Configuration

Build hooks run in a semi-hermetic environment, so environment variables never
reach them. Configuration comes from the root package's `pubspec.yaml`:

```yaml
hooks:
  user_defines:
    namida_waveform:
      prebuilt_dir: external/ffmpeg_build
```

| Key | Purpose |
| --- | --- |
| `prebuilt_dir` | Bundle an already built library instead of compiling. Looked up as `<os>/<arch>/<library>`, `<os>/<library>`, then `<library>`. |
| `ffmpeg_dir` | An FFmpeg install (`include/` + `lib/`) to compile against. Links statically when the prefix only carries `.a` archives. |
| `ffmpeg_kit_aar` | Override the ffmpeg-kit `.aar` the Android build links against. |

Paths resolve against the directory of the `pubspec.yaml` declaring them. Each
is optional and falls through to the next option when it holds nothing for the
target, so a checkout that has not built the artifacts still compiles.

## Credits

Written by Claude (Opus 5).