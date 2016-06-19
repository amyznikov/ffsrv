#! /bin/bash

mydir=$(pwd)
modules="libpcl libmagic x264 lame openssl opencore-amr ffpoll"

# target='aarch64-rpi3-linux-gnueabi'

target=$1

case "${target}" in
    
    clean)
	for m in ${modules} ; do
	    make -i -C $m clean distclean
	done
	;;
    
    *)
	for m in ${modules} ; do
	    echo -e "BUILD ${m}...\n"
	    ./build-${m}.sh "${target}" || exit 1
	    echo -e "DONE ${m}\n\n\n"    
	done
	;;
esac
