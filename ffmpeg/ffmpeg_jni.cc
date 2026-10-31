/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <android/log.h>
// 영상 프레임을 화면(Surface)에 직접 부으려면 필요하다. 소리 쪽에는 없던 것이다.
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdlib.h>
#include <new>
#include <string.h>

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#define LOG_TAG "ffmpeg_jni"
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define LOGD(...) \
  ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))

#define LIBRARY_FUNC(RETURN_TYPE, NAME, ...)                               \
  extern "C" {                                                             \
  JNIEXPORT RETURN_TYPE                                                    \
  Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_##NAME(JNIEnv* env,    \
                                                           jobject thiz,   \
                                                           ##__VA_ARGS__); \
  }                                                                        \
  JNIEXPORT RETURN_TYPE                                                    \
  Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_##NAME(                \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

#define AUDIO_DECODER_FUNC(RETURN_TYPE, NAME, ...)               \
  extern "C" {                                                   \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);                 \
  }                                                              \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

#define ERROR_STRING_BUFFER_LENGTH 256

// Output format corresponding to AudioFormat.ENCODING_PCM_16BIT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
// Output format corresponding to AudioFormat.ENCODING_PCM_FLOAT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;

// LINT.IfChange
static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;
// LINT.ThenChange(../java/androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder.java)

static jmethodID growOutputBufferMethod;

// 영상 쪽(아래 "영상" 구역에서 쓴다).
static jmethodID initForYuvFrameMethod;
static jfieldID videoBufferDataField;
static jfieldID videoBufferWidthField;
static jfieldID videoBufferHeightField;
static jfieldID videoBufferYStrideField;
static jfieldID videoBufferUvStrideField;

// 상자 쪽(아래 "상자" 구역에서 쓴다). 자바의 스트림에서 읽어 오는 통로다.
static jmethodID readFromInputMethod;
static jmethodID seekInInputMethod;
static jmethodID getInputSizeMethod;

/**
 * Returns the AVCodec with the specified name, or NULL if it is not available.
 */
const AVCodec* getCodecByName(JNIEnv* env, jstring codecName);

/**
 * Allocates and opens a new AVCodecContext for the specified codec, passing the
 * provided extraData as initialization data for the decoder if it is non-NULL.
 * Returns the created context.
 */
AVCodecContext* createContext(JNIEnv* env, const AVCodec* codec,
                              jbyteArray extraData, jboolean outputFloat,
                              jint rawSampleRate, jint rawChannelCount,
                              jint blockAlign, jint bitsPerCodedSample,
                              jint bitRate, jboolean useContainerInfo);

struct GrowOutputBufferCallback {
  uint8_t* operator()(int requiredSize) const;

  JNIEnv* env;
  jobject thiz;
  jobject decoderOutputBuffer;
};

/**
 * Decodes the packet into the output buffer, returning the number of bytes
 * written, or a negative AUDIO_DECODER_ERROR constant value in the case of an
 * error.
 */
int decodePacket(AVCodecContext* context, AVPacket* packet,
                 uint8_t* outputBuffer, int outputSize,
                 GrowOutputBufferCallback growBuffer);

/**
 * Transforms ffmpeg AVERROR into a negative AUDIO_DECODER_ERROR constant value.
 */
int transformError(int errorNumber);

/**
 * Outputs a log message describing the avcodec error number.
 */
void logError(const char* functionName, int errorNumber);

/**
 * Releases the specified context.
 */
void releaseContext(AVCodecContext* context);

/**
 * 소리 변환기(swr)와, 처음 프레임에서 정한 **출력** 형식. 오디오 디코더 문맥의 `opaque`에 단다.
 *
 * 원본(Media3)은 변환기를 처음 프레임의 채널 구성으로 한 번만 만들었다. 흐름 도중에 채널이 바뀌면
 * (5.1 → 2.0으로 바뀌는 방송 AC3·DTS를 담은 MKV — 상자 머리는 처음 형식만 적는다) 평면 형식은
 * 없는 채널 칸(NULL)을 읽어 죽고, 섞인 형식은 입력·출력 버퍼를 넘겨 읽고 썼다(2026-10-29 보안 점검 M4).
 * 이제 입력이 바뀌면 변환기를 다시 만들되 **출력은 처음 형식 그대로** 둔다 — 자바 쪽
 * `FfmpegAudioDecoder`는 첫 디코딩 뒤 채널 수·표본율을 한 번만 읽어 그 형식으로 소리 장치를 연다.
 */
struct AudioResampler {
  SwrContext* swr = nullptr;
  AVChannelLayout inLayout = {};
  AVSampleFormat inFormat = AV_SAMPLE_FMT_NONE;
  int inRate = 0;
  AVChannelLayout outLayout = {};
  int outRate = 0;
  bool hasOutput = false;
};

static void freeResampler(AudioResampler* resampler) {
  if (!resampler) {
    return;
  }
  swr_free(&resampler->swr);
  av_channel_layout_uninit(&resampler->inLayout);
  av_channel_layout_uninit(&resampler->outLayout);
  delete resampler;
}

/** 순서를 모르는 구성(채널 수만 앎)은 그 수의 기본 구성으로 바꿔 담는다 — 그래야 swr가 채널을 섞을 수 있다. */
static void copyLayout(AVChannelLayout* target, const AVChannelLayout* source) {
  av_channel_layout_uninit(target);
  if (source->order == AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_default(target, source->nb_channels);
  } else {
    av_channel_layout_copy(target, source);
  }
}

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    LOGE("JNI_OnLoad: GetEnv failed");
    return -1;
  }
  jclass clazz =
      env->FindClass("androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder");
  if (!clazz) {
    LOGE("JNI_OnLoad: FindClass failed");
    return -1;
  }
  growOutputBufferMethod =
      env->GetMethodID(clazz, "growOutputBuffer",
                       "(Landroidx/media3/decoder/"
                       "SimpleDecoderOutputBuffer;I)Ljava/nio/ByteBuffer;");
  if (!growOutputBufferMethod) {
    LOGE("JNI_OnLoad: GetMethodID failed");
    return -1;
  }

  /*
   * 영상 쪽. 프레임마다 찾으면 비싸서 여기서 한 번만 찾아 둔다.
   *
   * initForYuvFrame은 그림 크기에 맞춰 자바 버퍼를 다시 잡아 주고, data는 그
   * 버퍼를 가리키는 자리다 — 네이티브는 거기에 직접 픽셀을 붓는다.
   */
  jclass videoBufferClass =
      env->FindClass("androidx/media3/decoder/VideoDecoderOutputBuffer");
  if (!videoBufferClass) {
    LOGE("JNI_OnLoad: VideoDecoderOutputBuffer FindClass failed");
    return -1;
  }
  initForYuvFrameMethod =
      env->GetMethodID(videoBufferClass, "initForYuvFrame", "(IIIII)Z");
  videoBufferDataField =
      env->GetFieldID(videoBufferClass, "data", "Ljava/nio/ByteBuffer;");
  videoBufferWidthField = env->GetFieldID(videoBufferClass, "width", "I");
  videoBufferHeightField = env->GetFieldID(videoBufferClass, "height", "I");
  videoBufferYStrideField = env->GetFieldID(videoBufferClass, "yStride", "I");
  videoBufferUvStrideField = env->GetFieldID(videoBufferClass, "uvStride", "I");
  if (!initForYuvFrameMethod || !videoBufferDataField ||
      !videoBufferWidthField || !videoBufferHeightField ||
      !videoBufferYStrideField || !videoBufferUvStrideField) {
    LOGE("JNI_OnLoad: VideoDecoderOutputBuffer members not found");
    return -1;
  }
  /*
   * 상자 쪽. FFmpeg이 자료를 더 달라고 할 때 이 통로로 자바에게 묻는다.
   */
  jclass demuxerClass =
      env->FindClass("androidx/media3/decoder/ffmpeg/FfmpegDemuxer");
  if (!demuxerClass) {
    LOGE("JNI_OnLoad: FfmpegDemuxer FindClass failed");
    return -1;
  }
  readFromInputMethod = env->GetMethodID(demuxerClass, "readFromInput", "([BI)I");
  seekInInputMethod = env->GetMethodID(demuxerClass, "seekInInput", "(JI)J");
  getInputSizeMethod = env->GetMethodID(demuxerClass, "getInputSize", "()J");
  if (!readFromInputMethod || !seekInInputMethod || !getInputSizeMethod) {
    LOGE("JNI_OnLoad: FfmpegDemuxer members not found");
    return -1;
  }
  return JNI_VERSION_1_6;
}

