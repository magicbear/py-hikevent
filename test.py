import hikevent
import time
import sys
import getopt
import json
import base64

try:
    opts, _ = getopt.getopt(sys.argv[1:], "hu:p:H:", ["help", "user=", "passwd=", "ip="])
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
except getopt.GetoptError:
    show_help()
    sys.exit(2)

cam = hikevent.hikevent(ip, user, passwd)
while True:
    evt = cam.getevent()
    if evt is not None:
        if evt['command'] == "COMM_UPLOAD_FACESNAP_RESULT":
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