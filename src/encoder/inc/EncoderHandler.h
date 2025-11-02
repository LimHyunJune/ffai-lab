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

#include <vector>
#include <string>
#include <map>
using namespace std;

enum class EncoderType
{
    H264, H265, AV1
};

struct EncoderParam
{
    int width;
    int height;
    int bit_rate;
};

struct EncoderConfig
{
    bool abr;
    vector<EncoderParam> params;
    AVRational frame_rate;
    AVRational time_base;
    string preset;
    bool use_gpu;
    EncoderType encoder_type;
};

class EncoderHandler
{
    private:
        map<int, AVCodecContext*> enc_ctxs;
        EncoderConfig encoder_config;

        const AVCodec* get_encoder_codec();

        AVBufferRef* get_hw_frame_ref(AVBufferRef* hw_device_ctx, int idx);


        struct GpuScaler {
            AVFilterGraph*  graph = nullptr;
            AVFilterContext *src = nullptr, *scale = nullptr, *sink = nullptr;
            int in_w=0, in_h=0, out_w=0, out_h=0;
            bool ready=false;
    
            int init(AVBufferRef* hw_frames_ctx, int iw, int ih, int ow, int oh);
            int process(AVFrame* in, AVFrame** out);
            void close();
        };

        struct SwScaler {
            SwsContext* sws = nullptr;
            int in_w=0, in_h=0, out_w=0, out_h=0;
            AVPixelFormat in_fmt = AV_PIX_FMT_NONE, out_fmt = AV_PIX_FMT_NONE;
    
            int ensure_ctx(int iw, int ih, AVPixelFormat ifmt, int ow, int oh, AVPixelFormat ofmt);
            int process(AVFrame* in, int ow, int oh, AVPixelFormat ofmt, AVFrame** out);
            void close();
        };

        vector<GpuScaler> gpu_scalers;
        vector<SwScaler>  sw_scalers;


    public:
        EncoderHandler() = delete;
        EncoderHandler(EncoderConfig encoder_config);
        ~EncoderHandler();
        map<int,AVCodecContext*> get_encoder_codec_context(AVBufferRef* hw_device_ctx = nullptr);

        int scale_frame(int idx, AVFrame* in, AVFrame** out);
};