LIBRARY_FUNC(jstring, ffmpegGetVersion) {
  return env->NewStringUTF(LIBAVCODEC_IDENT);
}

LIBRARY_FUNC(jint, ffmpegGetInputBufferPaddingSize) {
  return (jint)AV_INPUT_BUFFER_PADDING_SIZE;
}

/**
 * AVI의 코덱 번호 -> FFmpeg 디코더 이름. 모르면 {@code null}.
 *
 * <p><b>표를 손으로 채우는 대신 FFmpeg에게 묻는다.</b> AVI가 쓰는 fourcc(영상)와
 * wFormatTag(소리)는 수백 가지인데, 우리가 아는 것만 적어 두면 늘 무언가 빠진다.
 * 실제로 `DUCK`(TrueMotion) 같은 것이 표에 없어 트랙째로 버려졌다. FFmpeg은 그 표를
 * 통째로 들고 있고 공개 API로 내준다.
 *
 * @param video 참이면 영상 fourcc, 거짓이면 소리 wFormatTag.
 */
LIBRARY_FUNC(jstring, ffmpegGetCodecNameForTag, jint tag, jboolean video) {
  const struct AVCodecTag* const tables[] = {
      video ? avformat_get_riff_video_tags() : avformat_get_riff_audio_tags(), NULL};
  enum AVCodecID id = av_codec_get_id(tables, (unsigned int)tag);
  if (id == AV_CODEC_ID_NONE) {
    return NULL;
  }
  const AVCodec* codec = avcodec_find_decoder(id);
  if (!codec || !codec->name) {
    return NULL;
  }
  return env->NewStringUTF(codec->name);
}

LIBRARY_FUNC(jboolean, ffmpegHasDecoder, jstring codecName) {
  return getCodecByName(env, codecName) != NULL;
}

AUDIO_DECODER_FUNC(jlong, ffmpegInitialize, jstring codecName,
                   jbyteArray extraData, jboolean outputFloat,
                   jint rawSampleRate, jint rawChannelCount,
                   jint blockAlign, jint bitsPerCodedSample, jint bitRate,
                   jboolean useContainerInfo) {
  const AVCodec* codec = getCodecByName(env, codecName);
  if (!codec) {
    LOGE("Codec not found.");
    return 0L;
  }
  return (jlong)createContext(env, codec, extraData, outputFloat, rawSampleRate,
                              rawChannelCount, blockAlign, bitsPerCodedSample,
                              bitRate, useContainerInfo);
}

AUDIO_DECODER_FUNC(jint, ffmpegDecode, jlong context, jobject inputData,
                   jint inputSize, jobject decoderOutputBuffer,
                   jobject outputData, jint outputSize) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  if (!inputData || !decoderOutputBuffer || !outputData) {
    LOGE("Input and output buffers must be non-NULL.");
    return -1;
  }
  if (inputSize < 0) {
    LOGE("Invalid input buffer size: %d.", inputSize);
    return -1;
  }
  if (outputSize < 0) {
    LOGE("Invalid output buffer length: %d", outputSize);
    return -1;
  }
  uint8_t* inputBuffer = (uint8_t*)env->GetDirectBufferAddress(inputData);
  uint8_t* outputBuffer = (uint8_t*)env->GetDirectBufferAddress(outputData);
  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    LOGE("Failed to allocate packet.");
    return -1;
  }
  packet->data = inputBuffer;
  packet->size = inputSize;
  const int ret =
      decodePacket((AVCodecContext*)context, packet, outputBuffer, outputSize,
                   GrowOutputBufferCallback{env, thiz, decoderOutputBuffer});
  av_packet_free(&packet);
  return ret;
}

uint8_t* GrowOutputBufferCallback::operator()(int requiredSize) const {
  jobject newOutputData = env->CallObjectMethod(
      thiz, growOutputBufferMethod, decoderOutputBuffer, requiredSize);
  if (env->ExceptionCheck()) {
    LOGE("growOutputBuffer() failed");
    env->ExceptionDescribe();
    return nullptr;
  }
  return static_cast<uint8_t*>(env->GetDirectBufferAddress(newOutputData));
}

AUDIO_DECODER_FUNC(jint, ffmpegGetChannelCount, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  // 내보내는 형식은 변환기의 출력이다 — 도중에 입력 채널이 바뀌어도 이것은 그대로다.
  AudioResampler* resampler = static_cast<AudioResampler*>(((AVCodecContext*)context)->opaque);
  if (resampler && resampler->hasOutput) {
    return resampler->outLayout.nb_channels;
  }
  return ((AVCodecContext*)context)->ch_layout.nb_channels;
}

AUDIO_DECODER_FUNC(jint, ffmpegGetSampleRate, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  AudioResampler* resampler = static_cast<AudioResampler*>(((AVCodecContext*)context)->opaque);
  if (resampler && resampler->hasOutput) {
    return resampler->outRate;
  }
  return ((AVCodecContext*)context)->sample_rate;
}

AUDIO_DECODER_FUNC(jlong, ffmpegReset, jlong jContext, jbyteArray extraData) {
  AVCodecContext* context = (AVCodecContext*)jContext;
  if (!context) {
    LOGE("Tried to reset without a context.");
    return 0L;
  }

  AVCodecID codecId = context->codec_id;
  if (codecId == AV_CODEC_ID_TRUEHD) {
    jboolean outputFloat =
        (jboolean)(context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
    // Release and recreate the context if the codec is TrueHD.
    // TODO: Figure out why flushing doesn't work for this codec.
    // 자바 쪽이 이미 연 출력 형식은 새 문맥에도 넘긴다(새 문맥의 첫 프레임 형식으로 바뀌면 안 된다).
    AudioResampler* old = static_cast<AudioResampler*>(context->opaque);
    AudioResampler* kept = nullptr;
    if (old && old->hasOutput) {
      kept = new (std::nothrow) AudioResampler();
      if (kept) {
        copyLayout(&kept->outLayout, &old->outLayout);
        kept->outRate = old->outRate;
        kept->hasOutput = true;
      }
    }
    releaseContext(context);
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
      LOGE("Unexpected error finding codec %d.", codecId);
      freeResampler(kept);
      return 0L;
    }
    AVCodecContext* fresh = createContext(env, codec, extraData, outputFloat,
                                          /* rawSampleRate= */ -1,
                                          /* rawChannelCount= */ -1,
                                          /* blockAlign= */ 0,
                                          /* bitsPerCodedSample= */ 0, /* bitRate= */ 0,
                                          /* useContainerInfo= */ JNI_FALSE);
    if (fresh) {
      fresh->opaque = kept;
    } else {
      freeResampler(kept);
    }
    return (jlong)fresh;
  }

  avcodec_flush_buffers(context);
  return (jlong)context;
}

