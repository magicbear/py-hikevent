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

using namespace std;

#define HPR_ERROR -1
#define HPR_OK 0

#define DVR_REMOTE_CALL_COMMAND 0x4000001

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

    pthread_mutex_t lock;
    NET_DVR_DEVICEINFO_V30 struDeviceInfo;
    NET_DVR_USER_LOGIN_INFO struLoginInfo = {0};
    NET_DVR_DEVICEINFO_V40 struDeviceInfoV40 = {0};

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



static PyObject *
hikevent_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char error_buffer[256];
    PyHIKEvent_Object *ps;
    ps = (PyHIKEvent_Object *) type->tp_alloc(type, 0);
    if (ps == NULL) return NULL;

    static char *kwlist[] = {"host", "user", "passwd", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sss", kwlist, &ps->ip, &ps->user, &ps->passwd)) {
        PyErr_SetString(PyExc_TypeError,
                        "No enough params provide, required: IP, user, passwd");
        return NULL;
    }

    TAILQ_INIT(&ps->head);
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

    ps->lUserID = NET_DVR_Login_V40(&ps->struLoginInfo, &ps->struDeviceInfoV40);

    if (ps->lUserID < 0)
    {
        sprintf(error_buffer, "Login error, %d\n", NET_DVR_GetLastError());
        PyErr_SetString(PyExc_TypeError, error_buffer);
        NET_DVR_Cleanup(); 
        return NULL;
    }

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
    struSetupAlarmParam.byRetVQDAlarmType = TRUE; //Prefer VQD Alarm type of NET_DVR_VQD_ALARM
    struSetupAlarmParam.byRetAlarmTypeV40 = TRUE;   // variable size
    struSetupAlarmParam.byLevel = 2;    // low priority
    struSetupAlarmParam.byFaceAlarmDetection = 1;//m_comFaceAlarmType.GetCurSel();
                
    struSetupAlarmParam.byRetDevInfoVersion = TRUE;
    struSetupAlarmParam.byAlarmInfoType = 1;
    struSetupAlarmParam.bySupport = 4 | 8;
    ps->lHandle = NET_DVR_SetupAlarmChan_V50(ps->lUserID, &struSetupAlarmParam, NULL, 0);

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
        NET_DVR_VIDEO_CALL_PARAM *callParam = (NET_DVR_VIDEO_CALL_PARAM*)lpBuffer;
    } else {
        printf("Receive RemoteCall Callback Event %d\n", dwType);
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

void CALLBACK fdwVoiceDataCallBack(LONG lVoiceComHandle, char *pRecvDataBuffer, DWORD dwBufSize, BYTE byAudioFlag, DWORD pUser)
{

}

void CALLBACK fVoiceDataCallBack(LONG lVoiceComHandle, char *pRecvDataBuffer, DWORD dwBufSize, BYTE byAudioFlag, void *pUser)
{

}

static PyObject *startVoiceTalk(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    long cameraNo;
    if (!PyArg_ParseTuple(args, "I", &cameraNo)) {
        return NULL;
    }

    if (cameraNo > 1)
    {
        cameraNo = ps->struDeviceInfoV40.struDeviceV30.byStartDTalkChan + 10 - 1;
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

    if (FALSE == NET_DVR_GetCurrentAudioCompress(ps->lUserID, &ps->compressAudioType))
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
    return Py_BuildValue("{s:i,s:i}", "AudioEncode", ps->compressAudioType.byAudioEncType, 
                        "SampleRate", sampleRate);
}

static PyObject *stopVoiceTalk(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    if (ps->lVoiceHandler == 0 || ps->lVoiceHandler == -1)
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

    Py_RETURN_NONE;
}

static PyObject *sendVoice(PyObject *self, PyObject *args) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    char *inputBuffer;
    Py_ssize_t bufferLength;
    if (!PyArg_ParseTuple(args, "y#", &inputBuffer, &bufferLength)) {
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
        
        if (!NET_DVR_VoiceComSendData(ps->lVoiceHandler, (char*)enc_proc_param.out_buf, enc_proc_param.out_frame_size))
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
            case COMM_ALARM_RULE: // 行为分析信息 -> NET_VCA_RULE_ALARM
            {
                    command="COMM_ALARM_RULE";
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

                    const char *pBuffer1 = (p->pAlarmInfo+offsetof(NET_VCA_FACESNAP_RESULT, pBuffer1));
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
                    break;
            }
            case COMM_VEHICLE_CONTROL_ALARM: // 黑白名单车辆报警上传 -> NET_DVR_VEHICLE_CONTROL_ALARM
            {
                    command="COMM_VEHICLE_CONTROL_ALARM";
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
                    static char alarmTime[24];
                    sprintf(alarmTime, "%04d-%02d-%02d %02d:%02d:%02d.%03d", struAlarmInfo.struAlarmFixedHeader.struAlarmTime.wYear, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byMonth, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byDay, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byHour, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byMinute, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.bySecond, struAlarmInfo.struAlarmFixedHeader.struAlarmTime.byRes);
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

                    static char alarmTime[24];
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
        }
        PyObject *arglist = Py_BuildValue("{s:s,s:i,s:O}", "command", command, "size", p->dwBufLen, "payload", payload);
        free(p->pAlarmInfo);
        free(p);
        return arglist;
    }    
}


static void release(PyObject *self) {
    PyHIKEvent_Object *ps = (PyHIKEvent_Object *)self;

    pthread_mutex_destroy(&ps->lock);

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