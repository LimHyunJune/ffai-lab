#include "VmafHandler.h"
#include "Logger.h"


static const char* hevc_slice_kind(int t) {
    // IRAP (랜덤 접근) 타입 포함
    switch (t) {
        case 16: return "BLA_W_LP";
        case 17: return "BLA_W_RADL";
        case 18: return "BLA_N_LP";
        case 19: return "IDR_W_RADL";
        case 20: return "IDR_N_LP";
        case 21: return "CRA";
        default: return (t >= 0 && t <= 31) ? "VCL" : "NON_VCL";
    }
}

static int next_annexb_nal(const uint8_t* p, const uint8_t* end,
                           const uint8_t** nal_start, const uint8_t** nal_end)
{
    // start code 찾기
    const uint8_t *s = p, *e = end;
    const uint8_t *start = nullptr, *stop = nullptr;

    // 현재 NAL 시작
    for (; s + 3 < e; ++s) {
        if ((s[0]==0 && s[1]==0 && s[2]==1) ||
            (s+4<e && s[0]==0 && s[1]==0 && s[2]==0 && s[3]==1)) {
            start = (s[2]==1) ? s+3 : s+4;
            s = start;
            break;
        }
    }
    if (!start) return 0;

    // 다음 start code = 현재 NAL 끝
    for (; s + 3 < e; ++s) {
        if ((s[0]==0 && s[1]==0 && s[2]==1) ||
            (s+4<e && s[0]==0 && s[1]==0 && s[2]==0 && s[3]==1)) {
            stop = s; break;
        }
    }
    if (!stop) stop = e;

    *nal_start = start;
    *nal_end   = stop;
    return 1;
}

static int first_vcl_nal_type_annexb(const uint8_t* p, const uint8_t* end) {
    const uint8_t *ns, *ne;
    while (next_annexb_nal(p, end, &ns, &ne)) {
        int nal_type = (ns[0] >> 1) & 0x3F;
        if (nal_type <= 31) return nal_type; // VCL
        p = ne;
    }
    return -1;
}

static int first_vcl_nal_type_lengthpref(const uint8_t* p, const uint8_t* end) {
    while (p + 4 <= end) {
        uint32_t len = AV_RB32(p); p += 4;
        if (len == 0 || p + len > end) break;
        int nal_type = (p[0] >> 1) & 0x3F;
        if (nal_type <= 31) return nal_type; // VCL
        p += len;
    }
    return -1;
}

static int hevc_first_vcl_nal_type_from_packet(const AVPacket* pkt) {
    if (!pkt || !pkt->data || pkt->size <= 0) return -1;
    const uint8_t* p = pkt->data;
    const uint8_t* end = pkt->data + pkt->size;

    // Annex B 시그니처가 보이면 Annex B로, 아니면 length-prefixed로 가정
    // (엄밀히 구분하려면 start code 스캔 결과로 판단)
    const uint8_t *ns, *ne;
    if (next_annexb_nal(p, end, &ns, &ne)) {
        // Annex B로 순회하며 첫 VCL 찾기
        return first_vcl_nal_type_annexb(p, end);
    } else {
        // length-prefixed로 순회하며 첫 VCL 찾기
        return first_vcl_nal_type_lengthpref(p, end);
    }
}


VmafHandler::VmafHandler(){}

VmafHandler::~VmafHandler(){
    if (vmaf_model) 
    { 
        vmaf_model_destroy(vmaf_model); 
        vmaf_model = nullptr; 
    }
    avcodec_free_context(&dec_ctx);
}

