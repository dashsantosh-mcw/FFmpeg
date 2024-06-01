/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define COBJMACROS
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "encode.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "codec_internal.h"
#include "internal.h"
#include "compat/w32dlfcn.h"



static int mf_enca_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 sz;

    if (avctx->codec_id != AV_CODEC_ID_MP3 && avctx->codec_id != AV_CODEC_ID_AC3) {
        hr = IMFAttributes_GetBlobSize(type, &MF_MT_USER_DATA, &sz);
        if (!FAILED(hr) && sz > 0) {
            avctx->extradata = av_mallocz(sz + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata)
                return AVERROR(ENOMEM);
            avctx->extradata_size = sz;
            hr = IMFAttributes_GetBlob(type, &MF_MT_USER_DATA, avctx->extradata, sz, NULL);
            if (FAILED(hr))
                return AVERROR_EXTERNAL;

            if (avctx->codec_id == AV_CODEC_ID_AAC && avctx->extradata_size >= 12) {
                // Get rid of HEAACWAVEINFO (after wfx field, 12 bytes).
                avctx->extradata_size = avctx->extradata_size - 12;
                memmove(avctx->extradata, avctx->extradata + 12, avctx->extradata_size);
            }
        }
    }

    // I don't know where it's documented that we need this. It happens with the
    // MS mp3 encoder MFT. The idea for the workaround is taken from NAudio.
    // (Certainly any lossy codec will have frames much smaller than 1 second.)
    if (!c->out_info.cbSize && !c->out_stream_provides_samples) {
        hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &sz);
        if (!FAILED(hr)) {
            av_log(avctx, AV_LOG_VERBOSE, "MFT_OUTPUT_STREAM_INFO.cbSize set to 0, "
                   "assuming %d bytes instead.\n", (int)sz);
            c->out_info.cbSize = sz;
        }
    }

    return 0;
}

static int mf_encv_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    HRESULT hr;
    UINT32 sz;

    hr = IMFAttributes_GetBlobSize(type, &MF_MT_MPEG_SEQUENCE_HEADER, &sz);
    if (!FAILED(hr) && sz > 0) {
        uint8_t *extradata = av_mallocz(sz + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!extradata)
            return AVERROR(ENOMEM);
        hr = IMFAttributes_GetBlob(type, &MF_MT_MPEG_SEQUENCE_HEADER, extradata, sz, NULL);
        if (FAILED(hr)) {
            av_free(extradata);
            return AVERROR_EXTERNAL;
        }
        av_freep(&avctx->extradata);
        avctx->extradata = extradata;
        avctx->extradata_size = sz;
    }

    return 0;
}

static int mf_output_type_get(AVCodecContext *avctx)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    IMFMediaType *type;
    int ret;

    hr = IMFTransform_GetOutputCurrentType(c->mft, c->out_stream_id, &type);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get output type\n");
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_VERBOSE, "final output type:\n");
    ff_media_type_dump(avctx, type);

    ret = 0;
    if (c->is_video) {
        ret = mf_encv_output_type_get(avctx, type);
    } else if (c->is_audio) {
        ret = mf_enca_output_type_get(avctx, type);
    }

    if (ret < 0)
        av_log(avctx, AV_LOG_ERROR, "output type not supported\n");

    IMFMediaType_Release(type);
    return ret;
}

static int mf_sample_to_avpacket(AVCodecContext *avctx, IMFSample *sample, AVPacket *avpkt)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    DWORD len;
    IMFMediaBuffer *buffer;
    BYTE *data;
    UINT64 t;
    UINT32 t32;

    hr = IMFSample_GetTotalLength(sample, &len);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    if ((ret = ff_get_encode_buffer(avctx, avpkt, len, 0)) < 0)
        return ret;

    IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        return AVERROR_EXTERNAL;
    }

    memcpy(avpkt->data, data, len);

    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);

    avpkt->pts = avpkt->dts = mf_sample_get_pts(avctx, sample);

    hr = IMFAttributes_GetUINT32(sample, &MFSampleExtension_CleanPoint, &t32);
    if (c->is_audio || (!FAILED(hr) && t32 != 0))
        avpkt->flags |= AV_PKT_FLAG_KEY;

    hr = IMFAttributes_GetUINT64(sample, &MFSampleExtension_DecodeTimestamp, &t);
    if (!FAILED(hr)) {
        avpkt->dts = mf_from_mf_time(avctx, t);
        // At least on Qualcomm's HEVC encoder on SD 835, the output dts
        // starts from the input pts of the first frame, while the output pts
        // is shifted forward. Therefore, shift the output values back so that
        // the output pts matches the input.
        if (c->reorder_delay == AV_NOPTS_VALUE)
            c->reorder_delay = avpkt->pts - avpkt->dts;
        avpkt->dts -= c->reorder_delay;
        avpkt->pts -= c->reorder_delay;
    }

    return 0;
}

