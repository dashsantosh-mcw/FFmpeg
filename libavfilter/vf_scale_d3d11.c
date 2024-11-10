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

typedef struct D3D11ScaleContext {
    const AVClass* classCtx;
    HMODULE d3d_dll;
    char *w_expr;
    char *h_expr;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11VideoProcessor* processor;
    ID3D11VideoProcessorEnumerator* enumerator;
    ID3D11VideoProcessorOutputView* outputView;
    ID3D11VideoProcessorInputView* inputView;
    ID3D11Texture2D* d3d11_vp_output_texture;
    ID3D11VideoDevice* videoDevice;
    AVBufferRef* hw_frames_ctx;
    void *priv;
    int width, height;
    int inputWidth, inputHeight;
} D3D11ScaleContext;

static int d3d11scale_init(AVFilterContext* ctx) {
    D3D11ScaleContext* s = ctx->priv;
    HRESULT hr;
    s->d3d_dll = LoadLibrary("D3D11.dll");
    if (!s->d3d_dll) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load D3D11.dll\n");
        return AVERROR_EXTERNAL;
    }
    HRESULT (WINAPI *pD3D11CreateDevice)(
            IDXGIAdapter *pAdapter,
            D3D_DRIVER_TYPE DriverType,
            HMODULE Software,
            UINT Flags,
            const D3D_FEATURE_LEVEL *pFeatureLevels,
            UINT FeatureLevels,
            UINT SDKVersion,
            ID3D11Device **ppDevice,
            D3D_FEATURE_LEVEL *pFeatureLevel,
            ID3D11DeviceContext **ppImmediateContext
        );

    pD3D11CreateDevice = (void *)GetProcAddress(s->d3d_dll, "D3D11CreateDevice");
    if (!pD3D11CreateDevice)
        return AVERROR_EXTERNAL;

    hr = pD3D11CreateDevice(
        0,
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
        av_log(ctx, AV_LOG_ERROR, "Failed to create D3D device\n");
        return AVERROR_EXTERNAL;
    }
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 device created\n");
    return 0;
}

static int d3d11scale_configure_processor(D3D11ScaleContext* s, AVFilterContext* ctx) {
    
    av_log(ctx, AV_LOG_VERBOSE, "INSIDE d3d11scale_configure_processor!!!!!!!!!\n");
    HRESULT hr;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = s->inputWidth;
    contentDesc.InputHeight = s->inputHeight;
    contentDesc.OutputWidth = s->width;
    contentDesc.OutputHeight = s->height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    av_log(ctx, AV_LOG_VERBOSE, "D3D11_VIDEO_PROCESSOR_CONTENT_DESC: %d %d %d %d\n", s->inputHeight, s->inputWidth, s->height, s->width);
    hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice, (void**)&s->videoDevice);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get video device interface.\n");
        return AVERROR_EXTERNAL;
    }

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(s->videoDevice, &contentDesc, &s->enumerator);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->videoDevice->lpVtbl->CreateVideoProcessor(s->videoDevice, s->enumerator, 0, &s->processor);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE, "Video processor created\n");

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {D3D11_VPOV_DIMENSION_TEXTURE2D};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    D3D11_TEXTURE2D_DESC opTexDesc = { 0 };
    opTexDesc.Width = s->width;
    opTexDesc.Height = s->height;
    opTexDesc.MipLevels = 1;
    opTexDesc.ArraySize = 1;
    opTexDesc.Format = DXGI_FORMAT_NV12;
    opTexDesc.SampleDesc.Count = 1;
    opTexDesc.Usage = D3D11_USAGE_DEFAULT;
    opTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    //  | D3D11_BIND_VIDEO_ENCODER;
    opTexDesc.MiscFlags = 0;

    hr = s->device->lpVtbl->CreateTexture2D(s->device, &opTexDesc, NULL, &s->d3d11_vp_output_texture);
        if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create Texture2D : HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
        s->videoDevice, (ID3D11Resource*)s->d3d11_vp_output_texture, s->enumerator, &outputViewDesc, &s->outputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create output view: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE, "Video processor configured\n");
    return 0;
}

