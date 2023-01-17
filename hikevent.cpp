#define PY_SSIZE_T_CLEAN

#include <iostream>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <unistd.h> 
#include <stdio.h>
#include <string.h>
#include <Python.h>
#include "HCNetSDK.h"
#include <sys/queue.h>
#include <iconv.h>
#include <signal.h>
#include <arpa/inet.h>

#include "LinuxPlayM4.h"
#define USECOLOR 1

extern "C" { 
#include "hikbase.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>
#include <libavutil/time.h>
#include "libswresample/swresample.h"
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

using namespace std;

#define HPR_ERROR -1
#define HPR_OK 0

#define DVR_REMOTE_CALL_COMMAND 0x4000001
#define DVR_VIDEO_DATA          0x4000002
#define DVR_REMOTE_CALL_STATUS  0x4000003
#define DVR_FLV_DATA            0x4000004

#define sprintfDVRTime(struAlarmTime) "%04d-%02d-%02d %02d:%02d:%02d", struAlarmTime.wYear, struAlarmTime.byMonth, struAlarmTime.byDay, struAlarmTime.byHour, struAlarmTime.byMinute, struAlarmTime.bySecond

struct entry {
    long lCommand;
    char *pAlarmInfo;
    size_t dwBufLen;
    TAILQ_ENTRY(entry) entries;
};


typedef struct {
    PyObject_HEAD
    char *ip;
    char *user;
    char *passwd;
    char *error_buffer;
    LONG lUserID;
    LONG lHandle;
    LONG lCallHandle;
    LONG lVoiceHandler;
    NET_DVR_COMPRESSION_AUDIO compressAudioType;
    bool alarmChannelOpened;
    bool callChannelOpened;

    TAILQ_HEAD(tailhead, entry) head;
    TAILQ_HEAD(decode_ctx_tailhead, hik_queue_s) decode_ctx;

    pthread_mutex_t lock;
    NET_DVR_DEVICEINFO_V30 struDeviceInfo;
    NET_DVR_USER_LOGIN_INFO struLoginInfo = {0};
    NET_DVR_DEVICEINFO_V40 struDeviceInfoV40 = {0};

    LONG nPort;

    int decode_way;
    pthread_t *decodeThread;
    // AVCodecContext *codec_ctx;
    // AVCodecParserContext *parser;
    // AVPacket *pkt;
    // AVFrame *frame;

    /* Type-specific fields go here. */
} PyHIKEvent_Object;


void CALLBACK MessageCallback(LONG lCommand, NET_DVR_ALARMER *pAlarmer, char *pAlarmInfo, DWORD dwBufLen, void* pUser)
{
    PyHIKEvent_Object *self = (PyHIKEvent_Object*)pUser;
    
    struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
    if (elem)
    {
        elem->lCommand = lCommand;
        elem->pAlarmInfo = (char *)malloc(dwBufLen);
        memcpy(elem->pAlarmInfo, pAlarmInfo, dwBufLen);
        elem->dwBufLen = dwBufLen;
        pthread_mutex_lock(&self->lock);
        TAILQ_INSERT_HEAD(&self->head, elem, entries);
        pthread_mutex_unlock(&self->lock);

    }
}

int CALLBACK MessageCallback_V31(LONG lCommand, NET_DVR_ALARMER *pAlarmer, char *pAlarmInfo, DWORD dwBufLen, void* pUser)
{
    MessageCallback(lCommand, pAlarmer, pAlarmInfo, dwBufLen, pUser);
    return 0;
}

static PyObject *
hikevent_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char error_buffer[256];
    PyHIKEvent_Object *ps;
    ps = (PyHIKEvent_Object *) type->tp_alloc(type, 0);
    if (ps == NULL) return NULL;

    static char *kwlist[] = {(char *)"host", (char *)"user", (char *)"passwd", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sss", kwlist, &ps->ip, &ps->user, &ps->passwd)) {
        PyErr_SetString(PyExc_TypeError,
                        "No enough params provide, required: IP, user, passwd");
        return NULL;
    }

    TAILQ_INIT(&ps->head);
    TAILQ_INIT(&ps->decode_ctx);
    if (pthread_mutex_init(&ps->lock, NULL) != 0) {
        PyErr_SetString(PyExc_TypeError, "mutex init has failed");
        return NULL;
    }

    ps->error_buffer = error_buffer;
    // 初始化
    NET_DVR_Init();
    //设置连接时间与重连时间
    NET_DVR_SetConnectTime(2000, 1);
    NET_DVR_SetReconnect(10000, true);

    ps->struLoginInfo.bUseAsynLogin = false;

    ps->struLoginInfo.wPort = 8000;
    memcpy(ps->struLoginInfo.sDeviceAddress, ps->ip, NET_DVR_DEV_ADDRESS_MAX_LEN);
    memcpy(ps->struLoginInfo.sUserName, ps->user, strlen(ps->user) > NAME_LEN ? NAME_LEN : strlen(ps->user));
    memcpy(ps->struLoginInfo.sPassword, ps->passwd, strlen(ps->passwd) > NAME_LEN ? NAME_LEN : strlen(ps->passwd));

    ps->lVoiceHandler = -1;
    ps->lUserID = NET_DVR_Login_V40(&ps->struLoginInfo, &ps->struDeviceInfoV40);

    if (ps->lUserID < 0)
    {
        sprintf(error_buffer, "Login error, %d\n", NET_DVR_GetLastError());
        PyErr_SetString(PyExc_TypeError, error_buffer);
        NET_DVR_Cleanup(); 
        return NULL;
    }

    // NET_DVR_SetDVRMessageCallBack_V31(MessageCallback_V31, (void *)ps);
    NET_DVR_SetDVRMessageCallBack_V51(0, MessageCallback, (void *)ps);

    return (PyObject *) ps;
}

extern "C"
{
static PyObject *receiveAlarmEvent(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    if (ps->alarmChannelOpened)
    {
        Py_RETURN_NONE;
        return NULL;
    }
    NET_DVR_SETUPALARM_PARAM_V50 struSetupAlarmParam = { 0 };
    struSetupAlarmParam.dwSize = sizeof(struSetupAlarmParam);
    struSetupAlarmParam.byRetAlarmTypeV40 = 1;   // variable size
    // struSetupAlarmParam.byLevel = 2;    // low priority
    struSetupAlarmParam.byLevel = 0;    // high priority
    struSetupAlarmParam.byAlarmInfoType = 1;
    struSetupAlarmParam.byRetDevInfoVersion = 1;
    struSetupAlarmParam.byRetVQDAlarmType = 1; //Prefer VQD Alarm type of NET_DVR_VQD_ALARM
    struSetupAlarmParam.byFaceAlarmDetection = 1;//m_comFaceAlarmType.GetCurSel();
                
    struSetupAlarmParam.byRetDevInfoVersion = TRUE;
    struSetupAlarmParam.byAlarmInfoType = 1;
    struSetupAlarmParam.bySupport = 1 | 2 | 8;
    struSetupAlarmParam.byDeployType = 0;

    ps->lHandle = NET_DVR_SetupAlarmChan_V50(ps->lUserID, &struSetupAlarmParam, (char *)"<SubscribeEvent version=\"2.0\" xmlns=\"http://www.isapi.org/ver20/XMLSchema\"><eventMode>all</eventMode><changedUploadSub/></SubscribeEvent> ", 0);

    if (ps->lHandle < 0)
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_SetupAlarmChan_V50 error, %d: %s  Handle ID: %d\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo), ps->lHandle);
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        NET_DVR_Logout(ps->lUserID);
        NET_DVR_Cleanup(); 
        return NULL;
    }
    ps->alarmChannelOpened = true;
    Py_RETURN_NONE;
}

static PyObject *unlock(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    if (FALSE == NET_DVR_ControlGateway(ps->lUserID, -1, 1))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_ControlGateway error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    // NET_DVR_CONTROL_GATEWAY ctrl;
    // memset(&ctrl, 0, sizeof(ctrl));
    // ctrl.dwSize = sizeof(NET_DVR_CONTROL_GATEWAY);
    // ctrl.byCommand = 1;
    // ctrl.dwGatewayIndex = 1;
    // ctrl.byLockType = 0;
    // ctrl.wLockID = 0;
    // strncpy((char*)ctrl.byControlSrc, "MANAGER",NAME_LEN);
    // ctrl.byControlType = 1;

    // if (FALSE == NET_DVR_RemoteControl(ps->lUserID, NET_DVR_REMOTECONTROL_GATEWAY, &ctrl, sizeof(NET_DVR_CONTROL_GATEWAY)))
    // {
    //     LONG pErrorNo = NET_DVR_GetLastError();
    //     sprintf(ps->error_buffer, "NET_DVR_RemoteControl error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
    //     PyErr_SetString(PyExc_TypeError, ps->error_buffer);
    //     return NULL;
    // }
    Py_RETURN_NONE;
}


void CALLBACK fRemoteCallCallback(DWORD dwType, void *lpBuffer, DWORD dwBufLen, void* pUser)
{
    PyHIKEvent_Object *self = (PyHIKEvent_Object*)pUser;

    //NET_SDK_CALLBACK_TYPE_STATUS 
    //NET_SDK_CALLBACK_TYPE_PROGRESS
    //NET_SDK_CALLBACK_TYPE_DATA
    if (dwType == NET_SDK_CALLBACK_TYPE_DATA)
    {
        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
        if (elem)
        {
            elem->lCommand = DVR_REMOTE_CALL_COMMAND;
            elem->pAlarmInfo = (char *)malloc(dwBufLen);
            memcpy(elem->pAlarmInfo, lpBuffer, dwBufLen);
            elem->dwBufLen = dwBufLen;
            pthread_mutex_lock(&self->lock);
            TAILQ_INSERT_HEAD(&self->head, elem, entries);
            pthread_mutex_unlock(&self->lock);
        }
        // NET_DVR_VIDEO_CALL_PARAM *callParam = (NET_DVR_VIDEO_CALL_PARAM*)lpBuffer;
    } else if (dwType == NET_SDK_CALLBACK_TYPE_STATUS)
    {
        NET_SDK_CALLBACK_STATUS_NORMAL *status = (NET_SDK_CALLBACK_STATUS_NORMAL *)lpBuffer;
        fprintf(stderr, "Receive RemoteCall Callback Event %d %d size: %d\n", dwType, *status, dwBufLen);
        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
        if (elem)
        {
            elem->lCommand = DVR_REMOTE_CALL_STATUS;
            elem->pAlarmInfo = NULL;
            elem->dwBufLen = *status;
            pthread_mutex_lock(&self->lock);
            TAILQ_INSERT_HEAD(&self->head, elem, entries);
            pthread_mutex_unlock(&self->lock);
        }
    } else {
        fprintf(stderr, "Receive RemoteCall Callback Event %d\n", dwType);
    }
}


static PyObject *receiveRemoteCall(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    if (ps->callChannelOpened)
    {
        Py_RETURN_NONE;
        return NULL;
    }
    NET_DVR_VIDEO_CALL_COND struVideoCallCond;
    memset(&struVideoCallCond, 0, sizeof(struVideoCallCond));
    struVideoCallCond.dwSize = sizeof(struVideoCallCond);

    if (-1 == (ps->lCallHandle = NET_DVR_StartRemoteConfig(ps->lUserID, NET_DVR_VIDEO_CALL_SIGNAL_PROCESS, (char *)&struVideoCallCond, sizeof(struVideoCallCond), fRemoteCallCallback, ps)))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_SendRemoteConfig error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    ps->callChannelOpened = true;
    Py_RETURN_NONE;
}


static PyObject *stopRemoteCall(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    if (!ps->callChannelOpened)
    {
        Py_RETURN_NONE;
        return NULL;
    }

    if (!NET_DVR_StopRemoteConfig(ps->lCallHandle))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_StopRemoteConfig error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    ps->callChannelOpened = false;
    Py_RETURN_NONE;
}


static PyObject *remoteCallCommand(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_VIDEO_CALL_PARAM struCallCmd;
    memset(&struCallCmd, 0, sizeof(NET_DVR_VIDEO_CALL_PARAM));
    long cmdType;

    if (!PyArg_ParseTuple(args, "I|iiiii", &cmdType, &struCallCmd.wPeriod, &struCallCmd.wBuildingNumber, &struCallCmd.wUnitNumber, &struCallCmd.wFloorNumber, &struCallCmd.wRoomNumber)) {
        return NULL;
    }
    struCallCmd.dwSize = sizeof(NET_DVR_VIDEO_CALL_PARAM);
    // 0- 请求呼叫，1- 取消本次呼叫，2- 接听本次呼叫，3- 拒绝本地来电呼叫，4- 被叫响铃超时，5- 结束本次通话，6- 设备正在通话中，7- 客户端正在通话中 
    struCallCmd.dwCmdType = cmdType;

    if (FALSE == NET_DVR_SendRemoteConfig(ps->lUserID, NET_DVR_VIDEO_CALL_SIGNAL_PROCESS, (char *)&struCallCmd, sizeof(NET_DVR_VIDEO_CALL_PARAM)))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_SendRemoteConfig error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    // if (cmdType == 2)
    // {
    //     NET_DVR_StartVoiceCom_V30(ps->lUserID, 1, 0, NULL, NULL);
    // }
    Py_RETURN_NONE;
}


static PyObject *getDeviceInfo(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;
    NET_DVR_DEV_BASE_INFO devConfig;
    NET_DVR_DEVICEID_INFO devInfo;
    uint32_t lpStatusList;

    memset(&devInfo, 0, sizeof(devInfo));
    devInfo.dwSize = sizeof(NET_DVR_DEVICEID_INFO);

    if (!NET_DVR_GetDeviceConfig(ps->lUserID, NET_DVR_GET_DEV_BASEINFO, 1, &devInfo, sizeof(devInfo), &lpStatusList, &devConfig, sizeof(devConfig)))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_GetDeviceConfig error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    size_t srclen = strlen((char *)devConfig.sDevName);
    char outbuf[NAME_LEN * 2];
    size_t outlen = NAME_LEN * 2;
    iconv_t cvSess = iconv_open("utf-8", "gb2312");

    /* 由于iconv()函数会修改指针，所以要保存源指针 */
    char *srcstart = (char *)devConfig.sDevName;
    char *tempoutbuf = outbuf;

    /* 进行转换
    *@param cd iconv_open()产生的句柄
    *@param srcstart 需要转换的字符串
    *@param srclen 存放还有多少字符没有转换
    *@param tempoutbuf 存放转换后的字符串
    *@param outlen 存放转换后,tempoutbuf剩余的空间
    *
    * */
    int ret = iconv (cvSess, &srcstart, &srclen, &tempoutbuf, &outlen);
    if (ret == -1)
    {
        sprintf(ps->error_buffer, "iconv name to utf-8 error, %d\n", errno);
        return NULL;
    }

    iconv_close(cvSess);

    // return Py_BuildValue("{s:s#,s:i,s:s}", "DVRName", outbuf, NAME_LEN * 2 - outlen, "DVRID", devConfig.dwDVRID, "SN", devConfig.sSerialNumber );
    return Py_BuildValue("{s:s#,s:i,s:s}", "DevName", outbuf, NAME_LEN * 2 - outlen );
}