static IMFSample *mf_a_avframe_to_sample(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    size_t len;
    size_t bps;
    IMFSample *sample;

    bps = av_get_bytes_per_sample(avctx->sample_fmt) * avctx->ch_layout.nb_channels;
    len = frame->nb_samples * bps;

    sample = ff_create_memory_sample(&c->functions, frame->data[0], len,
                                     c->in_info.cbAlignment);
    if (sample)
        IMFSample_SetSampleDuration(sample, mf_to_mf_time(avctx, frame->nb_samples));
    return sample;
}

static IMFSample *mf_v_avframe_to_sample(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample;
    IMFMediaBuffer *buffer;
    BYTE *data;
    HRESULT hr;
    int ret;
    int size;

    size = av_image_get_buffer_size(avctx->pix_fmt, avctx->width, avctx->height, 1);
    if (size < 0)
        return NULL;

    sample = ff_create_memory_sample(&c->functions, NULL, size,
                                     c->in_info.cbAlignment);
    if (!sample)
        return NULL;

    hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
    if (FAILED(hr)) {
        IMFSample_Release(sample);
        return NULL;
    }

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        IMFSample_Release(sample);
        return NULL;
    }

    ret = av_image_copy_to_buffer((uint8_t *)data, size, (void *)frame->data, frame->linesize,
                                  avctx->pix_fmt, avctx->width, avctx->height, 1);
    IMFMediaBuffer_SetCurrentLength(buffer, size);
    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);
    if (ret < 0) {
        IMFSample_Release(sample);
        return NULL;
    }

    IMFSample_SetSampleDuration(sample, mf_to_mf_time(avctx, frame->duration));

    return sample;
}

static IMFSample *mf_avframe_to_sample(AVCodecContext *avctx, const AVFrame *frame)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample;

    if (c->is_audio) {
        sample = mf_a_avframe_to_sample(avctx, frame);
    } else {
        sample = mf_v_avframe_to_sample(avctx, frame);
    }

    if (sample)
        mf_sample_set_pts(avctx, sample, frame->pts);

    return sample;
}


static int mf_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    MFContext *c = avctx->priv_data;
    IMFSample *sample = NULL;
    int ret;

    if (!c->frame->buf[0]) {
        ret = ff_encode_get_frame(avctx, c->frame);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    if (c->frame->buf[0]) {
        sample = mf_avframe_to_sample(avctx, c->frame);
        if (!sample) {
            av_frame_unref(c->frame);
            return AVERROR(ENOMEM);
        }
        if (c->is_video && c->codec_api) {
            if (c->frame->pict_type == AV_PICTURE_TYPE_I || !c->sample_sent)
                ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncVideoForceKeyFrame, FF_VAL_VT_UI4(1));
        }
    }

    ret = mf_send_sample(avctx, sample);
    if (sample)
        IMFSample_Release(sample);
    if (ret != AVERROR(EAGAIN))
        av_frame_unref(c->frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return ret;

    ret = mf_receive_sample(avctx, &sample);
    if (ret < 0)
        return ret;

    ret = mf_sample_to_avpacket(avctx, sample, avpkt);
    IMFSample_Release(sample);

    return ret;
}

// Most encoders seem to enumerate supported audio formats on the output types,
// at least as far as channel configuration and sample rate is concerned. Pick
// the one which seems to match best.
static int64_t mf_enca_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 t;
    GUID tg;
    int64_t score = 0;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr) && t == avctx->sample_rate)
        score |= 1LL << 32;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr) && t == avctx->ch_layout.nb_channels)
        score |= 2LL << 32;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score |= 4LL << 32;
    }

    // Select the bitrate (lowest priority).
    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &t);
    if (!FAILED(hr)) {
        int diff = (int)t - avctx->bit_rate / 8;
        if (diff >= 0) {
            score |= (1LL << 31) - diff; // prefer lower bitrate
        } else {
            score |= (1LL << 30) + diff; // prefer higher bitrate
        }
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, &t);
    if (!FAILED(hr) && t != 0)
        return -1;

    return score;
}

