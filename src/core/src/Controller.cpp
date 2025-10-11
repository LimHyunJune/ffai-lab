#include "Controller.h"
#include <chrono>

Controller::Controller(){}

Controller::~Controller()
{
    av_buffer_unref(&hw_device_ctx);
    delete input_handler;
    delete decoder_handler;
    delete filter_handler;
    delete encoder_handler;
    delete output_handler;
}

void Controller::start(string req, string target)
{
    controller_config.main_input_path = "~/ffai-lab/akina.mp4";
    // controller_config.main_input_path = "srt://0.0.0.0:8080?mode=listener&latency=600&rcvbuf=32768000";
    controller_config.backup_input_path = "srt://0.0.0.0:8081?mode=listener&latency=600&rcvbuf=32768000";

    controller_config.filter_config.filter_type = FilterType::NONE;
    controller_config.filter_config.use_gpu = false;

    controller_config.encoder_config.width = 3840;
    controller_config.encoder_config.height = 2160;
    controller_config.encoder_config.bit_rate = 20000000;
    controller_config.encoder_config.frame_rate = AVRational{60,1};
    controller_config.encoder_config.time_base = AVRational{1,90000};
    controller_config.encoder_config.use_gpu = false;
    controller_config.encoder_config.preset = "p4";
    controller_config.encoder_config.encoder_type = EncoderType::H265;

    controller_config.output_config.output_path = "~/ffai-lab/akina_filtered.mp4";
    // controller_config.output_config.output_path = "srt://" + target + "?mode=caller";
    // controller_config.output_config.output_path = "srt://abr:9999?mode=caller";
    // controller_config.output_config.output_path = "./output/out.mp4";
    controller_config.output_config.output_type = OutputType::SRT;
    create_streaming_pipeline();
}

bool Controller::init_hw_device()
{
    int ret =  av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0)
    {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR,"error init hw device",ret);
        return false;
    }

    hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if(!hw_frames_ctx) return false;

    AVHWFramesContext* fctx = (AVHWFramesContext*)hw_frames_ctx->data;
    fctx->format = AV_PIX_FMT_CUDA;
    fctx->sw_format = AV_PIX_FMT_NV12;
    fctx->width = 3840;
    fctx->height = 2160;
    fctx->initial_pool_size = 8;
    if((ret = av_hwframe_ctx_init(hw_frames_ctx)) < 0)
        return false;

        return true;
}

int Controller::interrupt_cb(void* opaque)
{
    auto* self = static_cast<Controller*>(opaque);
    const int64_t now = av_gettime_relative();
    return (now - self->last_io_ts_us.load()) > self->io_timeout_us ? 1 : 0;
}


bool Controller::init_input()
{
    input_handler = new InputHandler(controller_config.main_input_path, controller_config.backup_input_path);
    
    main_input_ctx = input_handler->get_main_input_context();
    if(!main_input_ctx)
        return false;
    
    main_video_stream = input_handler->get_main_video_stream();
    main_input_ctx->interrupt_callback.callback = &Controller::interrupt_cb;
    main_input_ctx->interrupt_callback.opaque = this;

    active_input_ctx = main_input_ctx;
    active_video_stream = main_video_stream;

    return true;
}

bool Controller::init_decoder()
{
    decoder_handler = new DecoderHandler();
    dec_ctx = decoder_handler->get_decoder_codec_context(active_video_stream, nullptr);

    // av_buffer_unref(&dec_ctx->hw_frames_ctx);
    // dec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    dec_ctx->thread_count = 1;

    if(!dec_ctx)
        return false;
    return true;
}

bool Controller::init_filter()
{
    controller_config.filter_config.dec_ctx = dec_ctx;
    controller_config.filter_config.video_stream = active_video_stream;
    // controller_config.filter_config.hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    filter_handler = new FilterHandler(controller_config.filter_config);
    
    pair<AVFilterContext*, AVFilterContext*> buffer = filter_handler->get_filter_context();
    buffersrc_ctx = buffer.first;
    buffersink_ctx = buffer.second;

    if(!buffersrc_ctx || !buffersink_ctx)
        return false;
    return true;
}

