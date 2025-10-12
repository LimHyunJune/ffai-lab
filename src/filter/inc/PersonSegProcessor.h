#pragma once
#include <vector>
#include <memory>
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

class PersonSegProcessor {
public:
    PersonSegProcessor() = default;
    ~PersonSegProcessor() { close(); }

    // model_path는 사용하지 않아도 됨. in_w/in_h: 내부 마스크 연산 해상도(예: 192x192)
    bool init(const char* model_path,
              int in_w, int in_h,
              float thr,
              int threads,
              int src_w, int src_h,
              AVPixelFormat src_fmt);

    void close();

    // src(원본), dst(YUV420P)로 출력. 성공=0
    int process(AVFrame* src, AVFrame* dst);

    // (옵션) 외부에서 투명도 바꾸고 싶을 때 호출 (0.0f~1.0f)
    void set_overlay_alpha(float a) { overlay_alpha_ = (a < 0.f ? 0.f : (a > 1.f ? 1.f : a)); }

private:
    // ---- 내부 상태 ----
    int in_w_ = 0, in_h_ = 0;          // 내부 마스크 해상도
    int src_w_ = 0, src_h_ = 0;        // 소스 크기
    AVPixelFormat src_fmt_ = AV_PIX_FMT_NONE;
    float mask_thr_ = 0.5f;            // 최종 마스킹 임계(0~1)
    float overlay_alpha_ = 0.35f;      // ✅ 반투명 정도(기본 35%)

    // sws 컨버터
    SwsContext* sws_in_to_rgb_ = nullptr;  // src -> RGB24
    SwsContext* sws_rgb_to_yuv_ = nullptr; // RGB24 -> YUV420P

    // sws 파라미터 캐시(컨텍스트 내부 필드 접근하지 않기 위해 보관)
    int sws_in_src_w_ = 0,  sws_in_src_h_ = 0,  sws_in_src_fmt_ = -1;
    int sws_in_dst_w_ = 0,  sws_in_dst_h_ = 0; // RGB24 고정

    int sws_out_src_w_ = 0, sws_out_src_h_ = 0; // RGB24 고정
    int sws_out_dst_w_ = 0, sws_out_dst_h_ = 0; // YUV420P 크기

    // 작업 버퍼
    std::vector<uint8_t> rgb_buf_;       // RGB24 (src_w x src_h x 3)
    std::vector<float>   input_chw_;     // CHW float (3 x in_h x in_w), [0..1]
    std::vector<float>   mask_f_;        // 출력 마스크(in_w x in_h), [0..1]

private:
    bool ensure_sws_in_to_rgb(AVPixelFormat in_fmt, int inW, int inH);
    bool ensure_sws_rgb_to_yuv(int outW, int outH);

    // RGB24(WH) -> CHW float(in_w_ x in_h_), [0..1]
    void rgb_to_chw_resized(const uint8_t* rgb, int srcW, int srcH,
                            int dstW, int dstH, std::vector<float>& chw3);

    // 마스크를 이용해 dst(YUV420P)에 "반투명 초록" 블렌딩
    void blend_green_with_mask(AVFrame* dst, const std::vector<float>& mask,
                               int maskW, int maskH, float thr, float alpha);

    // 간이 세그멘테이션: Otsu(밝기) + YCbCr 피부색 결합, 부족하면 자동 완화
    bool run_mask(const float* chw, int inW, int inH, std::vector<float>& outMask /*size inW*inH*/);
};
