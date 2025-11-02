#pragma once
#include <vector>
#include <memory>
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// 추가: ORT 래퍼
#include "personseg_ort.h"

class PersonSegProcessor {
public:
    PersonSegProcessor() = default;
    ~PersonSegProcessor() { close(); }

    bool init(const char* model_path,
              int in_w, int in_h,
              float thr,
              int threads,
              int src_w, int src_h,
              AVPixelFormat src_fmt);

    void close();
    int  process(AVFrame* src, AVFrame* dst);

    void set_overlay_alpha(float a) { overlay_alpha_ = (a < 0.f ? 0.f : (a > 1.f ? 1.f : a)); }

private:
    // ---- 신규: ORT 핸들/플래그 ----
    PersonSegORT* ort_ = nullptr;    // personseg_ort.h 의 핸들
    bool          ort_ready_ = false;
    int           ort_threads_ = 1;

    // ---- 기존 상태 ----
    int in_w_ = 0, in_h_ = 0;
    int src_w_ = 0, src_h_ = 0;
    AVPixelFormat src_fmt_ = AV_PIX_FMT_NONE;
    float mask_thr_ = 0.5f;
    float overlay_alpha_ = 0.35f;

    SwsContext* sws_in_to_rgb_ = nullptr;
    SwsContext* sws_rgb_to_yuv_ = nullptr;

    int sws_in_src_w_ = 0,  sws_in_src_h_ = 0,  sws_in_src_fmt_ = -1;
    int sws_in_dst_w_ = 0,  sws_in_dst_h_ = 0;
    int sws_out_src_w_ = 0, sws_out_src_h_ = 0;
    int sws_out_dst_w_ = 0, sws_out_dst_h_ = 0;

    std::vector<uint8_t> rgb_buf_;
    std::vector<float>   input_chw_;
    std::vector<float>   mask_f_;

private:
    bool ensure_sws_in_to_rgb(AVPixelFormat in_fmt, int inW, int inH);
    bool ensure_sws_rgb_to_yuv(int outW, int outH);
    void rgb_to_chw_resized(const uint8_t* rgb, int srcW, int srcH,
                            int dstW, int dstH, std::vector<float>& chw3);
    void blend_green_with_mask(AVFrame* dst, const std::vector<float>& mask,
                               int maskW, int maskH, float thr, float alpha);

    // 👇 신규: ONNX 추론
    bool run_mask_onnx(const float* chw, int inW, int inH, std::vector<float>& outMask);
};
