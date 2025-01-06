set -e

jobs=28

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SVFHOME="${SCRIPT_DIR}"
LLVMHome="llvm-16.0.4.obj"

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
    # echo "Unzipping LLVM source..."
    # mkdir llvm-16-source
    # unzip llvmorg-16.0.4.zip -d llvm-16-source

    echo "Building LLVM..."
    mkdir llvm-16-build
    cd llvm-16-build
    # /*/ is a dirty hack to get llvm-project-llvmorg-version...
    cmake -G Ninja -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DCMAKE_BUILD_TYPE=Release -DLLVM_BINUTILS_INCDIR=/opt/binutils/include -DCMAKE_INSTALL_PREFIX="$SVFHOME/$LLVMHome" -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld;lldb;compiler-rt;mlir;pstl;openmp;flang" ../llvm-16-source/*/llvm
    cmake --build . -j ${jobs}
    cmake --install .

    cd ..
}

build_llvm_from_source
