#pragma once
extern "C"{
    #include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
	#include <libavfilter/avfilter.h>
	#include <libavfilter/buffersink.h>
	#include <libavfilter/buffersrc.h>
    #include <libavutil/opt.h>
}

#include <string>
#include <map>
#include <vector>
using namespace std;

enum class FilterType
{
    NONE
};

struct FilterConfig
{
    AVCodecContext* dec_ctx;
    AVStream* video_stream;
    bool use_gpu;
    FilterType filter_type;
    AVBufferRef* hw_frames_ctx;
};

class FilterHandler
{
    private:
        AVFilterGraph* filter_graph = nullptr;
        AVFilterContext* buffersrc_ctx = nullptr;
        AVFilterContext* buffersink_ctx = nullptr;

        AVFilterInOut* inputs = nullptr;
        AVFilterInOut* outputs = nullptr;

        string filter_descr = "";
        string format = "";
        string scale_type = "";
        string overlay_type = "";
        string hwupload = "";
        string hwdownload = "";

        void make_filter_graph_parser();
        void create_filter_graph();
        void generate_filter();

        FilterConfig filter_config;
    public:
        FilterHandler() = delete;
        FilterHandler(FilterConfig filter_config);
        ~FilterHandler();
        pair<AVFilterContext*,AVFilterContext*> get_filter_context();
};