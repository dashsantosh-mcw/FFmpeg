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
    AVBufferRef* hw_device_ctx;
    AVFrame* frame;
    AVFrame *tmp_frame;
    AVBufferRef *frames_ctx;
    AVCodecContext* hw_frames_ctx;
    void *priv;
    int width, height;
int inputWidth, inputHeight;
} D3D11ScaleContext;

static av_cold int init_d3d11_hwframe_ctx(D3D11ScaleContext *s, AVBufferRef *device_ctx, int width, int height) {
    AVBufferRef *out_ref = NULL;
    AVHWFramesContext *out_ctx;
    int ret;

    printf("Inside init_d3d11_hwframe_ctx !!!!!\n");
    out_ref = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!out_ref)
        return AVERROR(ENOMEM);

    out_ctx = (AVHWFramesContext*)out_ref->data;
    out_ctx->format = AV_PIX_FMT_D3D11;
    out_ctx->sw_format = AV_PIX_FMT_NV12;
    out_ctx->width = FFALIGN(width, 32);
    out_ctx->height = FFALIGN(height, 32);

    ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0) {
        av_buffer_unref(&out_ref);
        return ret;
    }

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = out_ref;

    return 0;
}

static av_cold int init_processing_chain(AVFilterContext *ctx, int in_width, int in_height, int out_width, int out_height) {
    D3D11ScaleContext *s = ctx->priv;
     FilterLink     *inl = ff_filter_link(ctx->inputs[0]);
    FilterLink    *outl = ff_filter_link(ctx->outputs[0]);

     if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }

    if(!ctx->hw_device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw device context provided on filter context\n");
        return AVERROR(EINVAL);
    }
    s->hw_device_ctx = ctx->hw_device_ctx;
    
    if(!s->hw_device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create buffer reference for D3D11 hardware device.\n");
        return AVERROR(ENOMEM);
    }
    // AVHWDeviceContext *in_hwctx;
    // int ret;
    // av_log(ctx, AV_LOG_VERBOSE, "Inside init_processing_chain !!!!!\n");
    // if (!ctx->hw_device_ctx) {
    //     av_log(ctx, AV_LOG_ERROR, "No hw device context provided on filter context\n");
    //     return AVERROR(EINVAL);
    // }
    
    // in_hwctx = (AVHWDeviceContext*)ctx->hw_device_ctx->data;

    // ret = init_d3d11_hwframe_ctx(s, ctx->hw_device_ctx, out_width, out_height);
    // if (ret < 0)
    //     return ret;

    outl->hw_frames_ctx = av_buffer_ref(inl->hw_frames_ctx);
        AVHWFramesContext *hw_frame_ctx_out;

    hw_frame_ctx_out = (AVHWFramesContext *)outl->hw_frames_ctx->data;
    hw_frame_ctx_out->format = AV_PIX_FMT_D3D11;
    hw_frame_ctx_out->width = out_width;
    hw_frame_ctx_out->height = out_height;
    av_log(ctx, AV_LOG_VERBOSE, "hw_frames_ctx created and init ================== %p\n", outl->hw_frames_ctx);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int d3d11scale_init(AVFilterContext* ctx) {
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 INIT called!!!!!!!!!\n");
    D3D11ScaleContext* s = ctx->priv;
    HRESULT hr;
    s->d3d_dll = LoadLibrary("D3D11.dll");
    if (!s->d3d_dll) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load D3D11.dll\n");
        return AVERROR_EXTERNAL;
    }
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&s->hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(s->hw_device_ctx);
    if (!ctx->hw_device_ctx) {
    av_log(ctx, AV_LOG_ERROR, "Failed to create buffer reference for D3D11 hardware device.\n");
    return AVERROR(ENOMEM);
    }
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 device created\n");
    return 0;
}

