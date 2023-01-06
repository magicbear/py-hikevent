#include <time.h>
#include <unistd.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <iconv.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "HCNetSDK.h"
#include "LinuxPlayM4.h"
extern "C" { 
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/opt.h>
#include <libavutil/file.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
char av_ts[AV_TS_MAX_STRING_SIZE] = { 0 };
#undef av_err2str
#undef av_ts2str
#undef av_ts2timestr
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)
#define av_ts2str(ts) av_ts_make_string(av_ts, ts)
#define av_ts2timestr(ts, tb) av_ts_make_time_string(av_ts, ts, tb)
}

#define AV_AUDIO_TRANSCODE

char		host[256] 	= {0};
char		user[256] 	= {0};
char		passwd[256] = {0};
char        STREAM_VIDEO_CODEC[256] = {0};
char        STREAM_AUDIO_CODEC[256] = {0};
int  		cameraNo;
int 		streamType;

static const char *shortopts = ":h:u:p:c:s:Nv?";

static struct option longopts[] = {
  {"host",          1, 0, 'h'},
  {"user",          1, 0, 'u'},
  {"passwd", 	    1, 0, 'p'},
  {"camera",        1, 0, 'c'},
  {"stream-type",   1, 0, 's'},
  {"ignore-acodec", 0, 0, 'N'},
  {"verbose",       0, 0, 'v'},
  {"help",          0, 0, '?'},

  {0, 0, 0, 0}
};

static volatile int keepRunning = 1;

void intHandler(int dummy) {
    keepRunning = 0;
}

struct entry {
    int bType;
    char *data;
    int start;
    size_t dwBufLen;
    TAILQ_ENTRY(entry) entries;
};

struct HIKEvent_DecodeThread;

typedef struct {
    char *ip;
    char *user;
    char *passwd;
    char *error_buffer;
    LONG lUserID;
    LONG lHandle;
    NET_DVR_DEVICEINFO_V30 struDeviceInfo;
    NET_DVR_COMPRESSION_AUDIO compressAudioType;
    NET_DVR_USER_LOGIN_INFO struLoginInfo = {0};
    NET_DVR_DEVICEINFO_V40 struDeviceInfoV40 = {0};

    int transcode;
    int debug_packet;

    uint32_t rx_size;
    uint32_t tx_size;
    double   last_reset;

    struct HIKEvent_DecodeThread *decthread_ctx;

    pthread_mutex_t lock;
    TAILQ_HEAD(decode_tailhead, entry) decode_head;
    TAILQ_HEAD(push_tailhead, entry) push_head;

    AVFormatContext *plivectx = nullptr;
    /* Type-specific fields go here. */
} HIKEvent_Object;

typedef struct HIKEvent_DecodeThread {
    HIKEvent_Object *ps;
    pthread_t thread;
    pthread_t process_thread;
    AVStream *out_vstream;
    AVStream *out_astream;

    int64_t       global_pts;

    /* Global timestamp for the audio frames. */
    int64_t video_pts;
    int64_t audio_pts;

    int src_rate;
    int dst_rate;

    AVFormatContext *pFormatCtx;
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
    SwrContext *swr;
    AVAudioFifo *fifo;
    void *g722_decoder;
    bool outputHeaderWrite;
    bool probedone;
} HIKEvent_DecodeThread;

#define MICRO_IN_SEC 1000000.00

double microtime(void)
{
    struct timeval tp = {0};

    if (gettimeofday(&tp, NULL)) {
        return 0;
    }

    return ((double)(tp.tv_sec + tp.tv_usec / MICRO_IN_SEC));
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, int output)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    fprintf(stderr, "[\033[%s%s\033[0m \033[%s%s\033[0m %d] pts:%16" PRId64 " pts_time:%-8s dts:%16" PRId64 " dts_time:%-8s duration:%-8s duration_time:%-8s tb: %4d/%-8d size: %d\n",
        (output ? "92m" :  "31m"),
        output?"OUT" : "IN ",
        fmt_ctx->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "38;5;118m" : "38;5;196m",
        fmt_ctx->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", pkt->stream_index,
           pkt->pts, av_ts2timestr(pkt->pts, time_base),
           pkt->dts, av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           time_base->num, time_base->den, 
           pkt->buf ? pkt->buf->size : -1);
}


/**
 * Initialize one data packet for reading or writing.
 * @param[out] packet Packet to be initialized
 * @return Error code (0 if successful)
 */
