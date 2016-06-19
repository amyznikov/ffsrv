#! /bin/bash


if [[ "$1" == "clean" ]] ; then
    make -i -C x264 clean distclean;
    exit 0;
fi

# Setup prefix, sysroot and PATH to $target-$cc 
source ./setup-build-env.sh || exit 1

cd x264 || exit 1

cross_args=""
[[ "${target}" != "" ]] && cross_args="--host=${target} --cross-prefix=${target}-" ;

./configure \
    --prefix="${prefix}" \
      ${cross_args} \
    --enable-static \
    --chroma-format=all \
    --enable-static \
    --disable-opencl \
    --disable-cli && \
          make V=1 all install DESTDIR="${destdir}"