static int d3d11scale_configure_processor(D3D11ScaleContext* s, AVFilterContext* ctx) {

    AVHWDeviceContext* hwctx = (AVHWDeviceContext*)s->hw_device_ctx->data;
    // Get AVD3D11VADeviceContext, which contains the ID3D11Device pointer
    AVD3D11VADeviceContext* d3d11_hwctx = (AVD3D11VADeviceContext*)hwctx->hwctx;

    s->device = (ID3D11Device*)d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    av_log(ctx, AV_LOG_VERBOSE, "INSIDE d3d11scale_configure_processor!!!!!!!!!\n");
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 device and context ptr %p\n", s->device);
 
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
    opTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
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
    AVFilterLink *outlink = ctx->outputs[0];
    
    
        av_log(ctx, AV_LOG_VERBOSE, "Inside Filter_frame function!!!!!!!!\n");
    s->inputWidth = in->width;
    s->inputHeight = in->height;

    av_log(ctx, AV_LOG_VERBOSE, "Captured input dimensions: %dx%d\n", s->inputWidth, s->inputHeight);

    
    AVFrame* out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate output frame through ff_get_video_buffer, attempting manual allocation.\n");
        
        out = av_frame_alloc();
        if (!out) {
            av_frame_free(&in);
            av_log(ctx, AV_LOG_ERROR, "Manual frame allocation failed.\n");
        return AVERROR(ENOMEM);
        }
    }

    s->inputWidth = in->width;
    s->inputHeight = in->height;
    if (!s->processor) {
        if (d3d11scale_configure_processor(s, ctx) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to configure processor\n");
            av_frame_free(&in);
            av_frame_free(&out);
            return AVERROR_EXTERNAL;

            return AVERROR_EXTERNAL;
        }
    }

    ID3D11Texture2D *d3d11_texture = (ID3D11Texture2D *)in->data[0];
    int subIdx = (int)(intptr_t)in->data[1];
    ID3D11VideoProcessorInputView* inputView = NULL;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
    // inputViewDesc.FourCC = DXGI_FORMAT_NV12;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.ArraySlice = subIdx;
    inputViewDesc.Texture2D.MipSlice = 0;


    // Check if in->data[0] is the expected type
    HRESULT hr = ((ID3D11Resource*)in->data[0])->lpVtbl->QueryInterface((ID3D11Resource*)in->data[0], &IID_ID3D11Texture2D, (void**)&d3d11_texture);
    if (FAILED(hr) || !d3d11_texture) {
        av_log(ctx, AV_LOG_ERROR, "Frame data does not reference a valid ID3D11Texture2D object (hr=0x%lX).\n", hr);
        av_frame_free(&in);
        return AVERROR_EXTERNAL;
    }

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorInputView(
        s->videoDevice, d3d11_texture, s->enumerator, &inputViewDesc, &inputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create input view: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {0};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView;
    stream.OutputIndex = 0;

    ID3D11VideoContext* videoContext = NULL;
    s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext, (void**)&videoContext);

    hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor, s->outputView, 0, 1, &stream);
    av_log(ctx, AV_LOG_VERBOSE, "After VideoProcessorBlt function!!!!!!!!\n");
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
        av_frame_free(&in);
        av_frame_free(&out);
        return AVERROR_EXTERNAL;
    }

    // // Code to write the frame
    // D3D11_TEXTURE2D_DESC stagingDesc = { 0 };
    // stagingDesc.Width = s->width;
    // stagingDesc.Height = s->height;
    // stagingDesc.MipLevels = 1;
    // stagingDesc.ArraySize = 1;
    // stagingDesc.Format = DXGI_FORMAT_NV12; 
    // stagingDesc.SampleDesc.Count = 1;
    // stagingDesc.Usage = D3D11_USAGE_STAGING;
    // stagingDesc.BindFlags = 0;
    // stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    // ID3D11Texture2D *stagingTexture = NULL;
    // hr = s->device->lpVtbl->CreateTexture2D(s->device, &stagingDesc, NULL, &stagingTexture);
    // if (FAILED(hr)) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to create staging texture\n");
    //     return AVERROR_EXTERNAL;
    // }

    // // // Copy resource
    // s->context->lpVtbl->CopyResource(s->context, stagingTexture, s->d3d11_vp_output_texture);

    // D3D11_MAPPED_SUBRESOURCE mappedResource;
    // hr = s->context->lpVtbl->Map(s->context, stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
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
    // else {
    // av_log(ctx, AV_LOG_ERROR, "Failed to map the output texture for inspection.\n");
    // }

    // // Access the data and write it to a file
    // // mappedResource.pData points to the pixel data
    // // mappedResource.RowPitch is the row size in bytes
    // //     FILE *file = fopen("output_frame.raw", "wb");
    // // if (file) {
    // //     for (int y = 0; y < s->height; y++) {
    // //         fwrite((uint8_t *)mappedResource.pData + y * mappedResource.RowPitch, 1, s->width, file);
    // //     }
    // //     fclose(file);
    // // }

    // FILE *file = fopen("output_frame.raw", "wb");
    // if (file) {
    //     // Write Y plane
    //     for (int y = 0; y < s->height; y++) {
    //         fwrite((uint8_t *)mappedResource.pData + y * mappedResource.RowPitch, 1, s->width, file);
    //     }
    //     // Write UV plane (height / 2 because UV is subsampled)
    //     for (int y = 0; y < s->height / 2; y++) {
    //         fwrite((uint8_t *)mappedResource.pData + s->height * mappedResource.RowPitch + y * mappedResource.RowPitch, 1, s->width, file);
    //     }
    //     fclose(file);
    // } else {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to open output_frame.raw\n");
    // }
    // // Unmap and release resources
    // s->context->lpVtbl->Unmap(s->context, stagingTexture, 0);
    // stagingTexture->lpVtbl->Release(stagingTexture);
    int ret;

    av_log(ctx, AV_LOG_VERBOSE, "Uncommented the av_frame_cpopy_propes-------------------!!!!!\n");
        ret = av_frame_copy_props(out, in);
        if (ret < 0){
            av_log(ctx, AV_LOG_ERROR, "Failed to copy frame properties\n");
            return ret;
        } else {
            av_log(ctx, AV_LOG_VERBOSE, "Frame properties copied successfully -------------------------------\n");
        }

    // if (d3d11_texture != s->d3d11_vp_output_texture) {
    // av_log(ctx, AV_LOG_ERROR, "Mismatch in texture references: in->data[0] does not match d3d11_vp_output_texture.\n");
    // d3d11_texture->lpVtbl->Release(d3d11_texture); // Release reference if it was created
    // av_frame_free(&in);
    // return AVERROR_EXTERNAL;
    // }

    AVD3D11FrameDescriptor *desc = (AVD3D11FrameDescriptor *)out->buf[0]->data;
    desc->texture = s->d3d11_vp_output_texture;
    desc->index = 0;
   
    // out->buf[0] = av_buffer_ref(desc);
    out->data[0] = (uint8_t *)s->d3d11_vp_output_texture;
    out->data[1]= 0;
    out->width = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;

    av_log(ctx, AV_LOG_VERBOSE, "Input dimensions: %dx%d\n", s->inputWidth, s->inputHeight);
    av_log(ctx, AV_LOG_VERBOSE, "Output dimensions: %dx%d\n", s->width, s->height);
    av_log(ctx, AV_LOG_VERBOSE, "Output pixel format: %s\n", av_get_pix_fmt_name(out->format));
    av_frame_free(&in);
    av_log(ctx, AV_LOG_VERBOSE, "Out Frame format: %s, width: %d, height: %d, data: %p, data1: %p\n", av_get_pix_fmt_name(out->format), out->width, out->height, out->data[0], out->data[1]);
    av_log(ctx, AV_LOG_VERBOSE, "Exiting d3d11scale_filter_frame function!!!!!!!!\n");
    // inputView->lpVtbl->Release(inputView); // Release inputView after use
    return ff_filter_frame(outlink, out);
}

static int d3d11scale_config_props(AVFilterLink* outlink) {
    AVFilterContext *ctx = outlink->src;
    D3D11ScaleContext *s  = ctx->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    FilterLink      *inl = ff_filter_link(inlink);
    AVHWFramesContext     *frames_ctx;
    AVD3D11VADeviceContext *device_hwctx;

    av_log(ctx, AV_LOG_VERBOSE, "Configuring output properties\n");
    int ret = ff_scale_eval_dimensions(s,s->w_expr, s->h_expr,
                                        ctx->inputs[0], outlink,
                                        &s->width, &s->height);
    if (ret < 0)
        return AVERROR(EINVAL);

    outlink->w = s->width;
    outlink->h = s->height;
    frames_ctx   = (AVHWFramesContext*)inl->hw_frames_ctx->data;
    device_hwctx = frames_ctx->device_ctx->hwctx;

    s->hw_device_ctx = device_hwctx;
    ret = init_processing_chain(ctx, s->inputWidth, s->inputHeight, s->width, s->height);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialize processing chain\n");
        return ret;
    }
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 output properties configured successfully\n");

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
