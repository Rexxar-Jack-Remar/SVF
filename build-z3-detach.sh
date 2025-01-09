set -e

jobs=28

SVF_TEMP_DIR="/root/SVF-temp"
Z3Home="/opt/z3.obj"

# check if unzip is missing (Z3)
function check_unzip {
    if ! type unzip &> /dev/null; then
        echo "Cannot find unzip. Please install unzip."
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
}

build_z3_from_source
export Z3_DIR="$Z3Home"