static PyObject *getPicture(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;
    NET_DVR_PICPARAM_V50 picParams;

    memset(&picParams, 0, sizeof(picParams));
    picParams.struParam.wPicSize = 5;
    picParams.struParam.wPicQuality = 1;

    long lChannelNo = 1;
    if (!PyArg_ParseTuple(args, "|I", &lChannelNo)) {
        return NULL;
    }

    char *picBuffer = (char *)malloc(16 * 1048576); // 16 MB Buffer
    DWORD picSize = 0;
    if (picBuffer == NULL)
    {
        sprintf(ps->error_buffer, "allocate buffer failed\n");
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    if (!NET_DVR_CapturePicture_V50(ps->lUserID, lChannelNo, &picParams, picBuffer, 16 * 1048576, &picSize))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_CapturePicture_V50 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    PyObject *ret = Py_BuildValue("y#", picBuffer, picSize);
    free(picBuffer);
    return ret;

}

static PyObject *getChannelName(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;
    NET_DVR_PICCFG_V40 picConfig;
    long cameraNo;
    DWORD lReceivedConfigSize;
    if (!PyArg_ParseTuple(args, "I", &cameraNo)) {
        return NULL;
    }

    if (!NET_DVR_GetDVRConfig(ps->lUserID, NET_DVR_GET_PICCFG_V40, cameraNo, &picConfig, sizeof(picConfig), &lReceivedConfigSize))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_PICCFG_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    size_t srclen = strlen((char *)picConfig.sChanName);
    char outbuf[NAME_LEN * 2];
    size_t outlen = NAME_LEN * 2;
    iconv_t cvSess = iconv_open("utf-8", "gb2312");

    /* 由于iconv()函数会修改指针，所以要保存源指针 */
    char *srcstart = (char *)picConfig.sChanName;
    char *tempoutbuf = outbuf;

    /* 进行转换
    *@param cd iconv_open()产生的句柄
    *@param srcstart 需要转换的字符串
    *@param srclen 存放还有多少字符没有转换
    *@param tempoutbuf 存放转换后的字符串
    *@param outlen 存放转换后,tempoutbuf剩余的空间
    *
    * */
    int ret = iconv (cvSess, &srcstart, &srclen, &tempoutbuf, &outlen);
    if (ret == -1)
    {
        sprintf(ps->error_buffer, "iconv name to utf-8 error, %d\n", errno);
        return NULL;
    }

    iconv_close(cvSess);

    return Py_BuildValue("s#", outbuf, NAME_LEN * 2 - outlen);
}

void CALLBACK fdwVoiceDataCallBack(LONG lVoiceComHandle, char *pRecvDataBuffer, DWORD dwBufSize, BYTE byAudioFlag, DWORD pUser)
{

}

void CALLBACK fVoiceDataCallBack(LONG lVoiceComHandle, char *pRecvDataBuffer, DWORD dwBufSize, BYTE byAudioFlag, void *pUser)
{

}

static PyObject *addDVRChannel(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_AUDIO_CHANNEL channelInfo;
    memset(&channelInfo, 0, sizeof(NET_DVR_AUDIO_CHANNEL));
    long cameraNo;
    if (!PyArg_ParseTuple(args, "I", &cameraNo)) {
        return NULL;
    }
    if (cameraNo > 1)
    {
        cameraNo = ps->struDeviceInfoV40.struDeviceV30.byStartDTalkChan + cameraNo - 1;
    }

    channelInfo.dwChannelNum = cameraNo;
    if (FALSE == NET_DVR_GetCurrentAudioCompress_V50(ps->lUserID, &channelInfo, &ps->compressAudioType))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_GetCurrentAudioCompress error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    int sampleRate = ps->compressAudioType.byAudioSamplingRate;
    switch(ps->compressAudioType.byAudioSamplingRate)
    {
        case 1: sampleRate = 16000; break;
        case 2: sampleRate = 32000; break;
        case 3: sampleRate = 48000; break;
        case 4: sampleRate = 44100; break;
        case 5: sampleRate = 8000; break;
        // default:
        //     sprintf(ps->error_buffer, "Unknow sample rate, %d\n", ps->compressAudioType.byAudioSamplingRate);
        //     PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        //     return NULL;
    }

    // NET_DVR_ClientAudioStart();
    long lVoiceHandler = NET_DVR_AddDVR_V30(ps->lUserID, cameraNo);
    if (lVoiceHandler == -1)
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_AddDVR_V30 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    return Py_BuildValue("{s:i,s:i,s:i}", "AudioEncode", ps->compressAudioType.byAudioEncType, 
                        "SampleRate", sampleRate, "handler", lVoiceHandler);
}


static PyObject *delDVRChannel(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    long handler;
    if (!PyArg_ParseTuple(args, "I", &handler)) {
        return NULL;
    }
    if (!NET_DVR_DelDVR_V30(handler))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_DelDVR_V30 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    Py_RETURN_NONE;
}


static PyObject *startVoiceTalk(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_AUDIO_CHANNEL channelInfo;
    memset(&channelInfo, 0, sizeof(NET_DVR_AUDIO_CHANNEL));
    long cameraNo;
    if (!PyArg_ParseTuple(args, "I", &cameraNo)) {
        return NULL;
    }

    if (cameraNo >= 1)
    {
        cameraNo = ps->struDeviceInfoV40.struDeviceV30.byStartDTalkChan + cameraNo - 1;
    } else {
        cameraNo = ps->struDeviceInfoV40.struDeviceV30.byStartDChan;
    }
    if (ps->lVoiceHandler != -1 && ps->lVoiceHandler != 0)
    {
        if (FALSE == NET_DVR_StopVoiceCom(ps->lVoiceHandler))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_StopVoiceCom error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            return NULL;
        }
    }
    ps->lVoiceHandler = NET_DVR_StartVoiceCom_MR_V30(ps->lUserID, cameraNo, fVoiceDataCallBack, NULL);
    if (ps->lVoiceHandler == -1)
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_StartVoiceCom_MR_V30 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    channelInfo.dwChannelNum = cameraNo;
    if (FALSE == NET_DVR_GetCurrentAudioCompress_V50(ps->lUserID, &channelInfo, &ps->compressAudioType))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_GetCurrentAudioCompress error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    int sampleRate = ps->compressAudioType.byAudioSamplingRate;
    switch(ps->compressAudioType.byAudioSamplingRate)
    {
        case 1: sampleRate = 16000; break;
        case 2: sampleRate = 32000; break;
        case 3: sampleRate = 48000; break;
        case 4: sampleRate = 44100; break;
        case 5: sampleRate = 8000; break;
        default: sampleRate = 8000; break;
        // default:
        //     sprintf(ps->error_buffer, "Unknow sample rate, %d\n", ps->compressAudioType.byAudioSamplingRate);
        //     PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        //     return NULL;
    }
    return Py_BuildValue("{s:i,s:i,s:i}", "AudioEncode", ps->compressAudioType.byAudioEncType, 
                        "SampleRate", sampleRate, "handler", ps->lVoiceHandler);
}

static PyObject *stopVoiceTalk(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    if (ps->lVoiceHandler == -1)
    {
        sprintf(ps->error_buffer, "Voice Talk is not started");
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    if (FALSE == NET_DVR_StopVoiceCom(ps->lVoiceHandler))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_StopVoiceCom error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    ps->lVoiceHandler = 0;

    Py_RETURN_NONE;
}

static PyObject *sendVoice(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    char *inputBuffer;
    Py_ssize_t bufferLength;
    LONG specifiedVoiceHandler = -1;
    if (!PyArg_ParseTuple(args, "y#|iii", &inputBuffer, &bufferLength, &ps->compressAudioType.byAudioEncType, &specifiedVoiceHandler)) {
        return NULL;
    }

    /**************Encode Audio in G.722 Mode**************/

    LPVOID hEncInstance = 0;
    NET_DVR_AUDIOENC_INFO info_param;
    NET_DVR_AUDIOENC_PROCESS_PARAM  enc_proc_param;
    memset(&enc_proc_param, 0 ,sizeof(NET_DVR_AUDIOENC_PROCESS_PARAM));
    
    unsigned char *encode_input[8192];  //20ms
    unsigned char *encoded_data[8192];
    enc_proc_param.in_buf   = (unsigned char *)encode_input;  //输入数据缓冲区，存放编码前PCM原始音频数据
    enc_proc_param.out_buf  = (unsigned char *)encoded_data;  //输出数据缓冲区，存放编码后音频数据
 
    int blockcount= 0;
    const char *encoderName = NULL;
    
    if (ps->compressAudioType.byAudioEncType == 0)
    {
        encoderName = "G722";
        hEncInstance = NET_DVR_InitG722Encoder(&info_param); //初始化G722编码
    } else if (ps->compressAudioType.byAudioEncType == 1)
    {
        encoderName = "G711_U";
        enc_proc_param.g711_type = 0;
        hEncInstance = NET_DVR_InitG711Encoder(&info_param);
        info_param.in_frame_size /= 2;
    } else if (ps->compressAudioType.byAudioEncType == 2)
    {
        encoderName = "G711_A";
        enc_proc_param.g711_type = 1;
        hEncInstance = NET_DVR_InitG711Encoder(&info_param);
    } else if (ps->compressAudioType.byAudioEncType == 4)
    {
        encoderName = "G726";
        // hEncInstance = NET_DVR_InitG726Encoder(&info_param);
    }
    if ((long)hEncInstance == -1)
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_Init%sEncoder error, %d: %s\n", encoderName, pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }

    int offset = 0;
    while (offset < bufferLength)
    {
        blockcount++;
        if (info_param.in_frame_size > bufferLength - offset)
            break;

        memcpy(enc_proc_param.in_buf, inputBuffer + offset, info_param.in_frame_size);
        offset+=info_param.in_frame_size;
        //PCM数据输入，编码成G722
        BOOL ret = FALSE;
        if (ps->compressAudioType.byAudioEncType == 0)
        {
            ret = NET_DVR_EncodeG722Frame(hEncInstance, &enc_proc_param);
        } else if (ps->compressAudioType.byAudioEncType == 1 || ps->compressAudioType.byAudioEncType == 2)
        {
            ret = NET_DVR_EncodeG711Frame(hEncInstance, &enc_proc_param);//((DWORD)enc_proc_param.g711_type, (BYTE *)enc_proc_param.in_buf, (BYTE *)enc_proc_param.out_buf);
            enc_proc_param.out_frame_size = 160;
        } else if (ps->compressAudioType.byAudioEncType == 4)
        {
            // ret = NET_DVR_EncodeG726Frame(hEncInstance, &enc_proc_param);
            ret = false;
        }
        if (ret == FALSE)
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_Encode%sFrame error, %d: %s\n", encoderName, pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            return NULL;
        }
        
        if (!NET_DVR_VoiceComSendData(specifiedVoiceHandler != -1 ? specifiedVoiceHandler : ps->lVoiceHandler, (char*)enc_proc_param.out_buf, enc_proc_param.out_frame_size))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_VoiceComSendData error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            return NULL;
        }
        // printf("sending %d  size = %d\n", blockcount, enc_proc_param.out_frame_size);
        sleep(0.02);
    }

    if (ps->compressAudioType.byAudioEncType == 0)
    {
        NET_DVR_ReleaseG722Decoder(&hEncInstance); //初始化G722编码
    } else if (ps->compressAudioType.byAudioEncType == 1)
    {
        NET_DVR_ReleaseG711Encoder(&hEncInstance);
    } else if (ps->compressAudioType.byAudioEncType == 2)
    {
        NET_DVR_ReleaseG711Encoder(&hEncInstance);
    } else if (ps->compressAudioType.byAudioEncType == 4)
    {
        // hEncInstance = NET_DVR_InitG726Encoder(&info_param);
    }
    
    Py_RETURN_NONE;
}

 

void yv12toYUV(char *outYuv, char *inYv12, int width, int height, int widthStep)
{
    int col, row;
    unsigned int Y, U, V;
    int tmp;
    int idx;

    for (row = 0; row<height; row++)
    {
        idx = row * widthStep;
        // int rowptr = row*width;

        for (col = 0; col<width; col++)
        {
            tmp = (row / 2)*(width / 2) + (col / 2);
            Y = (unsigned int)inYv12[row*width + col];
            U = (unsigned int)inYv12[width*height + width*height / 4 + tmp];
            V = (unsigned int)inYv12[width*height + tmp];
            if ((idx + col * 3 + 2)> (1200 * widthStep))
            {
//printf("row * widthStep=%d,idx+col*3+2=%d.\n",1200 * widthStep,idx+col*3+2);
            }
            outYuv[idx + col * 3] = Y;
            outYuv[idx + col * 3 + 1] = U;
            outYuv[idx + col * 3 + 2] = V;
        }
    }
}


void CALLBACK DecCBFun(int nPort, char * pBuf, int nSize, FRAME_INFO * pFrameInfo, void *pUser, int nReserved2)
{
    long lFrameType = pFrameInfo->nType;

    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)pUser;
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)dp->ps;
    uint8_t *video_src_data[4], *video_dst_data[4];

    if (lFrameType != T_YV12)
    {
        fprintf(stderr, "Non support frame type\n");
        return;
    }

    if (ps == NULL)
    {
        fprintf(stderr, "Error: cannnot found decode context\n");
        return;
    }

    if (dp->sws_ctx == NULL)
    {
        dp->sws_ctx = sws_getContext(pFrameInfo->nWidth, pFrameInfo->nHeight, AV_PIX_FMT_YUV420P,
                     pFrameInfo->nWidth, pFrameInfo->nHeight, AV_PIX_FMT_RGB24,
                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
        /* allocate image where the decoded image will be put */
        int ret = av_image_alloc(video_src_data, dp->video_src_linesize,
                             pFrameInfo->nWidth, pFrameInfo->nHeight, AV_PIX_FMT_YUV420P, 1);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate raw video buffer\n");
            return;
        }
        /* allocate image where the decoded image will be put */
        ret = av_image_alloc(video_dst_data, dp->video_dst_linesize,
                             pFrameInfo->nWidth, pFrameInfo->nHeight, AV_PIX_FMT_RGB24, 1);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate raw video buffer\n");
            return;
        }
        av_freep(&video_src_data[0]);
        av_freep(&video_dst_data[0]);
    }

    if (lFrameType == T_YV12)
    {
        char *yuvData = (char *)malloc(8 + pFrameInfo->nWidth * pFrameInfo->nHeight * 3);

        *((uint32_t *)yuvData) = pFrameInfo->nWidth;
        *((uint32_t *)yuvData+1) = pFrameInfo->nHeight;

        /* convert to destination format */
        video_src_data[0] = (uint8_t*)pBuf;
        video_src_data[1] = (uint8_t*)(pBuf + pFrameInfo->nHeight * pFrameInfo->nWidth + pFrameInfo->nHeight * pFrameInfo->nWidth / 4);
        video_src_data[2] = (uint8_t*)(pBuf + pFrameInfo->nHeight * pFrameInfo->nWidth);
        video_dst_data[0] = (uint8_t *)(yuvData + 8);
        
        sws_scale(dp->sws_ctx, (const uint8_t * const*)video_src_data,
                    dp->video_src_linesize, 0, pFrameInfo->nHeight, video_dst_data, dp->video_dst_linesize);

        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
        elem->lCommand = DVR_VIDEO_DATA;
        // elem->pAlarmInfo = yuvData;
        elem->pAlarmInfo = yuvData;
        elem->dwBufLen = 8 + pFrameInfo->nWidth * pFrameInfo->nHeight * 3;
        memcpy(elem->pAlarmInfo, yuvData, elem->dwBufLen);
        pthread_mutex_lock(&ps->lock);
        TAILQ_INSERT_TAIL(&ps->head, elem, entries);
        pthread_mutex_unlock(&ps->lock);
    }
}

