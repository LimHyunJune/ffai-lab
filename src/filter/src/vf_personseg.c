#include <libavfilter/avfilter.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include "personseg_ort.h"
#include <string.h>
#include <math.h>

typedef struct PersonSegContext {
    const AVClass *class;

    /* options */
    char *model;     /* onnx 경로 */
    int   in_w;      /* 모델 입력 W */
    int   in_h;      /* 모델 입력 H */
    float thr;       /* 0..1 threshold */
    int   threads;   /* ORT intra-op threads */

    /* runtime */
    struct SwsContext *sws_to_rgb; /* input->RGB(in_w x in_h) */
    struct SwsContext *sws_up;     /* mask(in_w x in_h)->input size */
    AVFrame *rgb;                  /* RGB24(in_w x in_h) */
    float   *nchw;                 /* 1x3xH x W float */
    float   *mask01;               /* H x W float */
    AVFrame *mask_small;           /* GRAY8(HxW) */
    AVFrame *mask_big;             /* GRAY8(in->W x in->H) */
    PersonSegORT* ort;
} PersonSegContext;

#define OFFSET(x) offsetof(PersonSegContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption personseg_options[] = {
    { "model",   "onnx model path",        OFFSET(model),   AV_OPT_TYPE_STRING, {.str=NULL}, 0,0, FLAGS },
    { "w",       "model input width",      OFFSET(in_w),    AV_OPT_TYPE_INT,    {.i64=320},  16, 4096, FLAGS },
    { "h",       "model input height",     OFFSET(in_h),    AV_OPT_TYPE_INT,    {.i64=320},  16, 4096, FLAGS },
    { "thr",     "mask threshold [0..1]",  OFFSET(thr),     AV_OPT_TYPE_FLOAT,  {.dbl=0.5},  0.0, 1.0,  FLAGS },
    { "threads", "ORT intra-op threads",   OFFSET(threads), AV_OPT_TYPE_INT,    {.i64=1},    1,   16,   FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(personseg);

static int query_formats(AVFilterContext *ctx)
{
    /* 처음엔 YUV420P만 지원 → 필요시 확장 */
    static const enum AVPixelFormat fmts[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE
    };
    return ff_set_common_formats_from_list(ctx, fmts);
}

static av_cold int init(AVFilterContext *ctx)
{
    PersonSegContext *s = ctx->priv;
    if (!s->model) {
        av_log(ctx, AV_LOG_ERROR, "model= is required\n");
        return AVERROR(EINVAL);
    }
    s->ort = personseg_create(s->model, s->in_w, s->in_h, s->threads);
    if (!s->ort) {
        av_log(ctx, AV_LOG_ERROR, "failed to create onnxruntime session\n");
        return AVERROR_EXTERNAL;
    }
    s->nchw   = av_malloc_array((size_t)1 * 3 * s->in_h * s->in_w, sizeof(float));
    s->mask01 = av_malloc_array((size_t)s->in_h * s->in_w, sizeof(float));
    if (!s->nchw || !s->mask01) return AVERROR(ENOMEM);

    /* 마스크 프레임들 초기화는 config_props에서 크기 결정 후 할당 */
    return 0;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    PersonSegContext *s   = ctx->priv;
    AVFilterLink *inlink  = ctx->inputs[0];

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->time_base           = inlink->time_base;

    /* RGB 버퍼 (모델 입력크기) */
    s->rgb = av_frame_alloc();
    if (!s->rgb) return AVERROR(ENOMEM);
    s->rgb->format = AV_PIX_FMT_RGB24;
    s->rgb->width  = s->in_w;
    s->rgb->height = s->in_h;
    if (av_frame_get_buffer(s->rgb, 32) < 0) return AVERROR(ENOMEM);

    /* input → RGB(in_w x in_h) */
    s->sws_to_rgb = sws_getContext(inlink->w, inlink->h, inlink->format,
                                   s->in_w, s->in_h, AV_PIX_FMT_RGB24,
                                   SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->sws_to_rgb) return AVERROR(EINVAL);

    /* mask(HxW GRAY8) → input size GRAY8 */
    s->sws_up = sws_getContext(s->in_w, s->in_h, AV_PIX_FMT_GRAY8,
                               inlink->w, inlink->h, AV_PIX_FMT_GRAY8,
                               SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->sws_up) return AVERROR(EINVAL);

    /* 마스크 프레임들 */
    s->mask_small = av_frame_alloc();
    s->mask_big   = av_frame_alloc();
    if (!s->mask_small || !s->mask_big) return AVERROR(ENOMEM);

    s->mask_small->format = AV_PIX_FMT_GRAY8; s->mask_small->width = s->in_w; s->mask_small->height = s->in_h;
    s->mask_big->format   = AV_PIX_FMT_GRAY8; s->mask_big->width   = inlink->w; s->mask_big->height  = inlink->h;

    if (av_frame_get_buffer(s->mask_small, 32) < 0) return AVERROR(ENOMEM);
    if (av_frame_get_buffer(s->mask_big,   32) < 0) return AVERROR(ENOMEM);

    return 0;
}

static void apply_mask_yuv420p(const AVFrame *src, AVFrame *dst,
                               const AVFrame *mask_u8, float thr)
{
    /* Y plane: mask<thr → 0(검정), >=thr → 원본 Y */
    const int W = src->width, H = src->height;
    for (int y = 0; y < H; ++y) {
        const uint8_t* sy = src->data[0] + (size_t)y * src->linesize[0];
        uint8_t*       dy = dst->data[0] + (size_t)y * dst->linesize[0];
        const uint8_t* my = mask_u8->data[0] + (size_t)y * mask_u8->linesize[0];
        const uint8_t  t  = (uint8_t)lrintf(thr * 255.f);
        for (int x = 0; x < W; ++x) dy[x] = (my[x] >= t) ? sy[x] : 0;
    }

    /* UV: 배경은 128(중성회색). 2x2 블록 평균으로 결정 */
    const int Wc = W / 2, Hc = H / 2;
    for (int y = 0; y < Hc; ++y) {
        const uint8_t* su = src->data[1] + (size_t)y * src->linesize[1];
        const uint8_t* sv = src->data[2] + (size_t)y * src->linesize[2];
        uint8_t*       du = dst->data[1] + (size_t)y * dst->linesize[1];
        uint8_t*       dv = dst->data[2] + (size_t)y * dst->linesize[2];
        for (int x = 0; x < Wc; ++x) {
            int mx = x * 2, my = y * 2;
            int sum = 0;
            sum += mask_u8->data[0][(size_t)my     * mask_u8->linesize[0] + mx    ];
            sum += mask_u8->data[0][(size_t)my     * mask_u8->linesize[0] + mx + 1];
            sum += mask_u8->data[0][(size_t)(my+1) * mask_u8->linesize[0] + mx    ];
            sum += mask_u8->data[0][(size_t)(my+1) * mask_u8->linesize[0] + mx + 1];
            float m = sum / (255.f * 4.f);
            if (m >= thr) { du[x] = su[x]; dv[x] = sv[x]; }
            else          { du[x] = 128;   dv[x] = 128;   }
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    PersonSegContext *s  = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) { av_frame_free(&in); return AVERROR(ENOMEM); }
    av_frame_copy_props(out, in);

    /* 1) input(YUV420P) → RGB(in_w x in_h) */
    const uint8_t* src_data[4] = { in->data[0], in->data[1], in->data[2], NULL };
    int            src_ls[4]   = { in->linesize[0], in->linesize[1], in->linesize[2], 0 };
    uint8_t*       rgb_data[4] = { s->rgb->data[0], NULL, NULL, NULL };
    int            rgb_ls[4]   = { s->rgb->linesize[0], 0, 0, 0 };
    if (sws_scale(s->sws_to_rgb, src_data, src_ls, 0, in->height, rgb_data, rgb_ls) <= 0) {
        av_frame_free(&in); av_frame_free(&out); return AVERROR_EXTERNAL;
    }

    /* 2) RGB24 → NCHW float[0..1] */
    const int W = s->in_w, H = s->in_h;
    float *R = s->nchw, *G = R + W*H, *B = G + W*H;
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = s->rgb->data[0] + (size_t)y * s->rgb->linesize[0];
        for (int x = 0; x < W; ++x) {
            B[y*W + x] = row[3*x + 0] / 255.f;
            G[y*W + x] = row[3*x + 1] / 255.f;
            R[y*W + x] = row[3*x + 2] / 255.f;
        }
    }

    /* 3) ORT 추론 → mask float(HxW) */
    int got = personseg_run(s->ort, s->nchw, 1, 3, H, W, s->mask01);
    if (got <= 0) {
        av_log(ctx, AV_LOG_ERROR, "onnx inference failed\n");
        av_frame_free(&in); av_frame_free(&out);
        return AVERROR_EXTERNAL;
    }

    /* 4) mask float → GRAY8(HxW) -> 업샘플 → GRAY8(input size) */
    for (int y = 0; y < H; ++y) {
        uint8_t* row = s->mask_small->data[0] + (size_t)y * s->mask_small->linesize[0];
        for (int x = 0; x < W; ++x) {
            float v = s->mask01[y*W + x];
            if (v < 0) v = 0; if (v > 1) v = 1;
            row[x] = (uint8_t)lrintf(v * 255.f);
        }
    }
    const uint8_t* ms_d[4] = { s->mask_small->data[0], NULL, NULL, NULL };
    int            ms_ls[4] = { s->mask_small->linesize[0], 0, 0, 0 };
    uint8_t*       mb_d[4] = { s->mask_big->data[0], NULL, NULL, NULL };
    int            mb_ls[4] = { s->mask_big->linesize[0], 0, 0, 0 };
    if (sws_scale(s->sws_up, ms_d, ms_ls, 0, H, mb_d, mb_ls) <= 0) {
        av_frame_free(&in); av_frame_free(&out);
        return AVERROR_EXTERNAL;
    }

    /* 5) 마스크 적용(Y/UV) */
    apply_mask_yuv420p(in, out, s->mask_big, s->thr);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    PersonSegContext *s = ctx->priv;
    if (s->rgb)         av_frame_free(&s->rgb);
    if (s->mask_small)  av_frame_free(&s->mask_small);
    if (s->mask_big)    av_frame_free(&s->mask_big);
    if (s->sws_to_rgb)  sws_freeContext(s->sws_to_rgb);
    if (s->sws_up)      sws_freeContext(s->sws_up);
    if (s->nchw)        av_free(s->nchw);
    if (s->mask01)      av_free(s->mask01);
    if (s->ort)         personseg_destroy(&s->ort);
}

AVFilterPad personseg_inputs[] = {
    { .name="default", .type=AVMEDIA_TYPE_VIDEO, .filter_frame=filter_frame },
};

AVFilterPad personseg_outputs[] = {
    { .name="default", .type=AVMEDIA_TYPE_VIDEO, .config_props=config_props },
};

const AVFilter ff_vf_personseg = {
    .name        = "personseg",
    .description = NULL_IF_CONFIG_SMALL("Person segmentation via ONNX Runtime"),
    .priv_size   = sizeof(PersonSegContext),
    .init        = init,
    .uninit      = uninit,
    FILTER_INPUTS(personseg_inputs),
    FILTER_OUTPUTS(personseg_outputs),
    FILTER_QUERY_FUNC(query_formats),
    .priv_class  = &personseg_create,
    .flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
