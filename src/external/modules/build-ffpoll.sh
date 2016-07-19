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

do_config=yes

if [[ "${do_config}" == "yes" ]]; then

./configure \
    --prefix=${prefix} \
      ${cross_args} \
    --target-os=linux \
    --disable-doc \
    --disable-ffserver \
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
    --extra-cflags="-I${destdir}/${prefix}/include " \
    --extra-cxxflags="-I${destdir}/${prefix}/include" \
    --extra-ldflags="-L${destdir}/${prefix}/lib -ldl" \
    --extra-ldexeflags="-L${destdir}/${prefix}/lib -ldl" \
    || exit 1
fi

make V=1 all install DESTDIR="${destdir}"

