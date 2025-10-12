#pragma once

// FFmpeg 필터그래프 미사용 -> libavfilter 헤더 불필요
struct AVCodecContext;
struct AVStream;
struct AVBufferRef;
struct AVFilterContext;

#include <string>
#include <utility>

enum class FilterType {
    NONE
};

struct FilterConfig {
    AVCodecContext* dec_ctx      = nullptr;
    AVStream*       video_stream = nullptr;
    bool            use_gpu      = false;
    FilterType      filter_type  = FilterType::NONE;
    AVBufferRef*    hw_frames_ctx = nullptr;
    std::string     model_path =
        "/home/ubuntu/ffai-lab/model/human_segmentation_pphumanseg_2023mar_int8.onnx";
};

class FilterHandler {
public:
    explicit FilterHandler(FilterConfig cfg);   // 선언만
    ~FilterHandler();                           // 선언만

    std::pair<AVFilterContext*, AVFilterContext*> get_filter_context(); // 선언만

private:
    FilterConfig filter_config;
};