bool VmafHandler::init(AVCodecContext* enc_ctx)
{
    this->enc_ctx = enc_ctx;
    
    const AVCodec* dec = avcodec_find_decoder(enc_ctx->codec_id);
    if(!dec)
    {
        BOOST_LOG(error) << "[VMAF] avcodec_find_decoder failed.";
        return false;
    }
    dec_ctx = avcodec_alloc_context3(dec);
    if(!dec_ctx)
    {
        BOOST_LOG(error) << "[VMAF] avcodec_alloc_context3 failed.";
        return false;
    }

    if (enc_ctx->extradata && enc_ctx->extradata_size > 0) {
        dec_ctx->extradata = (uint8_t*)av_malloc(enc_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!dec_ctx->extradata) { BOOST_LOG(error) << "[VMAF] extradata alloc failed."; return false; }
        memcpy(dec_ctx->extradata, enc_ctx->extradata, enc_ctx->extradata_size);
        memset(dec_ctx->extradata + enc_ctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        dec_ctx->extradata_size = enc_ctx->extradata_size;
    }

    dec_ctx->time_base = enc_ctx->time_base;
    dec_ctx->thread_count = 1;

    if(avcodec_open2(dec_ctx, dec, nullptr) < 0)
    {
        BOOST_LOG(error) << "[VMAF] avcodec_open2 failed.";
        return false;
    }

    VmafModelConfig mcfg{};
    mcfg.name = "vmaf";
    mcfg.flags = 0;
    
    if (vmaf_model_load_from_path(&vmaf_model, &mcfg, model_path)) 
    {
        BOOST_LOG(error) << "[VMAF] vmaf_model_load failed.";
        return false;
    }

    return true;
}

int VmafHandler::alloc_and_copy(const AVFrame* f, VmafPicture* pic) {
    if (!f || !pic) return AVERROR(EINVAL);

    int bpc = 8;
    VmafPixelFormat vfmt = VMAF_PIX_FMT_YUV420P;

    const int w = f->width;
    const int h = f->height;

    if (vmaf_picture_alloc(pic, vfmt, bpc, w, h) != 0)
        return AVERROR(ENOMEM);

    {
        const uint8_t* src = f->data[0];
        int src_ls = f->linesize[0];
        // linesize가 음수일 경우 top-row 기준으로 보정
        if (src_ls < 0) 
            src += (h - 1) * src_ls;

        // 8bit: 1 byte/pixel, 10bit(LE): 2 bytes/pixel
        const int row_bytes = (bpc == 8) ? w : (w * 2);

        for (int y = 0; y < h; ++y) {
            memcpy((uint8_t*)pic->data[0] + y * pic->stride[0],
                   src + y * src_ls, row_bytes);
        }
    }

    // --- U/V plane 복사 (4:2:0) ---
    {
        const int cw = w / 2;
        const int ch = h / 2;
        const uint8_t* src_u = f->data[1];
        const uint8_t* src_v = f->data[2];
        int src_lsu = f->linesize[1];
        int src_lsv = f->linesize[2];

        if (src_lsu < 0) 
            src_u += (ch - 1) * src_lsu;

        if (src_lsv < 0) 
            src_v += (ch - 1) * src_lsv;

        const int crow_bytes = (bpc == 8) ? cw : (cw * 2);

        for (int y = 0; y < ch; ++y) {
            memcpy((uint8_t*)pic->data[1] + y * pic->stride[1],
                   src_u + y * src_lsu, crow_bytes);
            memcpy((uint8_t*)pic->data[2] + y * pic->stride[2],
                   src_v + y * src_lsv, crow_bytes);
        }
    }
    return 0;
}

bool VmafHandler::eval_frame(const AVFrame* ref, const AVFrame* dist, double* out_score)
{
    VmafContext* vmaf_ctx{nullptr};
    VmafConfiguration cfg{};
    cfg.log_level = VMAF_LOG_LEVEL_ERROR;
    cfg.n_threads = 4;
    cfg.n_subsample = 1;
    cfg.cpumask = 0;

    if (vmaf_init(&vmaf_ctx, cfg)) 
    {
        BOOST_LOG(error) << "[VMAF] vmaf_init failed.";
        return false;
    }

    if (vmaf_use_features_from_model(vmaf_ctx, vmaf_model))
    {
        BOOST_LOG(error) << "[VMAF] vmaf_use_features_from_model failed.";
        vmaf_close(vmaf_ctx);
        return false;
    }

    if (!vmaf_ctx || !vmaf_model || !ref || !dist)
    {
        BOOST_LOG(debug) << "[VMAF] no ctx or model or ref or dist";
        vmaf_close(vmaf_ctx);
        return false;
    }
    if (ref->format != AV_PIX_FMT_YUV420P || dist->format != AV_PIX_FMT_YUV420P)
    {
        BOOST_LOG(debug) << "[VMAF] format is not yuv420p";
        vmaf_close(vmaf_ctx);
        return false;
    }
    if (ref->width != dist->width || ref->height != dist->height)
    {
        BOOST_LOG(debug) << "[VMAF] ref W/H is not same with dist W/H";
        vmaf_close(vmaf_ctx);
        return false;
    }

    VmafPicture pr{}, pd{};
    if (alloc_and_copy(ref,  &pr) != 0) { BOOST_LOG(debug) << "[VMAF] ref alloc_and_copy failed"; return false; }
    if (alloc_and_copy(dist, &pd) != 0) { vmaf_picture_unref(&pr); BOOST_LOG(debug) << "[VMAF] dist alloc_and_copy failed"; return false; }

    int err = vmaf_read_pictures(vmaf_ctx, &pr, &pd, 0);
    vmaf_picture_unref(&pr);
    vmaf_picture_unref(&pd);
    if (err) {
        BOOST_LOG(error) << "[VMAF] vmaf_read_pictures failed" << " err=" << err;
        vmaf_close(vmaf_ctx);
        return false;
    }
    // flush
    if(vmaf_read_pictures(vmaf_ctx, nullptr, nullptr, 0))
    {
        BOOST_LOG(error) << "[VMAF] flush failed";
        vmaf_close(vmaf_ctx);
        return false;
    }

    double score = 0.0;
    int res = vmaf_score_pooled(vmaf_ctx, vmaf_model, VMAF_POOL_METHOD_MEAN, &score, 0, 0);
    if (res != 0) {
        BOOST_LOG(error) << "[VMAF] vmaf_score_at_index failed"  << " err=" << res;
        vmaf_close(vmaf_ctx);
        return false;
    }
    *out_score = score;
    vmaf_close(vmaf_ctx);
    return true;
}

AVFrame* VmafHandler::make_ref_for_vmaf(const AVFrame* in, int w, int h) {
    if (!in) return nullptr;

    const AVFrame* src = in;
    AVFrame* tmp_sw = nullptr;

    // 1) GPU → SW
    if (in->format == AV_PIX_FMT_CUDA) {
        AVPixelFormat swfmt = AV_PIX_FMT_NV12;
        if (enc_ctx && enc_ctx->hw_frames_ctx) {
            auto* fctx = (AVHWFramesContext*)enc_ctx->hw_frames_ctx->data;
            if (fctx && fctx->sw_format != AV_PIX_FMT_NONE)
                swfmt = (AVPixelFormat)fctx->sw_format;
        }

        tmp_sw = av_frame_alloc();
        if (!tmp_sw) return nullptr;
        tmp_sw->format = swfmt;
        tmp_sw->width  = in->width;
        tmp_sw->height = in->height;

        if (av_frame_get_buffer(tmp_sw, 32) < 0) {
            av_frame_free(&tmp_sw);
            return nullptr;
        }
        if (av_hwframe_transfer_data(tmp_sw, in, 0) < 0) {
            av_frame_free(&tmp_sw);
            return nullptr;
        }
        tmp_sw->pts = in->pts;
        src = tmp_sw; // 이제 SW 프레임
    }

    const bool same_size = (src->width == w && src->height == h);
    const AVPixelFormat src_fmt = (AVPixelFormat)src->format;

    // 2) 해상도 동일 + 이미 YUV420P면 스케일/컨버전 불필요 → 참조 복제
    if (same_size && src_fmt == AV_PIX_FMT_YUV420P) {
        AVFrame* clone = av_frame_clone(src); // ref-count 복제
        if (tmp_sw) av_frame_free(&tmp_sw);
        return clone;
    }

    // 3) 변환이 필요한 경우에만 sws 사용
    AVFrame* dst = av_frame_alloc();
    if (!dst) { if (tmp_sw) av_frame_free(&tmp_sw); return nullptr; }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = w;
    dst->height = h;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        if (tmp_sw) av_frame_free(&tmp_sw);
        return nullptr;
    }

    // 동일 해상도면 포맷 변환만 → 빠른 스케일러 사용
    int flags = same_size ? SWS_FAST_BILINEAR : SWS_BICUBIC;

    SwsContext* sws = sws_getContext(
        src->width, src->height, src_fmt,
        w, h, AV_PIX_FMT_YUV420P, flags, nullptr, nullptr, nullptr);
    if (!sws) {
        av_frame_free(&dst);
        if (tmp_sw) av_frame_free(&tmp_sw);
        return nullptr;
    }

    sws_scale(sws, src->data, src->linesize, 0, src->height,
              dst->data, dst->linesize);
    sws_freeContext(sws);

    if (tmp_sw) av_frame_free(&tmp_sw);

    dst->pts = in->pts;
    return dst;
}

double VmafHandler::get_score(AVFrame* frame, AVPacket* pkt)
{

    std::lock_guard<std::mutex> lock_guard(mtx_);
    int nal_type = hevc_first_vcl_nal_type_from_packet(pkt); 
    BOOST_LOG(error) << "[VMAF]" << " nal=" << nal_type << "(" << hevc_slice_kind(nal_type) << ")";

    double score = 0.0;

    if (avcodec_send_packet(dec_ctx, pkt) >= 0)
    {
        AVFrame* dist = av_frame_alloc();
        if (avcodec_receive_frame(dec_ctx, dist) == 0)
        {
            AVFrame* dist_ref = make_ref_for_vmaf(dist, enc_ctx->width, enc_ctx->height);
            double s = 0.0;
            if (eval_frame(frame, dist_ref, &s)) {
                score = s;
            } else {
                BOOST_LOG(debug) << "[VMAF] eval frame fail";
            }
            av_frame_free(&dist_ref);
        }
        else {
            BOOST_LOG(debug) << "[VMAF] frame receive fail";
        }
        av_frame_free(&dist);
    }
    else {
        BOOST_LOG(debug) << "[VMAF] packet send fail";
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return score;
}