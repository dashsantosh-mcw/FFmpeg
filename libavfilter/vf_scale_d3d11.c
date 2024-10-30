#include "libavutil/opt.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/scale_eval.h"
#include "libavutil/pixdesc.h"
#include "video.h"
#include "compat/w32dlfcn.h"
#include "libavcodec/mf_utils.h"
#include <d3d11.h>

#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif

// #if CONFIG_D3D11VA
typedef struct D3D11ScaleContext {
    const AVClass* classCtx;
    HMODULE d3d_dll;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11VideoProcessor* processor;
    ID3D11VideoProcessorEnumerator* enumerator;
    ID3D11VideoProcessorOutputView* outputView;
    ID3D11VideoProcessorInputView* inputView;
    ID3D11VideoDevice* videoDevice;
    void *priv;
    int width, height;
    int inputWidth, inputHeight;
} D3D11ScaleContext;

static int d3d11scale_init(AVFilterContext* ctx) {
    D3D11ScaleContext* s = ctx->priv;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr;
    s->d3d_dll = LoadLibrary("D3D11.dll");
    if (!s->d3d_dll) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load D3D11.dll\n");
        return AVERROR_EXTERNAL;
    }
    HRESULT (WINAPI *pD3D11CreateDevice)(
            _In_opt_        IDXGIAdapter        *pAdapter,
                            D3D_DRIVER_TYPE     DriverType,
                            HMODULE             Software,
                            UINT                Flags,
            _In_opt_  const D3D_FEATURE_LEVEL   *pFeatureLevels,
                            UINT                FeatureLevels,
                            UINT                SDKVersion,
            _Out_opt_       ID3D11Device        **ppDevice,
            _Out_opt_       D3D_FEATURE_LEVEL   *pFeatureLevel,
            _Out_opt_       ID3D11DeviceContext **ppImmediateContext
        );
        HRESULT (WINAPI *pMFCreateDXGIDeviceManager)(
        _Out_ UINT                 *pResetToken,
        _Out_ IMFDXGIDeviceManager **ppDXVAManager
        );
        av_log(ctx, AV_LOG_VERBOSE, "load function...");

        pD3D11CreateDevice = (void *)GetProcAddress(s->d3d_dll, "D3D11CreateDevice");
        if (!pD3D11CreateDevice)
            return AVERROR_EXTERNAL;
        av_log(ctx, AV_LOG_VERBOSE, "call create device...");

        hr = pD3D11CreateDevice(0,
                                D3D_DRIVER_TYPE_HARDWARE,
                                NULL,
                                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                NULL,
                                0,
                                D3D11_SDK_VERSION,
                                &s->device,
                                NULL,
                                &s->context);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "failed to create D3D device \n");
        }
        av_log(ctx, AV_LOG_VERBOSE, "device created \n");

        av_log(ctx, AV_LOG_VERBOSE, "create device manager... \n");

    // hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)c->dxgiManager);
    // if (FAILED(hr)){
    //     av_log(ctx, AV_LOG_ERROR, "failed to set manager: %s\n", ff_hr_str(hr));
    // } else {
    //     av_log(ctx, AV_LOG_VERBOSE, "d3d manager set\n");
    // }

    // hr = ID3D11Device_QueryInterface(s->device, &IID_ID3D11VideoDevice, (void**)&s->videoDevice);
    // if (FAILED(hr)) {
    //     av_log(ctx, AV_LOG_VERBOSE,"Failed to get video device interface.\n");
    //     return AVERROR_EXTERNAL;
    // }

    // // Create D3D11 device and context
    // hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &s->device, &featureLevel, &s->context);
    // if (FAILED(hr)) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to create D3D11 device: HRESULT 0x%lX\n", hr);
    //     return AVERROR_EXTERNAL;
    // }

    // // Query for video device
    // hr = ID3D11Device_QueryInterface(s->device, &IID_ID3D11VideoDevice, (void**)&s->videoDevice);
    // if (FAILED(hr)) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to query ID3D11VideoDevice interface: HRESULT 0x%lX\n", hr);
    //     return AVERROR_EXTERNAL;
    // }
    av_log(ctx, AV_LOG_VERBOSE, "Exiting d3d11scale_init... \n");
    return 0;
}

