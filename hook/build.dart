// ignore_for_file: avoid_print

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:logging/logging.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

// Build hooks run in a semi-hermetic environment, so configuration comes from
// the root package's `pubspec.yaml` rather than environment variables:
//
// ```yaml
// hooks:
//   user_defines:
//     namida_waveform:
//       ffmpeg_dir: external/ffmpeg_build/install/usr/local
// ```
//
// Paths resolve against the directory of the `pubspec.yaml` declaring them, and
// the files used from them are registered as build dependencies so a change
// re-runs the hook.

/// A directory holding an already built `namida_waveform_native` library for the
/// target, looked up as `<os>/<arch>/<library>`, `<os>/<library>` or
/// `<library>`. Bundled as-is, and only when the `version` file next to it
/// matches this package's version. Falls through to a source build when the
/// directory holds nothing for this target, so a checkout that has not produced
/// the artifact yet still builds.
const _definePrebuiltDir = 'prebuilt_dir';

/// Root of an FFmpeg install (`include/` + `lib/`) to build the desktop library
/// against. Pointed at a static FFmpeg install (a prefix with only `.a`
/// archives), the library resolves nothing on the user's machine. Without it the
/// build falls back to a system install.
const _defineFFmpegDir = 'ffmpeg_dir';

/// The package whose build hook ships FFmpeg (`libffmpegkit`) on Android, iOS
/// and macOS. Its hook hands the library's path to this one as metadata, so both
/// link the same FFmpeg and this package carries no decoder of its own there.
const _ffmpegKitPackage = 'ffmpeg_kit_extended_flutter';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;

    final targetOS = input.config.code.targetOS;
    final arch = input.config.code.targetArchitecture;
    final packageName = input.packageName;
    final assetName = '${packageName}_bindings_generated.dart';

    final prebuilt = _findPrebuilt(input, output, targetOS: targetOS, arch: arch);
    if (prebuilt != null) {
      print('namida_waveform: bundling prebuilt as ${prebuilt.path}');
      output.assets.code.add(
        CodeAsset(
          package: packageName,
          name: assetName,
          linkMode: DynamicLoadingBundled(),
          file: prebuilt.uri,
        ),
      );
      return;
    }

    final includes = <String>['src'];
    final libraries = <String>[];
    final libraryDirectories = <String>[];
    final flags = <String>[];
    // CBuilder's own default, kept for the Apple targets
    final frameworks = <String>['Foundation'];

    switch (targetOS) {
      case OS.android:
        final abi = _androidAbi(arch);
        if (abi == null) {
          throw UnsupportedError('namida_waveform: unsupported Android architecture "$arch".');
        }
        // the vendored headers match the FFmpeg release libffmpegkit is built from,
        // so struct layouts match the library we link against.
        includes.add('src/ffmpeg/include');
        final ffmpegKit = _ffmpegKitLibrary(input, output);
        libraryDirectories.add(ffmpegKit.parent.path);
        libraries.addAll(['ffmpegkit', 'm']);

      case OS.iOS || OS.macOS:
        includes.add('src/ffmpeg/include');
        // the binary sits at <dir>/ffmpegkit.framework/ffmpegkit on iOS, and at
        // <dir>/ffmpegkit.framework/Versions/A/ffmpegkit on macOS
        final ffmpegKit = _ffmpegKitLibrary(input, output);
        final isIOS = targetOS == OS.iOS;
        final framework = isIOS ? ffmpegKit.parent : ffmpegKit.parent.parent.parent;
        flags.addAll(['-F', framework.parent.path]);
        frameworks.add('ffmpegkit');
        // the framework sits next to this one in the app's Frameworks folder
        final rpaths = isIOS ? ['@executable_path/Frameworks', '@loader_path/..'] : ['@executable_path/../Frameworks', '@loader_path/../../..'];
        flags.addAll(rpaths.map((e) => '-Wl,-rpath,$e'));

      case OS.linux || OS.windows:
        final ffmpeg = _desktopFFmpeg(input, output, targetOS);
        includes.addAll(ffmpeg.includes);
        libraryDirectories.addAll(ffmpeg.libraryDirectories);
        libraries.addAll(ffmpeg.libraries);
        flags.addAll(ffmpeg.extraFlags);

      default:
        throw UnsupportedError('namida_waveform: unsupported target OS "$targetOS".');
    }

    if (targetOS != OS.windows) flags.add('-fvisibility=hidden');

    final builder = CBuilder.library(
      name: '${packageName}_native',
      assetName: assetName,
      sources: [
        'src/nw_decoder.c', 'src/nw_fft.c', 'src/nr_analyzer.c', 'src/nl_analyzer.c', //
        'src/namida_waveform.c', 'src/namida_spectrum.c', 'src/namida_rhythm.c', 'src/namida_lossless.c', 'src/namida_palette.c', //
      ],
      includes: includes,
      libraries: libraries,
      libraryDirectories: libraryDirectories,
      flags: flags,
      frameworks: frameworks,
      language: Language.c,
      std: 'c11',
    );

    await builder.run(input: input, output: output, logger: _logger());
  });
}

