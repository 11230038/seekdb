cmake_minimum_required(VERSION 3.20)

# CMake is the compatibility production/pretest builder while Bazel remains
# the authoritative modular build graph. Keep this surface deliberately
# narrow: one Release-compatible configuration and Unity compilation only.
include(cmake/Utils.cmake)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  include(cmake/IOS.cmake)
endif()
include(cmake/Env.cmake)

project("OceanBase"
  VERSION 1.4.0.0
  DESCRIPTION "OceanBase seekdb database system"
  HOMEPAGE_URL "https://www.oceanbase.ai"
  LANGUAGES CXX C ASM)

if(OB_MACOS27)
  include(CheckCXXSourceCompiles)
  # An empty main misses LLVM/SDK libc++ incompatibilities.
  unset(OB_MACOS27_CXX_WORKS CACHE)
  check_cxx_source_compiles("#include <vector>
    #include <iostream>
    int main() { std::vector<int> v{27}; std::cout << v.at(0); }"
    OB_MACOS27_CXX_WORKS)
  if(NOT OB_MACOS27_CXX_WORKS)
    message(FATAL_ERROR "The selected LLVM cannot compile/link against the macOS 27 C++ library. "
      "Use matching Xcode/Command Line Tools or a newer Homebrew LLVM via OB_MACOS_LLVM_ROOT.")
  endif()
endif()

option(OB_ENABLE_STANDBY "Build physical standby support" ON)
option(ENABLE_SANITY "Build with seekdb memory Sanity instrumentation" OFF)
# AUTO compiles the GPU backend whenever a hipVS/cuVS package is discoverable,
# so a machine with an AMD Radeon GPU needs no extra build flag. A missing
# hipVS install leaves the build identical to the CPU-only configuration.
set(OB_ENABLE_HIPVS "AUTO" CACHE STRING
  "Build the AMD hipVS (GPU) vector index backend: AUTO, ON, or OFF")
