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
    MULTIVIEW_1, MULTIVIEW_2, MULTIVIEW_3, MULTIVIEW_4, SEPARATE_1, SEPARATE_2, SEPARATE_3, SEPARATE_4, NONE
};

struct FilterConfig
{
    map<int,AVCodecContext*> dec_ctxs;
    vector<AVStream*> video_streams;
    bool use_gpu;
    FilterType filter_type;
    AVBufferRef* hw_frames_ctx;
};

class FilterHandler
{
    private:
        AVFilterGraph* filter_graph = nullptr;
        map<int,AVFilterContext*> buffersrc_ctxs;
        AVFilterContext* buffersink_ctx = nullptr;

        AVFilterInOut* inputs = nullptr;
        AVFilterInOut* outputs = nullptr;

        string filter_descr = "";
        string format = "";
        string scale_type = "";
        string overlay_type = "";
        string hwupload = "";
        string hwdownload = "";

        void filter_multiview_1();
        void filter_multiview_2();
        void filter_multiview_3();
        void filter_multiview_4();
        void filter_separate_1();
        void filter_separate_2();
        void filter_separate_3();
        void filter_separate_4();

        void make_filter_graph_parser(int idx);
        void create_filter_graph(int idx);
        void generate_filter();

        FilterConfig filter_config;
    public:
        FilterHandler() = delete;
        FilterHandler(FilterConfig filter_config);
        ~FilterHandler();
        pair<map<int,AVFilterContext*>,AVFilterContext*> get_filter_context();
};