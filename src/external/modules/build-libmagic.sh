#! /bin/bash

moduledir=libmagic

# Setup prefix, sysroot and PATH to $target-$cc
source ./setup-build-env.sh || exit 1

cd "${moduledir}" || exit 1

if [[ "$1" == "clean" ]] ; then
    make -i clean distclean ;
    exit 0;
fi

if [[ "$1" == "uninstall" ]] ; then
    make -i V=1 uninstall DESTDIR="${destdir}" ;
    exit 0;
fi


autoreconf -f -i || exit 1

./configure \
    --prefix="${prefix}" \
    --host="${target}" \
    --enable-shared=no \
    --enable-static=yes \
    --disable-silent-rules \
    --disable-dependency-tracking \
    --enable-fast-install=yes \
    --with-pic=no \
    || exit 1

make V=1 && make V=1 install DESTDIR="${destdir}" || exit 1
