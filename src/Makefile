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


cross   =
sysroot =
DESTDIR =
prefix  = /usr/local
bindir  = $(prefix)/bin
incdir  = $(prefix)/include
libdir  = $(prefix)/lib


# find -L . -type d 
SUBDIRS = .\
	./cc \
	./cc/debug \
	./cc/daemon \
	./cc/ipaddrs \
	./cc/coscheduler \
	./cc/cctstr \
	./cc/ffmpeg \
	./cc/ffgop \
	./cc/getifaddrs \
	./cc/sockopt \
	./cc/url-parser \
	./cc/http \
	./cc/rtsp \
	./cc/pg \
	./ffsrv \
	./ffsrv/ffobject \
	./ffsrv/ffobject/ffinput \
	./ffsrv/ffobject/ffoutput \
	./ffsrv/ffobject/ffdecoder \
	./ffsrv/ffobject/ffencoder \
	./ffsrv/ffobject/ffmixer \
	./ffsrv/ffobject/ffsink \
	./ffsrv/http \
	./ffsrv/ffcfg \
	./ffsrv/rtsp \
	./ffsrv/ffdb \
	./ffsrv/ffdb/textfile \
	./ffsrv/ffdb/pg \
	./ffsrv/ffdb/sqlite3 \


INCLUDES+=$(foreach s,$(SUBDIRS),$(addprefix -I,$(s))) -I/usr/include/postgresql
SOURCES = $(foreach s,$(SUBDIRS), $(wildcard $s/*.c)) 
HEADERS = $(foreach s,$(SUBDIRS), $(wildcard $s/*.h))
MODULES = $(foreach s,$(SOURCES),$(addsuffix .o,$(basename $(s))))


use_local_ffmpeg_libs = 1

# order is important
FFLIBS := avdevice avfilter avformat avcodec swscale swresample avresample postproc avutil 

ifeq ($(strip $(use_local_ffmpeg_libs)),1)
PKG_CONFIG = PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config
FFMPEG_LIBS := $(addprefix /usr/local/lib/lib,$(addsuffix .a,$(FFLIBS))) \
   $(filter-out $(addprefix -l,$(FFLIBS)),$(sort $(shell $(PKG_CONFIG) --static --libs $(addprefix lib,$(FFLIBS)))))
FFMPEG_CFLAGS := $(sort $(shell $(PKG_CONFIG) --static --cflags $(addprefix lib,$(FFLIBS))))
else
PKG_CONFIG = pkg-config
FFMPEG_LIBS := $(shell $(PKG_CONFIG) --libs  $(addprefix lib,$(FFLIBS)) )
FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags  $(addprefix lib,$(FFLIBS)) )
endif




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

LIBPQ=-lpq

LDLIBS +=  $(FFMPEG_LIBS) $(LIBPQ) -lpcl -lrt -lpthread


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