static int d3d11scale_configure_processor(D3D11ScaleContext* s, AVFilterContext* ctx) {
    HRESULT hr;
    av_log(ctx, AV_LOG_VERBOSE, "inside d3d11scale_configure_processor... \n");
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = { 0 };
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = s->inputWidth;
    contentDesc.InputHeight = s->inputHeight;
    contentDesc.OutputWidth = s->width;
    contentDesc.OutputHeight = s->height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    // Create Video Processor Enumerator
    hr = s->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(s->videoDevice, &contentDesc, &s->enumerator);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }
    av_log(ctx, AV_LOG_VERBOSE, "after CreateVideoProcessorEnumerator... \n");

    // Create Video Processor
    hr = s->videoDevice->lpVtbl->CreateVideoProcessor(s->videoDevice, s->enumerator, 0, &s->processor);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }
    av_log(ctx, AV_LOG_VERBOSE, "after CreateVideoProcessor... \n");
    return 0;
}

static int d3d11scale_filter_frame(AVFilterLink* inlink, AVFrame* in) {
    AVFilterContext* ctx = inlink->dst;
    D3D11ScaleContext* s = ctx->priv;

    av_log(ctx, AV_LOG_VERBOSE, "inside d3d11scale_filter_frame... \n");
    s->inputWidth = in->width;
    s->inputHeight = in->height;

    if (!s->processor) {
        if (d3d11scale_configure_processor(s, ctx) < 0) {
            return AVERROR_EXTERNAL;
        }
    }

    AVFrame* out = ff_get_video_buffer(inlink->dst->outputs[0], s->width, s->height);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ID3D11VideoContext* videoContext = NULL;
    s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext, (void**)&videoContext);

    D3D11_VIDEO_PROCESSOR_STREAM stream = { 0 };
    stream.Enable = TRUE;
    stream.pInputSurface = s->inputView;

    HRESULT hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor, s->outputView, 0, 1, &stream);
    av_log(ctx, AV_LOG_VERBOSE, "after CreateVideoProcessor... \n");
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
static int d3d11scale_config_props(AVFilterLink* outlink) {
    AVFilterContext* ctx = outlink->src;
    D3D11ScaleContext* s = ctx->priv;
    av_log(ctx, AV_LOG_VERBOSE, "inside d3d11scale_config_props... \n");
    outlink->w = s->width;
    outlink->h = s->height;
    return 0;
}

// static int d3d11scale_uninit(AVFilterContext* ctx) {
//     D3D11ScaleContext* s = ctx->priv;
//     if (s->outputView) ID3D11VideoProcessorOutputView_Release(s->outputView);
//     if (s->inputView) ID3D11VideoProcessorInputView_Release(s->inputView);
//     if (s->processor) ID3D11VideoProcessor_Release(s->processor);
//     if (s->enumerator) ID3D11VideoProcessorEnumerator_Release(s->enumerator);
//     if (s->context) ID3D11DeviceContext_Release(s->context);
//     if (s->device) ID3D11Device_Release(s->device);
//     if (s->videoDevice) ID3D11VideoDevice_Release(s->videoDevice);
//     return 0;
// }

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

static const AVClass d3d11scale_class = {
    .class_name = "d3d11scale",
    .item_name  = av_default_item_name,
    .option     = d3d11scale_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const AVFilter ff_vf_scale_d3d11 = {
    .name      = "scale_d3d11",
    .description = NULL,
    .priv_size = sizeof(D3D11ScaleContext),
    .priv_class = &d3d11scale_class,
    .init      = d3d11scale_init,
    .inputs    = d3d11scale_inputs,
    .outputs   = d3d11scale_outputs,
    .flags     = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};

// #endif