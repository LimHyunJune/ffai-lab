#include "EncoderHandler.h"
#include "Logger.h"

EncoderHandler::EncoderHandler(EncoderConfig encoder_config)
{
    this->encoder_config = encoder_config;
    gpu_scalers.resize(encoder_config.params.size());
    sw_scalers.resize(encoder_config.params.size());
}

EncoderHandler::~EncoderHandler()
{
    for(auto &kv : enc_ctxs)
        avcodec_free_context(&kv.second);

    for (auto& g : gpu_scalers) 
        g.close();
    for (auto& s : sw_scalers) 
        s.close();
}

const AVCodec* EncoderHandler::get_encoder_codec()
{
    switch (encoder_config.encoder_type)
    {
        case EncoderType::H264:
            if (encoder_config.use_gpu)
            {
                return avcodec_find_encoder_by_name("h264_nvenc");
            }
            else
            {
                return avcodec_find_encoder(AVCodecID::AV_CODEC_ID_H264);
            }
        case EncoderType::H265:
            if (encoder_config.use_gpu)
            {
                return avcodec_find_encoder_by_name("hevc_nvenc");
            }
            else
            {
                return avcodec_find_encoder(AVCodecID::AV_CODEC_ID_HEVC);
            }
        case EncoderType::AV1:
            if (encoder_config.use_gpu)
            {
                return avcodec_find_encoder_by_name("av1_nvenc");
            }
            else
            {
                return avcodec_find_encoder(AVCodecID::AV_CODEC_ID_AV1);
            }
        default:
            break;
    }
    return nullptr;
}

AVBufferRef* EncoderHandler::get_hw_frame_ref(AVBufferRef* hw_device_ctx, int idx)
{
    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ref) {
        Logger::get_instance().print_log(AV_LOG_ERROR, "Failed to allocate hwframe context.");
        return nullptr;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = encoder_config.params[idx].width;
    frames_ctx->height = encoder_config.params[idx].height;
    frames_ctx->initial_pool_size = 20;

    int ret = av_hwframe_ctx_init(hw_frames_ref);
    if (ret < 0)
    {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "Failed to init hw frames context",ret);
        return nullptr;
    }
    return hw_frames_ref;
}

int EncoderHandler::GpuScaler::init(AVBufferRef* hw_frames_ctx, int iw, int ih, int ow, int oh) {
    if (!hw_frames_ctx) return AVERROR(EINVAL);
    close();

    int ret = 0;
    graph = avfilter_graph_alloc();
    if (!graph) return AVERROR(ENOMEM);

    const AVFilter* buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter* scale_cuda = avfilter_get_by_name("scale_cuda");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    if (!buffersrc || !scale_cuda || !buffersink) return AVERROR_FILTER_NOT_FOUND;

    // 1) buffer에 args로 pix_fmt 등 명시
    src = avfilter_graph_alloc_filter(graph, buffersrc, "src");
    if (!src) return AVERROR(ENOMEM);

    // 2) hw_frames_ctx는 parameters로 붙임 (필수)
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    par->format = AV_PIX_FMT_CUDA;
    par->hw_frames_ctx = hw_frames_ctx;
    par->time_base = AVRational{1, 90000};
    par->width  = iw;
    par->height = ih;
    ret = av_buffersrc_parameters_set(src, par);
    av_free(par);
    if (ret < 0) return ret;

    if ((ret = avfilter_init_str(src, nullptr)) < 0) return ret;

    // scale_cuda
    char sargs[64];
    snprintf(sargs, sizeof(sargs), "w=%d:h=%d", ow, oh);
    scale = avfilter_graph_alloc_filter(graph, scale_cuda, "scale");
    if (!scale) return AVERROR(ENOMEM);
    if ((ret = avfilter_init_str(scale, sargs)) < 0) return ret;

    // sink
    sink = avfilter_graph_alloc_filter(graph, buffersink, "sink");
    if (!sink) return AVERROR(ENOMEM);
    if ((ret = avfilter_init_str(sink, nullptr)) < 0) return ret;

    if ((ret = avfilter_link(src,   0, scale, 0)) < 0) return ret;
    if ((ret = avfilter_link(scale, 0, sink,  0)) < 0) return ret;
    if ((ret = avfilter_graph_config(graph, nullptr)) < 0) return ret;

    in_w=iw; in_h=ih; out_w=ow; out_h=oh;
    ready = true;
    return 0;
}

int EncoderHandler::GpuScaler::process(AVFrame* in, AVFrame** out) 
{
    *out = nullptr;
    if (!ready) 
        return AVERROR(EINVAL);
    int ret = av_buffersrc_add_frame_flags(src, in, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) 
        return ret;
    AVFrame* hw_out = av_frame_alloc();
    if (!hw_out)
        return AVERROR(ENOMEM);
    ret = av_buffersink_get_frame(sink, hw_out);
    if (ret < 0) { av_frame_free(&hw_out); return ret; }
    if (hw_out->pts == AV_NOPTS_VALUE) 
        hw_out->pts = in->pts;
    *out = hw_out; // AV_PIX_FMT_CUDA
    return 0;
}

void EncoderHandler::GpuScaler::close() {
    if (graph) 
        avfilter_graph_free(&graph);
    graph=nullptr; 
    src=scale=sink=nullptr;
    ready=false;
}

