#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mf.h>
#include "mf_utils.h"
#include "mpeg4audio.h"
#include "decode.h"

// Used to destroy the decoder once the last frame reference has been
// released when using opaque decoding mode.
typedef struct MFDecoder {
    IMFTransform *mft;
    AVBufferRef *device_ref;
} MFDecoder;

static int mf_sample_to_v_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    AVFrame *mf_frame = c->tmp_frame;
    int ret = 0;

    if (!c->frames_ref)
        return AVERROR(EINVAL);

    av_frame_unref(mf_frame);
    av_frame_unref(frame);

    mf_frame->width = avctx->width;
    mf_frame->height = avctx->height;
    mf_frame->format = AV_PIX_FMT_MF;
    mf_frame->data[3] = (void *)sample;

    if ((ret = ff_decode_frame_props(avctx, mf_frame)) < 0)
        return ret;

    // ff_decode_frame_props() overwites this
    mf_frame->format = AV_PIX_FMT_MF;

    mf_frame->hw_frames_ctx = av_buffer_ref(c->frames_ref);
    if (!mf_frame->hw_frames_ctx)
        return AVERROR(ENOMEM);

    if (c->use_opaque) {
        struct frame_ref *ref = av_mallocz(sizeof(*ref));
        if (!ref)
            return AVERROR(ENOMEM);
        ref->sample = sample;
        ref->decoder_ref = av_buffer_ref(c->decoder_ref);
        if (!ref->decoder_ref) {
            av_free(ref);
            return AVERROR(ENOMEM);
        }
        mf_frame->buf[0] = av_buffer_create((void *)ref, sizeof(*ref),
                                            mf_buffer_ref_free, NULL,
                                            AV_BUFFER_FLAG_READONLY);
        if (!mf_frame->buf[0]) {
            av_buffer_unref(&ref->decoder_ref);
            av_free(ref);
            return AVERROR(ENOMEM);
        }
        IMFSample_AddRef(sample);
        av_frame_move_ref(frame, mf_frame);
    } else {
        frame->width = mf_frame->width;
        frame->height = mf_frame->height;
        frame->format = c->sw_format;

        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
            return ret;

        if ((ret = av_hwframe_transfer_data(frame, mf_frame, 0)) < 0)
            return ret;
    }

    // Strictly optional - release the IMFSample a little bit earlier.
    av_frame_unref(mf_frame);

    return 0;
}

static int mf_sample_to_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    int ret;

    if (c->is_audio) {
        ret = mf_sample_to_a_avframe(avctx, sample, frame);
    } else {
        ret = mf_sample_to_v_avframe(avctx, sample, frame);
    }

    frame->pts = mf_sample_get_pts(avctx, sample);
    frame->best_effort_timestamp = frame->pts;
    frame->pkt_dts = AV_NOPTS_VALUE;

    return ret;
}

static int mf_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    int ret;
    IMFSample *sample = NULL;
    if (avpkt) {
        sample = mf_avpacket_to_sample(avctx, avpkt);
        if (!sample)
            return AVERROR(ENOMEM);
    }
    ret = mf_send_sample(avctx, sample);
    if (sample)
        IMFSample_Release(sample);
    return ret;
}

static int mf_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    IMFSample *sample;
    int ret;
    AVPacket packet;

    while (1) {
        ret = mf_receive_sample(avctx, &sample);
        if (ret == 0) {
            ret = mf_sample_to_avframe(avctx, sample, frame);
            IMFSample_Release(sample);
            return ret;
        } else if (ret == AVERROR(EAGAIN)) {
            ret = ff_decode_get_packet(avctx, &packet);
            if (ret < 0) {
                return ret;
            }
            ret = mf_send_packet(avctx, &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                return ret;
            }
        } else {
            return ret;
        }
    }
}

static int mf_deca_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;

    int sample_rate = avctx->sample_rate;
    int channels = avctx->channels;

    IMFAttributes_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFAttributes_SetGUID(type, &MF_MT_SUBTYPE, &c->main_subtype);

    if (avctx->codec_id == AV_CODEC_ID_AAC) {
        int assume_adts = avctx->extradata_size == 0;
        // The first 12 bytes are the remainder of HEAACWAVEINFO.
        // Fortunately all fields can be left 0.
        size_t ed_size = 12 + (size_t)avctx->extradata_size;
        uint8_t *ed = av_mallocz(ed_size);
        if (!ed)
            return AVERROR(ENOMEM);
        if (assume_adts)
            ed[0] = 1; // wPayloadType=1 (ADTS)
        if (avctx->extradata_size) {
            MPEG4AudioConfig c = {0};
            memcpy(ed + 12, avctx->extradata, avctx->extradata_size);
            if (avpriv_mpeg4audio_get_config(&c, avctx->extradata, avctx->extradata_size * 8, 0) >= 0) {
                if (c.channels > 0)
                    channels = c.channels;
                sample_rate = c.sample_rate;
            }
        }
        IMFAttributes_SetBlob(type, &MF_MT_USER_DATA, ed, ed_size);
        av_free(ed);
        IMFAttributes_SetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, assume_adts ? 1 : 0);
    } else if (avctx->extradata_size) {
        IMFAttributes_SetBlob(type, &MF_MT_USER_DATA, avctx->extradata, avctx->extradata_size);
    }

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, channels);

    // WAVEFORMATEX stuff; might be required by some codecs.
    if (avctx->block_align)
        IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, avctx->block_align);
    if (avctx->bit_rate)
        IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avctx->bit_rate / 8);
    if (avctx->bits_per_coded_sample)
        IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, avctx->bits_per_coded_sample);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_PREFER_WAVEFORMATEX, 1);

    return 0;
}

