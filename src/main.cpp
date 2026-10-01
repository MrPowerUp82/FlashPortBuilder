#include "flashport/AssetExtractor.hpp"
#include "flashport/Disassembler.hpp"
#include "flashport/MoviePack.hpp"
#include "flashport/ProjectGenerator.hpp"
#include "flashport/SWFDocument.hpp"
#include "flashport/SWFReader.hpp"
#include "flashport/Unpacker.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace flashport;

static void printUsage() {
    std::cout <<
        "FlashPortBuilder v0.5.0\n\n"
        "Usage:\n"
        "  flashport analyze <game.swf>\n"
        "  flashport matrix <game1.swf> <game2.swf> [...]\n"
        "  flashport unpack <game.swf> [--output <inner.swf>]\n"
        "  flashport disasm <game.swf> [--output <directory>]\n"
        "  flashport extract <game.swf> [--output <directory>]\n"
        "  flashport pack <game.swf> [--output <movie.pack>]\n"
        "  flashport build <game.swf> [--output <directory>]\n";
}

static std::string printableMagic(const std::string& magic) {
    bool printable = magic.size() == 3;
    for (unsigned char c : magic) {
        if (c < 32 || c > 126) printable = false;
    }
    if (printable) return magic;
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0');
    for (unsigned char c : magic) out << std::setw(2) << static_cast<unsigned>(c);
    return out.str();
}

static std::uint64_t abcMethods(const SWFSummary& s) {
    std::uint64_t n = 0; for (const auto& a : s.abcBlocks) n += a.methods; return n;
}
static std::uint64_t abcBodies(const SWFSummary& s) {
    std::uint64_t n = 0; for (const auto& a : s.abcBlocks) n += a.methodBodies; return n;
}
static std::uint64_t abcBytes(const SWFSummary& s) {
    std::uint64_t n = 0; for (const auto& a : s.abcBlocks) n += a.bytecodeBytes; return n;
}
static std::uint64_t tagCount(const SWFSummary& s, std::uint16_t code) {
    const auto it = s.tagCounts.find(code); return it == s.tagCounts.end() ? 0 : it->second;
}

static void printSummary(const SWFSummary& s, unsigned indent = 0) {
    const std::string pad(indent, ' ');
    const auto width = (s.frameRect.xmax - s.frameRect.xmin) / 20;
    const auto height = (s.frameRect.ymax - s.frameRect.ymin) / 20;
    std::uint64_t classes = 0, scripts = 0;
    for (const auto& abc : s.abcBlocks) {
        classes += abc.classes;
        scripts += abc.scripts;
    }

    std::cout << pad << "FlashPortBuilder Analyzer\n\n";
    std::cout << pad << "Source:         " << s.sourceName << "\n";
    std::cout << pad << "Signature:      " << s.signature << "\n";
    std::cout << pad << "SWF version:    " << static_cast<unsigned>(s.version) << "\n";
    std::cout << pad << "File size:      " << s.actualFileLength << " bytes\n";
    std::cout << pad << "Declared size:  " << s.declaredFileLength << " bytes\n";
    std::cout << pad << "Stage:          " << width << "x" << height << "\n";
    std::cout << pad << "FPS:            " << std::fixed << std::setprecision(2) << s.fps << "\n";
    std::cout << pad << "Main frames:    " << s.frameCount << "\n";
    std::cout << pad << "ActionScript:   " << actionScriptKind(s) << "\n";
    std::cout << pad << "Document class: " << (s.documentClass.empty() ? "<none>" : s.documentClass) << "\n\n";

    std::cout << pad << "ABC blocks:     " << s.abcBlocks.size() << "\n";
    std::cout << pad << "ABC methods:    " << abcMethods(s) << "\n";
    std::cout << pad << "ABC classes:    " << classes << "\n";
    std::cout << pad << "ABC scripts:    " << scripts << "\n";
    std::cout << pad << "Method bodies:  " << abcBodies(s) << "\n";
    std::cout << pad << "Bytecode bytes: " << abcBytes(s) << "\n";
    std::cout << pad << "Tags total:     " << s.totalTags << " (recursive)\n\n";

    std::cout << pad << "Tags:\n";
    for (const auto& [code, count] : s.tagCounts) {
        std::cout << pad << "  " << std::setw(3) << code << "  " << std::setw(28) << std::left
                  << tagName(code) << std::right << " " << count << "\n";
    }

    if (!s.binaryData.empty()) {
        std::cout << "\n" << pad << "DefineBinaryData:\n";
        for (const auto& b : s.binaryData) {
            std::cout << pad << "  id=" << b.characterId << " size=" << b.payloadBytes;
            if (!b.magic.empty()) std::cout << " magic=" << printableMagic(b.magic);
            if (b.embeddedSwf) std::cout << "  [embedded SWF detected]";
            std::cout << "\n";
        }
    }

    for (const auto& b : s.binaryData) {
        if (b.embeddedSwf) {
            std::cout << "\n" << pad << "==== Embedded SWF from BinaryData #" << b.characterId << " ====\n\n";
            printSummary(*b.embeddedSwf, indent + 2);
        }
    }
}