Logger _logger() => Logger('')
  ..level = Level.ALL
  ..onRecord.listen((record) => print(record.message));

String? _androidAbi(Architecture arch) => switch (arch) {
      Architecture.arm64 => 'arm64-v8a',
      Architecture.arm => 'armeabi-v7a',
      Architecture.x64 => 'x86_64',
      Architecture.ia32 => 'x86',
      _ => null,
    };

String _libraryFileName(OS targetOS) => switch (targetOS) {
      OS.windows => 'namida_waveform_native.dll',
      OS.macOS || OS.iOS => 'libnamida_waveform_native.dylib',
      _ => 'libnamida_waveform_native.so',
    };

/// `libffmpegkit` of the target, as the build hook of [_ffmpegKitPackage]
/// extracted it. On Apple targets it is the binary inside `ffmpegkit.framework`.
File _ffmpegKitLibrary(BuildInput input, BuildOutputBuilder output) {
  final path = input.metadata[_ffmpegKitPackage]['library_path'];
  if (path is! String) {
    throw StateError(
      'namida_waveform: $_ffmpegKitPackage passed no library_path to this hook. It needs a version '
      'of it whose build hook does (the namida fork), or .$_definePrebuiltDir pointing at a directory '
      'with a prebuilt library.',
    );
  }
  final library = File(path);
  if (!library.existsSync()) {
    throw StateError('namida_waveform: $_ffmpegKitPackage reported $path, which does not exist.');
  }
  output.dependencies.add(library.absolute.uri);
  return library;
}

/// Resolves a user-define holding a path. Callers register the files they use:
/// flutter treats a directory dependency as a missing input and reruns the
/// hooks every build.
String? _definedPath(BuildInput input, String key) {
  final uri = input.userDefines.path(key);
  return uri?.toFilePath();
}

/// Looks for an already built library for this target, so release builds can
/// ship one linked against a known FFmpeg instead of whatever the build machine
/// happens to have installed.
File? _findPrebuilt(BuildInput input, BuildOutputBuilder output, {required OS targetOS, required Architecture arch}) {
  final directory = _definedPath(input, _definePrebuiltDir);
  if (directory == null) return null;

  final fileName = _libraryFileName(targetOS);
  final candidates = [
    File('$directory/${targetOS.name}/${arch.name}/$fileName').absolute,
    File('$directory/${targetOS.name}/$fileName').absolute,
    File('$directory/$fileName').absolute,
  ];
  for (final file in candidates) {
    if (file.existsSync()) {
      output.dependencies.add(file.uri);
      _ensurePrebuiltVersion(input, output, file);
      // -- a copy, since a failed flutter build deletes the previous assets as stale outputs
      final bundledPath = input.outputDirectory.resolve(fileName).toFilePath();
      return file.copySync(bundledPath);
    }
  }
  // -- so a prebuilt produced later replaces the source build
  output.dependencies.addAll(candidates.map((e) => e.uri));
  print(
    'namida_waveform: $_definePrebuiltDir is "$directory" but it holds no $fileName for '
    '$targetOS $arch, building from source instead.',
  );
  return null;
}

