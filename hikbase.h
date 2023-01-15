#ifndef _HIK_BASE_H
#define _HIK_BASE_H
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
#include <pthread.h>
#include <sys/queue.h>

static char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
static char av_ts[AV_TS_MAX_STRING_SIZE] = { 0 };

#undef av_err2str
#undef av_ts2str
#undef av_ts2timestr
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)
#define av_ts2str(ts) av_ts_make_string(av_ts, ts)
#define av_ts2timestr(ts, tb) av_ts_make_time_string(av_ts, ts, tb)

struct hik_queue_s {
    int bType;
    char *data;
    int start;
    size_t dwBufLen;
    TAILQ_ENTRY(hik_queue_s) entries;
};

typedef struct HIKEvent_DecodeThread {
    void *ps;
    pthread_t thread;
    pthread_t process_thread;

    pthread_mutex_t lock;
    TAILQ_HEAD(decode_tailhead, hik_queue_s) decode_head;
    TAILQ_HEAD(push_tailhead, hik_queue_s) push_head;

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
    struct SwsContext *sws_ctx;	// AV_PIX_FMT_YUV420P to RGB
    SwrContext *swr;
    AVAudioFifo *fifo;
    void *g722_decoder;
    char outputHeaderWrite;
    char probedone;
    char stop;

    NET_DVR_COMPRESSION_AUDIO compressAudioType;
    
    int      video_src_linesize[4];
    int      video_dst_linesize[4];

    long 	nPort;
} HIKEvent_DecodeThread;

#define MICRO_IN_SEC 1000000.00

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


double microtime(void);
void dump_hex(unsigned char *buf, size_t len);
void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, int output);
HIKEvent_DecodeThread *init_decode_ctx();
void free_decode_ctx(HIKEvent_DecodeThread *p);
int init_packet(AVPacket **packet);
int init_input_frame(AVFrame **frame);
int decode_audio_frame(AVFrame *frame,
                       AVPacket *input_packet,
                       AVCodecContext *input_codec_context,
                       int *data_present, int *finished);
int add_samples_to_fifo(AVAudioFifo *fifo,
                        uint8_t **converted_input_samples,
                        const int frame_size);
int init_converted_samples(uint8_t ***converted_input_samples,
                           AVCodecContext *output_codec_context,
                           int frame_size);
int convert_samples(const uint8_t **input_data,
                    uint8_t **converted_data, 
                    const int src_frame_size,
                    const int dst_frame_size,
                    SwrContext *resample_context);
int init_output_frame(AVFrame **frame,
                      AVCodecContext *output_codec_context,
                      int frame_size);
int convert_and_store(HIKEvent_DecodeThread *dp,
                                         AVFrame *input_frame,
                                         int *dst_nb_samples,
                                         int *finished);
int read_decode_convert_and_store(HIKEvent_DecodeThread *dp,
                                         AVPacket *input_packet,
                                         int *dst_nb_samples,
                                         int *finished);
int encode_audio_frame(HIKEvent_DecodeThread *dp,
                              AVFrame *frame,
                              AVPacket **pkt,
                              AVCodecContext *output_codec_context,
                              int *data_present);

AVFormatContext *init_input_ctx(AVIOContext *pb);
#endif