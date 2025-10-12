#include "PersonSegProcessor.h"
#include <algorithm>
#include <cstring>
#include <cmath>

// ======================
// 도우미 함수 / SWS 보장
// ======================

bool PersonSegProcessor::ensure_sws_in_to_rgb(AVPixelFormat in_fmt, int inW, int inH) {
    const int outW = inW, outH = inH; // 동일 크기 RGB24
    const AVPixelFormat out_fmt = AV_PIX_FMT_RGB24;

    bool need_new =
        (sws_in_to_rgb_ == nullptr) ||
        (sws_in_src_w_ != inW)  || (sws_in_src_h_ != inH)  || (sws_in_src_fmt_ != (int)in_fmt) ||
        (sws_in_dst_w_ != outW) || (sws_in_dst_h_ != outH);

    if (!need_new) return true;

    if (sws_in_to_rgb_) {
        sws_freeContext(sws_in_to_rgb_);
        sws_in_to_rgb_ = nullptr;
    }

    sws_in_to_rgb_ = sws_getContext(inW, inH, in_fmt,
                                    outW, outH, out_fmt,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_in_to_rgb_) return false;

    // 캐시 갱신
    sws_in_src_w_  = inW;
    sws_in_src_h_  = inH;
    sws_in_src_fmt_= (int)in_fmt;
    sws_in_dst_w_  = outW;
    sws_in_dst_h_  = outH;

    // RGB 버퍼 준비
    rgb_buf_.resize(static_cast<size_t>(outW) * outH * 3u);
    return true;
}

bool PersonSegProcessor::ensure_sws_rgb_to_yuv(int outW, int outH) {
    const AVPixelFormat in_fmt  = AV_PIX_FMT_RGB24;
    const AVPixelFormat out_fmt = AV_PIX_FMT_YUV420P;

    bool need_new =
        (sws_rgb_to_yuv_ == nullptr) ||
        (sws_out_src_w_ != outW) || (sws_out_src_h_ != outH) ||
        (sws_out_dst_w_ != outW) || (sws_out_dst_h_ != outH);

    if (!need_new) return true;

    if (sws_rgb_to_yuv_) {
        sws_freeContext(sws_rgb_to_yuv_);
        sws_rgb_to_yuv_ = nullptr;
    }

    sws_rgb_to_yuv_ = sws_getContext(outW, outH, in_fmt,
                                     outW, outH, out_fmt,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_rgb_to_yuv_) return false;

    sws_out_src_w_ = outW;
    sws_out_src_h_ = outH;
    sws_out_dst_w_ = outW;
    sws_out_dst_h_ = outH;
    return true;
}

// ======================
// RGB -> CHW float
// ======================

void PersonSegProcessor::rgb_to_chw_resized(const uint8_t* rgb, int srcW, int srcH,
                                            int dstW, int dstH, std::vector<float>& chw3)
{
    // 최근접 샘플링
    chw3.resize(static_cast<size_t>(3) * dstW * dstH);
    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;

    float* c0 = chw3.data();
    float* c1 = c0 + dstW * dstH;
    float* c2 = c1 + dstW * dstH;

    for (int y = 0; y < dstH; ++y) {
        int sy = std::min(srcH - 1, (int)std::floor(y * scaleY));
        for (int x = 0; x < dstW; ++x) {
            int sx = std::min(srcW - 1, (int)std::floor(x * scaleX));
            const uint8_t* p = rgb + (sy * srcW + sx) * 3;
            // RGB -> [0..1]
            c0[y * dstW + x] = p[0] / 255.0f;
            c1[y * dstW + x] = p[1] / 255.0f;
            c2[y * dstW + x] = p[2] / 255.0f;
        }
    }
}

// ======================
// 반투명 초록 블렌딩(YUV420P)
// ======================

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

