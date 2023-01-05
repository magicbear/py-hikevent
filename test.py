import hikevent
import time
import sys
import getopt
import json
import base64
import websockets
import asyncio
import threading
import struct
from scipy.signal import resample
import numpy as np
import cv2

decode_way = 0
try:
    opts, _ = getopt.getopt(sys.argv[1:], "hu:p:H:d:", ["help", "user=", "passwd=", "ip=", "decode_way="])
    ip = None
    user = None
    passwd = None
    for opt, arg in opts:
        if opt in ["-h", "--help"]:
            sys.exit()
        if opt in ["-u", "--user"]:
            user = arg
        if opt in ["-p", "--passwd"]:
            passwd = arg
        if opt in ["-H", "--ip"]:
            ip = arg
        if opt in ["-d", "--decode_way"]:
            decode_way = int(arg)
except getopt.GetoptError:
    show_help()
    sys.exit(2)

cam = hikevent.hikevent(ip, user, passwd)
# print(cam.addDVRChannel(9))
# handler = cam.getChannelName(10)
# print(cam.addDVRChannel(8))
# print(handler)
#print(cam.SmartSearchPicture(43,0,0,int(time.time()-3600*12)))
cam.startRealPlay(3, 0, decode_way)
# cam.startRealPlay(3, 1, 0)

# print(cam.ISAPI("GET /ISAPI/System/deviceInfo", ""))
# print(cam.ISAPI("GET /ISAPI/System/deviceInfo", ""))

# cam.sendVoice(open("test_16k.pcm", "rb").read(), 16)

gpu_orig = None
gpu_bgr = None

while True:
    evt = cam.getevent()
    if evt is not None:
        if evt['command'] == "DVR_VIDEO_DATA":
            # print("Get frame")
            size = struct.unpack("=LL", evt['payload'][0:8]);
            if len(evt['payload']) - 8 == size[0] * size[1] * 3:
                frame = np.frombuffer(evt['payload'][8:], dtype=np.uint8).reshape((size[1], size[0], 3))
                frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
            else:
                frame = np.frombuffer(evt['payload'][8:], dtype=np.uint8).reshape((size[1] + int(size[1] / 2), size[0]))
                frame = cv2.cvtColor(frame, cv2.COLOR_YUV420p2RGB)
            # if gpu_orig is None:
            #     gpu_orig = cv2.cuda_GpuMat(size[1] + int(size[1] / 2), size[0], cv2.CV_8UC2)
            #     gpu_bgr = cv2.cuda_GpuMat(size[1], size[0], cv2.CV_8UC3)
            #
            # gpu_orig.upload(frame)
            # cv2.cuda.cvtColor(gpu_orig, cv2.COLOR_YUV420p2RGB, gpu_bgr)
            # frame = gpu_bgr.download().astype(np.uint8)

            cv2.imshow("frame_%d" % decode_way, cv2.resize(frame, (640, 360)))
            cv2.waitKey(1)
        elif evt['command'] == "COMM_UPLOAD_FACESNAP_RESULT":
            # evt['payload']['FacePic'] = base64.b64encode(evt['payload']['FacePic']).decode('utf-8')
            print(json.dumps({
                'devid': 'facesnap',
                "cls": "HIK-FACESNAP",
                "timestamp": int(time.time()),
                'status': evt['payload']
            }), flush=True)
        elif evt['command'] == "COMM_SNAP_MATCH_ALARM":
            evt['payload']['PersonName'] = evt['payload']['PersonInfo']['Name']
            evt['payload']['PersonID'] = evt['payload']['PersonInfo']['RegisterID']
            print(json.dumps({
                'devid': 'facematch',
                "cls": "HIK-FACERECOGNIZE",
                "timestamp": int(time.time()),
                'status': evt['payload']
            }), flush=True)
        elif evt['command'] == "COMM_ALARM_V40":
            if evt['payload']['dwAlarmType'] == 0:  # 信号量
                print(json.dumps({
                    'devid': 'alarm-%d' % (evt['payload']['AlarmInputNo']),
                    "cls": "HIK-ALARMIN",
                    "timestamp": int(time.time()),
                    'status': evt['payload']
                }), flush=True)
        else:
            print(evt, file=sys.stderr)
    else:
        time.sleep(0.01)
