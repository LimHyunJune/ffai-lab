#include "Controller.h"
#include "Logger.h"
#include <chrono>

extern "C" {
  #include <libswscale/swscale.h>
  #include <libavutil/avutil.h>
  #include <libavutil/frame.h>
}

static inline bool valid_rate(AVRational r) { return r.num > 0 && r.den > 0; }

Controller::Controller() {}

Controller::~Controller() {
  // 파일 close/트레일러는 start() 종료 전에 보장
  av_buffer_unref(&hw_device_ctx);
  av_buffer_unref(&hw_frames_ctx);
  delete input_handler;
  delete decoder_handler;
  delete encoder_handler;
  delete output_handler;
  if (pseg_) pseg_->close();
}

/* 엔트리 */
void Controller::start() {
  controller_config.main_input_path   = "/home/ubuntu/ffai-lab/assets/akina.mp4";
  controller_config.backup_input_path = "srt://0.0.0.0:8081?mode=listener&latency=600&rcvbuf=32768000";

  controller_config.filter_config.filter_type = FilterType::NONE;
  controller_config.filter_config.use_gpu     = false;

  // 초기값(디코더와 동기화 예정)
  controller_config.encoder_config.width       = 1440;
  controller_config.encoder_config.height      = 1080;
  controller_config.encoder_config.bit_rate    = 20000000;
  controller_config.encoder_config.frame_rate  = AVRational{30, 1};
  controller_config.encoder_config.time_base   = av_inv_q(controller_config.encoder_config.frame_rate);
  controller_config.encoder_config.use_gpu     = false;
  controller_config.encoder_config.preset      = "fast";
  controller_config.encoder_config.encoder_type = EncoderType::H264;

  // TS 컨테이너 사용 (확장자 .ts 권장)
  controller_config.output_config.output_path  = "/home/ubuntu/ffai-lab/assets/akina_filtered.ts";

  create_streaming_pipeline();
}

/* (옵션) GPU 초기화 — CPU면 사용 안 함 */
bool Controller::init_hw_device() {
  int ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
  if (ret < 0) {
    Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "error init hw device", ret);
    return false;
  }
  hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
  if (!hw_frames_ctx) return false;

  AVHWFramesContext* fctx = (AVHWFramesContext*)hw_frames_ctx->data;
  fctx->format = AV_PIX_FMT_CUDA;
  fctx->sw_format = AV_PIX_FMT_NV12;
  fctx->width  = controller_config.encoder_config.width;
  fctx->height = controller_config.encoder_config.height;
  fctx->initial_pool_size = 8;

  if ((ret = av_hwframe_ctx_init(hw_frames_ctx)) < 0) return false;
  return true;
}

int Controller::interrupt_cb(void* opaque) {
  auto* self = static_cast<Controller*>(opaque);
  const int64_t now = av_gettime_relative();
  return (now - self->last_io_ts_us.load()) > self->io_timeout_us ? 1 : 0;
}

/* 입력 초기화 */
bool Controller::init_input() {
  input_handler = new InputHandler(controller_config.main_input_path, controller_config.backup_input_path);

  main_input_ctx = input_handler->get_main_input_context();
  if (!main_input_ctx) return false;

  main_video_stream = input_handler->get_main_video_stream();
  main_input_ctx->interrupt_callback.callback = &Controller::interrupt_cb;
  main_input_ctx->interrupt_callback.opaque   = this;

  active_input_ctx    = main_input_ctx;
  active_video_stream = main_video_stream;
  return true;
}

/* 디코더 초기화 */
bool Controller::init_decoder() {
  decoder_handler = new DecoderHandler();
  dec_ctx = decoder_handler->get_decoder_codec_context(active_video_stream, nullptr);
  if (!dec_ctx) return false;

  dec_ctx->thread_count = 1;
  return true;
}

