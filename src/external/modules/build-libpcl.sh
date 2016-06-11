#! /bin/bash


if [[ "$1" == "clean" ]] ; then
    make -i -C libpcl clean distclean ;
    exit 0;
fi

# Setup prefix, sysroot and PATH to $target-$cc
source ./setup-build-env.sh || exit 1


cd libpcl || exit 1

aclocal && libtoolize &&  autoconf && automake -f --add-missing || exit 1

./configure \
    --prefix="${prefix}" \
    --host="${target}" \
    --enable-shared=no \
    --enable-static=yes \
    || exit 1

make V=1 && make V=1 install || exit 1