static int init_packet(AVPacket **packet)
{
    if (!(*packet = av_packet_alloc())) {
        fprintf(stderr, "Could not allocate packet\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Initialize one audio frame for reading from the input file.
 * @param[out] frame Frame to be initialized
 * @return Error code (0 if successful)
 */
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate input frame\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Decode one audio frame from the input file.
 * @param      frame                Audio frame to be decoded
 * @param      input_format_context Format context of the input file
 * @param      input_codec_context  Codec context of the input file
 * @param[out] data_present         Indicates whether data has been decoded
 * @param[out] finished             Indicates whether the end of file has
 *                                  been reached and all data has been
 *                                  decoded. If this flag is false, there
 *                                  is more data to be decoded, i.e., this
 *                                  function has to be called again.
 * @return Error code (0 if successful)
 */
static int decode_audio_frame(AVFrame *frame,
                              AVPacket *input_packet,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished)
{
    int error;

    /* Send the audio frame stored in the temporary packet to the decoder.
     * The input audio stream decoder is used to do this. */
    if ((error = avcodec_send_packet(input_codec_context, input_packet)) < 0) {
        fprintf(stderr, "Could not send packet for decoding (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

    /* Receive one frame from the decoder. */
    error = avcodec_receive_frame(input_codec_context, frame);
    /* If the decoder asks for more data to be able to decode a frame,
     * return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
    /* If the end of the input file is reached, stop decoding. */
    } else if (error == AVERROR_EOF) {
        *finished = 1;
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not decode frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    /* Default case: Return decoded data. */
    } else {
        *data_present = 1;
        goto cleanup;
    }

cleanup:
    return error;
}

/**
 * Add converted input audio samples to the FIFO buffer for later processing.
 * @param fifo                    Buffer to add the samples to
 * @param converted_input_samples Samples to be added. The dimensions are channel
 *                                (for multi-channel audio), sample.
 * @param frame_size              Number of samples to be converted
 * @return Error code (0 if successful)
 */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /* Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples. */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO\n");
        return error;
    }

    /* Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}


/**
 * Initialize a temporary storage for the specified number of audio samples.
 * The conversion requires temporary storage due to the different format.
 * The number of audio samples to be allocated is specified in frame_size.
 * @param[out] converted_input_samples Array of converted samples. The
 *                                     dimensions are reference, channel
 *                                     (for multi-channel audio), sample.
 * @param      output_codec_context    Codec context of the output file
 * @param      frame_size              Number of samples to be converted in
 *                                     each round
 * @return Error code (0 if successful)
 */
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    int error;

    /* Allocate as many pointers as there are audio channels.
     * Each pointer will later point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     */
    if (!(*converted_input_samples = (uint8_t **)calloc(output_codec_context->channels,
                                            sizeof(**converted_input_samples)))) {
        fprintf(stderr, "Could not allocate converted input sample pointers\n");
        return AVERROR(ENOMEM);
    }

    /* Allocate memory for the samples of all channels in one consecutive
     * block for convenience. */
    if ((error = av_samples_alloc(*converted_input_samples, NULL,
                                  output_codec_context->channels,
                                  frame_size,
                                  output_codec_context->sample_fmt, 0)) < 0) {
        fprintf(stderr,
                "Could not allocate converted input samples (error '%s')\n",
                av_err2str(error));
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return error;
    }
    return 0;
}

/**
 * Convert the input audio samples into the output sample format.
 * The conversion happens on a per-frame basis, the size of which is
 * specified by frame_size.
 * @param      input_data       Samples to be decoded. The dimensions are
 *                              channel (for multi-channel audio), sample.
 * @param[out] converted_data   Converted samples. The dimensions are channel
 *                              (for multi-channel audio), sample.
 * @param      frame_size       Number of samples to be converted
 * @param      resample_context Resample context for the conversion
 * @return Error code (0 if successful)
 */
static int convert_samples(const uint8_t **input_data,
                           uint8_t **converted_data, 
                           const int src_frame_size,
                           const int dst_frame_size,
                           SwrContext *resample_context)
{
    int error;

    /* Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, dst_frame_size,
                             input_data    , src_frame_size)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                av_err2str(error));
        return error;
    }

    return 0;
}


static int convert_and_store(HIKEvent_DecodeThread *dp,
                                         AVFrame *input_frame,
                                         int *dst_nb_samples,
                                         int *finished)
{
    /* Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int ret = AVERROR_EXIT;

    /* If there is decoded data, convert and store it. */
    /* compute the number of converted samples: buffering is avoided
     * ensuring that the output buffer will contain at least all the
     * converted input samples */
    *dst_nb_samples =
        av_rescale_rnd(input_frame->nb_samples, dp->dst_rate, dp->src_rate, AV_ROUND_UP);

    /* Initialize the temporary storage for the converted input samples. */
    if (init_converted_samples(&converted_input_samples, dp->enc_ctx,
                               *dst_nb_samples))
        goto cleanup;

    /* Convert the input samples to the desired output sample format.
     * This requires a temporary storage provided by converted_input_samples. */
    if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,
                        input_frame->nb_samples, *dst_nb_samples, dp->swr))
        goto cleanup;

    /* Add the converted input samples to the FIFO buffer for later processing. */
    if (add_samples_to_fifo(dp->fifo, converted_input_samples,
                            *dst_nb_samples))
        goto cleanup;
    ret = 0;

cleanup:
    if (converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }

    return ret;
}

/**
 * Read one audio frame from the input file, decode, convert and store
 * it in the FIFO buffer.
 * @param      fifo                 Buffer used for temporary storage
 * @param      input_format_context Format context of the input file
 * @param      input_codec_context  Codec context of the input file
 * @param      output_codec_context Codec context of the output file
 * @param      resampler_context    Resample context for the conversion
 * @param[out] finished             Indicates whether the end of file has
 *                                  been reached and all data has been
 *                                  decoded. If this flag is false,
 *                                  there is more data to be decoded,
 *                                  i.e., this function has to be called
 *                                  again.
 * @return Error code (0 if successful)
 */
static int read_decode_convert_and_store(HIKEvent_DecodeThread *dp,
                                         AVPacket *input_packet,
                                         int *dst_nb_samples,
                                         int *finished)
{
    /* Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    int data_present = 0;
    int ret = AVERROR_EXIT;

    /* Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame))
        goto cleanup;
    /* Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, input_packet,
                           dp->dec_ctx, &data_present, finished))
        goto cleanup;
    /* If we are at the end of the file and there are no more samples
     * in the decoder which are delayed, we are actually finished.
     * This must not be treated as an error. */
    if (*finished) {
        ret = 0;
        goto cleanup;
    }
    /* If there is decoded data, convert and store it. */
    if (data_present) {
        ret = convert_and_store(dp, input_frame, dst_nb_samples, finished);
    }
    ret = 0;

cleanup:
    av_frame_free(&input_frame);

    return ret;
}


/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 * @param[out] frame                Frame to be initialized
 * @param      output_codec_context Codec context of the output file
 * @param      frame_size           Size of the frame
 * @return Error code (0 if successful)
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    /* Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        fprintf(stderr, "Could not allocate output frame samples (error '%s') nb_samples: %d  sample_rate: %d  format: %d\n",
                av_err2str(error), frame_size, output_codec_context->sample_rate, output_codec_context->sample_fmt);
        av_frame_free(frame);
        return error;
    }

    return 0;
}

/**
 * Encode one frame worth of audio to the output file.
 * @param      frame                 Samples to be encoded
 * @param      output_format_context Format context of the output file
 * @param      output_codec_context  Codec context of the output file
 * @param[out] data_present          Indicates whether data has been
 *                                   encoded
 * @return Error code (0 if successful)
 */
static int encode_audio_frame(HIKEvent_DecodeThread *dp,
                              AVFrame *frame,
                              AVPacket **pkt,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    /* Packet used for temporary storage. */
    AVPacket *output_packet;
    int error;

    error = init_packet(&output_packet);
    if (error < 0)
        return error;

    /* Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = dp->audio_pts;
        dp->audio_pts += frame->nb_samples;
    }

    /* Send the audio frame stored in the temporary packet to the encoder.
     * The output audio stream encoder is used to do this. */
    error = avcodec_send_frame(output_codec_context, frame);
    /* The encoder signals that it has nothing more to encode. */
    if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not send packet for encoding (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

    /* Receive one encoded frame from the encoder. */
    error = avcodec_receive_packet(output_codec_context, output_packet);
    /* If the encoder asks for more data to be able to provide an
     * encoded frame, return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
    /* If the last frame has been encoded, stop encoding. */
    } else if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not encode frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    /* Default case: Return encoded data. */
    } else {
        *data_present = 1;
    }

    /* Write one audio frame from the temporary packet to the output file. */
    if (*data_present)
    {
        *pkt = output_packet;
    }