AUDIO_DECODER_FUNC(void, ffmpegRelease, jlong context) {
  if (context) {
    releaseContext((AVCodecContext*)context);
  }
}

const AVCodec* getCodecByName(JNIEnv* env, jstring codecName) {
  if (!codecName) {
    return NULL;
  }
  const char* codecNameChars = env->GetStringUTFChars(codecName, NULL);
  const AVCodec* codec = avcodec_find_decoder_by_name(codecNameChars);
  env->ReleaseStringUTFChars(codecName, codecNameChars);
  return codec;
}

AVCodecContext* createContext(JNIEnv* env, const AVCodec* codec,
                              jbyteArray extraData, jboolean outputFloat,
                              jint rawSampleRate, jint rawChannelCount,
                              jint blockAlign, jint bitsPerCodedSample,
                              jint bitRate, jboolean useContainerInfo) {
  AVCodecContext* context = avcodec_alloc_context3(codec);
  if (!context) {
    LOGE("Failed to allocate context.");
    return NULL;
  }
  context->request_sample_fmt =
      outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    context->extradata_size = size;
    context->extradata =
        (uint8_t*)av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
      LOGE("Failed to allocate extradata.");
      releaseContext(context);
      return NULL;
    }
    env->GetByteArrayRegion(extraData, 0, size, (jbyte*)context->extradata);
  }
  if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
      context->codec_id == AV_CODEC_ID_PCM_ALAW) {
    context->sample_rate = rawSampleRate;
    av_channel_layout_default(&context->ch_layout, rawChannelCount);
  }
  /*
   * **WMA·ADPCM은 이것 없이는 열리지도 않는다.** (원본에 없던 자리다.)
   *
   * 다른 코덱은 알맹이나 머리말에 표본율·채널·프레임 크기가 들어 있어 디코더가
   * 스스로 알아내지만, WMA는 그 셋이 **상자 쪽 WAVEFORMATEX에만** 적혀 있다.
   * 특히 block_align(압축된 덩어리 하나의 크기)이 0이면 `avcodec_open2`가
   * "no block align" 으로 실패한다 — 소리가 이상해지는 게 아니라 아예 안 열린다.
   *
   * 값은 해석기가 상자에서 읽어 실어 보낸다(AVI는 [StreamFormatChunk], 그 밖은
   * [FfmpegExtractor]). block_align이 0이면 그런 코덱이 아니라는 뜻이라 손대지 않는다 —
   * 알맹이에 정보가 든 코덱에 엉뚱한 값을 넣으면 오히려 어긋난다.
   */
  if (useContainerInfo) {
    context->sample_rate = rawSampleRate;
    av_channel_layout_default(&context->ch_layout, rawChannelCount);
    if (blockAlign > 0) {
      context->block_align = blockAlign;
    }
    /*
     * APE 같은 코덱은 block_align이 아니라 이 값을 요구한다. 없으면 열기가 실패한다.
     */
    if (bitsPerCodedSample > 0) {
      context->bits_per_coded_sample = bitsPerCodedSample;
    }
    if (bitRate > 0) {
      context->bit_rate = bitRate;
    }
  }
  context->err_recognition = AV_EF_IGNORE_ERR;
  int result = avcodec_open2(context, codec, NULL);
  if (result < 0) {
    logError("avcodec_open2", result);
    releaseContext(context);
    return NULL;
  }
  return context;
}

int decodePacket(AVCodecContext* context, AVPacket* packet,
                 uint8_t* outputBuffer, int outputSize,
                 GrowOutputBufferCallback growBuffer) {
  int result = 0;
  // Queue input data.
  result = avcodec_send_packet(context, packet);
  if (result) {
    logError("avcodec_send_packet", result);
    return transformError(result);
  }

  // Dequeue output data until it runs out.
  int outSize = 0;
  while (true) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
      LOGE("Failed to allocate output frame.");
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    result = avcodec_receive_frame(context, frame);
    if (result) {
      av_frame_free(&frame);
      if (result == AVERROR(EAGAIN)) {
        break;
      }
      logError("avcodec_receive_frame", result);
      return transformError(result);
    }

    // Resample output.
    // 형식은 문맥이 아니라 이 프레임의 것으로 본다 — 흐름 도중에 바뀔 수 있다([AudioResampler]).
    AudioResampler* resampler = static_cast<AudioResampler*>(context->opaque);
    if (!resampler) {
      resampler = new (std::nothrow) AudioResampler();
      if (!resampler) {
        LOGE("Failed to allocate the resampler.");
        av_frame_free(&frame);
        return AUDIO_DECODER_ERROR_OTHER;
      }
      context->opaque = resampler;
    }
    AVSampleFormat sampleFormat = static_cast<AVSampleFormat>(frame->format);
    int sampleRate = frame->sample_rate > 0 ? frame->sample_rate : context->sample_rate;
    AVChannelLayout frameLayout = {};
    copyLayout(&frameLayout, frame->ch_layout.nb_channels > 0 ? &frame->ch_layout
                                                              : &context->ch_layout);
    if (!resampler->hasOutput) {
      copyLayout(&resampler->outLayout, &frameLayout);
      resampler->outRate = sampleRate;
      resampler->hasOutput = true;
    }
    if (!resampler->swr ||
        av_channel_layout_compare(&resampler->inLayout, &frameLayout) != 0 ||
        resampler->inFormat != sampleFormat || resampler->inRate != sampleRate) {
      if (resampler->swr) {
        LOGD("Audio input changed to %d channels at %d Hz; rebuilding the resampler.",
             frameLayout.nb_channels, sampleRate);
        swr_free(&resampler->swr);
      }
      result = swr_alloc_set_opts2(&resampler->swr,               // ps
                                   &resampler->outLayout,         // out_ch_layout
                                   context->request_sample_fmt,   // out_sample_fmt
                                   resampler->outRate,            // out_sample_rate
                                   &frameLayout,                  // in_ch_layout
                                   sampleFormat,                  // in_sample_fmt
                                   sampleRate,                    // in_sample_rate
                                   0,                             // log_offset
                                   NULL                           // log_ctx
      );
      if (result >= 0) {
        result = swr_init(resampler->swr);
      }
      if (result < 0) {
        logError("swr_init", result);
        swr_free(&resampler->swr);
        av_channel_layout_uninit(&frameLayout);
        av_frame_free(&frame);
        return transformError(result);
      }
      copyLayout(&resampler->inLayout, &frameLayout);
      resampler->inFormat = sampleFormat;
      resampler->inRate = sampleRate;
    }
    av_channel_layout_uninit(&frameLayout);

    int outChannelCount = resampler->outLayout.nb_channels;
    int outSampleSize = av_get_bytes_per_sample(context->request_sample_fmt);
    int outSamples = swr_get_out_samples(resampler->swr, frame->nb_samples);
    if (outSamples < 0) {
      logError("swr_get_out_samples", outSamples);
      av_frame_free(&frame);
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    int bufferOutSize = outSampleSize * outChannelCount * outSamples;
    if (outSize + bufferOutSize > outputSize) {
      LOGD(
          "Output buffer size (%d) too small for output data (%d), "
          "reallocating buffer.",
          outputSize, outSize + bufferOutSize);
      outputSize = outSize + bufferOutSize;
      uint8_t* grown = growBuffer(outputSize);
      if (!grown) {
        LOGE("Failed to reallocate output buffer.");
        av_frame_free(&frame);
        return AUDIO_DECODER_ERROR_OTHER;
      }
      // 새 버퍼는 앞 프레임들의 출력을 담은 채 처음 주소로 온다 — 이어 쓸 자리는 그 뒤다
      // (원본은 처음 주소에 덮어써 한 패킷에 프레임이 여럿일 때 앞 소리를 지웠다).
      outputBuffer = grown + outSize;
    }
    // 출력 한도는 **표본 수**다(원본은 바이트 수를 넘겨, 적힌 크기보다 더 쓸 수 있었다).
    result = swr_convert(resampler->swr, &outputBuffer, outSamples,
                         (const uint8_t**)frame->extended_data, frame->nb_samples);
    av_frame_free(&frame);
    if (result < 0) {
      logError("swr_convert", result);
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }
    // 표본율을 바꾸지 않을 때만 남는 것이 없어야 한다(바꾸면 변환기가 다음 프레임 몫을 조금 들고 있다).
    if (resampler->inRate == resampler->outRate) {
      int available = swr_get_out_samples(resampler->swr, 0);
      if (available != 0) {
        LOGE("Expected no samples remaining after resampling, but found %d.",
             available);
        return AUDIO_DECODER_ERROR_INVALID_DATA;
      }
    }
    int written = result * outSampleSize * outChannelCount;
    outputBuffer += written;
    outSize += written;
  }
  return outSize;
}