void *decode_thread(void *data)
{
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)data;
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)dp->ps;

    AVClass dec_cls = {
        .class_name = "HIKEVENT",
        .item_name = av_default_item_name,
        .version = LIBAVUTIL_VERSION_INT
    };
    AVClass *pdec_cls = &dec_cls;
    size_t avio_ctx_buffer_size = 1024 * 1024;
    AVFormatContext *pFormatCtx = NULL;
    dp->last_packet_rx = microtime();

    if (ps->decode_way >= 3)
    {
        pthread_create(&dp->process_thread, NULL, process_thread, dp);
    }

    //ffmpeg打开流的回调
    auto onReadData = [](void* pUser, uint8_t* buf, int bufSize)->int
    {
        HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)pUser;
        PyHIKEvent_Object *ps = (PyHIKEvent_Object *)dp->ps;

        while (!dp->stop && (ps->decode_way != 3 || ESRCH != pthread_kill(dp->process_thread, 0)))
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
                sleep(0.02);
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

    //ffmpeg打开流的回调
    auto onWriteData = [](void* pUser, uint8_t* buf, int bufSize)->int
    {
        HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)pUser;
        PyHIKEvent_Object *ps = (PyHIKEvent_Object *)dp->ps;

        struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
        elem->lCommand = DVR_FLV_DATA;
        elem->pAlarmInfo = (char *)malloc(bufSize + 2);
        *(int16_t *)elem->pAlarmInfo = dp->channel;
        memcpy(elem->pAlarmInfo + 2, buf, bufSize);
        elem->dwBufLen = bufSize;
        pthread_mutex_lock(&ps->lock);
        TAILQ_INSERT_TAIL(&ps->head, elem, entries);
        pthread_mutex_unlock(&ps->lock);

        return bufSize;
    };
    
    int video_stream_idx = -1;
    AVCodecContext *dec_ctx = NULL;
    static uint8_t *video_src_data[4] = {NULL};
    static int video_src_bufsize;

    static uint8_t *video_dst_data[4] = {NULL};
    static int video_dst_bufsize;
    AVFrame *frame;
    AVPacket *pkt;

    if (ps->decode_way > 0)
    {
        //ffmpeg-------------------------------
        uint8_t* avio_ctx_buffer = (uint8_t*)av_malloc(avio_ctx_buffer_size);
        if (!avio_ctx_buffer)
        {
            av_log(&pdec_cls, AV_LOG_ERROR, "av_malloc ctx buffer failed!");
            return NULL;
        }
        AVIOContext *pb = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, dp, onReadData, NULL, NULL);
        if (pb == nullptr)  //分配空间失败
        {
            av_freep(&avio_ctx_buffer);
            av_log(&pdec_cls, AV_LOG_ERROR, "avio_alloc_context failed");
            goto end;
        }

        pFormatCtx = init_input_ctx(pb, dp->playback ? 1 : 0);
        if (pFormatCtx == NULL)
        {
            if (pb->buffer != NULL)
            {
                av_freep(&pb->buffer);
                pb->buffer = NULL;
            }
            avio_context_free(&pb);
            goto end;
        }
        dp->pInputCtx = pFormatCtx;

        if (ps->decode_way >= 3)
        {
            uint8_t *avio_output_ctx_buffer = (uint8_t *)av_malloc(avio_ctx_buffer_size);
            if (!avio_output_ctx_buffer)
            {
                av_log(&pdec_cls, AV_LOG_ERROR, "av_malloc ctx buffer failed!");
                return NULL;                
            }
            AVIOContext *pOutputIO = avio_alloc_context(avio_output_ctx_buffer, avio_ctx_buffer_size, 1, dp, NULL, onWriteData, NULL);
            if (pOutputIO == nullptr)  //分配空间失败
            {
                av_freep(&avio_output_ctx_buffer);
                av_log(&pdec_cls, AV_LOG_ERROR, "avio_alloc_context failed");
                goto end;
            }

            /* Create a new format context for the output container format. */
            if (!(dp->pOutputCtx = avformat_alloc_context())) {
                av_log(&pdec_cls, AV_LOG_ERROR, "Could not allocate output format context\n");

                if (pOutputIO->buffer != NULL)
                {
                    av_freep(&pOutputIO->buffer);
                    pOutputIO->buffer = NULL;
                }
                avio_context_free(&pOutputIO);
                goto end;
            }

            if (!(dp->pOutputCtx->oformat = av_guess_format("flv", NULL,
                                                                      NULL))) {
                av_log(&pdec_cls, AV_LOG_ERROR, "Could not find output file format\n");

                if (pOutputIO->buffer != NULL)
                {
                    av_freep(&pOutputIO->buffer);
                    pOutputIO->buffer = NULL;
                }
                avio_context_free(&pOutputIO);
                goto end;
            }
            dp->pOutputCtx->flags |= AVFMT_NOFILE;
            dp->pOutputCtx->pb = pOutputIO;
        }

        int ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (ret < 0) {
            av_log(&pdec_cls, AV_LOG_ERROR, "Could not find %s stream\n",
                    av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
            goto end;
        } else {
            video_stream_idx = ret;
            AVStream *st = pFormatCtx->streams[ret];

            /* find decoder for the stream */
            if (ps->decode_way >= 3)
            {
                AVStream *out_stream;

                out_stream = avformat_new_stream(dp->pOutputCtx, NULL);
                if (!out_stream) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Failed allocating output stream\n");
                    ret = AVERROR_UNKNOWN;
                    return NULL;
                }

                ret = avcodec_parameters_copy(out_stream->codecpar, st->codecpar);
                if (ret < 0) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Failed to copy codec parameters\n");
                    return NULL;
                }
                out_stream->codecpar->codec_tag = 0;
                dp->out_vstream = out_stream;
            } else 
            {
                AVCodec *dec = NULL;
                if (st->codecpar->codec_id == AV_CODEC_ID_H264 && ps->decode_way == 2)
                {
                    dec = avcodec_find_decoder_by_name("h264_cuvid");
                    if (dec == NULL)
                        dec = avcodec_find_decoder_by_name("h264_qsv");
                    if (dec == NULL)
                        dec = avcodec_find_decoder_by_name("h264_v4l2m2m");
                } else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC && ps->decode_way == 2)
                {
                    dec = avcodec_find_decoder_by_name("hevc_cuvid");
                    if (dec == NULL)
                        dec = avcodec_find_decoder_by_name("hevc_qsv");
                    if (dec == NULL)
                        dec = avcodec_find_decoder_by_name("hevc_v4l2m2m");
                }
                if (dec == NULL) {
                   dec = avcodec_find_decoder(st->codecpar->codec_id);
                }
                if (!dec) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Failed to find %s codec\n",
                            av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                    goto end;
                }

                av_log(&pdec_cls, AV_LOG_INFO, "Decode video using %s -> %s\n", dec->name, dec->long_name);

                dec_ctx = avcodec_alloc_context3(dec);
                if (!dec_ctx) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Failed to allocate the %s codec context\n",
                            av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                    goto end;
                }

                /* Copy codec parameters from input stream to output codec context */
                if ((ret = avcodec_parameters_to_context(dec_ctx, st->codecpar)) < 0) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Failed to copy %s codec parameters to decoder context\n",
                            av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                    goto end;
                }

                /* Init the decoders */
                if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Failed to open %s codec\n",
                            av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
                    goto end;
                }

                if (dec_ctx->width == 0 || dec_ctx->height == 0)
                {
                    av_log(&pdec_cls, AV_LOG_ERROR, "invalid video buffer\n");
                    goto end;
                }

                /* allocate image where the decoded image will be put */
                ret = av_image_alloc(video_src_data, dp->video_src_linesize,
                                     dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, 1);
                if (ret < 0) {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Could not allocate raw video buffer\n");
                    goto end;
                }
                video_src_bufsize = ret;
                if (dec_ctx->pix_fmt != AV_PIX_FMT_RGB24)
                {
                    dp->sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                                     dec_ctx->width, dec_ctx->height, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
                    if (!dp->sws_ctx) {
                        av_log(&pdec_cls, AV_LOG_ERROR,
                                "Impossible to create scale context for the conversion "
                                "fmt:%s -> fmt:%s\n",
                                av_get_pix_fmt_name(dec_ctx->pix_fmt), 
                                av_get_pix_fmt_name(AV_PIX_FMT_RGB24));
                        goto end;
                    }

                    /* buffer is going to be written to rawvideo file, no alignment */
                    if ((ret = av_image_alloc(video_dst_data, dp->video_dst_linesize,
                                              dec_ctx->width, dec_ctx->height, AV_PIX_FMT_RGB24, 1)) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Could not allocate destination image\n");
                        goto end;
                    }
                    video_dst_bufsize = ret;
                }
                frame = av_frame_alloc();
                if (!frame) {
                    av_log(NULL, AV_LOG_ERROR, "Could not allocate video frame\n");
                    goto end;
                }
            }
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
        if (ps->decode_way >= 3)
        {
            if (ret < 0) {
                av_log(&pdec_cls, AV_LOG_ERROR, "Could not find %s stream, nb_streams: %d\n",
                        av_get_media_type_string(AVMEDIA_TYPE_AUDIO), pFormatCtx->nb_streams);
            } else {
                AVStream *st = pFormatCtx->streams[ret];

                if (dp->out_astream == NULL)
                {
                    if (init_audio_decoder(dp, st))
                        return NULL;
                }
            }
        } else 
        {
            av_dump_format(pFormatCtx, 0, NULL, 0);
        }
    }
    dp->probedone = true;
    
    while (!dp->stop)
    {
        if (ps->decode_way == 0)
        {
            usleep(100000);
            continue;
        }
        if (dp->realtime_playback && dp->global_pts != 0)
        {
            if (dp->first_pts == 0)
            {
                dp->first_pts = dp->global_pts;
                dp->start_pts = av_gettime_relative();
            }
            double pts = (dp->global_pts - dp->first_pts) * av_q2d(dp->out_vstream->time_base);
            double now = (av_gettime_relative() - dp->start_pts) / 1000000.;
            if (pts / 4 - now > 5)
            {
                dp->first_pts = 0;
            }
            if (pts >= 3 && pts > now * 4 && pts < now + 1)
            {
                usleep(floor((pts - now) * 1000000));   // wait 10ms
                continue;
            }
        }
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
                av_log(&pdec_cls, AV_LOG_ERROR, "Decode Thread: Error %s\n", error);
                av_packet_free(&pkt);
                goto end;
            }
            usleep(1000);   // wait 1ms
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            continue;
        }
        if (ps->decode_way < 3)
        {
            if (pkt->stream_index != video_stream_idx)
            {
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                continue;
            }
            // submit the packet to the decoder
            int ret = avcodec_send_packet(dec_ctx, pkt);
            if (ret < 0) {
                av_log(&pdec_cls, AV_LOG_ERROR, "Error submitting a packet for decoding\n");
                av_packet_free(&pkt);
                goto end;
            }
            // get all the available frames from the decoder
            while (ret >= 0) {
                frame = av_frame_alloc();
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret < 0) {
                    // those two return values are special and mean there is no output
                    // frame available, but there were no errors during decoding
                    if (ret == AVERROR_EOF)
                    {
                        av_log(&pdec_cls, AV_LOG_WARNING, "Video Decode EOF\n");
                        goto end;
                    }
                    if (ret == AVERROR(EAGAIN))
                        break;

                    av_log(&pdec_cls, AV_LOG_ERROR, "Error during decoding\n");
                    av_packet_free(&pkt);
                    goto end;
                }

                // write the frame data to output file
                if (dec_ctx->codec->type == AVMEDIA_TYPE_VIDEO)
                {
                    size_t video_bufsize;
                    uint8_t *video_data;
                    if (dp->sws_ctx)
                    {
                        video_bufsize = video_src_bufsize;
                        av_image_copy(video_src_data, dp->video_src_linesize,
                          (const uint8_t **)(frame->data), frame->linesize,
                          dec_ctx->pix_fmt, dec_ctx->width, dec_ctx->height);
                        video_data = video_src_data[0];
                    } else 
                    {
                        video_bufsize = video_dst_bufsize;
                        av_image_copy(video_src_data, dp->video_src_linesize,
                            (const uint8_t **)(frame->data), frame->linesize,
                            dec_ctx->pix_fmt, dec_ctx->width, dec_ctx->height);
                        /* convert to destination format */
                        sws_scale(dp->sws_ctx, (const uint8_t * const*)video_src_data,
                                  dp->video_src_linesize, 0, dec_ctx->height, video_dst_data, dp->video_dst_linesize);
                        video_data = video_dst_data[0];
                    }

                    struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
                    elem->lCommand = DVR_VIDEO_DATA;
                    // elem->pAlarmInfo = yuvData;
                    elem->pAlarmInfo = (char *)malloc(video_bufsize + 8);
                    elem->dwBufLen = 8 + video_bufsize;
                    *((uint32_t *)elem->pAlarmInfo) = dec_ctx->width;
                    *((uint32_t *)elem->pAlarmInfo+1) = dec_ctx->height;
                    memcpy(elem->pAlarmInfo + 8, video_data, video_bufsize);
                    pthread_mutex_lock(&ps->lock);
                    TAILQ_INSERT_TAIL(&ps->head, elem, entries);
                    pthread_mutex_unlock(&ps->lock);
                }

                av_frame_unref(frame);
                frame = NULL;
                if (ret < 0)
                {
                    av_log(&pdec_cls, AV_LOG_ERROR, "Error to receive frame from decoder, error: %s\n", av_err2str(ret));
                    av_packet_free(&pkt);
                    goto end;
                }
                av_packet_free(&pkt);
            }
        }else 
        {
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
    }
