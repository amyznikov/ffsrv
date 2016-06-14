#! /bin/bash


if [[ "$1" == "clean" ]] ; then
    make -i -C ffpoll clean distclean;
    exit 0;
fi    


# Setup prefix, sysroot and PATH to $target-$cc
source ./setup-build-env.sh || exit 1

arch="${target%%-*}"

cd ffpoll || exit 1

cross_args=""
[[ "${arch}" != "" ]] && cross_args="--enable-cross-compile --arch=${arch}"

./configure \
    --prefix=${prefix} \
      ${cross_args} \
    --target-os=linux \
    --disable-doc \
    --disable-ffplay \
    --disable-ffserver \
    --disable-ffmpeg \
    --disable-ffprobe \
    --enable-avresample \
    --enable-libx264 \
    --enable-libmp3lame  \
    --enable-libopencore-amrnb \
    --enable-libopencore-amrwb \
    --enable-openssl \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --ar="${AR}" \
    --as="${CC}" \
    --strip="${STRIP}" \
    --extra-cflags="-I${prefix}/include " \
    --extra-cxxflags="-I${prefix}/include" \
    --extra-ldflags="-L${prefix}/lib -ldl" \
    --extra-ldexeflags="-L${prefix}/lib -ldl" \
    || exit 1

make V=1 all install

