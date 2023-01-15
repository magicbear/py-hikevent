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
#include "hikbase.h"
}

#define AV_AUDIO_TRANSCODE

char        host[256]   = {0};
char        user[256]   = {0};
char        passwd[256] = {0};
char        STREAM_VIDEO_CODEC[256] = {0};
char        STREAM_AUDIO_CODEC[256] = {0};
int         cameraNo;
int         streamType;

static const char *shortopts = ":h:u:p:c:s:P:Nv?";

static struct option longopts[] = {
  {"host",          1, 0, 'h'},
  {"user",          1, 0, 'u'},
  {"passwd",         1, 0, 'p'},
  {"camera",        1, 0, 'c'},
  {"stream-type",   1, 0, 's'},
  {"playback",      1, 0, 'P'},
  {"ignore-acodec", 0, 0, 'N'},
  {"verbose",       0, 0, 'v'},
  {"help",          0, 0, '?'},

  {0, 0, 0, 0}
};

static volatile int keepRunning = 1;

void intHandler(int dummy) {
    keepRunning = 0;
}


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

    time_t   playback;
    uint32_t rx_size;
    uint32_t tx_size;
    double   last_reset;

    struct HIKEvent_DecodeThread *decthread_ctx;

    pthread_mutex_t lock;

    AVFormatContext *plivectx = nullptr;
    /* Type-specific fields go here. */
} HIKEvent_Object;


static volatile HIKEvent_Object *global_ps = NULL;

void debugHandler(int dummy) {
    if (global_ps != NULL)
        global_ps->debug_packet = (global_ps->debug_packet + 1) % 4;
}


