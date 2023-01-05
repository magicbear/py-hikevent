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
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

}

char		host[256] 	= {0};
char		user[256] 	= {0};
char		passwd[256] = {0};
char        STREAM_VIDEO_CODEC[256] = {0};
char        STREAM_AUDIO_CODEC[256] = {0};
int  		cameraNo;
int 		streamType;

static const char *shortopts = ":h:u:p:c:s:";

static struct option longopts[] = {
  {"host",          1, 0, 'h'},
  {"user",          1, 0, 'u'},
  {"passwd", 	    1, 0, 'p'},
  {"camera",        1, 0, 'c'},
  {"stream-type",   1, 0, 's'},

  {0, 0, 0, 0}
};

static volatile int keepRunning = 1;

void intHandler(int dummy) {
    keepRunning = 0;
}

struct entry {
    long lCommand;
    char *pAlarmInfo;
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
    NET_DVR_USER_LOGIN_INFO struLoginInfo = {0};
    NET_DVR_DEVICEINFO_V40 struDeviceInfoV40 = {0};

    LONG nPort;

    uint32_t rx_size;
    uint32_t tx_size;
    double   last_reset;

    struct HIKEvent_DecodeThread *dec_video_ctx;
    struct HIKEvent_DecodeThread *dec_audio_ctx;

    pthread_mutex_t lock;
    TAILQ_HEAD(decode_video_tailhead, entry) decode_video_head;
    TAILQ_HEAD(decode_audio_tailhead, entry) decode_audio_head;
    TAILQ_HEAD(push_tailhead, entry) push_head;

    AVFormatContext *plivectx = nullptr;
    /* Type-specific fields go here. */
} HIKEvent_Object;

typedef struct HIKEvent_DecodeThread {
    HIKEvent_Object *ps;
    pthread_t thread;
    int decode_type;
    AVStream *out_stream;
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
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished)
{
    /* Packet used for temporary storage. */
    AVPacket *input_packet;
    int error;

    error = init_packet(&input_packet);
    if (error < 0)
        return error;

    /* Read one audio frame from the input file into a temporary packet. */
    if ((error = av_read_frame(input_format_context, input_packet)) < 0) {
        /* If we are at the end of the file, flush the decoder below. */
        if (error == AVERROR_EOF)
            *finished = 1;
        else {
            fprintf(stderr, "Could not read frame (error '%s')\n",
                    av_err2str(error));
            goto cleanup;
        }
    }

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
    av_packet_free(&input_packet);
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
                           uint8_t **converted_data, const int frame_size,
                           SwrContext *resample_context)
{
    int error;

    /* Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, frame_size,
                             input_data    , frame_size)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                av_err2str(error));
        return error;
    }

    return 0;
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
static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resampler_context,
                                         int *finished)
{
    /* Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    /* Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int data_present = 0;
    int ret = AVERROR_EXIT;

    /* Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame))
        goto cleanup;
    /* Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, input_format_context,
                           input_codec_context, &data_present, finished))
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
        /* Initialize the temporary storage for the converted input samples. */
        if (init_converted_samples(&converted_input_samples, output_codec_context,
                                   input_frame->nb_samples))
            goto cleanup;

        /* Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples. */
        if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,
                            input_frame->nb_samples, resampler_context))
            goto cleanup;

        /* Add the converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, converted_input_samples,
                                input_frame->nb_samples))
            goto cleanup;
        ret = 0;
    }
    ret = 0;

cleanup:
    if (converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }
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
        fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
                av_err2str(error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}


/* Global timestamp for the audio frames. */
static int64_t audio_pts = 0;

/**
 * Encode one frame worth of audio to the output file.
 * @param      frame                 Samples to be encoded
 * @param      output_format_context Format context of the output file
 * @param      output_codec_context  Codec context of the output file
 * @param[out] data_present          Indicates whether data has been
 *                                   encoded
 * @return Error code (0 if successful)
 */
