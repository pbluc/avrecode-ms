include ffmpeg/config.mak

CXXFLAGS = -std=c++1y -Wall -g -I./ffmpeg \
	   $(shell pkg-config --cflags protobuf)
LDLIBS = -L./ffmpeg/libavdevice -lavdevice \
	 -L./ffmpeg/libavformat -lavformat \
	 -L./ffmpeg/libavfilter -lavfilter \
	 -L./ffmpeg/libavcodec -lavcodec \
	 -L./ffmpeg/libswresample -lswresample \
	 -L./ffmpeg/libswscale -lswscale \
	 -L./ffmpeg/libavutil -lavutil \
	 $(EXTRALIBS) \
	 $(shell pkg-config --libs protobuf) \
	 -lc++

recode: recode.o recode.pb.o ffmpeg/libavcodec/libavcodec.a

recode.o: recode.cpp recode.pb.h

recode.pb.cc recode.pb.h: recode.proto
	protoc --cpp_out=. $<

clean:
	rm -f recode recode.o recode.pb.{cc,h,o}
