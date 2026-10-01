#include "flashport/ProjectGenerator.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace flashport {
namespace fs = std::filesystem;

static std::string escapeCpp(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

void ProjectGenerator::generate(const SWFSummary& s,
                                const std::string& inputSwf,
                                const std::string& outputDir,
                                const std::string& runtimeDir) const {
    const fs::path root(outputDir);
    fs::create_directories(root / "src");
    fs::create_directories(root / "assets");

    {
        std::ofstream f(root / "CMakeLists.txt");
        if (!f) throw std::runtime_error("cannot write generated CMakeLists.txt");
        f << R"(cmake_minimum_required(VERSION 3.21)
project(GeneratedFlashPort LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MINGW)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()
find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)
file(GLOB FLASHPORT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(flash_game ${FLASHPORT_SOURCES})
target_include_directories(flash_game PRIVATE src)
# SDL2main must precede SDL2 on the link line (MinGW resolves archives left to right).
if(TARGET SDL2::SDL2main)
  target_link_libraries(flash_game PRIVATE SDL2::SDL2main)
endif()

# Make the executable runnable outside the toolchain's environment:
# MinGW links everything statically (no SDL2.dll / libstdc++ DLLs needed);
# other toolchains get the SDL2 runtime DLL copied next to the executable.
if(MINGW AND TARGET SDL2::SDL2-static)
  target_link_libraries(flash_game PRIVATE SDL2::SDL2-static ZLIB::ZLIB)
  target_link_options(flash_game PRIVATE -static)
else()
  target_link_libraries(flash_game PRIVATE SDL2::SDL2 ZLIB::ZLIB)
  if(WIN32)
    add_custom_command(TARGET flash_game POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:flash_game> $<TARGET_FILE_DIR:flash_game>
      COMMAND_EXPAND_LISTS)
  endif()
endif()

# Multi-file games keep their other SWF packs and data files in data/ next to movie.pack.
if (EXISTS ${CMAKE_SOURCE_DIR}/data)
  add_custom_command(TARGET flash_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:flash_game>/data)
endif()
# The runtime loads movie.pack from the executable's directory.
add_custom_command(TARGET flash_game POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/movie.pack $<TARGET_FILE_DIR:flash_game>/movie.pack)
)";
    }

    {
        std::ofstream f(root / "src" / "GameMetadata.hpp");
        f << R"(#pragma once
#include <cstdint>
namespace generated {
struct GameMetadata {
    static constexpr int width = )" << ((s.frameRect.xmax - s.frameRect.xmin) / 20) << R"(;
    static constexpr int height = )" << ((s.frameRect.ymax - s.frameRect.ymin) / 20) << R"(;
    static constexpr double fps = )" << std::fixed << std::setprecision(3) << s.fps << R"(;
    static constexpr std::uint16_t frames = )" << s.frameCount << R"(;
    static constexpr bool actionScript3 = )" << (s.actionScript3 ? "true" : "false") << R"(;
    static const char* documentClass();
    static const char* sourceFile();
};
}
)";
    }

    {
        std::ofstream f(root / "src" / "GameMetadata.cpp");
        f << "#include \"GameMetadata.hpp\"\nnamespace generated {\n";
        f << "const char* GameMetadata::documentClass() { return \"" << escapeCpp(s.documentClass) << "\"; }\n";
        f << "const char* GameMetadata::sourceFile() { return \"" << escapeCpp(fs::path(inputSwf).filename().string()) << "\"; }\n";
        f << "}\n";
    }

    // The runtime (movie pack loader, display list, renderer, main loop) is shared by
    // every generated project and copied verbatim from the builder's runtime/ directory.
    if (!fs::exists(fs::path(runtimeDir) / "FlashRuntime.cpp")) {
        throw std::runtime_error("runtime sources not found in " + runtimeDir);
    }
    for (const auto& entry : fs::directory_iterator(runtimeDir)) {
        const auto ext = entry.path().extension();
        if (entry.is_regular_file() && (ext == ".cpp" || ext == ".hpp")) {
            fs::copy_file(entry.path(), root / "src" / entry.path().filename(), fs::copy_options::overwrite_existing);
        }
    }

    {
        std::ofstream f(root / "analysis.txt");
        f << "FlashPortBuilder generated project\n\n";
        f << "Source: " << fs::path(inputSwf).filename().string() << "\n";
        f << "SWF version: " << static_cast<unsigned>(s.version) << "\n";
        f << "Stage: " << ((s.frameRect.xmax - s.frameRect.xmin) / 20) << "x"
          << ((s.frameRect.ymax - s.frameRect.ymin) / 20) << "\n";
        f << "FPS: " << s.fps << "\n";
        f << "Frames: " << s.frameCount << "\n";
        f << "ActionScript 3: " << (s.actionScript3 ? "yes" : "no") << "\n";
        f << "Document class: " << s.documentClass << "\n";
        f << "DoABC blocks: " << s.abcBlocks.size() << "\n";
        std::uint64_t methods = 0, bodies = 0, codeBytes = 0;
        for (const auto& abc : s.abcBlocks) {
            methods += abc.methods;
            bodies += abc.methodBodies;
            codeBytes += abc.bytecodeBytes;
        }
        f << "ABC methods: " << methods << "\n";
        f << "ABC method bodies: " << bodies << "\n";
        f << "AVM2 bytecode bytes: " << codeBytes << "\n\n";
        f << "Tag counts (recursive):\n";
        for (const auto& [code, count] : s.tagCounts) {
            f << "  " << code << " " << tagName(code) << ": " << count << "\n";
        }
    }

    {
        std::ofstream f(root / "README.md");
        f << R"(# Generated FlashPort project

This directory was generated by FlashPortBuilder v0.5.0.

## Contents

- `assets/` - converted characters: `bitmaps/*.png`, `shapes/*.svg`, `sounds/*.mp3|wav`,
  `streams/*.mp3|wav` and `manifest.json` (character kinds, bounds, sprite frame counts,
  symbol/linkage names). Nested SWFs (wrappers, packed loaders) get `embedded_<id>/` or
  `unpacked/` subdirectories; the innermost one is the game.
- `code/` - decoded AVM2 (`avm2/<abc>/<Class>.abc.txt`) and AVM1 (`avm1/*.avm1.txt`)
  listings with basic blocks, successors and verified stack/scope depths. These are the
  input for the upcoming C++ emitter.
- `movie.pack` - the game's timelines, tessellated shapes, textures and recognised
  frame actions, loaded by the runtime (copied next to the executable on build).
- `src/` - the SDL2 runtime: display list with Flash goto semantics, nested MovieClips,
  buttons (up state), colour transforms and a symbol viewer. Scripts are not executed:
  only literal stop()/play()/gotoAndPlay()-style frame actions are honoured.

## Controls

Space pause, Right/Left step frames, Home restart, F ignore stop(), V symbol viewer
(N/P next/previous), Esc quit.

## Build

```bash
cmake -B build
cmake --build build
```

## Next implementation layers

1. FlashValue / Object / Class runtime.
2. C++ emission for AVM2 method bodies and AVM1 action blocks.
3. EventDispatcher, input mapping and button interaction.
4. Text (fonts), masks, filters, morph shapes and sound playback.
)";
    }
}

} // namespace flashport