int EncoderHandler::SwScaler::ensure_ctx(int iw, int ih, AVPixelFormat ifmt,
    int ow, int oh, AVPixelFormat ofmt) 
{
    if (sws && iw==in_w && ih==in_h && ow==out_w && oh==out_h && ifmt==in_fmt && ofmt==out_fmt)
        return 0;
    sws = sws_getCachedContext(sws, iw, ih, ifmt, ow, oh, ofmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws) 
        return AVERROR(ENOMEM);
    in_w=iw; in_h=ih; out_w=ow; out_h=oh; in_fmt=ifmt; out_fmt=ofmt;
    return 0;
}

int EncoderHandler::SwScaler::process(AVFrame* in, int ow, int oh, AVPixelFormat ofmt, AVFrame** out) 
{
    *out = nullptr;
    if (!in) 
        return AVERROR(EINVAL);

    if(ofmt == AV_PIX_FMT_NONE)
        ofmt = AV_PIX_FMT_NV12;

    int er = ensure_ctx(in->width, in->height, (AVPixelFormat)in->format, ow, oh, ofmt);
    if (er < 0) 
        return er;

    AVFrame* dst = av_frame_alloc();
    if (!dst) 
        return AVERROR(ENOMEM);
    dst->format = ofmt;
    dst->width  = ow;
    dst->height = oh;
    if ((er = av_frame_get_buffer(dst, 32)) < 0) { av_frame_free(&dst); return er; }

    sws_scale(sws, in->data, in->linesize, 0, in->height, dst->data, dst->linesize);
    dst->pts = in->pts;
    *out = dst; // SW frame
    return 0;
}

void EncoderHandler::SwScaler::close() 
{
    if (sws) { sws_freeContext(sws); sws=nullptr; }
}

int EncoderHandler::scale_frame(int idx, AVFrame* in, AVFrame** out) 
{
    if (!in || !out) return AVERROR(EINVAL);
    *out = nullptr;

    const bool input_is_cuda = (in->format == AV_PIX_FMT_CUDA);
    const int  ow = encoder_config.params[idx].width;
    const int  oh = encoder_config.params[idx].height;

    if (input_is_cuda) {
        // 인코더 컨텍스트에 설정된 hw_frames_ctx를 사용
        AVCodecContext* enc = enc_ctxs.at(idx);
        AVBufferRef* hwfc = enc ? enc->hw_frames_ctx : nullptr;
        if (!hwfc) {
            // 인코더가 GPU 모드인데 hw_frames_ctx가 없다면 스케일 불가
            return AVERROR(EINVAL);
        }

        // 해상도 변화 시 그래프 재구성
        if (!gpu_scalers[idx].ready ||
            gpu_scalers[idx].in_w  != in->width  ||
            gpu_scalers[idx].in_h  != in->height ||
            gpu_scalers[idx].out_w != ow         ||
            gpu_scalers[idx].out_h != oh) {
            int ir = gpu_scalers[idx].init(hwfc, in->width, in->height, ow, oh);
            if (ir < 0) return ir;
        }
        return gpu_scalers[idx].process(in, out); // out: AV_PIX_FMT_CUDA
    } else {
        // SW 경로
        AVPixelFormat ofmt = static_cast<AVPixelFormat>(in->format);
        return sw_scalers[idx].process(in, ow, oh, ofmt, out); // out: SW frame
    }
}

map<int,AVCodecContext*> EncoderHandler::get_encoder_codec_context(AVBufferRef* hw_device_ctx)
{
    for(int i = 0; i < encoder_config.params.size(); i++)
    {
        const AVCodec* codec = get_encoder_codec();
        AVCodecContext* enc_ctx = avcodec_alloc_context3(codec);
        enc_ctx->height = encoder_config.params[i].height;
        enc_ctx->width = encoder_config.params[i].width;
        enc_ctx->time_base = encoder_config.time_base;
        enc_ctx->framerate = encoder_config.frame_rate;
        enc_ctx->bit_rate = encoder_config.params[i].bit_rate;
        av_opt_set(enc_ctx->priv_data, "preset", encoder_config.preset.c_str(), 0);

        AVDictionary* options = NULL;
        if (encoder_config.encoder_type == EncoderType::AV1) {
            enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        enc_ctx->rc_max_rate = enc_ctx->bit_rate;
        enc_ctx->rc_min_rate = enc_ctx->bit_rate;
        enc_ctx->rc_buffer_size = enc_ctx->bit_rate;
        enc_ctx->gop_size = round((double)enc_ctx->framerate.num / enc_ctx->framerate.den);
        enc_ctx->keyint_min = enc_ctx->gop_size;
        enc_ctx->max_b_frames = 0;
        enc_ctx->qcompress = 0.6;
        enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        enc_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        
        if (encoder_config.use_gpu)
        {
            enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
            enc_ctx->hw_frames_ctx = get_hw_frame_ref(hw_device_ctx,i);
            enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            av_dict_set(&options, "forced-idr", "1", 0);
            av_dict_set(&options, "no-scenecut", "1", 0);
            av_dict_set(&options, "strict_gop", "1", 0);
            av_dict_set(&options, "repeat-headers", "1", 0);
            av_dict_set(&options, "aud", "1", 0);
        }
        else
        {
            av_opt_set(enc_ctx->priv_data, "x265-params", "scenecut=0:open-gop=0:bframes=0", 0);
        }

        int ret = avcodec_open2(enc_ctx, codec, &options);

        if (ret < 0)
        {
            Logger::get_instance().print_log_with_reason(AV_LOG_ERROR,"encoder avcode_open2 failed.",ret);
            continue;
        }
        enc_ctxs[i] = enc_ctx;
    }
    return enc_ctxs;
}