// Visit a SWF and, recursively, every SWF embedded in DefineBinaryData or recoverable
// through a known loader/packer scheme. Nested SWFs get "embedded_<id>" / "unpacked" subdirectories.
template <typename Visit>
static void forEachSWF(const SWFDocument& doc, const std::filesystem::path& dir, Visit&& visit, unsigned depth = 0) {
    visit(doc, dir);
    if (depth >= 8) return;
    const auto plain = binaryDataPayloads(doc, true);
    for (const auto& b : plain) {
        try {
            const auto inner = loadSWFDocument(b.bytes, doc.sourceName + "::BinaryData#" + std::to_string(b.characterId));
            forEachSWF(inner, dir / ("embedded_" + std::to_string(b.characterId)), visit, depth + 1);
        } catch (const std::exception& e) {
            std::cerr << "warning: embedded SWF #" << b.characterId << ": " << e.what() << "\n";
        }
    }
    if (plain.empty()) {
        if (const auto unpacked = unpackLoader(doc)) {
            std::cout << "Unpacked " << doc.sourceName << ": " << unpacked->method << "\n";
            const auto inner = loadSWFDocument(unpacked->swf, doc.sourceName + "::unpacked");
            forEachSWF(inner, dir / "unpacked", visit, depth + 1);
        }
    }
}

static std::string outputArg(int argc, char** argv, const std::string& input, const std::string& suffix) {
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) return argv[i + 1];
    }
    return "output/" + std::filesystem::path(input).stem().string() + suffix;
}

// The runtime sources live in the builder's runtime/ directory: FLASHPORT_RUNTIME_DIR
// (environment), then the path baked in at build time, then next to the executable.
static std::string findRuntimeDir(const char* argv0) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("FLASHPORT_RUNTIME_DIR")) candidates.emplace_back(env);
#ifdef FLASHPORT_RUNTIME_DIR
    candidates.emplace_back(FLASHPORT_RUNTIME_DIR);
#endif
    const auto exeDir = fs::absolute(argv0).parent_path();
    candidates.push_back(exeDir / "runtime");
    candidates.push_back(exeDir.parent_path() / "runtime");
    for (const auto& c : candidates) {
        if (fs::exists(c / "FlashRuntime.cpp")) return c.string();
    }
    throw std::runtime_error("FlashPort runtime sources not found (set FLASHPORT_RUNTIME_DIR)");
}

