cmake_minimum_required(VERSION 3.16)
project(ffsdl_rtsp_player LANGUAGES C)

include(CTest)

set(CLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/clib" CACHE PATH
  "Path to the bundled or external clib source tree")
set(FFMPEG_VERSION "4.3" CACHE STRING "FFmpeg ABI selection: 4.3 or 6.1")
set_property(CACHE FFMPEG_VERSION PROPERTY STRINGS 4.3 6.1)
if(NOT FFMPEG_VERSION STREQUAL "4.3" AND NOT FFMPEG_VERSION STREQUAL "6.1")
  message(FATAL_ERROR "FFMPEG_VERSION must be 4.3 or 6.1")
endif()
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-${FFMPEG_VERSION}" CACHE PATH
  "Selected FFmpeg SDK prefix")
set(FFMPEG_LINK_LIBS "" CACHE STRING "Optional explicit FFmpeg linker libraries")
set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl2" CACHE PATH
  "SDL2 SDK prefix for bundled clib")
set(THIRD_PARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
  "Project third_party runtime and SDK root for bundled clib")

if(NOT EXISTS "${CLIB_ROOT}/Makefile")
  message(FATAL_ERROR "CLIB_ROOT must contain clib's Makefile: ${CLIB_ROOT}")
endif()

  set(CLIB_MAKE_ARGS DISPLAY_BACKEND=sdl)
  # clib is built by a nested Makefile, so CMake's toolchain is not
  # propagated automatically.  Pass the selected compiler explicitly;
  # otherwise the nested build falls back to the host `gcc` and cannot link
  # the AArch64 FFmpeg/SDL/RGA libraries.
  if(CMAKE_C_COMPILER)
    list(APPEND CLIB_MAKE_ARGS "CC=${CMAKE_C_COMPILER}")
  endif()
  if(CMAKE_AR)
    list(APPEND CLIB_MAKE_ARGS "AR=${CMAKE_AR}")
  endif()
  if(FFMPEG_ROOT)
    list(APPEND CLIB_MAKE_ARGS "FFMPEG_ROOT=${FFMPEG_ROOT}")
  endif()
  list(APPEND CLIB_MAKE_ARGS "FFMPEG_VERSION=${FFMPEG_VERSION}")
  if(FFMPEG_LINK_LIBS)
    list(APPEND CLIB_MAKE_ARGS "FFMPEG_LIBS=${FFMPEG_LINK_LIBS}")
  endif()
  if(SDL2_ROOT)
    list(APPEND CLIB_MAKE_ARGS "SDL2_ROOT=${SDL2_ROOT}")
  endif()
  if(THIRD_PARTY_ROOT)
    list(APPEND CLIB_MAKE_ARGS "THIRD_PARTY_ROOT=${THIRD_PARTY_ROOT}")
  endif()

  add_custom_target(clib_sdl ALL
    COMMAND ${CMAKE_MAKE_PROGRAM} -C "${CLIB_ROOT}" -B ${CLIB_MAKE_ARGS}
    COMMENT "Building bundled clib with the SDL2 display backend"
    VERBATIM)

  set(CLIB_INCLUDE_DIR "${CLIB_ROOT}/include")
  if(APPLE)
    set(CLIB_LIBRARY "${CLIB_ROOT}/libclib.dylib")
  else()
    set(CLIB_LIBRARY "${CLIB_ROOT}/libclib.so")
  endif()
  add_library(clib_sdl_library SHARED IMPORTED GLOBAL)
  set_target_properties(clib_sdl_library PROPERTIES
    IMPORTED_LOCATION "${CLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CLIB_INCLUDE_DIR}")

  add_executable(ffsdl src/clib_rtsp_sdl_player.c)
  add_dependencies(ffsdl clib_sdl)
  target_link_libraries(ffsdl PRIVATE clib_sdl_library)
  if(UNIX AND NOT APPLE)
    target_link_options(ffsdl PRIVATE "-Wl,--allow-shlib-undefined")
  endif()
  if(UNIX AND NOT APPLE)
    # Keep a copied demo self-contained on the target: libclib.so lives next
    # to the executable in the deployment directory.
    set_target_properties(ffsdl PROPERTIES
      BUILD_RPATH "$ORIGIN/../clib;$ORIGIN/../third_party/ffmpeg-${FFMPEG_VERSION}/lib;$ORIGIN/../third_party/sdl2/lib;$ORIGIN/../third_party/lib/aarch64-linux-gnu"
      BUILD_WITH_INSTALL_RPATH FALSE
      INSTALL_RPATH "$ORIGIN")
  elseif(APPLE)
    set_target_properties(ffsdl PROPERTIES
      BUILD_RPATH "${CLIB_ROOT}")
  endif()
  if(BUILD_TESTING)
    add_executable(test_rga_dmabuf_csc tests/test_rga_dmabuf_csc.c)
    add_dependencies(test_rga_dmabuf_csc clib_sdl)
    target_include_directories(test_rga_dmabuf_csc PRIVATE ${THIRD_PARTY_ROOT}/include/mpp)
    target_link_libraries(test_rga_dmabuf_csc PRIVATE clib_sdl_library)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      find_library(ROCKCHIP_MPP_LIBRARY NAMES rockchip_mpp)
      if(ROCKCHIP_MPP_LIBRARY)
        target_link_libraries(test_rga_dmabuf_csc PRIVATE ${ROCKCHIP_MPP_LIBRARY})
      endif()
    endif()
    if(UNIX AND NOT APPLE)
      target_link_options(test_rga_dmabuf_csc PRIVATE "-Wl,--allow-shlib-undefined")
    endif()
    add_test(NAME rga_dmabuf_csc COMMAND test_rga_dmabuf_csc)

    add_executable(test_clib_sdl_display tests/test_clib_sdl_display.c)
    add_dependencies(test_clib_sdl_display clib_sdl)
    target_link_libraries(test_clib_sdl_display PRIVATE clib_sdl_library)
    if(UNIX AND NOT APPLE)
      target_link_options(test_clib_sdl_display PRIVATE "-Wl,--allow-shlib-undefined")
    endif()
    if(UNIX AND NOT APPLE)
      set_target_properties(test_clib_sdl_display PROPERTIES
        BUILD_RPATH "$ORIGIN/../clib;$ORIGIN/../third_party/ffmpeg-${FFMPEG_VERSION}/lib;$ORIGIN/../third_party/sdl2/lib;$ORIGIN/../third_party/lib/aarch64-linux-gnu"
        BUILD_WITH_INSTALL_RPATH FALSE
        INSTALL_RPATH "$ORIGIN")
    elseif(APPLE)
      set_target_properties(test_clib_sdl_display PROPERTIES
        BUILD_RPATH "${CLIB_ROOT}")
    endif()
    add_test(NAME clib_sdl_display COMMAND test_clib_sdl_display)
    set_tests_properties(clib_sdl_display PROPERTIES
      ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  endif()
