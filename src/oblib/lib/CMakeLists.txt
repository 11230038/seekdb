add_subdirectory(compress)
add_subdirectory(restore)

seekdb_apply_unity_inventory(oblib_lib SEEKDB_OBLIB_LIB_UNITY)
# Bazel deliberately compiles the compression implementation outside its
# historical OBLib Unity group; preserve that action boundary here.
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/src/oblib/lib/oblog/ob_log_compressor.cpp
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# The hipVS bridge pulls in HIP/cuVS headers that do not tolerate being mixed
# into a unity translation unit, and it is only meaningful when the GPU backend
# is enabled. Keep it as a standalone object.
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/src/oblib/lib/vector/ob_hipvs_bridge.cpp
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
ob_lib_add_target(oblib_lib)

if(OB_HIPVS_ENABLED)
  find_package(cuvs CONFIG REQUIRED)
  if(EXISTS "${HIPVS_ROOT}/include")
    target_include_directories(oblib_lib PRIVATE "${HIPVS_ROOT}/include")
  endif()
  # Define the platform macro explicitly so the bridge does not depend on how the
  # HIP headers detect the platform; without it <hip/hip_runtime.h> rejects the
  # translation unit.
  target_compile_definitions(oblib_lib PRIVATE OB_ENABLE_HIPVS=1 __HIP_PLATFORM_AMD__=1)
  # Keep the GPU dependency private to OBLib: the bridge is only referenced by
  # ob_vsag_adaptor.cpp in this same static library. PRIVATE still propagates the
  # link dependency to the final executable, but without leaking HIP compile
  # options or include paths into every other target in the tree.
  target_link_libraries(oblib_lib PRIVATE cuvs::cuvs)
  if(OB_HIPVS_HIP_TOOLCHAIN)
    # cuVS/hipVS instantiate device code from their headers, so the bridge must be
    # compiled by the HIP toolchain. Only this one source switches languages;
    # the rest of the target keeps using the host C++ compiler.
    set_source_files_properties(
      ${CMAKE_SOURCE_DIR}/src/oblib/lib/vector/ob_hipvs_bridge.cpp
      PROPERTIES LANGUAGE HIP)
    find_package(hip CONFIG QUIET)
    if(TARGET hip::host)
      target_link_libraries(oblib_lib PRIVATE hip::host)
    endif()
    if(TARGET hip::device)
      target_link_libraries(oblib_lib PRIVATE hip::device)
    endif()
    if(NOT TARGET hip::host AND CMAKE_HIP_COMPILER)
      # The hip package config was not found; derive the ROCm root from the
      # compiler location (ROCM/bin/amdclang++) so <hip/hip_runtime.h> and
      # libamdhip64 are still reachable.
      get_filename_component(hipvs_compiler_bin "${CMAKE_HIP_COMPILER}" DIRECTORY)
      get_filename_component(hipvs_rocm_root "${hipvs_compiler_bin}" DIRECTORY)
      if(EXISTS "${hipvs_rocm_root}/include/hip/hip_runtime.h")
        target_include_directories(oblib_lib PRIVATE "${hipvs_rocm_root}/include")
        target_link_directories(oblib_lib PRIVATE "${hipvs_rocm_root}/lib")
        target_link_libraries(oblib_lib PRIVATE amdhip64)
      endif()
    endif()
  endif()
endif()

seekdb_apply_unity_inventory(oblib_lib_simd SEEKDB_OBLIB_SIMD_UNITY)
ob_lib_add_target(oblib_lib_simd)
if(ARCHITECTURE STREQUAL "x86_64")
  target_compile_options(oblib_lib_simd PRIVATE -mavx2 -mssse3)
elseif(ARCHITECTURE STREQUAL "amd64")
  target_compile_options(oblib_lib_simd PRIVATE
    -mavx2 -mssse3 -msse3 -msse4.1)
endif()

seekdb_apply_unity_inventory(oblib_lib_bitmap SEEKDB_OBLIB_BITMAP_UNITY)
ob_lib_add_target(oblib_lib_bitmap)

seekdb_apply_unity_inventory(ob_malloc_object SEEKDB_OBLIB_MALLOC_UNITY)
ob_add_new_object_target(ob_malloc_object ob_malloc_object)
target_link_libraries(ob_malloc_object oblib_base_without_pass)
add_library(ob_malloc STATIC)
target_link_libraries(ob_malloc ob_malloc_object)
if(OB_HAVE_BUNDLED_JEMALLOC)
  target_link_libraries(ob_malloc_object seekdb_jemalloc)
  target_link_libraries(ob_malloc seekdb_jemalloc)
