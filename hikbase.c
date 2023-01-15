#include "hikbase.h"
#include <sys/time.h>

double microtime(void)
{
    struct timeval tp = {0};

    if (gettimeofday(&tp, NULL)) {
        return 0;
    }

    return ((double)(tp.tv_sec + tp.tv_usec / MICRO_IN_SEC));
}


void dump_hex(unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0)
        {
            fprintf(stderr, "\n%08lx: ", i);
        }
        fprintf(stderr, "%02x ", buf[i]);
    }
    fprintf(stderr, "\n");
}


void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, int output)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    const char *output_color, *output_str;
    switch (output)
    {
        case 0:
            output_color = "31m";
            output_str = "OUT";
            break;
        case 1:
            output_color = "38;5;220m";
            output_str = "IN ";
            break;
        default:
            output_color = "41m";
            output_str = "SKP";
            break;
    }

    fprintf(stderr, "[\033[%s%s\033[0m \033[%s%s\033[0m %d] pts:%16" PRId64 " pts_time:%-9.3f dts:%16" PRId64 " dts_time:%-9.3f duration:%-8" PRId64 " duration_time:%-8s tb: %4d/%-8d size: %d\n",
        output_color,
        output_str,
        fmt_ctx->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "32m" : "95m",
        fmt_ctx->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", pkt->stream_index,
           pkt->pts, av_q2d(*time_base) * pkt->pts,
           pkt->dts, av_q2d(*time_base) * pkt->dts,
           pkt->duration, av_ts2timestr(pkt->duration, time_base),
           time_base->num, time_base->den, 
           pkt->buf ? pkt->buf->size : -1);
}


HIKEvent_DecodeThread *init_decode_ctx()
{
    av_log_set_level( AV_LOG_DEBUG);

    HIKEvent_DecodeThread *p = (HIKEvent_DecodeThread *)calloc(1, sizeof(HIKEvent_DecodeThread));
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        fprintf(stderr, "mutex init has failed");
        return NULL;
    }

    TAILQ_INIT(&p->decode_head);
    TAILQ_INIT(&p->push_head);

    return p;
}


void free_decode_ctx(HIKEvent_DecodeThread *dp)
{
    pthread_mutex_destroy(&dp->lock);

    struct hik_queue_s *np = NULL;
    while (NULL != (np = TAILQ_FIRST(&dp->decode_head))) {
        TAILQ_REMOVE(&dp->decode_head, np, entries);
        free(np->data);
        free(np);
    }
    while (NULL != (np = TAILQ_FIRST(&dp->push_head))) {
        TAILQ_REMOVE(&dp->push_head, np, entries);
        free(np->data);
        free(np);
    }
    free(dp);
}


/**
 * Initialize one data packet for reading or writing.
 * @param[out] packet Packet to be initialized
 * @return Error code (0 if successful)
 */
int init_packet(AVPacket **packet)
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
int init_input_frame(AVFrame **frame)
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
int decode_audio_frame(AVFrame *frame,
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
int add_samples_to_fifo(AVAudioFifo *fifo,
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
int init_converted_samples(uint8_t ***converted_input_samples,
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
int convert_samples(const uint8_t **input_data,
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



/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 * @param[out] frame                Frame to be initialized
 * @param      output_codec_context Codec context of the output file
 * @param      frame_size           Size of the frame
 * @return Error code (0 if successful)
 */
int init_output_frame(AVFrame **frame,
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


int convert_and_store(HIKEvent_DecodeThread *dp,
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
int read_decode_convert_and_store(HIKEvent_DecodeThread *dp,
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
 * Encode one frame worth of audio to the output file.
 * @param      frame                 Samples to be encoded
 * @param      output_format_context Format context of the output file
 * @param      output_codec_context  Codec context of the output file
 * @param[out] data_present          Indicates whether data has been
 *                                   encoded
 * @return Error code (0 if successful)
 */
int encode_audio_frame(HIKEvent_DecodeThread *dp,
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

AVFormatContext *init_input_ctx(AVIOContext *pb)
{
    AVFormatContext *pFormatCtx;
    if (!(pFormatCtx = avformat_alloc_context())) {
        fprintf(stderr, "avformat_alloc_context failed");
        return NULL;
    }

    AVInputFormat *inputFmt = av_find_input_format("mpeg");
    AVDictionary* opts = NULL;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_analyze_duration", "10", 0);
    av_dict_set(&opts, "max_delay", "1000", 0);
    av_dict_set(&opts, "stimeout", "1000000", 0);
    av_dict_set(&opts, "probesize", "4096", 0);             //加快打开
    av_dict_set(&opts, "max_probe_packets", "5", 0);             //加快打开

    pFormatCtx->pb = pb;

    if (avformat_open_input(&pFormatCtx, NULL, inputFmt, &opts) < 0)
    {
        if (NULL != opts)
            av_dict_free(&opts);
        avformat_free_context(pFormatCtx);
        fprintf(stderr, "avformat_open_input failed\n");
        return NULL;
    }

    if (NULL != opts)
        av_dict_free(&opts);

    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        avformat_close_input(&pFormatCtx);
        avformat_free_context(pFormatCtx);
        return NULL;
    }

    return pFormatCtx;
}