end:
    if (!dp->stop)
    {
        dp->stop = 2;
    }

    while (ps->decode_way != 3 || ESRCH != pthread_kill(dp->process_thread, 0))
    {
        // waiting process thread to terminated
        usleep(1000);
    }

    if (dec_ctx)
        avcodec_free_context(&dec_ctx);

    if (dp->sws_ctx)
        sws_freeContext(dp->sws_ctx);

    if (pFormatCtx)
    {
        avformat_close_input(&pFormatCtx);
    }

    if (dp->pOutputCtx && dp->outputHeaderWrite)
        av_write_trailer(dp->pOutputCtx);

    if (dp->pOutputCtx && !(dp->pOutputCtx->flags & AVFMT_NOFILE))
    {
        avio_closep(&dp->pOutputCtx->pb);
        dp->pOutputCtx->pb = NULL;
    } else 
    {
        if (dp->pOutputCtx->pb->buffer)
        {
            dp->pOutputCtx->pb->buffer = NULL;
            av_freep(&dp->pOutputCtx->pb->buffer);
        }
        avio_context_free(&dp->pOutputCtx->pb);
        dp->pOutputCtx->pb = NULL;
    }

    if (dp->pOutputCtx)
    {
        avformat_free_context(dp->pOutputCtx);
        dp->pOutputCtx = NULL;
    }

    av_freep(&video_src_data[0]);
    av_freep(&video_dst_data[0]);
    if (pFormatCtx)
    {
        avformat_free_context(pFormatCtx);
    }
    struct entry *elem = (struct entry *)calloc(1, sizeof(struct entry));
    elem->lCommand = DVR_FLV_DATA;
    elem->pAlarmInfo = (char *)malloc(1);
    *(int16_t *)elem->pAlarmInfo = dp->channel;
    elem->dwBufLen = 0;
    pthread_mutex_lock(&ps->lock);
    TAILQ_INSERT_TAIL(&ps->head, elem, entries);
    pthread_mutex_unlock(&ps->lock);

    // fprintf(stderr, "Decode Thread: stopped\n");
    if (dp->stop == 1)
    {
        free(dp);
    }
    return NULL;
}


void CALLBACK g_RealDataCallBack_V30(LONG lRealHandle, DWORD dwDataType, BYTE *pBuffer,DWORD dwBufSize,void* dwUser) {
    HIKEvent_DecodeThread *dp = (HIKEvent_DecodeThread *)dwUser;
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)dp->ps;

    switch (dwDataType) {
        case NET_DVR_SYSHEAD: //系统头
        case NET_DVR_AUDIOSTREAMDATA:   // 音频数据
            if (dwBufSize > 0 && ps->decode_way == 0 && dwDataType == NET_DVR_SYSHEAD)
            {
                if (!PlayM4_GetPort(&ps->nPort))
                    break;
                if (!PlayM4_OpenStream(ps->nPort, pBuffer, dwBufSize, 1024 * 1024))
                {
                    fprintf(stderr,"Error: PlayM4_OpenStream %d\n", PlayM4_GetLastError(ps->nPort));
                }
                if (!PlayM4_SetDecCallBackMend(ps->nPort, DecCBFun, dp))
                {
                    fprintf(stderr,"Error: PlayM4_SetDecCallback %d\n", PlayM4_GetLastError(ps->nPort));
                }
                if (!PlayM4_Play(ps->nPort, 0))
                {
                    fprintf(stderr,"Error: PlayM4_Play %d\n", PlayM4_GetLastError(ps->nPort));
                }
            } else if (ps->decode_way > 0 && (dwDataType == NET_DVR_SYSHEAD || ps->decode_way > 2))
            {
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
            }
            break;
        case NET_DVR_STREAMDATA: //码流数据
            if (dwBufSize > 0 && ps->nPort != -1 && ps->decode_way == 0) {
                BOOL inData = PlayM4_InputData(ps->nPort, pBuffer, dwBufSize);
                while (!inData)
                {
                    inData = PlayM4_InputData(ps->nPort, pBuffer, dwBufSize);
                }
            } else if (dwBufSize > 0 && ps->decode_way > 0) {
                program_stream_map_t *psm = (program_stream_map_t *)pBuffer;
                if (psm->packet_start_code_prefix == 0x010000)
                {
                    switch (psm->map_stream_id)
                    {
                        case 0xc0:  // Audio
                            if (ps->decode_way >= 3 && dp->transcode == -1 && dwBufSize == 96)
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
                            } else if (ps->decode_way <= 2)
                            {
                                break;  // Ignore Audio
                            }
                        case 0xba:  // PSH Header
                        case 0xe0:  // Video
                        {
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
                            fprintf(stderr, "rx psm header  %08x \n", htonl(*(uint32_t *)pBuffer));
                            break;
                        case 0xbd:  // Private Stream Data
                            break;
                        default:
                            fprintf(stderr, "rx other data  %08x \n", htonl(*(uint32_t *)pBuffer));
                    }
                } else 
                {
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
                }
                break;
            }
            // printf("NET_DVR_SYSHEAD data,the size is %ld,%d.\n", time(NULL), dwBufSize);
            break; 
        default: //其他数据
            fprintf(stderr, "Other data,the size is %ld,%d.\n", time(NULL), dwBufSize);
            break;
    }
}

static PyObject *startRealPlay(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    long cameraNo;
    long streamType = 0;
    long playback = 0, playback_stop = 0;
    long lRealPlayHandle;
    ps->decode_way = 0;
    if (!PyArg_ParseTuple(args, "I|iiii", &cameraNo, &streamType, &ps->decode_way, &playback, &playback_stop)) {
        PyErr_SetString(PyExc_SyntaxError, "Required Params is <channel>, [stream-type], [decode way], [start], [end]");

        return NULL;
    }

    HIKEvent_DecodeThread *dp = init_decode_ctx();
    if (dp == NULL)
    {
        PyErr_SetString(PyExc_MemoryError, "Init decode context failed");
        
        return NULL;
    }
    dp->channel = cameraNo;
    dp->ps = ps;

    NET_DVR_AUDIO_CHANNEL channelInfo;
    memset(&channelInfo, 0, sizeof(NET_DVR_AUDIO_CHANNEL));

    channelInfo.dwChannelNum = ps->struDeviceInfoV40.struDeviceV30.byStartDTalkChan + cameraNo - 1;
    if (FALSE == NET_DVR_GetCurrentAudioCompress_V50(ps->lUserID, &channelInfo, &dp->compressAudioType))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        fprintf(stderr, "NET_DVR_GetCurrentAudioCompress error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        release_decode_ctx(dp);
        return NULL;
    }

    if (ps->decode_way == 4)
        dp->transcode = 0;
    else
        dp->transcode = 1;
    if (dp->compressAudioType.byAudioEncType == 0)
    {
        // Require G722 Decoder
        dp->g722_decoder = NET_DVR_InitG722Decoder();
    }

    if (playback)
    {
        dp->playback = playback;
        dp->realtime_playback = 1;

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
        struPlayback.struBeginTime.bySecond = tp->tm_sec - 2;

        time_t endtime = playback_stop > 0 ? playback_stop : dp->playback + 300;
        tp = localtime((const time_t *)&endtime);
        struPlayback.struEndTime.wYear = 1900 + tp->tm_year;
        struPlayback.struEndTime.byMonth = 1 + tp->tm_mon;
        struPlayback.struEndTime.byDay = tp->tm_mday;
        struPlayback.struEndTime.byHour = tp->tm_hour;
        struPlayback.struEndTime.byMinute = tp->tm_min;
        struPlayback.struEndTime.bySecond = tp->tm_sec;

        //按时间回放
        lRealPlayHandle = NET_DVR_PlayBackByTime_V50(ps->lUserID, &struPlayback);
        if (lRealPlayHandle < 0)
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_PlayBackByTime_V50 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
            free(dp);
            return NULL;
        }

        if (!NET_DVR_SetPlayDataCallBack_V40(lRealPlayHandle, g_RealDataCallBack_V30, dp))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_SetPlayDataCallBack_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
            free(dp);
            return NULL;
        }

        //---------------------------------------
        //开始
        if (!NET_DVR_PlayBackControl_V40(lRealPlayHandle, NET_DVR_PLAYSTART, NULL, 0, NULL, NULL))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_PlayBackControl_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
            free(dp);
            return NULL;
        }
    } else 
    {
        NET_DVR_PREVIEWINFO struPlayInfo = {0};
        struPlayInfo.hPlayWnd     = 0;  // 仅取流不解码。这是Linux写法，Windows写法是struPlayInfo.hPlayWnd = NULL;
        struPlayInfo.lChannel     = cameraNo; // 通道号
        struPlayInfo.dwStreamType = streamType;  // 0- 主码流，1-子码流，2-码流3，3-码流4，以此类推
        struPlayInfo.dwLinkMode   = 0;  // 0- TCP方式，1- UDP方式，2- 多播方式，3- RTP方式，4-RTP/RTSP，5-RSTP/HTTP
        struPlayInfo.bBlocked     = 1;  // 0- 非阻塞取流，1- 阻塞取流
        //struPlayInfo.dwDisplayBufNum = 1;


        lRealPlayHandle = NET_DVR_RealPlay_V40(ps->lUserID, &struPlayInfo, g_RealDataCallBack_V30, dp); // NET_DVR_RealPlay_V40 实时预览（支持多码流）。
        //lRealPlayHandle = NET_DVR_RealPlay_V30(lUserID, &ClientInfo, NULL, NULL, 0); // NET_DVR_RealPlay_V30 实时预览。
        if (lRealPlayHandle < 0) {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_RealPlay_V40 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
            
            free(dp);
            return NULL;
        }
        
    }

    dp->nPort = lRealPlayHandle;
    pthread_create(&dp->thread, NULL, decode_thread, dp);
    
    struct hik_queue_s *elem = (struct hik_queue_s *)calloc(1, sizeof(struct hik_queue_s));
    if (elem)
    {
        elem->start = lRealPlayHandle;
        elem->data = (char *)dp;
        pthread_mutex_lock(&ps->lock);
        TAILQ_INSERT_TAIL(&ps->decode_ctx, elem, entries);
        pthread_mutex_unlock(&ps->lock);
    }

    return Py_BuildValue("i", lRealPlayHandle);
}

static PyObject *stopRealPlay(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    int lRealPlayHandle;
    if (!PyArg_ParseTuple(args, "i", &lRealPlayHandle)) {
        return NULL;
    }

    struct hik_queue_s *p = NULL;
    HIKEvent_DecodeThread *dp = NULL;
    time_t playback = 0;

    if (!TAILQ_EMPTY(&ps->decode_ctx))
    {
        pthread_mutex_lock(&ps->lock);
        TAILQ_FOREACH(p, &ps->decode_ctx, entries) {
            if (p->start == lRealPlayHandle)
            {
                TAILQ_REMOVE(&ps->decode_ctx, p, entries);
                dp = (HIKEvent_DecodeThread *)p->data;
                playback = dp->playback;
                if (dp->stop)
                {
                    free(p->data);
                } else 
                {
                    dp->stop = 1;
                }
                free(p);
                break;
            }
        }
        pthread_mutex_unlock(&ps->lock);
    }


    if (playback)
    {
        //停止回放
        if (!NET_DVR_StopPlayBack(lRealPlayHandle))
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_StopPlayBack error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);

            return NULL;
        }
    } else 
    {
        if (false == NET_DVR_StopRealPlay(lRealPlayHandle)) {    // 停止取流
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_StopRealPlay error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            
            return NULL;
        }
    }

    if (ps->decodeThread)
    {
        if (ESRCH == pthread_kill(*ps->decodeThread, 0))
        {
            ps->decodeThread = NULL;
        }
    }

    Py_RETURN_NONE;
}


static PyObject *ptzControl(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    LONG lChannelNo;
    DWORD dwPTZCommand;
    DWORD stop = 0, speed = 1;
    if (!PyArg_ParseTuple(args, "II|pI", &lChannelNo, &dwPTZCommand, &stop, &speed)) {
        return NULL;
    }
    if (speed < 0 || speed > 7)
    {
        sprintf(ps->error_buffer, "Speed invalid, accept range 0-7\n");
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        
        return NULL;
    }
    // LIGHT_PWRON 2 接通灯光电源 
    // WIPER_PWRON 3 接通雨刷开关 
    // FAN_PWRON 4 接通风扇开关 
    // HEATER_PWRON 5 接通加热器开关 
    // AUX_PWRON1 6 接通辅助设备开关 
    // AUX_PWRON2 7 接通辅助设备开关 
    // ZOOM_IN 11 焦距变大(倍率变大) 
    // ZOOM_OUT 12 焦距变小(倍率变小) 
    // FOCUS_NEAR 13 焦点前调 
    // FOCUS_FAR 14 焦点后调 
    // IRIS_OPEN 15 光圈扩大 
    // IRIS_CLOSE 16 光圈缩小 
    // TILT_UP 21 云台上仰 
    // TILT_DOWN 22 云台下俯 
    // PAN_LEFT 23 云台左转 
    // PAN_RIGHT 24 云台右转 
    // UP_LEFT 25 云台上仰和左转 
    // UP_RIGHT 26 云台上仰和右转 
    // DOWN_LEFT 27 云台下俯和左转 
    // DOWN_RIGHT 28 云台下俯和右转 
    // PAN_AUTO 29 云台左右自动扫描 
    // TILT_DOWN_ZOOM_IN  58 云台下俯和焦距变大(倍率变大) 
    // TILT_DOWN_ZOOM_OUT 59 云台下俯和焦距变小(倍率变小) 
    // PAN_LEFT_ZOOM_IN 60 云台左转和焦距变大(倍率变大) 
    // PAN_LEFT_ZOOM_OUT 61 云台左转和焦距变小(倍率变小) 
    // PAN_RIGHT_ZOOM_IN 62 云台右转和焦距变大(倍率变大) 
    // PAN_RIGHT_ZOOM_OUT 63 云台右转和焦距变小(倍率变小) 
    // UP_LEFT_ZOOM_IN 64 云台上仰和左转和焦距变大(倍率变大) 
    // UP_LEFT_ZOOM_OUT 65 云台上仰和左转和焦距变小(倍率变小) 
    // UP_RIGHT_ZOOM_IN 66 云台上仰和右转和焦距变大(倍率变大) 
    // UP_RIGHT_ZOOM_OUT 67 云台上仰和右转和焦距变小(倍率变小) 
    // DOWN_LEFT_ZOOM_IN 68 云台下俯和左转和焦距变大(倍率变大) 
    // DOWN_LEFT_ZOOM_OUT 69 云台下俯和左转和焦距变小(倍率变小) 
    // DOWN_RIGHT_ZOOM_IN  70 云台下俯和右转和焦距变大(倍率变大) 
    // DOWN_RIGHT_ZOOM_OUT 71 云台下俯和右转和焦距变小(倍率变小) 
    // TILT_UP_ZOOM_IN 72 云台上仰和焦距变大(倍率变大) 
    // TILT_UP_ZOOM_OUT 73 
    if (!NET_DVR_PTZControlWithSpeed_Other(ps->lUserID, lChannelNo, dwPTZCommand, stop, speed))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_PTZControlWithSpeed_Other error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        
        return NULL;
    }
    Py_RETURN_NONE;
}


