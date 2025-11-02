#pragma once

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libvmaf/libvmaf.h>
    #include <libvmaf/model.h>
    #include <libvmaf/picture.h>
    #include <libvmaf/feature.h>
    #include <libavutil/opt.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/intreadwrite.h>
    #include <libswscale/swscale.h>
}
#include <mutex>
#include <thread>

class VmafHandler
{
    private:
        const char* model_path = "/playout-multiview-generator/assets/vmaf_4k_v0.6.1.json";
        uint64_t     index_{0};
        std::mutex mtx_;

        AVCodecContext* enc_ctx = nullptr;
        AVCodecContext* dec_ctx = nullptr;

        VmafModel* vmaf_model{nullptr};

        int alloc_and_copy(const AVFrame* f, VmafPicture* pic);

    public:
        bool init(AVCodecContext* enc_ctx);
        AVFrame* make_ref_for_vmaf(const AVFrame* in, int w, int h);
        bool eval_frame(const AVFrame* ref, const AVFrame* dist, double* out_score);
        double get_score(AVFrame* frame, AVPacket* packet);

        ~VmafHandler();
        VmafHandler();
};