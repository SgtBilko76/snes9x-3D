cmake_minimum_required(VERSION 3.22)
project(snes9xvr CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(OPENXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr")

# --- OpenXR loader (extracted from the Khronos AAR by build.sh) -------------
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${OPENXR_DIR}/libs/${ANDROID_ABI}/libopenxr_loader.so")

# --- native_app_glue --------------------------------------------------------
add_library(native_app_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue")

# --- Snes9x core ------------------------------------------------------------
set(CORE_SOURCES
    apu/apu.cpp apu/bapu/dsp/sdsp.cpp apu/bapu/smp/smp.cpp apu/bapu/smp/smp_state.cpp
    bml.cpp bsx.cpp c4.cpp c4emu.cpp cheats.cpp cheats2.cpp clip.cpp conffile.cpp
    controls.cpp cpu.cpp cpuexec.cpp cpuops.cpp crosshairs.cpp dma.cpp dsp.cpp
    dsp1.cpp dsp2.cpp dsp3.cpp dsp4.cpp fscompat.cpp fxemu.cpp fxinst.cpp gfx.cpp
    globals.cpp memmap.cpp movie.cpp msu1.cpp obc1.cpp ppu.cpp sa1.cpp sa1cpu.cpp
    screenshot.cpp sdd1.cpp sdd1emu.cpp seta.cpp seta010.cpp seta011.cpp seta018.cpp
    sha256.cpp snapshot.cpp snes9x.cpp spc7110.cpp srtc.cpp stream.cpp tile.cpp
    tileimpl-n1x1.cpp tileimpl-n2x1.cpp tileimpl-h2x1.cpp)
list(TRANSFORM CORE_SOURCES PREPEND "${CORE_DIR}/")

# Zip support, so ROMs can be pushed as-is.
list(APPEND CORE_SOURCES "${CORE_DIR}/loadzip.cpp")
set(UNZIP_SOURCES "${CORE_DIR}/unzip/unzip.c" "${CORE_DIR}/unzip/ioapi.c")

add_library(snes9x_core STATIC ${CORE_SOURCES} ${UNZIP_SOURCES}
            "${CORE_DIR}/filter/snes_ntsc.c")
target_include_directories(snes9x_core PUBLIC
    "${CORE_DIR}" "${CORE_DIR}/apu" "${CORE_DIR}/apu/bapu")
target_compile_definitions(snes9x_core PUBLIC
    RIGHTSHIFT_IS_SAR HAVE_STRINGS_H HAVE_STDINT_H SNES9X_VR UNZIP_SUPPORT)
target_compile_options(snes9x_core PRIVATE
    -O3 -fno-strict-aliasing -Wno-unused-value -Wno-deprecated-declarations)

# --- VR frontend ------------------------------------------------------------
add_library(snes9xvr SHARED
    main.cpp renderer.cpp emu.cpp audio.cpp input.cpp menu.cpp port_glue.cpp)
target_include_directories(snes9xvr PRIVATE "${OPENXR_DIR}/include")
if(NOT DEFINED SNES9X_VR_VERSION)
    set(SNES9X_VR_VERSION "dev")
endif()

target_compile_definitions(snes9xvr PRIVATE
    XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES
    SNES9X_VR_VERSION="${SNES9X_VR_VERSION}")
target_compile_options(snes9xvr PRIVATE -O2)
# native_app_glue's entry point is only referenced by the framework, so keep
# --gc-sections from dropping it.
target_link_options(snes9xvr PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(snes9xvr
    snes9x_core native_app_glue openxr_loader
    android log EGL GLESv3 aaudio z)
