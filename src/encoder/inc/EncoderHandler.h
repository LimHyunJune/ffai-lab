#pragma once

extern "C"
{
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/opt.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/pixdesc.h>
    #include <libavcodec/bsf.h>
    #include <libavutil/avutil.h>
    #include <libswscale/swscale.h>
    #include <libavutil/pixfmt.h>
    #include <libavutil/frame.h>
    #include <libavutil/imgutils.h>
}

#include <string>
using namespace std;

enum class EncoderType
{
    H264, H265, AV1
};

struct EncoderConfig
{
    int height;
    int width;
    int bit_rate;
    AVRational frame_rate;
    AVRational time_base;
    string preset;
    bool use_gpu;
    EncoderType encoder_type;
};

class EncoderHandler
{
    private:
        AVCodecContext *enc_ctx = nullptr;
        EncoderConfig encoder_config;

        const AVCodec* get_encoder_codec();

        AVBufferRef* get_hw_frame_ref(AVBufferRef* hw_device_ctx);
    public:
        EncoderHandler() = delete;
        EncoderHandler(EncoderConfig encoder_config);
        ~EncoderHandler();
        AVCodecContext* get_encoder_codec_context(AVBufferRef* hw_device_ctx = nullptr);
};