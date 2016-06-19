#! /bin/bash

moduledir=opencore-amr

if [[ "$1" == "clean" ]] ; then
    make -i -C "${moduledir}" clean distclean ;
    exit 0;
fi

# Setup prefix, sysroot and PATH to $target-$cc 
source ./setup-build-env.sh || exit 1

cd "${moduledir}" || exit 1

autoreconf -fi || exit 1
#aclocal && libtoolize && autoconf && automake -f --add-missing || exit 1

ac_cv_func_malloc_0_nonnull=yes \
   ./configure \
       --host="${target}" \
       --prefix="${prefix}" \
       --enable-static=yes \
       --enable-shared=no \
       --with-pic=no \
       --with-gnu-ld \
       --disable-dependency-tracking \
         CPP="${CPP}" \
         CC="${CC}" \
         CXX="${CXX}" \
            || exit 1

make V=1 all install DESTDIR="${destdir}" 