cleanup:
    // av_packet_free(&output_packet);
    return error;
}


int init_audio_decoder(HIKEvent_DecodeThread *dp, AVStream *st)
{
    HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;
    int ret = 0;
    AVCodec *enc_codec;

    printf("init audio encoder  Audio Encode Type: %d\n", ps->compressAudioType.byAudioEncType);

    int sampleRate = ps->compressAudioType.byAudioSamplingRate;
    switch(ps->compressAudioType.byAudioSamplingRate)
    {
        case 1: sampleRate = 16000; break;
        case 2: sampleRate = 32000; break;
        case 3: sampleRate = 48000; break;
        case 4: sampleRate = 44100; break;
        case 5: sampleRate = 8000; break;
        default: sampleRate = 8000;
    }
    switch (ps->compressAudioType.byAudioEncType)
    {
    case 0: // Note: Not support
    case 9:
        dp->ps->transcode = -1;
        // st->codecpar->codec_id = AV_CODEC_ID_ADPCM_G722;
        sampleRate = 16000;
        st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        break;
    case 1: st->codecpar->codec_id = AV_CODEC_ID_PCM_MULAW; break;
    case 2: st->codecpar->codec_id = AV_CODEC_ID_PCM_ALAW; break;
    case 5: st->codecpar->codec_id = AV_CODEC_ID_MP2; break;
    case 6: st->codecpar->codec_id = AV_CODEC_ID_ADPCM_G726LE; break;
    case 7: st->codecpar->codec_id = AV_CODEC_ID_AAC; break;
    case 8: st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE; break;
    case 10:
        st->codecpar->codec_id = AV_CODEC_ID_G723_1; break;
    case 11:
        st->codecpar->codec_id = AV_CODEC_ID_G729; break;
    case 15: st->codecpar->codec_id = AV_CODEC_ID_MP3; break;
    // case 16: st->codecpar->codec_id = AV_CODEC_ID_ADPCM; break;
    }
    
    st->codecpar->sample_rate = sampleRate;
    st->codecpar->channels = 1;
    st->codecpar->channel_layout = av_get_default_channel_layout(st->codecpar->channels);

    AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        fprintf(stderr, "Failed to find %s codec\n",
                av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        goto end;
    }

    dp->dec_ctx = avcodec_alloc_context3(dec);
    if (!dp->dec_ctx) {
        fprintf(stderr, "Failed to allocate the %s codec context\n",
                av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        goto end;
    }

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(dp->dec_ctx, st->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        goto end;
    }

    /* Init the decoders */
    if ((ret = avcodec_open2(dp->dec_ctx, dec, NULL)) < 0) {
        fprintf(stderr, "Failed to open %s codec\n",
                av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        goto end;
    }


    if (dp->ps->transcode)
    {
        enc_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        // enc_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (!enc_codec) {
            fprintf(stderr, "Codec not found\n");
            goto end;
        }
        dp->enc_ctx = avcodec_alloc_context3(enc_codec);
        if (!dp->enc_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
            goto end;
        }
        // dp->enc_ctx->bit_rate = 96000;
        dp->enc_ctx->sample_fmt = enc_codec->sample_fmts[0];
        dp->enc_ctx->sample_rate = 44100;
        dp->enc_ctx->channels    = 1;
        dp->enc_ctx->channel_layout = av_get_default_channel_layout(1);
        dp->enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        dp->enc_ctx->time_base = (AVRational){1, dp->enc_ctx->sample_rate};

        dp->src_rate = st->codecpar->sample_rate;
        dp->dst_rate = dp->enc_ctx->sample_rate;

        // Set up SWR context once you've got codec information
        dp->swr = swr_alloc();
        av_opt_set_int(dp->swr, "in_channel_layout",  av_get_default_channel_layout(dp->dec_ctx->channels), 0);
        av_opt_set_int(dp->swr, "out_channel_layout", av_get_default_channel_layout(dp->enc_ctx->channels),  0);
        av_opt_set_int(dp->swr, "in_sample_rate",     dp->src_rate, 0);
        av_opt_set_int(dp->swr, "out_sample_rate",    dp->enc_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(dp->swr, "in_sample_fmt",  dp->dec_ctx->sample_fmt, 0);
        av_opt_set_sample_fmt(dp->swr, "out_sample_fmt", dp->enc_ctx->sample_fmt,  0);
        swr_init(dp->swr);

        /* open it */
        if ((ret = avcodec_open2(dp->enc_ctx, enc_codec, NULL)) < 0) {
            fprintf(stderr, "Could not open encode codec\n");
            goto end;
        }

        if (!(dp->fifo = av_audio_fifo_alloc(dp->enc_ctx->sample_fmt,
                                          dp->enc_ctx->channels, 1))) {
            ret = -1;
            fprintf(stderr, "Could not allocate FIFO\n");
            goto end;
        }
    }

    dp->out_astream = avformat_new_stream(dp->ps->plivectx, NULL);
    if (!dp->out_astream) {
        ret = -1;
        fprintf(stderr, "Failed allocating output stream\n");
        goto end;
    }

    if (dp->enc_ctx)
    {
        ret = avcodec_parameters_from_context(dp->out_astream->codecpar, dp->enc_ctx);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
        // dp->out_astream->codecpar->codec_tag = 0;
    } else 
    {
        ret = avcodec_parameters_copy(dp->out_astream->codecpar, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            return 1;
        }
        dp->out_astream->codecpar->codec_tag = 0;
    }

    av_dump_format(dp->pFormatCtx, 0, NULL, 0);
    av_dump_format(dp->ps->plivectx, 0, NULL, 1);
    
    if (!dp->outputHeaderWrite)
    {
        dp->outputHeaderWrite = true;
        ret = avformat_write_header(dp->ps->plivectx, NULL);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when opening output file\n");
            goto end;
        }
    }
    printf("init audio encoder ok\n");
    return ret;
end:
    if (dp->dec_ctx)
    {
        avcodec_free_context(&dp->dec_ctx);
        dp->dec_ctx = NULL;
    }
    if (dp->enc_ctx)
    {
        avcodec_free_context(&dp->enc_ctx);
        dp->enc_ctx = NULL;
    }
    if (dp->fifo)
    {
        av_audio_fifo_free(dp->fifo);
        dp->fifo = NULL;
    }
    if (dp->swr){
        swr_free(&dp->swr);
        dp->swr = NULL;
    }

    return ret;
}

void *process_thread(void *data)
{
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)data;
    HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;
    AVPacket *output_packet = NULL;
    int ret = 0;
    int bType = 0;

    while (keepRunning)
    {
        AVPacket *pkt = NULL;
        pthread_mutex_lock(&ps->lock);
        if (TAILQ_EMPTY(&ps->push_head) || !dp->probedone)
        {
            pthread_mutex_unlock(&ps->lock);
            usleep(1000);
            continue;
        } else 
        {
            struct entry *p = NULL;
            p = TAILQ_FIRST(&ps->push_head);
            bType = p->bType;
            pkt = (AVPacket *)p->data;
            TAILQ_REMOVE(&ps->push_head, p, entries);
            pthread_mutex_unlock(&ps->lock);
            free(p);
        }

        AVFormatContext *pFormatCtx = dp->pFormatCtx;
        AVStream *in_stream = NULL;

        if (bType == 0)
        {
            in_stream = pFormatCtx->streams[pkt->stream_index];
        } else 
        {
            in_stream = pFormatCtx->streams[1];
        }
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            AVStream *out_stream = dp->out_vstream;
            // if (dp->out_astream == NULL)
            // {
            //     printf("waiting audio pts = %ld \n", pkt->pts);
            //     av_packet_unref(pkt);
            //     av_packet_free(&pkt);
            //     continue;
            // }
            if (!dp->outputHeaderWrite)
            {
                dp->outputHeaderWrite = true;
                ret = avformat_write_header(ps->plivectx, NULL);
                if (ret < 0) {
                    fprintf(stderr, "Error occurred when opening output file\n");
                    goto end;
                }
            }
            if (ps->debug_packet)
                log_packet(pFormatCtx, pkt, 0);
            /* copy packet */
            av_packet_rescale_ts(pkt, in_stream->time_base, dp->out_vstream->time_base);
            if (dp->video_pts)
            {
                pkt->pts += dp->video_pts;
            }
            dp->global_pts = pkt->pts;
            pkt->pos = -1;
            pkt->stream_index = out_stream->index;

            ps->tx_size += pkt->buf ? pkt->buf->size : 0;
            if (ps->debug_packet)
                log_packet(ps->plivectx, pkt, 1);
            ret = av_interleaved_write_frame(ps->plivectx, pkt);
            if (ret < 0) {
                fprintf(stderr, "Error muxing packet %s\n", av_err2str(ret));
                break;
            }
            av_packet_unref(pkt);
            av_packet_free(&pkt);
        } else if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (dp->out_astream == NULL && init_audio_decoder(dp, in_stream))
            {
                break;
            }
            int finished                = 0;
            int dst_nb_samples          = 0;

            if (dp->ps->transcode)
            {
                /* Decode one frame worth of audio samples, convert it to the
                     * output sample format and put it into the FIFO buffer. */
                if (bType == 0)
                {
                    if (read_decode_convert_and_store(dp, pkt, &dst_nb_samples, &finished))
                        goto end;
                    if (ps->debug_packet)
                        log_packet(pFormatCtx, pkt, 0);
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                } else 
                {
                    AVFrame *frame = (AVFrame *)pkt;
                    if (convert_and_store(dp, frame, &dst_nb_samples, &finished))
                        goto end;
                    av_frame_free(&frame);
                }
                
                if (dp->audio_pts == 0)
                {
                    if (dp->global_pts)
                    {
                        dp->audio_pts = av_rescale_q_rnd(dp->global_pts, dp->out_vstream->time_base, dp->enc_ctx->time_base, AV_ROUND_UP);
                    } else 
                    {
                        // Wait global pts, cache the audio frame to FIFO queue
                        // dp->audio_pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, dp->enc_ctx->time_base, AV_ROUND_UP);
                        continue;
                    }
                }
                /* Use the encoder's desired frame size for processing. */
                // 
                const int output_frame_size = dp->enc_ctx->frame_size == 0 ? dst_nb_samples : dp->enc_ctx->frame_size;

                /* Make sure that there is one frame worth of samples in the FIFO
                 * buffer so that the encoder can do its work.
                 * Since the decoder's and the encoder's frame size may differ, we
                 * need to FIFO buffer to store as many frames worth of input samples
                 * that they make up at least one frame worth of output samples. */
                if (av_audio_fifo_size(dp->fifo) < output_frame_size) {
                    /* If we are at the end of the input file, we continue
                     * encoding the remaining audio samples to the output file. */
                    if (finished)
                        break;
                }

                /* If we have enough samples for the encoder, we encode them.
                 * At the end of the file, we pass the remaining samples to
                 * the encoder. */
                while (av_audio_fifo_size(dp->fifo) >= output_frame_size ||
                       (finished && av_audio_fifo_size(dp->fifo) > 0))
                {
                    if (!keepRunning)
                        break;
                    /* Temporary storage of the output samples of the frame written to the file. */
                    AVFrame *output_frame;

                    /* Use the maximum number of possible samples per frame.
                     * If there is less than the maximum possible frame size in the FIFO
                     * buffer use this number. Otherwise, use the maximum possible frame size. */
                    const int frame_size = FFMIN(av_audio_fifo_size(dp->fifo),
                                                 output_frame_size);
                    int data_written;

                    /* Initialize temporary storage for one output frame. */
                    if (init_output_frame(&output_frame, dp->enc_ctx, frame_size))
                        goto end;

                    /* Read as many samples from the FIFO buffer as required to fill the frame.
                     * The samples are stored in the frame temporarily. */
                    if (av_audio_fifo_read(dp->fifo, (void **)output_frame->data, frame_size) < frame_size) {
                        fprintf(stderr, "Could not read data from FIFO\n");
                        av_frame_free(&output_frame);
                        goto end;
                    }

                    output_packet = av_packet_alloc();
                    /* Encode one frame worth of audio samples. */
                    if (encode_audio_frame(dp, output_frame, &output_packet,
                                           dp->enc_ctx, &data_written)) {
                        av_frame_free(&output_frame);
                        goto end;
                    }
                    if (ps->debug_packet)
                        log_packet(pFormatCtx, output_packet, 0);
                    if (data_written)
                    {
                        output_packet->pos = -1;
                        output_packet->stream_index = dp->out_astream->index;

                        // Translate Encode timebase to outstream timebase
                        av_packet_rescale_ts(output_packet, dp->enc_ctx->time_base, dp->out_astream->time_base);

                        // log_packet(ps->plivectx, output_packet, 1);
                        ret = av_interleaved_write_frame(ps->plivectx, output_packet);
                        if (ret < 0) {
                            fprintf(stderr, "Error muxing packet\n");
                            goto end;
                        }
                        av_packet_unref(output_packet);
                        av_packet_free(&output_packet);
                    }
                    av_frame_free(&output_frame);
                }
            } else 
            {
                log_packet(pFormatCtx, pkt, 0);

                av_packet_rescale_ts(pkt,
                                     in_stream->time_base,
                                     dp->out_astream->time_base);
                pkt->pos = -1;
                pkt->stream_index = dp->out_astream->index;

                ps->tx_size += pkt->buf ? pkt->buf->size : 0;
                // log_packet(ps->plivectx, pkt, 1);
                int ret = av_interleaved_write_frame(ps->plivectx, pkt);
                if (ret < 0) {
                    fprintf(stderr, "Error muxing packet\n");
                    break;
                }
                av_packet_unref(pkt);
                av_packet_free(&pkt);
            }
        } else 
        {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
        }
    }
