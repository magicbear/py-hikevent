import hikevent
import time

cam = hikevent.hikevent("192.168.28.2", "viewer", "123456")
while True:
    evt = cam.getevent()
    if evt is not None:
        print(evt)
    else:
        time.sleep(0.001)