static int encode_audio_frame(AVFrame *frame,
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
        frame->pts = audio_pts;
        audio_pts += frame->nb_samples;
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

        while (keepRunning)
        {
            pthread_mutex_lock(&ps->lock);
            if ((dp->decode_type == 0 && TAILQ_EMPTY(&ps->decode_video_head)) || (dp->decode_type == 1 && TAILQ_EMPTY(&ps->decode_audio_head)))
            {
                pthread_mutex_unlock(&ps->lock);
                sleep(0.02);
            } else 
            {
                struct entry *p = dp->decode_type == 0 ? TAILQ_FIRST(&ps->decode_video_head) : TAILQ_FIRST(&ps->decode_audio_head);
                if ((int)(p->dwBufLen - p->lCommand) < bufSize)
                {
                    if (dp->decode_type == 0)
                    {
                        TAILQ_REMOVE(&ps->decode_video_head, p, entries);
                    } else 
                    {
                        TAILQ_REMOVE(&ps->decode_audio_head, p, entries);
                    }
                }
                pthread_mutex_unlock(&ps->lock);
                
                size_t wsize = (int)(p->dwBufLen - p->lCommand) > bufSize ? bufSize : (int)(p->dwBufLen - p->lCommand);
                memcpy(buf, p->pAlarmInfo + p->lCommand, wsize);
                p->lCommand += wsize;
                
                if (p->dwBufLen - p->lCommand == 0)
                {
                    free(p->pAlarmInfo);
                    free(p);
                }
                return wsize;
            }

        }
        return 0;
    };
    size_t avio_ctx_buffer_size = 1024 * 1024;
    uint8_t* avio_ctx_buffer = nullptr;
    int64_t start_pts = 0, start_dts = 0;
    
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

    pFormatCtx->pb = pb;

    AVInputFormat *inputFmt = NULL;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_analyze_duration", "10", 0);
    av_dict_set(&opts, "max_delay", "1000", 0);
    av_dict_set(&opts, "stimeout", "1000000", 0);
    av_dict_set(&opts, "probesize", "4096", 0);             //加快打开
    if (dp->decode_type == 1)
    {
        inputFmt = av_find_input_format("alaw");
        av_dict_set(&opts, "sample_rate", "8000", 0);
    }
    
    // 
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

    AVCodecContext *dec_ctx = NULL, *enc_ctx = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr = NULL;
    AVCodec *enc_codec = NULL;
    AVAudioFifo *fifo = NULL;
    /* Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int ret;

    // for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++)
    // {
    //     printf("Stream %d: %s\n", i, avcodec_get_name(pFormatCtx->streams[i]->codec->codec_id));
    // }

    if (dp->decode_type == 0)
    {
        int ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
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
            dp->out_stream = out_stream;
        }
    } else 
    {
        int ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (ret < 0) {
            fprintf(stderr, "Could not find %s stream\n",
                    av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        } else {
            // video_stream_idx = ret;
            AVStream *st = pFormatCtx->streams[ret];
            AVStream *out_stream;

            AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!dec) {
                fprintf(stderr, "Failed to find %s codec\n",
                        av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                goto end;
            }

            dec_ctx = avcodec_alloc_context3(dec);
            if (!dec_ctx) {
                fprintf(stderr, "Failed to allocate the %s codec context\n",
                        av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
                goto end;
            }

            /* Copy codec parameters from input stream to output codec context */
            if ((ret = avcodec_parameters_to_context(dec_ctx, st->codecpar)) < 0) {
                fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                        av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                goto end;
            }

            /* Init the decoders */
            if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
                fprintf(stderr, "Failed to open %s codec\n",
                        av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                goto end;
            }

            av_dump_format(pFormatCtx, 0, NULL, 0);

            // Set up SWR context once you've got codec information
            swr = swr_alloc();
            av_opt_set_int(swr, "in_channel_layout",  av_get_default_channel_layout(dec_ctx->channels), 0);
            av_opt_set_int(swr, "out_channel_layout", av_get_default_channel_layout(1),  0);
            av_opt_set_int(swr, "in_sample_rate",     dec_ctx->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate",    44100, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt",  dec_ctx->sample_fmt, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP,  0);
            swr_init(swr);

            enc_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (!enc_codec) {
                fprintf(stderr, "Codec not found\n");
                goto end;
            }
            enc_ctx = avcodec_alloc_context3(enc_codec);
            if (!enc_ctx) {
                fprintf(stderr, "Failed to allocate the %s codec context\n",
                        av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
                goto end;
            }
            enc_ctx->bit_rate = 96000;
            enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            enc_ctx->sample_rate = 44100;
            enc_ctx->channels    = 1;
            enc_ctx->channel_layout = av_get_default_channel_layout(1);
            enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

            /* open it */
            if (avcodec_open2(enc_ctx, enc_codec, NULL) < 0) {
                fprintf(stderr, "Could not open encode codec\n");
                goto end;
            }

            if (!(fifo = av_audio_fifo_alloc(enc_ctx->sample_fmt,
                                              enc_ctx->channels, 1))) {
                fprintf(stderr, "Could not allocate FIFO\n");
                goto end;
            }

            /* frame containing input raw audio */
            frame = av_frame_alloc();
            if (!frame) {
                fprintf(stderr, "Could not allocate audio frame\n");
                goto end;
            }
            // frame->nb_samples     = dec_ctx->frame_size;
            // frame->format         = dec_ctx->sample_fmt;
            
            // /* allocate the data buffers */
            // ret = av_frame_get_buffer(frame, 0);
            // if (ret < 0) {
            //     fprintf(stderr, "Could not allocate audio data buffers\n");
            //     exit(1);
            // }

            out_stream = avformat_new_stream(ps->plivectx, NULL);
            if (!out_stream) {
                fprintf(stderr, "Failed allocating output stream\n");
                goto end;
            }

            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                fprintf(stderr, "Failed to copy codec parameters\n");
                goto end;
            }
            out_stream->codecpar->codec_tag = 0;
            dp->out_stream = out_stream;
        }
    }

    av_dump_format(pFormatCtx, 0, NULL, 0);
    dp->probedone = true;

    while (keepRunning)
    {
        if (dp->decode_type == 0)
        {
            AVPacket *pkt = av_packet_alloc();
            int iRet = av_read_frame(pFormatCtx, pkt);
            //3-检查处理视频 处理重连机制
            if (iRet < 0)
            {
                if (iRet == AVERROR_EOF || !pFormatCtx->pb || avio_feof(pFormatCtx->pb) || pFormatCtx->pb->error)
                {
                    fprintf(stderr, "Error\n");
                    av_packet_free(&pkt);
                    goto end;
                }
                continue;
            }
            AVStream *in_stream = pFormatCtx->streams[pkt->stream_index];
            if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) // in_stream->codecpar->codec_id == AV_CODEC_ID_H264)
            {
                AVStream *out_stream = dp->out_stream;
                /* copy packet */
                pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
                pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
                if (pkt->pts < 0)
                {
                    printf("ignore\n");
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                    continue;
                }
                if ((start_pts == 0 || start_dts == 0) && pkt->pts > 0)
                {
                    start_pts = pkt->pts;
                    start_dts = pkt->dts;
                }
                pkt->pts -= start_pts;
                pkt->dts -= start_dts;
                printf("%d write stream: %d pts = %d  %d\n", dp->decode_type, out_stream->index, pkt->pts, start_pts);
                pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
                pkt->pos = -1;
                pkt->stream_index = out_stream->index;

                ps->rx_size += pkt->buf ? pkt->buf->size : 0;

                struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                if (elem)
                {
                    elem->lCommand = time(NULL);
                    elem->pAlarmInfo = (char *)pkt;
                    elem->dwBufLen = pkt->buf->size;
                    pthread_mutex_lock(&ps->lock);
                    TAILQ_INSERT_TAIL(&ps->push_head, elem, entries);
                    pthread_mutex_unlock(&ps->lock);
                }
            } else 
            {
                av_packet_unref(pkt);
                av_packet_free(&pkt);
            }
        } else {
            /* Use the encoder's desired frame size for processing. */
            const int output_frame_size = enc_ctx->frame_size;
            int finished                = 0;
            AVPacket *pkt = NULL;

            /* Make sure that there is one frame worth of samples in the FIFO
             * buffer so that the encoder can do its work.
             * Since the decoder's and the encoder's frame size may differ, we
             * need to FIFO buffer to store as many frames worth of input samples
             * that they make up at least one frame worth of output samples. */
            while (av_audio_fifo_size(fifo) < output_frame_size) {
                /* Decode one frame worth of audio samples, convert it to the
                 * output sample format and put it into the FIFO buffer. */
                if (read_decode_convert_and_store(fifo, pFormatCtx,
                                                  dec_ctx,
                                                  enc_ctx,
                                                  swr, &finished))
                    goto end;

                /* If we are at the end of the input file, we continue
                 * encoding the remaining audio samples to the output file. */
                if (finished)
                    break;
            }

            /* If we have enough samples for the encoder, we encode them.
             * At the end of the file, we pass the remaining samples to
             * the encoder. */
            while (av_audio_fifo_size(fifo) >= output_frame_size ||
                   (finished && av_audio_fifo_size(fifo) > 0))
            {
                /* Temporary storage of the output samples of the frame written to the file. */
                AVFrame *output_frame;

                /* Use the maximum number of possible samples per frame.
                 * If there is less than the maximum possible frame size in the FIFO
                 * buffer use this number. Otherwise, use the maximum possible frame size. */
                const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                             enc_ctx->frame_size);
                int data_written;

                /* Initialize temporary storage for one output frame. */
                if (init_output_frame(&output_frame, enc_ctx, frame_size))
                    goto end;

                /* Read as many samples from the FIFO buffer as required to fill the frame.
                 * The samples are stored in the frame temporarily. */
                if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
                    fprintf(stderr, "Could not read data from FIFO\n");
                    av_frame_free(&output_frame);
                    goto end;
                }

                /* Encode one frame worth of audio samples. */
                if (encode_audio_frame(output_frame, &pkt,
                                       enc_ctx, &data_written)) {
                    av_frame_free(&output_frame);
                    goto end;
                }
                av_frame_free(&output_frame);

                if (pkt != NULL)
                {
                    AVStream *out_stream = dp->out_stream;
                    // /* copy packet */
                    // pkt->pts = av_rescale_q_rnd(pkt->pts, enc_ctx->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
                    // pkt->dts = av_rescale_q_rnd(pkt->dts, enc_ctx->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
                    // pkt->duration = av_rescale_q(pkt->duration, enc_ctx->time_base, out_stream->time_base);
                    // pkt->pos = -1;
                    printf("%d write stream: %d pts = %ld\n", dp->decode_type, out_stream->index, pkt->pts);
                    // pkt->stream_index = out_stream->index;

                    ps->rx_size += pkt->buf ? pkt->buf->size : 0;

                    struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                    if (elem)
                    {
                        elem->lCommand = time(NULL);
                        elem->pAlarmInfo = (char *)pkt;
                        elem->dwBufLen = pkt->buf->size;
                        pthread_mutex_lock(&ps->lock);
                        TAILQ_INSERT_TAIL(&ps->push_head, elem, entries);
                        pthread_mutex_unlock(&ps->lock);
                    }
                }
            }
            
        }
        // ret = av_interleaved_write_frame(ps->plivectx, pkt);
        // if (ret < 0) {
        //     fprintf(stderr, "Error muxing packet\n");
        //     break;
        // }
        // av_packet_unref(pkt);

    }
