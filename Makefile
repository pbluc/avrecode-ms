include ffmpeg/config.mak

# CXXFLAGS += -Wconversion -Wno-sign-conversion 
#-O3
CXXFLAGS += -fsanitize=address -std=c++17 -Wall -g -I. -I./ffmpeg \
	$(shell pkg-config --cflags protobuf)
LDLIBS = -fsanitize=address  -L./ffmpeg/libavdevice -lavdevice \
	 -L./ffmpeg/libavformat -lavformat \
	 -L./ffmpeg/libavfilter -lavfilter \
	 -L./ffmpeg/libavcodec -lavcodec \
	 -L./ffmpeg/libswresample -lswresample \
	 -L./ffmpeg/libswscale -lswscale \
	 -L./ffmpeg/libavutil -lavutil \
	 $(EXTRALIBS) \
	 $(shell pkg-config --libs protobuf) \
	 -lstdc++

recode: recode.o test.o recode.pb.o ffmpeg/libavcodec/libavcodec.a

recode.o: recode.cpp test.h recode.pb.h arithmetic_code.h cabac_code.h

test.o: test.cpp test.h

recode.pb.cc recode.pb.h: recode.proto
	protoc --cpp_out=. $<

test/arithmetic_code: test/arithmetic_code.o

test/arithmetic_code.o: test/arithmetic_code.cpp arithmetic_code.h cabac_code.h

clean:
	rm -f recode recode.o perftest.o recode.pb.{cc,h,o}
