#include "AVM1.hpp"
#include "FlashRuntime.hpp"
#include "GameMetadata.hpp"
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ScriptedInput {
    std::uint64_t tick{};
    std::string kind; // click, down, up, key (down+up), move
    float x{}, y{};
    int key{};
};

struct Options {
    std::string pack;
    std::string capture;       // --capture <file.bmp>: save a frame and exit (for automated checks)
    std::string audioOut;      // --audio-out <file.wav>: with --capture, also save the mixed audio
    int captureAfter = 1;      // --after <ticks>
    bool ignoreStops = false;  // --play-all
    int viewSymbol = -1;       // --view <index>: start in the symbol viewer
    int viewId = -1;           // --view-id <character>: start the viewer on that character
    bool dump = false;         // --dump: print the display list when capturing
    std::vector<ScriptedInput> input; // --input "tick:click:x,y;tick:key:code;..."
};

std::vector<ScriptedInput> parseInput(const std::string& spec) {
    std::vector<ScriptedInput> out;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';')) {
        ScriptedInput in;
        const auto a = item.find(':'), b = item.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        in.tick = std::strtoull(item.substr(0, a).c_str(), nullptr, 10);
        in.kind = item.substr(a + 1, b - a - 1);
        const auto args = item.substr(b + 1);
        if (in.kind == "click" || in.kind == "move") std::sscanf(args.c_str(), "%f,%f", &in.x, &in.y);
        else in.key = std::atoi(args.c_str());
        out.push_back(in);
    }
    return out;
}

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--audio-out" && i + 1 < argc) o.audioOut = argv[++i];
        else if (a == "--capture" && i + 1 < argc) o.capture = argv[++i];
        else if (a == "--after" && i + 1 < argc) o.captureAfter = std::max(0, std::atoi(argv[++i]));
        else if (a == "--play-all") o.ignoreStops = true;
        else if (a == "--dump") o.dump = true;
        else if (a == "--view-id" && i + 1 < argc) o.viewId = std::atoi(argv[++i]);
        else if (a == "--view" && i + 1 < argc) o.viewSymbol = std::max(0, std::atoi(argv[++i]));
        else if (a == "--input" && i + 1 < argc) o.input = parseInput(argv[++i]);
        else o.pack = a;
    }
    if (o.pack.empty()) {
        if (char* p = SDL_GetBasePath()) { o.pack = p; SDL_free(p); }
        o.pack += "movie.pack";
    }
    return o;
}

const fp::Movie* g_movie = nullptr; // for --dump

// Display list dump for debugging (--dump): depth, kind, character, instance name, play head.
void dumpClip(const fp::Clip& clip, int indent, int maxDepth) {
    if (indent > maxDepth) return;
    for (const auto& [depth, obj] : clip.children()) {
        static const char* kinds[] = {"shape", "sprite", "button", "text", "other"};
        std::cout << std::string(static_cast<std::size_t>(indent) * 2, ' ') << depth << " " << kinds[static_cast<int>(obj->kind)]
                  << " #" << obj->character;
        if (!obj->name.empty()) std::cout << " '" << obj->name << "'";
        if (!obj->visible) std::cout << " hidden";
        if (obj->clipDepth) std::cout << " mask->" << obj->clipDepth;
        if (obj->cxform.mul[3] != 1 || obj->cxform.add[3] != 0) std::cout << " alpha*" << obj->cxform.mul[3] << "+" << obj->cxform.add[3];
        if (obj->kind == fp::DisplayObject::Kind::Text) std::cout << " text=\"" << obj->text << "\"";
        if (g_movie && std::getenv("FP_DUMP_BOUNDS")) {
            // Bounds of the object in its parent's pixel space.
            fp::Geometry geo(*g_movie);
            float b[4];
            if (geo.bounds(*obj, fp::Matrix{}, b)) std::printf(" bounds=[%.0f,%.0f %.0f,%.0f]", b[0] / 20, b[1] / 20, b[2] / 20, b[3] / 20);
        }
        if (obj->kind == fp::DisplayObject::Kind::Shape && g_movie && std::getenv("FP_DUMP_BUTTONS")) {
            std::size_t tris = 0;
            if (auto it = g_movie->shapes.find(obj->character); it != g_movie->shapes.end()) for (const auto& m : it->second.meshes) tris += m.indices.size() / 3;
            std::cout << " tris=" << tris;
        }
        if (obj->kind == fp::DisplayObject::Kind::Button && std::getenv("FP_DUMP_BUTTONS")) {
            std::cout << " state=" << int(obj->buttonState) << " records:";
            if (auto* def = g_movie ? &g_movie->buttons : nullptr; def && def->count(obj->character)) {
                for (const auto& r : def->at(obj->character).records) {
                    std::cout << " [" << int(r.states) << " #" << r.character
                              << (g_movie->shapes.count(r.character) ? " shape" : g_movie->timelines.count(r.character) ? " sprite"
                                  : g_movie->editTexts.count(r.character) ? " edittext" : " ?") << "]";
                }
            }
        }
        std::cout << " @(" << obj->matrix.tx / 20 << "," << obj->matrix.ty / 20 << ")";
        if (obj->clip) {
            std::cout << " frame " << obj->clip->currentFrame() + 1 << "/" << obj->clip->totalFrames()
                      << (obj->clip->playing() ? " playing" : " stopped");
        }
        std::cout << "\n";
        if (obj->clip) dumpClip(*obj->clip, indent + 1, maxDepth);
        if (std::getenv("FP_DUMP_BUTTONS")) {
            for (const auto& bc : obj->buttonClips) if (bc) dumpClip(*bc, indent + 2, maxDepth);
        }
    }
}

