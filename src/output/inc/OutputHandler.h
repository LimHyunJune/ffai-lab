#pragma once

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
}
    
#include <string>
#include <vector>
#include <map>
using namespace std;

enum class OutputType
{
    FILE, SRT
};

struct OutputParam
{
    AVCodecContext* enc_ctx;
    string output_path;
};

struct OutputConfig
{
    vector<OutputParam> params;
    OutputType output_type;
};

class OutputHandler
{
    private:
        map<int,AVFormatContext*> output_fmt_ctxs;
        map<int,AVStream*> output_streams;

        OutputConfig output_config;
    public:
        OutputHandler() = delete;
        OutputHandler(OutputConfig output_config);
        ~OutputHandler();

        map<int,AVStream*> get_output_stream();
	    map<int,AVFormatContext*> get_output_format_context();
};