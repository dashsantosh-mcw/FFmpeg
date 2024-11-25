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
    // ID3D11Texture2D** texturePool;
    // ID3D11VideoProcessorOutputView** outputViews; 
    AVBufferRef* hw_device_ctx;
    AVCodecContext* hw_frames_ctx;
    void *priv;
    int width, height;
int inputWidth, inputHeight;
} D3D11ScaleContext;
// Forward declaration
// int create_texture_pool_and_views(D3D11ScaleContext *s, AVFilterContext *ctx);

// static int d3d11scale_init(AVFilterContext* ctx) {
//     av_log(ctx, AV_LOG_VERBOSE, "D3D11 INIT called!!!!!!!!!\n");
//     D3D11ScaleContext* s = ctx->priv;
//     HRESULT hr;
//     s->d3d_dll = LoadLibrary("D3D11.dll");
//     if (!s->d3d_dll) {
//         av_log(ctx, AV_LOG_ERROR, "Failed to load D3D11.dll\n");
//         return AVERROR_EXTERNAL;
//     }
//     int err = 0;

//     if ((err = av_hwdevice_ctx_create(&s->hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA,
//                                       NULL, NULL, 0)) < 0) {
//         fprintf(stderr, "Failed to create specified HW device.\n");
//         return err;
//     }
//     av_log(ctx, AV_LOG_INFO, "Filter Device: %p, Context: %p\n", s->device, s->context);
//     ctx->hw_device_ctx = av_buffer_ref(s->hw_device_ctx);
//     if (!ctx->hw_device_ctx) {
//     av_log(ctx, AV_LOG_ERROR, "Failed to create buffer reference for D3D11 hardware device.\n");
//     return AVERROR(ENOMEM);
//     }
//     av_log(ctx, AV_LOG_VERBOSE, "D3D11 device created\n");
//     return 0;
// }
static int d3d11scale_init(AVFilterContext* ctx) {
    D3D11ScaleContext* s = ctx->priv;

    // if (!ctx->inputs[0]->hw_frames_ctx) {
    //     av_log(ctx, AV_LOG_ERROR, "No hardware frames context provided on input.\n");
    //     return AVERROR(EINVAL);
    // }

    // AVHWFramesContext *frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;
    // s->hw_device_ctx = av_buffer_ref(frames_ctx->device_ref);
    // if (!s->hw_device_ctx) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to reference hardware device context from input.\n");
    //     return AVERROR(ENOMEM);
    // }

    // AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    // AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
    // s->device = (ID3D11Device *)d3d11_hwctx->device;
    // s->context = d3d11_hwctx->device_context;
    return 0;
}


static int d3d11scale_configure_processor(D3D11ScaleContext *s, AVFilterContext *ctx) {
    HRESULT hr;

    // Get D3D11 device and context from hardware device context
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
    s->device = (ID3D11Device *)d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    av_log(ctx, AV_LOG_VERBOSE, "Configuring D3D11 video processor.\n");

    // Define the video processor content description
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputWidth = s->inputWidth,
        .InputHeight = s->inputHeight,
        .OutputWidth = s->width,
        .OutputHeight = s->height,
        .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    };

    // Query video device interface
    hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice, (void **)&s->videoDevice);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get D3D11 video device interface: HRESULT 0x%lX.\n", hr);
        return AVERROR_EXTERNAL;
    }

    // Create video processor enumerator
    hr = s->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(s->videoDevice, &contentDesc, &s->enumerator);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX.\n", hr);
        return AVERROR_EXTERNAL;
    }

    // Create the video processor
    hr = s->videoDevice->lpVtbl->CreateVideoProcessor(s->videoDevice, s->enumerator, 0, &s->processor);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX.\n", hr);
        return AVERROR_EXTERNAL;
    }
    // Call create_texture_pool_and_views to initialize textures and views
    // int ret = create_texture_pool_and_views(s, ctx);
    // if (ret < 0) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to create texture pool and output views.\n");
    //     return ret;
    // }

    // av_log(ctx, AV_LOG_VERBOSE, "D3D11 video processor successfully configured.\n");
    // return 0;
    // Create the output texture
    D3D11_TEXTURE2D_DESC textureDesc = {
        .Width = s->width,
        .Height = s->height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_NV12,
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER,
    };

    hr = s->device->lpVtbl->CreateTexture2D(s->device, &textureDesc, NULL, &s->d3d11_vp_output_texture);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create output texture: HRESULT 0x%lX.\n", hr);
        return AVERROR_EXTERNAL;
    }

    // Create the output view
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {
        .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 },
    };

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
        s->videoDevice, (ID3D11Resource *)s->d3d11_vp_output_texture, s->enumerator, &outputViewDesc, &s->outputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor output view: HRESULT 0x%lX.\n", hr);
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 video processor successfully configured.\n");
    return 0;
}