static PyObject *ptzPreset(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    LONG lChannelNo;
    DWORD dwPresetIndex;
    DWORD dwPTZPresetCmd = GOTO_PRESET;
    if (!PyArg_ParseTuple(args, "II|I", &lChannelNo, &dwPresetIndex, &dwPTZPresetCmd)) {
        return NULL;
    }
    if (dwPTZPresetCmd != GOTO_PRESET && dwPTZPresetCmd != SET_PRESET && dwPTZPresetCmd != CLE_PRESET)
    {
        sprintf(ps->error_buffer, "CMD Type invalid, accept SET_PRESET(8) CLE_PRESET(9) GOTO_PRESET(39)\n");
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        
        return NULL;
    }
    if (!NET_DVR_PTZPreset_Other(ps->lUserID, lChannelNo, dwPTZPresetCmd, dwPresetIndex))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_PTZPreset_Other error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *ISAPI(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_XML_CONFIG_INPUT inputParam;
    NET_DVR_XML_CONFIG_OUTPUT outputParam;

    memset(&inputParam, 0, sizeof(NET_DVR_XML_CONFIG_INPUT));
    inputParam.dwSize = sizeof(NET_DVR_XML_CONFIG_INPUT);
    
    memset(&outputParam, 0, sizeof(NET_DVR_XML_CONFIG_OUTPUT));
    outputParam.dwSize = sizeof(NET_DVR_XML_CONFIG_OUTPUT);

    if (!PyArg_ParseTuple(args, "s#|s#", &inputParam.lpRequestUrl, &inputParam.dwRequestUrlLen, &inputParam.lpInBuffer, &inputParam.dwInBufferSize)) {
        sprintf(ps->error_buffer, "input params invalid.\n");
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        return NULL;
    }
    outputParam.dwOutBufferSize = 1048576 * 32;    // 32 MB
    outputParam.lpOutBuffer = malloc(outputParam.dwOutBufferSize);
    outputParam.dwStatusSize = 1048576;    // 1 MB
    outputParam.lpStatusBuffer = malloc(outputParam.dwStatusSize);
    if (!NET_DVR_STDXMLConfig(ps->lUserID, &inputParam, &outputParam))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_STDXMLConfig error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        free(outputParam.lpStatusBuffer);
        free(outputParam.lpOutBuffer);

        return NULL;
    }
    PyObject *rc = Py_BuildValue("{s:s,s:s#}", "status", outputParam.lpStatusBuffer, "result", outputParam.lpOutBuffer,
        outputParam.dwReturnedXMLSize);
    free(outputParam.lpStatusBuffer);
    free(outputParam.lpOutBuffer);
    return rc;
}

NET_DVR_TIME_EX timestampToDVRTime(const time_t t)
{
    struct tm *ptm = localtime(&t);
    NET_DVR_TIME_EX r;
    r.wYear = ptm->tm_year + 1900;
    r.byMonth = ptm->tm_mon + 1;
    r.byDay = ptm->tm_mday;
    r.byHour = ptm->tm_hour;
    r.byMinute = ptm->tm_min;
    r.bySecond = ptm->tm_sec;
    return r;
}

time_t DVRTimeTotimestamp(const NET_DVR_TIME_EX r)
{
    struct tm stm;
    struct tm *ptm = &stm;
    memset(ptm, 0, sizeof(struct tm));
    ptm->tm_year = r.wYear - 1900;
    ptm->tm_mon = r.byMonth - 1;
    ptm->tm_mday = r.byDay;
    ptm->tm_hour = r.byHour;
    ptm->tm_min = r.byMinute;
    ptm->tm_sec = r.bySecond;
    
    return mktime(ptm);
}

static PyObject *SmartSearchPicture(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_SMART_SEARCH_PIC_PARA findParams;
    memset(&findParams, 0, sizeof(findParams));
    time_t     tStart = 0, tEnd = 0;

    if (!PyArg_ParseTuple(args, "|IIIII", &findParams.dwChanNo, &findParams.byStreamID, &findParams.wSearchType, &tStart, &tEnd)) {
        return NULL;
    }

    if (tStart == 0)
        tStart = time(NULL) - 86400;
    if (tEnd == 0)
        tEnd = time(NULL);

    findParams.struStartTime = timestampToDVRTime(tStart);
    findParams.struEndTime = timestampToDVRTime(tEnd);

    LONG lHandle;
    if (-1 == (lHandle = NET_DVR_SmartSearchPicture(ps->lUserID, &findParams)))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_SmartSearchPicture error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        
        return NULL;
    }

    PyObject *list = PyList_New(0);
    int hasError = false;
    while (!hasError)
    {
        NET_DVR_SMART_SEARCH_PIC_RET struFindData;
        LONG rc = NET_DVR_FindNextSmartPicture(lHandle, &struFindData);
        switch (rc)
        {
            case NET_DVR_NOMOREFILE:
                hasError = 1;
            case NET_DVR_FILE_SUCCESS:
            {
                const char *infoKey;
                PyObject *payload;
                switch (struFindData.wPicType)
                {
                    case 0:
                        infoKey = "plateInfo";
                        payload = Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:{s:f,s:f,s:f,s:f},s:y#}",
                            "plateType", struFindData.uPicFeature.struPlateInfo.byPlateType,
                            "Color", struFindData.uPicFeature.struPlateInfo.byColor,
                            "Bright", struFindData.uPicFeature.struPlateInfo.byBright,
                            "LicenseLen", struFindData.uPicFeature.struPlateInfo.byLicenseLen,
                            "EntireBelieve", struFindData.uPicFeature.struPlateInfo.byEntireBelieve,
                            "Region", struFindData.uPicFeature.struPlateInfo.byRegion,
                            "Country", struFindData.uPicFeature.struPlateInfo.byCountry,
                            "Area", struFindData.uPicFeature.struPlateInfo.byArea,
                            "PlateSize", struFindData.uPicFeature.struPlateInfo.byPlateSize,
                            "AddInfoFlag", struFindData.uPicFeature.struPlateInfo.byAddInfoFlag,
                            "CRIndex", struFindData.uPicFeature.struPlateInfo.wCRIndex,
                            "Rect", 
                                "x", struFindData.uPicFeature.struPlateInfo.struPlateRect.fX,
                                "y", struFindData.uPicFeature.struPlateInfo.struPlateRect.fY,
                                "w", struFindData.uPicFeature.struPlateInfo.struPlateRect.fWidth,
                                "h", struFindData.uPicFeature.struPlateInfo.struPlateRect.fHeight,
                            "License",
                                struFindData.uPicFeature.struPlateInfo.sLicense, strlen(struFindData.uPicFeature.struPlateInfo.sLicense)
                        ); 
                        break;
                    case 1:
                        infoKey = "faceInfo";
                        payload = Py_BuildValue("{}");
                        break;
                    default:
                        infoKey = "other";
                        payload = Py_BuildValue("{}");
                        break;
                }
                PyList_Append(list, Py_BuildValue("{s:s,s:i,s:i,s:i,s:O}",
                    "FileName", struFindData.sFileName,
                    "FileSize", struFindData.dwFileSize,
                    "PicType", struFindData.wPicType,
                    "Time", DVRTimeTotimestamp(struFindData.struTime),
                    infoKey, payload
                ));
                break;
            }
            case NET_DVR_FILE_NOFIND:
                hasError = -1;
                break;
            case NET_DVR_ISFINDING:
                break;
            case NET_DVR_FILE_EXCEPTION:
            case -1:
                LONG pErrorNo = NET_DVR_GetLastError();
                sprintf(ps->error_buffer, "NET_DVR_FindNextSmartPicture error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
                PyErr_SetString(PyExc_TypeError, ps->error_buffer);
                
                hasError = -1;
                break;
        }
        // printf("%d: %s\n", rc, struFindData.sFileName);
    }

    if (!NET_DVR_CloseSmartSearchPicture(lHandle))
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_CloseSmartSearchPicture error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_TypeError, ps->error_buffer);
        
        return NULL;
    }
    return list;
}


NET_DVR_TIME_SEARCH_COND timestampToSearchTime(const time_t t)
{
    struct tm *ptm = localtime(&t);
    NET_DVR_TIME_SEARCH_COND r;
    r.wYear = ptm->tm_year + 1900;
    r.byMonth = ptm->tm_mon + 1;
    r.byDay = ptm->tm_mday;
    r.byHour = ptm->tm_hour;
    r.byMinute = ptm->tm_min;
    r.bySecond = ptm->tm_sec;
    r.byLocalOrUTC = 0;
    return r;
}

time_t SearchTimeTotimestamp(const NET_DVR_TIME_SEARCH r)
{
    struct tm stm;
    struct tm *ptm = &stm;
    memset(ptm, 0, sizeof(struct tm));
    ptm->tm_year = r.wYear - 1900;
    ptm->tm_mon = r.byMonth - 1;
    ptm->tm_mday = r.byDay;
    ptm->tm_hour = r.byHour;
    ptm->tm_min = r.byMinute;
    ptm->tm_sec = r.bySecond;
    
    return mktime(ptm);
}

static PyObject *FindFile(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_FILECOND_V50 struFileCond;
    memset(&struFileCond, 0, sizeof(NET_DVR_FILECOND_V50));
    time_t     tStart = 0, tEnd = 0;
    struFileCond.dwFileType = 0xFF;
    // struFileCond.dwFileType = 1;
    struFileCond.struStreamID.dwSize = sizeof(NET_DVR_STREAM_INFO);
    struFileCond.struStreamID.dwChannel = 1;
    struFileCond.byIsLocked = 0xFF;
    // struFileCond.byStreamType = 0xFF;    // not support
    struFileCond.byNeedCard = 0;

    int quickSearch = 0, cameraNo = 0;
    if (!PyArg_ParseTuple(args, "I|IIi", &cameraNo, &tStart, &tEnd, &quickSearch)) {
        PyErr_SetString(PyExc_SyntaxError, "Required Params is <channel>, [start], [end], [isQuickSearch]");

        return NULL;
    }

    struFileCond.struStreamID.dwChannel = cameraNo;
    struFileCond.byQuickSearch = quickSearch;
    int foundCount;

    if (tStart == 0)
        tStart = time(NULL) - 86400;
    if (tEnd == 0)
        tEnd = time(NULL);

    struFileCond.struStartTime = timestampToSearchTime(tStart);
    struFileCond.struStopTime = timestampToSearchTime(tEnd);

    //---------------------------------------
    //查找录像文件
    int lFindHandle = NET_DVR_FindFile_V50(ps->lUserID, &struFileCond);
    if (lFindHandle < 0)
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_FindFile_V50 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
        
        return NULL;
    }

    //逐个获取查询文件结果
    NET_DVR_FINDDATA_V50 struFileData;
    int hasError = 0;
    PyObject *list = PyList_New(0);
    while (!hasError)
    {
        int result = NET_DVR_FindNextFile_V50(lFindHandle, &struFileData);
        if (result == NET_DVR_ISFINDING)
        {
            continue;
        }
        else if (result == NET_DVR_FILE_SUCCESS)
        {
            foundCount++;
            PyList_Append(list, Py_BuildValue("{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:l,s:i,s:i}",
                "FileName", struFileData.sFileName,
                "FileSize", struFileData.dwFileSize,
                "FileType", struFileData.byFileType,
                "QuickSearch", struFileData.byQuickSearch,
                "StreamType", struFileData.byStreamType,
                "FileIndex", struFileData.dwFileIndex,
                "Locked", struFileData.byLocked,
                "BigFileType", struFileData.byBigFileType,
                "BigFileLen", struFileData.byBigFileType ? ((int64_t)struFileData.dwTotalLenH << 32) | struFileData.dwTotalLenL : 0,
                "StartTime", SearchTimeTotimestamp(struFileData.struStartTime),
                "StopTime", SearchTimeTotimestamp(struFileData.struStopTime)
            ));
            continue;
        }
        else if (result == NET_DVR_FILE_NOFIND || result == NET_DVR_NOMOREFILE)
        {
            hasError = -1;
            break;
        }
        else
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_FindNextFile_V50 NET_DVR_FILE_EXCEPTION, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
            
            hasError = -1;
            break;
        }
    }

    //停止查找，释放资源
    if (lFindHandle >= 0)
    {
        NET_DVR_FindClose_V30(lFindHandle);
    }

    return list;
}