void PersonSegProcessor::blend_green_with_mask(AVFrame* dst,
                                               const std::vector<float>& mask,
                                               int maskW, int maskH, float thr, float alpha)
{
    // YUV420P planes
    uint8_t* Y = dst->data[0];
    uint8_t* U = dst->data[1];
    uint8_t* V = dst->data[2];
    int ys = dst->linesize[0];
    int us = dst->linesize[1];
    int vs = dst->linesize[2];

    const int W = dst->width;
    const int H = dst->height;

    // "초록"의 YUV (대략 BT.601, sRGB [0,255] 기준: R=0,G=255,B=0)
    const uint8_t Yg = 150, Ug = 44, Vg = 21;

    // --- Y(휘도): 픽셀 단위 알파 블렌딩 ---
    float sX = (float)maskW / (float)W;
    float sY = (float)maskH / (float)H;

    for (int y = 0; y < H; ++y) {
        int my = std::min(maskH - 1, (int)std::floor(y * sY));
        uint8_t* yrow = Y + y * ys;
        for (int x = 0; x < W; ++x) {
            int mx = std::min(maskW - 1, (int)std::floor(x * sX));
            float m = mask[my * maskW + mx];
            if (m <= 0.f) continue;

            // 연속 블렌딩: a = alpha * soft(m,thr)
            float soft = (thr <= 0.f) ? m : std::max(0.f, (m - thr) / (1.f - thr));
            float a = std::clamp(alpha * soft, 0.f, 1.f);

            if (a > 0.f) {
                int y0 = yrow[x];
                int y1 = (int)std::round((1.0f - a) * y0 + a * (int)Yg);
                yrow[x] = clamp_u8(y1);
            }
        }
    }

    // --- U/V(크로마): 4:2:0 → 2×2 블록 평균 알파로 블렌딩 ---
    int CW = (W + 1) >> 1;
    int CH = (H + 1) >> 1;

    float sXc = (float)maskW / (float)(CW << 1); // 크로마 샘플이 커버하는 2x2 블록 기준
    float sYc = (float)maskH / (float)(CH << 1);

    for (int cy = 0; cy < CH; ++cy) {
        uint8_t* urow = U + cy * us;
        uint8_t* vrow = V + cy * vs;

        // 이 크로마 샘플이 커버하는 원본 픽셀 범위(대략)
        int py = cy << 1;
        int my0 = std::min(maskH - 1, (int)std::floor((py + 0.0f) * sY));
        int my1 = std::min(maskH - 1, (int)std::floor((py + 1.0f) * sY));

        for (int cx = 0; cx < CW; ++cx) {
            int px = cx << 1;

            int mx0 = std::min(maskW - 1, (int)std::floor((px + 0.0f) * sX));
            int mx1 = std::min(maskW - 1, (int)std::floor((px + 1.0f) * sX));

            // 2x2 주변 마스크 평균
            float m00 = mask[my0 * maskW + mx0];
            float m01 = mask[my0 * maskW + mx1];
            float m10 = mask[my1 * maskW + mx0];
            float m11 = mask[my1 * maskW + mx1];
            float m_avg = (m00 + m01 + m10 + m11) * 0.25f;

            if (m_avg <= 0.f) continue;

            float soft = (thr <= 0.f) ? m_avg : std::max(0.f, (m_avg - thr) / (1.f - thr));
            float a = std::clamp(alpha * soft, 0.f, 1.f);
            if (a <= 0.f) continue;

            int u0 = urow[cx];
            int v0 = vrow[cx];
            int u1 = (int)std::round((1.0f - a) * u0 + a * (int)Ug);
            int v1 = (int)std::round((1.0f - a) * v0 + a * (int)Vg);
            urow[cx] = clamp_u8(u1);
            vrow[cx] = clamp_u8(v1);
        }
    }
}

// ======================
// Otsu + YCbCr 피부색 기반 마스크
// ======================