int transformError(int errorNumber) {
  return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                            : AUDIO_DECODER_ERROR_OTHER;
}

void logError(const char* functionName, int errorNumber) {
  char* buffer = (char*)malloc(ERROR_STRING_BUFFER_LENGTH * sizeof(char));
  av_strerror(errorNumber, buffer, ERROR_STRING_BUFFER_LENGTH);
  LOGE("Error in %s: %s", functionName, buffer);
  free(buffer);
}

void releaseContext(AVCodecContext* context) {
  if (!context) {
    return;
  }
  freeResampler(static_cast<AudioResampler*>(context->opaque));
  context->opaque = NULL;
  avcodec_free_context(&context);
}

// ===========================================================================
// 영상
//
// **이 아래는 Media3 원본에 없다.** 원본의 ExperimentalFfmpegVideoRenderer는 속이
// 비어 있고("NOT yet functional"), 영상 디코더는 아예 없다. 기기가 못 푸는 옛
// 코덱(WMV·DivX3·MPEG-2·MJPEG 등)을 풀려고 여기서 만든다.
//
// 짝이 되는 자바는 [FfmpegVideoDecoder]다.
//
// 화면에 내는 방법: 디코딩 결과를 **출력 버퍼에 YUV420P로 담는다.** 재생 화면에는 자바의
// FfmpegYuvGlRenderer가 GL로 붓고, 아래 ffmpegRenderFrame(ANativeWindow에 붓는 길)은
// 썸네일의 ImageReader에만 쓴다 — 표면을 놓아 주지 못해 재생 화면에는 못 쓴다. 프레임을 네이티브에 쥐고 있다가 나중에
// 그리는 길(libgav1이 쓰는 방식)도 있지만, 그러면 출력 버퍼가 여럿일 때 어느
// 프레임을 그릴지 짝을 맞춰야 한다. 버퍼마다 제 그림을 안고 가면 그 걱정이 없다.
// 대신 복사가 한 번 더 든다.
// ===========================================================================

#define VIDEO_DECODER_FUNC(RETURN_TYPE, NAME, ...)               \
  extern "C" {                                                   \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);                 \
  }                                                              \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

// LINT.IfChange
static const int VIDEO_DECODER_ERROR_INVALID_DATA = -1;
static const int VIDEO_DECODER_ERROR_OTHER = -2;
/** 넣은 것은 받았지만 아직 내놓을 그림이 없다. 오류가 아니다. */
static const int VIDEO_DECODER_ERROR_NO_FRAME = -3;
static const int VIDEO_DECODER_SUCCESS = 0;
// LINT.ThenChange(../java/androidx/media3/decoder/ffmpeg/FfmpegVideoDecoder.java)

/** VideoDecoderOutputBuffer.COLORSPACE_BT601. */
static const int COLORSPACE_BT601 = 1;

struct VideoContext {
  AVCodecContext* codecContext;
  /** 디코더가 내놓은 그림. */
  AVFrame* frame;
  /** YUV420P가 아닌 그림을 옮겨 담을 자리. 필요할 때만 만든다. */
  AVFrame* convertedFrame;
  SwsContext* swsContext;
  int swsSrcFormat;
  int swsWidth;
  int swsHeight;
  /** 화면에 부을 때 쓰는 YUV420P -> RGBA 변환기. */
  SwsContext* renderSwsContext;
  int renderWidth;
  int renderHeight;
};

static void releaseVideoContext(VideoContext* context) {
  if (!context) {
    return;
  }
  if (context->renderSwsContext) {
    sws_freeContext(context->renderSwsContext);
  }
  if (context->swsContext) {
    sws_freeContext(context->swsContext);
  }
  if (context->convertedFrame) {
    av_frame_free(&context->convertedFrame);
  }
  if (context->frame) {
    av_frame_free(&context->frame);
  }
  if (context->codecContext) {
    avcodec_free_context(&context->codecContext);
  }
  free(context);
}

VIDEO_DECODER_FUNC(jlong, ffmpegInitialize, jstring codecName,
                   jbyteArray extraData, jint threadCount, jint width,
                   jint height) {
  const AVCodec* codec = getCodecByName(env, codecName);
  if (!codec) {
    LOGE("Video codec not found.");
    return 0L;
  }
  VideoContext* context = (VideoContext*)calloc(1, sizeof(VideoContext));
  if (!context) {
    return 0L;
  }
  context->swsSrcFormat = AV_PIX_FMT_NONE;
  context->codecContext = avcodec_alloc_context3(codec);
  if (!context->codecContext) {
    LOGE("Failed to allocate video context.");
    releaseVideoContext(context);
    return 0L;
  }
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    context->codecContext->extradata_size = size;
    context->codecContext->extradata =
        (uint8_t*)av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->codecContext->extradata) {
      LOGE("Failed to allocate video extradata.");
      releaseVideoContext(context);
      return 0L;
    }
    memset(context->codecContext->extradata + size, 0,
           AV_INPUT_BUFFER_PADDING_SIZE);
    env->GetByteArrayRegion(extraData, 0, size,
                            (jbyte*)context->codecContext->extradata);
  }
  /*
   * 깨진 조각에서 멈추지 않게 한다. 옛 코덱이 든 파일은 떠돌다 조각이 잘려 나간
   * 것이 흔한데, 거기서 재생이 서면 사용자가 잃는 것이 더 크다. 소리 쪽도 같은
   * 값을 쓴다.
   */
  context->codecContext->err_recognition = AV_EF_IGNORE_ERR;
  /*
   * **시간 단위를 알려주지 않으면 되돌아오는 시각이 엉뚱해진다.**
   *
   * 우리는 마이크로초를 pts에 실어 보내는데, FFmpeg은 그 숫자가 무슨 단위인지
   * pkt_timebase로만 안다. 안 알려주면 best_effort_timestamp 계산이 어긋나고,
   * 재생기는 그 그림을 먼 미래의 것으로 보고 붙들어 둔다 — 오류 하나 없이 화면만
   * 검은 채로 몇 장 만에 멈춘다. B프레임으로 차례가 바뀌는 코덱(VC-1 등)에서 드러난다.
   */
  context->codecContext->pkt_timebase = AVRational{1, 1000000};
  context->codecContext->thread_count = threadCount;
  context->codecContext->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  /*
   * **크기를 넣지 않으면 열리지도 않는다.** 옛 코덱(WMV·MSMPEG4 등)은 여는 자리에서
   * 프레임 버퍼를 미리 잡느라 가로세로를 요구하는데, 그 값은 알맹이가 아니라 상자에만
   * 있다. 안 넣으면 avcodec_open2가 "Invalid argument"로 거부한다.
   */
  if (width > 0 && height > 0) {
    context->codecContext->width = width;
    context->codecContext->height = height;
  }

  int result = avcodec_open2(context->codecContext, codec, NULL);
  if (result < 0) {
    logError("avcodec_open2", result);
    releaseVideoContext(context);
    return 0L;
  }
  context->frame = av_frame_alloc();
  if (!context->frame) {
    releaseVideoContext(context);
    return 0L;
  }
  return (jlong)context;
}

