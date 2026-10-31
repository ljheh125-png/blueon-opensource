#!/bin/bash
#
# FFmpeg을 안드로이드용으로 빌드한다 (Media3 decoder_ffmpeg 모듈이 쓰는 정적 라이브러리).
#
# Media3의 libraries/decoder_ffmpeg/src/main/jni/build_ffmpeg.sh(Apache-2.0)를 고친 것이다.
# 고친 곳:
#   - **윈도우에서 돈다.** 원본은 "윈도우 지원 안 함"이라고 적혀 있는데, NDK가 make.exe와
#     bash로 된 clang 껍데기를 같이 들고 다녀서 Git Bash로 된다. nproc이 없어 코어 수는
#     NUMBER_OF_PROCESSORS에서 읽는다.
#   - **ABI를 골라 돌린다.** 넷을 다 도는 데 한참 걸려서, 확인할 때는 하나만 돌린다.
#   - **디코더 목록을 이 파일 안에 둔다.** 부를 때마다 손으로 적으면 ABI마다 다른 것이
#     들어가는 사고가 난다.
#
# 쓰는 법:
#   tools/build_ffmpeg.sh <ffmpeg 소스 경로> [ABI ...]
#   예) tools/build_ffmpeg.sh /c/AndroidProjects/ffmpeg-build/ffmpeg arm64-v8a
#   ABI를 안 주면 넷을 다 돈다.
#
set -eu

