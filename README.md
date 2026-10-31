# Blueon open source components

Source code of the LGPL libraries in **Blueon**, an Android video and music player, as required by the GNU LGPL 2.1.
Blueon itself is not open source. This repository holds only the libraries below and what is needed to rebuild them.

Blueon의 LGPL 라이브러리 소스입니다. Blueon 앱 자체는 공개 소스가 아니며, 여기에는 아래 라이브러리와 다시 빌드하는 데 필요한 것만 있습니다.

## What is here

| Library | License | In the app | Source here |
|---|---|---|---|
| FFmpeg 6.0.2 (`release/6.0`, commit `3f92512`) | LGPL 2.1 or later | static, inside `libffmpegJNI.so` | `ffmpeg/` |
| LAME 3.100 | LGPL 2.1 | shared, `libmp3lame.so` | `lame/` |
| GNU FriBidi 1.0.16 | LGPL 2.1 | static, inside `libass.so` | `libass/` |

`libass.so` comes unchanged from [libass-android](https://github.com/peerless2012/libass-android) 0.5.1.
Because FriBidi is linked into it, everything needed to rebuild that file is included in `libass/`: libass, FreeType, HarfBuzz, Fontconfig, Expat, libunibreak and the build scripts.

`SHA256SUMS` lists every archive. `COPYING.LGPLv2.1` is the license text.

## Changes we made

- **FFmpeg:** none to the code. `ffmpeg/changes.diff` is one change to a build file (`ffbuild/library.mak`), so that long `ar` command lines work on Windows. `build_ffmpeg.sh` applies it for you.
- **`ffmpeg_jni.cc`:** the JNI wrapper from AndroidX Media3 (Apache License 2.0) with our changes. It is the only other code inside `libffmpegJNI.so`, so it is included in full.
- **LAME:** none. `lame/config.h` replaces the file that `configure` would generate.
- **libass and its libraries:** none. We use the published package as it is.

No FFmpeg GPL or non-free features are enabled.

## Rebuild

You need the Android NDK `28.2.13676358`, CMake 3.22, Ninja and Bash. The minimum Android API is 29.
Set `NDK` to the NDK folder. `build_ffmpeg.sh` reads `NDK_PATH` and defaults to a Windows host, so on Linux also set `HOST_PLATFORM`.

```bash
export NDK=/path/to/ndk/28.2.13676358 NDK_PATH=$NDK HOST_PLATFORM=linux-x86_64

# FFmpeg: unpack, build the static libraries, then link libffmpegJNI.so
tar xzf ffmpeg/ffmpeg-3f92512.tar.gz
ffmpeg/build_ffmpeg.sh "$PWD/ffmpeg-3f92512" arm64-v8a

cmake -S ffmpeg -B out/ffmpeg -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFFMPEG_DIR="$PWD/ffmpeg-3f92512"
cmake --build out/ffmpeg

# LAME
tar xzf lame/lame-3.100.tar.gz
cmake -S lame -B out/lame -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLAME_DIR="$PWD/lame-3.100"
cmake --build out/lame
```

To rebuild `libass.so` you need Linux with autotools. `.github/workflows/rebuild-libass.yml` runs every step from the archives in `libass/`, so you can follow it or run it in a fork.

## Replace a library in the app

Each library is its own file in the APK: `lib/<ABI>/libffmpegJNI.so`, `lib/<ABI>/libmp3lame.so`, `lib/<ABI>/libass.so`.
Replace the file, then sign the APK with your own key using `apksigner`. A version signed with a different key cannot update the one from Google Play, so uninstall that first.

## Get a copy by email

For at least three years after you received the app, you can email **ljheh125@gmail.com** for a copy of this source code, for no more than the cost of sending it.
