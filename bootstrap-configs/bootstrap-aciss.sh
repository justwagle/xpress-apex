#!/bin/bash -e

pwddir=`pwd`
basedir=/tmp
cd $basedir

###################################################################
#configure parameters - set what ever you need in this top section!
###################################################################

# REQUIRED libraries

export BOOST_ROOT=$BOOST

# OPTIONAL libraries - if left undefined, they likely won't be used.

export BFD_ROOT=$HOME/install/binutils-2.23.2
export GPERFTOOLS_ROOT=$HOME/install/gperftools/2.4
#export RCR_ROOT=$HOME/src/RCRdaemon
#export PAPI_ROOT=/usr/local/papi/5.3.2
#export TAU_ROOT=$HOME/src/tau2

# other CMake variables - for special situations / architectures / compilers.

cmake_build_type="-DCMAKE_BUILD_TYPE=Debug" # Debug, Release, RelWithDebInfo, etc.
cmake_apex_throttle="-DAPEX_THROTTLE=FALSE" # TRUE or FALSE
cmake_build_shared_libs="-DBUILD_SHARED_LIBS=TRUE" # TRUE or FALSE
cmake_install_prefix="-DCMAKE_INSTALL_PREFIX=../install" # the installation path
cmake_use_codeblocks="-G \"CodeBlocks - Unix Makefiles\"" # if you want to debug in CodeBlocks
cmake_make_verbose=""  # for verbose, use -DCMAKE_VERBOSE_MAKEFILE=ON
cmake_use_mpi="-DUSE_MPI=FALSE" # TRUE or FALSE



# runtime parameters for HPX-3 (LSU)
export APEX_POLICY=1
export APEX_CONCURRENCY=0
export APEX_TAU=0
# this one is only meaningful for HPX-3 from LSU
# export HPX_HAVE_ITTNOTIFY=1

# NO NEED TO MODIFY ANYTHING BELOW THIS LINE
# ------------------------------------------------------------------------

# Get time as a UNIX timestamp (seconds elapsed since Jan 1, 1970 0:00 UTC)
T="$(date +%s)"

if [ $# -eq 1 ] ; then
	if [ $1 == "--clean" ] || [ $1 == "-c" ] ; then
		rm -rf build_*
	fi
fi

datestamp=`date +%Y.%m.%d-%H.%M.%S`
dir="build_$datestamp"
mkdir $dir
cd $dir

# Enable shared libraries, if desired.
#-DTAU_ROOT=$TAU_ROOT \
#-DUSE_BINUTILS=FALSE \

cmake \
-G "CodeBlocks - Unix Makefiles" \
-DBOOST_ROOT=$BOOST_ROOT \
-DBFD_ROOT=$BFD_ROOT \
-DGPERFTOOLS_ROOT=$GPERFTOOLS_ROOT \
-DUSE_BINUTILS=TRUE \
-DCMAKE_BUILD_TYPE=Debug \
-DAPEX_THROTTLE=FALSE \
-DBUILD_SHARED_LIBS=TRUE \
-DUSE_PAPI=FALSE \
-DCMAKE_INSTALL_PREFIX=$pwddir/install \
-DCMAKE_VERBOSE_MAKEFILE=ON \
$pwddir

procs=1
if [ -f '/proc/cpuinfo' ] ; then
  procs=`grep -c ^processor /proc/cpuinfo`
fi
make -j `expr $procs + 1`

make test
make install

printf "\nSUCCESS!\n"
T="$(($(date +%s)-T))"
printf "Time to configure and build APEX: %02d hours %02d minutes %02d seconds.\n" "$((T/3600))" "$((T/60%60))" "$((T%60))"