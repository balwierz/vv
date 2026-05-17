cmake_minimum_required(VERSION 3.16)
project(vv
    VERSION 1.7.0
    DESCRIPTION "Universal genomic file viewer"
    HOMEPAGE_URL "https://github.com/balwierz/vv"
    LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# `-march=x86-64` would force the baseline-x86_64 ABI — fine on x86 (it's the
# default for 64-bit gcc anyway) but fatal on aarch64 / arm64 where gcc
# rejects the value. Let gcc pick the host-appropriate default; the static
# release CI matrix runs natively on each architecture.
add_compile_options(-O2 -ffunction-sections -fdata-sections)

# ── Build mimalloc as a static lib from source ───────────────────────────────
include(FetchContent)
FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.9
    GIT_SHALLOW    TRUE
)
set(MI_BUILD_SHARED      OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT      OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL  OFF CACHE BOOL "" FORCE)
# Don't override malloc/free — Arrow uses mi_* prefixed API
set(MI_OVERRIDE          OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)

# ── Arrow / Parquet ───────────────────────────────────────────────────────────
# Prefer a locally-built static Arrow at ../arrow-install/ (the developer's
# local Arrow build); otherwise fall back to whatever Arrow is on the
# system search path (Ubuntu apt, Homebrew, etc.). The hint-based lookups
# are QUIET — they may legitimately not find anything (e.g. on CI), in
# which case the REQUIRED fallback below has to succeed.
set(_arrow_hint   "${CMAKE_CURRENT_SOURCE_DIR}/../arrow-install/lib/cmake/Arrow")
set(_parquet_hint "${CMAKE_CURRENT_SOURCE_DIR}/../arrow-install/lib/cmake/Parquet")
set(_compute_hint "${CMAKE_CURRENT_SOURCE_DIR}/../arrow-install/lib/cmake/ArrowCompute")
find_package(Arrow        QUIET PATHS "${_arrow_hint}"   NO_DEFAULT_PATH)
find_package(Parquet      QUIET PATHS "${_parquet_hint}" NO_DEFAULT_PATH)
find_package(ArrowCompute QUIET PATHS "${_compute_hint}" NO_DEFAULT_PATH)

if(NOT TARGET Arrow::arrow_static AND NOT TARGET Arrow::arrow_shared)
  find_package(Arrow        REQUIRED)
  find_package(Parquet      REQUIRED)
  find_package(ArrowCompute QUIET)
endif()

# Prefer Arrow's static targets only when the AlmaLinux 8 build's
# `../static-libs/` is present (which supplies every transitive .a we
# need — bz2, snappy, brotli, abseil, thrift, …). On plain Ubuntu / Mac
# the system / brew packages ship libarrow.a too, but lack the static
# transitive deps, so the link line would miss BZ2_*, ZSTD_*, etc.
set(_static_libs_dir "${CMAKE_CURRENT_SOURCE_DIR}/../static-libs/lib")
set(_prefer_static FALSE)
if(IS_DIRECTORY "${_static_libs_dir}")
  set(_prefer_static TRUE)
endif()

if(_prefer_static AND TARGET Arrow::arrow_static)
  set(ARROW_TARGET Arrow::arrow_static)
else()
  set(ARROW_TARGET Arrow::arrow_shared)
endif()

if(_prefer_static AND TARGET Parquet::parquet_static)
  set(PARQUET_TARGET Parquet::parquet_static)
else()
  set(PARQUET_TARGET Parquet::parquet_shared)
endif()

# Arrow Compute kernels — separate static archive when building from
# arrow-install. Needed for LociSSD region filter (equal / less / greater /
# filter); without it the runtime aborts with "No function registered".
if(_prefer_static AND TARGET ArrowCompute::arrow_compute_static)
  set(ARROW_COMPUTE_TARGET ArrowCompute::arrow_compute_static)
elseif(TARGET ArrowCompute::arrow_compute_shared)
  set(ARROW_COMPUTE_TARGET ArrowCompute::arrow_compute_shared)
else()
  set(ARROW_COMPUTE_TARGET "")
endif()

