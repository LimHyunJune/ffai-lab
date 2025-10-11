#pragma once

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
}

#include "Logger.h"
#include <vector>
#include <map>
using namespace std;

class DecoderHandler
{
    private:
        AVCodecContext* dec_ctx;
        const AVCodec* get_decoder_codec(AVCodecID codec_id, bool use_gpu);

    public:
        DecoderHandler();
        ~DecoderHandler();

       AVCodecContext* get_decoder_codec_context(AVStream* video_stream, AVBufferRef* hw_device_ctx = nullptr);
};