bool PersonSegProcessor::run_mask(const float* chw, int inW, int inH, std::vector<float>& outMask) {
    if (!chw || inW <= 0 || inH <= 0) return false;

    const float* R = chw;
    const float* G = R + inW * inH;
    const float* B = G + inW * inH;

    const int N = inW * inH;

    // 1) 밝기(0..255) 히스토그램 (BT.601 근사)
    int hist[256] = {0};
    auto toY = [&](int idx)->uint8_t {
        float y = 0.299f * R[idx] + 0.587f * G[idx] + 0.114f * B[idx];
        int   v = (int)std::round(std::clamp(y, 0.0f, 1.0f) * 255.0f);
        return (uint8_t)std::clamp(v, 0, 255);
    };
    for (int i = 0; i < N; ++i) hist[toY(i)]++;

    // 2) Otsu 임계
    int total = N;
    double sum = 0.0;
    for (int t = 0; t < 256; ++t) sum += t * hist[t];

    double sumB = 0.0;
    int wB = 0, wF = 0;
    double varMax = -1.0;
    int threshold = 127;

    for (int t = 0; t < 256; ++t) {
        wB += hist[t];
        if (wB == 0) continue;
        wF = total - wB;
        if (wF == 0) break;

        sumB += (double)t * hist[t];

        double mB = sumB / wB;
        double mF = (sum - sumB) / wF;

        double varBetween = (double)wB * (double)wF * (mB - mF) * (mB - mF);
        if (varBetween > varMax) {
            varMax = varBetween;
            threshold = t;
        }
    }

    // 3) YCbCr 피부색 마스크 (완화된 범위)
    outMask.assign(N, 0.0f);

    for (int i = 0; i < N; ++i) {
        // RGB[0..1] -> 8bit
        float r = std::clamp(R[i], 0.0f, 1.0f) * 255.0f;
        float g = std::clamp(G[i], 0.0f, 1.0f) * 255.0f;
        float b = std::clamp(B[i], 0.0f, 1.0f) * 255.0f;

        // BT.601 변환
        float Y = 16.0f  + (65.481f*r + 128.553f*g + 24.966f*b) / 255.0f;
        float Cb = 128.0f + (-37.797f*r - 74.203f*g + 112.0f*b) / 255.0f;
        float Cr = 128.0f + (112.0f*r - 93.786f*g - 18.214f*b) / 255.0f;

        bool skin = (Cb >= 70.f && Cb <= 135.f && Cr >= 125.f && Cr <= 180.f && Y > 16.0f);
        bool otsu = (toY(i) >= threshold);

        // soft mask: 피부면 1, 아니면 Otsu로 보완(0/1)
        outMask[i] = (skin || otsu) ? 1.0f : 0.0f;
    }

    // 4) 너무 적으면 자동 완화
    int total_on = 0;
    for (float v : outMask) total_on += (v >= 1.0f);

    if (total_on < N / 200) {
        for (int i = 0; i < N; ++i) {
            float r = std::clamp(R[i], 0.0f, 1.0f) * 255.0f;
            float g = std::clamp(G[i], 0.0f, 1.0f) * 255.0f;
            float b = std::clamp(B[i], 0.0f, 1.0f) * 255.0f;

            float Y = 16.0f  + (65.481f*r + 128.553f*g + 24.966f*b) / 255.0f;
            float Cb = 128.0f + (-37.797f*r - 74.203f*g + 112.0f*b) / 255.0f;
            float Cr = 128.0f + (112.0f*r - 93.786f*g - 18.214f*b) / 255.0f;

            bool skin2 = (Cb >= 60.f && Cb <= 150.f && Cr >= 115.f && Cr <= 190.f && Y > 12.0f);
            bool otsu2 = (toY(i) >= std::max(0, threshold - 20));

            if (skin2 || otsu2) outMask[i] = 1.0f;
        }
    }

    // 5) 3x3 팽창
    std::vector<float> tmp = outMask;
    auto at = [&](int y, int x)->float { return tmp[y*inW + x]; };

    for (int y = 1; y < inH-1; ++y) {
        for (int x = 1; x < inW-1; ++x) {
            float m = 0.0f;
            for (int dy=-1; dy<=1; ++dy)
                for (int dx=-1; dx<=1; ++dx)
                    m = std::max(m, at(y+dy, x+dx));
            outMask[y*inW + x] = m;
        }
    }

    return true;
}

// ======================
// Public APIs
// ======================

bool PersonSegProcessor::init(const char* /*model_path*/,
                              int in_w, int in_h,
                              float thr,
                              int /*threads*/,
                              int src_w, int src_h,
                              AVPixelFormat src_fmt)
{
    in_w_   = in_w;
    in_h_   = in_h;
    mask_thr_= thr;                    // Controller에서 0.3~0.6 정도로 조정
    src_w_  = src_w;
    src_h_  = src_h;
    src_fmt_= src_fmt;

    // sws 컨텍스트는 실제 처리 시점에 생성/갱신
    rgb_buf_.clear();
    input_chw_.clear();
    mask_f_.clear();

    return true;
}