static int64_t mf_decv_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    uint32_t fourcc;
    GUID tg;
    HRESULT hr;
    int score = -1;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;

        // For the MPEG-4 decoder (selects MPEG-4 variant via FourCC).
        if (ff_fourcc_from_guid(&tg, &fourcc) >= 0 && fourcc == avctx->codec_tag)
            score = 2;
    }

    return score;
}

static int mf_decv_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int use_extradata = avctx->extradata_size && !c->bsfc;

    IMFAttributes_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);

    hr = IMFAttributes_GetItem(type, &MF_MT_SUBTYPE, NULL);
    if (FAILED(hr))
        IMFAttributes_SetGUID(type, &MF_MT_SUBTYPE, &c->main_subtype);

    ff_MFSetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);

    IMFAttributes_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);

    if (avctx->sample_aspect_ratio.num)
        ff_MFSetAttributeRatio((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO,
                               avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);

    if (avctx->bit_rate)
        IMFAttributes_SetUINT32(type, &MF_MT_AVG_BITRATE, avctx->bit_rate);

    if (IsEqualGUID(&c->main_subtype, &MFVideoFormat_MP4V) ||
        IsEqualGUID(&c->main_subtype, &MFVideoFormat_MP43) ||
        IsEqualGUID(&c->main_subtype, &ff_MFVideoFormat_MP42)) {
        if (avctx->extradata_size < 3 ||
            avctx->extradata[0] || avctx->extradata[1] ||
            avctx->extradata[2] != 1)
            use_extradata = 0;
    }

    if (use_extradata)
        IMFAttributes_SetBlob(type, &MF_MT_USER_DATA, avctx->extradata, avctx->extradata_size);

    return 0;
}

static int64_t mf_deca_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    GUID tg;
    int score = -1;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;
    }

    return score;
}

// Sort the types by preference:
// - float sample format (highest)
// - sample depth
// - channel count
// - sample rate (lowest)
// Assume missing information means any is allowed.
static int64_t mf_deca_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 t;
    int sample_fmt;
    int64_t score = 0;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr))
        score |= t;

    // MF doesn't seem to tell us the native channel count. Try to get the
    // same number of channels by looking at the input codec parameters.
    // (With some luck they are correct, or even come from a parser.)
    // Prefer equal or larger channel count.
    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr)) {
        int channels = av_get_channel_layout_nb_channels(avctx->request_channel_layout);
        int64_t ch_score = 0;
        int diff;
        if (channels < 1)
            channels = c->original_channels;
        diff = (int)t - channels;
        if (diff >= 0) {
            ch_score |= (1LL << 7) - diff;
        } else {
            ch_score |= (1LL << 6) + diff;
        }
        score |= ch_score << 20;
    }

    sample_fmt = ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sample_fmt == AV_SAMPLE_FMT_NONE) {
        score = -1;
    } else {
        score |= av_get_bytes_per_sample(sample_fmt) << 28;
        if (sample_fmt == AV_SAMPLE_FMT_FLT)
            score |= 1LL << 32;
    }

    return score;
}

static int mf_deca_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    int block_align;
    HRESULT hr;

    // Some decoders (wmapro) do not list any output types. I have no clue
    // what we're supposed to do, and this is surely a MFT bug. Setting an
    // arbitrary output type helps.
    hr = IMFAttributes_GetItem(type, &MF_MT_MAJOR_TYPE, NULL);
    if (!FAILED(hr))
        return 0;

    IMFAttributes_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);

    block_align = 4;
    IMFAttributes_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 32);

    block_align *= avctx->channels;
    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, avctx->channels);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, avctx->sample_rate);

    IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_align * avctx->sample_rate);

    return 0;
}

static int64_t mf_decv_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt == AV_PIX_FMT_NONE)
        return -1;
    if (pix_fmt == AV_PIX_FMT_P010)
        return 2;
    if (pix_fmt == AV_PIX_FMT_NV12)
        return 1;
    return 0;
}


#define OFFSET(x) offsetof(MFContext, x)

