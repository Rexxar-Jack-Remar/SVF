#!/usr/bin/env bash
# type './build.sh'       for release build
# type './build.sh debug' for debug build
# if the LLVM_DIR variable is not set, LLVM will be downloaded.
#
# Dependencies include: build-essential libncurses5 libncurses-dev cmake zlib1g-dev
set -e # exit on first error

jobs=28

#########
# VARs and Links
########
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SVFHOME="${SCRIPT_DIR}"
SVF_TEMP_DIR="/root/SVF-temp"
sysOS=$(uname -s)
arch=$(uname -m)
MajorLLVMVer=16
LLVMVer=${MajorLLVMVer}.0.4
UbuntuArmLLVM="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVMVer}/clang+llvm-${LLVMVer}-aarch64-linux-gnu.tar.xz"
UbuntuLLVM="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVMVer}/clang+llvm-${LLVMVer}-x86_64-linux-gnu-ubuntu-22.04.tar.xz"
SourceLLVM="https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-${LLVMVer}.zip"
UbuntuZ3="https://github.com/Z3Prover/z3/releases/download/z3-4.8.8/z3-4.8.8-x64-ubuntu-16.04.zip"
UbuntuZ3Arm="https://github.com/SVF-tools/SVF-npm/raw/prebuilt-libs/z3-4.8.7-aarch64-ubuntu.zip"
SourceZ3="https://github.com/Z3Prover/z3/archive/refs/tags/z3-4.8.8.zip"

# Keep LLVM version suffix for version checking and better debugging
# keep the version consistent with LLVM_DIR in setup.sh and llvm_version in Dockerfile
LLVMHome="/opt/llvm-${LLVMVer}.obj"
Z3Home="/opt/z3.obj"

# check if unzip is missing (Z3)
function check_unzip {
    if ! type unzip &> /dev/null; then
        echo "Cannot find unzip. Please install unzip."
        exit 1
    fi
}

# check if xz is missing (LLVM)
function check_xz {
    if ! type xz &> /dev/null; then
        echo "Cannot find xz. Please install xz-utils."
        exit 1
    fi
}

function build_z3_from_source {
    mkdir "$Z3Home"
    check_unzip
    echo "Unzipping Z3 source..."
    mkdir "${SVF_TEMP_DIR}/z3-source"
    unzip "${SVF_TEMP_DIR}/z3.zip" -d "${SVF_TEMP_DIR}/z3-source"

    echo "Building Z3..."
    mkdir "${SVF_TEMP_DIR}/z3-build"
    cd "${SVF_TEMP_DIR}/z3-build"
    # /* is a dirty hack to get z3-version...
    cmake -DCMAKE_INSTALL_PREFIX="$Z3Home" -DZ3_BUILD_LIBZ3_SHARED=false -DPYTHON_EXECUTABLE=/opt/miniconda3/envs/py38/bin/python ../z3-source/*
    cmake --build . -j ${jobs}
    cmake --install .

    cd "${SVFHOME}"
}

function build_llvm_from_source {
    mkdir "$LLVMHome"
    check_unzip
    echo "Unzipping LLVM source..."
    mkdir "${SVF_TEMP_DIR}/llvm-16-source"
    unzip "${SVF_TEMP_DIR}/llvmorg-16.0.4.zip" -d "${SVF_TEMP_DIR}/llvm-16-source"

    echo "Building LLVM..."
    mkdir "${SVF_TEMP_DIR}/llvm-16-build"
    cd "${SVF_TEMP_DIR}/llvm-16-build"
    # /*/ is a dirty hack to get llvm-project-llvmorg-version...
    cmake -G Ninja -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DCMAKE_BUILD_TYPE=Release -DLLVM_BINUTILS_INCDIR=/opt/binutils/include -DCMAKE_INSTALL_PREFIX="$LLVMHome" -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld;lldb;compiler-rt;mlir;pstl;openmp;flang" ../llvm-16-source/*/llvm
    cmake --build . -j ${jobs}
    cmake --install .

    cd "${SVFHOME}"
}

function check_and_install_brew {
    if command -v brew >/dev/null 2>&1; then
        echo "Homebrew is already installed."
    else
        echo "Homebrew not found. Installing Homebrew..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        if [ $? -eq 0 ]; then
            echo "Homebrew installation completed."
        else
            echo "Homebrew installation failed."
            exit 1
        fi
    fi
}

# OS-specific values.
urlLLVM=""
urlZ3=""
OSDisplayName=""

########
# Set OS-specific values, mainly URLs to download binaries from.
# M1 Macs give back arm64, some Linuxes can give aarch64 for arm architecture
#######
if [[ $sysOS == "Darwin" ]]; then
    check_and_install_brew
    if [[ "$arch" == "arm64" ]]; then
        OSDisplayName="macOS arm64"
    else
        OSDisplayName="macOS x86"
    fi
elif [[ $sysOS == "Linux" ]]; then
    if [[ "$arch" == "aarch64" ]]; then
        urlLLVM="$UbuntuArmLLVM"
        urlZ3="$UbuntuZ3Arm"
        OSDisplayName="Ubuntu arm64"
    else
        urlLLVM="$UbuntuLLVM"
        urlZ3="$UbuntuZ3"
        OSDisplayName="Ubuntu x86"
    fi
else
    echo "Builds outside Ubuntu and macOS are not supported."
fi



build_llvm_from_source
export LLVM_DIR="$LLVMHome"
build_z3_from_source
export Z3_DIR="$Z3Home"

# Add LLVM & Z3 to $PATH and $LD_LIBRARY_PATH (prepend so that selected instances will be used first)
PATH=$LLVM_DIR/bin:$Z3_DIR/bin:$PATH
LD_LIBRARY_PATH=$LLVM_DIR/lib:$Z3_BIN/lib:$LD_LIBRARY_PATH

echo "LLVM_DIR=$LLVM_DIR"
echo "Z3_DIR=$Z3_DIR"

########
# Build SVF
########
if [[ $1 =~ ^[Dd]ebug$ ]]; then
    BUILD_TYPE='Debug'
else
    BUILD_TYPE='Release'
fi
BUILD_DIR="${SVFHOME}/${BUILD_TYPE}-build"

rm -rf "${BUILD_DIR}"
mkdir "${BUILD_DIR}"
# If you need shared libs, turn BUILD_SHARED_LIBS on
cmake -D CMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" \
    -DSVF_ENABLE_ASSERTIONS:BOOL=true            \
    -DSVF_SANITIZE="${SVF_SANITIZER}"            \
    -DBUILD_SHARED_LIBS=off                      \
    -S "${SVFHOME}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j ${jobs}

########
# Set up environment variables of SVF
########
source ${SVFHOME}/setup-16-detach.sh ${BUILD_TYPE}

#########
# Optionally, you can also specify a CXX_COMPILER and your $LLVM_HOME for your build
# cmake -DCMAKE_CXX_COMPILER=$LLVM_DIR/bin/clang++ -DLLVM_DIR=$LLVM_DIR
#########