bool Controller::init_encoder()
{
    encoder_handler = new EncoderHandler(controller_config.encoder_config);
    enc_ctx = encoder_handler->get_encoder_codec_context(hw_device_ctx);

    enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
    av_buffer_unref(&enc_ctx->hw_frames_ctx);
    enc_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    enc_ctx->max_b_frames = 0;

    if(!enc_ctx)
        return false;
    return true;
}

bool Controller::init_output()
{
    controller_config.output_config.enc_ctx = enc_ctx;
    output_handler = new OutputHandler(controller_config.output_config);
    out_fmt_ctx = output_handler->get_output_format_context();
    output_stream = output_handler->get_output_stream();

    if(!out_fmt_ctx)
        return false;
    return true;
}

bool Controller::initialize() {
    return init_hw_device() && init_input() && init_decoder() && init_filter() && init_encoder() && init_output();
}

void Controller::run_input_thread() {
    input_thread = std::thread([this]() {
        BOOST_LOG(debug) << "input thread start";
        while (flag) {
            BOOST_LOG(debug) << "input loop start";
            AVPacket* pkt = av_packet_alloc();
            int ret = av_read_frame(active_input_ctx, pkt);
            if(ret >= 0)
            {
                if(active_input_ctx->streams[pkt->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) 
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    av_packet_free(&pkt);
                    continue;
                }
                AVPacket* copy = av_packet_alloc();
                av_packet_ref(copy, pkt);
                
                input_queue.push(copy);                
                av_packet_free(&pkt);
            }
            else
            {
                Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "input read error.", ret);
                av_packet_free(&pkt);
            }
            BOOST_LOG(debug) << "input loop end";
        }
        BOOST_LOG(debug) << "input loop exit";
        input_queue.stop();
    });
}

void Controller::run_decoder_thread() {
    decoder_thread = std::thread([this]() {
        AVPacket* pkt = nullptr;
        while (flag && input_queue.pop(pkt)) {
            BOOST_LOG(debug) << "decode loop start";
            int idx = pkt->stream_index;
            if(!buffersrc_ctx)
            {
                av_packet_free(&pkt);
                continue;
            }
            int ret = avcodec_send_packet(dec_ctx, pkt);
            if (ret < 0) {
                Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "decoder send error", ret);
                av_packet_free(&pkt);
                continue;
            }        

            while (true) {
                AVFrame* frame = av_frame_alloc();
                int ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret >= 0) {
                    // frame->pts = pkt->pts;
                    // frame->pkt_dts = pkt->dts;
                    // frame->duration = pkt->duration;
                    // frame->time_base = pkt->time_base;
                    decoder_queue.push(frame);
                }
                else
                {
                    Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "decoder receive error.", ret);
                    av_packet_free(&pkt);
                    av_frame_free(&frame);
                    break;
                }
            }
            av_packet_free(&pkt);
        }
        BOOST_LOG(debug) << "decode loop exit";
        decoder_queue.stop();
    });
}

static inline bool is_hw_frame_fmt(int fmt) { return fmt == AV_PIX_FMT_CUDA; }

static AVPixelFormat pick_swfmt_from_dec(AVCodecContext* dec) {
    if (dec && dec->hw_frames_ctx) {
        auto* fctx = (AVHWFramesContext*)dec->hw_frames_ctx->data;
        if (fctx && fctx->sw_format != AV_PIX_FMT_NONE) return (AVPixelFormat)fctx->sw_format;
    }
    return AV_PIX_FMT_NV12;
}