static int mf_enca_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    // (some decoders allow adjusting this freely, but it can also cause failure
    //  to set the output type - so it's commented for being too fragile)
    //IMFAttributes_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avctx->bit_rate / 8);
    //IMFAttributes_SetUINT32(type, &MF_MT_AVG_BITRATE, avctx->bit_rate);

    return 0;
}

static int64_t mf_enca_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    HRESULT hr;
    UINT32 t;
    int64_t score = 0;

    enum AVSampleFormat sformat = ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sformat == AV_SAMPLE_FMT_NONE)
        return -1; // can not use

    if (sformat == avctx->sample_fmt)
        score |= 1;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr) && t == avctx->sample_rate)
        score |= 2;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr) && t == avctx->ch_layout.nb_channels)
        score |= 4;

    return score;
}

static int mf_enca_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    HRESULT hr;
    UINT32 t;

    enum AVSampleFormat sformat = ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sformat != avctx->sample_fmt) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input sample format set\n");
        return AVERROR(EINVAL);
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (FAILED(hr) || t != avctx->sample_rate) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input sample rate set\n");
        return AVERROR(EINVAL);
    }

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (FAILED(hr) || t != avctx->ch_layout.nb_channels) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input channel number set\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int64_t mf_encv_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    GUID tg;
    HRESULT hr;
    int score = -1;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;
    }

    return score;
}

static int mf_encv_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFContext *c = avctx->priv_data;
    AVRational framerate;

    ff_MFSetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);
    IMFAttributes_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
        framerate = avctx->framerate;
    } else {
        framerate = av_inv_q(avctx->time_base);
#if FF_API_TICKS_PER_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
        framerate.den *= avctx->ticks_per_frame;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    }

    ff_MFSetAttributeRatio((IMFAttributes *)type, &MF_MT_FRAME_RATE, framerate.num, framerate.den);

    // (MS HEVC supports eAVEncH265VProfile_Main_420_8 only.)
    if (avctx->codec_id == AV_CODEC_ID_H264) {
        UINT32 profile = ff_eAVEncH264VProfile_Base;
        switch (avctx->profile) {
        case AV_PROFILE_H264_MAIN:
            profile = ff_eAVEncH264VProfile_Main;
            break;
        case AV_PROFILE_H264_HIGH:
            profile = ff_eAVEncH264VProfile_High;
            break;
        }
        IMFAttributes_SetUINT32(type, &MF_MT_MPEG2_PROFILE, profile);
    }

    IMFAttributes_SetUINT32(type, &MF_MT_AVG_BITRATE, avctx->bit_rate);

    // Note that some of the ICodecAPI options must be set before SetOutputType.
    if (c->codec_api) {
        if (avctx->bit_rate)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncCommonMeanBitRate, FF_VAL_VT_UI4(avctx->bit_rate));

        if (c->opt_enc_rc >= 0)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncCommonRateControlMode, FF_VAL_VT_UI4(c->opt_enc_rc));

        if (c->opt_enc_quality >= 0)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncCommonQuality, FF_VAL_VT_UI4(c->opt_enc_quality));

        // Always set the number of b-frames. Qualcomm's HEVC encoder on SD835
        // defaults this to 1, and that setting is buggy with many of the
        // rate control modes. (0 or 2 b-frames works fine with most rate
        // control modes, but 2 seems buggy with the u_vbr mode.) Setting
        // "scenario" to "camera_record" sets it in CFR mode (where the default
        // is VFR), which makes the encoder avoid dropping frames.
        ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncMPVDefaultBPictureCount, FF_VAL_VT_UI4(avctx->max_b_frames));
        avctx->has_b_frames = avctx->max_b_frames > 0;

        ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVEncH264CABACEnable, FF_VAL_VT_BOOL(1));

        if (c->opt_enc_scenario >= 0)
            ICodecAPI_SetValue(c->codec_api, &ff_CODECAPI_AVScenarioInfo, FF_VAL_VT_UI4(c->opt_enc_scenario));
    }

    return 0;
}

static int64_t mf_encv_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt != avctx->pix_fmt)
        return -1; // can not use

    return 0;
}

static int mf_encv_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt != avctx->pix_fmt) {
        av_log(avctx, AV_LOG_ERROR, "unsupported input pixel format set\n");
        return AVERROR(EINVAL);
    }

    //ff_MFSetAttributeSize((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);

    return 0;
}


#define OFFSET(x) offsetof(MFContext, x)

