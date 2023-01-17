LDFLAGS=$(shell pkg-config --cflags --libs librtmp libavcodec libavformat libavutil libswresample libswscale) -Iinclude -L/usr/local/lib/hcnetsdk -Llib/HCNetSDKCom/ -lHCCore -lhcnetsdk -lPlayCtrl -lAudioRender -lSuperRender -lpthread
CFLAGS=$(shell pkg-config --cflags librtmp libavcodec libavformat libavutil libswresample libswscale) -Iinclude -L/usr/local/lib/hcnetsdk -Llib/HCNetSDKCom/

all: hikrtmp

%.o: %.cpp
	g++ $< $(LDFLAGS) -pipe -g -Wall -c

hikrtmp: hikrtmp.cpp hikbase.o
	g++ hikbase.o $< $(LDFLAGS) -pipe -g -Wall -o $@