############################################################
#
# ffsrv 
# 	ffmpeg-based live streamer
#
# A.Myznikov Mar 17, 2016
#
############################################################

SHELL = /bin/bash

TARGET = ../bin/ffsrv

all: $(TARGET)


#arch  = aarch64-rpi3-linux-gnueabi
arch   = 

mydir   := $(shell pwd)
cross   := $(if $(arch),$(strip $(arch))-,)
sysroot =
lsysroot= $(mydir)/external/$(strip $(arch))/sysroot
DESTDIR = 
prefix  = /usr/local
bindir  = $(prefix)/bin
incdir  = $(prefix)/include
libdir  = $(prefix)/lib

SUBDIRS = .\
	./cc \
	./cc/debug \
	./cc/daemon \
	./cc/ipaddrs \
	./cc/coscheduler \
	./cc/strfuncs \
	./cc/hashfuncs \
	./cc/pathfuncs \
	./cc/ffmpeg \
	./cc/ffgop \
	./cc/getifaddrs \
	./cc/sockopt \
	./cc/resolv \
	./cc/http \
	./cc/rtsp \
	./ffsrv \
	./ffsrv/ffobject \
	./ffsrv/ffobject/ffinput \
	./ffsrv/ffobject/ffoutput \
	./ffsrv/ffobject/ffdecoder \
	./ffsrv/ffobject/ffencoder \
	./ffsrv/ffobject/ffmixer \
	./ffsrv/ffobject/ffsink \
	./ffsrv/ffobject/ffsegments \
	./ffsrv/http \
	./ffsrv/http/http-client-context \
	./ffsrv/http/http-client-context/http-get-file \
	./ffsrv/http/http-client-context/http-get-directory \
	./ffsrv/http/http-client-context/http-get-online-stream \
	./ffsrv/http/http-client-context/http-get-segments \
	./ffsrv/http/http-client-context/http-post-online-stream \
	./ffsrv/ffcfg \
	./ffsrv/rtsp \
	./ffsrv/ffdb



INCLUDES+=$(foreach s,$(SUBDIRS),$(addprefix -I,$(s))) -I$(lsysroot)/usr/include
SOURCES = $(foreach s,$(SUBDIRS), $(wildcard $s/*.c)) 
HEADERS = $(foreach s,$(SUBDIRS), $(wildcard $s/*.h))
MODULES = $(foreach s,$(SOURCES),$(addsuffix .o,$(basename $(s))))


# order is important
FFLIBS := avdevice avfilter avformat avcodec swscale swresample avresample postproc avutil
PKG_CONFIG := PKG_CONFIG_PATH=$(lsysroot)/usr/lib/pkgconfig pkg-config
FFLIST := $(shell $(PKG_CONFIG) --static --libs $(addprefix lib,$(FFLIBS)))
FFMPEG_LIBS := $(addprefix $(lsysroot)/usr/lib/lib,$(addsuffix .a,$(FFLIBS)))  $(filter-out $(addprefix -l,$(FFLIBS)),$(sort $(FFLIST)))


#FFMPEG_LIBS := $(sort $(shell $(PKG_CONFIG) --static --libs $(addprefix lib,$(FFLIBS))))

#FFMPEG_LIBS := $(addprefix -l, $(FFLIBS))
#x264 opencore-amrnb opencore-amrwb mp3lame ssl crypto z m



# C preprocessor flags
CPPFLAGS=$(DEFINES) $(INCLUDES)

# C Compiler and flags
CC = $(cross)gcc -std=gnu99
CFLAGS= -Wall -Wextra -g3 -O3

# Loader Flags And Libraries
LD=$(CC)
LDFLAGS = $(CFLAGS)

# STRIP = $(cross)strip --strip-all
STRIP = @echo "don't strip "

LDLIBS += $(FFMPEG_LIBS) -L$(lsysroot)/usr/lib -lssl -lcrypto -lpcl -lmagic -lm -lrt -ldl -lpthread


#########################################


$(MODULES): $(HEADERS) Makefile
$(TARGET) : $(MODULES) Makefile
	$(LD) $(LDFLAGS) $(MODULES) $(LDLIBS) -o $@

clean:
	$(RM) $(MODULES)

distclean: clean
	$(RM) $(TARGET)

install: $(TARGET) $(DESTDIR)/$(bindir)
	cp $(TARGET) $(DESTDIR)/$(bindir) && $(STRIP) $(DESTDIR)/$(bindir)/$(TARGET)

uninstall:
	$(RM) $(DESTDIR)/$(bindir)/$(TARGET)


$(DESTDIR)/$(bindir):
	mkdir -p $@

test:
	@echo "PKG_CONFIG=$(PKG_CONFIG)"
	@echo "FFLIST=$(FFLIST)"
	@echo "FFMPEG_LIBS=$(FFMPEG_LIBS)"
