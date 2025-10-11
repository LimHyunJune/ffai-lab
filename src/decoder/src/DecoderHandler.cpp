#include "DecoderHandler.h"

DecoderHandler::DecoderHandler(){}

DecoderHandler::~DecoderHandler()
{
    avcodec_free_context(&dec_ctx);
}

const AVCodec* DecoderHandler::get_decoder_codec(AVCodecID codec_id, bool use_gpu)
{
	switch (codec_id)
	{
		case AVCodecID::AV_CODEC_ID_H264:
			if (use_gpu)
				return avcodec_find_decoder_by_name("h264_cuvid");
			else
				return avcodec_find_decoder_by_name("h264");
		case AVCodecID::AV_CODEC_ID_HEVC:
			if (use_gpu)
				return avcodec_find_decoder_by_name("hevc_cuvid");
			else
				return avcodec_find_decoder_by_name("hevc");
		case AVCodecID::AV_CODEC_ID_AV1:
			if (use_gpu)
				return avcodec_find_decoder_by_name("av1_cuvid");
			else
				return avcodec_find_decoder_by_name("av1");
		default:
			break;
	}
	return avcodec_find_decoder(codec_id);
}


AVCodecContext* DecoderHandler::get_decoder_codec_context(AVStream* video_stream, AVBufferRef* hw_device_ctx)
{
	bool use_gpu = false;
	if (hw_device_ctx != nullptr)
	{
		use_gpu = true;
	}

    int index = video_stream->index;
    BOOST_LOG(debug) << "get decoder context / video stream index : " << index;
    AVCodecID codec_id = video_stream->codecpar->codec_id;
    const AVCodec* decoder = get_decoder_codec(codec_id, use_gpu);
    if(!decoder)
    {
        av_log(nullptr, AV_LOG_ERROR, "Decoder not found!\n");
        return nullptr;
    }

    dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, video_stream->codecpar);
    dec_ctx->pkt_timebase = video_stream->time_base;
    
    if (hw_device_ctx != nullptr)
    {
        dec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }
    else {
        dec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

     // AVCodec 구조체의 capabilities flag를 보고 프레임 단위 병렬을 지원하는지, 슬라이스 단위 병렬만 지원하는지 판단

    dec_ctx->thread_count = 0;
    if (decoder->capabilities & AV_CODEC_CAP_FRAME_THREADS)
        dec_ctx->thread_type = FF_THREAD_FRAME;
    else if (decoder->capabilities & AV_CODEC_CAP_SLICE_THREADS)
        dec_ctx->thread_type = FF_THREAD_SLICE;
    else
        dec_ctx->thread_count = 1; //don't use multithreading
    int ret = avcodec_open2(dec_ctx, decoder, nullptr);
    if (ret < 0)
    {
        char errorStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_make_error_string(errorStr, AV_ERROR_MAX_STRING_SIZE, ret);
        av_log(nullptr, AV_LOG_ERROR, "decoder avcode_open2 failed. : %s\n", errorStr);
        return nullptr;
    }
	return dec_ctx;
}