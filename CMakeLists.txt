cmake_minimum_required(VERSION 3.20)
project(FlashPortBuilder VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MinGW builds link statically, so prefer static archives when searching.
if (MINGW)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

find_package(ZLIB REQUIRED)

# Optional: libjpeg-turbo lets the asset stage decode JPEGs (needed to merge
# DefineBitsJPEG3/4 alpha planes into PNGs). Without it JPEGs are copied verbatim.
find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg libturbojpeg)

# Optional: libmpg123 decodes MP3 sounds into the movie pack (without it MP3 sounds are silent).
find_path(MPG123_INCLUDE_DIR mpg123.h)
find_library(MPG123_LIBRARY NAMES mpg123 libmpg123)

# Optional: liblzma (xz) decodes ZWS (LZMA-compressed) SWFs.
find_path(LZMA_INCLUDE_DIR lzma.h)
find_library(LZMA_LIBRARY NAMES lzma liblzma)

add_executable(flashport
    src/main.cpp
    src/swf/SWFReader.cpp
    src/swf/SWFDocument.cpp
    src/swf/SWFStructures.cpp
    src/avm1/AVM1Code.cpp
    src/avm2/ABCReader.cpp
    src/avm2/ABCFile.cpp
    src/avm2/AVM2Code.cpp
    src/assets/Image.cpp
    src/assets/ImageDecode.cpp
    src/assets/Audio.cpp
    src/assets/Shape.cpp
    src/assets/AssetExtractor.cpp
    src/assets/Tessellator.cpp
    src/unpack/Unpacker.cpp
    src/generator/Disassembler.cpp
    src/generator/FrameScripts.cpp
    src/generator/MoviePack.cpp
    src/generator/ProjectGenerator.cpp
)

target_include_directories(flashport PRIVATE include)
target_compile_definitions(flashport PRIVATE FLASHPORT_RUNTIME_DIR="${CMAKE_SOURCE_DIR}/runtime")
target_link_libraries(flashport PRIVATE ZLIB::ZLIB)

if (TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIBRARY)
    message(STATUS "libjpeg-turbo: ${TURBOJPEG_LIBRARY}")
    target_include_directories(flashport PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(flashport PRIVATE ${TURBOJPEG_LIBRARY})
    target_compile_definitions(flashport PRIVATE FLASHPORT_HAVE_TURBOJPEG)
else()
    message(STATUS "libjpeg-turbo not found: JPEG bitmaps will be kept as .jpg")
endif()

if (MPG123_INCLUDE_DIR AND MPG123_LIBRARY)
    message(STATUS "libmpg123: ${MPG123_LIBRARY}")
    target_include_directories(flashport PRIVATE ${MPG123_INCLUDE_DIR})
    target_link_libraries(flashport PRIVATE ${MPG123_LIBRARY} $<$<BOOL:${MINGW}>:shlwapi>)
    target_compile_definitions(flashport PRIVATE FLASHPORT_HAVE_MPG123)
else()
    message(STATUS "libmpg123 not found: MP3 sounds will be silent in the movie pack")
endif()

if (LZMA_INCLUDE_DIR AND LZMA_LIBRARY)
    message(STATUS "liblzma: ${LZMA_LIBRARY}")
    target_include_directories(flashport PRIVATE ${LZMA_INCLUDE_DIR})
    target_link_libraries(flashport PRIVATE ${LZMA_LIBRARY})
    target_compile_definitions(flashport PRIVATE FLASHPORT_HAVE_LZMA LZMA_API_STATIC)
else()
    message(STATUS "liblzma not found: ZWS (LZMA) SWFs are not supported")
endif()

if (MSVC)
    target_compile_options(flashport PRIVATE /W4)
else()
    target_compile_options(flashport PRIVATE -Wall -Wextra -Wpedantic)
endif()

# MinGW: avoid depending on libstdc++/libgcc/zlib DLLs at runtime.
if (MINGW)
    target_link_options(flashport PRIVATE -static)
endif()