static PyObject *FindFileStart(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_FILECOND_V50 struFileCond;
    memset(&struFileCond, 0, sizeof(NET_DVR_FILECOND_V50));
    time_t     tStart = 0, tEnd = 0;
    struFileCond.dwFileType = 0xFF;
    // struFileCond.dwFileType = 1;
    struFileCond.struStreamID.dwSize = sizeof(NET_DVR_STREAM_INFO);
    struFileCond.struStreamID.dwChannel = 1;
    struFileCond.byIsLocked = 0xFF;
    // struFileCond.byStreamType = 0xFF;    // not support
    struFileCond.byNeedCard = 0;

    int quickSearch = 0, cameraNo = 0;
    if (!PyArg_ParseTuple(args, "I|IIi", &cameraNo, &tStart, &tEnd, &quickSearch)) {
        PyErr_SetString(PyExc_SyntaxError, "Required Params is <channel>, [start], [end], [isQuickSearch]");

        return NULL;
    }

    struFileCond.struStreamID.dwChannel = cameraNo;
    struFileCond.byQuickSearch = quickSearch;

    if (tStart == 0)
        tStart = time(NULL) - 86400;
    if (tEnd == 0)
        tEnd = time(NULL);

    struFileCond.struStartTime = timestampToSearchTime(tStart);
    struFileCond.struStopTime = timestampToSearchTime(tEnd);

    //---------------------------------------
    //查找录像文件
    int lFindHandle = NET_DVR_FindFile_V50(ps->lUserID, &struFileCond);
    if (lFindHandle < 0)
    {
        LONG pErrorNo = NET_DVR_GetLastError();
        sprintf(ps->error_buffer, "NET_DVR_FindFile_V50 error, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
        PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
        
        return NULL;
    }

    return Py_BuildValue("i", lFindHandle);
}

static PyObject *FindFileNext(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    NET_DVR_FINDDATA_V50 struFileData;
    int lFindHandle;
    if (!PyArg_ParseTuple(args, "i", &lFindHandle)) {
        PyErr_SetString(PyExc_SyntaxError, "Required Params is <SearchHandler>");

        return NULL;
    }

    PyObject *list = PyList_New(0);
    while (true)
    {
        int result = NET_DVR_FindNextFile_V50(lFindHandle, &struFileData);
        if (result == NET_DVR_ISFINDING)
        {
            return Py_BuildValue("{s:O,s:i}", "results", list, "remain", 1);
        }
        else if (result == NET_DVR_FILE_SUCCESS)
        {
            PyList_Append(list, Py_BuildValue("{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:l,s:i,s:i}",
                "FileName", struFileData.sFileName,
                "FileSize", struFileData.dwFileSize,
                "FileType", struFileData.byFileType,
                "QuickSearch", struFileData.byQuickSearch,
                "StreamType", struFileData.byStreamType,
                "FileIndex", struFileData.dwFileIndex,
                "Locked", struFileData.byLocked,
                "BigFileType", struFileData.byBigFileType,
                "BigFileLen", struFileData.byBigFileType ? ((int64_t)struFileData.dwTotalLenH << 32) | struFileData.dwTotalLenL : 0,
                "StartTime", SearchTimeTotimestamp(struFileData.struStartTime),
                "StopTime", SearchTimeTotimestamp(struFileData.struStopTime)
            ));
        }
        else if (result == NET_DVR_FILE_NOFIND || result == NET_DVR_NOMOREFILE)
        {
            NET_DVR_FindClose_V30(lFindHandle);
            return Py_BuildValue("{s:O,s:i}", "results", list, "remain", 0);
        }
        else
        {
            LONG pErrorNo = NET_DVR_GetLastError();
            sprintf(ps->error_buffer, "NET_DVR_FindNextFile_V50 NET_DVR_FILE_EXCEPTION, %d: %s\n", pErrorNo, NET_DVR_GetErrorMsg(&pErrorNo));
            PyErr_SetString(PyExc_RuntimeError, ps->error_buffer);
            NET_DVR_FindClose_V30(lFindHandle);
            
            return Py_BuildValue("{s:O,s:i}", "results", list, "error", 1);
        }
    }
}

static PyObject *getevent(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    pthread_mutex_lock(&ps->lock);
    if (TAILQ_EMPTY(&ps->head))
    {
        pthread_mutex_unlock(&ps->lock);
        Py_RETURN_NONE;
    } else 
    {
        PyObject *payload = Py_None;
        struct entry *p = TAILQ_FIRST(&ps->head);
        TAILQ_REMOVE(&ps->head, p, entries);
        pthread_mutex_unlock(&ps->lock);

        const char *command = NULL;
        switch (p->lCommand)
        {
            case DVR_REMOTE_CALL_COMMAND:   // 自定义事件 -> DVR_REMOTE_CALL_COMMAND
            {
                command = "DVR_REMOTE_CALL_COMMAND";

                NET_DVR_VIDEO_CALL_PARAM *callParam = (NET_DVR_VIDEO_CALL_PARAM*)p->pAlarmInfo;

                payload = Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i}",
                    "CmdType", callParam->dwCmdType,
                    "Period", callParam->wPeriod,
                    "BuildingNumber", callParam->wBuildingNumber, 
                    "UnitNumber", callParam->wUnitNumber, 
                    "FloorNumber", callParam->wFloorNumber, 
                    "RoomNumber", callParam->wRoomNumber);
                break;
            }
            case DVR_REMOTE_CALL_STATUS:
            {
                command = "DVR_REMOTE_CALL_STATUS";

                payload = Py_BuildValue("{s:i}",
                    "status", p->dwBufLen);
                break;                
            }
            case DVR_VIDEO_DATA:   // 自定义事件 -> DVR_REMOTE_CALL_COMMAND
                command = "DVR_VIDEO_DATA";

                payload = Py_BuildValue("y#", p->pAlarmInfo, p->dwBufLen);
                break;
            case DVR_FLV_DATA:
                command = "DVR_FLV_DATA";

                payload = Py_BuildValue("{s:i,s:y#}", "channel", *(uint16_t *)p->pAlarmInfo, "data", p->pAlarmInfo+2, p->dwBufLen);
                break;
            case COMM_ALARM_RULE: // 行为分析信息 -> NET_VCA_RULE_ALARM
            {
                    command="COMM_ALARM_RULE";
                    NET_VCA_RULE_ALARM *struAlarmInfo = (NET_VCA_RULE_ALARM *)p->pAlarmInfo;
                    NET_VCA_DEV_INFO *dev = &struAlarmInfo->struDevInfo;
                    
                    payload = Py_BuildValue("{s:i,s:i,s:s,s:{s:s,s:i,s:i,s:i},s:i,s:y#}",
                        "alarmTime", struAlarmInfo->dwAbsTime,
                        "wEventType", struAlarmInfo->struRuleInfo.wEventTypeEx,
                        "RuleName", struAlarmInfo->struRuleInfo.byRuleName, 
                        "DevInfo",
                                "IP", dev->struDevIP.sIpV4,
                                "port", dev->wPort,
                                "channel", dev->byChannel,
                                "IvmsChannel", dev->byIvmsChannel,
                        "picType", struAlarmInfo->byPicTransType,
                        "image", struAlarmInfo->pImage, struAlarmInfo->dwPicDataLen);
                    break;
            }
            case COMM_ALARM_PDC: // 客流量统计报警信息 -> NET_DVR_PDC_ALRAM_INFO
            {
                    command="COMM_ALARM_PDC";
                    break;
            }
            case COMM_RULE_INFO_UPLOAD: // 事件数据信息 -> NET_DVR_RULE_INFO_ALARM
            {
                    command="COMM_RULE_INFO_UPLOAD";
                    break;
            }
            case COMM_ALARM_FACE: // 人脸检测识别报警信息 -> NET_DVR_FACEDETECT_ALARM
            {
                    command="COMM_ALARM_FACE";
                    break;
            }
            case COMM_UPLOAD_FACESNAP_RESULT: // 人脸抓拍结果信息 -> NET_VCA_FACESNAP_RESULT
            {
                    command="COMM_UPLOAD_FACESNAP_RESULT";

                    NET_VCA_FACESNAP_RESULT struAlarmInfo;
                    memcpy(&struAlarmInfo, p->pAlarmInfo, sizeof(NET_VCA_FACESNAP_RESULT));
                    NET_VCA_DEV_INFO *dev = &struAlarmInfo.struDevInfo;
                    NET_VCA_HUMAN_FEATURE *human = &struAlarmInfo.struFeature;

                    // const char *pBuffer1 = (p->pAlarmInfo+offsetof(NET_VCA_FACESNAP_RESULT, pBuffer1));
                    // printf("%.*s\n", struAlarmInfo.dwFacePicLen, pBuffer1);
                    payload = Py_BuildValue("{s:s,s:i,s:{s:s,s:i,s:i,s:i},s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i}", //,s:f,s:f,s:O}",
                            "StorageIP", struAlarmInfo.sStorageIP,
                            "UploadEventDataType", struAlarmInfo.byUploadEventDataType,
                            "DevInfo",
                                "IP", dev->struDevIP.sIpV4,
                                "port", dev->wPort,
                                "channel", dev->byChannel,
                                "IvmsChannel", dev->byIvmsChannel,
                            // "Human",
                                "AgeGroup", human->byAgeGroup,    //年龄段,参见 HUMAN_AGE_GROUP_ENUM
                                "Sex", human->bySex,         //性别, 0-表示“未知”（算法不支持）,1 – 男 , 2 – 女, 0xff-算法支持，但是没有识别出来
                                "EyeGlass", human->byEyeGlass,    //是否戴眼镜 0-表示“未知”（算法不支持）,1 – 不戴, 2 – 戴,0xff-算法支持，但是没有识别出来
                                //抓拍图片人脸年龄的使用方式，如byAge为15,byAgeDeviation为1,表示，实际人脸图片年龄的为14-16之间
                                "Age", human->byAge,//年龄 0-表示“未知”（算法不支持）,0xff-算法支持，但是没有识别出来
                                "AgeDeviation", human->byAgeDeviation,//年龄误差值
                                "Ethnic", human->byEthnic,
                                "Mask", human->byMask,       //是否戴口罩 0-表示“未知”（算法不支持）,1 – 不戴, 2 –戴普通眼镜, 3 –戴墨镜,0xff-算法支持，但是没有识别出来
                                "Smile", human->bySmile,      //是否微笑 0-表示“未知”（算法不支持）,1 – 不微笑, 2 – 微笑, 0xff-算法支持，但是没有识别出来
                                "FaceExpression", human->byFaceExpression,    /* 表情,参见FACE_EXPRESSION_GROUP_ENUM*/
                                "Beard", human->byBeard, // 胡子, 0-不支持，1-没有胡子，2-有胡子，0xff-unknow表示未知,算法支持未检出
                                "Race", human->byRace,
                                "Hat", human->byHat // 帽子, 0-不支持,1-不戴帽子,2-戴帽子,0xff-unknow表示未知,算法支持未检出
    
                    );
                    // ,
                    //         "FaceWidth", struAlarmInfo.struRect.fWidth,
                    //         "FaceHeight", struAlarmInfo.struRect.fHeight,
                    //         "FacePic", PyByteArray_FromStringAndSize(pBuffer1, struAlarmInfo.dwFacePicLen)
                    //     );
                    
                    break;
            }
            case COMM_FACECAPTURE_STATISTICS_RESULT: // 人脸抓拍人员统计信息 -> NET_DVR_FACECAPTURE_STATISTICS_RESULT
            {
                    command="COMM_FACECAPTURE_STATISTICS_RESULT";
                    break;
            }
            case COMM_SNAP_MATCH_ALARM: // 人脸黑名单比对结果信息 -> NET_VCA_FACESNAP_MATCH_ALARM
            {
                    command="COMM_SNAP_MATCH_ALARM";

                    NET_VCA_FACESNAP_MATCH_ALARM struAlarmInfo;
                    memcpy(&struAlarmInfo, p->pAlarmInfo, sizeof(NET_VCA_FACESNAP_MATCH_ALARM));
                    NET_VCA_HUMAN_ATTRIBUTE *person = &struAlarmInfo.struBlackListInfo.struBlackListInfo.struAttribute;
                    NET_VCA_DEV_INFO *dev = &struAlarmInfo.struSnapInfo.struDevInfo;

                    payload = Py_BuildValue("{s:s,s:i,s:f,s:i,s:i,s:i,s:i,s:i,s:f,s:{s:s,s:i,s:i,s:i},s:{s:i,s:i,s:s,s:i}}", 
                            "StorageIP", struAlarmInfo.sStorageIP,
                            "LivenessDetectionStatus", struAlarmInfo.byLivenessDetectionStatus, //活体检测状态：0-保留，1-未知（检测失败），2-非真人人脸，3-真人人脸，4-未开启活体检测
                            "Similarity", struAlarmInfo.fSimilarity,
                            "Mask", struAlarmInfo.byMask,           //抓拍图是否戴口罩，0-保留，1-未知，2-不戴口罩，3-戴口罩
                            "Smile", struAlarmInfo.bySmile,         //抓拍图是否微笑，0-保留，1-未知，2-不微笑，3-微笑
                            "Contrast", struAlarmInfo.byContrastStatus,
                            "Sex", struAlarmInfo.struSnapInfo.bySex,        //性别，0-未知，1-男，2-女,0xff-算法支持，但是没有识别出来
                            "Glasses", struAlarmInfo.struSnapInfo.byGlasses,     //是否带眼镜，0-未知，1-是，2-否,3-戴墨镜, 0xff-算法支持，但是没有识别出来
                            "StayDuration", struAlarmInfo.struSnapInfo.fStayDuration,
                            "DevInfo",
                                "IP", dev->struDevIP.sIpV4,
                                "port", dev->wPort,
                                "channel", dev->byChannel,
                                "IvmsChannel", dev->byIvmsChannel,
                            "PersonInfo", 
                                "RegisterID", struAlarmInfo.struBlackListInfo.struBlackListInfo.dwRegisterID,
                                "Type", struAlarmInfo.struBlackListInfo.struBlackListInfo.byType,
                                "Name", person->byName,
                                "Sex", person->bySex
                        );
                    
                    break;
            }
            case COMM_ALARM_FACE_DETECTION: // 人脸侦测报警信息 -> NET_DVR_FACE_DETECTION
            {
                    command="COMM_ALARM_FACE_DETECTION";
                    break;
            }
            case COMM_ALARM_TARGET_LEFT_REGION: // 教师离开讲台报警 -> NET_DVR_TARGET_LEFT_REGION_ALARM
            {
                    command="COMM_ALARM_TARGET_LEFT_REGION";
                    break;
            }
            case COMM_PEOPLE_DETECTION_UPLOAD: // 人员侦测信息 -> NET_DVR_PEOPLE_DETECTION_RESULT
            {
                    command="COMM_PEOPLE_DETECTION_UPLOAD";
                    break;
            }
            case COMM_VCA_ALARM:    // 智能检测通用报警(Json数据结构)  人体目标识别报警Json数据
            {
                    command="COMM_VCA_ALARM";

                    payload = Py_BuildValue("s#", p->pAlarmInfo, (Py_ssize_t)p->dwBufLen);
                    break;
            }
            case COMM_SIGN_ABNORMAL_ALARM: // 体征异常报警(Json数据结构) -> EVENT_JSON
            {
                    command="COMM_SIGN_ABNORMAL_ALARM";
                    break;
            }
            case COMM_ALARM_VQD_EX: // VQD报警信息 -> NET_DVR_VQD_ALARM
            {
                    command="COMM_ALARM_VQD_EX";
                    break;
            }
            case COMM_ALARM_VQD: // VQD诊断报警信息 -> NET_DVR_VQD_DIAGNOSE_INFO
            {
                    command="COMM_ALARM_VQD";
                    break;
            }
            case COMM_SCENECHANGE_DETECTION_UPLOAD: // 场景变更报警信息 -> NET_DVR_SCENECHANGE_DETECTION_RESULT
            {
                    command="COMM_SCENECHANGE_DETECTION_UPLOAD";
                    break;
            }
            case COMM_CROSSLINE_ALARM: // 压线报警信息 -> NET_DVR_CROSSLINE_ALARM
            {
                    command="COMM_CROSSLINE_ALARM";
                    break;
            }
            case COMM_ALARM_AUDIOEXCEPTION: // 声音报警信息 -> NET_DVR_AUDIOEXCEPTION_ALARM
            {
                    command="COMM_ALARM_AUDIOEXCEPTION";
                    break;
            }
            case COMM_ALARM_DEFOCUS: // 虚焦报警信息 -> NET_DVR_DEFOCUS_ALARM
            {
                    command="COMM_ALARM_DEFOCUS";
                    break;
            }
            case COMM_SWITCH_LAMP_ALARM: // 开关灯检测报警信息 -> NET_DVR_SWITCH_LAMP_ALARM
            {
                    command="COMM_SWITCH_LAMP_ALARM";
                    break;
            }
            case COMM_UPLOAD_HEATMAP_RESULT: // 热度图报警信息 -> NET_DVR_HEATMAP_RESULT
            {
                    command="COMM_UPLOAD_HEATMAP_RESULT";
                    break;
            }
            case COMM_FIREDETECTION_ALARM: // 火点检测报警信息 -> NET_DVR_FIREDETECTION_ALARM
            {
                    command="COMM_FIREDETECTION_ALARM";
                    break;
            }
            case COMM_THERMOMETRY_DIFF_ALARM: // 温差报警信息 -> NET_DVR_THERMOMETRY_DIFF_ALARM
            {
                    command="COMM_THERMOMETRY_DIFF_ALARM";
                    break;
            }
            case COMM_THERMOMETRY_ALARM: // 温度报警信息 -> NET_DVR_THERMOMETRY_ALARM
            {
                    command="COMM_THERMOMETRY_ALARM";
                    break;
            }
            case COMM_ALARM_SHIPSDETECTION: // 船只检测报警信息 -> NET_DVR_SHIPSDETECTION_ALARM
            {
                    command="COMM_ALARM_SHIPSDETECTION";
                    break;
            }
            case COMM_ALARM_AID: // 交通事件报警信息 -> NET_DVR_AID_ALARM
            {
                    command="COMM_ALARM_AID";
                    break;
            }
            case COMM_ALARM_TPS: // 交通参数统计报警信息 -> NET_DVR_TPS_ALARM
            {
                    command="COMM_ALARM_TPS";
                    break;
            }
            case COMM_ALARM_TFS: // 交通取证报警信息 -> NET_DVR_TFS_ALARM
            {
                    command="COMM_ALARM_TFS";
                    break;
            }
            case COMM_ALARM_TPS_V41: // 交通参数统计报警信息(扩展) -> NET_DVR_TPS_ALARM_V41
            {
                    command="COMM_ALARM_TPS_V41";
                    break;
            }
            case COMM_ALARM_AID_V41: // 交通事件报警信息扩展 -> NET_DVR_AID_ALARM_V41
            {
                    command="COMM_ALARM_AID_V41";
                    break;
            }
            case COMM_UPLOAD_PLATE_RESULT: // 交通抓拍结果 -> NET_DVR_PLATE_RESULT
            {
                    command="COMM_UPLOAD_PLATE_RESULT";
                    break;
            }
            case COMM_ITS_PLATE_RESULT: // 交通抓拍结果(新报警信息) -> NET_ITS_PLATE_RESULT
            {
                    command="COMM_ITS_PLATE_RESULT";
                    break;
            }
            case COMM_ITS_TRAFFIC_COLLECT: // 交通统计数据上传 -> NET_ITS_TRAFFIC_COLLECT
            {
                    command="COMM_ITS_TRAFFIC_COLLECT";
                    break;
            }
            // case COMM_ITS_BLACKLIST_ALARM: // 车辆黑名单报警上传 -> NET_ITS_ECT_BLACKLIST
            // {
            //         command="COMM_ITS_BLACKLIST_ALARM";
            //         break;
            // }
            case COMM_VEHICLE_CONTROL_LIST_DSALARM: // 车辆黑白名单数据需要同步报警上传 -> NET_DVR_VEHICLE_CONTROL_LIST_DSALARM
            {
                    command="COMM_VEHICLE_CONTROL_LIST_DSALARM";
                    NET_DVR_VEHICLE_CONTROL_LIST_DSALARM *struAlarmInfo = (NET_DVR_VEHICLE_CONTROL_LIST_DSALARM *)p->pAlarmInfo;

                    payload = Py_BuildValue("{s:i,s:y#}", 
                            "dataIndex", struAlarmInfo->dwDataIndex,
                            "operateIndex", struAlarmInfo->sOperateIndex, MAX_OPERATE_INDEX_LEN
                        );
                    break;
            }
            case COMM_VEHICLE_CONTROL_ALARM: // 黑白名单车辆报警上传 -> NET_DVR_VEHICLE_CONTROL_ALARM
            {
                    command="COMM_VEHICLE_CONTROL_ALARM";
                    NET_DVR_VEHICLE_CONTROL_ALARM *struAlarmInfo = (NET_DVR_VEHICLE_CONTROL_ALARM *)p->pAlarmInfo;
                    
                    payload = Py_BuildValue("{s:i,s:i,s:i,s:y#,s:y#,s:i,s:y#}", 
                            "ListType", struAlarmInfo->byListType,
                            "PlateType", struAlarmInfo->byPlateType,
                            "PlateColor", struAlarmInfo->byPlateColor,
                            "License", struAlarmInfo->sLicense, strlen(struAlarmInfo->sLicense),
                            "CardNo", struAlarmInfo->sCardNo, strlen(struAlarmInfo->sCardNo),
                            "PicType", struAlarmInfo->byPicType,
                            "Picture", struAlarmInfo->pPicData, struAlarmInfo->dwPicDataLen
                        );
                    break;
            }
            case COMM_FIRE_ALARM: // 消防报警上传 -> NET_DVR_FIRE_ALARM
            {
                    command="COMM_FIRE_ALARM";
                    break;
            }
            case COMM_VEHICLE_RECOG_RESULT: // 车辆二次识别结果上传 -> NET_DVR_VEHICLE_RECOG_RESULT
            {
                    command="COMM_VEHICLE_RECOG_RESULT";
                    break;
            }
            case COMM_ALARM_SENSORINFO_UPLOAD: // 传感器上传信息 -> NET_DVR_SENSOR_INFO_UPLOAD
            {
                    command="COMM_ALARM_SENSORINFO_UPLOAD";
                    break;
            }
            case COMM_ALARM_CAPTURE_UPLOAD: // 抓拍图片上传 -> NET_DVR_CAPTURE_UPLOAD
            {
                    command="COMM_ALARM_CAPTURE_UPLOAD";
                    break;
            }
            case COMM_SIGNAL_LAMP_ABNORMAL: // 信号灯异常检测上传 -> NET_DVR_SIGNALLAMP_DETCFG
            {
                    command="COMM_SIGNAL_LAMP_ABNORMAL";
                    break;
            }
            case COMM_ALARM_TPS_REAL_TIME: // TPS实时过车数据上传 -> NET_DVR_TPS_REAL_TIME_INFO
            {
                    command="COMM_ALARM_TPS_REAL_TIME";
                    break;
            }
            case COMM_ALARM_TPS_STATISTICS: // TPS统计过车数据上传 -> NET_DVR_TPS_STATISTICS_INFO
            {
                    command="COMM_ALARM_TPS_STATISTICS";
                    break;
            }
            case COMM_ITS_ROAD_EXCEPTION: // 路口设备异常报警信息 -> NET_ITS_ROADINFO
            {
                    command="COMM_ITS_ROAD_EXCEPTION";
                    break;
            }
            case COMM_ITS_EXTERNAL_CONTROL_ALARM: // 指示灯外控报警信息 -> NET_DVR_EXTERNAL_CONTROL_ALARM
            {
                    command="COMM_ITS_EXTERNAL_CONTROL_ALARM";
                    break;
            }
            case COMM_ITS_GATE_FACE: // 出入口人脸抓拍数据 -> NET_ITS_GATE_FACE
            {
                    command="COMM_ITS_GATE_FACE";
                    break;
            }
            case COMM_ITS_GATE_ALARMINFO: // 出入口控制机数据 -> NET_DVR_GATE_ALARMINFO
            {
                    command="COMM_ITS_GATE_ALARMINFO";
                    NET_DVR_GATE_ALARMINFO *pAlarmInfo = (NET_DVR_GATE_ALARMINFO *)p->pAlarmInfo;
                    static char alarmTime[48];
                    sprintf(alarmTime, "%04d-%02d-%02d %02d:%02d:%02d", pAlarmInfo->struAlarmTime.wYear, pAlarmInfo->struAlarmTime.byMonth, pAlarmInfo->struAlarmTime.byDay, pAlarmInfo->struAlarmTime.byHour, pAlarmInfo->struAlarmTime.byMinute, pAlarmInfo->struAlarmTime.bySecond);

                    payload = Py_BuildValue("{s:i,s:i,s:i,s:s}", "alarmType", pAlarmInfo->byAlarmType, 
                        "externalDevType", pAlarmInfo->byExternalDevType,
                        "externalDevStatus", pAlarmInfo->byExternalDevStatus,
                        "alarmTime", alarmTime);
                    break;
            }
            case COMM_GATE_CHARGEINFO_UPLOAD: // 出入口付费信息 -> NET_DVR_GATE_CHARGEINFO
            {
                    command="COMM_GATE_CHARGEINFO_UPLOAD";
                    break;
            }
            case COMM_TME_VEHICLE_INDENTIFICATION: // 出入口控制器TME车辆抓拍信息 -> NET_DVR_TME_VEHICLE_RESULT
            {
                    command="COMM_TME_VEHICLE_INDENTIFICATION";
                    break;
            }
            case COMM_GATE_CARDINFO_UPLOAD: // 出入口卡片信息 -> NET_DVR_GATE_CARDINFO
            {
                    command="COMM_GATE_CARDINFO_UPLOAD";
                    break;
            }
            case COMM_ALARM_ALARMHOST: // 网络报警主机报警信息 -> NET_DVR_ALARMHOST_ALARMINFO
            {
                    command="COMM_ALARM_ALARMHOST";
                    break;
            }
            case COMM_SENSOR_VALUE_UPLOAD: // 模拟量数据实时信息 -> NET_DVR_SENSOR_ALARM
            {
                    command="COMM_SENSOR_VALUE_UPLOAD";
                    break;
            }
            case COMM_SENSOR_ALARM: // 模拟量报警信息 -> NET_DVR_SENSOR_ALARM
            {
                    command="COMM_SENSOR_ALARM";
                    break;
            }
            case COMM_SWITCH_ALARM: // 开关量报警信息 -> NET_DVR_SWITCH_ALARM
            {
                    command="COMM_SWITCH_ALARM";
                    break;
            }
            case COMM_ALARMHOST_EXCEPTION: // 故障报警信息 -> NET_DVR_ALARMHOST_EXCEPTION_ALARM
            {
                    command="COMM_ALARMHOST_EXCEPTION";
                    break;
            }
            case COMM_ALARMHOST_SAFETYCABINSTATE: // 防护舱状态信息 -> NET_DVR_ALARMHOST_SAFETYCABINSTATE
            {
                    command="COMM_ALARMHOST_SAFETYCABINSTATE";
                    break;
            }
            case COMM_ALARMHOST_ALARMOUTSTATUS: // 报警输出口或警号状态信息 -> NET_DVR_ALARMHOST_ALARMOUTSTATUS
            {
                    command="COMM_ALARMHOST_ALARMOUTSTATUS";
                    break;
            }
            case COMM_ALARMHOST_CID_ALARM: // 报警主机CID报告报警上传 -> NET_DVR_CID_ALARM
            {
                    command="COMM_ALARMHOST_CID_ALARM";
                    break;
            }
            case COMM_ALARMHOST_EXTERNAL_DEVICE_ALARM: // 报警主机外接设备报警信息 -> NET_DVR_485_EXTERNAL_DEVICE_ALARMINFO
            {
                    command="COMM_ALARMHOST_EXTERNAL_DEVICE_ALARM";
                    break;
            }
            case COMM_ALARMHOST_DATA_UPLOAD: // 报警数据信息 -> NET_DVR_ALARMHOST_DATA_UPLOAD
            {
                    command="COMM_ALARMHOST_DATA_UPLOAD";
                    break;
            }
            case COMM_ALARM_WIRELESS_INFO: // 无线网络信息上传 -> NET_DVR_ALARMWIRELESSINFO
            {
                    command="COMM_ALARM_WIRELESS_INFO";
                    break;
            }
            case COMM_ALARM: // 移动侦测、视频丢失、遮挡、IO信号量等报警信息(V3.0以下版本支持的设备) -> NET_DVR_ALARMINFO
            {
                    command="COMM_ALARM";
                    NET_DVR_ALARMINFO *pAlarmInfo = (NET_DVR_ALARMINFO *)p->pAlarmInfo;

                    payload = Py_BuildValue("{s:i,s:i,s:i,s:y#,s:y#,s:y#,s:y#}", "alarmType", pAlarmInfo->dwAlarmType, 
                        "alarmInputNumber", pAlarmInfo->dwAlarmInputNumber,
                        "alarmOutputNumber", pAlarmInfo->dwAlarmOutputNumber, MAX_ALARMOUT * sizeof(DWORD),
                        "alarmRelateChannel", pAlarmInfo->dwAlarmRelateChannel, MAX_CHANNUM * sizeof(DWORD),
                        "channel", pAlarmInfo->dwChannel, MAX_CHANNUM * sizeof(DWORD),
                        "diskNumber", pAlarmInfo->dwDiskNumber, MAX_DISKNUM * sizeof(DWORD));
                    break;
            }
            case COMM_ALARM_V30: // 移动侦测、视频丢失、遮挡、IO信号量等报警信息(V3.0以上版本支持的设备) -> NET_DVR_ALARMINFO_V30
            {
                    command="COMM_ALARM_V30";
                    break;
            }
            case COMM_ALARM_V40: // 移动侦测、视频丢失、遮挡、IO信号量等报警信息，报警数据为可变长 -> NET_DVR_ALARMINFO_V40
            {
                    command="COMM_ALARM_V40";
                    NET_DVR_ALARMINFO_V40 struAlarmInfo;
                    memcpy(&struAlarmInfo, p->pAlarmInfo, sizeof(NET_DVR_ALARMINFO_V40));

                    // // 0-信号量报警，1-硬盘满，2-信号丢失，3-移动侦测，4-硬盘未格式化，5-写硬盘出错，6-遮挡报警，7-制式不匹配，8-非法访问，9-视频信号异常，10-录像异常，11-智能场景变化，12-阵列异常，13-前端/录像分辨率不匹配，15-智能侦测，16-POE供电异常，17-录播主机报警，18-TME语音对讲请求报警，23-脉冲报警，24-人脸库硬盘异常，25-人脸库变更，26-人脸库图片变更
                    static char alarmTime[48];
                    sprintf(alarmTime, "%04d-%02d-%02d %02d:%02d:%02d", struAlarmInfo.struAlarmFixedHeader.struAlarmTime.wYear, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byMonth, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byDay, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byHour, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byMinute, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.bySecond);
                    switch (struAlarmInfo.struAlarmFixedHeader.dwAlarmType)
                    {
                        case 0:
                        case 23:
                            payload = Py_BuildValue("{s:i,s:s,s:i,s:i,s:i}", "dwAlarmType", struAlarmInfo.struAlarmFixedHeader.dwAlarmType, "alarmTime", alarmTime,
                                "AlarmInputNo", struAlarmInfo.struAlarmFixedHeader.uStruAlarm.struIOAlarm.dwAlarmInputNo,
                                "TrigerAlarmOutNum", struAlarmInfo.struAlarmFixedHeader.uStruAlarm.struIOAlarm.dwTrigerAlarmOutNum,
                                "TrigerRecordChanNum", struAlarmInfo.struAlarmFixedHeader.uStruAlarm.struIOAlarm.dwTrigerRecordChanNum);
                            break;
                        case 2:
                        case 3:
                        case 6:
                        case 9:
                        case 10:
                        case 11:
                        case 13:
                        case 15:
                        case 16:
                            payload = Py_BuildValue("{s:i,s:s,s:i}", "dwAlarmType", struAlarmInfo.struAlarmFixedHeader.dwAlarmType, "alarmTime", alarmTime,
                                "AlarmChannelCount", struAlarmInfo.struAlarmFixedHeader.uStruAlarm.struAlarmChannel.dwAlarmChanNum);
                            break;
                        case 1:
                        case 4:
                        case 5:
                            payload = Py_BuildValue("{s:i,s:s,s:i}", "dwAlarmType", struAlarmInfo.struAlarmFixedHeader.dwAlarmType, "alarmTime", alarmTime, "errorHDD", struAlarmInfo.struAlarmFixedHeader.uStruAlarm.struAlarmHardDisk.dwAlarmHardDiskNum);
                            break;
                        default:
                            payload = Py_BuildValue("{s:i,s:s}", "dwAlarmType", struAlarmInfo.struAlarmFixedHeader.dwAlarmType, "alarmTime", alarmTime);
                    }
                    
                    break;
            }
            case COMM_IPCCFG: // 混合型DVR、NVR等在IPC接入配置改变时的报警信息 -> NET_DVR_IPALARMINFO
            {
                    command="COMM_IPCCFG";
                    break;
            }
            case COMM_IPCCFG_V31: // 混合型DVR、NVR等在IPC接入配置改变时的报警信息（扩展） -> NET_DVR_IPALARMINFO_V31
            {
                    command="COMM_IPCCFG_V31";
                    break;
            }
            case COMM_IPC_AUXALARM_RESULT: // PIR报警、无线报警、呼救报警信息 -> NET_IPC_AUXALARM_RESULT
            {
                    command="COMM_IPC_AUXALARM_RESULT";
                    break;
            }
            case COMM_ALARM_DEVICE: // CVR设备报警信息，由于通道值大于256而扩展 -> NET_DVR_ALARMINFO_DEV
            {
                    command="COMM_ALARM_DEVICE";
                    break;
            }
            case COMM_ALARM_DEVICE_V40: // CVR设备报警信息扩展(增加报警信息子结构) -> NET_DVR_ALARMINFO_DEV_V40
            {
                    command="COMM_ALARM_DEVICE_V40";
                    break;
            }
            case COMM_ALARM_CVR: // CVR外部报警信息 -> NET_DVR_CVR_ALARM
            {
                    command="COMM_ALARM_CVR";
                    break;
            }
            case COMM_TRADEINFO: // ATM DVR交易信息 -> NET_DVR_TRADEINFO
            {
                    command="COMM_TRADEINFO";
                    break;
            }
            case COMM_ALARM_HOT_SPARE: // 热备异常报警（N+1模式异常报警）信息 -> NET_DVR_ALARM_HOT_SPARE
            {
                    command="COMM_ALARM_HOT_SPARE";
                    break;
            }
            case COMM_ALARM_BUTTON_DOWN_EXCEPTION: // 按钮按下报警信息(IP可视对讲主机) -> NET_BUTTON_DOWN_EXCEPTION_ALARM
            {
                    command="COMM_ALARM_BUTTON_DOWN_EXCEPTION";
                    break;
            }
            case COMM_ALARM_ACS: // 门禁主机报警信息 -> NET_DVR_ACS_ALARM_INFO
            {
                    command="COMM_ALARM_ACS";
                    NET_DVR_ACS_ALARM_INFO *acsAlarm = (NET_DVR_ACS_ALARM_INFO *)p->pAlarmInfo;
                    NET_DVR_ACS_EVENT_INFO *acsEvent = (NET_DVR_ACS_EVENT_INFO *)&acsAlarm->struAcsEventInfo;

                    static char alarmTime[48];
                    sprintf(alarmTime, "%04d-%02d-%02d %02d:%02d:%02d", acsAlarm->struTime.dwYear, acsAlarm->struTime.dwMonth, acsAlarm->struTime.dwDay, acsAlarm->struTime.dwHour, acsAlarm->struTime.dwMinute, acsAlarm->struTime.dwSecond);
                    static char cardNo[ACS_CARD_NO_LEN * 2];
                    for (int i = 0; i < ACS_CARD_NO_LEN; i++)
                        sprintf(cardNo+i*2, "%02x", acsEvent->byCardNo[i]);
                    // Major & Minor reference to https://open.hikvision.com/hardware/structures/NET_DVR_ACS_ALARM_INFO.html
                    payload = Py_BuildValue("{s:s,s:i,s:i,s:s,s:{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i}}", "AlarmTime", alarmTime, "Major", acsAlarm->dwMajor, "Minor", acsAlarm->dwMinor,
                                    "NetUser", acsAlarm->sNetUser,
                                    "ACSInfo",
                                        "CardNo", cardNo,
                                        "CardType", acsEvent->byCardType,
                                        "WhiteListNo", acsEvent->byWhiteListNo,
                                        "ReportChannel", acsEvent->byReportChannel,
                                        "CardReaderKind", acsEvent->byCardReaderKind,
                                        "CardReaderNo", acsEvent->dwCardReaderNo,
                                        "DoorNo", acsEvent->dwDoorNo,
                                        "VerifyNo", acsEvent->dwVerifyNo,
                                        "AccessChannel", acsEvent->wAccessChannel,
                                        "DeviceNo", acsEvent->byDeviceNo,
                                        "DistractControlNo", acsEvent->byDistractControlNo,
                                        "EmployeeNo", acsEvent->dwEmployeeNo,
                                        "LocalControllerID", acsEvent->wLocalControllerID,
                                        "Type", acsEvent->byType,
                                        "SwipeCardType", acsEvent->bySwipeCardType,
                                        "ChannelControllerID", acsEvent->byChannelControllerID,
                                        "ChannelControllerLampID", acsEvent->byChannelControllerLampID,
                                        "ChannelControllerIRAdaptorID", acsEvent->byChannelControllerIRAdaptorID,
                                        "ChannelControllerIREmitterID", acsEvent->byChannelControllerIREmitterID
                            );
                    
                    break;
            }
            case COMM_SCREEN_ALARM: // 多屏控制器上传的报警信息 -> NET_DVR_SCREENALARMCFG
            {
                    command="COMM_SCREEN_ALARM";
                    break;
            }
            case COMM_ALARM_LCD: // LCD屏幕报警信息 -> NET_DVR_LCD_ALARM
            {
                    command="COMM_ALARM_LCD";
                    break;
            }
            case COMM_UPLOAD_VIDEO_INTERCOM_EVENT: // 可视对讲事件记录信息 -> NET_DVR_VIDEO_INTERCOM_EVENT
            {
                    command="COMM_UPLOAD_VIDEO_INTERCOM_EVENT";
                    break;
            }
            case COMM_ALARM_VIDEO_INTERCOM: // 可视对讲报警信息 -> NET_DVR_VIDEO_INTERCOM_ALARM
            {
                    command="COMM_ALARM_VIDEO_INTERCOM";
                    break;
            }
            case COMM_ALARM_DEC_VCA: // 解码器智能解码报警信息 -> NET_DVR_DEC_VCA_ALARM
            {
                    command="COMM_ALARM_DEC_VCA";
                    break;
            }
            case COMM_GISINFO_UPLOAD: // GIS信息 -> NET_DVR_GIS_UPLOADINFO
            {
                    command="COMM_GISINFO_UPLOAD";
                    break;
            }
            case COMM_VANDALPROOF_ALARM: // 防破坏报警信息 -> NET_DVR_VANDALPROOF_ALARM
            {
                    command="COMM_VANDALPROOF_ALARM";
                    break;
            }
            case COMM_ALARM_STORAGE_DETECTION: // 存储智能检测报警信息 -> NET_DVR_STORAGE_DETECTION_ALARM
            {
                    command="COMM_ALARM_STORAGE_DETECTION";
                    break;
            }
            case COMM_ALARM_ALARMGPS: // GPS报警信息 -> NET_DVR_GPSALARMINFO
            {
                    command="COMM_ALARM_ALARMGPS";
                    break;
            }
            case COMM_ALARM_SWITCH_CONVERT: // 交换机报警信息 -> NET_DVR_SWITCH_CONVERT_ALARM
            {
                    command="COMM_ALARM_SWITCH_CONVERT";
                    break;
            }
            case COMM_INQUEST_ALARM: // 审讯主机报警信息 -> NET_DVR_INQUEST_ALARM
            {
                    command="COMM_INQUEST_ALARM";
                    break;
            }
            case COMM_PANORAMIC_LINKAGE_ALARM: // 鹰眼全景联动到位事件信息 -> NET_DVR_PANORAMIC_LINKAGE
            {
                    command="COMM_PANORAMIC_LINKAGE_ALARM";
                    break;
            }
            default:
            {
                char tcommand[128];
                sprintf(tcommand, "UNKNOW command: %lx", p->lCommand);
                PyObject *arglist = Py_BuildValue("{s:s,s:i,s:O}", "command", tcommand, "size", p->dwBufLen);
                
                free(p->pAlarmInfo);
                free(p);
                return arglist;
            }
        }
        PyObject *arglist = Py_BuildValue("{s:s,s:i,s:O}", "command", command, "size", p->dwBufLen, "payload", payload);
        Py_DECREF(payload);
        free(p->pAlarmInfo);
        free(p);
        return arglist;
    }    
}