end:
    // if (pkt)
    //     av_packet_free(&pkt);
    if (frame)
        av_frame_free(&frame);
    if (dec_ctx)
        avcodec_free_context(&dec_ctx);

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

void CALLBACK g_RealDataCallBack_V30(LONG lRealHandle, DWORD dwDataType, BYTE *pBuffer,DWORD dwBufSize,void* dwUser) {
    HIKEvent_Object *ps = (HIKEvent_Object *)dwUser;

    switch (dwDataType) {
        case NET_DVR_SYSHEAD: //系统头
        case NET_DVR_STREAMDATA: //码流数据
        case NET_DVR_AUDIOSTREAMDATA:   // 音频数据
        {
            program_stream_map_t *psm = (program_stream_map_t *)pBuffer;
            if (psm->packet_start_code_prefix == 0x010000)
            {
                switch (psm->map_stream_id)
                {
                    case 0xc0:  // Audio
                    {
                        // htons(psm->psm_length) + 6 == dwBufSize
                        unsigned short stream_map_length = htons(*(uint16_t *)(pBuffer + sizeof(program_stream_map_t) + psm->psm_stream_info_len));
                        // printf("rx audio psm_length: %d psm_stream_info_len: %d  stream_map_length: %d data size %d\n",
                        //     htons(psm->psm_length), psm->psm_stream_info_len, stream_map_length, dwBufSize);
                        // for (int i = 0; i < dwBufSize; i++) printf("%02x ", pBuffer[i]); printf("\n");
                        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                        if (elem)
                        {
                            elem->lCommand = 0;
                            elem->pAlarmInfo = (char *)malloc(dwBufSize);
                            elem->dwBufLen = dwBufSize - (8 + psm->psm_stream_info_len);
                            memcpy(elem->pAlarmInfo, pBuffer + 8 + psm->psm_stream_info_len, elem->dwBufLen);
                            pthread_mutex_lock(&ps->lock);
                            TAILQ_INSERT_TAIL(&ps->decode_audio_head, elem, entries);
                            pthread_mutex_unlock(&ps->lock);
                        }
                        break;
                    }
                    case 0xe0:  // Video
                    {
                        // unsigned short psm_stream_info_len = htons(psm->psm_stream_info_len);
                        // unsigned short stream_map_length = htons(*(uint16_t *)(pBuffer + sizeof(program_stream_map_t) + psm->psm_stream_info_len));
                        // printf("rx video psm_stream_info_len: %d  stream_map_length: %d data size %d\n", psm_stream_info_len, stream_map_length, dwBufSize);
                        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                        if (elem)
                        {
                            elem->lCommand = 0;
                            elem->pAlarmInfo = (char *)malloc(dwBufSize);
                            memcpy(elem->pAlarmInfo, pBuffer, dwBufSize);
                            elem->dwBufLen = dwBufSize;
                            pthread_mutex_lock(&ps->lock);
                            TAILQ_INSERT_TAIL(&ps->decode_video_head, elem, entries);
                            pthread_mutex_unlock(&ps->lock);
                        }
                        break;
                    }
                    case 0xba:  // BSH Header
                    {
                        // uint32_t timestamp = htonl(*(uint32_t *)(pBuffer + 4));
                        // uint32_t max_bitrate = htonl(*(uint32_t *)(pBuffer + 10));
                        break;
                    }
                    case 0xbc:  // PSM Header
                        // https://blog.csdn.net/fanyun_01/article/details/120537670
                        break;
                    case 0xbd:  // Private Stream Data
                        break;
                    default:
                        printf("rx data  %08x \n", htonl(*(uint32_t *)pBuffer));
                }
            }
            // struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
            // if (elem)
            // {
            //     elem->lCommand = 0;
            //     elem->pAlarmInfo = (char *)malloc(dwBufSize);
            //     memcpy(elem->pAlarmInfo, pBuffer, dwBufSize);
            //     elem->dwBufLen = dwBufSize;
            //     pthread_mutex_lock(&ps->lock);
            //     TAILQ_INSERT_TAIL(&ps->decode_head, elem, entries);
            //     pthread_mutex_unlock(&ps->lock);
            // }
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
        case '?': // 输入未定义的选项, 都会将该选项的值变为 ?
            fprintf(stderr,"unknown option \n");
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

	HIKEvent_Object *ps = (HIKEvent_Object *)calloc(1, sizeof(HIKEvent_Object));

    if (pthread_mutex_init(&ps->lock, NULL) != 0) {
        fprintf(stderr, "mutex init has failed");
        return 1;
    }

    TAILQ_INIT(&ps->decode_video_head);
    TAILQ_INIT(&ps->decode_audio_head);
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

    ps->dec_video_ctx = (HIKEvent_DecodeThread *)calloc(1, sizeof(HIKEvent_DecodeThread));
    ps->dec_video_ctx->ps = ps;
    ps->dec_video_ctx->decode_type = 0;
    pthread_create(&ps->dec_video_ctx->thread, NULL, decode_thread, ps->dec_video_ctx);

    ps->dec_audio_ctx = (HIKEvent_DecodeThread *)calloc(1, sizeof(HIKEvent_DecodeThread));
    ps->dec_audio_ctx->ps = ps;
    ps->dec_audio_ctx->decode_type = 1;
    pthread_create(&ps->dec_audio_ctx->thread, NULL, decode_thread, ps->dec_audio_ctx);

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


    NET_DVR_PREVIEWINFO struPlayInfo = {0};
    struPlayInfo.hPlayWnd     = 0;  // 仅取流不解码。这是Linux写法，Windows写法是struPlayInfo.hPlayWnd = NULL;
    struPlayInfo.lChannel     = cameraNo; // 通道号
    struPlayInfo.dwStreamType = streamType;  // 0- 主码流，1-子码流，2-码流3，3-码流4，以此类推
    struPlayInfo.dwLinkMode   = 0;  // 0- TCP方式，1- UDP方式，2- 多播方式，3- RTP方式，4-RTP/RTSP，5-RSTP/HTTP
    struPlayInfo.bBlocked     = 1;  // 0- 非阻塞取流，1- 阻塞取流
    //struPlayInfo.dwDisplayBufNum = 1;

    long lRealPlayHandle = NET_DVR_RealPlay_V40(ps->lUserID, &struPlayInfo, g_RealDataCallBack_V30, ps); // NET_DVR_RealPlay_V40 实时预览（支持多码流）。
    //lRealPlayHandle = NET_DVR_RealPlay_V30(lUserID, &ClientInfo, NULL, NULL, 0); // NET_DVR_RealPlay_V30 实时预览。
    if (lRealPlayHandle < 0) {
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        NET_DVR_Cleanup();
        return 1;
    }
    
    bool outputHeaderWrite = false;
    while (keepRunning && ESRCH != pthread_kill(ps->dec_video_ctx->thread, 0) && ESRCH != pthread_kill(ps->dec_audio_ctx->thread, 0))
    {
        if (!ps->dec_video_ctx->probedone || !ps->dec_audio_ctx->probedone)
        {
            sleep(0.01);
            continue;
        }

        pthread_mutex_lock(&ps->lock);
        if (TAILQ_EMPTY(&ps->push_head))
        {
            pthread_mutex_unlock(&ps->lock);
            sleep(0.02);
        } else 
        {
            struct entry *p = TAILQ_FIRST(&ps->push_head);
            TAILQ_REMOVE(&ps->push_head, p, entries);
            pthread_mutex_unlock(&ps->lock);

            if (!outputHeaderWrite)
            {
                outputHeaderWrite = true;
                av_dump_format(ps->plivectx, 0, NULL, 1);

                int ret = avformat_write_header(ps->plivectx, NULL);
                if (ret < 0) {
                    fprintf(stderr, "Error occurred when opening output file\n");
                    break;
                }
            }
            
            double now = microtime();
            AVPacket *pkt = (AVPacket *)p->pAlarmInfo;
            if (time(NULL) - p->lCommand <= 1)
            {
                ps->tx_size += pkt->buf ? pkt->buf->size : 0;
                // int ret = av_interleaved_write_frame(ps->plivectx, pkt);
                int ret = av_interleaved_write_frame(ps->plivectx, pkt);
                if (ret < 0) {
                    fprintf(stderr, "Error muxing packet\n");
                    break;
                }
            } else {
                fprintf(stderr, "Ignore too far packet\n");
            }
            if (now - ps->last_reset > 2)
            {
                printf("rx %.2f Mbps  tx %.2f Mbps      \n", ps->rx_size * 8.0 / 1048576 / (now - ps->last_reset), ps->tx_size * 8.0 / 1048576 / (now - ps->last_reset));
                fflush(stdout);
                ps->rx_size = 0;
                ps->tx_size = 0;
                ps->last_reset = now;
            }
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            free(p);
        }

        // sleep(10);
    }
    if (ps->plivectx)
        av_write_trailer(ps->plivectx);

    if (ps->plivectx && !(ps->plivectx->flags & AVFMT_NOFILE))
        avio_closep(&ps->plivectx->pb);
    avformat_free_context(ps->plivectx);

    if (false == NET_DVR_StopRealPlay(lRealPlayHandle)) {    // 停止取流
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        NET_DVR_Cleanup();
        return 1;
    }

	return 0;
}