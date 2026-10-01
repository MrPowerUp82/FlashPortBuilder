#include "flashport/MoviePack.hpp"
#include "flashport/AssetExtractor.hpp"
#include "flashport/Audio.hpp"
#include "flashport/BitReader.hpp"
#include "flashport/ByteReader.hpp"
#include "flashport/FrameScripts.hpp"
#include "flashport/SWFStructures.hpp"
#include "flashport/Shape.hpp"
#include "flashport/Tessellator.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <zlib.h>

namespace flashport {
namespace {

constexpr std::uint32_t kPackVersion = 6;
constexpr std::uint32_t kGradientTextureBase = 0x10000; // above any SWF character id

class PackWriter {
public:
    void u8(std::uint8_t v) { buf.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void str(const std::string& s) {
        const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xffff));
        u16(n);
        buf.insert(buf.end(), s.begin(), s.begin() + n);
    }
    void bytes(const std::vector<std::uint8_t>& b) { buf.insert(buf.end(), b.begin(), b.end()); }
    void matrix(const Matrix& m) {
        for (double v : {m.a, m.b, m.c, m.d, m.tx, m.ty}) f32(static_cast<float>(v));
    }
    void cxform(const ColorTransform& c) {
        for (double v : {c.rMul, c.gMul, c.bMul, c.aMul}) f32(static_cast<float>(v));
        for (auto v : {c.rAdd, c.gAdd, c.bAdd, c.aAdd}) i16(static_cast<std::int16_t>(v));
    }
    std::vector<std::uint8_t> buf;
};

std::vector<std::uint8_t> deflate(const std::vector<std::uint8_t>& raw) {
    uLongf len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(len);
    if (compress2(z.data(), &len, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        throw std::runtime_error("deflate failed");
    }
    z.resize(len);
    return z;
}

void writeBitmap(PackWriter& w, std::uint32_t id, const ImageRGBA& img) {
    w.u32(id);
    w.u16(static_cast<std::uint16_t>(img.width));
    w.u16(static_cast<std::uint16_t>(img.height));
    const auto z = deflate(img.pixels);
    w.u32(static_cast<std::uint32_t>(z.size()));
    w.bytes(z);
}

struct PlaceCmd {
    std::uint8_t type{}; // 1 place/move, 2 remove
    PlaceObject place;
    std::uint16_t depth{};
};

struct ActionBlock {
    std::size_t offset{}, length{}; // AVM1 bytecode inside SWFDocument::data
};

struct InitAction {
    std::uint16_t sprite{};
    ActionBlock code;
};

struct Frame {
    std::string label;
    std::vector<PlaceCmd> cmds;
    std::vector<FrameAction> actions;  // recognised timeline calls (used when scripts are not run)
    std::vector<ActionBlock> scripts;  // DoAction, in tag order
    std::vector<InitAction> initScripts; // DoInitAction
};

struct ButtonRecord {
    std::uint8_t states{}; // 0x01 up, 0x02 over, 0x04 down, 0x08 hit test
    std::uint16_t character{}, depth{};
    Matrix matrix;
    ColorTransform cxform;
};

struct ButtonCondAction {
    std::uint16_t conditions{}; // BUTTONCONDACTION flags as stored (little-endian u16)
    ActionBlock code;
};

struct Button {
    bool trackAsMenu = false;
    std::vector<ButtonRecord> records;
    std::vector<ButtonCondAction> actions;
};

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// DefineButton / DefineButton2: every state record plus the condition actions.
// DefineButton (v1) has a single action list that runs on release (OverDownToOverUp).
Button parseButton(const SWFDocument& doc, const TagRecord& t) {
    Button b;
    const std::size_t end = t.offset + t.length;
    std::size_t pos = t.offset + 2;
    std::size_t actionStart = 0;
    if (t.code == 34) {
        b.trackAsMenu = (doc.data[pos] & 0x01) != 0;
        const auto actionOffset = le16(&doc.data[pos + 1]);
        if (actionOffset) actionStart = pos + 1 + actionOffset;
        pos += 3;
    }
    while (pos < end) {
        const auto flags = doc.data[pos++];
        if (flags == 0) break;
        if (pos + 4 > end) throw std::runtime_error("button record truncated");
        ButtonRecord r;
        r.states = flags & 0x0f;
        r.character = le16(&doc.data[pos]);
        r.depth = le16(&doc.data[pos + 2]);
        pos += 4;
        BitReader bits(doc.data.data(), end, pos);
        r.matrix = readMatrix(bits);
        if (t.code == 34) r.cxform = readColorTransform(bits, true);
        pos = bits.nextByte();
        if (t.code == 34 && (flags & 0x10)) {
            std::uint8_t n = 0;
            pos = skipFilterList(doc, pos, end, n);
        }
        if (t.code == 34 && (flags & 0x20)) ++pos; // blend mode
        b.records.push_back(r);
    }
    if (t.code == 7) {
        if (pos < end) b.actions.push_back({0x0008, {pos, end - pos}});
    } else if (actionStart) {
        pos = actionStart;
        while (pos + 4 <= end) {
            const auto size = le16(&doc.data[pos]);
            const auto cond = le16(&doc.data[pos + 2]);
            const std::size_t codeEnd = size == 0 ? end : std::min(end, pos + size);
            if (codeEnd > pos + 4) b.actions.push_back({cond, {pos + 4, codeEnd - pos - 4}});
            if (size == 0) break;
            pos += size;
        }
    }
    std::stable_sort(b.records.begin(), b.records.end(), [](const ButtonRecord& x, const ButtonRecord& y) { return x.depth < y.depth; });
    return b;
}

// ---- sound ----

struct SoundPlay {
    std::uint8_t flags{};          // 0x01 stop, 0x02 no multiple, 0x04 stream
    std::uint32_t inPoint{}, outPoint{};
    std::uint16_t loops = 1;
    bool hasIn = false, hasOut = false;
    struct Env { std::uint32_t pos; std::uint16_t left, right; };
    std::vector<Env> env;          // volume envelope (positions in 44.1 kHz samples, levels 0..32768)
};

SoundPlay readSoundInfo(ByteReader& r) {
    SoundPlay p;
    const auto f = r.u8();
    if (f & 0x20) p.flags |= 0x01;
    if (f & 0x10) p.flags |= 0x02;
    if (f & 0x01) { p.inPoint = r.u32(); p.hasIn = true; }
    if (f & 0x02) { p.outPoint = r.u32(); p.hasOut = true; }
    if (f & 0x04) p.loops = r.u16();
    if (f & 0x08) {
        const auto n = r.u8();
        for (unsigned i = 0; i < n; ++i) {
            SoundPlay::Env e;
            e.pos = r.u32();
            e.left = r.u16();
            e.right = r.u16();
            p.env.push_back(e);
        }
    }
    return p;
}

void writeSoundPlay(PackWriter& w, const SoundPlay& p) {
    w.u8(static_cast<std::uint8_t>(p.flags | (p.hasIn ? 0x08 : 0) | (p.hasOut ? 0x10 : 0)));
    w.u32(p.inPoint);
    w.u32(p.outPoint);
    w.u16(p.loops);
    w.u8(static_cast<std::uint8_t>(p.env.size()));
    for (const auto& e : p.env) { w.u32(e.pos); w.u16(e.left); w.u16(e.right); }
}

struct SoundStart { std::uint32_t timeline{}, frame{}, sound{}; SoundPlay play; };
struct ButtonSounds { std::uint16_t button{}; std::uint16_t sound[4]{}; SoundPlay play[4]; };

constexpr std::uint32_t kStreamSoundBase = 0x10000; // virtual sound id of a timeline's stream: base + timeline

// Decodes every DefineSound and stream to PCM and collects StartSound / DefineButtonSound records.
struct AudioPack {
    std::map<std::uint32_t, PCMSound> sounds;
    std::vector<SoundStart> starts;
    std::vector<ButtonSounds> buttons;
};

AudioPack collectAudio(const SWFDocument& doc, MoviePackReport& r) {
    AudioPack a;
    static const unsigned kRates[4] = {5512, 11025, 22050, 44100};
    struct Stream {
        unsigned format{}, rate{};
        bool is16{}, stereo{};
        std::vector<std::uint8_t> data;
        std::uint32_t firstFrame = 0;
        bool started = false;
    };
    std::map<std::uint16_t, Stream> streams;
    for (const auto& t : doc.tags) {
        try {
            const auto* p = doc.payload(t);
            switch (t.code) {
                case 14: { // DefineSound
                    if (t.length < 7) break;
                    const auto id = le16(p);
                    const unsigned format = p[2] >> 4;
                    std::size_t start = 7;
                    unsigned seek = 0;
                    if (format == 2 && t.length >= 9) {
                        seek = static_cast<unsigned>(std::max<int>(0, static_cast<std::int16_t>(le16(p + 7))));
                        start = 9;
                    }
                    PCMSound pcm;
                    std::string err;
                    if (decodeSwfSound(format, kRates[(p[2] >> 2) & 3], (p[2] & 2) != 0, (p[2] & 1) != 0, p + start,
                                       t.length - start, seek, pcm, &err)) {
                        a.sounds[id] = std::move(pcm);
                    } else {
                        r.problem("sound " + std::to_string(id) + ": " + err);
                    }
                    break;
                }
                case 15: { // StartSound
                    ByteReader br(doc.data, t.offset);
                    SoundStart s;
                    s.sound = br.u16();
                    s.timeline = t.spriteId;
                    s.frame = t.frame;
                    s.play = readSoundInfo(br);
                    a.starts.push_back(s);
                    break;
                }
                case 17: { // DefineButtonSound
                    ByteReader br(doc.data, t.offset);
                    ButtonSounds b;
                    b.button = br.u16();
                    for (int i = 0; i < 4; ++i) {
                        b.sound[i] = br.u16();
                        if (b.sound[i]) b.play[i] = readSoundInfo(br);
                    }
                    a.buttons.push_back(b);
                    break;
                }
                case 18: case 45: { // SoundStreamHead(2)
                    if (t.length < 4) break;
                    Stream s;
                    s.format = p[1] >> 4;
                    s.rate = kRates[(p[1] >> 2) & 3];
                    s.is16 = (p[1] & 2) != 0;
                    s.stereo = (p[1] & 1) != 0;
                    streams[t.spriteId] = std::move(s);
                    break;
                }
                case 19: { // SoundStreamBlock
                    auto it = streams.find(t.spriteId);
                    if (it == streams.end()) break;
                    auto& s = it->second;
                    if (!s.started) { s.started = true; s.firstFrame = t.frame; }
                    const std::size_t skip = s.format == 2 ? 4 : 0; // MP3: sampleCount + seekSamples
                    if (t.length > skip) s.data.insert(s.data.end(), p + skip, p + t.length);
                    break;
                }
                default: break;
            }
        } catch (const std::exception& e) {
            r.problem("sound tag " + std::to_string(t.code) + ": " + e.what());
        }
    }
    for (auto& [timeline, s] : streams) {
        if (s.data.empty()) continue;
        PCMSound pcm;
        std::string err;
        if (!decodeSwfSound(s.format, s.rate, s.is16, s.stereo, s.data.data(), s.data.size(), 0, pcm, &err)) {
            r.problem("stream sound in timeline " + std::to_string(timeline) + ": " + err);
            continue;
        }
        const auto id = kStreamSoundBase + timeline;
        a.sounds[id] = std::move(pcm);
        SoundStart st;
        st.timeline = timeline;
        st.frame = s.firstFrame;
        st.sound = id;
        st.play.flags = 0x04;
        a.starts.push_back(st);
    }
    return a;
}

void writeAudio(PackWriter& w, const AudioPack& a, MoviePackReport& r) {
    w.u32(static_cast<std::uint32_t>(a.sounds.size()));
    for (const auto& [id, snd] : a.sounds) {
        // Stereo with identical channels is stored as mono.
        std::vector<std::int16_t> samples = snd.samples;
        unsigned channels = snd.channels;
        if (channels == 2) {
            bool same = true;
            for (std::size_t i = 0; i + 1 < samples.size() && same; i += 2) same = samples[i] == samples[i + 1];
            if (same) {
                for (std::size_t i = 0; i < samples.size() / 2; ++i) samples[i] = samples[2 * i];
                samples.resize(samples.size() / 2);
                channels = 1;
            }
        }
        std::vector<std::uint8_t> raw(samples.size() * 2);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            raw[2 * i] = static_cast<std::uint8_t>(samples[i] & 0xff);
            raw[2 * i + 1] = static_cast<std::uint8_t>((samples[i] >> 8) & 0xff);
        }
        const auto z = deflate(raw);
        w.u32(id);
        w.u32(snd.rate);
        w.u8(static_cast<std::uint8_t>(channels));
        w.u32(static_cast<std::uint32_t>(samples.size() / channels));
        w.u32(static_cast<std::uint32_t>(z.size()));
        w.bytes(z);
        ++r.sounds;
    }
    std::uint32_t starts = 0;
    PackWriter sw;
    for (const auto& s : a.starts) {
        if (!a.sounds.count(s.sound)) continue;
        sw.u32(s.timeline);
        sw.u32(s.frame);
        sw.u32(s.sound);
        writeSoundPlay(sw, s.play);
        ++starts;
    }
    w.u32(starts);
    w.bytes(sw.buf);
    w.u32(static_cast<std::uint32_t>(a.buttons.size()));
    for (const auto& b : a.buttons) {
        w.u32(b.button);
        for (int i = 0; i < 4; ++i) {
            w.u16(a.sounds.count(b.sound[i]) ? b.sound[i] : 0);
            writeSoundPlay(w, b.play[i]);
        }
    }
}

} // namespace