VIDEO_DECODER_FUNC(jint, ffmpegSendPacket, jlong jContext, jobject encoded,
                   jint length, jlong inputTimeUs) {
  VideoContext* context = (VideoContext*)jContext;
  if (!context) {
    return VIDEO_DECODER_ERROR_OTHER;
  }
  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    return VIDEO_DECODER_ERROR_OTHER;
  }
  packet->data = (uint8_t*)env->GetDirectBufferAddress(encoded);
  packet->size = length;
  /*
   * 되돌아 나올 그림에 시각을 태워 보낸다. B프레임이 있으면 넣은 차례와 나오는
   * 차례가 달라서, 이렇게 하지 않으면 어느 그림이 몇 초짜리인지 짝을 잃는다.
   */
  packet->pts = inputTimeUs;

  int result = avcodec_send_packet(context->codecContext, packet);
  av_packet_free(&packet);
  if (result) {
    if (result == AVERROR_INVALIDDATA) {
      return VIDEO_DECODER_ERROR_INVALID_DATA;
    }
    if (result == AVERROR(EAGAIN)) {
      // 아직 안 꺼내 간 그림이 있다는 뜻이다. 자바 쪽이 먼저 꺼내 가면 된다.
      return VIDEO_DECODER_ERROR_NO_FRAME;
    }
    logError("avcodec_send_packet", result);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  return VIDEO_DECODER_SUCCESS;
}

/**
 * 그림 하나를 꺼내 출력 버퍼에 담는다.
 *
 * <p>돌려주는 값은 <b>그 그림의 시각(마이크로초)</b>이고, 음수면 위의 오류 값이다.
 * 시각은 늘 0 이상이라 갈라 볼 수 있다.
 *
 * <p>YUV420P가 아니면 swscale로 옮겨 담는다 — MJPEG이 4:2:2·4:4:4로 나오는 것이
 * 대표적이다. 이미 420이면 변환하지 않고 그대로 복사한다(대부분이 이쪽이다).
 */