static int d3d11scale_filter_frame(AVFilterLink* inlink, AVFrame* in) 
{
    AVFilterContext *ctx = inlink->dst;
    D3D11ScaleContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11VideoProcessorInputView *inputView = NULL;
    ID3D11VideoContext *videoContext = NULL;
    AVFrame *out = NULL;
    int ret;

    // av_log(ctx, AV_LOG_INFO, "Inside Filter_frame function!\n");

    // Validate input hw_frames_ctx
    if (!in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hardware frames context available in input frame.\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }
// AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
    // Reference the input hardware frames context
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)in->hw_frames_ctx->data;
    // av_log(ctx, AV_LOG_VERBOSE, "Pool size inside filter frame %d.\n", frames_ctx->initial_pool_size);

    // Validate that the filter's hardware device context has been initialized
    if (!s->hw_device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Filter hardware device context is uninitialized. Ensure config_props has been called.\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    // Validate hardware device compatibility
    AVHWDeviceContext *input_device_ctx = (AVHWDeviceContext *)frames_ctx->device_ref->data;
    AVHWDeviceContext *filter_device_ctx = (AVHWDeviceContext *)s->hw_device_ctx->data;

    if (input_device_ctx->type != filter_device_ctx->type) {
        av_log(ctx, AV_LOG_ERROR, "Mismatch between input and filter hardware device types.\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    // av_log(ctx, AV_LOG_VERBOSE, "Hardware context validation succeeded.\n");

    // FOR SOME REASON THE BELOW CHECK FAILS, NOT SURE BECAUSE OF WHICH WE ARE FACING MEMORY ISSUES
    // Verify the device matches the filter's context
    // if (frames_ctx->device_ref != s->hw_device_ctx) {
    //     av_log(ctx, AV_LOG_ERROR, "Mismatch between input frame and filter hardware device contexts.\n");
    //     av_frame_free(&in);
    //     return AVERROR(EINVAL);
    // }
    // int allocated_surfaces = frames_ctx->initial_pool_size;
    // int used_surfaces = 0;

    // if (frames_ctx->pool) {
    //     // Use buffer pool ref count as a proxy for usage
    //     used_surfaces = av_buffer_get_ref_count(frames_ctx->pool);
    // }

    // av_log(ctx, AV_LOG_VERBOSE, "Current pool usage: %d/%d\n", used_surfaces, allocated_surfaces);

    // // Proceed with filter processing
    // av_log(ctx, AV_LOG_VERBOSE, "Input and filter contexts share the same hardware device.\n");  
    
    // Lock the hardware context for processing
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)filter_device_ctx->hwctx;
    d3d11_hwctx->lock(d3d11_hwctx->lock_ctx); 
    s->inputWidth = in->width;
    s->inputHeight = in->height;

    // av_log(ctx, AV_LOG_VERBOSE, "Captured input dimensions: %dx%d\n", s->inputWidth, s->inputHeight);

    // Allocate output frame
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate output frame.\n");
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    // Configure the D3D11 video processor
    if (!s->processor) {
        if (d3d11scale_configure_processor(s, ctx) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to configure processor\n");
            av_frame_free(&in);
            av_frame_free(&out);
            return AVERROR_EXTERNAL;
        }
    }

    ID3D11Texture2D *d3d11_texture = (ID3D11Texture2D *)in->data[0];
    int subIdx = (int)(intptr_t)in->data[1];

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {
        .FourCC = DXGI_FORMAT_NV12,
        .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
        .Texture2D.ArraySlice = subIdx
    };

    HRESULT hr = s->videoDevice->lpVtbl->CreateVideoProcessorInputView(
        s->videoDevice, d3d11_texture, s->enumerator, &inputViewDesc, &inputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create input view: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }
//  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {D3D11_VPOV_DIMENSION_TEXTURE2D};
//     outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
//     outputViewDesc.Texture2D.MipSlice = 0;

//     D3D11_TEXTURE2D_DESC opTexDesc = { 0 };
//     opTexDesc.Width = s->width;
//     opTexDesc.Height = s->height;
//     opTexDesc.MipLevels = 1;
//     opTexDesc.ArraySize = 1;
//     opTexDesc.Format = DXGI_FORMAT_NV12;
//     opTexDesc.SampleDesc.Count = 1;
//     opTexDesc.Usage = D3D11_USAGE_DEFAULT;
//     opTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
//     opTexDesc.MiscFlags = 0;

//     hr = s->device->lpVtbl->CreateTexture2D(s->device, &opTexDesc, NULL, &outputTexture);
//         if (FAILED(hr)) {
//         av_log(ctx, AV_LOG_ERROR, "Failed to create Texture2D : HRESULT 0x%lX\n", hr);
//         return AVERROR_EXTERNAL;
//     }

//     hr = s->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
//         s->videoDevice, (ID3D11Resource*)outputTexture, s->enumerator, &outputViewDesc, &outputView);
//     if (FAILED(hr)) {
//         av_log(ctx, AV_LOG_ERROR, "Failed to create output view: HRESULT 0x%lX\n", hr);
//         return AVERROR_EXTERNAL;
//     }

   D3D11_VIDEO_PROCESSOR_STREAM stream = {
        .Enable = TRUE,
        .pInputSurface = inputView,
        .OutputIndex = 0
    };

    s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext, (void**)&videoContext);

    hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor, s->outputView, 0, 1, &stream);
    // av_log(ctx, AV_LOG_VERBOSE, "After VideoProcessorBlt function!!!!!!!!\n");
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
        av_frame_free(&in);
        av_frame_free(&out);
        return AVERROR_EXTERNAL;
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0){
        av_log(ctx, AV_LOG_ERROR, "Failed to copy frame properties\n");
        return ret;
    }

    out->data[0] = (uint8_t *)s->d3d11_vp_output_texture;
    out->data[1]= (uint8_t *)(intptr_t)0;
    out->width = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;

    // av_log(ctx, AV_LOG_VERBOSE, "Input dimensions: %dx%d\n", s->inputWidth, s->inputHeight);
    // av_log(ctx, AV_LOG_VERBOSE, "Output dimensions: %dx%d\n", s->width, s->height);
    // av_log(ctx, AV_LOG_VERBOSE, "Output pixel format: %s\n", av_get_pix_fmt_name(out->format));
    av_frame_free(&in);
    // av_log(ctx, AV_LOG_VERBOSE, "Out Frame format: %s, width: %d, height: %d, data: %p, data1: %p\n", av_get_pix_fmt_name(out->format), out->width, out->height, out->data[0], out->data[1]);
    // av_log(ctx, AV_LOG_VERBOSE, "Exiting d3d11scale_filter_frame function!!!!!!!!\n");
    inputView->lpVtbl->Release(inputView);
    videoContext->lpVtbl->Release(videoContext);
    d3d11_hwctx->unlock(d3d11_hwctx->lock_ctx);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int d3d11scale_config_props(AVFilterLink* outlink) 
{
    AVFilterContext *ctx = outlink->src;
    D3D11ScaleContext *s = ctx->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);

    // av_log(ctx, AV_LOG_VERBOSE, "Configuring output properties\n");

    // Evaluate output dimensions
    int ret = ff_scale_eval_dimensions(s, s->w_expr, s->h_expr, inlink, outlink, &s->width, &s->height);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to evaluate dimensions.\n");
        return AVERROR(EINVAL);
    }

    // av_log(ctx, AV_LOG_VERBOSE, "Evaluated dimensions: width=%d, height=%d\n", s->width, s->height);
    outlink->w = s->width;
    outlink->h = s->height;

    // Validate input hw_frames_ctx
    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw_frames_ctx available on input link.\n");
        return AVERROR(EINVAL);
    }

    // Propagate hw_frames_ctx to output
    outl->hw_frames_ctx = av_buffer_ref(inl->hw_frames_ctx);
    if (!outl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Failed to propagate hw_frames_ctx from input to output.\n");
        return AVERROR(ENOMEM);
    }

    // Initialize filter's hardware device context
    if (!s->hw_device_ctx) {
        AVHWFramesContext *in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
        // in_frames_ctx->initial_pool_size += 6;
        av_log(ctx, AV_LOG_VERBOSE, "Input frame pool size: %d\n!!!!!", in_frames_ctx->initial_pool_size); // in_frames_ctx->initial_pool_size
        s->hw_device_ctx = av_buffer_ref(in_frames_ctx->device_ref);
        if (!s->hw_device_ctx) {
            av_log(ctx, AV_LOG_ERROR, "Failed to initialize filter hardware device context.\n");
            return AVERROR(ENOMEM);
        }
        av_log(ctx, AV_LOG_VERBOSE, "Filter hardware device context initialized: %p\n", s->hw_device_ctx);
    }

    // Initialize D3D11 device and context
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;

    s->device = (ID3D11Device *)d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    if (!s->device || !s->context) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialize filter device or context in config_props.\n");
        return AVERROR(EINVAL);
    }

    // av_log(ctx, AV_LOG_WARNING, "Filter device initialized: %p, Filter context: %p\n", s->device, s->context);
    // av_log(ctx, AV_LOG_VERBOSE, "D3D11 output properties configured successfully.\n");
    return 0;
}

static void d3d11scale_uninit(AVFilterContext* ctx) {
    av_log(ctx, AV_LOG_VERBOSE, "Uninitializing D3D11 scale filter\n");
    D3D11ScaleContext* s = ctx->priv;
    if (s->outputView) s->outputView->lpVtbl->Release(s->outputView);
    // if (s->d3d11_vp_output_texture) s->d3d11_vp_output_texture->lpVtbl->Release(s->d3d11_vp_output_texture);
    if (s->inputView) s->inputView->lpVtbl->Release(s->inputView);
    // if (s->processor) s->processor->lpVtbl->Release(s->processor);
    // if (s->enumerator) s->enumerator->lpVtbl->Release(s->enumerator); //If not commented will crash/fail not sure why, can't open output for 100+ frames, bbut works for 10 frames!
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
    .flags     = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};