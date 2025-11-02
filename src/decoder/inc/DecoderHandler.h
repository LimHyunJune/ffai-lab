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
        map<int, AVCodecContext*> dec_ctxs;
        const AVCodec* get_decoder_codec(AVCodecID codec_id, bool use_gpu);

    public:
        DecoderHandler();
        ~DecoderHandler();

        map<int,AVCodecContext*> get_decoder_codec_context(vector<AVStream*> video_streams, AVBufferRef* hw_device_ctx = nullptr);
};