#define MF_ENCODER(MEDIATYPE, NAME, ID, OPTS, FMTS, CAPS) \
    static const AVClass ff_ ## NAME ## _mf_encoder_class = {                  \
        .class_name = #NAME "_mf",                                             \
        .item_name  = av_default_item_name,                                    \
        .option     = OPTS,                                                    \
        .version    = LIBAVUTIL_VERSION_INT,                                   \
    };                                                                         \
    const FFCodec ff_ ## NAME ## _mf_encoder = {                               \
        .p.priv_class   = &ff_ ## NAME ## _mf_encoder_class,                   \
        .p.name         = #NAME "_mf",                                         \
        CODEC_LONG_NAME(#ID " via MediaFoundation"),                           \
        .p.type         = AVMEDIA_TYPE_ ## MEDIATYPE,                          \
        .p.id           = AV_CODEC_ID_ ## ID,                                  \
        .priv_data_size = sizeof(MFContext),                                   \
        .init           = mf_init,                                             \
        .close          = mf_close,                                            \
        FF_CODEC_RECEIVE_PACKET_CB(mf_receive_packet),                         \
        FMTS                                                                   \
        CAPS                                                                   \
        .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,                           \
    };

#define AFMTS \
        .p.sample_fmts  = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16,    \
                                                         AV_SAMPLE_FMT_NONE },
#define ACAPS \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HYBRID |           \
                          AV_CODEC_CAP_DR1 | AV_CODEC_CAP_VARIABLE_FRAME_SIZE,

MF_ENCODER(AUDIO, aac,         AAC, NULL, AFMTS, ACAPS);
MF_ENCODER(AUDIO, ac3,         AC3, NULL, AFMTS, ACAPS);
MF_ENCODER(AUDIO, mp3,         MP3, NULL, AFMTS, ACAPS);

#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption venc_opts[] = {
    {"rate_control",  "Select rate control mode", OFFSET(opt_enc_rc), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VE, "rate_control"},
    { "default",      "Default mode", 0, AV_OPT_TYPE_CONST, {.i64 = -1}, 0, 0, VE, "rate_control"},
    { "cbr",          "CBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_CBR}, 0, 0, VE, "rate_control"},
    { "pc_vbr",       "Peak constrained VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_PeakConstrainedVBR}, 0, 0, VE, "rate_control"},
    { "u_vbr",        "Unconstrained VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_UnconstrainedVBR}, 0, 0, VE, "rate_control"},
    { "quality",      "Quality mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_Quality}, 0, 0, VE, "rate_control" },
    // The following rate_control modes require Windows 8.
    { "ld_vbr",       "Low delay VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_LowDelayVBR}, 0, 0, VE, "rate_control"},
    { "g_vbr",        "Global VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_GlobalVBR}, 0, 0, VE, "rate_control" },
    { "gld_vbr",      "Global low delay VBR mode", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVEncCommonRateControlMode_GlobalLowDelayVBR}, 0, 0, VE, "rate_control"},

    {"scenario",          "Select usage scenario", OFFSET(opt_enc_scenario), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VE, "scenario"},
    { "default",          "Default scenario", 0, AV_OPT_TYPE_CONST, {.i64 = -1}, 0, 0, VE, "scenario"},
    { "display_remoting", "Display remoting", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVScenarioInfo_DisplayRemoting}, 0, 0, VE, "scenario"},
    { "video_conference", "Video conference", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVScenarioInfo_VideoConference}, 0, 0, VE, "scenario"},
    { "archive",          "Archive", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVScenarioInfo_Archive}, 0, 0, VE, "scenario"},
    { "live_streaming",   "Live streaming", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVScenarioInfo_LiveStreaming}, 0, 0, VE, "scenario"},
    { "camera_record",    "Camera record", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVScenarioInfo_CameraRecord}, 0, 0, VE, "scenario"},
    { "display_remoting_with_feature_map", "Display remoting with feature map", 0, AV_OPT_TYPE_CONST, {.i64 = ff_eAVScenarioInfo_DisplayRemotingWithFeatureMap}, 0, 0, VE, "scenario"},

    {"quality",       "Quality", OFFSET(opt_enc_quality), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 100, VE},
    {"hw_encoding",   "Force hardware encoding", OFFSET(opt_enc_hw), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VE},
    {NULL}
};

#define VFMTS \
        .p.pix_fmts     = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12,       \
                                                        AV_PIX_FMT_YUV420P,    \
                                                        AV_PIX_FMT_NONE },
#define VCAPS \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HYBRID |           \
                          AV_CODEC_CAP_DR1,

MF_ENCODER(VIDEO, h264,        H264, venc_opts, VFMTS, VCAPS);
MF_ENCODER(VIDEO, hevc,        HEVC, venc_opts, VFMTS, VCAPS);