set_property(CACHE OB_ENABLE_HIPVS PROPERTY STRINGS AUTO ON OFF)
set(HIPVS_ROOT "/opt/hipvs" CACHE PATH "hipVS installation prefix")
# hipVS installs a cuvs package config; make sure a non-system prefix is
# searched even when the caller only passes -DHIPVS_ROOT=...
if(HIPVS_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${HIPVS_ROOT}")
endif()
# hipVS is commonly installed next to ROCm or under /usr/local. Probe those
# prefixes as well so AUTO does not silently miss an existing installation.
foreach(hipvs_hint IN ITEMS "$ENV{HIPVS_ROOT}" "$ENV{ROCM_PATH}" "/opt/rocm" "/usr/local")
  if(hipvs_hint AND EXISTS "${hipvs_hint}")
    list(APPEND CMAKE_PREFIX_PATH "${hipvs_hint}")
  endif()
endforeach()
string(TOUPPER "${OB_ENABLE_HIPVS}" OB_HIPVS_MODE)
if(OB_HIPVS_MODE STREQUAL "AUTO")
  find_package(cuvs CONFIG QUIET)
  if(cuvs_FOUND)
    set(OB_HIPVS_ENABLED ON)
  else()
    set(OB_HIPVS_ENABLED OFF)
  endif()
elseif(OB_ENABLE_HIPVS)
  set(OB_HIPVS_ENABLED ON)
else()
  set(OB_HIPVS_ENABLED OFF)
endif()
if(OB_HIPVS_ENABLED AND CMAKE_VERSION VERSION_LESS 3.21)
  # The HIP language was added in CMake 3.21. The project still accepts 3.20 for
  # CPU-only builds, so probe for the GPU backend only when the toolchain is new
  # enough to drive hipcc instead of failing the configuration.
  message(WARNING "CMake ${CMAKE_VERSION} cannot build the hipVS bridge "
    "(HIP language requires CMake 3.21+); building without the GPU vector index backend")
  set(OB_HIPVS_ENABLED OFF)
endif()
if(OB_HIPVS_ENABLED)
  # The bridge instantiates cuVS/hipVS templates that emit device code, so the
  # translation unit has to be compiled by the HIP toolchain (hipcc). Without a
  # usable HIP compiler the headers cannot be built by the host C++ compiler, so
  # disable the backend instead of producing a broken build.
  # Point HIP discovery at a common ROCm install when ROCm is not in PATH.
  if(NOT CMAKE_HIP_COMPILER)
    foreach(hip_compiler_hint IN ITEMS
        "$ENV{ROCM_PATH}/bin/amdclang++" "/opt/rocm/bin/amdclang++")
      if(hip_compiler_hint AND EXISTS "${hip_compiler_hint}")
        set(CMAKE_HIP_COMPILER "${hip_compiler_hint}" CACHE FILEPATH
          "HIP compiler used to build the hipVS bridge")
        break()
      endif()
    endforeach()
  endif()
  # HIP code must target the GPUs of the machine that will run it; a compiler
  # default easily produces a binary that cannot launch on the installed card.
  # Ask ROCm's own enumerator, which also works when /opt/rocm/bin is not on
  # PATH. A cross build can always pre-set CMAKE_HIP_ARCHITECTURES explicitly.
  if(NOT CMAKE_HIP_ARCHITECTURES)
    find_program(ROCM_AGENT_ENUMERATOR rocm_agent_enumerator
      HINTS "${HIPVS_ROOT}/bin" "$ENV{ROCM_PATH}/bin" "/opt/rocm/bin")
    if(ROCM_AGENT_ENUMERATOR)
      execute_process(COMMAND "${ROCM_AGENT_ENUMERATOR}"
        OUTPUT_VARIABLE hipvs_agents OUTPUT_STRIP_TRAILING_WHITESPACE)
      string(REPLACE "\n" ";" hipvs_agents "${hipvs_agents}")
      set(hipvs_archs "")
      foreach(hipvs_agent IN LISTS hipvs_agents)
        string(STRIP "${hipvs_agent}" hipvs_agent)
        if(hipvs_agent MATCHES "^gfx[0-9a-z]+$")
          list(APPEND hipvs_archs "${hipvs_agent}")
        endif()
      endforeach()
      if(hipvs_archs)
        list(REMOVE_DUPLICATES hipvs_archs)
        string(REPLACE ";" " " hipvs_archs "${hipvs_archs}")
        set(CMAKE_HIP_ARCHITECTURES "${hipvs_archs}" CACHE STRING
          "GPU architectures the hipVS bridge is built for")
        message(STATUS "hipVS GPU architectures: ${CMAKE_HIP_ARCHITECTURES}")
      endif()
    endif()
  endif()
  include(CheckLanguage)
  check_language(HIP)
  if(CMAKE_HIP_COMPILER)
    enable_language(HIP)
    set(OB_HIPVS_HIP_TOOLCHAIN ON)
  else()
    message(WARNING "hipVS was found but no HIP compiler is available; "
      "building without the GPU vector index backend")
    set(OB_HIPVS_ENABLED OFF)
    set(OB_HIPVS_HIP_TOOLCHAIN OFF)
  endif()
else()
  set(OB_HIPVS_HIP_TOOLCHAIN OFF)
endif()
message(STATUS "hipVS GPU vector index backend: ${OB_HIPVS_ENABLED}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SEEKDB_GENERATED_ROOT "${CMAKE_BINARY_DIR}/generated")
set(SEEKDB_INNER_TABLE_GENERATOR
  "${CMAKE_SOURCE_DIR}/src/share/inner_table/generate_inner_table_schema.py")
set(SEEKDB_INNER_TABLE_DEF
  "${CMAKE_SOURCE_DIR}/src/share/inner_table/ob_inner_table_schema_def.py")
set(SEEKDB_INNER_TABLE_INIT_DATA
  "${CMAKE_SOURCE_DIR}/src/share/inner_table/ob_inner_table_init_data.py")
set(SEEKDB_INNER_TABLE_INPUTS
  "${SEEKDB_INNER_TABLE_GENERATOR}"
  "${SEEKDB_INNER_TABLE_DEF}"
  "${SEEKDB_INNER_TABLE_INIT_DATA}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${SEEKDB_INNER_TABLE_INPUTS})

set(SEEKDB_INNER_TABLE_INPUT_DIGEST "")
foreach(SEEKDB_INNER_TABLE_INPUT IN LISTS SEEKDB_INNER_TABLE_INPUTS)
  file(SHA256 "${SEEKDB_INNER_TABLE_INPUT}" SEEKDB_INNER_TABLE_INPUT_SHA256)
  string(APPEND SEEKDB_INNER_TABLE_INPUT_DIGEST
    "${SEEKDB_INNER_TABLE_INPUT}:${SEEKDB_INNER_TABLE_INPUT_SHA256}\n")
endforeach()
string(SHA256 SEEKDB_INNER_TABLE_DIGEST "${SEEKDB_INNER_TABLE_INPUT_DIGEST}")
set(SEEKDB_INNER_TABLE_STAMP "${SEEKDB_GENERATED_ROOT}/.inner_table_schema.sha256")
set(SEEKDB_INNER_TABLE_CURRENT_DIGEST "")
if(EXISTS "${SEEKDB_INNER_TABLE_STAMP}")
  file(READ "${SEEKDB_INNER_TABLE_STAMP}" SEEKDB_INNER_TABLE_CURRENT_DIGEST)
  string(STRIP "${SEEKDB_INNER_TABLE_CURRENT_DIGEST}"
    SEEKDB_INNER_TABLE_CURRENT_DIGEST)
endif()
if(NOT "${SEEKDB_INNER_TABLE_CURRENT_DIGEST}" STREQUAL "${SEEKDB_INNER_TABLE_DIGEST}" OR
   NOT EXISTS "${SEEKDB_GENERATED_ROOT}/share/inner_table/ob_inner_table_schema.h" OR
   NOT EXISTS "${SEEKDB_GENERATED_ROOT}/share/inner_table/local_table_schema.h" OR
   NOT EXISTS "${SEEKDB_GENERATED_ROOT}/observer/virtual_table/ob_all_virtual_local_tables.cpp")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${SEEKDB_INNER_TABLE_GENERATOR}"
      --def-file "${SEEKDB_INNER_TABLE_DEF}"
      --share-output-dir "${SEEKDB_GENERATED_ROOT}/share/inner_table"
      --observer-output-dir "${SEEKDB_GENERATED_ROOT}/observer/virtual_table"
      --quiet
    RESULT_VARIABLE SEEKDB_INNER_TABLE_GENERATOR_RESULT)
  if(NOT SEEKDB_INNER_TABLE_GENERATOR_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate inner-table schema sources")
  endif()
  file(WRITE "${SEEKDB_INNER_TABLE_STAMP}" "${SEEKDB_INNER_TABLE_DIGEST}\n")
endif()

set(SEEKDB_CMAKE_INVENTORY "${CMAKE_BINARY_DIR}/seekdb_bazel_source_inventory.cmake")
set(SEEKDB_CMAKE_INVENTORY_INPUTS
  "${CMAKE_SOURCE_DIR}/src/oblib/oblib_source_inventory.bzl"
  "${CMAKE_SOURCE_DIR}/src/data_plane/data_plane_build_defs.bzl"
  "${CMAKE_SOURCE_DIR}/src/query/query_build_defs.bzl"
  "${CMAKE_SOURCE_DIR}/src/share/share_source_inventory.bzl"
  "${CMAKE_SOURCE_DIR}/src/sql/sql_source_inventory.bzl"
  "${CMAKE_SOURCE_DIR}/src/storage/storage_source_inventory.bzl"
  "${CMAKE_SOURCE_DIR}/src/storage/storage_build_defs.bzl"
  "${CMAKE_SOURCE_DIR}/src/pl/pl_source_inventory.bzl"
  "${CMAKE_SOURCE_DIR}/src/rootserver/rootserver_source_inventory.bzl"
  "${CMAKE_SOURCE_DIR}/src/logservice/logservice_build_defs.bzl"
  "${CMAKE_SOURCE_DIR}/src/observer/observer_source_inventory.bzl")
foreach(SEEKDB_PRETEST_MODULE
    oblib share storage sql rootserver observer logservice pl query data_plane)
  set(SEEKDB_PRETEST_BUILD_FILE
    "${CMAKE_SOURCE_DIR}/unittest/${SEEKDB_PRETEST_MODULE}/BUILD.bazel")
  if(EXISTS "${SEEKDB_PRETEST_BUILD_FILE}")
    list(APPEND SEEKDB_CMAKE_INVENTORY_INPUTS
      "${SEEKDB_PRETEST_BUILD_FILE}")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tools/cmake/emit_bazel_source_inventory.py"
  ${SEEKDB_CMAKE_INVENTORY_INPUTS})
execute_process(
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/cmake/emit_bazel_source_inventory.py"
    --repo "${CMAKE_SOURCE_DIR}"
    --output "${SEEKDB_CMAKE_INVENTORY}"
  RESULT_VARIABLE SEEKDB_CMAKE_INVENTORY_RESULT)
if(NOT SEEKDB_CMAKE_INVENTORY_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to translate Bazel production source inventories")
endif()
include("${SEEKDB_CMAKE_INVENTORY}")

if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" AND
   NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_BUILD_TYPE STREQUAL "Debug"))
  message(FATAL_ERROR
    "The compatibility CMake build requires RelWithDebInfo; "
    "iOS also supports Debug")
endif()
if(NOT OB_ENABLE_UNITY)
  message(FATAL_ERROR "The compatibility CMake build supports only Unity compilation")
endif()
set(SEEKDB_CMAKE_PACKAGE_BUILD OFF)
set(SEEKDB_CMAKE_PACKAGE_FORMAT "")
set(SEEKDB_CMAKE_PACKAGE_FORMAT_COUNT 0)
foreach(SEEKDB_CMAKE_FORMAT RPM DEB TGZ WIX)
  if(CMAKE_BUILD_${SEEKDB_CMAKE_FORMAT})
    message(DEPRECATION
      "CMAKE_BUILD_${SEEKDB_CMAKE_FORMAT} is deprecated; "
      "use OB_BUILD_${SEEKDB_CMAKE_FORMAT} instead")
    set(OB_BUILD_${SEEKDB_CMAKE_FORMAT} ON)
  endif()
endforeach()
foreach(SEEKDB_CMAKE_FORMAT RPM DEB TGZ WIX)
  if(OB_BUILD_${SEEKDB_CMAKE_FORMAT})
    math(EXPR SEEKDB_CMAKE_PACKAGE_FORMAT_COUNT
      "${SEEKDB_CMAKE_PACKAGE_FORMAT_COUNT} + 1")
    set(SEEKDB_CMAKE_PACKAGE_FORMAT "${SEEKDB_CMAKE_FORMAT}")
  endif()
endforeach()
if(SEEKDB_CMAKE_PACKAGE_FORMAT_COUNT GREATER 1)
  message(FATAL_ERROR "Select only one package format: RPM, DEB, TGZ, or WIX")
elseif(SEEKDB_CMAKE_PACKAGE_FORMAT_COUNT EQUAL 1)
  if(NOT OB_BUILD_PACKAGE)
    message(FATAL_ERROR
      "OB_BUILD_${SEEKDB_CMAKE_PACKAGE_FORMAT} requires OB_BUILD_PACKAGE=ON")
  endif()
  if((SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "RPM" OR
      SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "DEB") AND
     NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "The CMake ${SEEKDB_CMAKE_PACKAGE_FORMAT} compatibility build supports Linux only")
  endif()
  if(SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "WIX" AND NOT WIN32)
    message(FATAL_ERROR "The CMake WIX compatibility build supports Windows only")
  endif()
  if(ANDROID)
    message(FATAL_ERROR "CMake packaging is not supported for Android")
  endif()
  set(SEEKDB_CMAKE_PACKAGE_BUILD ON)
endif()

if(OB_ERRSIM OR WITH_COVERAGE OR OB_BUILD_CCLS)
  message(FATAL_ERROR
    "The compatibility CMake build supports only Release Unity profiles")
endif()
if(ENABLE_SANITY AND SEEKDB_CMAKE_PACKAGE_BUILD)
  message(FATAL_ERROR "Sanity instrumentation cannot be combined with packaging")
endif()
if((ENABLE_AUTO_FDO OR ENABLE_THIN_LTO OR ENABLE_HOTFUNC OR
    OB_STATIC_LINK_LGPL_DEPS) AND NOT SEEKDB_CMAKE_PACKAGE_BUILD)
  message(FATAL_ERROR
    "Package optimization options require a CMake packaging profile")
endif()
if(OB_BUILD_PACKAGE AND NOT SEEKDB_CMAKE_PACKAGE_BUILD)
  message(FATAL_ERROR
    "OB_BUILD_PACKAGE requires one package format: RPM, DEB, TGZ, or WIX")
endif()

string(TOLOWER "${ARCHITECTURE}" SEEKDB_HOST_ARCHITECTURE)
if(WIN32)
  if(NOT SEEKDB_HOST_ARCHITECTURE MATCHES "^(amd64|x86_64)$")
    message(FATAL_ERROR "The CMake compatibility build supports Windows x64 only")
  endif()
elseif(ANDROID)
  if(NOT SEEKDB_HOST_ARCHITECTURE MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "The CMake compatibility build supports Android arm64-v8a only")
  endif()
elseif(APPLE)
  if(NOT SEEKDB_HOST_ARCHITECTURE MATCHES "^(x86_64|aarch64|arm64)$")
    message(FATAL_ERROR "The CMake compatibility build supports macOS x86_64/arm64 only")
  endif()
elseif(UNIX)
  if(NOT SEEKDB_HOST_ARCHITECTURE MATCHES "^(x86_64|aarch64|arm64)$")
    message(FATAL_ERROR "The CMake compatibility build supports Linux x86_64/aarch64 only")
  endif()
else()
  message(FATAL_ERROR "Unsupported CMake compatibility platform")
endif()

if(WIN32)
  find_program(LLVM_LIB_PROGRAM llvm-lib)
  if(LLVM_LIB_PROGRAM)
    set(CMAKE_AR "${LLVM_LIB_PROGRAM}" CACHE FILEPATH "Archiver" FORCE)
  endif()
  add_compile_definitions(strncasecmp=_strnicmp strcasecmp=_stricmp)
endif()

if(ANDROID)
  add_definitions(-D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION)
  add_compile_options(-include ${CMAKE_SOURCE_DIR}/include/android_compat.h)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(STDC_SHARED_LIB "")
set(STDC_SHARED_LIB_SYMLINK "")

if(APPLE)
  set(CMAKE_NO_SYSTEM_FROM_IMPORTED 1)
  if(POLICY CMP0152)
    cmake_policy(SET CMP0152 NEW)
  endif()
  set(STDC_SHARED_LIB_SYMLINK "${DEVTOOLS_DIR}/lib64/libstdc++.dylib")
elseif(UNIX)
  set(STDC_SHARED_LIB_SYMLINK "${DEVTOOLS_DIR}/lib64/libstdc++.so")
endif()

add_subdirectory(deps/external)
if(TARGET seekdb_jemalloc)
  set(OB_HAVE_BUNDLED_JEMALLOC TRUE)
endif()

# ENABLE_SANITY is a user request at configure time, but the source-level
# definition is an effective capability: once defined it implies every
# prerequisite below.  Keep these checks ahead of add_compile_definitions so
# Sanity-specific C++ code only needs to test ENABLE_SANITY.
set(SEEKDB_SANITY_NO_BUILTIN_COMPILE_OPTIONS "")
set(SEEKDB_SANITY_WRAP_LINK_OPTIONS "")
if(ENABLE_SANITY)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Sanity instrumentation is supported only on Linux")
  elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Sanity instrumentation requires Clang")
  elseif(NOT OB_HAVE_BUNDLED_JEMALLOC)
    message(FATAL_ERROR "Sanity instrumentation requires bundled jemalloc")
  endif()

  set(SEEKDB_SANITY_PASS "${DEVTOOLS_DIR}/lib64/libsanitypass.so")
  set(SEEKDB_SANITY_HEADER "${DEVTOOLS_DIR}/include/sanity/sanity.h")
  foreach(SEEKDB_SANITY_REQUIRED_FILE
      "${SEEKDB_SANITY_PASS}"
      "${SEEKDB_SANITY_HEADER}")
    if(NOT EXISTS "${SEEKDB_SANITY_REQUIRED_FILE}")
      message(FATAL_ERROR
        "Required Sanity component not found: ${SEEKDB_SANITY_REQUIRED_FILE}")
    endif()
  endforeach()

  add_compile_definitions(ENABLE_SANITY=1)
  set(SEEKDB_SANITY_NO_BUILTIN_COMPILE_OPTIONS
    -fno-builtin-memset
    -fno-builtin-bzero
    -fno-builtin-memcpy
    -fno-builtin-memmove
    -fno-builtin-memcmp
    -fno-builtin-strlen
    -fno-builtin-strnlen
    -fno-builtin-strcpy
    -fno-builtin-strncpy
    -fno-builtin-strcmp
    -fno-builtin-strncmp
    -fno-builtin-strcasecmp
    -fno-builtin-strncasecmp
    -fno-builtin-vsprintf
    -fno-builtin-vsnprintf
    -fno-builtin-sprintf
    -fno-builtin-snprintf)
  set(SEEKDB_SANITY_WRAP_LINK_OPTIONS
    -Wl,--wrap=memcpy
    -Wl,--wrap=memmove
    -Wl,--wrap=memset
    -Wl,--wrap=bzero
    -Wl,--wrap=memcmp
    -Wl,--wrap=strlen
    -Wl,--wrap=strnlen
    -Wl,--wrap=strcpy
    -Wl,--wrap=strncpy
    -Wl,--wrap=strcmp
    -Wl,--wrap=strncmp
    -Wl,--wrap=strcasecmp
    -Wl,--wrap=strncasecmp
    -Wl,--wrap=vsprintf
    -Wl,--wrap=vsnprintf
    -Wl,--wrap=sprintf
    -Wl,--wrap=snprintf)
  message(STATUS "Sanity instrumentation enabled: ${SEEKDB_SANITY_PASS}")
endif()

# Only resolve path if it exists (on macOS, this directory may not exist)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-#pragma-messages")
if(EXISTS "${STDC_SHARED_LIB_SYMLINK}")
  file(REAL_PATH "${STDC_SHARED_LIB_SYMLINK}" STDC_SHARED_LIB)
  message(STATUS "STDC_SHARED_LIB: ${STDC_SHARED_LIB}")
endif()

# Rust owns the MySQL protocol/NIO engine.  Keep Cargo's artifact inside the
# selected CMake build directory and expose its C ABI header to the migrated
# src/oblib and Observer sources.
include(cmake/Rust.cmake)
include_directories("${CMAKE_SOURCE_DIR}/rust/sql-nio/include")

add_subdirectory(src)

# Farm compile and pretest still publish and consume liboberror.so.  Keep the
# legacy error-code utility available as an explicit compatibility target
# without adding it to the default production build.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(tools/ob_error EXCLUDE_FROM_ALL)
endif()

# The farm pretest contract remains available during the CMake transition.
# Other unit-test workflows and their architecture rules stay Bazel-owned.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND EXISTS "${CMAKE_SOURCE_DIR}/unittest/CMakeLists.txt")
  enable_testing()
  add_subdirectory(unittest EXCLUDE_FROM_ALL)
  foreach(SEEKDB_PRETEST_MODULE IN LISTS SEEKDB_PRETEST_MODULES)
    string(TOUPPER "${SEEKDB_PRETEST_MODULE}" SEEKDB_PRETEST_MODULE_UPPER)
    set(SEEKDB_PRETEST_PREFIX
      "SEEKDB_PRETEST_${SEEKDB_PRETEST_MODULE_UPPER}")
    set(SEEKDB_PRETEST_TARGET "${${SEEKDB_PRETEST_PREFIX}_TARGET}")
    install(PROGRAMS "$<TARGET_FILE:${SEEKDB_PRETEST_TARGET}>"
      DESTINATION "unittest/${SEEKDB_PRETEST_MODULE}"
      COMPONENT pretest)
  endforeach()
endif()

if(SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "RPM")
  include(cmake/RPM.cmake)
elseif(SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "DEB")
  include(cmake/DEB.cmake)
elseif(SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "TGZ")
  include(cmake/TGZ.cmake)
elseif(SEEKDB_CMAKE_PACKAGE_FORMAT STREQUAL "WIX")
  include(cmake/WIX.cmake)
endif()