static int d3d11scale_filter_frame(AVFilterLink* inlink, AVFrame* in) {
    
    AVFilterContext* ctx = inlink->dst;
    D3D11ScaleContext* s = ctx->priv;
    ID3D11Texture2D *d3d11_texture = NULL;
    av_log(ctx, AV_LOG_VERBOSE, "INSIDE d3d11scale_filter_frame!!!!!!!!!\n");
    s->inputWidth = in->width;
    s->inputHeight = in->height;
    AVFilterLink *outlink = ctx->outputs[0];

    if (!s->processor) {
        if (d3d11scale_configure_processor(s, ctx) < 0) {
            return AVERROR_EXTERNAL;
        }
    }

    // AVFrame *out = NULL; 
    // out = av_frame_alloc();
    // if (!out) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to allocate output frame\n");
    //     return AVERROR(ENOMEM);
    // }
    AVFrame* out = ff_get_video_buffer(inlink->dst->outputs[0], s->width, s->height);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ID3D11VideoProcessorInputView* inputView = NULL;

    d3d11_texture = (ID3D11Texture2D *)in->data[0];
    // subIdx = (int)(intptr_t)frame->data[1];
    int subIdx = (int)(intptr_t)in->data[1];

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
    inputViewDesc.FourCC = DXGI_FORMAT_NV12;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.ArraySlice = subIdx;
    inputViewDesc.Texture2D.MipSlice = 0;
    HRESULT hr = s->videoDevice->lpVtbl->CreateVideoProcessorInputView(
        s->videoDevice, d3d11_texture, s->enumerator, &inputViewDesc, &inputView);
        //(ID3D11Resource*)in->data[0]
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create input view: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView;
    stream.OutputIndex = 0;

    ID3D11VideoContext* videoContext = NULL;
    s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext, (void**)&videoContext);

    hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor, s->outputView, 0, 1, &stream);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
        av_frame_free(&in);
        av_frame_free(&out);
        return AVERROR_EXTERNAL;
    }


    // Code to write the frame
    D3D11_TEXTURE2D_DESC stagingDesc = { 0 };
    stagingDesc.Width = s->width;
    stagingDesc.Height = s->height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_NV12; 
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *stagingTexture = NULL;
    hr = s->device->lpVtbl->CreateTexture2D(s->device, &stagingDesc, NULL, &stagingTexture);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create staging texture\n");
        return AVERROR_EXTERNAL;
    }

    // Copy resource
    s->context->lpVtbl->CopyResource(s->context, stagingTexture, s->d3d11_vp_output_texture);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = s->context->lpVtbl->Map(s->context, stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
    // if (SUCCEEDED(hr)) {
    //     // Analyze data to confirm non-black values
    //     uint8_t *data = (uint8_t *)mappedResource.pData;
    //     int nonZeroPixelCount = 0;
    //     for (int y = 0; y < s->height; y++) {
    //         for (int x = 0; x < mappedResource.RowPitch; x++) {
    //             if (data[y * mappedResource.RowPitch + x] != 0) {
    //                 nonZeroPixelCount++;
    //                 break;
    //             }
    //         }
    //     }
    //     av_log(ctx, AV_LOG_VERBOSE, "Non-zero pixels in the frame: %d\n", nonZeroPixelCount);

    //     s->context->lpVtbl->Unmap(s->context, stagingTexture, 0);
    // }


    // Access the data and write it to a file
    // mappedResource.pData points to the pixel data
    // mappedResource.RowPitch is the row size in bytes
    //     FILE *file = fopen("output_frame.raw", "wb");
    // if (file) {
    //     for (int y = 0; y < s->height; y++) {
    //         fwrite((uint8_t *)mappedResource.pData + y * mappedResource.RowPitch, 1, s->width, file);
    //     }
    //     fclose(file);
    // }

    FILE *file = fopen("output_frame.raw", "wb");
    if (file) {
        // Write Y plane
        for (int y = 0; y < s->height; y++) {
            fwrite((uint8_t *)mappedResource.pData + y * mappedResource.RowPitch, 1, s->width, file);
        }
        // Write UV plane (height / 2 because UV is subsampled)
        for (int y = 0; y < s->height / 2; y++) {
            fwrite((uint8_t *)mappedResource.pData + s->height * mappedResource.RowPitch + y * mappedResource.RowPitch, 1, s->width, file);
        }
        fclose(file);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Failed to open output_frame.raw\n");
    }
    // Unmap and release resources
    s->context->lpVtbl->Unmap(s->context, stagingTexture, 0);
    stagingTexture->lpVtbl->Release(stagingTexture);

    out->data[0] = &s->d3d11_vp_output_texture;
    out->data[1] = 0;
    out->width = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;
    out->hw_frames_ctx = av_buffer_ref(in->hw_frames_ctx);

    av_log(ctx, AV_LOG_VERBOSE, "Input dimensions: %dx%d\n", s->inputWidth, s->inputHeight);
    av_log(ctx, AV_LOG_VERBOSE, "Output dimensions: %dx%d\n", s->width, s->height);
    av_log(ctx, AV_LOG_VERBOSE, "Output pixel format: %s\n", av_get_pix_fmt_name(out->format));

    av_frame_copy_props(out, in);
    av_log(ctx, AV_LOG_VERBOSE, "Frame filtered\n");
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int d3d11scale_config_props(AVFilterLink* outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    FilterLink      *inl = ff_filter_link(inlink);
    D3D11ScaleContext *s  = ctx->priv;

    av_log(ctx, AV_LOG_VERBOSE, "Configuring output properties\n");
    int ret;

    if ((ret = ff_scale_eval_dimensions(s,
                                        s->w_expr, s->h_expr,
                                        inlink, outlink,
                                        &s->width, &s->height)) < 0)
        return AVERROR(EINVAL);

    outlink->w = s->width;
    outlink->h = s->height;

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 output properties configured successfully\n");

    return 0;
}



static void d3d11scale_uninit(AVFilterContext* ctx) {
    D3D11ScaleContext* s = ctx->priv;
    if (s->outputView) s->outputView->lpVtbl->Release(s->outputView);
    if (s->inputView) s->inputView->lpVtbl->Release(s->inputView);
    if (s->processor) s->processor->lpVtbl->Release(s->processor);
    if (s->enumerator) s->enumerator->lpVtbl->Release(s->enumerator);
    if (s->context) s->context->lpVtbl->Release(s->context);
    if (s->device) s->device->lpVtbl->Release(s->device);
    if (s->videoDevice) s->videoDevice->lpVtbl->Release(s->videoDevice);
    return;
}

static const AVFilterPad d3d11scale_inputs[] = {
    { "default", AVMEDIA_TYPE_VIDEO, .filter_frame = d3d11scale_filter_frame },
 
};

static const AVFilterPad d3d11scale_outputs[] = {
    { "default", AVMEDIA_TYPE_VIDEO, .config_props = d3d11scale_config_props },
};

#define OFFSET(x) offsetof(D3D11ScaleContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption d3d11scale_options[] = {
    { "width", "Output video width",
            OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "height", "Output video height",
            OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
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
    .description = NULL_IF_CONFIG_SMALL("Scale video using Direct3D11"),
    .priv_size = sizeof(D3D11ScaleContext),
    .priv_class = &d3d11scale_class,
    .init      = d3d11scale_init,
    .uninit    = d3d11scale_uninit,
    FILTER_INPUTS(d3d11scale_inputs),
    FILTER_OUTPUTS(d3d11scale_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    // .flags     = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