void Controller::run_filter_thread() {
    filter_thread = std::thread([this]() {
        AVFrame* frame = nullptr;
        while (flag && decoder_queue.pop(frame)) {
            BOOST_LOG(debug) << "filter loop start";
            // int64_t duration = frame->duration;

            // if (is_hw_frame_fmt(frame->format)) {
            //     AVPixelFormat swfmt = pick_swfmt_from_dec(dec_ctxs[idx]);
            //     AVFrame* sw = av_frame_alloc();
            //     sw->format = swfmt;
            //     sw->width  = frame->width;
            //     sw->height = frame->height;
            //     int tr = av_hwframe_transfer_data(sw, frame, 0);
            //     if (tr < 0) { av_frame_free(&sw); av_frame_free(&frame); continue; }
            //     sw->pts = frame->pts; sw->pkt_dts = frame->pkt_dts; sw->duration = frame->duration;
            //     sw->sample_aspect_ratio = frame->sample_aspect_ratio;
            //     sw->opaque = frame->opaque;
            //     av_frame_free(&frame);
            //     frame = sw;
            // }

            int ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, 0);
            av_frame_free(&frame);

            while (true) {
                AVFrame* filt_frame = av_frame_alloc();
                ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                if (ret >= 0) {
                    // filt_frame->pts = filt_frame->pkt_dts;
                    // filt_frame->duration = duration;
                    filter_queue.push(filt_frame);
                }
                else {
                    Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "filter buffer sink error.", ret);
                    av_frame_free(&filt_frame);
                    break;
                }
            }
            BOOST_LOG(debug) << "filter loop end!";
        }
        BOOST_LOG(debug) << "filter loop exit";
        filter_queue.stop();
    });
}

void Controller::run_encoder_thread() {
    encoder_thread = std::thread([this]() {
        AVFrame* frame = nullptr;
        TSQueue<AVFrame*>& src_queue = controller_config.filter_config.filter_type != FilterType::NONE ? filter_queue : decoder_queue;
        while (flag && src_queue.pop(frame)) {
            BOOST_LOG(debug) << "encode loop start";
            avcodec_send_frame(enc_ctx, frame);
            while (true) {
                AVPacket* pkt = av_packet_alloc();
                int ret = avcodec_receive_packet(enc_ctx, pkt);
                if (ret >= 0) {
                    AVPacket* copy = av_packet_alloc();
                    av_packet_ref(copy, pkt);
                    // copy->pts = frame->pts;
                    // copy->dts = frame->pkt_dts;
                    // copy->duration = frame->duration;
                    // copy->time_base = frame->time_base;
                    encoder_queue.push(copy);
                    av_packet_unref(pkt);
                }
                else
                {
                    av_packet_unref(pkt);
                    break;
                }
            }
            av_frame_free(&frame);
            BOOST_LOG(debug) << "encode loop end";
        }
        BOOST_LOG(debug) << "encode loop exit";
        encoder_queue.stop();
    });
}

void Controller::run_output_thread() {
    output_thread = std::thread([this]() {
        AVPacket* pkt = nullptr;
        while (flag && encoder_queue.pop(pkt)) {
            BOOST_LOG(debug) << "output loop start";
            pkt->stream_index = output_stream->index;
            pkt->pts = av_rescale_q(pkt->pts, enc_ctx->time_base, output_stream->time_base);
            pkt->dts = av_rescale_q(pkt->dts, enc_ctx->time_base, output_stream->time_base);
            pkt->duration = av_rescale_q(pkt->duration, enc_ctx->time_base, output_stream->time_base);

            BOOST_LOG(debug) << "PKT PTS debug : " << pkt->pts << " " << pkt->dts << " " << pkt->duration << " " << pkt->time_base.num << "/" << pkt->time_base.den << " ";
            av_interleaved_write_frame(out_fmt_ctx, pkt);
            // av_write_frame(out_fmt_ctx, pkt);
            av_packet_free(&pkt);
            BOOST_LOG(debug) << "output loop end";
        }
        BOOST_LOG(debug) << "output loop exit";
        av_write_trailer(out_fmt_ctx);
    });
}

void Controller::create_streaming_pipeline() {
    if (!initialize()) return;
    flag = true;

    run_input_thread();
    run_decoder_thread();
    run_filter_thread();
    run_encoder_thread();
    run_output_thread();

    input_thread.join();
    BOOST_LOG(debug) << "input_thread join finish";
    decoder_thread.join();
    BOOST_LOG(debug) << "decoder_thread join finish";
    filter_thread.join();
    BOOST_LOG(debug) << "filter_thread join finish";
    encoder_thread.join();
    BOOST_LOG(debug) << "encoder_thread join finish";
    output_thread.join();
    BOOST_LOG(debug) << "output_thread join finish";
}