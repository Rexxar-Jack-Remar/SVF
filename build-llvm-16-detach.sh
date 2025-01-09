set -e

jobs=28

SVF_TEMP_DIR="/root/SVF-temp"
MajorLLVMVer=16
LLVMVer=${MajorLLVMVer}.0.4
LLVMHome="/opt/llvm-${LLVMVer}.obj"

# check if unzip is missing (Z3)
function check_unzip {
    if ! type unzip &> /dev/null; then
        echo "Cannot find unzip. Please install unzip."
        exit 1
    fi
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
}

build_llvm_from_source
export LLVM_DIR="$LLVMHome"