endif()

# malloc_hook is only part of the Linux production runtime.
if(NOT APPLE AND NOT WIN32 AND NOT ANDROID)
  add_library(malloc_hook STATIC ${SEEKDB_OBLIB_MALLOC_HOOK_STANDALONE})
  target_link_libraries(malloc_hook oblib_base)
  if(OB_HAVE_BUNDLED_JEMALLOC)
    target_link_libraries(malloc_hook seekdb_jemalloc)
  endif()
endif()

if(WIN32)
  set(VCPKG_LIB_DIR "${OB_VCPKG_DIR}/lib")
  target_link_libraries(oblib_lib
    PUBLIC ob_malloc compress restore
    ${VCPKG_LIB_DIR}/libxml2.lib)
else()
  target_link_libraries(oblib_lib
    PUBLIC ob_malloc compress restore
    ${DEP_DIR}/lib/libxml2.a
    ${DEP_DIR}/lib/liblzma.a)
endif()

if(ANDROID)
  # Android dependencies are installed directly under DEP_DIR/lib.
  target_link_directories(oblib_lib PUBLIC ${DEP_DIR}/lib)
  target_compile_options(oblib_lib PUBLIC -fopenmp)
  target_link_libraries(oblib_lib PUBLIC
    libvsag_static.a
    libantlr4-autogen.a
    libantlr4-runtime.a
    libdiskann.a
    libsimd.a
    libcpuinfo.a
    libfmt.a
    libopenblas.a
    libspdlog.a
    libroaring.a
    -fopenmp -static-openmp)
else()
  target_link_directories(oblib_lib PUBLIC
    ${DEP_DIR}/lib ${DEP_DIR}/lib/vsag_lib)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Apple Clang accepts OpenMP in its frontend; the iOS runtime is linked separately.
    target_compile_options(oblib_lib PUBLIC "SHELL:-Xpreprocessor -fopenmp")
  elseif(UNIX AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # AppleClang's driver does not expose -fopenmp, but its frontend does.
    # Keep using the libomp runtime packaged with the arm64 VSAG libraries.
    target_compile_options(oblib_lib PUBLIC "SHELL:-Xclang -fopenmp")
  elseif(UNIX)
    target_compile_options(oblib_lib PUBLIC -fopenmp)
  endif()

  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    target_link_libraries(oblib_lib PUBLIC
      libcpuinfo.a
      libsimd.a
      libvsag_static.a
      libdiskann.a
      libopenblas.a
      libgfortran_static.a
      libquadmath_static.a
      libgomp_static.a
      libroaring.a
      libantlr4-runtime.a
      libantlr4-autogen.a
      libfmt.a)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_link_libraries(oblib_lib PUBLIC
      libcpuinfo.a
      libsimd.a
      libvsag_static.a
      libdiskann.a
      libopenblas.a
      libgfortran_static.a
      libgomp_static.a
      libroaring.a
      libantlr4-runtime.a
      libantlr4-autogen.a
      libfmt.a)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    target_link_libraries(oblib_lib PUBLIC
      libcpuinfo.a
      libsimd.a
      libvsag_static.a
      libdiskann.a
      libio.a
      libroaring.a
      libantlr4-runtime.a
      libantlr4-autogen.a
      libfmt.a)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      target_link_libraries(oblib_lib PUBLIC
        libomp.a liblapacke.a "-framework Accelerate")
    else()
      target_link_libraries(oblib_lib PUBLIC
        libgfortran_static.a libquadmath_static.a libgcc.a
        libomp_static.a libopenblas.a)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    target_link_directories(oblib_lib PUBLIC ${OB_VSAG_DIR}/lib)
    target_include_directories(oblib_lib PUBLIC ${OB_VSAG_DIR}/include)
    target_link_libraries(oblib_lib PUBLIC
      cpuinfo.lib
      simd.lib
      vsag_static.lib
      diskann.lib
      openblas.lib
      libomp.lib
      roaring.lib
      antlr4-runtime-static.lib
      antlr4-autogen.lib
      fmt.lib)
  else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()