VIDEO_DECODER_FUNC(jlong, ffmpegReceiveFrame, jlong jContext,
                   jobject jOutputBuffer) {
  VideoContext* context = (VideoContext*)jContext;
  if (!context) {
    return VIDEO_DECODER_ERROR_OTHER;
  }
  int result = avcodec_receive_frame(context->codecContext, context->frame);
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return VIDEO_DECODER_ERROR_NO_FRAME;
  }
  if (result < 0) {
    logError("avcodec_receive_frame", result);
    return VIDEO_DECODER_ERROR_OTHER;
  }

  AVFrame* frame = context->frame;
  int width = frame->width;
  int height = frame->height;
  if (width <= 0 || height <= 0) {
    av_frame_unref(frame);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  int64_t timeUs = frame->best_effort_timestamp;
  if (timeUs == AV_NOPTS_VALUE) {
    timeUs = frame->pts;
  }
  if (timeUs == AV_NOPTS_VALUE || timeUs < 0) {
    timeUs = 0;
  }

  AVFrame* source = frame;
  if (frame->format != AV_PIX_FMT_YUV420P &&
      frame->format != AV_PIX_FMT_YUVJ420P) {
    // 형식이나 크기가 바뀌면 변환기를 다시 만든다.
    if (!context->swsContext || context->swsSrcFormat != frame->format ||
        context->swsWidth != width || context->swsHeight != height) {
      if (context->swsContext) {
        sws_freeContext(context->swsContext);
        context->swsContext = NULL;
      }
      if (context->convertedFrame) {
        av_frame_free(&context->convertedFrame);
      }
      context->swsContext = sws_getContext(
          width, height, (AVPixelFormat)frame->format, width, height,
          AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
      context->swsSrcFormat = frame->format;
      context->swsWidth = width;
      context->swsHeight = height;
      context->convertedFrame = av_frame_alloc();
      if (context->convertedFrame) {
        context->convertedFrame->format = AV_PIX_FMT_YUV420P;
        context->convertedFrame->width = width;
        context->convertedFrame->height = height;
        if (av_frame_get_buffer(context->convertedFrame, 0) < 0) {
          av_frame_free(&context->convertedFrame);
        }
      }
    }
    if (!context->swsContext || !context->convertedFrame) {
      LOGE("Failed to set up pixel format conversion.");
      av_frame_unref(frame);
      return VIDEO_DECODER_ERROR_OTHER;
    }
    sws_scale(context->swsContext, frame->data, frame->linesize, 0, height,
              context->convertedFrame->data, context->convertedFrame->linesize);
    source = context->convertedFrame;
  }

  // 자바 쪽 버퍼를 이 그림 크기에 맞춰 다시 잡는다.
  jboolean initResult = env->CallBooleanMethod(
      jOutputBuffer, initForYuvFrameMethod, width, height, source->linesize[0],
      source->linesize[1], COLORSPACE_BT601);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    av_frame_unref(frame);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  if (!initResult) {
    av_frame_unref(frame);
    return VIDEO_DECODER_ERROR_OTHER;
  }

  jobject dataObject = env->GetObjectField(jOutputBuffer, videoBufferDataField);
  uint8_t* data = (uint8_t*)env->GetDirectBufferAddress(dataObject);
  if (!data) {
    av_frame_unref(frame);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  const int uvHeight = (height + 1) / 2;
  const size_t yLength = (size_t)source->linesize[0] * height;
  const size_t uvLength = (size_t)source->linesize[1] * uvHeight;
  memcpy(data, source->data[0], yLength);
  memcpy(data + yLength, source->data[1], uvLength);
  memcpy(data + yLength + uvLength, source->data[2], uvLength);

  av_frame_unref(frame);
  return (jlong)timeUs;
}

/**
 * 출력 버퍼에 담긴 그림을 화면에 붓는다.
 *
 * <p>창을 <b>RGBA_8888</b>로 잡고 swscale로 색을 바꿔 넣는다.
 *
 * <p>처음에는 YV12(안드로이드가 아는 4:2:0 평면 형식)로 잡아 세 판을 그대로
 * 복사했다. 색 변환이 없어 더 빠르지만 <b>에뮬레이터에서 ANativeWindow_lock이
 * stride를 0으로 돌려준다.</b> 그러면 모든 줄이 같은 자리에 겹쳐 쓰이고 나머지가
 * 0으로 남아, 화면이 통째로 초록(#008700 — YUV가 전부 0일 때 나오는 색)이 된다.
 * RGBA는 CPU가 잠글 수 있는 형식으로 어디서나 보장되고 stride도 제대로 온다.
 */
VIDEO_DECODER_FUNC(jint, ffmpegRenderFrame, jlong jContext, jobject jSurface,
                   jobject jOutputBuffer, jint displayedWidth,
                   jint displayedHeight) {
  VideoContext* context = (VideoContext*)jContext;
  if (!context) {
    return VIDEO_DECODER_ERROR_OTHER;
  }
  ANativeWindow* window = ANativeWindow_fromSurface(env, jSurface);
  if (!window) {
    LOGE("Failed to get native window.");
    return VIDEO_DECODER_ERROR_OTHER;
  }
  jobject dataObject = env->GetObjectField(jOutputBuffer, videoBufferDataField);
  uint8_t* data = (uint8_t*)env->GetDirectBufferAddress(dataObject);
  if (!data) {
    ANativeWindow_release(window);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  const int frameWidth = env->GetIntField(jOutputBuffer, videoBufferWidthField);
  const int frameHeight =
      env->GetIntField(jOutputBuffer, videoBufferHeightField);
  const int srcYStride =
      env->GetIntField(jOutputBuffer, videoBufferYStrideField);
  const int srcUvStride =
      env->GetIntField(jOutputBuffer, videoBufferUvStrideField);
  if (frameWidth <= 0 || frameHeight <= 0 || srcYStride <= 0 ||
      srcUvStride <= 0) {
    ANativeWindow_release(window);
    return VIDEO_DECODER_ERROR_OTHER;
  }

  // 크기가 바뀌면 변환기를 다시 만든다.
  if (!context->renderSwsContext || context->renderWidth != frameWidth ||
      context->renderHeight != frameHeight) {
    if (context->renderSwsContext) {
      sws_freeContext(context->renderSwsContext);
    }
    context->renderSwsContext =
        sws_getContext(frameWidth, frameHeight, AV_PIX_FMT_YUV420P, frameWidth,
                       frameHeight, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL,
                       NULL);
    context->renderWidth = frameWidth;
    context->renderHeight = frameHeight;
  }
  if (!context->renderSwsContext) {
    LOGE("Failed to create render converter.");
    ANativeWindow_release(window);
    return VIDEO_DECODER_ERROR_OTHER;
  }

  if (ANativeWindow_setBuffersGeometry(window, frameWidth, frameHeight,
                                       WINDOW_FORMAT_RGBA_8888)) {
    LOGE("Failed to set native window geometry.");
    ANativeWindow_release(window);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  ANativeWindow_Buffer nativeBuffer;
  if (ANativeWindow_lock(window, &nativeBuffer, NULL)) {
    LOGE("Failed to lock native window.");
    ANativeWindow_release(window);
    return VIDEO_DECODER_ERROR_OTHER;
  }

  const int srcUvHeight = (frameHeight + 1) / 2;
  uint8_t* srcSlices[3];
  srcSlices[0] = data;
  srcSlices[1] = srcSlices[0] + (size_t)srcYStride * frameHeight;
  srcSlices[2] = srcSlices[1] + (size_t)srcUvStride * srcUvHeight;
  int srcStrides[3] = {srcYStride, srcUvStride, srcUvStride};

  uint8_t* dstSlices[1] = {(uint8_t*)nativeBuffer.bits};
  // stride는 픽셀 수라 바이트로 바꾼다 (RGBA는 한 픽셀이 4바이트).
  int dstStrides[1] = {nativeBuffer.stride * 4};
  const int scaleHeight =
      nativeBuffer.height < frameHeight ? nativeBuffer.height : frameHeight;

  sws_scale(context->renderSwsContext, srcSlices, srcStrides, 0, scaleHeight,
            dstSlices, dstStrides);

  if (ANativeWindow_unlockAndPost(window)) {
    LOGE("Failed to unlock and post to native window.");
    ANativeWindow_release(window);
    return VIDEO_DECODER_ERROR_OTHER;
  }
  ANativeWindow_release(window);
  return VIDEO_DECODER_SUCCESS;
}

VIDEO_DECODER_FUNC(void, ffmpegFlushBuffers, jlong jContext) {
  VideoContext* context = (VideoContext*)jContext;
  if (context && context->codecContext) {
    avcodec_flush_buffers(context->codecContext);
  }
}

VIDEO_DECODER_FUNC(void, ffmpegReleaseContext, jlong jContext) {
  releaseVideoContext((VideoContext*)jContext);
}

// ===========================================================================
// 창 크기 잡기 (편집이 GL로 부을 때만 쓴다)
//
// 짝이 되는 자바는 [FfmpegYuvGlRenderer]다.
//
// **왜 네이티브가 필요한가.** 편집은 그림을 SurfaceTexture에 GL로 붓는다. 그런데
// eglCreateWindowSurface는 만들 때 창에게 크기를 물어보고, SurfaceTexture의 기본
// 크기는 **1x1**이다. 그래서 아무것도 안 하면 1x1짜리 그림이 나가고, 결과물은 한 가지
// 색으로만 채워진다 (2026-09-04에 실제로 그렇게 나왔다 — 프레임 수도 맞고 오류도
// 없는데 색이 하나뿐이었다).
//
// MediaCodec은 이 문제가 없다. 네이티브 생산자라 제가 이 함수를 부르기 때문이다.
// **Media3도 이 자리를 일부러 비워 두었다** — SurfaceTexture.setDefaultBufferSize를
// 부르는 곳은 자동 등록 경로뿐이고, 편집이 쓰는 수동 등록 경로에는
// "b/351776002 - Call setDefaultBufferSize on the INPUT_TYPE_SURFACE path too"라는
// TODO만 있다. 즉 **생산자가 알아서 하라는 뜻**이고, 자바에는 그 길이 없다.
// ===========================================================================

#define YUV_GL_RENDERER_FUNC(RETURN_TYPE, NAME, ...)               \
  extern "C" {                                                     \
  JNIEXPORT RETURN_TYPE                                            \
  Java_androidx_media3_decoder_ffmpeg_FfmpegYuvGlRenderer_##NAME(  \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);                   \
  }                                                                \
  JNIEXPORT RETURN_TYPE                                            \
  Java_androidx_media3_decoder_ffmpeg_FfmpegYuvGlRenderer_##NAME(  \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

/**
 * 창이 내어 줄 그림칸의 크기를 잡는다. <b>EGL 창 표면을 만들기 전에 불러야 한다.</b>
 *
 * <p>형식에 0을 주면 창이 쓰던 형식을 그대로 둔다 — 색 형식은 EGL이 제 설정에 맞춰
 * 따로 정하므로 우리가 건드릴 자리가 아니다.
 */
YUV_GL_RENDERER_FUNC(jboolean, ffmpegSetWindowSize, jobject jSurface, jint width,
                     jint height) {
  if (width <= 0 || height <= 0) {
    return JNI_FALSE;
  }
  ANativeWindow* window = ANativeWindow_fromSurface(env, jSurface);
  if (!window) {
    LOGE("Failed to get native window.");
    return JNI_FALSE;
  }
  int32_t result =
      ANativeWindow_setBuffersGeometry(window, width, height, /* format= */ 0);
  ANativeWindow_release(window);
  if (result) {
    LOGE("Failed to set native window size.");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

// ===========================================================================
// 상자 (해석기)
//
// **이 아래도 Media3 원본에 없다.** Media3는 ASF(.wmv)·RealMedia(.rm) 같은 상자를
// 아예 모른다 — 알맹이를 풀 수 있어도 파일을 열지 못해 시작조차 안 된다. 그래서
// FFmpeg의 해석기(libavformat)를 빌려 그 상자만 연다.
//
// 짝이 되는 자바는 [FfmpegExtractor]다.
//
// **읽기는 앞으로만 한다**(`seekable = 0`). Media3가 주는 스트림은 되감기가 안 되고,
// ASF·RM은 원래 방송용이라 순차 해석이 된다. 되감기를 흉내 내려면 스레드를 하나 더
// 두고 위아래를 묶어야 하는데, 그 복잡함은 지금 얻는 것에 비해 너무 크다.
// ===========================================================================

#define DEMUXER_FUNC(RETURN_TYPE, NAME, ...)                  \
  extern "C" {                                                \
  JNIEXPORT RETURN_TYPE                                       \
  Java_androidx_media3_decoder_ffmpeg_FfmpegDemuxer_##NAME(   \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);              \
  }                                                           \
  JNIEXPORT RETURN_TYPE                                       \
  Java_androidx_media3_decoder_ffmpeg_FfmpegDemuxer_##NAME(   \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

// LINT.IfChange
static const int DEMUXER_ERROR_OTHER = -2;
static const int DEMUXER_RESULT_END_OF_INPUT = -1;
// LINT.ThenChange(../java/androidx/media3/decoder/ffmpeg/FfmpegDemuxer.java)

/** FFmpeg이 한 번에 읽어 갈 크기. */
static const int IO_BUFFER_SIZE = 64 * 1024;

struct DemuxerContext {
  AVFormatContext* formatContext;
  AVIOContext* ioContext;
  AVPacket* packet;
  /**
   * 자바가 읽어 담아 주는 자리. 전역 참조로 잡아 두어야 콜백에서 살아 있다.
   */
  jbyteArray scratch;
  /**
   * <b>콜백이 도는 동안에만 쓸 수 있다.</b> FFmpeg의 읽기 콜백은 우리가 부른
   * JNI 함수 안에서만 불리므로, 들어올 때 채우고 나갈 때 지운다.
   */
  JNIEnv* env;
  jobject thiz;
};

/**
 * FFmpeg이 자료를 더 달라고 할 때 불린다.
 *
 * <p>자바의 스트림에서 읽어 `scratch`에 담게 하고, 그것을 FFmpeg 버퍼로 옮긴다.
 * 자바를 거치는 이유는 Media3가 주는 스트림이 자바 객체이기 때문이다.
 */
static int readPacketCallback(void* opaque, uint8_t* buf, int bufSize) {
  DemuxerContext* context = (DemuxerContext*)opaque;
  if (!context->env || !context->thiz || !context->scratch) {
    return AVERROR_EXTERNAL;
  }
  if (bufSize > IO_BUFFER_SIZE) {
    bufSize = IO_BUFFER_SIZE;
  }
  jint read = context->env->CallIntMethod(context->thiz, readFromInputMethod,
                                          context->scratch, bufSize);
  if (context->env->ExceptionCheck()) {
    context->env->ExceptionClear();
    return AVERROR_EXTERNAL;
  }
  if (read <= 0) {
    // 끝났다는 뜻이다. FFmpeg은 EOF를 이 값으로 안다.
    return AVERROR_EOF;
  }
  context->env->GetByteArrayRegion(context->scratch, 0, read, (jbyte*)buf);
  return read;
}

/**
 * FFmpeg이 자리를 옮기고 싶을 때 불린다.
 *
 * <p>되감기까지 되는 통로가 있을 때만 붙인다. 자바 쪽이 파일을 한 번 더 열어
 * 임의 접근을 내주는 경우다. 없으면 이 콜백 자체를 달지 않아 FFmpeg이 앞으로만 읽는다.
 */
static int64_t seekCallback(void* opaque, int64_t offset, int whence) {
  DemuxerContext* context = (DemuxerContext*)opaque;
  if (!context->env || !context->thiz) {
    return AVERROR_EXTERNAL;
  }
  // AVSEEK_FORCE는 "되감기 되는지 다시 재보지 말라"는 덧붙임일 뿐이라 떼어낸다.
  int cleanWhence = whence & ~AVSEEK_FORCE;
  jlong result;
  if (cleanWhence == AVSEEK_SIZE) {
    result = context->env->CallLongMethod(context->thiz, getInputSizeMethod);
  } else {
    result = context->env->CallLongMethod(context->thiz, seekInInputMethod,
                                          (jlong)offset, (jint)cleanWhence);
  }
  if (context->env->ExceptionCheck()) {
    context->env->ExceptionClear();
    return AVERROR_EXTERNAL;
  }
  return result < 0 ? AVERROR(EINVAL) : (int64_t)result;
}

static void releaseDemuxerContext(JNIEnv* env, DemuxerContext* context) {
  if (!context) {
    return;
  }
  if (context->packet) {
    av_packet_free(&context->packet);
  }
  if (context->formatContext) {
    // avformat_close_input이 ioContext의 버퍼까지 정리하지 않으므로 순서를 지킨다.
    avformat_close_input(&context->formatContext);
  }
  if (context->ioContext) {
    av_freep(&context->ioContext->buffer);
    avio_context_free(&context->ioContext);
  }
  if (context->scratch) {
    env->DeleteGlobalRef(context->scratch);
  }
  free(context);
}

DEMUXER_FUNC(jlong, ffmpegOpen, jboolean seekable) {
  DemuxerContext* context = (DemuxerContext*)calloc(1, sizeof(DemuxerContext));
  if (!context) {
    return 0L;
  }
  jbyteArray scratch = env->NewByteArray(IO_BUFFER_SIZE);
  if (!scratch) {
    free(context);
    return 0L;
  }
  context->scratch = (jbyteArray)env->NewGlobalRef(scratch);
  env->DeleteLocalRef(scratch);
  context->env = env;
  context->thiz = thiz;

  uint8_t* ioBuffer = (uint8_t*)av_malloc(IO_BUFFER_SIZE);
  if (!ioBuffer) {
    releaseDemuxerContext(env, context);
    return 0L;
  }
  context->ioContext = avio_alloc_context(
      ioBuffer, IO_BUFFER_SIZE, /* write_flag= */ 0, context, readPacketCallback,
      /* write_packet= */ NULL, seekable ? seekCallback : NULL);
  if (!context->ioContext) {
    av_free(ioBuffer);
    releaseDemuxerContext(env, context);
    return 0L;
  }
  /*
   * 되감기가 되는지 못 박는다. 안 되는데 된다고 하면 해석기가 되감기를 시도했다가
   * 파일을 통째로 못 여는 것으로 끝나고, 되는데 안 된다고 하면 사용자가 구간을
   * 못 옮긴다.
   */
  context->ioContext->seekable = seekable ? AVIO_SEEKABLE_NORMAL : 0;

  context->formatContext = avformat_alloc_context();
  if (!context->formatContext) {
    releaseDemuxerContext(env, context);
    return 0L;
  }
  context->formatContext->pb = context->ioContext;
  context->formatContext->flags |= AVFMT_FLAG_CUSTOM_IO;

  int result = avformat_open_input(&context->formatContext, /* url= */ NULL,
                                   /* fmt= */ NULL, /* options= */ NULL);
  if (result < 0) {
    logError("avformat_open_input", result);
    context->env = NULL;
    context->thiz = NULL;
    releaseDemuxerContext(env, context);
    return 0L;
  }
  result = avformat_find_stream_info(context->formatContext, NULL);
  if (result < 0) {
    logError("avformat_find_stream_info", result);
    context->env = NULL;
    context->thiz = NULL;
    releaseDemuxerContext(env, context);
    return 0L;
  }
  context->packet = av_packet_alloc();
  if (!context->packet) {
    context->env = NULL;
    context->thiz = NULL;
    releaseDemuxerContext(env, context);
    return 0L;
  }
  context->env = NULL;
  context->thiz = NULL;
  return (jlong)context;
}

DEMUXER_FUNC(jint, ffmpegGetTrackCount, jlong jContext) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  return context ? (jint)context->formatContext->nb_streams : 0;
}

DEMUXER_FUNC(jlong, ffmpegGetDurationUs, jlong jContext) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context || context->formatContext->duration == AV_NOPTS_VALUE) {
    return -1;
  }
  // AV_TIME_BASE가 이미 마이크로초다.
  return (jlong)context->formatContext->duration;
}

/** 0=소리, 1=그림, -1=그 밖(자막·자료 등, 지금은 안 쓴다). */
DEMUXER_FUNC(jint, ffmpegGetTrackType, jlong jContext, jint index) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context || index < 0 || index >= (jint)context->formatContext->nb_streams) {
    return -1;
  }
  switch (context->formatContext->streams[index]->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      return 0;
    case AVMEDIA_TYPE_VIDEO:
      return 1;
    default:
      return -1;
  }
}

DEMUXER_FUNC(jstring, ffmpegGetTrackCodecName, jlong jContext, jint index) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context || index < 0 || index >= (jint)context->formatContext->nb_streams) {
    return NULL;
  }
  AVCodecID id = context->formatContext->streams[index]->codecpar->codec_id;
  /*
   * **설명표(descriptor)의 이름이 아니라 디코더의 이름을 돌려준다.** 둘은 자주 다르다 —
   * 예를 들어 MS-MPEG4 v3는 설명표에선 `msmpeg4v3`인데 디코더 이름은 `msmpeg4`이고,
   * ATRAC3+는 `atrac3p`와 `atrac3plus`로 갈린다. 자바 쪽은 이 이름을 그대로
   * `avcodec_find_decoder_by_name`에 도로 넘기므로 디코더 이름이어야 맞는다.
   */
  const AVCodec* codec = avcodec_find_decoder(id);
  if (codec && codec->name) {
    return env->NewStringUTF(codec->name);
  }
  const AVCodecDescriptor* descriptor = avcodec_descriptor_get(id);
  if (!descriptor || !descriptor->name) {
    return NULL;
  }
  return env->NewStringUTF(descriptor->name);
}