get_target_property(ARROW_INCLUDE_DIRS   ${ARROW_TARGET}   INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(PARQUET_INCLUDE_DIRS ${PARQUET_TARGET} INTERFACE_INCLUDE_DIRECTORIES)

# ── Strip shared-lib paths for libs we link statically ───────────────────────
# Arrow's imported targets inject explicit /usr/lib/libXXX.so paths into
# INTERFACE_LINK_LIBRARIES. Remove the ones we supply as static .a files so
# the linker doesn't add them to the binary's NEEDED list.
foreach(_t ${ARROW_TARGET} ${PARQUET_TARGET} Arrow::arrow_bundled_dependencies mimalloc-static)
  if(NOT TARGET ${_t})
    continue()
  endif()
  get_target_property(_iface ${_t} INTERFACE_LINK_LIBRARIES)
  if(_iface)
    list(FILTER _iface EXCLUDE REGEX ".*(zstd|[Ll][Zz]4|[Bb][Zz]ip2?|[Ll][Zz][Mm][Aa]|liblzma|libxz|deflate|[Bb]rotli|utf8proc|re2|thrift|[Ss]nappy|ZLIB|libz\\.|absl|[Nn]curses|tinfo|atomic).*")
    set_target_properties(${_t} PROPERTIES INTERFACE_LINK_LIBRARIES "${_iface}")
  endif()
endforeach()

# ── ncurses ───────────────────────────────────────────────────────────────────
find_package(Curses REQUIRED)

# ── htslib (BAM support) ─────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(HTSLIB REQUIRED htslib)

# ── libBigWig (bigBed / bigWig support, vendored) ────────────────────────────
# libBigWig includes <zlib.h> in its public header, so we need zlib's headers
# on the include path. Locally that's /usr/include; the static Docker build
# resolves it via CMAKE_PREFIX_PATH pointing at static-libs/.
find_package(ZLIB REQUIRED)
file(GLOB LIBBIGWIG_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/vendored/libBigWig/*.c")
add_library(bigwig STATIC ${LIBBIGWIG_SRCS})
# NOCURL: local files only — drops the optional libcurl dependency. Must
# be PUBLIC so the consumer of bigWig.h (main.cpp) sees it too — the
# header conditionally pulls in <curl/curl.h>.
target_compile_definitions(bigwig PUBLIC NOCURL)
target_compile_options(bigwig PRIVATE -O2 -ffunction-sections -fdata-sections -w)
target_include_directories(bigwig
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendored/libBigWig"
           ${ZLIB_INCLUDE_DIRS})

# ── SQLite (for .sqlite / .db / .sqlite3 source) ─────────────────────────────
# Local builds use the system libsqlite3 (every distro ships it). The
# AlmaLinux 8 static build compiles a libsqlite3.a from the upstream
# amalgamation and installs it under static-libs/lib — the explicit
# SQLITE_STATIC_LIB lookup below picks it up.
find_package(SQLite3 REQUIRED)

# ── Static runtime (libstdc++, libgcc) ───────────────────────────────────────
option(STATIC_RUNTIME "Link libstdc++ and libgcc statically" OFF)
if(STATIC_RUNTIME)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# ── Static htslib / ncurses when arrow is static ─────────────────────────────
# Only enable the static-link branch when there's a `../static-libs/` dir
# next to the source — that's the AlmaLinux 8 Docker build's vendored
# deps directory, which contains every transitive .a we need (htscodecs,
# abseil, thrift, …). On a plain Ubuntu CI box that dir is absent: even
# though /usr/lib/x86_64-linux-gnu/libhts.a is present, it requires
# libhtscodecs.a for CRAM symbols, and Ubuntu doesn't ship that as a
# static archive. Fall through to the shared-link else branch there.
# Look up *.a archives only — `NAMES hts` would also match libhts.so.
if(_prefer_static AND TARGET Arrow::arrow_static)
  pkg_check_modules(HTSLIB_STATIC QUIET htslib)
  find_library(HTSLIB_STATIC_LIB NAMES libhts.a)
  find_library(NCURSES_STATIC_LIB NAMES libncursesw.a libncurses.a libncursesw_g.a libncurses_g.a
               PATHS "${_static_libs_dir}")
  find_library(NCURSES_STATIC_TINFO NAMES libtinfow.a libtinfo.a
               PATHS "${_static_libs_dir}")
endif()

# ── Target ────────────────────────────────────────────────────────────────────
add_executable(vv main.cpp)

target_include_directories(vv PRIVATE
    ${ARROW_INCLUDE_DIRS}
    ${PARQUET_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${HTSLIB_INCLUDE_DIRS}
)

# Collect all static libs built into static-libs/
find_library(ZSTD_STATIC         NAMES libzstd.a         PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(LZ4_STATIC          NAMES liblz4.a          PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(BZ2_STATIC          NAMES libbz2.a          PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(LZMA_STATIC         NAMES liblzma.a         PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(DEFLATE_STATIC      NAMES libdeflate.a      PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(BROTLIENC_STATIC    NAMES libbrotlienc.a    PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(BROTLIDEC_STATIC    NAMES libbrotlidec.a    PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(BROTLICOMMON_STATIC NAMES libbrotlicommon.a PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(UTF8PROC_STATIC     NAMES libutf8proc.a     PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(RE2_STATIC          NAMES libre2.a          PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(THRIFT_STATIC       NAMES libthrift.a       PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(SNAPPY_STATIC       NAMES libsnappy.a       PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(ZLIB_STATIC         NAMES libz.a            PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)
find_library(SQLITE_STATIC       NAMES libsqlite3.a      PATHS "${_static_libs_dir}" NO_DEFAULT_PATH)

# Collect every libabsl_*.a from static-libs/
file(GLOB ABSL_STATIC_LIBS "${_static_libs_dir}/libabsl_*.a")

foreach(_lib ZSTD_STATIC LZ4_STATIC BZ2_STATIC LZMA_STATIC DEFLATE_STATIC
             BROTLIENC_STATIC BROTLIDEC_STATIC BROTLICOMMON_STATIC
             UTF8PROC_STATIC RE2_STATIC THRIFT_STATIC SNAPPY_STATIC ZLIB_STATIC)
  if(${_lib})
    message(STATUS "Static ${_lib}: ${${_lib}}")
  endif()
endforeach()
list(LENGTH ABSL_STATIC_LIBS _nabsl)
message(STATUS "Static absl libs found: ${_nabsl}")

find_library(ATOMIC_STATIC NAMES libatomic.a)

if(TARGET Arrow::arrow_static AND HTSLIB_STATIC_LIB AND NCURSES_STATIC_LIB)
  message(STATUS "Static linking: htslib=${HTSLIB_STATIC_LIB}  ncurses=${NCURSES_STATIC_LIB}")
  set(_tinfo_lib "")
  if(NCURSES_STATIC_TINFO)
    set(_tinfo_lib ${NCURSES_STATIC_TINFO})
  endif()

  set(_zstd         $<IF:$<BOOL:${ZSTD_STATIC}>,${ZSTD_STATIC},-lzstd>)
  set(_lz4          $<IF:$<BOOL:${LZ4_STATIC}>,${LZ4_STATIC},-llz4>)
  set(_bz2          $<IF:$<BOOL:${BZ2_STATIC}>,${BZ2_STATIC},-lbz2>)
  set(_lzma         $<IF:$<BOOL:${LZMA_STATIC}>,${LZMA_STATIC},-llzma>)
  set(_deflate      $<IF:$<BOOL:${DEFLATE_STATIC}>,${DEFLATE_STATIC},-ldeflate>)
  set(_brotlienc    $<IF:$<BOOL:${BROTLIENC_STATIC}>,${BROTLIENC_STATIC},-lbrotlienc>)
  set(_brotlidec    $<IF:$<BOOL:${BROTLIDEC_STATIC}>,${BROTLIDEC_STATIC},-lbrotlidec>)
  set(_brotlicommon $<IF:$<BOOL:${BROTLICOMMON_STATIC}>,${BROTLICOMMON_STATIC},-lbrotlicommon>)
  set(_utf8proc     $<IF:$<BOOL:${UTF8PROC_STATIC}>,${UTF8PROC_STATIC},-lutf8proc>)
  set(_re2          $<IF:$<BOOL:${RE2_STATIC}>,${RE2_STATIC},-lre2>)
  set(_thrift       $<IF:$<BOOL:${THRIFT_STATIC}>,${THRIFT_STATIC},-lthrift>)
  set(_snappy       $<IF:$<BOOL:${SNAPPY_STATIC}>,${SNAPPY_STATIC},-lsnappy>)
  set(_zlib         $<IF:$<BOOL:${ZLIB_STATIC}>,${ZLIB_STATIC},-lz>)
  set(_sqlite       $<IF:$<BOOL:${SQLITE_STATIC}>,${SQLITE_STATIC},-lsqlite3>)

  target_link_libraries(vv PRIVATE
      ${PARQUET_TARGET}
      # arrow_compute kernels live in a separate archive; wrap in
      # --whole-archive so the static-initialisers that register
      # equal/less/greater/filter actually run.
      $<$<BOOL:${ARROW_COMPUTE_TARGET}>:-Wl,--whole-archive>
      ${ARROW_COMPUTE_TARGET}
      $<$<BOOL:${ARROW_COMPUTE_TARGET}>:-Wl,--no-whole-archive>
      ${ARROW_TARGET}
      mimalloc-static
      ${NCURSES_STATIC_LIB}
      ${_tinfo_lib}
      ${HTSLIB_STATIC_LIB}
      bigwig
      ${_sqlite}
      ${_deflate} ${_zlib} ${_bz2} ${_lzma} ${_lz4} ${_zstd}
      ${_brotlienc} ${_brotlidec} ${_brotlicommon}
      ${_snappy} ${_utf8proc} ${_re2} ${_thrift}
      # absl: use --start-group/--end-group to handle circular refs
      -Wl,--start-group ${ABSL_STATIC_LIBS} -Wl,--end-group
      $<$<BOOL:${ATOMIC_STATIC}>:${ATOMIC_STATIC}>
      -lm -pthread
  )
else()
  # `--whole-archive` is GNU ld syntax; macOS ld64 uses `-force_load`.
  # For a shared Arrow build (else-branch on macOS / Linux) the compute
  # kernels' static initialisers are already pulled by the .dylib /
  # .so symbols, so we don't need either flag — Arrow's compute
  # registry runs at .so init time, not at vv link time.
  target_link_libraries(vv PRIVATE
      ${PARQUET_TARGET}
      ${ARROW_COMPUTE_TARGET}
      ${ARROW_TARGET}
      mimalloc-static
      ${CURSES_LIBRARIES}
      ${HTSLIB_LINK_LIBRARIES}
      bigwig
      SQLite::SQLite3
  )
endif()

# `--gc-sections` is GNU ld; macOS ld64 strips unused sections by default
# (via `-dead_strip`, which we don't need to pass explicitly).
if(NOT APPLE)
  target_link_options(vv PRIVATE -Wl,--gc-sections)
endif()
target_link_options(vv PRIVATE
    # Put static-libs dir first so -lzstd/-llz4/-lbz2/-llzma resolve to .a
    -L${_static_libs_dir}
)

# Strip in Release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_custom_command(TARGET vv POST_BUILD
      COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:vv>
      COMMENT "Stripping binary")
endif()

message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "Arrow target: ${ARROW_TARGET}")
message(STATUS "htslib      : ${HTSLIB_VERSION}")

# ── Install rules ─────────────────────────────────────────────────────────────
install(TARGETS vv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# `vh` is the same binary as `vv`; when invoked via this name it auto-enables
# vertical-head mode (transposed preview, non-interactive) so `vh wide.parquet`
# scrolls vertically instead of horizontally.
install(CODE "
    set(_vv \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/vv\")
    set(_vh \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/vh\")
    file(REMOVE \"\${_vh}\")
    execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink vv \"\${_vh}\")
")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/man/vv.1")
    install(FILES man/vv.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/completions/vv.bash")
    install(FILES completions/vv.bash
            DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
            RENAME vv)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/completions/vv.fish")
    install(FILES completions/vv.fish
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/completions/_vv")
    install(FILES completions/_vv
            DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)
endif()
