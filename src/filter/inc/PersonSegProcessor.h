#pragma once
#include <vector>
#include <memory>
#include <algorithm>
#include <cstdint>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "personseg_ort.h" // personseg_create / personseg_run / personseg_destroy

class PersonSegProcessor {
public:
    PersonSegProcessor() = default;
    ~PersonSegProcessor() { close(); }

    // model_path가 비면 ORT는 비활성화되고 휴리스틱으로만 동작
    bool init(const char* model_path,
              int in_w, int in_h,
              float thr,
              int threads,
              int src_w, int src_h,
              AVPixelFormat src_fmt);

    void close();

    // src(원본 어떤 포맷), dst(YUV420P)로 출력. 성공=0
    int process(AVFrame* src, AVFrame* dst);

    // (옵션) 외부에서 투명도 바꾸고 싶을 때 호출 (0.0f~1.0f)
    void set_overlay_alpha(float a) { overlay_alpha_ = (a < 0.f ? 0.f : (a > 1.f ? 1.f : a)); }

private:
    // ---- ORT 핸들/플래그 ----
    PersonSegORT* ort_ = nullptr;       // personseg_ort.h 의 핸들
    bool          ort_ready_ = false;
    int           ort_threads_ = 1;

    // ---- 내부 상태 ----
    int in_w_ = 0, in_h_ = 0;           // 내부 마스크 해상도
    int src_w_ = 0, src_h_ = 0;         // 소스 크기(참고용)
    AVPixelFormat src_fmt_ = AV_PIX_FMT_NONE;
    float mask_thr_ = 0.5f;             // 마스크 임계(0~1)
    float overlay_alpha_ = 0.35f;       // 반투명 정도

    // sws 컨버터
    SwsContext* sws_in_to_rgb_ = nullptr;  // src -> RGB24
    SwsContext* sws_rgb_to_yuv_ = nullptr; // RGB24 -> YUV420P

    // sws 파라미터 캐시
    int sws_in_src_w_ = 0,  sws_in_src_h_ = 0,  sws_in_src_fmt_ = -1;
    int sws_in_dst_w_ = 0,  sws_in_dst_h_ = 0; // RGB24 고정

    int sws_out_src_w_ = 0, sws_out_src_h_ = 0; // RGB24 고정
    int sws_out_dst_w_ = 0, sws_out_dst_h_ = 0; // YUV420P 크기

    // 작업 버퍼
    std::vector<uint8_t> rgb_buf_;      // RGB24 (src_w x src_h x 3)
    std::vector<float>   input_chw_;    // CHW float (3 x in_h x in_w), [0..1]
    std::vector<float>   mask_f_;       // 출력 마스크(in_w x in_h), [0..1]

private:
    bool ensure_sws_in_to_rgb(AVPixelFormat in_fmt, int inW, int inH);
    bool ensure_sws_rgb_to_yuv(int outW, int outH);

    // RGB24(WH) -> CHW float(in_w_ x in_h_), [0..1]
    void rgb_to_chw_resized(const uint8_t* rgb, int srcW, int srcH,
                            int dstW, int dstH, std::vector<float>& chw3);

    // 마스크를 이용해 dst(YUV420P)에 "반투명 초록" 블렌딩
    void blend_green_with_mask(AVFrame* dst, const std::vector<float>& mask,
                               int maskW, int maskH, float thr, float alpha);

    // 휴리스틱(밝기 Otsu + YCbCr 피부색 + 완화 + 3x3 팽창)
    bool run_mask_heuristic(const float* chw, int inW, int inH, std::vector<float>& outMask);

    // ONNX 추론
    bool run_mask_onnx(const float* chw, int inW, int inH, std::vector<float>& outMask);
};