static void printMatrix(const std::vector<std::pair<std::string, SWFSummary>>& rows) {
    std::cout << "FlashPortBuilder compatibility matrix\n\n";
    std::cout << std::left
              << std::setw(34) << "File"
              << std::setw(6) << "Ver"
              << std::setw(14) << "Stage"
              << std::setw(8) << "FPS"
              << std::setw(18) << "Script"
              << std::setw(9) << "DoABC"
              << std::setw(10) << "DoAction"
              << std::setw(9) << "Sprites"
              << std::setw(10) << "Binary"
              << "Embedded" << "\n";
    std::cout << std::string(128, '-') << "\n";
    for (const auto& [file, s] : rows) {
        const auto w = (s.frameRect.xmax - s.frameRect.xmin) / 20;
        const auto h = (s.frameRect.ymax - s.frameRect.ymin) / 20;
        const std::string stage = std::to_string(w) + "x" + std::to_string(h);
        std::size_t embedded = 0;
        for (const auto& b : s.binaryData) if (b.embeddedSwf) ++embedded;
        std::cout << std::left
                  << std::setw(34) << std::filesystem::path(file).filename().string().substr(0, 33)
                  << std::setw(6) << static_cast<unsigned>(s.version)
                  << std::setw(14) << stage
                  << std::setw(8) << std::fixed << std::setprecision(1) << s.fps
                  << std::setw(18) << actionScriptKind(s).substr(0, 17)
                  << std::setw(9) << s.abcBlocks.size()
                  << std::setw(10) << tagCount(s, 12)
                  << std::setw(9) << tagCount(s, 39)
                  << std::setw(10) << s.binaryData.size()
                  << embedded << "\n";
    }
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            printUsage();
            return 0;
        }

        const std::string command = argv[1];
        SWFReader reader;

        if (command == "matrix") {
            if (argc < 3) { printUsage(); return 2; }
            std::vector<std::pair<std::string, SWFSummary>> rows;
            for (int i = 2; i < argc; ++i) rows.emplace_back(argv[i], reader.analyzeFile(argv[i]));
            printMatrix(rows);
            return 0;
        }

        if (argc < 3) { printUsage(); return 2; }
        const std::string input = argv[2];

        if (command == "unpack") {
            const auto doc = loadSWFDocumentFile(input);
            const auto unpacked = unpackLoader(doc);
            if (!unpacked) {
                std::cerr << "no known loader/packer scheme matched\n";
                return 1;
            }
            const auto out = outputArg(argc, argv, input, "_unpacked.swf");
            if (std::filesystem::path(out).has_parent_path()) {
                std::filesystem::create_directories(std::filesystem::path(out).parent_path());
            }
            std::ofstream f(out, std::ios::binary);
            f.write(reinterpret_cast<const char*>(unpacked->swf.data()), static_cast<std::streamsize>(unpacked->swf.size()));
            std::cout << "Method: " << unpacked->method << "\n"
                      << "Wrote " << unpacked->swf.size() << " bytes to " << out << "\n";
            return 0;
        }

        if (command == "disasm") {
            const auto outDir = outputArg(argc, argv, input, "_disasm");
            DisasmReport report;
            forEachSWF(loadSWFDocumentFile(input), outDir, [&](const SWFDocument& doc, const std::filesystem::path& dir) {
                disassembleSWF(doc, dir.string(), report);
            });
            printDisasmReport(report, std::cout);
            std::cout << "\nListings written to: " << outDir << "\n";
            return 0;
        }

        if (command == "extract") {
            const auto outDir = outputArg(argc, argv, input, "_assets");
            forEachSWF(loadSWFDocumentFile(input), outDir, [&](const SWFDocument& doc, const std::filesystem::path& dir) {
                AssetReport report;
                extractAssets(doc, dir.string(), report);
                std::cout << "[" << dir.string() << "]\n";
                printAssetReport(report, std::cout);
            });
            return 0;
        }

        if (command == "pack") {
            SWFDocument gameDoc;
            forEachSWF(loadSWFDocumentFile(input), "", [&](const SWFDocument& doc, const std::filesystem::path&) { gameDoc = doc; });
            MoviePackReport pack;
            const auto out = outputArg(argc, argv, input, ".pack");
            writeMoviePack(gameDoc, out, pack);
            printMoviePackReport(pack, std::cout);
            std::cout << "Wrote " << out << "\n";
            return 0;
        }

        const auto summary = reader.analyzeFile(input);

        if (command == "analyze") {
            printSummary(summary);
            return 0;
        }

        if (command == "build") {
            const auto output = outputArg(argc, argv, input, "");
            const std::filesystem::path root(output);
            // Assets and code listings for the whole SWF tree; the innermost SWF
            // (unwrapped or unpacked) is the actual game and drives the runtime metadata.
            DisasmReport disasm;
            SWFDocument gameDoc;
            forEachSWF(loadSWFDocumentFile(input), "", [&](const SWFDocument& doc, const std::filesystem::path& rel) {
                AssetReport assets;
                extractAssets(doc, (root / "assets" / rel).string(), assets);
                std::cout << "[assets/" << rel.generic_string() << "]\n";
                printAssetReport(assets, std::cout);
                disassembleSWF(doc, (root / "code" / rel).string(), disasm);
                gameDoc = doc;
            });
            MoviePackReport pack;
            writeMoviePack(gameDoc, (root / "movie.pack").string(), pack);
            const auto game = reader.analyzeBytes(gameDoc.data, gameDoc.sourceName);
            ProjectGenerator{}.generate(game, input, output, findRuntimeDir(argv[0]));
            std::cout << "\n";
            printDisasmReport(disasm, std::cout);
            printMoviePackReport(pack, std::cout);
            std::cout << "\nGame SWF: " << gameDoc.sourceName << " (" << (game.frameRect.xmax - game.frameRect.xmin) / 20 << "x"
                      << (game.frameRect.ymax - game.frameRect.ymin) / 20 << " @ " << game.fps << " fps, "
                      << actionScriptKind(game) << ")\n";
            std::cout << "Generated project: " << output << "\n";
            return 0;
        }

        printUsage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
