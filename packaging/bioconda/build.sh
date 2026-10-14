#!/usr/bin/env bash
set -euxo pipefail

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# xlsxio: a static, position-independent library in a scratch prefix
# (packaging/rpm/vv.spec builds it the same way). It declares an old CMake
# minimum, hence CMAKE_POLICY_VERSION_MINIMUM.
cmake ${CMAKE_ARGS:-} -G Ninja -S xlsxio -B xlsxio-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="${SRC_DIR}/xlsxio-local" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
    -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCUMENTATION=OFF \
    -DWITH_LIBZIP=OFF -DWITH_MINIZIP=ON
cmake --build xlsxio-build -j"${CPU_COUNT}"
cmake --install xlsxio-build
XLSXIO_A=$(echo "${SRC_DIR}"/xlsxio-local/lib*/libxlsxio_read.a)

# glibc 2.17 keeps pthread_create in libpthread; vv 1.31.0's CMakeLists does
# not link Threads::Threads in the shared-deps build (later releases do).
export LDFLAGS="${LDFLAGS:-} -pthread"

cmake ${CMAKE_ARGS:-} -G Ninja -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DVV_PREFER_STATIC=OFF \
    -DVV_WITH_DUCKDB=OFF \
    -DVV_BUILD_GUI=OFF \
    -DXLSXIO_STATIC:FILEPATH="${XLSXIO_A}" \
    -DXLSXIO_INCLUDE_DIR:PATH="${SRC_DIR}/xlsxio-local/include"
cmake --build build -j"${CPU_COUNT}"
cmake --install build
test -L "${PREFIX}/bin/vh"