static void release(PyObject *self) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    pthread_mutex_destroy(&ps->lock);

    if (ps->lVoiceHandler > 0)
    {
        if (!NET_DVR_StopVoiceCom(ps->lCallHandle))
        {
            sprintf(ps->error_buffer, "NET_DVR_StopVoiceCom error, %d\n", NET_DVR_GetLastError());
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            NET_DVR_Logout_V30(ps->lUserID);
            NET_DVR_Cleanup(); 
            return;
        }
    }

    if (ps->callChannelOpened)
    {
        if (!NET_DVR_StopRemoteConfig(ps->lCallHandle))
        {
            sprintf(ps->error_buffer, "NET_DVR_StopRemoteConfig error, %d\n", NET_DVR_GetLastError());
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            NET_DVR_Logout_V30(ps->lUserID);
            NET_DVR_Cleanup(); 
            return;
        }
        ps->callChannelOpened = false;
    }

    if (ps->alarmChannelOpened)
    {
        if (!NET_DVR_CloseAlarmChan_V30(ps->lHandle))
        {
            sprintf(ps->error_buffer, "NET_DVR_CloseAlarmChan_V30 error, %d\n", NET_DVR_GetLastError());
            PyErr_SetString(PyExc_TypeError, ps->error_buffer);
            NET_DVR_Logout_V30(ps->lUserID);
            NET_DVR_Cleanup(); 
            return;
        }
        ps->alarmChannelOpened = false;
    }

    //关闭预览
    NET_DVR_Logout(ps->lUserID);
    //注销用户
    NET_DVR_Cleanup();
}