/* personseg 초기화 (현재 사용 안 하거나 pass-through 가능) */
bool Controller::init_filter() {
  pseg_ = std::make_unique<PersonSegProcessor>();

  BOOST_LOG(debug) << "personseg model path = " << controller_config.filter_config.model_path;

  use_personseg_ = pseg_->init(
    controller_config.filter_config.model_path.c_str(),
    /*in_w*/ 192, /*in_h*/ 192,
    /*thr*/  0.5f,
    /*threads*/ 1,
    /*src_w*/ dec_ctx->width,
    /*src_h*/ dec_ctx->height,
    /*src_fmt*/ (AVPixelFormat)dec_ctx->pix_fmt
  );

  if (!use_personseg_) {
    BOOST_LOG(debug) << "PersonSegProcessor init failed; bypassing person-seg";
    pseg_.reset();
  }
  return true;
}

/* 인코더 초기화 (디코더와 동기화) */
bool Controller::init_encoder() {
  controller_config.encoder_config.width  = dec_ctx->width;
  controller_config.encoder_config.height = dec_ctx->height;

  AVRational src_fps = active_video_stream->r_frame_rate;
  if (!valid_rate(src_fps)) src_fps = AVRational{30,1};

  controller_config.encoder_config.frame_rate = src_fps;
  controller_config.encoder_config.time_base  = av_inv_q(controller_config.encoder_config.frame_rate);

  if (controller_config.encoder_config.use_gpu) {
    if (!hw_device_ctx && !init_hw_device()) {
      Logger::get_instance().print_log(AV_LOG_ERROR, "HW device init failed; fallback to CPU encoding");
      controller_config.encoder_config.use_gpu = false;
    }
  }

  encoder_handler = new EncoderHandler(controller_config.encoder_config);
  enc_ctx = encoder_handler->get_encoder_codec_context(
    controller_config.encoder_config.use_gpu ? hw_device_ctx : nullptr
  );
  if (!enc_ctx) return false;

  return true;
}

/* 출력 초기화 */
bool Controller::init_output() {
  controller_config.output_config.enc_ctx = enc_ctx;
  output_handler = new OutputHandler(controller_config.output_config);
  out_fmt_ctx    = output_handler->get_output_format_context();
  output_stream  = output_handler->get_output_stream();
  if (!out_fmt_ctx || !output_stream) return false;

  // 스트림 time_base/avg_frame_rate는 OutputHandler에서 enc_ctx 기준으로 설정됨
  return true;
}

/* 전체 초기화 순서 */
bool Controller::initialize() {
  if (!init_input())   return false;
  if (!init_decoder()) return false;
  if (!init_filter())  return false;
  if (!init_encoder()) return false;
  if (!init_output())  return false;
  return true;
}

/* 입력 스레드 */
void Controller::run_input_thread() {
  input_thread = std::thread([this]() {
    BOOST_LOG(debug) << "input thread start";
    while (flag) {
      AVPacket* pkt = av_packet_alloc();
      int ret = av_read_frame(active_input_ctx, pkt);
      if (ret >= 0) {
        if (active_input_ctx->streams[pkt->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          av_packet_free(&pkt);
          continue;
        }
        AVPacket* copy = av_packet_alloc();
        av_packet_ref(copy, pkt);
        input_queue.push(copy);
        av_packet_free(&pkt);
      } else if (ret == AVERROR_EOF) {
        av_packet_free(&pkt);
        break;
      } else {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "input read error.", ret);
        av_packet_free(&pkt);
        break;
      }
    }
    input_queue.stop();
  });
}