end:
    return NULL;
}

void *decode_thread(void *data)
{
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)data;
    HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;
    AVIOContext *pb = nullptr;
    AVFormatContext *pFormatCtx = NULL;

    //ffmpeg打开流的回调
    auto onReadData = [](void* pUser, uint8_t* buf, int bufSize)->int
    {
        HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)pUser;
        HIKEvent_Object *ps = dp->ps;

        while (keepRunning && ESRCH != pthread_kill(ps->decthread_ctx->process_thread, 0))
        {
            pthread_mutex_lock(&ps->lock);
            if (TAILQ_EMPTY(&ps->decode_head))
            {
                pthread_mutex_unlock(&ps->lock);
                usleep(1000);   // wait 1ms
            } else 
            {
                struct entry *p = TAILQ_FIRST(&ps->decode_head);
                if ((int)(p->dwBufLen - p->start) < bufSize)
                {
                    TAILQ_REMOVE(&ps->decode_head, p, entries);
                }
                pthread_mutex_unlock(&ps->lock);

                size_t wsize = (int)(p->dwBufLen - p->start) > bufSize ? bufSize : (int)(p->dwBufLen - p->start);
                memcpy(buf, p->data + p->start, wsize);
                p->start += wsize;
                
                if (p->dwBufLen - p->start == 0)
                {
                    free(p->data);
                    free(p);
                }
                return wsize;
            }

        }
        return 0;
    };
    size_t avio_ctx_buffer_size = 1024 * 1024;
    uint8_t* avio_ctx_buffer = nullptr;
    
    //ffmpeg-------------------------------
    avio_ctx_buffer = (uint8_t*)av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer)
    {
        fprintf(stderr, "av_malloc ctx buffer failed!");
        return NULL;
    }
    pb = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, dp, onReadData, NULL, NULL);
    if (pb == nullptr)  //分配空间失败
    {
        av_freep(&avio_ctx_buffer);
        fprintf(stderr, "avio_alloc_context failed");
        return NULL;
    }

    if (!(pFormatCtx = avformat_alloc_context())) {
        av_freep(&avio_ctx_buffer);
        fprintf(stderr, "avformat_alloc_context failed");
        return NULL;
    }

    dp->pFormatCtx = pFormatCtx;
    pFormatCtx->pb = pb;

    AVInputFormat *inputFmt = av_find_input_format("mpeg");
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_analyze_duration", "10", 0);
    av_dict_set(&opts, "max_delay", "1000", 0);
    av_dict_set(&opts, "stimeout", "1000000", 0);
    av_dict_set(&opts, "probesize", "4096", 0);             //加快打开
    av_dict_set(&opts, "max_probe_packets", "4", 0);             //加快打开

    if (avformat_open_input(&pFormatCtx, NULL, inputFmt, &opts) < 0)
    {
        if (nullptr != opts)
            av_dict_free(&opts);
        av_freep(&avio_ctx_buffer);
        avformat_free_context(pFormatCtx);
        fprintf(stderr, "avformat_open_input failed\n");
        return NULL;
    }
    if (nullptr != opts)
        av_dict_free(&opts);

    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        av_freep(&avio_ctx_buffer);
        avformat_close_input(&pFormatCtx);
        avformat_free_context(pFormatCtx);
        return NULL;
    }

    int ret;

    ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        goto end;
    } else {
        AVStream *st = pFormatCtx->streams[ret];
        AVStream *out_stream;

        out_stream = avformat_new_stream(ps->plivectx, NULL);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            return NULL;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            return NULL;
        }
        out_stream->codecpar->codec_tag = 0;
        dp->out_vstream = out_stream;
    }
    ret = -1;
    for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++)
    {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            ret = i;
            break;
        }
    }
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream, nb_streams: %d\n",
                av_get_media_type_string(AVMEDIA_TYPE_AUDIO), pFormatCtx->nb_streams);
    } else {
        AVStream *st = pFormatCtx->streams[ret];

        if (dp->out_astream == NULL)
        {
            if (init_audio_decoder(dp, st))
                return NULL;
        }
    }

    dp->probedone = true;

    while (keepRunning)
    {
        AVPacket *pkt = NULL;

        pkt = av_packet_alloc();
        int iRet = av_read_frame(pFormatCtx, pkt);
        //3-检查处理视频 处理重连机制
        if (iRet < 0)
        {
            if (iRet == AVERROR_EOF || !pFormatCtx->pb || avio_feof(pFormatCtx->pb) || pFormatCtx->pb->error)
            {
                fprintf(stderr, "Error av_read_frame : %s\n", av_err2str(iRet));
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                goto end;
            }
            usleep(1000);   // wait 1ms
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            continue;
        }

        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
        if (elem)
        {
            elem->bType = 0;
            elem->data = (char *)pkt;
            elem->dwBufLen = 0;
            pthread_mutex_lock(&ps->lock);
            TAILQ_INSERT_TAIL(&ps->push_head, elem, entries);
            pthread_mutex_unlock(&ps->lock);
        }
    }