#define MF_DECODER(MEDIATYPE, NAME, ID, OPTS) \
    static const AVClass ff_ ## NAME ## _mf_decoder_class = {                  \
        .class_name = #NAME "_mf",                                             \
        .item_name  = av_default_item_name,                                    \
        .option     = OPTS,                                                    \
        .version    = LIBAVUTIL_VERSION_INT,                                   \
    };                                                                         \
    AVCodec ff_ ## NAME ## _mf_decoder = {                                     \
        .priv_class     = &ff_ ## NAME ## _mf_decoder_class,                   \
        .name           = #NAME "_mf",                                         \
        .long_name      = NULL_IF_CONFIG_SMALL(#ID " via MediaFoundation"),    \
        .type           = AVMEDIA_TYPE_ ## MEDIATYPE,                          \
        .id             = AV_CODEC_ID_ ## ID,                                  \
        .priv_data_size = sizeof(MFContext),                                   \
        .init           = mf_init,                                             \
        .close          = mf_close,                                            \
        .receive_frame  = mf_receive_frame,                                    \
        .flush          = mf_flush,                                            \
        .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,     \
        .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |                          \
                          FF_CODEC_CAP_INIT_THREADSAFE |                       \
                          FF_CODEC_CAP_INIT_CLEANUP,                           \
    };

MF_DECODER(AUDIO, ac3,         AC3,             NULL);
MF_DECODER(AUDIO, eac3,        EAC3,            NULL);
MF_DECODER(AUDIO, aac,         AAC,             NULL);
MF_DECODER(AUDIO, mp1,         MP1,             NULL);
MF_DECODER(AUDIO, mp2,         MP2,             NULL);
MF_DECODER(AUDIO, mp3,         MP3,             NULL);
MF_DECODER(AUDIO, wmav1,       WMAV1,           NULL);
MF_DECODER(AUDIO, wmav2,       WMAV2,           NULL);
MF_DECODER(AUDIO, wmalossless, WMALOSSLESS,     NULL);
MF_DECODER(AUDIO, wmapro,      WMAPRO,          NULL);
MF_DECODER(AUDIO, wmavoice,    WMAVOICE,        NULL);

#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption vdec_opts[] = {
    // Only used for non-opaque output (otherwise, the AVHWDeviceContext matters)
    {"use_d3d",       "D3D decoding mode", OFFSET(opt_use_d3d), AV_OPT_TYPE_INT, {.i64 = AV_MF_NONE}, 0, INT_MAX, VD, "use_d3d"},
    { "auto",         "Any (or none) D3D mode", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_AUTO}, 0, 0, VD, "use_d3d"},
    { "none",         "Disable D3D mode", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_NONE}, 0, 0, VD, "use_d3d"},
    { "d3d9",         "D3D9 decoding", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_D3D9}, 0, 0, VD, "use_d3d"},
    { "d3d11",        "D3D11 decoding", 0, AV_OPT_TYPE_CONST, {.i64 = AV_MF_D3D11}, 0, 0, VD, "use_d3d"},
    // Can be used to fail early if no hwaccel is available
    {"require_d3d",   "Fail init if D3D cannot be used", OFFSET(opt_require_d3d), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD},
    // Experimenting with h264/d3d11 shows: allocated_textures = MIN(out_samples, 5) + 18
    // (not set if -1)
    {"out_samples",   "Minimum output sample count", OFFSET(opt_out_samples), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 100, VD},
    // D3D11_BIND_FLAG used for texture allocations; must include D3D11_BIND_DECODER
    // (not set if -1)
    {"d3d_bind_flags","Texture D3D_BIND_FLAG", OFFSET(opt_d3d_bind_flags), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VD},
    {NULL}
};

#define MF_VIDEO_DECODER(NAME, ID) \
    AVHWAccel ff_ ## NAME ## _mf_hwaccel = {                                   \
        .name       = #NAME "_mf",                                             \
        .type       = AVMEDIA_TYPE_VIDEO,                                      \
        .id         = AV_CODEC_ID_ ## ID,                                      \
        .pix_fmt    = AV_PIX_FMT_MF,                                           \
    };                                                                         \
    MF_DECODER(VIDEO, NAME, ID, vdec_opts);

MF_VIDEO_DECODER(h264,         H264);
MF_VIDEO_DECODER(hevc,         HEVC);
MF_VIDEO_DECODER(vc1,          VC1);
MF_VIDEO_DECODER(wmv1,         WMV1);
MF_VIDEO_DECODER(wmv2,         WMV2);
MF_VIDEO_DECODER(wmv3,         WMV3);
MF_VIDEO_DECODER(mpeg2,        MPEG2VIDEO);
MF_VIDEO_DECODER(mpeg4,        MPEG4);
MF_VIDEO_DECODER(msmpeg4v1,    MSMPEG4V1);
MF_VIDEO_DECODER(msmpeg4v2,    MSMPEG4V2);
MF_VIDEO_DECODER(msmpeg4v3,    MSMPEG4V3);
MF_VIDEO_DECODER(mjpeg,        MJPEG);