final _versionRegex = RegExp(r'^version:\s*(\S+)', multiLine: true);

/// A prebuilt from older sources would miss fixes or crash on a missing symbol,
/// so the `version` file build_waveform.sh writes next to it has to match ours.
void _ensurePrebuiltVersion(BuildInput input, BuildOutputBuilder output, File prebuilt) {
  final pubspecFile = File.fromUri(input.packageRoot.resolve('pubspec.yaml'));
  final versionFile = File('${prebuilt.parent.path}/version');
  output.dependencies.addAll([pubspecFile.uri, versionFile.uri]);

  final pubspec = pubspecFile.readAsStringSync();
  final packageVersion = _versionRegex.firstMatch(pubspec)?.group(1);
  final prebuiltVersion = versionFile.existsSync() ? versionFile.readAsStringSync().trim() : null;
  if (prebuiltVersion == packageVersion) return;

  final prebuiltVersionText = prebuiltVersion ?? 'unknown (no version file)';
  throw StateError(
    'namida_waveform: ${prebuilt.path} was built from version $prebuiltVersionText, '
    'but this package is $packageVersion. Rebuild it with build_waveform.sh, or drop $_definePrebuiltDir to build from source.',
  );
}

class _FFmpegFlags {
  const _FFmpegFlags(this.includes, this.libraryDirectories, this.libraries, [this.extraFlags = const []]);
  final List<String> includes;
  final List<String> libraryDirectories;
  final List<String> libraries;

  /// Anything pkg-config reports that is not `-I`/`-L`/`-l`, such as `-pthread`.
  final List<String> extraFlags;
}

/// FFmpeg to build the desktop library against.
///
/// Prefers the prefix in the `ffmpeg_dir` user-define, which release builds
/// point at their own FFmpeg, and otherwise falls back to a system install.
_FFmpegFlags _desktopFFmpeg(BuildInput input, BuildOutputBuilder output, OS targetOS) {
  const packages = ['libavformat', 'libavcodec', 'libavutil'];
  final fallbackLibraries = <String>[
    'avformat',
    'avcodec',
    'avutil',
    if (targetOS != OS.windows) 'm',
  ];

  final root = _definedPath(input, _defineFFmpegDir);
  if (root != null) {
    if (Directory(root).existsSync()) {
      final libDirectory = Directory('$root/lib');
      if (libDirectory.existsSync()) {
        final libFiles = libDirectory.listSync().whereType<File>().map((e) => e.absolute.uri);
        output.dependencies.addAll(libFiles);
      }
      // A staged install carries .pc files describing the transitive static
      // dependencies (zlib, openssl, pthread); use them when they are there.
      final pkgConfigDirectory = Directory('$root/lib/pkgconfig');
      if (pkgConfigDirectory.existsSync()) {
        final flags = _runPkgConfig(packages, static: true, pkgConfigPath: pkgConfigDirectory.path);
        if (flags != null) return flags;
        print('namida_waveform: pkg-config failed for $root, using its include/ and lib/ directly.');
      }
      return _FFmpegFlags(['$root/include'], ['$root/lib'], fallbackLibraries);
    }
    print('namida_waveform: $_defineFFmpegDir is "$root" but that directory does not exist, falling back to a system install.');
  }

  final flags = _runPkgConfig(packages, static: false, pkgConfigPath: null);
  if (flags != null) return flags;

  if (targetOS == OS.windows) {
    throw StateError(
      'namida_waveform: building from source on Windows needs '
      'hooks.user_defines.namida_waveform.$_defineFFmpegDir pointing at an FFmpeg install '
      'with include/ and lib/, or .$_definePrebuiltDir pointing at a directory with a '
      'prebuilt namida_waveform_native.dll.',
    );
  }
  print('namida_waveform: no FFmpeg found through pkg-config, using default search paths.');
  return _FFmpegFlags(const [], const [], fallbackLibraries);
}