/** 0=너비 1=높이 2=표본율 3=채널수 4=block_align 5=초당비트 6=표본당비트. 없으면 -1. */
DEMUXER_FUNC(jint, ffmpegGetTrackValue, jlong jContext, jint index, jint what) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context || index < 0 || index >= (jint)context->formatContext->nb_streams) {
    return -1;
  }
  AVCodecParameters* par = context->formatContext->streams[index]->codecpar;
  switch (what) {
    case 0:
      return par->width;
    case 1:
      return par->height;
    case 2:
      return par->sample_rate;
    case 3:
      return par->ch_layout.nb_channels;
    case 4:
      return par->block_align;
    case 5:
      return (jint)par->bit_rate;
    case 6:
      return par->bits_per_coded_sample;
    default:
      return -1;
  }
}

DEMUXER_FUNC(jbyteArray, ffmpegGetTrackExtraData, jlong jContext, jint index) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context || index < 0 || index >= (jint)context->formatContext->nb_streams) {
    return NULL;
  }
  AVCodecParameters* par = context->formatContext->streams[index]->codecpar;
  if (!par->extradata || par->extradata_size <= 0) {
    return NULL;
  }
  jbyteArray result = env->NewByteArray(par->extradata_size);
  if (!result) {
    return NULL;
  }
  env->SetByteArrayRegion(result, 0, par->extradata_size,
                          (const jbyte*)par->extradata);
  return result;
}