/* 디코더 스레드 */
void Controller::run_decoder_thread() {
  decoder_thread = std::thread([this]() {
    AVPacket* pkt = nullptr;
    while (flag && input_queue.pop(pkt)) {
      int ret = avcodec_send_packet(dec_ctx, pkt);
      av_packet_free(&pkt);
      if (ret < 0) {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "decoder send error", ret);
        continue;
      }
      while (true) {
        AVFrame* frame = av_frame_alloc();
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret >= 0) {
          if (frame->pts == AV_NOPTS_VALUE) {
            int64_t best = frame->best_effort_timestamp;
            if (best != AV_NOPTS_VALUE) frame->pts = best;
          }
          decoder_queue.push(frame);
        } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          av_frame_free(&frame);
          break;
        } else {
          Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "decoder receive error", ret);
          av_frame_free(&frame);
          break;
        }
      }
    }

    // 디코더 플러시
    avcodec_send_packet(dec_ctx, nullptr);
    while (true) {
      AVFrame* frame = av_frame_alloc();
      int ret = avcodec_receive_frame(dec_ctx, frame);
      if (ret >= 0) {
        if (frame->pts == AV_NOPTS_VALUE) {
          int64_t best = frame->best_effort_timestamp;
          if (best != AV_NOPTS_VALUE) frame->pts = best;
        }
        decoder_queue.push(frame);
      } else {
        av_frame_free(&frame);
        break;
      }
    }

    decoder_queue.stop();
  });
}

/* personseg 스레드 (현재 pass-through 처리 포함) */
void Controller::run_filter_thread() {
  filter_thread = std::thread([this]() {
    AVFrame* frame = nullptr;
    while (flag && decoder_queue.pop(frame)) {
      // personseg 사용 시: out 프레임에 그려서 전달
      if (pseg_) {
        AVFrame* out = av_frame_alloc();
        out->format = AV_PIX_FMT_YUV420P;
        out->width  = frame->width;
        out->height = frame->height;
        if (av_frame_get_buffer(out, 32) < 0) {
          av_frame_free(&out);
          // 실패 시 원본 그대로 패스스루
          filter_queue.push(frame);
          continue;
        }
        av_frame_copy_props(out, frame);

        int pr = pseg_->process(frame, out);
        if (pr == 0) {
          av_frame_free(&frame);
          filter_queue.push(out);
        } else {
          Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "personseg process failed", pr);
          av_frame_free(&out);
          // 실패 시 원본 그대로 전달 (소유권 유지)
          filter_queue.push(frame);
        }
      } else {
        // personseg 미사용: 패스스루
        filter_queue.push(frame);
      }
    }
    filter_queue.stop();
  });
}