end:
    // if (pkt)
    //     av_packet_free(&pkt);
    if (pb)
    {
        if (pb->buffer)
            av_freep(&pb->buffer);
        avio_context_free(&pb);
    }
    if (pFormatCtx)
    {
        avformat_close_input(&pFormatCtx);
        // avformat_free_context(pFormatCtx);
    }
    return NULL;
}

// GB28181 PSM packet
typedef struct 
{
    unsigned int packet_start_code_prefix:24;
    unsigned char map_stream_id;
    unsigned short psm_length;
    unsigned current_next_indictor  :1;
    unsigned reserved1              :2;
    unsigned psm_version            :5;
    unsigned reserved2              :7;
    unsigned marker_bit             :1;
    unsigned char psm_stream_info_len;
} __attribute__ ((packed)) program_stream_map_t;

// MEPG-1 PSM Header
typedef struct 
{
    uint32_t packet_start_code_prefix:32;   // 00 00 01 ba
    unsigned reserved1              :4;     // always 0010
    unsigned sys_clock_ref          :3;
    unsigned mark1                  :1;     // always set
    unsigned scr_1                 :8;
    unsigned scr_2                 :7;
    unsigned mark2                  :1;     // always set
    unsigned scr_3                 :8;
    unsigned scr_4                 :7;
    unsigned mark3                  :1;
    unsigned marker_bit             :1;
    unsigned multiple_rate         :22;
    unsigned mark4                  :1;
    unsigned char psm_stream_info_len;
} __attribute__ ((packed)) mpeg_psm_head_t;