void PersonSegProcessor::close() {
    if (sws_in_to_rgb_) {
        sws_freeContext(sws_in_to_rgb_);
        sws_in_to_rgb_ = nullptr;
    }
    if (sws_rgb_to_yuv_) {
        sws_freeContext(sws_rgb_to_yuv_);
        sws_rgb_to_yuv_ = nullptr;
    }
}

int PersonSegProcessor::process(AVFrame* src, AVFrame* dst) {
    if (!src || !dst) return AVERROR(EINVAL);

    // dst는 호출부에서 YUV420P로 할당되어 온다고 가정(안전장치)
    if (dst->format != AV_PIX_FMT_YUV420P ||
        dst->width  != src->width ||
        dst->height != src->height)
    {
        return AVERROR(EINVAL);
    }

    // 1) src -> RGB24 (동일 해상도)
    if (!ensure_sws_in_to_rgb((AVPixelFormat)src->format, src->width, src->height)) {
        // 실패 시: src를 바로 YUV420P로 변환만 수행하고 종료
        if (!ensure_sws_rgb_to_yuv(src->width, src->height)) return AVERROR(EIO);
        const uint8_t* in_data[4] = { src->data[0], src->data[1], src->data[2], nullptr };
        int            in_ls [4]  = { src->linesize[0], src->linesize[1], src->linesize[2], 0 };
        uint8_t*       out_data[4]= { dst->data[0], dst->data[1], dst->data[2], nullptr };
        int            out_ls [4] = { dst->linesize[0], dst->linesize[1], dst->linesize[2], 0 };
        SwsContext* s = sws_getContext(src->width, src->height, (AVPixelFormat)src->format,
                                       dst->width,  dst->height,  AV_PIX_FMT_YUV420P,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!s) return AVERROR(EIO);
        sws_scale(s, in_data, in_ls, 0, src->height, out_data, out_ls);
        sws_freeContext(s);
        return 0;
    }

    // src -> RGB24
    {
        const uint8_t* in_data[4] = { src->data[0], src->data[1], src->data[2], nullptr };
        int            in_ls [4]  = { src->linesize[0], src->linesize[1], src->linesize[2], 0 };
        uint8_t*       out_data[4]= { rgb_buf_.data(), nullptr, nullptr, nullptr };
        int            out_ls [4] = { src->width * 3, 0, 0, 0 };
        sws_scale(sws_in_to_rgb_, in_data, in_ls, 0, src->height, out_data, out_ls);
    }

    // 2) RGB24 -> CHW float (내부 마스크 해상도)
    rgb_to_chw_resized(rgb_buf_.data(), src->width, src->height, in_w_, in_h_, input_chw_);

    // 3) Otsu + 피부색 결합 마스크 생성
    if (!run_mask(input_chw_.data(), in_w_, in_h_, mask_f_)) {
        // 실패: RGB24 -> YUV420P 복사
        if (!ensure_sws_rgb_to_yuv(src->width, src->height)) return AVERROR(EIO);
        const uint8_t* in_data[4] = { rgb_buf_.data(), nullptr, nullptr, nullptr };
        int            in_ls [4]  = { src->width * 3, 0, 0, 0 };
        uint8_t*       out_data[4]= { dst->data[0], dst->data[1], dst->data[2], nullptr };
        int            out_ls [4] = { dst->linesize[0], dst->linesize[1], dst->linesize[2], 0 };
        sws_scale(sws_rgb_to_yuv_, in_data, in_ls, 0, src->height, out_data, out_ls);
        return 0;
    }

    // 4) 기본 배경: 원본을 YUV420P로 깔기
    {
        if (!ensure_sws_rgb_to_yuv(src->width, src->height)) return AVERROR(EIO);
        const uint8_t* in_data[4] = { rgb_buf_.data(), nullptr, nullptr, nullptr };
        int            in_ls [4]  = { src->width * 3, 0, 0, 0 };
        uint8_t*       out_data[4]= { dst->data[0], dst->data[1], dst->data[2], nullptr };
        int            out_ls [4] = { dst->linesize[0], dst->linesize[1], dst->linesize[2], 0 };
        sws_scale(sws_rgb_to_yuv_, in_data, in_ls, 0, src->height, out_data, out_ls);
    }

    // 5) 마스크가 1(전경=사람 추정)인 곳을 "반투명 초록"으로 블렌딩
    blend_green_with_mask(dst, mask_f_, in_w_, in_h_, mask_thr_, overlay_alpha_);

    return 0;
}
