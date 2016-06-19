#! /bin/bash


if [[ "$1" == "clean" ]] ; then
    make -i -C openssl clean distclean;
    exit 0;
fi


# Setup prefix, sysroot and PATH to $target-$cc 
source ./setup-build-env.sh || exit 1

cd openssl || exit 1

if [[ "${target}" == "" ]] ; then       
    ./config --prefix="${prefix}" --install_prefix="${destdir}" no-shared || exit 1    
else
    T="linux-${target%%-*}"
   ./Configure --prefix="${prefix}" --cross-compile-prefix="${target}-" --install_prefix="${destdir}"  no-shared "${T}" || exit 1
fi   

make build_libcrypto build_libssl openssl.pc install_sw DIRS='crypto ssl engines'