typedef struct
{
    unsigned reserved1                     :2;     // always 10
    unsigned transport_scrambling_control  :2;     // 加扰控制
    unsigned priority                      :1;
    unsigned data_locate                   :1;
    unsigned copyright                     :1;
    unsigned original_or_copy              :1;

    unsigned PTS_DTS_flags                 :2;
    unsigned ESCR_flag                     :1;
    unsigned ES_rate_flag                  :1;
    unsigned DSM_trick_mode_flag           :1;
    unsigned additional_copy_info_flag     :1;
    unsigned PES_CRC_flag                  :1;
    unsigned PES_extension_flag            :1;

    unsigned char PES_header_data_length;
} __attribute__ ((packed)) mpeg_pes_head_t;

typedef struct {
    unsigned PES_private_data_flag              :1;
    unsigned pack_header_field_flag             :1;
    unsigned program_packet_sequence_counter_flag :1;
    unsigned P_STD_buffer_flag       :1;
    unsigned reserved                :3;
    unsigned PES_extension_flag_2    :1;
} __attribute__ ((packed)) mpeg_pes_extension_t;

void dump_hex(unsigned char *buf, DWORD len)
{
    for (DWORD i = 0; i < len; i++) {
        if (i % 16 == 0)
        {
            printf("\n%08x: ", i);
        }
        printf("%02x ", buf[i]);
    }
        printf("\n");
}

void CALLBACK g_RealDataCallBack_V30(LONG lRealHandle, DWORD dwDataType, BYTE *pBuffer,DWORD dwBufSize,void* dwUser) {
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)dwUser;
    HIKEvent_Object *ps = dp->ps;
        static FILE *fp = fopen("test.ps", "wb");
    // int dRet;

    switch (dwDataType) {
        case NET_DVR_SYSHEAD: //系统头
        case NET_DVR_AUDIOSTREAMDATA:   // 音频数据
        {
            printf("dwDataType: NET_DVR_AUDIOSTREAMDATA\n");
            struct entry *elem;
            char *buf;

            elem = (struct entry *)calloc(1, sizeof(struct entry));
            if (!elem) {
                fprintf(stderr, "allocate storage element failed\n");
                return;
            }
            elem->data = (char *)malloc(dwBufSize);
            buf = elem->data;

            elem->bType = 0;
            memcpy(buf, pBuffer, dwBufSize);
            elem->dwBufLen += dwBufSize;
            pthread_mutex_lock(&ps->lock);
            TAILQ_INSERT_TAIL(&ps->decode_head, elem, entries);
            pthread_mutex_unlock(&ps->lock);
                fwrite(elem->data, elem->dwBufLen, 1, fp);
                fflush(fp);
            break;
        }
        case NET_DVR_STREAMDATA: //码流数据
        {
            program_stream_map_t *psm = (program_stream_map_t *)pBuffer;
            if (psm->packet_start_code_prefix == 0x010000)
            {
                switch (psm->map_stream_id)
                {
                    case 0xc0:  // Audio
                        // printf("[audio] raw: size = %d\n", dwBufSize);
                        // for (DWORD i = 0; i < dwBufSize; i++) printf("%02x ", pBuffer[i]);
                        //     printf("\n");

                        break;
                    case 0xe0:  // Video
                        // printf("[video] raw: size = %d\n", dwBufSize);
                        // break;
                    case 0xba:  // BSH Header
                        // mpeg_psm_head_t *head = (mpeg_psm_head_t *)pBuffer;
                        // printf("[head ] raw: size = %d  scr = %d  multiple_rate = %d\n", dwBufSize, (head->scr_1 << 22) | (head->scr_2 << 14) | (head->scr_3 << 7) | head->scr_4, head->multiple_rate);
                        break;
                }
            } else 
            {
                printf("ERR %d\n", psm->packet_start_code_prefix);
                for (DWORD i = 0; i < dwBufSize; i++) printf("%02x ", pBuffer[i]);
                    printf("\n");
            }

            if (psm->packet_start_code_prefix == 0x010000)
            {
                switch (psm->map_stream_id)
                {
                    case 0xc0:  // Audio
                        ps->rx_size += dwBufSize;
                        if (dp->ps->transcode == -1 && dwBufSize == 96)
                        {
                            mpeg_pes_head_t *pes_head = (mpeg_pes_head_t *)(pBuffer + 6);

                            /* frame containing input raw audio */
                            AVFrame *frame = av_frame_alloc();
                            if (!frame) {
                                fprintf(stderr, "Could not allocate audio frame\n");
                                return ;
                            }

                            frame->nb_samples     = 640;
                            frame->format         = AV_SAMPLE_FMT_S16;
                            frame->channel_layout = av_get_default_channel_layout(1);

                            /* allocate the data buffers */
                            int ret = av_frame_get_buffer(frame, 0);
                            if (ret < 0) {
                                fprintf(stderr, "Could not allocate audio data buffers\n");
                                return;
                            }
                            ret = av_frame_make_writable(frame);
                            if (ret < 0)
                                return;

                            NET_DVR_AUDIODEC_PROCESS_PARAM decodeParam;
                            decodeParam.in_buf = (unsigned char *)pBuffer + 16;
                            decodeParam.in_data_size = 80;
                            decodeParam.dec_info.nchans = 1;
                            decodeParam.dec_info.sample_rate = 16000;
                            decodeParam.out_buf = frame->data[0];
                            if (!NET_DVR_DecodeG722Frame(dp->g722_decoder, &decodeParam))
                            {
                                LONG pErrorNo = NET_DVR_GetLastError();
                                fprintf(stderr, "NET_DVR_DecodeG722Frame error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
                            }

                            struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                            if (elem)
                            {
                                elem->bType = 1;
                                elem->data = (char *)frame;
                                elem->dwBufLen = 0;
                                pthread_mutex_lock(&ps->lock);
                                TAILQ_INSERT_TAIL(&ps->push_head, elem, entries);
                                pthread_mutex_unlock(&ps->lock);
                            }

                            break;
                        }
                    case 0xba:  // PSH Header
                    case 0xe0:  // Video
                    {
                        ps->rx_size += dwBufSize;
                        // unsigned short psm_stream_info_len = htons(psm->psm_stream_info_len);
                        // unsigned short stream_map_length = htons(*(uint16_t *)(pBuffer + sizeof(program_stream_map_t) + psm->psm_stream_info_len));
                        // printf("rx video psm_stream_info_len: %d  stream_map_length: %d data size %d\n", psm_stream_info_len, stream_map_length, dwBufSize);
                        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                        if (elem)
                        {
                            elem->bType = 0;
                            elem->data = (char *)malloc(dwBufSize);
                            memcpy(elem->data, pBuffer, dwBufSize);
                            elem->dwBufLen = dwBufSize;
                            pthread_mutex_lock(&ps->lock);
                            TAILQ_INSERT_TAIL(&ps->decode_head, elem, entries);
                            pthread_mutex_unlock(&ps->lock);
                        }
                        break;
                    }
                    case 0xbc:  // PSM Header
                        // https://blog.csdn.net/fanyun_01/article/details/120537670
                        printf("rx data  %08x \n", htonl(*(uint32_t *)pBuffer));
                        break;
                    case 0xbd:  // Private Stream Data
                        break;
                    default:
                        printf("rx data  %08x \n", htonl(*(uint32_t *)pBuffer));
                }
            }
            break; 
        }
        default: //其他数据
            fprintf(stderr, "Other data,the size is %ld,%d.\n", time(NULL), dwBufSize);
            break;
    }
}

