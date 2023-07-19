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

static const char *shortopts = ":h:u:p:c:s:P:v:N?";

static struct option longopts[] = {
  {"host",          1, 0, 'h'},
  {"user",          1, 0, 'u'},
  {"passwd",         1, 0, 'p'},
  {"camera",        1, 0, 'c'},
  {"stream-type",   1, 0, 's'},
  {"playback",      1, 0, 'P'},
  {"verbose",       1, 0, 'v'},
  {"ignore-acodec", 0, 0, 'N'},
  {"help",          0, 0, '?'},

  {0, 0, 0, 0}
};

static volatile int keepRunning = 1;

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

    int debug_packet;

    time_t   playback;
    uint32_t rx_size;
    uint32_t tx_size;
    double   last_reset;

    struct HIKEvent_DecodeThread *decthread_ctx;

    pthread_mutex_t lock;

    /* Type-specific fields go here. */
} HIKEvent_Object;


static volatile HIKEvent_Object *global_ps = NULL;

void debugHandler(int dummy) {
    if (global_ps != NULL)
    {
        global_ps->debug_packet = (global_ps->debug_packet + 1) % 4;
        global_ps->decthread_ctx->debug_packet = global_ps->debug_packet;
    }
}


void intHandler(int dummy) {
    keepRunning = 0;
    if (global_ps != NULL)
    {
        global_ps->decthread_ctx->stop = 1;
    }
}


void *decode_thread(void *data)
{
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)data;
    HIKEvent_Object *ps = (HIKEvent_Object *)dp->ps;
    AVFormatContext *pFormatCtx = NULL;
    dp->last_packet_rx = microtime();

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
                if (microtime() - dp->last_packet_rx >= 3)
                {
                    // fprintf(stderr, "Decode Thread: Receive Packet Timeout\n");
                    return AVERROR_EOF;
                }
                usleep(1000);   // wait 1ms
            } else 
            {
                dp->last_packet_rx = microtime();
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
    AVIOContext *pb = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, dp, onReadData, NULL, NULL);
    if (pb == nullptr)  //分配空间失败
    {
        av_freep(&avio_ctx_buffer);
        fprintf(stderr, "avio_alloc_context failed");
        return NULL;
    }

    pFormatCtx = init_input_ctx(pb, 0);
    if (pFormatCtx == NULL)
    {
        av_freep(&pb->buffer);
        av_freep(&pb);
        goto end;
    }
    dp->pInputCtx = pFormatCtx;
    
    int ret;

    ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        goto end;
    } else {
        AVStream *st = pFormatCtx->streams[ret];
        AVStream *out_stream;

        out_stream = avformat_new_stream(dp->pOutputCtx, NULL);
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
                const char *error = "Unknow";
                if (iRet == AVERROR_EOF) error = "EOF";
                if (!pFormatCtx->pb) error = "No Input Buffer";
                if (avio_feof(pFormatCtx->pb)) error = "Input Buffer EOF";
                if (pFormatCtx->pb->error) error = "Input Buffer Error";
                fprintf(stderr, "Decode Thread: Error %s %s\n", error, av_err2str(iRet));
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
                        if (dp->transcode == -1 && dwBufSize == 96)
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
    int transcode = 1;

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
            transcode = 0;
            break;
        case 'v':
            ps->debug_packet = atoi(optarg);
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
            fprintf(stderr, "  -v --verbose <level>      Log frames\n");
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

    HIKEvent_DecodeThread *dp = init_decode_ctx();
    ps->decthread_ctx = dp;
    dp->ps = ps;
    dp->transcode = transcode;
    dp->debug_packet = ps->debug_packet;
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
    if (!(dp->pOutputCtx = avformat_alloc_context())) {
        fprintf(stderr, "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    if (!(dp->pOutputCtx->oformat = av_guess_format("flv", NULL,
                                                              NULL))) {
        fprintf(stderr, "Could not find output file format\n");
        return 1;
    }

    /* Associate the output file (pointer) with the container format context. */
    dp->pOutputCtx->pb = pOutputIO;

    NET_DVR_AUDIO_CHANNEL channelInfo;
    memset(&channelInfo, 0, sizeof(NET_DVR_AUDIO_CHANNEL));

    channelInfo.dwChannelNum = ps->struDeviceInfoV40.struDeviceV30.byStartDTalkChan + cameraNo - 1;
    if (FALSE == NET_DVR_GetCurrentAudioCompress_V50(ps->lUserID, &channelInfo, &dp->compressAudioType))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_GetCurrentAudioCompress error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        return pErrorNo;
    }

    if (dp->compressAudioType.byAudioEncType == 0)
    {
        // Require G722 Decoder
        dp->g722_decoder = NET_DVR_InitG722Decoder();
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
        dp->playback = ps->playback;
        struct tm *tp = localtime((const time_t *)&dp->playback);

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

        time_t endtime = dp->playback + 300;
        tp = localtime((const time_t *)&endtime);
        struPlayback.struEndTime.wYear = 1900 + tp->tm_year;
        struPlayback.struEndTime.byMonth = 1 + tp->tm_mon;
        struPlayback.struEndTime.byDay = tp->tm_mday;
        struPlayback.struEndTime.byHour = tp->tm_hour;
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
    retry:
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
            if (pErrorNo == 91 && streamType == 1)
            {
                streamType = 0;
                goto retry;
            }
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
            fprintf(stderr, "NET_DVR_StopRealPlay error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            NET_DVR_Cleanup();
            return 1;
        }
    }
cleanup:
    keepRunning = false;

    if (dp->pOutputCtx && ps->decthread_ctx->outputHeaderWrite)
        av_write_trailer(dp->pOutputCtx);

    if (dp->pOutputCtx && !(dp->pOutputCtx->flags & AVFMT_NOFILE))
        avio_closep(&dp->pOutputCtx->pb);
    avformat_free_context(dp->pOutputCtx);

    if (ps->decthread_ctx->g722_decoder)
        NET_DVR_ReleaseG722Decoder(ps->decthread_ctx->g722_decoder);

    NET_DVR_Logout(ps->lUserID);
    NET_DVR_Cleanup();
    
    return 0;
}