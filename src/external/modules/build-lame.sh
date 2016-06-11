#! /bin/bash


if [[ "$1" == "clean" ]] ; then
    make -i -C lame clean distclean;
    exit 0;
fi

# This will setup prefix, sysroot and PATH to $target-$cc
source ./setup-build-env.sh || exit 1


cd lame || exit 1

aclocal && libtoolize &&  autoconf && automake -f --add-missing || exit 1	

./configure \
        --prefix="${prefix}" \
	--host="${target}" \
        --enable-static=yes \
        --enable-shared=no \
        --disable-frontend \
        --disable-rpath && \
    make V=1 all install
