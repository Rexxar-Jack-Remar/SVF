set -e

jobs=28

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SVFHOME="${SCRIPT_DIR}"

########
# Build SVF
########
if [[ $1 =~ ^[Dd]ebug$ ]]; then
    BUILD_TYPE='Debug'
else
    BUILD_TYPE='Release'
fi
BUILD_DIR="./${BUILD_TYPE}-build"

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