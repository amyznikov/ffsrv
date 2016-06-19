#! /bin/bash

moduledir=lame

# This will setup prefix, sysroot and PATH to $target-$cc
source ./setup-build-env.sh || exit 1

cd "${moduledir}" || exit 1

if [[ "$1" == "clean" ]] ; then
    make -i clean distclean;
    exit 0;
fi


autoreconf -fi || exit 1
# aclocal && libtoolize &&  autoconf && automake -f --add-missing || exit 1	

./configure \
        --prefix="${prefix}" \
	--host="${target}" \
        --enable-static=yes \
        --enable-shared=no \
        --disable-frontend \
        --disable-rpath \
    || exit 1

make V=1 all install DESTDIR="${destdir}"