/// Returns null when pkg-config is missing or does not know these packages.
_FFmpegFlags? _runPkgConfig(List<String> packages, {required bool static, required String? pkgConfigPath}) {
  final ProcessResult result;
  try {
    result = Process.runSync(
      'pkg-config',
      [
        // A DESTDIR-staged install keeps `prefix=/usr/local` in its .pc files,
        // so pkg-config has to recompute it from where the .pc file actually
        // sits, or every path it reports points at a directory that is not there.
        if (pkgConfigPath != null) '--define-prefix',
        if (static) '--static',
        '--cflags-only-I',
        '--libs',
        ...packages,
      ],
      environment: pkgConfigPath == null ? null : {'PKG_CONFIG_PATH': pkgConfigPath},
    );
  } on ProcessException {
    return null;
  }
  if (result.exitCode != 0) return null;

  final includes = <String>[];
  final libraryDirectories = <String>[];
  final libraries = <String>[];
  final extraFlags = <String>[];
  for (final token in (result.stdout as String).split(RegExp(r'\s+'))) {
    if (token.isEmpty) continue;
    switch (token.length >= 2 ? token.substring(0, 2) : '') {
      case '-I':
        includes.add(token.substring(2));
      case '-L':
        libraryDirectories.add(token.substring(2));
      case '-l':
        libraries.add(token.substring(2));
      default:
        // `-pthread` and friends have to reach the linker as written.
        if (token.startsWith('-')) extraFlags.add(token);
    }
  }
  if (libraries.isEmpty) return null;
  // Duplicate `-l` entries are kept: with static archives the linker resolves
  // circular references by revisiting them, so the repeats are load-bearing.
  return _FFmpegFlags(
    includes.toSet().toList(),
    libraryDirectories.toSet().toList(),
    libraries,
    extraFlags.toSet().toList(),
  );
}

