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

void FilterHandler::filter_multiview()
{
    filter_descr += "[v0]"+scale_type+"=2496:1404"+hwdownload+",format="+format+",pad=3840:2160:240:160:#001219[main];";
    filter_descr += "[v1]"+scale_type+"=784:440"+hwdownload+",format="+format+",fps=60[sub1];";
    filter_descr += "[v2]"+scale_type+"=784:440"+hwdownload+",format="+format+",fps=60[sub2];";
    filter_descr += "[v3]"+scale_type+"=784:440"+hwdownload+",format="+format+",fps=60[sub3];";
    filter_descr += "[main][sub1]overlay=2816:160:repeatlast=1[o2];";
    filter_descr += "[o2][sub2]overlay=2816:642:repeatlast=1[o3];";
    filter_descr += "[o3][sub3]overlay=2816:1124:repeatlast=1"+hwupload+"[out]";
}

void FilterHandler::create_filter_graph(int idx)
{
    char name[8]; snprintf(name, sizeof(name), "v%d", idx); 

    AVPixelFormat format = AV_PIX_FMT_CUDA;
    if (filter_config.use_gpu)
        format = AV_PIX_FMT_NV12;

    char args[512];
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/%d:pixel_aspect=1/1",
        filter_config.width, 
        filter_config.height, 
        format,
        filter_config.time_base.num,
        filter_config.time_base.den,
        filter_config.frame_rate.num,
        filter_config.frame_rate.den
    );
    avfilter_graph_create_filter(&buffersrc_ctxs[idx], avfilter_get_by_name("buffer"), name, args, nullptr, filter_graph);

    AVBufferSrcParameters params;
    memset(&params,0,sizeof(params));
    params.hw_frames_ctx = av_buffer_ref(filter_config.hw_frames_ctx);
    params.format = AV_PIX_FMT_CUDA;
    params.time_base = filter_config.time_base;
    params.width = filter_config.width;
    params.height = filter_config.height;
    av_buffersrc_parameters_set(buffersrc_ctxs[idx], &params);
}

void FilterHandler::make_filter_graph_parser(int idx)
{
    AVFilterInOut* out = avfilter_inout_alloc();
    char label[8];
    snprintf(label,sizeof(label),"v%d",idx);

    out->name = av_strdup(label);
    out->filter_ctx = buffersrc_ctxs[idx];
    out->pad_idx = 0;
    out->next = outputs;
    outputs = out;
}

void FilterHandler::generate_filter()
{
    filter_graph = avfilter_graph_alloc();
    av_opt_set_int(filter_graph, "threads", 1, 0);

    for(int i = 0; i < 4; i++)
        create_filter_graph(i);

	avfilter_graph_create_filter(&buffersink_ctx, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, filter_graph);
 
    filter_multiview();        
	outputs = nullptr;

    for(int i = 0; i < 4; i++)
        make_filter_graph_parser(i);

    inputs = avfilter_inout_alloc();
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = nullptr;

	avfilter_graph_parse_ptr(filter_graph, filter_descr.c_str(), &inputs, &outputs, nullptr);
	avfilter_graph_config(filter_graph, nullptr);
}

pair<map<int,AVFilterContext*>,AVFilterContext*> FilterHandler::get_filter_context()
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
    return {buffersrc_ctxs, buffersink_ctx};
}