int init_audio_decoder(HIKEvent_DecodeThread *dp, AVStream *st)
{
    HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;
    int ret = 0;
    AVCodec *enc_codec;

    fprintf(stderr, "init audio encoder  Audio Encode Type: %d\n", ps->compressAudioType.byAudioEncType);

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
        ps->transcode = -1;
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


    if (ps->transcode)
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

    dp->out_astream = avformat_new_stream(ps->plivectx, NULL);
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
    av_dump_format(ps->plivectx, 0, NULL, 1);
    
    if (!dp->outputHeaderWrite)
    {
        dp->outputHeaderWrite = true;
        fprintf(stderr, "Output: Write header\n");
        ret = avformat_write_header(ps->plivectx, NULL);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when opening output file\n");
            goto end;
        }
    }
    fprintf(stderr, "init audio encoder ok\n");
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
            if (dp->out_astream == NULL)
            {
                log_packet(pFormatCtx, pkt, 2);
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                continue;
            }
            // if (!dp->outputHeaderWrite)
            // {
            //     dp->outputHeaderWrite = true;
            //     fprintf(stderr, "Output: Write header\n");
            //     ret = avformat_write_header(ps->plivectx, NULL);
            //     if (ret < 0) {
            //         fprintf(stderr, "Error occurred when opening output file\n");
            //         goto end;
            //     }
            // }
            if (ps->debug_packet & 0x1)
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
            if (ps->debug_packet & 0x2)
                log_packet(ps->plivectx, pkt, 1);
            ret = av_interleaved_write_frame(ps->plivectx, pkt);
            if (ret < 0) {
                fprintf(stderr, "Error muxing video packet: %s\n", av_err2str(ret));
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

            if (ps->transcode)
            {
                /* Decode one frame worth of audio samples, convert it to the
                     * output sample format and put it into the FIFO buffer. */
                if (bType == 0)
                {
                    if (read_decode_convert_and_store(dp, pkt, &dst_nb_samples, &finished))
                        goto end;
                    if (ps->debug_packet & 0x1)
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
                    fprintf(stderr, "ERROR: NOT SYNC Audio, reset audio buffer  audio pts = %" PRId64 "/%.6f  video pts = %" PRId64 "/%.6f\n", 
                        dp->audio_pts,
                        dp->audio_pts * av_q2d(dp->enc_ctx->time_base), 
                        dp->global_pts,
                        dp->global_pts * av_q2d(dp->out_vstream->time_base));
                    av_audio_fifo_reset(dp->fifo);
                    continue;
                } else if (dp->audio_pts != 0 && audio_t < video_t - 1)
                {
                    fprintf(stderr, "ERROR: NOT SYNC Video, reset audio pts  audio pts = %" PRId64 "/%.6f  video pts = %" PRId64 "/%.6f\n", 
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
                    if (data_written)
                    {
                        output_packet->pos = -1;
                        output_packet->stream_index = dp->out_astream->index;

                        // Translate Encode timebase to outstream timebase
                        av_packet_rescale_ts(output_packet, dp->enc_ctx->time_base, dp->out_astream->time_base);

                        if (ps->debug_packet & 0x2)
                            log_packet(ps->plivectx, output_packet, 1);
                        // log_packet(ps->plivectx, output_packet, 1);
                        ret = av_interleaved_write_frame(ps->plivectx, output_packet);
                        if (ret < 0) {
                            fprintf(stderr, "Error muxing audio packet: %s\n", av_err2str(ret));
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
                    fprintf(stderr, "Error muxing audio packet: %s\n", av_err2str(ret));
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
        HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;

        while (keepRunning && ESRCH != pthread_kill(ps->decthread_ctx->process_thread, 0))
        {
            pthread_mutex_lock(&dp->lock);
            if (TAILQ_EMPTY(&dp->decode_head))
            {
                pthread_mutex_unlock(&dp->lock);
                usleep(1000);   // wait 1ms
            } else 
            {
                struct hik_queue_s *p = TAILQ_FIRST(&dp->decode_head);
                if ((int)(p->dwBufLen - p->start) < bufSize)
                {
                    TAILQ_REMOVE(&dp->decode_head, p, entries);
                }
                pthread_mutex_unlock(&dp->lock);

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

    pFormatCtx = init_input_ctx(pb);
    if (pFormatCtx == NULL)
    {
        av_freep(&avio_ctx_buffer);
        goto end;
    }
    dp->pFormatCtx = pFormatCtx;
    
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

        struct hik_queue_s *elem = (struct hik_queue_s *)calloc(1, sizeof(struct hik_queue_s));
        if (elem)
        {
            elem->bType = 0;
            elem->data = (char *)pkt;
            elem->dwBufLen = 0;
            pthread_mutex_lock(&dp->lock);
            TAILQ_INSERT_TAIL(&dp->push_head, elem, entries);
            pthread_mutex_unlock(&dp->lock);
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


void CALLBACK g_RealDataCallBack_V30(LONG lRealHandle, DWORD dwDataType, BYTE *pBuffer,DWORD dwBufSize,void* dwUser) {
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)dwUser;
    HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;
    // int dRet;

    switch (dwDataType) {
        case NET_DVR_SYSHEAD: //系统头
        case NET_DVR_AUDIOSTREAMDATA:   // 音频数据
        {
            fprintf(stderr, "dwDataType: NET_DVR_AUDIOSTREAMDATA\n");
            struct hik_queue_s *elem;
            char *buf;

            elem = (struct hik_queue_s *)calloc(1, sizeof(struct hik_queue_s));
            if (!elem) {
                fprintf(stderr, "allocate storage element failed\n");
                return;
            }
            elem->data = (char *)malloc(dwBufSize);
            buf = elem->data;

            elem->bType = 0;
            memcpy(buf, pBuffer, dwBufSize);
            elem->dwBufLen += dwBufSize;
            pthread_mutex_lock(&dp->lock);
            TAILQ_INSERT_TAIL(&dp->decode_head, elem, entries);
            pthread_mutex_unlock(&dp->lock);
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
                // fprintf(stderr, "ERR %d\n", psm->packet_start_code_prefix);
                ps->rx_size += dwBufSize;
                struct hik_queue_s *elem = (struct hik_queue_s *)calloc(1, sizeof(struct hik_queue_s));
                if (elem)
                {
                    elem->bType = 0;
                    elem->data = (char *)malloc(dwBufSize);
                    memcpy(elem->data, pBuffer, dwBufSize);
                    elem->dwBufLen = dwBufSize;
                    pthread_mutex_lock(&dp->lock);
                    TAILQ_INSERT_TAIL(&dp->decode_head, elem, entries);
                    pthread_mutex_unlock(&dp->lock);
                }
                break;
                // for (DWORD i = 0; i < dwBufSize; i++) printf("%02x ", pBuffer[i]);
                //     printf("\n");
            }

            if (psm->packet_start_code_prefix == 0x010000)
            {
                switch (psm->map_stream_id)
                {
                    case 0xc0:  // Audio
                        ps->rx_size += dwBufSize;
                        if (ps->transcode == -1 && dwBufSize == 96)
                        {
                            // mpeg_pes_head_t *pes_head = (mpeg_pes_head_t *)(pBuffer + 6);

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

                            struct hik_queue_s *elem = (struct hik_queue_s *)calloc(1, sizeof(struct hik_queue_s));
                            if (elem)
                            {
                                elem->bType = 1;
                                elem->data = (char *)frame;
                                elem->dwBufLen = 0;
                                pthread_mutex_lock(&dp->lock);
                                TAILQ_INSERT_TAIL(&dp->push_head, elem, entries);
                                pthread_mutex_unlock(&dp->lock);
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
                        struct hik_queue_s *elem = (struct hik_queue_s *)calloc(1, sizeof(struct hik_queue_s));
                        if (elem)
                        {
                            elem->bType = 0;
                            elem->data = (char *)malloc(dwBufSize);
                            memcpy(elem->data, pBuffer, dwBufSize);
                            elem->dwBufLen = dwBufSize;
                            pthread_mutex_lock(&dp->lock);
                            TAILQ_INSERT_TAIL(&dp->decode_head, elem, entries);
                            pthread_mutex_unlock(&dp->lock);
                        }
                        break;
                    }
                    case 0xbc:  // PSM Header
                        // https://blog.csdn.net/fanyun_01/article/details/120537670
                        fprintf(stderr, "rx data  %08x \n", htonl(*(uint32_t *)pBuffer));
                        break;
                    case 0xbd:  // Private Stream Data
                        break;
                    default:
                        fprintf(stderr, "rx data  %08x \n", htonl(*(uint32_t *)pBuffer));
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
    global_ps = ps;
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
            ps->debug_packet = 3;
            break;
        case 'P':
            ps->playback = atoi(optarg);
            break;
        case '?': // 输入未定义的选项, 都会将该选项的值变为 ?
            fprintf(stderr, "Usage: hikrtmp -h <host> -u <user> -p <passwd> -c <camera channel> [-s <stream type>] [-Nvh] <rtmp url>\n");
            fprintf(stderr, "Convert Hikvision PS stream to RTMP\n");
            fprintf(stderr, "  \n");
            fprintf(stderr, "Options:\n");
            fprintf(stderr, "  -h --host <str:host>      NVR/DVR/Camera IP\n");
            fprintf(stderr, "  -u --user <str:user>      NVR/DVR/Camera Username\n");
            fprintf(stderr, "  -p --passwd <str:pw>      NVR/DVR/Camera Password\n");
            fprintf(stderr, "  -c --camera <int:cam>     NVR/DVR/Camera Camera Channel\n");
            fprintf(stderr, "  -s --stream-type <int:s>  NVR/DVR/Camera Stream Type (0: Main Stream 1: Sub Stream...)\n");
            fprintf(stderr, "  -N --ignore-acodec        Copy acodec without transcode to AAC\n");
            fprintf(stderr, "  -v --verbose              Log frames\n");
            fprintf(stderr, "  -P --playback <int:ts>    Playback timestamp\n");
            fprintf(stderr, "  -? --help                 Show help\n\n");
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
    fprintf(stderr, "rtmp url: %s\n", argv[0]);

    signal(SIGINT, intHandler);
    signal(SIGUSR1, debugHandler);

    if (pthread_mutex_init(&ps->lock, NULL) != 0) {
        fprintf(stderr, "mutex init has failed");
        return 1;
    }

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

    ps->decthread_ctx = init_decode_ctx();
    ps->decthread_ctx->ps = ps;
    pthread_create(&ps->decthread_ctx->thread, NULL, decode_thread, ps->decthread_ctx);
    pthread_create(&ps->decthread_ctx->process_thread, NULL, process_thread, ps->decthread_ctx);

    int error;
    if (strcmp(argv[0],"-") == 0)
    {
        if ((error = avio_open(&pOutputIO, "pipe:1",
                               AVIO_FLAG_READ_WRITE)) < 0) {
            fprintf(stderr, "Could not open output file '%s' (error)\n",
                    argv[0]);
            return error;
        }
    } else 
    {
        if ((error = avio_open(&pOutputIO, argv[0],
                               AVIO_FLAG_READ_WRITE)) < 0) {
            fprintf(stderr, "Could not open output file '%s' (error)\n",
                    argv[0]);
            return error;
        }
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

    long lRealPlayHandle = 0;
    
    if (ps->playback)
    {
        struct tm *tp = localtime((const time_t *)&ps->playback);

        NET_DVR_VOD_PARA_V50 struPlayback = {0};
        memset(&struPlayback, 0, sizeof(struPlayback));
        struPlayback.dwSize = sizeof(struPlayback);
        struPlayback.struIDInfo.dwSize = sizeof(NET_DVR_STREAM_INFO);
        struPlayback.struIDInfo.dwChannel = cameraNo;

        struPlayback.struBeginTime.wYear = 1900 + tp->tm_year;
        struPlayback.struBeginTime.byMonth = 1 + tp->tm_mon;
        struPlayback.struBeginTime.byDay = tp->tm_mday;
        struPlayback.struBeginTime.byHour = tp->tm_hour;
        struPlayback.struBeginTime.byMinute = tp->tm_min;
        struPlayback.struBeginTime.bySecond = tp->tm_sec;

        struPlayback.struEndTime.wYear = 1900 + tp->tm_year;
        struPlayback.struEndTime.byMonth = 1 + tp->tm_mon;
        struPlayback.struEndTime.byDay = tp->tm_mday;
        struPlayback.struEndTime.byHour = tp->tm_hour + 1;
        struPlayback.struEndTime.byMinute = tp->tm_min;
        struPlayback.struEndTime.bySecond = tp->tm_sec;

        fprintf(stderr, "Playback Time：%s", asctime(tp));
        // struPlayback.struEndTime.wYear = 2021;
        // struPlayback.struEndTime.byMonth = 4;
        // struPlayback.struEndTime.byDay = 7;
        // struPlayback.struEndTime.byHour = 10;
        // struPlayback.struEndTime.byMinute = 0;
        // struPlayback.struEndTime.bySecond = 0;

        //按时间回放
        lRealPlayHandle = NET_DVR_PlayBackByTime_V50(ps->lUserID, &struPlayback);
        if (lRealPlayHandle < 0)
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            fprintf(stderr, "NET_DVR_PlayBackByTime_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            goto cleanup;
        }

        if (!NET_DVR_SetPlayDataCallBack_V40(lRealPlayHandle, g_RealDataCallBack_V30, ps->decthread_ctx))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            fprintf(stderr, "NET_DVR_SetPlayDataCallBack_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            goto cleanup;
        }

        //---------------------------------------
        //开始
        if (!NET_DVR_PlayBackControl_V40(lRealPlayHandle, NET_DVR_PLAYSTART, NULL, 0, NULL, NULL))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            fprintf(stderr, "NET_DVR_PlayBackControl_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            goto cleanup;
        }

        // NET_DVR_PlayBackByTime_V50
    } else 
    {
        NET_DVR_PREVIEWINFO struPlayInfo = {0};
        struPlayInfo.hPlayWnd     = 0;  // 仅取流不解码。这是Linux写法，Windows写法是struPlayInfo.hPlayWnd = NULL;
        struPlayInfo.lChannel     = cameraNo; // 通道号
        struPlayInfo.dwStreamType = streamType;  // 0- 主码流，1-子码流，2-码流3，3-码流4，以此类推
        struPlayInfo.dwLinkMode   = 0;  // 0- TCP方式，1- UDP方式，2- 多播方式，3- RTP方式，4-RTP/RTSP，5-RSTP/HTTP
        struPlayInfo.bBlocked     = 1;  // 0- 非阻塞取流，1- 阻塞取流
        //struPlayInfo.dwDisplayBufNum = 1;

        lRealPlayHandle = NET_DVR_RealPlay_V40(ps->lUserID, &struPlayInfo, g_RealDataCallBack_V30, ps->decthread_ctx); // NET_DVR_RealPlay_V40 实时预览（支持多码流）。
        //lRealPlayHandle = NET_DVR_RealPlay_V30(lUserID, &ClientInfo, NULL, NULL, 0); // NET_DVR_RealPlay_V30 实时预览。
        if (lRealPlayHandle < 0) {
            LONG pErrorNo = NET_DVR_GetLastError();
            fprintf(stderr, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            NET_DVR_Cleanup();
            return 1;
        }
    }
    
    ps->last_reset = microtime();
    while (keepRunning && ESRCH != pthread_kill(ps->decthread_ctx->thread, 0) && ESRCH != pthread_kill(ps->decthread_ctx->process_thread, 0))
    {
        double now = microtime();
        if (now - ps->last_reset > 1)
        {
            // printf("rx %-10d bytes  %.2f kbps  tx %.2f kbps      \n", ps->rx_size, ps->rx_size * 8.0 / 1024 / (now - ps->last_reset), ps->tx_size * 8.0 / 1024 / (now - ps->last_reset));
            // fflush(stdout);
            ps->rx_size = 0;
            ps->tx_size = 0;
            ps->last_reset = now;
        }
        usleep(100000);   // wait 100ms
    }
    if (ps->playback)
    {
        //停止回放
        if (!NET_DVR_StopPlayBack(lRealPlayHandle))
        {
            fprintf(stderr, "failed to stop file [%d]\n", NET_DVR_GetLastError());
            goto cleanup;
        }
    } else 
    {
        if (false == NET_DVR_StopRealPlay(lRealPlayHandle)) {    // 停止取流
            LONG pErrorNo = NET_DVR_GetLastError();
            fprintf(stderr, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            NET_DVR_Cleanup();
            return 1;
        }
    }
cleanup:
    keepRunning = false;

    if (ps->plivectx && ps->decthread_ctx->outputHeaderWrite)
        av_write_trailer(ps->plivectx);

    if (ps->plivectx && !(ps->plivectx->flags & AVFMT_NOFILE))
        avio_closep(&ps->plivectx->pb);
    avformat_free_context(ps->plivectx);

    if (ps->decthread_ctx->g722_decoder)
        NET_DVR_ReleaseG722Decoder(ps->decthread_ctx->g722_decoder);

    NET_DVR_Logout(ps->lUserID);
    NET_DVR_Cleanup();
    
    return 0;
}