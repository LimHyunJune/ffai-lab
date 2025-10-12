#include "EncoderHandler.h"
#include "Logger.h"
#include <cmath>

extern "C" {
  #include <libavutil/opt.h>
  #include <libavutil/hwcontext.h>
}

EncoderHandler::EncoderHandler(EncoderConfig encoder_config) {
  this->encoder_config = encoder_config;
}

EncoderHandler::~EncoderHandler() {
  avcodec_free_context(&enc_ctx);
}

const AVCodec* EncoderHandler::get_encoder_codec() {
  switch (encoder_config.encoder_type) {
    case EncoderType::H264:
      if (encoder_config.use_gpu) return avcodec_find_encoder_by_name("h264_nvenc");
      return avcodec_find_encoder(AV_CODEC_ID_H264);
    case EncoderType::H265:
      if (encoder_config.use_gpu) return avcodec_find_encoder_by_name("hevc_nvenc");
      return avcodec_find_encoder(AV_CODEC_ID_HEVC);
    case EncoderType::AV1:
      if (encoder_config.use_gpu) return avcodec_find_encoder_by_name("av1_nvenc");
      return avcodec_find_encoder(AV_CODEC_ID_AV1);
    default:
      break;
  }
  return nullptr;
}

AVBufferRef* EncoderHandler::get_hw_frame_ref(AVBufferRef* hw_device_ctx) {
  AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
  if (!hw_frames_ref) {
    Logger::get_instance().print_log(AV_LOG_ERROR, "Failed to allocate hwframe context.");
    return nullptr;
  }

  AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
  frames_ctx->format = AV_PIX_FMT_CUDA;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = enc_ctx->width;
  frames_ctx->height = enc_ctx->height;
  frames_ctx->initial_pool_size = 20;

  int ret = av_hwframe_ctx_init(hw_frames_ref);
  if (ret < 0) {
    Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "Failed to init hw frames context", ret);
    av_buffer_unref(&hw_frames_ref);
    return nullptr;
  }
  return hw_frames_ref;
}

AVCodecContext* EncoderHandler::get_encoder_codec_context(AVBufferRef* hw_device_ctx) {
  const AVCodec* codec = get_encoder_codec();
  enc_ctx = avcodec_alloc_context3(codec);
  if (!enc_ctx) return nullptr;

  enc_ctx->height    = encoder_config.height;
  enc_ctx->width     = encoder_config.width;
  enc_ctx->time_base = encoder_config.time_base;  // {1/fps}
  enc_ctx->framerate = encoder_config.frame_rate; // e.g. 30/1
  enc_ctx->bit_rate  = encoder_config.bit_rate;
  enc_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;

  av_opt_set(enc_ctx->priv_data, "preset", encoder_config.preset.c_str(), 0);

  // 공통 인코딩 파라미터
  if (encoder_config.encoder_type == EncoderType::AV1) {
    enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  enc_ctx->rc_max_rate = enc_ctx->bit_rate;
  enc_ctx->rc_min_rate = enc_ctx->bit_rate;
  enc_ctx->rc_buffer_size = enc_ctx->bit_rate;

  enc_ctx->gop_size   = std::lround((double)enc_ctx->framerate.num / enc_ctx->framerate.den);
  enc_ctx->keyint_min = enc_ctx->gop_size;
  enc_ctx->max_b_frames = 0;
  enc_ctx->qcompress  = 0.6;

  // x265/HEVC (libx265) 또는 nvenc 설정
  if (!encoder_config.use_gpu) {
    // libx265 계열: TS 호환을 위해 AUD/반복헤더/closed-gop/b-frames 0
    av_opt_set(enc_ctx->priv_data, "x265-params",
               "scenecut=0:open-gop=0:bframes=0:aud=1:repeat-headers=1", 0);
    // 일부 빌드에선 별도 옵션도 허용되지만 x265-params로 충분
    av_opt_set_int(enc_ctx->priv_data, "forced-idr", 1, 0);
  } else {
    // NVENC 계열: 유사 옵션
    av_opt_set_int(enc_ctx->priv_data, "gop", enc_ctx->gop_size, 0);
    av_opt_set_int(enc_ctx->priv_data, "bf", 0, 0);
    av_opt_set_int(enc_ctx->priv_data, "no-scenecut", 1, 0);
    av_opt_set_int(enc_ctx->priv_data, "strict_gop", 1, 0);
    // AUD / 헤더 반복
    av_opt_set_int(enc_ctx->priv_data, "aud", 1, 0);
    av_opt_set_int(enc_ctx->priv_data, "repeat-headers", 1, 0);

    enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
    enc_ctx->hw_frames_ctx = av_buffer_ref(get_hw_frame_ref(hw_device_ctx));
    enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  }

  AVDictionary* options = nullptr;
  int ret = avcodec_open2(enc_ctx, codec, &options);
  if (ret < 0) {
    Logger::get_instance().print_log_with_reason(AV_LOG_ERROR,"encoder avcodec_open2 failed.", ret);
    av_dict_free(&options);
    return nullptr;
  }
  av_dict_free(&options);
  return enc_ctx;
}