void saveCapture(SDL_Renderer* sdl, const std::string& path, int w, int h) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_RenderReadPixels(sdl, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
        SDL_SaveBMP(s, path.c_str());
        std::cout << "Captured " << path << "\n";
    } else {
        std::cerr << "capture failed: " << SDL_GetError() << "\n";
    }
    if (s) SDL_FreeSurface(s);
}

const char* kHelp =
    "Game input: keyboard and mouse go to the game.\n"
    "Debug keys:\n"
    "  F2           pause / resume\n"
    "  F3           step one frame (pauses)\n"
    "  F4           restart the movie\n"
    "  F5           ignore stop() calls (lets every timeline run)\n"
    "  F6           toggle mask rendering\n"
    "  F7           symbol viewer on/off; PgDn / PgUp = next / previous symbol\n"
    "  Esc          leave the viewer, or quit\n";

} // namespace

int main(int argc, char** argv) {
    using generated::GameMetadata;
    const auto opts = parseOptions(argc, argv);
    fp::Movie movie;
    g_movie = &movie;
    std::string error;
    if (!movie.load(opts.pack, error)) {
        const std::string msg = "Cannot load movie pack:\n" + opts.pack + "\n\n" + error;
        std::cerr << msg << "\n";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FlashPort", msg.c_str(), nullptr);
        return 1;
    }
    std::cout << "FlashPort runtime - " << GameMetadata::sourceFile() << "\n"
              << "Stage " << movie.stageWidth() << "x" << movie.stageHeight() << " @ " << movie.fps << " fps, "
              << movie.shapes.size() << " shapes, " << movie.bitmaps.size() << " textures, "
              << movie.timelines.size() << " timelines, scripts " << (movie.runScripts ? "executed (AVM1)" : "not executed")
              << "\n\n" << kHelp;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    const bool capturing = !opts.capture.empty();
    const int W = std::max(1, movie.stageWidth()), H = std::max(1, movie.stageHeight());
    SDL_Window* window = SDL_CreateWindow("FlashPort", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                          capturing ? SDL_WINDOW_HIDDEN
                                                    : (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI));
    if (!window) { std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n"; SDL_Quit(); return 1; }
    SDL_Renderer* sdl = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | (capturing ? 0 : SDL_RENDERER_PRESENTVSYNC));
    if (!sdl) sdl = SDL_CreateRenderer(window, -1, 0);
    if (!sdl) { std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n"; SDL_Quit(); return 1; }
    SDL_RenderSetLogicalSize(sdl, W, H);
    // Capture mode renders into a stage-sized texture so results do not depend on the window.
    SDL_Texture* captureTarget = nullptr;
    if (capturing) {
        captureTarget = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W, H);
        if (captureTarget) SDL_SetRenderTarget(sdl, captureTarget);
    }

    auto player = std::make_unique<fp::Player>(movie);
    player->ignoreStops = opts.ignoreStops;
    player->virtualClock = capturing;
    player->audio.open(!capturing);
    player->dataRoot = (std::filesystem::path(opts.pack).parent_path() / "data").string();
    if (!opts.audioOut.empty()) player->audio.record();
    player->start();
    fp::Renderer renderer(sdl, movie, W, H);
    const auto bindRenderer = [&renderer](fp::Player& p) {
        p.renderObject = [&renderer](const fp::DisplayObject& o, const fp::Matrix& m, int w, int h, std::vector<std::uint32_t>& out) {
            return renderer.renderToPixels(o, m, w, h, out);
        };
    };
    bindRenderer(*player);
    // Stage coordinates (twips, origin at the frame rect) -> window pixels.
    const fp::Matrix stageMatrix{1.0f / 20, 0, 0, 1.0f / 20, -movie.stage[0] / 20.0f, -movie.stage[2] / 20.0f};

    // Symbol viewer: named symbols first, then every other sprite.
    std::vector<std::pair<std::uint32_t, std::string>> viewList;
    for (const auto& [id, name] : movie.symbols) {
        if (id != 0 && (movie.timelines.count(id) || movie.shapes.count(id))) viewList.emplace_back(id, name);
    }
    for (const auto& [id, tl] : movie.timelines) {
        if (id != 0 && std::none_of(viewList.begin(), viewList.end(), [&](const auto& v) { return v.first == id; })) {
            viewList.emplace_back(id, "sprite " + std::to_string(id));
        }
    }
    bool viewer = false;
    std::size_t viewIndex = 0;
    std::unique_ptr<fp::Clip> viewClip;
    fp::TimelineDef wrapper; // one-frame timeline placing a bare shape
    fp::Matrix viewMatrix;
    auto selectSymbol = [&](std::size_t index) {
        if (viewList.empty()) return;
        viewIndex = index % viewList.size();
        const auto id = viewList[viewIndex].first;
        viewClip.reset();
        if (auto it = movie.timelines.find(id); it != movie.timelines.end()) {
            viewClip = std::make_unique<fp::Clip>(*player, it->second, id, nullptr);
        } else {
            wrapper = fp::TimelineDef{};
            wrapper.frames.resize(1);
            fp::PlaceCmd cmd;
            cmd.type = 1;
            cmd.depth = 1;
            cmd.flags = 0x01;
            cmd.character = static_cast<std::uint16_t>(id);
            wrapper.frames[0].cmds = {cmd};
            viewClip = std::make_unique<fp::Clip>(*player, wrapper, id, nullptr);
        }
        float b[4];
        if (!renderer.bounds(*viewClip, fp::Matrix{}, b) || b[2] <= b[0] || b[3] <= b[1]) {
            b[0] = b[1] = -400; b[2] = b[3] = 400;
        }
        const float bw = (b[2] - b[0]) / 20, bh = (b[3] - b[1]) / 20;
        const float k = std::min({W * 0.85f / bw, H * 0.85f / bh, 4.0f});
        viewMatrix = {k / 20, 0, 0, k / 20, W / 2.0f - k * (b[0] + b[2]) / 40, H / 2.0f - k * (b[1] + b[3]) / 40};
    };
    if (opts.viewId >= 0) {
        for (std::size_t i = 0; i < viewList.size(); ++i) if (static_cast<int>(viewList[i].first) == opts.viewId) { viewer = true; selectSymbol(i); }
        if (!viewer) { viewList.emplace_back(static_cast<std::uint32_t>(opts.viewId), "character " + std::to_string(opts.viewId)); viewer = true; selectSymbol(viewList.size() - 1); }
    }
    if (opts.viewSymbol >= 0) {
        viewer = true;
        selectSymbol(static_cast<std::size_t>(opts.viewSymbol));
    }

    bool running = true, paused = false;
    const double frameTime = 1.0 / movie.fps;
    double accumulator = 0;
    auto last = SDL_GetPerformanceCounter();
    auto step = [&] {
        if (viewer && viewClip) { ++player->tick; viewClip->advance(player->tick); }
        else player->step();
        // Scripted input for automated runs.
        for (const auto& in : opts.input) {
            if (in.tick != player->tick) continue;
            if (in.kind == "click") { player->mouseMove(in.x, in.y); player->mouseButton(true); player->mouseButton(false); }
            else if (in.kind == "move") player->mouseMove(in.x, in.y);
            else if (in.kind == "down") player->keyEvent(in.key, true);
            else if (in.kind == "up") player->keyEvent(in.key, false);
            else if (in.kind == "key") { player->keyEvent(in.key, true); player->keyEvent(in.key, false); }
        }
    };
    Uint32 lastTitle = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_MOUSEMOTION: if (!viewer) player->mouseMove(static_cast<float>(e.motion.x), static_cast<float>(e.motion.y)); break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    if (!viewer && e.button.button == SDL_BUTTON_LEFT) {
                        player->mouseMove(static_cast<float>(e.button.x), static_cast<float>(e.button.y));
                        player->mouseButton(e.type == SDL_MOUSEBUTTONDOWN);
                    }
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    const bool down = e.type == SDL_KEYDOWN;
                    const auto sym = e.key.keysym.sym;
                    if (down) {
                        bool handled = true;
                        switch (sym) {
                            case SDLK_ESCAPE: if (viewer) viewer = false; else running = false; break;
                            case SDLK_F2: paused = !paused; break;
                            case SDLK_F3: paused = true; step(); break;
                            case SDLK_F4:
                                viewClip.reset();
                                player = std::make_unique<fp::Player>(movie);
                                player->audio.open(!capturing);
    player->dataRoot = (std::filesystem::path(opts.pack).parent_path() / "data").string();
                                bindRenderer(*player);
                                player->start();
                                break;
                            case SDLK_F5: player->ignoreStops = !player->ignoreStops; if (player->root && player->ignoreStops) player->root->setPlaying(true); break;
                            case SDLK_F6: renderer.masksEnabled = !renderer.masksEnabled; break;
                            case SDLK_F7: viewer = !viewer; if (viewer && !viewClip) selectSymbol(0); break;
                            case SDLK_PAGEDOWN: if (viewer) selectSymbol(viewIndex + 1); else handled = false; break;
                            case SDLK_PAGEUP: if (viewer) selectSymbol(viewIndex + viewList.size() - 1); else handled = false; break;
                            default: handled = false; break;
                        }
                        if (handled) break;
                    }
                    if (!viewer) player->keyEvent(fp::avm1::flashKeyCode(sym), down);
                    break;
                }
                default: break;
            }
        }

        const auto now = SDL_GetPerformanceCounter();
        accumulator += double(now - last) / double(SDL_GetPerformanceFrequency());
        last = now;
        if (paused || capturing) accumulator = 0;
        if (capturing && player->tick < static_cast<std::uint64_t>(opts.captureAfter)) step(); // deterministic stepping
        for (int n = 0; accumulator >= frameTime && n < 4; ++n) { step(); accumulator -= frameTime; }
        if (accumulator > frameTime) accumulator = 0; // too slow: drop frames instead of spiralling

        SDL_SetRenderDrawColor(sdl, 0, 0, 0, 255);
        SDL_RenderClear(sdl);
        SDL_SetRenderDrawColor(sdl, movie.background.r, movie.background.g, movie.background.b, 255);
        SDL_Rect stageRect{0, 0, W, H};
        SDL_RenderFillRect(sdl, &stageRect);
        renderer.trianglesDrawn = 0;
        if (viewer && viewClip) renderer.drawClip(*viewClip, viewMatrix, fp::ColorTransform{});
        else if (player->root) renderer.drawClip(*player->root, stageMatrix, fp::ColorTransform{});
        if (capturing && player->tick >= static_cast<std::uint64_t>(opts.captureAfter)) {
            if (opts.dump && player->root) {
                std::cout << "main timeline frame " << player->root->currentFrame() + 1 << "/" << player->root->totalFrames() << "\n";
                dumpClip(*player->root, 0, std::getenv("FP_DUMP_DEPTH") ? std::atoi(std::getenv("FP_DUMP_DEPTH")) : 3);
            }
            saveCapture(sdl, opts.capture, W, H);
            if (!opts.audioOut.empty() && !player->audio.saveWav(opts.audioOut)) std::cerr << "cannot write " << opts.audioOut << "\n";
            running = false;
        }
        SDL_RenderPresent(sdl);

        if (!capturing && SDL_GetTicks() - lastTitle > 250) {
            lastTitle = SDL_GetTicks();
            char title[256];
            if (viewer && viewClip) {
                std::snprintf(title, sizeof title, "[viewer %zu/%zu] %s  frame %d/%d%s", viewIndex + 1, viewList.size(),
                              viewList[viewIndex].second.c_str(), viewClip->currentFrame() + 1, viewClip->totalFrames(),
                              paused ? "  (paused)" : "");
            } else {
                std::snprintf(title, sizeof title, "%s  frame %d/%d%s%s", GameMetadata::sourceFile(),
                              player->root ? player->root->currentFrame() + 1 : 0, player->root ? player->root->totalFrames() : 0,
                              paused ? "  (paused)" : "", player->ignoreStops ? "  [ignoring stop()]" : "");
            }
            SDL_SetWindowTitle(window, title);
        }
    }

    if (player->vm) {
        std::cout << "AVM1: " << player->vm->instructions << " actions executed, " << player->vm->errors << " script errors\n";
    }
    viewClip.reset();
    player.reset();
    SDL_DestroyRenderer(sdl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
