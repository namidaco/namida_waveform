# namida_waveform

Cross-platform audio waveform and cover art palette extraction for Namida.
Decodes with `libavformat` / `libavcodec` through Dart FFI and native assets,
and reduces an audio stream to RMS amplitudes in the `0..100` range, or a
picture to its dominant colors.

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

## Spectrum

```dart
final data = NamidaSpectrum.extract(path, framesPerSecond: 30, bandCount: 16);
final row = data.rows.sublist(frame * data.stride, (frame + 1) * data.stride);
```

Rows of `bandCount + 1` bytes: the level of every log spaced band between 40Hz
and 16kHz, low to high, then how hard a beat lands on that frame. Frame `i` is
centered on `i / framesPerSecond` seconds. Levels span 54dB below the loudest
band of the track, so a quiet recording moves as much as a loud one. It is a
decode pass of its own, extracting a waveform costs nothing extra.

## Rhythm

```dart
final data = NamidaRhythm.analyze(path, bpmHint: tagBpm);
print(data.bpm); // 128.002
print(data.beatOffsetMS); // a beat of the grid, within the first period
print(data.key); // 0..11 major C..B, 12..23 minor C..B, -1 unknown
```

Tempo comes from the autocorrelation of the spectral flux, weighted towards
120 BPM to settle between a tempo and its half or double unless `bpmHint` says
otherwise. The beat grid is then fitted over the whole track by folding the
kick-weighted onsets onto one beat, so its offset still holds at the end of the
track, and `beatConfidence` drops when parts of the track drift off it. The key
is the best correlation of the tuning-corrected pitch classes with Temperley's
profiles. `audibleStartMS`/`audibleEndMS` mark where the audio rises above and
falls below silence, and `fadeOutStartMS` where the track settles into its fade
out or quiet outro. Blocking, call it from a worker isolate.

## Palettes

```dart
final palette = await NamidaPalette.extractAsync(path: coverPath, maxColors: 16);
print(palette.colors); // Uint32List of 0xAARRGGBB, most populous first
```

The picture is decoded natively, box-averaged onto a grid at most `maxHeight`
rows tall (the width follows the aspect ratio, and every source pixel weighs
in), and reduced with the median-cut quantizer of Android's Palette (the same
one `palette_generator` ports, with identical output for the same pixels). JPEG
decodes at 1/2, 1/4 or 1/8 resolution straight out of the DCT when that still
covers the grid. Every still image
format the linked libavcodec carries is accepted -- JPEG, PNG, WebP, GIF, BMP
and TIFF in the desktop build, more with ffmpeg-kit -- and the container is
detected from the bytes, never from the file name.

`extractAsync` runs on a native thread of its own and posts the result back
through a `NativeCallable.listener`, so no isolate is spawned and no pixel
buffer ever crosses an isolate boundary. `extract` is the blocking variant for
callers that already have a worker.

## Building

| Target | FFmpeg |
| --- | --- |
| Android | Links the FFmpeg `ffmpeg_kit_flutter` already ships inside the APK, so this package adds no decoder of its own. Headers for n6.0 are vendored in `src/ffmpeg/include` to match its ABI. |
| Linux / Windows | Links a static FFmpeg into the library, since a desktop machine is not guaranteed to have one. |

For a release, build the desktop library with `build_waveform.sh` in namida's
`external/ffmpeg_build`, and point `prebuilt_dir` at its output. That script
builds a decode-only FFmpeg -- every demuxer, audio decoders and parsers, the
still image decoders and a static zlib for PNG, no network -- so the library
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