/* 인코더 스레드 — 입력 tb로 누적 PTS 생성 → enc_tb로 1회 변환 */
void Controller::run_encoder_thread() {
  encoder_thread = std::thread([this]() {
    AVFrame* frame = nullptr;

    TSQueue<AVFrame*>& src_queue = use_personseg_ ? filter_queue : decoder_queue;

    const AVRational in_tb  = active_video_stream->time_base; // 보통 {1/90000}
    const AVRational enc_tb = enc_ctx->time_base;             // {1/fps}
    AVRational src_fps = active_video_stream->r_frame_rate;
    if (!valid_rate(src_fps)) src_fps = AVRational{30,1};

    const int64_t in_frame_dur = av_rescale_q(1, av_inv_q(src_fps), in_tb);
    int64_t next_in_pts = AV_NOPTS_VALUE; // 입력 tb 기준 누적 pts

    while (flag && src_queue.pop(frame)) {
      frame->pict_type = AV_PICTURE_TYPE_NONE;

      // 입력 tb 기반의 일관된 pts 만들기
      int64_t in_pts = frame->pts;
      if (in_pts == AV_NOPTS_VALUE) {
        if (next_in_pts == AV_NOPTS_VALUE) next_in_pts = 0;
        in_pts = next_in_pts;
        next_in_pts += in_frame_dur;
      } else {
        if (next_in_pts == AV_NOPTS_VALUE) next_in_pts = in_pts + in_frame_dur;
        else next_in_pts = std::max(next_in_pts, in_pts + in_frame_dur);
      }

      // enc_tb로 1회 변환
      frame->pts = av_rescale_q(in_pts, in_tb, enc_tb);

      // 필요 시 YUV420P로 변환
      AVFrame* to_send = frame;
      if (enc_ctx->pix_fmt == AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUV420P) {
        AVFrame* yuv = av_frame_alloc();
        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width  = frame->width;
        yuv->height = frame->height;
        if (av_frame_get_buffer(yuv, 32) == 0) {
          SwsContext* sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                           yuv->width, yuv->height, AV_PIX_FMT_YUV420P,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
          if (sws) {
            const uint8_t* src_data[4] = { frame->data[0], frame->data[1], frame->data[2], nullptr };
            int            src_ls[4]   = { frame->linesize[0], frame->linesize[1], frame->linesize[2], 0 };
            uint8_t*       dst_data[4] = { yuv->data[0], yuv->data[1], yuv->data[2], nullptr };
            int            dst_ls[4]   = { yuv->linesize[0], yuv->linesize[1], yuv->linesize[2], 0 };
            sws_scale(sws, src_data, src_ls, 0, frame->height, dst_data, dst_ls);
            sws_freeContext(sws);
            av_frame_copy_props(yuv, frame);
            yuv->pts = frame->pts; // enc_tb 기준
            av_frame_free(&frame);
            to_send = yuv;
          } else {
            av_frame_free(&yuv);
          }
        } else {
          av_frame_free(&yuv);
        }
      }

      int ret = avcodec_send_frame(enc_ctx, to_send);
      av_frame_free(&to_send);
      if (ret < 0) {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "avcodec_send_frame failed", ret);
        continue;
      }

      // 패킷 수거
      while (true) {
        AVPacket* pkt = av_packet_alloc();
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret >= 0) {
          if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts; // DTS 보정
          if (pkt->duration == 0) pkt->duration = 1;           // enc_tb 기준 1 프레임
          AVPacket* copy = av_packet_alloc();
          av_packet_ref(copy, pkt);
          encoder_queue.push(copy);
          av_packet_unref(pkt);
        } else {
          av_packet_unref(pkt);
          break;
        }
      }
    }

    // 인코더 플러시
    avcodec_send_frame(enc_ctx, nullptr);
    while (true) {
      AVPacket* pkt = av_packet_alloc();
      int ret = avcodec_receive_packet(enc_ctx, pkt);
      if (ret >= 0) {
        if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;
        if (pkt->duration == 0) pkt->duration = 1;
        AVPacket* copy = av_packet_alloc();
        av_packet_ref(copy, pkt);
        encoder_queue.push(copy);
        av_packet_unref(pkt);
      } else {
        av_packet_unref(pkt);
        break;
      }
    }

    encoder_queue.stop();
  });
}

/* 출력 스레드 — enc_tb → mux_tb(스트림 tb)로 1회 리스케일 */
void Controller::run_output_thread() {
  output_thread = std::thread([this]() {
    AVPacket* pkt = nullptr;

    const AVRational enc_tb = enc_ctx->time_base;
    const AVRational mux_tb = output_stream->time_base; // 강제 1/90000 금지, 스트림 tb 사용

    while (flag && encoder_queue.pop(pkt)) {
      pkt->stream_index = output_stream->index;

      if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;
      if (pkt->duration <= 0) pkt->duration = 1; // enc_tb 기준 1 프레임

      // 1회 리스케일
      pkt->pts      = av_rescale_q(pkt->pts,      enc_tb, mux_tb);
      pkt->dts      = av_rescale_q(pkt->dts,      enc_tb, mux_tb);
      pkt->duration = av_rescale_q(pkt->duration, enc_tb, mux_tb);

      av_interleaved_write_frame(out_fmt_ctx, pkt);
      av_packet_free(&pkt);
    }
    // 트레일러는 메인 스레드에서 호출
  });
}

/* 파이프라인 구동 */
void Controller::create_streaming_pipeline() {
  if (!initialize()) return;
  flag = true;

  run_input_thread();
  run_decoder_thread();

  if (use_personseg_) {
    run_filter_thread();
  }

  run_encoder_thread();
  run_output_thread();

  input_thread.join();
  decoder_thread.join();
  if (use_personseg_) filter_thread.join();
  encoder_thread.join();
  output_thread.join();

  // 모든 쓰기 종료 후 트레일러 → flush → close
  av_write_trailer(out_fmt_ctx);
  if (out_fmt_ctx && out_fmt_ctx->pb) {
    avio_flush(out_fmt_ctx->pb);
    avio_closep(&out_fmt_ctx->pb);
  }
}