void writeMoviePack(const SWFDocument& doc, const std::string& path, MoviePackReport& r) {
    // ---- bitmaps ----
    std::map<std::uint16_t, BitmapInfo> bitmapInfo;
    std::map<std::uint32_t, ImageRGBA> images;
    const std::uint8_t* jpegTables = nullptr;
    std::size_t jpegTablesLen = 0;
    RGBA background{255, 255, 255, 255};
    for (const auto& t : doc.tags) {
        if (t.code == 8) { jpegTables = doc.payload(t); jpegTablesLen = t.length; continue; }
        if (t.code == 9 && t.length >= 3) { background = {doc.payload(t)[0], doc.payload(t)[1], doc.payload(t)[2], 255}; continue; }
        if (t.code != 6 && t.code != 21 && t.code != 35 && t.code != 90 && t.code != 20 && t.code != 36) continue;
        if (t.length < 2) continue;
        const auto id = le16(doc.payload(t));
        try {
            auto bm = decodeBitmapTag(doc, t, jpegTables, jpegTablesLen);
            if (!bm.decoded) { r.problem("bitmap " + std::to_string(id) + " not decoded" + (bm.error.empty() ? "" : ": " + bm.error)); continue; }
            bitmapInfo[id] = {bm.width, bm.height, ""};
            images[id] = std::move(bm.image);
        } catch (const std::exception& e) {
            r.problem("bitmap " + std::to_string(id) + ": " + e.what());
        }
    }

    // ---- shapes ----
    GradientAtlas gradients(kGradientTextureBase);
    PackWriter shapesOut;
    std::uint32_t shapeCount = 0;
    const auto emitShapeBody = [&](const Shape& shape, const std::vector<Mesh>& meshes) {
        for (auto v : {shape.bounds.xmin, shape.bounds.ymin, shape.bounds.xmax, shape.bounds.ymax}) shapesOut.i32(v);
        shapesOut.u32(static_cast<std::uint32_t>(meshes.size()));
        for (const auto& m : meshes) {
            shapesOut.i32(m.texture);
            shapesOut.u32(static_cast<std::uint32_t>(m.vertices.size()));
            for (const auto& v : m.vertices) {
                shapesOut.f32(v.x);
                shapesOut.f32(v.y);
                for (auto c : v.rgba) shapesOut.u8(c);
                shapesOut.f32(v.u);
                shapesOut.f32(v.v);
            }
            shapesOut.u32(static_cast<std::uint32_t>(m.indices.size()));
            for (auto i : m.indices) shapesOut.u32(i);
            ++r.meshes;
            r.triangles += m.indices.size() / 3;
        }
        ++shapeCount;
    };
    const auto emitShape = [&](const Shape& shape, const std::vector<Mesh>& meshes) {
        shapesOut.u32(shape.id);
        emitShapeBody(shape, meshes);
    };

    // Static text becomes an ordinary shape built from its fonts' glyph outlines.
    std::map<std::uint16_t, Font> fonts;
    for (const auto& t : doc.tags) {
        if (t.code != 10 && t.code != 48 && t.code != 75) continue;
        try {
            auto font = parseFont(doc, t);
            fonts[font.id] = std::move(font);
        } catch (const std::exception& e) {
            r.problem("font " + std::to_string(le16(doc.payload(t))) + ": " + e.what());
        }
    }
    for (const auto& t : doc.tags) {
        if (t.code != 11 && t.code != 33) continue;
        try {
            const auto text = parseText(doc, t, fonts);
            emitShape(text, tessellateShape(text, bitmapInfo, gradients));
            ++r.texts;
        } catch (const std::exception& e) {
            r.problem("text " + std::to_string(le16(doc.payload(t))) + ": " + e.what());
        }
    }

    for (const auto& t : doc.tags) {
        if (t.code != 2 && t.code != 22 && t.code != 32 && t.code != 83) continue;
        try {
            static const bool trace = std::getenv("FLASHPORT_TRACE_SHAPES") != nullptr;
            if (trace) std::fprintf(stderr, "shape %u\n", le16(doc.payload(t)));
            const auto started = std::chrono::steady_clock::now();
            const auto shape = parseShape(doc, t);
            const auto meshes = tessellateShape(shape, bitmapInfo, gradients);
            MoviePackReport::HeavyShape heavy{shape.id, 0,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()};
            for (const auto& m : meshes) heavy.triangles += m.indices.size() / 3;
            if (trace) std::fprintf(stderr, "tris %u %llu %.1f\n", heavy.id, static_cast<unsigned long long>(heavy.triangles), heavy.ms);
            r.heaviest.push_back(heavy);
            std::sort(r.heaviest.begin(), r.heaviest.end(), [](const auto& a, const auto& b) { return a.ms > b.ms; });
            if (r.heaviest.size() > 8) r.heaviest.pop_back();
            emitShape(shape, meshes);
        } catch (const std::exception& e) {
            r.problem("shape " + std::to_string(le16(doc.payload(t))) + ": " + e.what());
        }
    }

    const bool tracePhases = std::getenv("FLASHPORT_TRACE_SHAPES") != nullptr;
    const auto phase = [&](const char* name) { if (tracePhases) std::fprintf(stderr, "phase %s\n", name); };
    phase("timelines");
    // ---- timelines ----
    std::map<std::uint16_t, std::vector<Frame>> timelines;
    timelines[0].resize(std::max<std::size_t>(1, doc.frameCount));
    for (const auto& t : doc.tags) {
        if (t.code == 39 && t.length >= 4) {
            const auto id = le16(doc.payload(t));
            timelines[id].resize(std::max<std::size_t>(1, le16(doc.payload(t) + 2)));
        }
    }
    auto frameAt = [&](std::uint16_t sprite, std::uint32_t frame) -> Frame* {
        auto& frames = timelines[sprite];
        if (frame >= frames.size()) frames.resize(frame + 1); // more ShowFrames than declared
        return &frames[frame];
    };
    for (const auto& t : doc.tags) {
        try {
            switch (t.code) {
                case 4: case 26: case 70: {
                    PlaceCmd c;
                    c.type = 1;
                    c.place = parsePlaceObject(doc, t);
                    frameAt(t.spriteId, t.frame)->cmds.push_back(std::move(c));
                    ++r.placeCommands;
                    break;
                }
                case 5: case 28: { // RemoveObject(2)
                    PlaceCmd c;
                    c.type = 2;
                    c.depth = le16(doc.payload(t) + (t.code == 5 ? 2 : 0));
                    frameAt(t.spriteId, t.frame)->cmds.push_back(c);
                    break;
                }
                case 12: // DoAction
                    frameAt(t.spriteId, t.frame)->scripts.push_back({t.offset, t.length});
                    ++r.actionBlocks;
                    break;
                case 59: // DoInitAction: u16 sprite id, then actions
                    if (t.length >= 2) {
                        frameAt(t.spriteId, t.frame)->initScripts.push_back({le16(doc.payload(t)), {t.offset + 2, t.length - 2u}});
                        ++r.actionBlocks;
                    }
                    break;
                case 43: { // FrameLabel
                    std::string label;
                    for (std::size_t i = 0; i < t.length && doc.payload(t)[i]; ++i) label.push_back(static_cast<char>(doc.payload(t)[i]));
                    frameAt(t.spriteId, t.frame)->label = label;
                    break;
                }
                default: break;
            }
        } catch (const std::exception& e) {
            r.problem("tag " + std::to_string(t.code) + " in timeline " + std::to_string(t.spriteId) + ": " + e.what());
        }
    }
    phase("frame scripts");
    const auto scripts = extractFrameScripts(doc);
    phase("frame scripts done");
    r.frameScripts = scripts.avm1Scripts + scripts.avm2Scripts;
    for (const auto& [key, acts] : scripts.actions) {
        auto it = timelines.find(key.first);
        if (it == timelines.end() || key.second >= it->second.size()) continue;
        auto& dst = it->second[key.second].actions;
        dst.insert(dst.end(), acts.begin(), acts.end());
        r.frameActions += acts.size();
    }

    // ---- morph shapes ----
    // Shape tweens are placed with a ratio that the timeline changes frame by frame. Those
    // ratios are static, so each (morph, ratio) pair is tessellated here. Pack id: the morph id
    // for ratio 0, `id | ratio << 16` otherwise; the runtime picks the closest ratio.
    phase("morph shapes");
    std::map<std::uint16_t, const TagRecord*> morphTags;
    for (const auto& t : doc.tags) {
        if ((t.code == 46 || t.code == 84) && t.length >= 2) morphTags[le16(doc.payload(t))] = &t;
    }
    if (!morphTags.empty()) {
        std::map<std::uint16_t, std::set<std::uint16_t>> morphRatios;
        for (const auto& [id, t] : morphTags) morphRatios[id].insert(0);
        for (const auto& [id, frames] : timelines) {
            std::map<std::uint16_t, std::uint16_t> atDepth; // depth -> character
            for (const auto& f : frames) {
                for (const auto& c : f.cmds) {
                    if (c.type == 2) { atDepth.erase(c.depth); continue; }
                    if (c.place.characterId) atDepth[c.place.depth] = *c.place.characterId;
                    const auto it = atDepth.find(c.place.depth);
                    if (it == atDepth.end() || !morphTags.count(it->second)) continue;
                    morphRatios[it->second].insert(c.place.ratio.value_or(0));
                }
            }
        }
        for (const auto& [id, ratios] : morphRatios) {
            for (const auto ratio : ratios) {
                try {
                    auto shape = parseMorphShape(doc, *morphTags[id], ratio);
                    const auto meshes = tessellateShape(shape, bitmapInfo, gradients);
                    shapesOut.u32(static_cast<std::uint32_t>(id) | (static_cast<std::uint32_t>(ratio) << 16));
                    emitShapeBody(shape, meshes);
                    ++r.morphFrames;
                } catch (const std::exception& e) {
                    r.problem("morph shape " + std::to_string(id) + " ratio " + std::to_string(ratio) + ": " + e.what());
                    break;
                }
            }
        }
        r.morphShapes = morphTags.size();
    }

    r.shapes = shapeCount;

    // ---- buttons and symbols ----
    std::map<std::uint16_t, Button> buttons;
    std::vector<std::pair<std::uint16_t, std::string>> symbols;
    bool hasAVM1 = r.actionBlocks > 0;
    for (const auto& [id, frames] : timelines) {
        for (const auto& f : frames) {
            for (const auto& c : f.cmds) hasAVM1 = hasAVM1 || !c.place.clipActions.empty();
        }
    }
    for (const auto& t : doc.tags) {
        try {
            if (t.code == 7 || t.code == 34) {
                buttons[le16(doc.payload(t))] = parseButton(doc, t);
                if (!buttons[le16(doc.payload(t))].actions.empty()) hasAVM1 = true;
            }
            if (t.code == 56 || t.code == 76) {
                ByteReader br(doc.data, t.offset);
                const auto count = br.u16();
                for (std::uint16_t i = 0; i < count && br.pos() < t.offset + t.length; ++i) {
                    const auto id = br.u16();
                    symbols.emplace_back(id, br.cstring());
                }
            }
        } catch (const std::exception& e) {
            r.problem("tag " + std::to_string(t.code) + ": " + e.what());
        }
    }

    phase("write");
    // ---- write ----
    PackWriter w;
    for (char c : {'F', 'P', 'K', '1'}) w.u8(static_cast<std::uint8_t>(c));
    w.u32(kPackVersion);
    for (auto v : {doc.frameRect.xmin, doc.frameRect.xmax, doc.frameRect.ymin, doc.frameRect.ymax}) w.i32(v);
    w.f32(static_cast<float>(doc.fps));
    w.u8(background.r); w.u8(background.g); w.u8(background.b);
    w.u8(doc.version);
    // Scripts are executed only for AVM1 movies; AS3 movies keep the recognised frame actions.
    const bool usesAVM2 = std::any_of(doc.tags.begin(), doc.tags.end(), [](const TagRecord& t) { return t.code == 82 || t.code == 72; });
    // 1 = run AVM1 scripts, 2 = run AVM2 (ABC blocks follow at the end of the pack), 0 = none.
    w.u8(usesAVM2 ? 2 : hasAVM1 ? 1 : 0);
    const auto code = [&](const ActionBlock& b) {
        w.u32(static_cast<std::uint32_t>(b.length));
        w.buf.insert(w.buf.end(), doc.data.begin() + static_cast<std::ptrdiff_t>(b.offset),
                     doc.data.begin() + static_cast<std::ptrdiff_t>(b.offset + b.length));
    };

    w.u32(static_cast<std::uint32_t>(images.size() + gradients.textures().size()));
    for (const auto& [id, img] : images) writeBitmap(w, id, img);
    for (const auto& [id, img] : gradients.textures()) writeBitmap(w, id, img);
    r.bitmaps = images.size();
    r.gradientTextures = gradients.textures().size();

    // Mesh data dominates the pack and compresses well (repeated colours, aligned floats).
    w.u32(shapeCount);
    const auto shapesZ = deflate(shapesOut.buf);
    w.u32(static_cast<std::uint32_t>(shapesOut.buf.size()));
    w.u32(static_cast<std::uint32_t>(shapesZ.size()));
    w.bytes(shapesZ);

    w.u32(static_cast<std::uint32_t>(timelines.size()));
    for (const auto& [id, frames] : timelines) {
        w.u32(id);
        w.u32(static_cast<std::uint32_t>(frames.size()));
        for (const auto& f : frames) {
            w.str(f.label);
            w.u32(static_cast<std::uint32_t>(f.cmds.size()));
            for (const auto& c : f.cmds) {
                w.u8(c.type);
                if (c.type == 2) { w.u16(c.depth); continue; }
                const auto& p = c.place;
                std::uint8_t flags = 0;
                if (p.characterId) flags |= 0x01;
                if (p.matrix) flags |= 0x02;
                if (p.colorTransform) flags |= 0x04;
                if (p.move) flags |= 0x08;
                if (p.clipDepth) flags |= 0x10;
                if (p.ratio) flags |= 0x20;
                if (p.name) flags |= 0x40;
                if (p.visible && !*p.visible) flags |= 0x80;
                w.u16(p.depth);
                w.u8(flags);
                if (p.characterId) w.u16(*p.characterId);
                if (p.matrix) w.matrix(*p.matrix);
                if (p.colorTransform) w.cxform(*p.colorTransform);
                if (p.clipDepth) w.u16(*p.clipDepth);
                if (p.ratio) w.u16(*p.ratio);
                if (p.name) w.str(*p.name);
                w.u32(static_cast<std::uint32_t>(p.clipActions.size()));
                for (const auto& ca : p.clipActions) {
                    w.u32(ca.events);
                    w.u8(ca.keyCode);
                    code({ca.actionOffset, ca.actionLength});
                    ++r.actionBlocks;
                }
            }
            w.u32(static_cast<std::uint32_t>(f.actions.size()));
            for (const auto& a : f.actions) {
                w.u8(static_cast<std::uint8_t>(a.kind));
                w.i32(a.frame);
                w.str(a.label);
            }
            w.u32(static_cast<std::uint32_t>(f.scripts.size()));
            for (const auto& s : f.scripts) code(s);
            w.u32(static_cast<std::uint32_t>(f.initScripts.size()));
            for (const auto& s : f.initScripts) { w.u16(s.sprite); code(s.code); }
            ++r.frames;
        }
        ++r.timelines;
    }

    w.u32(static_cast<std::uint32_t>(buttons.size()));
    for (const auto& [id, b] : buttons) {
        w.u32(id);
        w.u8(b.trackAsMenu ? 1 : 0);
        w.u32(static_cast<std::uint32_t>(b.records.size()));
        for (const auto& rec : b.records) {
            w.u8(rec.states);
            w.u16(rec.character);
            w.u16(rec.depth);
            w.matrix(rec.matrix);
            w.cxform(rec.cxform);
        }
        w.u32(static_cast<std::uint32_t>(b.actions.size()));
        for (const auto& a : b.actions) { w.u16(a.conditions); code(a.code); }
        ++r.buttons;
    }

    w.u32(static_cast<std::uint32_t>(symbols.size()));
    for (const auto& [id, name] : symbols) { w.u32(id); w.str(name); }

    // Fonts with outlines: one untextured mesh per glyph, in glyph units (for dynamic text).
    std::uint32_t fontCount = 0;
    PackWriter fontsOut;
    for (const auto& [id, font] : fonts) {
        if (font.glyphs.empty()) continue;
        fontsOut.u16(id);
        fontsOut.f32(static_cast<float>(font.emSquare));
        fontsOut.i16(font.ascent);
        fontsOut.i16(font.descent);
        fontsOut.i16(font.leading);
        fontsOut.u32(static_cast<std::uint32_t>(font.glyphs.size()));
        for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
            Shape glyph = font.glyphs[i];
            std::int32_t xmax = 0;
            for (auto& g : glyph.groups) {
                for (auto& f : g.fills) f.color = {255, 255, 255, 255};
                for (const auto& p : g.fillPaths) for (const auto& c : p.contours) for (const auto& e : c) xmax = std::max({xmax, e.from.x, e.to.x});
            }
            const double advance = i < font.advances.size() ? font.advances[i] : xmax + font.emSquare * 0.08;
            fontsOut.u16(i < font.codes.size() ? font.codes[i] : static_cast<std::uint16_t>(i));
            fontsOut.f32(static_cast<float>(advance));
            const auto meshes = tessellateShape(glyph, bitmapInfo, gradients);
            std::uint32_t vcount = 0, icount = 0;
            for (const auto& m : meshes) { vcount += static_cast<std::uint32_t>(m.vertices.size()); icount += static_cast<std::uint32_t>(m.indices.size()); }
            fontsOut.u32(vcount);
            for (const auto& m : meshes) for (const auto& v : m.vertices) { fontsOut.f32(v.x); fontsOut.f32(v.y); }
            fontsOut.u32(icount);
            std::uint32_t base = 0;
            for (const auto& m : meshes) {
                for (auto idx : m.indices) fontsOut.u32(base + idx);
                base += static_cast<std::uint32_t>(m.vertices.size());
            }
        }
        ++fontCount;
    }
    w.u32(fontCount);
    w.bytes(fontsOut.buf);

    std::vector<EditText> editTexts;
    for (const auto& t : doc.tags) {
        if (t.code != 37) continue;
        try {
            editTexts.push_back(parseEditText(doc, t));
        } catch (const std::exception& e) {
            r.problem("edit text " + std::to_string(le16(doc.payload(t))) + ": " + e.what());
        }
    }
    w.u32(static_cast<std::uint32_t>(editTexts.size()));
    for (const auto& e : editTexts) {
        w.u16(e.id);
        for (auto v : {e.bounds.xmin, e.bounds.ymin, e.bounds.xmax, e.bounds.ymax}) w.i32(v);
        w.u16(e.fontId);
        w.u16(e.height);
        w.u8(e.color.r); w.u8(e.color.g); w.u8(e.color.b); w.u8(e.color.a);
        w.u8(e.align);
        w.u16(e.leftMargin);
        w.u16(e.rightMargin);
        w.u16(e.indent);
        w.i16(e.leading);
        w.u8(static_cast<std::uint8_t>((e.wordWrap ? 1 : 0) | (e.multiline ? 2 : 0) | (e.password ? 4 : 0) | (e.html ? 8 : 0) |
                                       (e.border ? 16 : 0) | (e.readOnly ? 32 : 0)));
        w.str(e.variable);
        w.str(e.initialText);
        ++r.editTexts;
    }

    // AVM2: every DoABC block in tag order (u32 flags, str name, u32 size, bytes).
    std::vector<const TagRecord*> abcTags;
    for (const auto& t : doc.tags) if (t.code == 82 || t.code == 72) abcTags.push_back(&t);
    w.u32(static_cast<std::uint32_t>(abcTags.size()));
    for (const auto* t : abcTags) {
        std::size_t pos = t->offset;
        std::uint32_t flags = 0;
        std::string name;
        if (t->code == 82) {
            flags = static_cast<std::uint32_t>(doc.data[pos]) | (static_cast<std::uint32_t>(doc.data[pos + 1]) << 8) |
                    (static_cast<std::uint32_t>(doc.data[pos + 2]) << 16) | (static_cast<std::uint32_t>(doc.data[pos + 3]) << 24);
            pos += 4;
            while (pos < t->offset + t->length && doc.data[pos]) name.push_back(static_cast<char>(doc.data[pos++]));
            ++pos;
        }
        w.u32(flags);
        w.str(name);
        code({pos, t->offset + t->length - pos});
        ++r.abcBlocks;
    }

    // Sounds (decoded to PCM, zlib per sound), StartSound records and button sounds.
    phase("audio");
    writeAudio(w, collectAudio(doc, r), r);

    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
    if (!f) throw std::runtime_error("cannot write " + path);
    r.packBytes = w.buf.size();
}

void printMoviePackReport(const MoviePackReport& r, std::ostream& out) {
    out << "Movie pack: " << r.bitmaps << " bitmaps + " << r.gradientTextures << " gradient textures, " << r.shapes
        << " shapes incl. " << r.texts << " static texts and " << r.morphFrames << " frames of " << r.morphShapes
        << " morph shapes (" << r.meshes << " meshes, " << r.triangles << " triangles), " << r.timelines << " timelines / "
        << r.frames << " frames / " << r.placeCommands << " placements, " << r.buttons << " buttons, "
        << r.frameActions << " timeline actions from " << r.frameScripts << " frame scripts, "
        << r.sounds << " sounds, " << (r.packBytes / (1024 * 1024)) << " MB\n";
    if (!r.heaviest.empty()) {
        out << "Slowest shapes:";
        for (const auto& h : r.heaviest) out << " #" << h.id << " (" << h.triangles << " tris, " << static_cast<int>(h.ms) << " ms)";
        out << "\n";
    }
    if (!r.problems.empty()) {
        out << "Pack problems (first " << r.problems.size() << "):\n";
        for (const auto& p : r.problems) out << "  - " << p << "\n";
    }
}

} // namespace flashport
