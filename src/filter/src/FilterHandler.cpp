#include "FilterHandler.h"
#include "Logger.h"

FilterHandler::FilterHandler(FilterConfig filter_config){
    this->filter_config = filter_config;
}
FilterHandler::~FilterHandler()
{
    avfilter_graph_free(&filter_graph);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	this->filter_descr = "";
}

void FilterHandler::create_filter_graph()
{
    char name[8]; snprintf(name, sizeof(name), "v%d", 0); 

    AVPixelFormat format = AV_PIX_FMT_YUV420P;
    if (filter_config.use_gpu)
        format = AV_PIX_FMT_NV12;

    char args[512];
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/%d:pixel_aspect=1/1",
        filter_config.dec_ctx->width, 
        filter_config.dec_ctx->height, 
        format,
        filter_config.video_stream->time_base.num,
        filter_config.video_stream->time_base.den,
        filter_config.video_stream->r_frame_rate.num,
        filter_config.video_stream->r_frame_rate.den
    );
    avfilter_graph_create_filter(&buffersrc_ctx, avfilter_get_by_name("buffer"), name, args, nullptr, filter_graph);

    AVBufferSrcParameters params;
    memset(&params,0,sizeof(params));
    params.hw_frames_ctx = av_buffer_ref(filter_config.hw_frames_ctx);
    params.format = AV_PIX_FMT_CUDA;
    params.time_base = filter_config.video_stream->time_base;
    params.width = 3840;
    params.height = 2160;
    av_buffersrc_parameters_set(buffersrc_ctx, &params);
}

void FilterHandler::make_filter_graph_parser()
{
    AVFilterInOut* out = avfilter_inout_alloc();
    char label[8];

    int index = filter_config.video_stream->index;

    out->name = av_strdup(label);
    out->filter_ctx = buffersrc_ctx;
    out->pad_idx = 0;
    out->next = outputs;
    outputs = out;
}

void FilterHandler::generate_filter()
{
    filter_graph = avfilter_graph_alloc();
    av_opt_set_int(filter_graph, "threads", 1, 0);

    create_filter_graph();
	avfilter_graph_create_filter(&buffersink_ctx, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, filter_graph);

    switch (filter_config.filter_type)
	{
        default:
            break;
    }

	outputs = nullptr;
    make_filter_graph_parser();

    inputs = avfilter_inout_alloc();
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = nullptr;

	avfilter_graph_parse_ptr(filter_graph, filter_descr.c_str(), &inputs, &outputs, nullptr);
	avfilter_graph_config(filter_graph, nullptr);
}

pair<AVFilterContext*,AVFilterContext*> FilterHandler::get_filter_context()
{
    if (filter_config.use_gpu) {
		this->format = "nv12";
		this->scale_type = "scale_cuda";
		this->overlay_type = "overlay_cuda";
		this->hwupload = ",hwupload_cuda";
        this->hwdownload = ",hwdownload";
	}
	else {
		this->format = "yuv420p";
		this->scale_type = "scale";
		this->overlay_type = "overlay";
		this->hwupload = "";
        this->hwdownload = "";
	}
    generate_filter();
    return {buffersrc_ctx, buffersink_ctx};
}