int main(int argc, char **argv)
{
    AVIOContext *pOutputIO = nullptr;
    HIKEvent_Object *ps = (HIKEvent_Object *)calloc(1, sizeof(HIKEvent_Object));
    ps->transcode = 1;

    av_log_set_level( AV_LOG_DEBUG);
	char c;
    while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
        switch (c) {
        case 'h':
            strcpy(host, optarg);
            break;
        case 'u':
            strcpy(user, optarg);
            break;
        case 'p':
            strcpy(passwd, optarg);
            break;
        case 'c':
            cameraNo = atoi(optarg);
            break;
        case 's':
            streamType = atoi(optarg);
            break;
        case 'N':
            ps->transcode = 0;
            break;
        case 'v':
            ps->debug_packet = 1;
            break;
        case '?': // 输入未定义的选项, 都会将该选项的值变为 ?
            fprintf(stderr, "Usage: hikrtmp -h <host> -u <user> -p <passwd> -c <camera channel> [-s <stream type>] [-Nvh] <rtmp url>\n");
            fprintf(stderr, "Convert Hikvision PS stream to RTMP\n");
            fprintf(stderr, "  \n");
            fprintf(stderr, "Options:\n");
            fprintf(stderr, "  -h --host            NVR/DVR/Camera IP\n");
            fprintf(stderr, "  -u --user            NVR/DVR/Camera Username\n");
            fprintf(stderr, "  -p --passwd          NVR/DVR/Camera Password\n");
            fprintf(stderr, "  -c --camera          NVR/DVR/Camera Camera Channel\n");
            fprintf(stderr, "  -s --stream-type     NVR/DVR/Camera Stream Type (0: Main Stream 1: Sub Stream...)\n");
            fprintf(stderr, "  -N --ignore-acodec   Copy acodec without transcode to AAC\n");
            fprintf(stderr, "  -v --verbose         Log frames\n");
            fprintf(stderr, "  -? --help            Show help\n\n");
            // fprintf(stderr,"unknown option \n");
            break;
        default:
            fprintf(stderr, "Unimplemented option -- %c\n", c);
            return 1;
        }
    }

    argc -= optind;
    argv += optind;
    if (argc == 0)
    {
    	fprintf(stderr, "no rtmp url\n");
    	return 1;
    }
    printf("rtmp url: %s\n", argv[0]);

    signal(SIGINT, intHandler);

    if (pthread_mutex_init(&ps->lock, NULL) != 0) {
        fprintf(stderr, "mutex init has failed");
        return 1;
    }

    TAILQ_INIT(&ps->decode_head);
    TAILQ_INIT(&ps->push_head);

    // 初始化
    NET_DVR_Init();
    //设置连接时间与重连时间
    NET_DVR_SetConnectTime(2000, 1);
    NET_DVR_SetReconnect(10000, true);

    ps->struLoginInfo.bUseAsynLogin = false;

    ps->struLoginInfo.wPort = 8000;
    memcpy(ps->struLoginInfo.sDeviceAddress, host, NET_DVR_DEV_ADDRESS_MAX_LEN);
    memcpy(ps->struLoginInfo.sUserName, user, strlen(user) > NAME_LEN ? NAME_LEN : strlen(user));
    memcpy(ps->struLoginInfo.sPassword, passwd, strlen(passwd) > NAME_LEN ? NAME_LEN : strlen(passwd));

    ps->lUserID = NET_DVR_Login_V40(&ps->struLoginInfo, &ps->struDeviceInfoV40);
    if (ps->lUserID < 0)
    {
        fprintf(stderr, "Login error, %d\n", NET_DVR_GetLastError());
        NET_DVR_Cleanup(); 
        return 1;
    }

    ps->decthread_ctx = (HIKEvent_DecodeThread *)calloc(1, sizeof(HIKEvent_DecodeThread));
    ps->decthread_ctx->ps = ps;
    pthread_create(&ps->decthread_ctx->thread, NULL, decode_thread, ps->decthread_ctx);
    pthread_create(&ps->decthread_ctx->process_thread, NULL, process_thread, ps->decthread_ctx);

    int error;
    if ((error = avio_open(&pOutputIO, argv[0],
                           AVIO_FLAG_READ_WRITE)) < 0) {
        fprintf(stderr, "Could not open output file '%s' (error)\n",
                argv[0]);
        return error;
    }

    /* Create a new format context for the output container format. */
    if (!(ps->plivectx = avformat_alloc_context())) {
        fprintf(stderr, "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    /* Associate the output file (pointer) with the container format context. */
    ps->plivectx->pb = pOutputIO;
    if (!(ps->plivectx->oformat = av_guess_format("flv", NULL,
                                                              NULL))) {
        fprintf(stderr, "Could not find output file format\n");
        return 1;
    }

    NET_DVR_AUDIO_CHANNEL channelInfo;
    memset(&channelInfo, 0, sizeof(NET_DVR_AUDIO_CHANNEL));

    channelInfo.dwChannelNum = ps->struDeviceInfoV40.struDeviceV30.byStartDTalkChan + cameraNo - 1;
    if (FALSE == NET_DVR_GetCurrentAudioCompress_V50(ps->lUserID, &channelInfo, &ps->compressAudioType))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_GetCurrentAudioCompress error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        return pErrorNo;
    }

    if (ps->compressAudioType.byAudioEncType == 0)
    {
        // Require G722 Decoder
        ps->decthread_ctx->g722_decoder = NET_DVR_InitG722Decoder();
    }

    // NET_DVR_XML_CONFIG_INPUT inputParam;
    // NET_DVR_XML_CONFIG_OUTPUT outputParam;

    // memset(&inputParam, 0, sizeof(NET_DVR_XML_CONFIG_INPUT));
    // inputParam.dwSize = sizeof(NET_DVR_XML_CONFIG_INPUT);
    
    // memset(&outputParam, 0, sizeof(NET_DVR_XML_CONFIG_OUTPUT));
    // outputParam.dwSize = sizeof(NET_DVR_XML_CONFIG_OUTPUT);
    // inputParam.lpRequestUrl = (char *)malloc(1024);
    // inputParam.lpInBuffer = (char *)malloc(4096);
    // sprintf((char *)inputParam.lpRequestUrl, "PUT /ISAPI/System/TwoWayAudio/channels/%d", cameraNo);
    // inputParam.dwRequestUrlLen = strlen((char *)inputParam.lpRequestUrl);
    // strcpy((char *)inputParam.lpInBuffer, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><TwoWayAudioChannel><id>1</id><enabled>true</enabled><audioCompressionType>G.711alaw</audioCompressionType></TwoWayAudioChannel>");
    // inputParam.dwInBufferSize = strlen((char *)inputParam.lpInBuffer);

    // outputParam.dwOutBufferSize = 1048576 * 32;    // 32 MB
    // outputParam.lpOutBuffer = malloc(outputParam.dwOutBufferSize);
    // outputParam.dwStatusSize = 1048576;    // 1 MB
    // outputParam.lpStatusBuffer = malloc(outputParam.dwStatusSize);
    // if (!NET_DVR_STDXMLConfig(ps->lUserID, &inputParam, &outputParam))
    // {
    //     LONG pErrorNo = NET_DVR_GetLastError();
    //     fprintf(stderr, "NET_DVR_STDXMLConfig error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
    //     free(outputParam.lpStatusBuffer);
    //     free(outputParam.lpOutBuffer);

    //     return pErrorNo;
    // }
    
    NET_DVR_PREVIEWINFO struPlayInfo = {0};
    struPlayInfo.hPlayWnd     = 0;  // 仅取流不解码。这是Linux写法，Windows写法是struPlayInfo.hPlayWnd = NULL;
    struPlayInfo.lChannel     = cameraNo; // 通道号
    struPlayInfo.dwStreamType = streamType;  // 0- 主码流，1-子码流，2-码流3，3-码流4，以此类推
    struPlayInfo.dwLinkMode   = 0;  // 0- TCP方式，1- UDP方式，2- 多播方式，3- RTP方式，4-RTP/RTSP，5-RSTP/HTTP
    struPlayInfo.bBlocked     = 1;  // 0- 非阻塞取流，1- 阻塞取流
    //struPlayInfo.dwDisplayBufNum = 1;

    long lRealPlayHandle = NET_DVR_RealPlay_V40(ps->lUserID, &struPlayInfo, g_RealDataCallBack_V30, ps->decthread_ctx); // NET_DVR_RealPlay_V40 实时预览（支持多码流）。
    //lRealPlayHandle = NET_DVR_RealPlay_V30(lUserID, &ClientInfo, NULL, NULL, 0); // NET_DVR_RealPlay_V30 实时预览。
    if (lRealPlayHandle < 0) {
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        NET_DVR_Cleanup();
        return 1;
    }
    
    ps->last_reset = microtime();
    while (keepRunning && ESRCH != pthread_kill(ps->decthread_ctx->thread, 0) && ESRCH != pthread_kill(ps->decthread_ctx->process_thread, 0))
    {
        double now = microtime();
        if (now - ps->last_reset > 1)
        {
            printf("rx %-10d bytes  %.2f kbps  tx %.2f kbps      \n", ps->rx_size, ps->rx_size * 8.0 / 1024 / (now - ps->last_reset), ps->tx_size * 8.0 / 1024 / (now - ps->last_reset));
            fflush(stdout);
            ps->rx_size = 0;
            ps->tx_size = 0;
            ps->last_reset = now;
        }
        usleep(100000);   // wait 100ms
    }
    if (false == NET_DVR_StopRealPlay(lRealPlayHandle)) {    // 停止取流
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        NET_DVR_Cleanup();
        return 1;
    }

    if (ps->plivectx && ps->decthread_ctx->outputHeaderWrite)
        av_write_trailer(ps->plivectx);

    if (ps->plivectx && !(ps->plivectx->flags & AVFMT_NOFILE))
        avio_closep(&ps->plivectx->pb);
    avformat_free_context(ps->plivectx);

    if (ps->decthread_ctx->g722_decoder)
        NET_DVR_ReleaseG722Decoder(ps->decthread_ctx->g722_decoder);

	return 0;
}