// -- obsolete android ffmpeg-kit lookups, see [_ffmpegKitLibrary]
//
// /// Overrides the ffmpeg-kit `.aar` the Android build links against. Found in the
// /// pub cache when omitted.
// const _defineFFmpegKitAar = 'ffmpeg_kit_aar';
//
// /// Extracts the ffmpeg-kit shared libraries for [abi] out of its `.aar` and
// /// returns the directory holding them.
// ///
// /// Android links against the FFmpeg that is already inside the APK, so this
// /// package adds no decoder of its own.
// String _prepareFFmpegKitLibs(BuildInput input, BuildOutputBuilder output, String abi) {
//   final aar = _locateFFmpegKitAar(input);
//   if (aar == null) {
//     throw StateError(
//       'namida_waveform: could not find the ffmpeg-kit .aar in the pub cache. Set '
//       'hooks.user_defines.namida_waveform.$_defineFFmpegKitAar to its path, or '
//       '.$_definePrebuiltDir to a directory with a prebuilt library.',
//     );
//   }
//   output.dependencies.add(aar.uri);
//
//   final outputDirectory = Directory.fromUri(input.outputDirectoryShared.resolve('ffmpeg_kit/$abi/'));
//   final marker = File.fromUri(outputDirectory.uri.resolve('.extracted'));
//   if (marker.existsSync() && marker.readAsStringSync() == aar.path) {
//     return outputDirectory.path;
//   }
//
//   if (outputDirectory.existsSync()) outputDirectory.deleteSync(recursive: true);
//   outputDirectory.createSync(recursive: true);
//
//   print('namida_waveform: extracting $abi FFmpeg libraries from ${aar.path}');
//   final result = Platform.isWindows
//       ? Process.runSync('tar', ['-xf', aar.path, '-C', outputDirectory.path, 'jni/$abi'])
//       : Process.runSync('unzip', ['-o', '-q', '-j', aar.path, 'jni/$abi/*', '-d', outputDirectory.path]);
//   if (result.exitCode != 0) {
//     throw StateError('namida_waveform: failed to extract ${aar.path}: ${result.stderr}');
//   }
//
//   // `tar` keeps the archive's directory structure, `unzip -j` flattens it.
//   final nested = Directory('${outputDirectory.path}/jni/$abi');
//   if (nested.existsSync()) {
//     for (final entity in nested.listSync()) {
//       if (entity is File) {
//         entity.renameSync('${outputDirectory.path}/${entity.uri.pathSegments.last}');
//       }
//     }
//     Directory('${outputDirectory.path}/jni').deleteSync(recursive: true);
//   }
//
//   final libraries = outputDirectory.listSync().whereType<File>().where((e) => e.path.endsWith('.so'));
//   if (libraries.isEmpty) {
//     throw StateError('namida_waveform: no .so found for $abi inside ${aar.path}');
//   }
//
//   marker.writeAsStringSync(aar.path);
//   return outputDirectory.path;
// }
//
// File? _locateFFmpegKitAar(BuildInput input) {
//   final override = _definedPath(input, _defineFFmpegKitAar);
//   if (override != null) {
//     final file = File(override);
//     if (file.existsSync()) return file;
//     throw StateError(
//       'namida_waveform: hooks.user_defines.namida_waveform.$_defineFFmpegKitAar points at a '
//       'missing file: $override',
//     );
//   }
//
//   final found = <File>[];
//   void collect(Directory repository) {
//     if (!repository.existsSync()) return;
//     for (final candidate in repository.listSync(recursive: true)) {
//       if (candidate is File && candidate.path.endsWith('.aar')) found.add(candidate);
//     }
//   }
//
//   // The resolved package is the one the app actually links against, and the
//   // package config is always handed to the hook, unlike the environment, which
//   // is filtered and may lack the variables locating the pub cache.
//   final packageRoot = _resolvedPackageRoot('ffmpeg_kit_flutter');
//   if (packageRoot != null) collect(Directory('${packageRoot.path}/android/repo/com/local/ffmpeg-kit'));
//
//   if (found.isEmpty) {
//     final pubCache = _pubCacheDirectory();
//     final gitDirectory = pubCache == null ? null : Directory('${pubCache.path}/git');
//     if (gitDirectory != null && gitDirectory.existsSync()) {
//       for (final entity in gitDirectory.listSync()) {
//         if (entity is! Directory) continue;
//         if (!entity.uri.pathSegments.any((s) => s.startsWith('ffmpeg-kit'))) continue;
//         collect(Directory('${entity.path}/flutter/flutter/android/repo/com/local/ffmpeg-kit'));
//       }
//     }
//   }
//   if (found.isEmpty) return null;
//
//   // Prefer the full build; it carries every decoder namida can be asked about.
//   found.sort((a, b) {
//     final aFull = a.path.contains('full_binary') ? 0 : 1;
//     final bFull = b.path.contains('full_binary') ? 0 : 1;
//     if (aFull != bFull) return aFull - bFull;
//     return b.path.compareTo(a.path);
//   });
//   return found.first;
// }
//
// /// Root of [package] as resolved in the package config this hook runs with.
// Directory? _resolvedPackageRoot(String package) {
//   final packageConfig = Platform.packageConfig;
//   if (packageConfig == null) return null;
//   final file = File.fromUri(Uri.parse(packageConfig));
//   if (!file.existsSync()) return null;
//   final json = jsonDecode(file.readAsStringSync()) as Map<String, Object?>;
//   for (final entry in json['packages'] as List<Object?>) {
//     final map = entry as Map<String, Object?>;
//     if (map['name'] != package) continue;
//     final root = file.uri.resolve(map['rootUri'] as String);
//     final directory = Directory.fromUri(root);
//     return directory.existsSync() ? directory : null;
//   }
//   return null;
// }
//
// Directory? _pubCacheDirectory() {
//   if (Platform.isWindows) {
//     for (final key in ['LOCALAPPDATA', 'APPDATA']) {
//       final base = Platform.environment[key];
//       if (base == null) continue;
//       final directory = Directory('$base\\Pub\\Cache');
//       if (directory.existsSync()) return directory;
//     }
//     return null;
//   }
//   final home = Platform.environment['HOME'];
//   if (home == null) return null;
//   final directory = Directory('$home/.pub-cache');
//   return directory.existsSync() ? directory : null;
// }
