#include "hikbase.h"
#include <sys/time.h>
#include <unistd.h>

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
            output_color = "38;5;220m";
            output_str = "IN ";
            break;
        case 1:
            output_color = "31m";
            output_str = "OUT";
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
    // av_log_set_level( AV_LOG_DEBUG);

    HIKEvent_DecodeThread *p = (HIKEvent_DecodeThread *)calloc(1, sizeof(HIKEvent_DecodeThread));
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        fprintf(stderr, "mutex init has failed");
        return NULL;
    }

    TAILQ_INIT(&p->decode_head);
    TAILQ_INIT(&p->push_head);

    return p;
}


void release_decode_ctx(HIKEvent_DecodeThread *dp)
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
        return ret;

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

AVFormatContext *init_input_ctx(AVIOContext *pb, int slow_probe)
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
    // av_dict_set(&opts, "stimeout", "1000000", 0);
    av_dict_set(&opts, "probesize", "4096", 0);             //加快打开
    if (!slow_probe)
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



int init_audio_decoder(HIKEvent_DecodeThread *dp, AVStream *st)
{
    int ret = 0;
    AVCodec *enc_codec;

    av_log(NULL, AV_LOG_INFO, "init audio encoder  Audio Encode Type: %d\n", dp->compressAudioType.byAudioEncType);

    int sampleRate = dp->compressAudioType.byAudioSamplingRate;
    switch(dp->compressAudioType.byAudioSamplingRate)
    {
        case 1: sampleRate = 16000; break;
        case 2: sampleRate = 32000; break;
        case 3: sampleRate = 48000; break;
        case 4: sampleRate = 44100; break;
        case 5: sampleRate = 8000; break;
        default: sampleRate = 8000;
    }
    switch (dp->compressAudioType.byAudioEncType)
    {
    case 0: // Note: Not support
    case 9:
        dp->transcode = -1;
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


    if (dp->transcode)
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

    dp->out_astream = avformat_new_stream(dp->pOutputCtx, NULL);
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

    av_dump_format(dp->pInputCtx, 0, NULL, 0);
    av_dump_format(dp->pOutputCtx, 0, NULL, 1);
    
    if (!dp->outputHeaderWrite)
    {
        dp->outputHeaderWrite = true;
        fprintf(stderr, "Output: Write header\n");
        AVDictionary* options = NULL;
        if (dp->decode_way == 5)
        {
            av_dict_set(&options, "movflags", "faststart", 0);
        }
        ret = avformat_write_header(dp->pOutputCtx, &options);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when opening output file\n");
            goto end;
        }
    }
    av_log(NULL, AV_LOG_DEBUG, "init audio encoder ok\n");
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
    AVClass dec_cls = {
        .class_name = "HIKEVENT.PROCESS",
        .item_name = av_default_item_name,
        .version = LIBAVUTIL_VERSION_INT
    };
    AVClass *pdec_cls = &dec_cls;

    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)data;
    AVPacket *output_packet = NULL;
    int ret = 0;
    int bType = 0;

    while (!dp->stop)
    {
        AVPacket *pkt = NULL;
        pthread_mutex_lock(&dp->lock);
        if (TAILQ_EMPTY(&dp->push_head) || !dp->probedone)
        {
            pthread_mutex_unlock(&dp->lock);
            usleep(1000);
            continue;
        } else 
        {
            struct hik_queue_s *p = NULL;
            p = TAILQ_FIRST(&dp->push_head);
            bType = p->bType;
            pkt = (AVPacket *)p->data;
            TAILQ_REMOVE(&dp->push_head, p, entries);
            pthread_mutex_unlock(&dp->lock);
            free(p);
        }

        AVFormatContext *pFormatCtx = dp->pInputCtx;
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
            if (dp->out_astream == NULL)
            {
                log_packet(pFormatCtx, pkt, 3);
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                continue;
            }
            // if (!dp->outputHeaderWrite)
            // {
            //     dp->outputHeaderWrite = true;
            //     fprintf(stderr, "Output: Write header\n");
            //     ret = avformat_write_header(dp->pOutputCtx, NULL);
            //     if (ret < 0) {
            //         fprintf(stderr, "Error occurred when opening output file\n");
            //         goto end;
            //     }
            // }
            if (dp->debug_packet & 0x1)
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

            if (dp->debug_packet & 0x2)
                log_packet(dp->pOutputCtx, pkt, 1);
            if (!dp->stop)
            {
                ret = av_interleaved_write_frame(dp->pOutputCtx, pkt);
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                if (ret < 0) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Error muxing video packet: %s\n", av_err2str(ret));
                    break;
                }   
            } else 
            {
                av_packet_unref(pkt);
                av_packet_free(&pkt);
            }
        } else if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (dp->out_astream == NULL && init_audio_decoder(dp, in_stream))
            {
                break;
            }
            int finished                = 0;
            int dst_nb_samples          = 0;

            if (dp->transcode)
            {
                /* Decode one frame worth of audio samples, convert it to the
                     * output sample format and put it into the FIFO buffer. */
                if (bType == 0)
                {
                    if (read_decode_convert_and_store(dp, pkt, &dst_nb_samples, &finished))
                        goto end;
                    if (dp->debug_packet & 0x1)
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
                
                double audio_t = dp->audio_pts * av_q2d(dp->enc_ctx->time_base);
                double video_t = dp->global_pts * av_q2d(dp->out_vstream->time_base);
                if (audio_t > video_t + 1)
                {
                    av_log(&pdec_cls, AV_LOG_WARNING, "NOT SYNC Audio, reset audio buffer  audio pts = %" PRId64 "/%.6f  video pts = %" PRId64 "/%.6f\n", 
                        dp->audio_pts,
                        dp->audio_pts * av_q2d(dp->enc_ctx->time_base), 
                        dp->global_pts,
                        dp->global_pts * av_q2d(dp->out_vstream->time_base));
                    av_audio_fifo_reset(dp->fifo);
                    continue;
                } else if (dp->audio_pts != 0 && audio_t < video_t - 1)
                {
                    av_log(&pdec_cls, AV_LOG_WARNING, "NOT SYNC Video, reset audio pts  audio pts = %" PRId64 "/%.6f  video pts = %" PRId64 "/%.6f\n", 
                        dp->audio_pts,
                        dp->audio_pts * av_q2d(dp->enc_ctx->time_base), 
                        dp->global_pts,
                        dp->global_pts * av_q2d(dp->out_vstream->time_base));
                    dp->audio_pts = 0;
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
                        av_audio_fifo_reset(dp->fifo);
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
                    if (dp->stop)
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
                        av_log(&pdec_cls, AV_LOG_ERROR, "Could not read data from FIFO\n");
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
                    if (data_written)
                    {
                        output_packet->pos = -1;
                        output_packet->stream_index = dp->out_astream->index;

                        // Translate Encode timebase to outstream timebase
                        av_packet_rescale_ts(output_packet, dp->enc_ctx->time_base, dp->out_astream->time_base);

                        if (!dp->stop)
                        {
                            if (dp->debug_packet & 0x2)
                                log_packet(dp->pOutputCtx, output_packet, 1);
                            // log_packet(dp->pOutputCtx, output_packet, 1);
                            ret = av_interleaved_write_frame(dp->pOutputCtx, output_packet);
                            av_packet_unref(output_packet);
                            av_packet_free(&output_packet);
                            if (ret < 0) {
                                av_log(&pdec_cls, AV_LOG_ERROR, "Error muxing audio packet: %s\n", av_err2str(ret));
                                goto end;
                            }
                        } else
                        {
                            av_packet_unref(output_packet);
                            av_packet_free(&output_packet);
                        }
                    }
                    av_frame_free(&output_frame);
                }
            } else 
            {
                if (dp->debug_packet & 0x1)
                    log_packet(dp->pInputCtx, pkt, 0);

                av_packet_rescale_ts(pkt,
                                     in_stream->time_base,
                                     dp->out_astream->time_base);
                pkt->pos = -1;
                pkt->stream_index = dp->out_astream->index;

                if (dp->debug_packet & 0x2)
                    log_packet(dp->pOutputCtx, output_packet, 1);
                int ret = av_interleaved_write_frame(dp->pOutputCtx, pkt);
                av_packet_unref(pkt);
                av_packet_free(&pkt);

                if (ret < 0) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Error muxing audio packet: %s\n", av_err2str(ret));
                    break;
                }
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