FFMPEG_PATH="$1"
shift
REQUESTED_ABIS=("$@")
if [[ ${#REQUESTED_ABIS[@]} -eq 0 ]]; then
    REQUESTED_ABIS=(armeabi-v7a arm64-v8a x86 x86_64)
fi

NDK_PATH="${NDK_PATH:-${LOCALAPPDATA}/Android/Sdk/ndk/28.2.13676358}"
# **윈도우 함정 2.** `%LOCALAPPDATA%`는 `C:\Users\...` 라 역슬래시가 섞여 있다. 이 글자가
# ffbuild/config.mak에 그대로 박히면, make가 그 줄을 sh에 넘길 때 `\U`·`\A`가 벗어나기
# 문자로 먹혀 `C:Users...`가 된다. 컴파일러를 못 찾고 전부 Error 127로 죽는다.
NDK_PATH="$(cygpath -u "${NDK_PATH}" 2>/dev/null || echo "${NDK_PATH}")"
HOST_PLATFORM="${HOST_PLATFORM:-windows-x86_64}"
# 앱의 minSdk와 같다. 이보다 높으면 안 된다.
ANDROID_ABI_VERSION=29

JOBS="$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")"

# **윈도우 함정 1.** configure는 TMPDIR/TEMP를 셸 경로로 쓰는데, 윈도우가 넣어둔
# `C:\Users\...` 는 역슬래시가 먹혀 `C:Users...`가 된다. 그러면 첫 컴파일 시험부터
# "Sanity test failed"로 선다. 유닉스식 경로로 덮어쓴다.
export TMPDIR="/tmp"
mkdir -p /tmp

# ---------------------------------------------------------------------------
# 무엇을 들고 갈 것인가
#
# **디코더와 해석기를 전부 켠다.** 예전에는 쓸 것만 골라 담았는데(50개), 목표가
# "어떤 파일이든 재생"이라 고르는 방식으로는 늘 무언가가 빠진다. 실제로 고르다가
# ADPCM(옛 AVI에 흔한 소리)과 MS-MPEG4 v3를 빠뜨렸고, 둘 다 조용히 무음·검은 화면으로
# 나타났다. 빠뜨린 것을 하나씩 찾아 넣는 것보다 다 넣고 용량을 재는 편이 낫다.
#
# 대신 **재생에 필요 없는 것은 모두 끈다** — 인코더·먹서·필터·프로토콜·기기 입출력.
# 우리는 파일을 만들지 않고(그 일은 Media3 Transformer와 LAME이 한다), 파일을 여는
# 것도 자바 쪽에서 넘겨주므로 프로토콜이 필요 없다.
#
# **해석기를 다 켜도 남의 파일을 가로채지 않는다.** 어떤 상자를 우리가 맡을지는
# FFmpeg의 자동 판별이 아니라 [FfmpegExtractor]가 파일 머리 표식을 보고 정하고,
# 그 해석기는 Media3 해석기들 뒤에 선다.
# ---------------------------------------------------------------------------
COMMON_OPTIONS="
    --target-os=android
    --enable-static
    --disable-shared
    --disable-doc
    --disable-programs
    --disable-encoders
    --disable-muxers
    --disable-protocols
    --disable-devices
    --disable-avdevice
    --enable-avformat
    --enable-swscale
    --disable-postproc
    --disable-avfilter
    --disable-symver
    --enable-swresample
    --extra-ldexeflags=-pie
    --disable-v4l2-m2m
    --disable-vulkan
    "

TOOLCHAIN_PREFIX="${NDK_PATH}/toolchains/llvm/prebuilt/${HOST_PLATFORM}/bin"
if [[ ! -d "${TOOLCHAIN_PREFIX}" ]]; then
    echo "NDK 경로가 틀렸다: ${NDK_PATH}"
    exit 1
fi
# NDK가 들고 다니는 make를 쓴다 (윈도우에는 make가 따로 없다).
#
# **윈도우 함정 3. 이 폴더를 PATH에 넣으면 안 된다.** 같은 자리에 윈도우용
# `cmp.exe`·`echo.exe`가 같이 들어 있어서 MSYS의 것을 가린다. 그러면 configure가
# 마지막에 `cmp`로 파일을 견주는 자리에서 "can't open file config.h"를 뱉고,
# config.h·avconfig.h 같은 것들이 아예 안 만들어진 채로 넘어간다.
MAKE_BIN="${NDK_PATH}/prebuilt/${HOST_PLATFORM}/bin/make.exe"
if [[ ! -e "${MAKE_BIN}" ]]; then
    MAKE_BIN="make"
fi

# **윈도우 함정 4.** 디코더를 전부 켜면 libavcodec의 목적 파일이 1500개를 넘는데,
# 그것을 한 줄에 늘어놓아 `ar`을 부르면 윈도우의 명령줄 길이 한계에서 잘린다.
# 잘린 자리에 따라 `llvm-ar: error: libavcodec/: Is a directory` 같은 엉뚱한 말이 나와
# 원인을 알아보기 어렵다.
#
# **응답 파일을 셸로 만들려 해도 안 된다** — `echo ... > 목록`도 결국 셸에게 그 긴 줄을
# 넘기는 것이라 똑같이 잘린다(그러면 `@목록: No such file or directory`가 된다).
# make 4.0의 `$(file ...)`는 셸을 거치지 않고 make가 직접 파일에 쓰므로 이것만 통한다.
# NDK가 들고 다니는 make는 4.3이다.
#
# configure가 library.mak을 매번 새로 만들므로 configure 뒤에 매번 고쳐야 한다.
fix_long_archive_command() {
    local mak="${FFMPEG_PATH}/ffbuild/library.mak"
    # **library.mak은 configure가 만드는 것이 아니라 FFmpeg 소스에 원래 있는 파일이다.**
    # 앞서 고쳐 둔 자국이 남아 있으면 아래 sed이 안 걸리므로, 늘 원본으로 되돌리고 시작한다.
    git -C "${FFMPEG_PATH}" checkout -- ffbuild/library.mak 2>/dev/null || true
    sed -i 's|.*(AR_O) \$\^.*|\t$(file >$@.objlist,$^)\n\t$(AR) $(ARFLAGS) $(AR_O) @$@.objlist|' "${mak}"
    if grep -q 'objlist' "${mak}"; then
        echo "library.mak: ar 명령을 응답 파일 방식으로 고쳤다"
    else
        echo "library.mak: 고칠 자리를 못 찾았다 — FFmpeg 판이 바뀌었는지 볼 것" >&2
        exit 1
    fi
}

build_one() {
    local abi="$1" arch="$2" cpu="$3" triple="$4" api="$5"
    shift 5
    local extra=("$@")

    echo "=============== ${abi} ==============="
    cd "${FFMPEG_PATH}"
    ./configure \
        --libdir="android-libs/${abi}" \
        --arch="${arch}" \
        --cpu="${cpu}" \
        --cross-prefix="${TOOLCHAIN_PREFIX}/${triple}${api}-" \
        --nm="${TOOLCHAIN_PREFIX}/llvm-nm" \
        --ar="${TOOLCHAIN_PREFIX}/llvm-ar" \
        --ranlib="${TOOLCHAIN_PREFIX}/llvm-ranlib" \
        --strip="${TOOLCHAIN_PREFIX}/llvm-strip" \
        "${extra[@]}" \
        ${COMMON_OPTIONS}
    fix_long_archive_command
    "${MAKE_BIN}" -j"${JOBS}"
    "${MAKE_BIN}" install-libs
    "${MAKE_BIN}" clean
}

for abi in "${REQUESTED_ABIS[@]}"; do
    case "${abi}" in
        armeabi-v7a)
            build_one armeabi-v7a arm armv7-a armv7a-linux-androideabi "${ANDROID_ABI_VERSION}" \
                --extra-cflags="-march=armv7-a -mfloat-abi=softfp" \
                --extra-ldflags="-Wl,--fix-cortex-a8"
            ;;
        arm64-v8a)
            build_one arm64-v8a aarch64 armv8-a aarch64-linux-android "${ANDROID_ABI_VERSION}"
            ;;
        x86)
            build_one x86 x86 i686 i686-linux-android "${ANDROID_ABI_VERSION}" --disable-asm
            ;;
        x86_64)
            build_one x86_64 x86_64 x86-64 x86_64-linux-android "${ANDROID_ABI_VERSION}" --disable-asm
            ;;
        *)
            echo "모르는 ABI: ${abi}"
            exit 1
            ;;
    esac
done

echo "끝났다. 결과: ${FFMPEG_PATH}/android-libs/<ABI>/lib*.a"
