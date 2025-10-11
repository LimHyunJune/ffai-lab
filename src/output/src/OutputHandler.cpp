#include "OutputHandler.h"
#include "Logger.h"

OutputHandler::OutputHandler(OutputConfig output_config)
{
    this->output_config = output_config;
	this->output_fmt_ctx = nullptr;
	this->output_stream = nullptr;
}

OutputHandler::~OutputHandler()
{
    avformat_free_context(output_fmt_ctx);
}

AVStream* OutputHandler::get_output_stream()
{
	return output_stream;
}

AVFormatContext* OutputHandler::get_output_format_context()
{
    avformat_alloc_output_context2(&output_fmt_ctx, nullptr, "mpegts", (output_config.output_path).c_str());

    output_stream = avformat_new_stream(output_fmt_ctx, nullptr);
	avcodec_parameters_from_context(output_stream->codecpar, output_config.enc_ctx);
	output_stream->time_base = output_config.enc_ctx->time_base;
	output_stream->avg_frame_rate = output_config.enc_ctx->framerate;

    AVDictionary *io_opts = NULL;
	av_dict_set(&io_opts, "reuse", "1", 0);
	av_dict_set(&io_opts, "pkt_size", "1316", 0);
	av_dict_set(&io_opts, "buffer_size", "1048576", 0);
	av_dict_set(&io_opts, "fifo_size", "524288", 0);
	AVIOContext *avio_ctx = NULL;
	avio_open2(&avio_ctx, (output_config.output_path).c_str(), AVIO_FLAG_WRITE, NULL, &io_opts);
	output_fmt_ctx->pb = avio_ctx;
	av_dict_free(&io_opts);

	AVDictionary *options = NULL;
	av_dict_set(&options, "mpegts_flags", "+resend_headers+initial_discontinuity", 0);
	av_dict_set(&options, "pat_period", "0.5", 0);
	av_dict_set(&options, "pmt_period", "0.5", 0);
	av_dict_set(&options, "pcr_period", "20", 0);
	av_dict_set(&options, "flush_packets", "1", 0);
	av_dict_set(&options, "mpegts_copyts", "1", 0);

	int ret;
	if (ret = avformat_write_header(output_fmt_ctx, &options) < 0)
	{
		Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "Failed to open input. Error Writing Headers.", ret);
		return nullptr;
	}
	av_dict_free(&options);
	av_dump_format(output_fmt_ctx, 0, (output_config.output_path).c_str(), 1);
	return output_fmt_ctx;
}