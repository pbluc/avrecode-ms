include ffmpeg/config.mak

CFLAGS = -Wall -g -I./ffmpeg
CXXFLAGS = -std=c++1y -Wall -g -I./ffmpeg
LDLIBS = -L./ffmpeg/libavdevice -lavdevice \
	 -L./ffmpeg/libavformat -lavformat \
	 -L./ffmpeg/libavfilter -lavfilter \
	 -L./ffmpeg/libavcodec -lavcodec \
	 -L./ffmpeg/libswresample -lswresample \
	 -L./ffmpeg/libswscale -lswscale \
	 -L./ffmpeg/libavutil -lavutil \
	 $(EXTRALIBS)

recode: recode.o

clean:
	rm -f recode recode.o
