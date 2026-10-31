/*
 * LAME을 안드로이드에서 빌드하기 위한 설정.
 *
 * LAME은 원래 autotools로 빌드하는 물건이라, `configure`가 기기를 살펴보고 이
 * 파일을 만들어준다. 우리는 CMake로 빌드하므로 그 자리를 이것으로 대신한다.
 * **LAME 소스는 한 글자도 고치지 않았고**, 바깥에서 이 파일 하나만 얹는다
 * (출처와 판은 VENDOR.md 참고).
 *
 * 안드로이드는 기기가 무엇이든 다음이 늘 참이라 살펴볼 것이 없다 — libc는
 * C99를 갖추고 있고, 바이트 순서는 little-endian이며, float는 IEEE754다.
 */
#ifndef LAME_ANDROID_CONFIG_H
#define LAME_ANDROID_CONFIG_H

/*
 * **이것이 빠지면 조용히 망가진다.** libmp3lame/machine.h는 STDC_HEADERS가
 * 없으면 옛 유닉스인 줄 알고 `#define strchr index`로 바꿔치기한다. 안드로이드에
 * index()는 없거나 폐기된 것이라 링크가 깨지거나 엉뚱한 것이 불린다.
 */
#define STDC_HEADERS 1

#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1

/*
 * **config.h는 매크로만 주는 곳이 아니라 타입도 주는 곳이다.**
 *
 * LAME 소스는 `ieee754_float32_t`를 아무 조건 없이 쓴다(util.h의 fast_log2 등).
 * 이 이름은 표준이 아니고 configure가 만든 config.h 안에서 typedef로 정의되던
 * 것이라, 매크로만 흉내 내면 "unknown type name"으로 무너진다.
 *
 * int8_t~uint64_t는 <stdint.h>가 이미 주므로 여기서 또 정의하면 안 된다 —
 * configure는 그 헤더가 없는 옛 시스템을 위해 넣어두던 것이고, 위에서
 * HAVE_STDINT_H를 켠 것이 곧 "그쪽을 쓰겠다"는 뜻이다.
 */
typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;

/*
 * 일부러 켜지 않은 것들. 켜면 안 되는 이유를 적어둔다 — 나중에 "왜 없지?" 하고
 * 넣었다가 깨뜨리는 일을 막으려는 것이다.
 *
 * HAVE_MPGLIB
 *   MP3를 **푸는** 코드. 우리는 만들기만 하고 푸는 일은 안드로이드가 이미
 *   할 줄 안다(OMX.google.mp3.decoder). 그래서 mpglib_interface.c도 안 넣었다.
 *
 * DECODE_ON_THE_FLY
 *   만들면서 되풀어 보고 클리핑을 재는 기능. mpglib이 있어야 하고, 우리가
 *   쓰지도 않는데 인코딩 시간만 두 배가 된다.
 *
 * HAVE_XMMINTRIN_H
 *   x86의 SSE 가속. 켜면 vector/xmm_quantize_sub.c도 함께 넣어야 하는데, 실제
 *   사용자 기기는 전부 ARM이라 어차피 안 쓰이는 길이다. x86은 에뮬레이터뿐이라
 *   ABI마다 다른 코드가 도는 위험만 사는 셈이라 넷 다 순수 C로 맞춘다.
 *
 * HAVE_NASM
 *   i386 어셈블리. 위와 같은 이유.
 *
 * TAKEHIRO_IEEE754_HACK
 *   float의 비트 배치를 직접 만져 반올림을 빠르게 하는 길. 안드로이드는 IEEE754라
 *   조건은 맞지만, **켠 쪽과 끈 쪽은 서로 다른 코드**다. 처음부터 빠른 길로 가서
 *   결과가 이상할 때 원인을 둘로 나눠 볼 이유가 없어서, 검증이 끝난 뒤에 켤 것으로
 *   미뤄둔다. 끈 쪽도 충분히 빠르다(실측은 VENDOR.md).
 */

#endif /* LAME_ANDROID_CONFIG_H */