/**
 * 다음 덩어리를 읽어 둔다. 돌려주는 값은 그 덩어리가 속한 트랙 번호,
 * 끝났으면 {@code -1}, 잘못됐으면 {@code -2}.
 */
DEMUXER_FUNC(jint, ffmpegReadPacket, jlong jContext) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context) {
    return DEMUXER_ERROR_OTHER;
  }
  context->env = env;
  context->thiz = thiz;
  av_packet_unref(context->packet);
  int result = av_read_frame(context->formatContext, context->packet);
  context->env = NULL;
  context->thiz = NULL;
  if (result == AVERROR_EOF) {
    return DEMUXER_RESULT_END_OF_INPUT;
  }
  if (result < 0) {
    logError("av_read_frame", result);
    return DEMUXER_ERROR_OTHER;
  }
  return context->packet->stream_index;
}

DEMUXER_FUNC(jint, ffmpegGetPacketSize, jlong jContext) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  return context ? context->packet->size : 0;
}

DEMUXER_FUNC(jlong, ffmpegGetPacketTimeUs, jlong jContext) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context) {
    return 0;
  }
  int64_t pts = context->packet->pts;
  if (pts == AV_NOPTS_VALUE) {
    pts = context->packet->dts;
  }
  if (pts == AV_NOPTS_VALUE) {
    return -1;
  }
  AVRational timeBase =
      context->formatContext->streams[context->packet->stream_index]->time_base;
  return (jlong)av_rescale_q(pts, timeBase, AVRational{1, 1000000});
}

DEMUXER_FUNC(jboolean, ffmpegIsPacketKeyFrame, jlong jContext) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  return context && (context->packet->flags & AV_PKT_FLAG_KEY) ? JNI_TRUE
                                                               : JNI_FALSE;
}

DEMUXER_FUNC(jint, ffmpegCopyPacketData, jlong jContext, jbyteArray target) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context || !target) {
    return DEMUXER_ERROR_OTHER;
  }
  int size = context->packet->size;
  if (env->GetArrayLength(target) < size) {
    return DEMUXER_ERROR_OTHER;
  }
  env->SetByteArrayRegion(target, 0, size, (const jbyte*)context->packet->data);
  return size;
}

/** 그 시각 언저리의 키프레임으로 옮긴다. 성공하면 0. */
DEMUXER_FUNC(jint, ffmpegSeekTo, jlong jContext, jlong timeUs) {
  DemuxerContext* context = (DemuxerContext*)jContext;
  if (!context) {
    return DEMUXER_ERROR_OTHER;
  }
  context->env = env;
  context->thiz = thiz;
  // AV_TIME_BASE가 마이크로초라 그대로 넘긴다. BACKWARD는 "그 앞의 키프레임으로"다 —
  // 뒤로 잡으면 목표 시각의 그림이 아직 안 나온 상태에서 시작한다.
  int result = av_seek_frame(context->formatContext, /* stream_index= */ -1,
                             (int64_t)timeUs, AVSEEK_FLAG_BACKWARD);
  context->env = NULL;
  context->thiz = NULL;
  if (result < 0) {
    logError("av_seek_frame", result);
    return DEMUXER_ERROR_OTHER;
  }
  return 0;
}

DEMUXER_FUNC(void, ffmpegClose, jlong jContext) {
  releaseDemuxerContext(env, (DemuxerContext*)jContext);
}