// Method definition object for this extension, these argumens mean:
// ml_name: The name of the method
// ml_meth: Function pointer to the method implementation
// ml_flags: Flags indicating special features of this method, such as
//          accepting arguments, accepting keyword arguments, being a
//          class method, or being a static method of a class.
// ml_doc:  Contents of this method's docstring
static PyMethodDef hiknvsevent_methods[] = { 
    {   
        "getevent", getevent, METH_NOARGS,
        "Get Event"
    },
    {   
        "unlock", unlock, METH_NOARGS,
        "Unlock"
    },
    {
        "receiveAlarmEvent", receiveAlarmEvent, METH_NOARGS,
        "Receive Alarm Event"
    },
    {
        "receiveRemoteCall", receiveRemoteCall, METH_NOARGS,
        "Receive Remote Call"
    },
    {
        "stopRemoteCall", stopRemoteCall, METH_NOARGS,
        "Stop Receive Remote Call"
    },
    {
        "remoteCallCommand", remoteCallCommand, METH_VARARGS,
        "Remote Call Command"
    },
    {
        "startVoiceTalk", startVoiceTalk, METH_VARARGS,
        "Start Voice Talk"
    },
    {
        "stopVoiceTalk", stopVoiceTalk, METH_NOARGS,
        "Stop Voice Talk"
    },
    {
        "sendVoice", sendVoice, METH_VARARGS,
        "Send Voice to Remote"
    },
    {
        "addDVRChannel", addDVRChannel, METH_VARARGS,
        "Add DVR Channel"
    },
    {
        "delDVRChannel", delDVRChannel, METH_VARARGS,
        "Del DVR Channel"
    },
    {
        "SmartSearchPicture", SmartSearchPicture, METH_VARARGS,
        "Smart Search Picture"
    },
    {
        "FindFile", FindFile, METH_VARARGS,
        "Find DVR File"
    },
    {
        "FindFileStart", FindFileStart, METH_VARARGS,
        "Find DVR File"
    },
    {
        "FindFileNext", FindFileNext, METH_VARARGS,
        "Find DVR File"
    },
    {
        "getDeviceInfo", getDeviceInfo, METH_NOARGS,
        "Get DVR Channel Info"
    },
    {
        "getPicture", getPicture, METH_VARARGS,
        "Get Picture"
    },
    {
        "getChannelName", getChannelName, METH_VARARGS,
        "Get DVR Channel Info"
    },
    {
        "startRealPlay", startRealPlay, METH_VARARGS,
        "Start Play"
    },
    {
        "stopRealPlay", stopRealPlay, METH_VARARGS,
        "Stop Play"
    },
    {
        "ptzControl", ptzControl, METH_VARARGS,
        "PTZ Control"
    },
    {
        "ptzPreset", ptzPreset, METH_VARARGS,
        "PTZ Preset"
    },
    {
        "ISAPI", ISAPI, METH_VARARGS,
        "ISAPI Command"
    },
    {NULL, NULL, 0, NULL}
};


static PyTypeObject HIKEventType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hikevent.hikevent",
    .tp_basicsize = sizeof(PyHIKEvent_Object),
    .tp_itemsize = 0,
    .tp_dealloc = release,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "HIKVision Event objects",
    .tp_methods = hiknvsevent_methods,
    .tp_new = hikevent_new,
};

// Module definition
// The arguments of this structure tell Python what to call your extension,
// what it's methods are and where to look for it's method definitions
static struct PyModuleDef hikevent_definition = { 
    PyModuleDef_HEAD_INIT,
    "hikevent",
    "A Python module that process hiknvsevent",
    -1, 
    NULL
};

// Module initialization
// Python calls this function when importing your extension. It is important
// that this function is named PyInit_[[your_module_name]] exactly, and matches
// the name keyword argument in setup.py's setup() call.
PyMODINIT_FUNC PyInit_hikevent(void) {
    PyObject *m;
    if (PyType_Ready(&HIKEventType) < 0)
        return NULL;

    m = PyModule_Create(&hikevent_definition);
    if (m == NULL)
        return NULL;

    Py_INCREF(&HIKEventType);
    if (PyModule_AddObject(m, "hikevent", (PyObject *) &HIKEventType) < 0) {
        Py_DECREF(&HIKEventType);
        Py_DECREF(m);
        return NULL;
    }

    return m;


    // return PyModule_Create(&bbip_definition);
}

}