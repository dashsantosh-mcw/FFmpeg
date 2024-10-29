#include "libavutil/opt.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/scale_eval.h"
#include "libavutil/pixdesc.h"
#include "video.h"
#include "compat/w32dlfcn.h"
#include "libavcodec/mf_utils.h"

#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif

#if CONFIG_D3D11VA
typedef struct D3D11ScaleContext {
    const AVClass* classCtx;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11VideoProcessor* processor;
    ID3D11VideoProcessorEnumerator* enumerator;
    ID3D11VideoProcessorOutputView* outputView;
    ID3D11VideoProcessorInputView* inputView;
    ID3D11VideoDevice* videoDevice;
    int width, height;
    int inputWidth, inputHeight;
} D3D11ScaleContext;

static int d3d11scale_init(AVFilterContext* ctx) {
    D3D11ScaleContext* s = ctx->priv;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &s->device, &featureLevel, &s->context);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create D3D11 device: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }
    hr = ID3D11Device_QueryInterface(s->device, &IID_ID3D11VideoDevice, (void**)&s->videoDevice);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to query ID3D11VideoDevice interface: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int d3d11scale_config_props(AVFilterLink* outlink) {
    AVFilterContext* ctx = outlink->src;
    D3D11ScaleContext* s = ctx->priv;
    outlink->w = s->width;
    outlink->h = s->height;
    return 0;
}

static int d3d11scale_filter_frame(AVFilterLink* inlink, AVFrame* in) {
    AVFilterContext* ctx = inlink->dst;
    D3D11ScaleContext* s = ctx->priv;
    s->inputWidth = in->width;
    s->inputHeight = in->height;

    if (!s->processor) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = { D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, s->inputWidth, s->inputHeight, s->width, s->height, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL };
        HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(s->videoDevice, &contentDesc, &s->enumerator);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX\n", hr);
            return AVERROR_EXTERNAL;
        }
        hr = ID3D11VideoDevice_CreateVideoProcessor(s->videoDevice, s->enumerator, 0, &s->processor);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX\n", hr);
            return AVERROR_EXTERNAL;
        }
    }

    AVFrame* out = ff_get_video_buffer(inlink->dst->outputs[0], s->width, s->height);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ID3D11VideoContext* videoContext;
    s->context->QueryInterface(&IID_ID3D11VideoContext, (void**)&videoContext);

    D3D11_VIDEO_PROCESSOR_STREAM stream = { TRUE, s->inputView };
    HRESULT hr = videoContext->VideoProcessorBlt(s->processor, s->outputView, 0, 1, &stream);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
        av_frame_free(&in);
        av_frame_free(&out);
        return AVERROR_EXTERNAL;
    }

    av_frame_copy_props(out, in);
    av_frame_free(&in);
    return ff_filter_frame(ctx->outputs[0], out);
}

static const AVFilterPad d3d11scale_inputs[] = {
    { "default", AVMEDIA_TYPE_VIDEO, .filter_frame = d3d11scale_filter_frame },
    { NULL }
};

static const AVFilterPad d3d11scale_outputs[] = {
    { "default", AVMEDIA_TYPE_VIDEO, .config_props = d3d11scale_config_props },
    { NULL }
};

static const AVOption d3d11scale_options[] = {
    { "width", "Output video width", offsetof(D3D11ScaleContext, width), AV_OPT_TYPE_INT, {.i64 = 1920}, 0, INT_MAX, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM },
    { "height", "Output video height", offsetof(D3D11ScaleContext, height), AV_OPT_TYPE_INT, {.i64 = 1080}, 0, INT_MAX, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM },
    { NULL }
};

const AVFilter ff_vf_scale_d3d11 = {
    .name      = "scale_d3d11",
    .description = NULL,
    .priv_size = sizeof(D3D11ScaleContext),
    .priv_class = &d3d11scale_class,
    .init      = d3d11scale_init,
    .uninit    = NULL,
    .inputs    = d3d11scale_inputs,
    .outputs   = d3d11scale_outputs,
    .flags     = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_HWFRAME_AWARE,
};
#endif