#pragma once

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
}
    
#include <string>
using namespace std;

enum class OutputType
{
    FILE, SRT
};

struct OutputConfig
{
    AVCodecContext* enc_ctx;
    string output_path;
    OutputType output_type;
};

class OutputHandler
{
    private:
        AVFormatContext* output_fmt_ctx;
        AVStream* output_stream;

        OutputConfig output_config;
    public:
        OutputHandler() = delete;
        OutputHandler(OutputConfig output_config);
        ~OutputHandler();

        AVStream* get_output_stream();
	    AVFormatContext* get_output_format_context();
};