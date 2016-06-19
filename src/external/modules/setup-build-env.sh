# Setup prefix, sysroot, PATH, etc to (cross-)compiller

mydir="$(pwd)"

target=""
prefix=""
destdir=""
cc=gcc
cxx=g++


function errmsg() {
    echo -e "\033[1m\033[31m$1\033[0m" 1>&2
}


for arg in "$@"
do
    case $arg in
	DESTDIR=*|destdir=*)
	    destdir="${arg#*=}"
	    shift
	    ;;
	
	prefix=*|--prefix=*)
	    prefix="${arg#*=}"
	    shift
	    ;;
	
	*)
	if [[ "${target}" != "" ]] ; then 	  
        	errmsg "Invalid target $arg specified"
		exit 1;
	fi
	target="${arg}"
	    ;;
    esac
done




[[ "${destdir}" == "" ]] && destdir="$(dirname $(pwd))/${target}/sysroot"
[[ "${prefix}"  == "" ]] && prefix="/usr"


if [[ "${target}" == "" ]]; then
    CC="${cc}"
    CXX="${cxx}"
else
    cross_cc=$(which "${target}-${cc}")
    cross_cxx=$(which "${target}-${cxx}")
    if [[ "$cross_cc" == "" ]] ; then
	# try x-tools
	test="${HOME}/x-tools/${target}/bin/${target}-${cc}"
	if [[ -f "${test}" ]] ; then
	    export PATH=$(dirname "${cross_cc}"):${PATH}
            cross_cc=$(which "${target}-${cc}")
            cross_cxx=$(which "${target}-${cxx}")
	fi
    fi

    if [[ "${cross_cc}" == "" ]] ; then
	errmsg "Can not locate ${target}-${cc}. Add it to your PATH and try again";
	exit 1;
    fi

    CC="${cross_cc}"
    CXX="${cross_cxx}"
fi

mkdir -p "${destdir}" || exit 1
