#include "FlashRuntime.hpp"
#include "AVM1.hpp"
#include "AVM2.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <zlib.h>

namespace fp {
namespace {

class Reader {
public:
    explicit Reader(std::vector<std::uint8_t> data) : d_(std::move(data)) {}
    std::uint8_t u8() { need(1); return d_[p_++]; }
    std::uint16_t u16() { need(2); std::uint16_t v = static_cast<std::uint16_t>(d_[p_] | (d_[p_ + 1] << 8)); p_ += 2; return v; }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(d_[p_ + i]) << (8 * i);
        p_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() { const auto v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    std::string str() { const auto n = u16(); need(n); std::string s(reinterpret_cast<const char*>(&d_[p_]), n); p_ += n; return s; }
    void bytes(std::vector<std::uint8_t>& out, std::size_t n) { need(n); out.assign(d_.begin() + static_cast<std::ptrdiff_t>(p_), d_.begin() + static_cast<std::ptrdiff_t>(p_ + n)); p_ += n; }
    Code code() {
        auto v = std::make_shared<std::vector<std::uint8_t>>();
        bytes(*v, u32());
        return v;
    }
    Matrix matrix() { Matrix m; m.a = f32(); m.b = f32(); m.c = f32(); m.d = f32(); m.tx = f32(); m.ty = f32(); return m; }
    ColorTransform cxform() {
        ColorTransform c;
        for (float& v : c.mul) v = f32();
        for (float& v : c.add) v = i16();
        return c;
    }
    // Guard element counts against the bytes left so a corrupt pack cannot request huge allocations.
    std::uint32_t count(std::size_t minElementBytes) {
        const auto n = u32();
        if (static_cast<std::uint64_t>(n) * minElementBytes > d_.size() - p_) throw std::runtime_error("corrupt movie pack (count)");
        return n;
    }
private:
    void need(std::size_t n) const { if (p_ + n > d_.size()) throw std::runtime_error("movie pack truncated"); }
    std::vector<std::uint8_t> d_;
    std::size_t p_ = 0;
};

// Snapshot of what a depth holds, used to rebuild a frame's display list on goto.
struct SlotState {
    std::uint16_t character{};
    Matrix matrix;
    ColorTransform cxform;
    std::uint16_t clipDepth{}, ratio{};
    std::uint8_t blendMode = 0;
    std::string name;
    bool visible = true;
    std::vector<ClipActionDef> clipActions;
};

template <typename T>
void applyProperties(T& target, const PlaceCmd& cmd, bool keepTransform) {
    if ((cmd.flags & 0x02) && !keepTransform) target.matrix = cmd.matrix;
    if ((cmd.flags & 0x04) && !keepTransform) target.cxform = cmd.cxform;
    if (cmd.flags & 0x10) target.clipDepth = cmd.clipDepth;
    if (cmd.flags & 0x20) target.ratio = cmd.ratio;
    if (cmd.flags & 0x40) target.name = cmd.name;
    if (cmd.flags & 0x80) target.visible = false;
    if (cmd.flags2 & 0x01) target.blendMode = cmd.blend;
}

void unionRect(float out[4], bool& any, float x0, float y0, float x1, float y1) {
    if (!any) { out[0] = x0; out[1] = y0; out[2] = x1; out[3] = y1; any = true; return; }
    out[0] = std::min(out[0], x0); out[1] = std::min(out[1], y0);
    out[2] = std::max(out[2], x1); out[3] = std::max(out[3], y1);
}

const TimelineDef& emptyTimeline() {
    static const TimelineDef t = [] {
        TimelineDef d;
        d.frames.resize(1);
        return d;
    }();
    return t;
}

constexpr std::uint16_t kEmptyClip = 0xffff;

} // namespace

// ---------------------------------------------------------------- Movie

bool Movie::load(const std::string& path, std::string& error) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open " + path);
        Reader r(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {}));
        if (r.u8() != 'F' || r.u8() != 'P' || r.u8() != 'K' || r.u8() != '1') throw std::runtime_error("not a FlashPort movie pack");
        if (r.u32() != 7) throw std::runtime_error("unsupported movie pack version (rebuild with this FlashPortBuilder)");
        for (auto& v : stage) v = r.i32();
        fps = r.f32();
        if (!(fps > 0 && fps < 240)) fps = 24;
        background = {r.u8(), r.u8(), r.u8(), 255};
        swfVersion = r.u8();
        const auto mode = r.u8();
        runScripts = mode == 1;
        runAVM2 = mode == 2;

        for (auto n = r.count(12); n--;) {
            const auto id = r.u32();
            auto& b = bitmaps[id];
            b.width = r.u16();
            b.height = r.u16();
            r.bytes(b.zlib, r.u32());
        }

        // Shapes are stored as one zlib block: u32 shapeCount, u32 rawSize, u32 zSize, bytes.
        const auto shapeCount = r.u32();
        const auto rawSize = r.u32();
        std::vector<std::uint8_t> zShapes, rawShapes(rawSize);
        r.bytes(zShapes, r.u32());
        uLongf rawLen = rawSize;
        if (uncompress(rawShapes.data(), &rawLen, zShapes.data(), static_cast<uLong>(zShapes.size())) != Z_OK ||
            rawLen != rawSize) {
            throw std::runtime_error("corrupt shape block");
        }
        std::vector<std::uint8_t>().swap(zShapes);
        Reader sr(std::move(rawShapes));
        for (auto n = shapeCount; n--;) {
            const auto id = sr.u32();
            auto& s = shapes[id];
            for (auto& v : s.bounds) v = sr.i32();
            s.meshes.resize(sr.count(12));
            for (auto& m : s.meshes) {
                m.texture = sr.i32();
                m.vertices.resize(sr.count(20));
                for (auto& v : m.vertices) {
                    v.x = sr.f32(); v.y = sr.f32();
                    for (auto& c : v.rgba) c = sr.u8();
                    v.u = sr.f32(); v.v = sr.f32();
                }
                m.indices.resize(sr.count(4));
                for (auto& i : m.indices) {
                    i = static_cast<int>(sr.u32());
                    if (static_cast<std::size_t>(i) >= m.vertices.size()) throw std::runtime_error("corrupt mesh index");
                }
            }
        }

        for (const auto& [id, def] : shapes) {
            if (id > 0xffff) morphRatios[static_cast<std::uint16_t>(id)].push_back(static_cast<std::uint16_t>(id >> 16));
        }
        for (auto& [id, ratios] : morphRatios) {
            ratios.push_back(0);
            std::sort(ratios.begin(), ratios.end());
        }

        for (auto n = r.count(8); n--;) {
            const auto id = r.u32();
            auto& tl = timelines[id];
            tl.frames.resize(r.count(14));
            for (std::size_t fi = 0; fi < tl.frames.size(); ++fi) {
                auto& fr = tl.frames[fi];
                fr.label = r.str();
                if (!fr.label.empty()) tl.labels.emplace(fr.label, static_cast<int>(fi));
                fr.cmds.resize(r.count(3));
                for (auto& c : fr.cmds) {
                    c.type = r.u8();
                    c.depth = r.u16();
                    if (c.type != 1) continue;
                    c.flags = r.u8();
                    c.flags2 = r.u8();
                    if (c.flags & 0x01) c.character = r.u16();
                    if (c.flags & 0x02) c.matrix = r.matrix();
                    if (c.flags & 0x04) c.cxform = r.cxform();
                    if (c.flags & 0x10) c.clipDepth = r.u16();
                    if (c.flags & 0x20) c.ratio = r.u16();
                    if (c.flags & 0x40) c.name = r.str();
                    if (c.flags2 & 0x01) c.blend = r.u8();
                    c.clipActions.resize(r.count(9));
                    for (auto& ca : c.clipActions) {
                        ca.events = r.u32();
                        ca.keyCode = r.u8();
                        ca.code = r.code();
                    }
                }
                fr.actions.resize(r.count(7));
                for (auto& a : fr.actions) {
                    a.kind = r.u8();
                    a.frame = r.i32();
                    a.label = r.str();
                }
                fr.scripts.resize(r.count(4));
                for (auto& s : fr.scripts) s = r.code();
                fr.initScripts.resize(r.count(6));
                for (auto& s : fr.initScripts) {
                    s.first = r.u16();
                    s.second = r.code();
                }
            }
        }
        for (auto n = r.count(13); n--;) {
            auto& b = buttons[r.u32()];
            b.trackAsMenu = r.u8() != 0;
            b.records.resize(r.count(45));
            for (auto& rec : b.records) {
                rec.states = r.u8();
                rec.character = r.u16();
                rec.depth = r.u16();
                rec.matrix = r.matrix();
                rec.cxform = r.cxform();
            }
            b.actions.resize(r.count(6));
            for (auto& a : b.actions) {
                a.first = r.u16();
                a.second = r.code();
            }
        }
        for (auto n = r.count(6); n--;) {
            const auto id = r.u32();
            auto name = r.str();
            exports.emplace(name, static_cast<std::uint16_t>(id));
            symbols.emplace_back(id, std::move(name));
        }
        for (auto n = r.count(12); n--;) {
            const auto id = r.u16();
            auto& font = fonts[id];
            font.emSquare = r.f32();
            font.ascent = r.i16();
            font.descent = r.i16();
            font.leading = r.i16();
            font.glyphs.resize(r.count(14));
            for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
                auto& g = font.glyphs[i];
                g.code = r.u16();
                g.advance = r.f32();
                g.xy.resize(static_cast<std::size_t>(r.count(8)) * 2);
                for (auto& v : g.xy) v = r.f32();
                g.indices.resize(r.count(4));
                for (auto& idx : g.indices) {
                    idx = static_cast<int>(r.u32());
                    if (static_cast<std::size_t>(idx) * 2 >= g.xy.size()) throw std::runtime_error("corrupt glyph index");
                }
                font.byCode.emplace(g.code, i);
            }
        }
        for (auto n = r.count(30); n--;) {
            auto& t = editTexts[r.u16()];
            for (auto& v : t.bounds) v = r.i32();
            t.font = r.u16();
            t.height = r.u16();
            t.color = {r.u8(), r.u8(), r.u8(), r.u8()};
            t.align = r.u8();
            t.leftMargin = r.u16();
            t.rightMargin = r.u16();
            t.indent = r.u16();
            t.leading = r.i16();
            t.flags = r.u8();
            t.variable = r.str();
            t.text = r.str();
            if (t.flags & 8) t.text = stripHtml(t.text);
        }
        for (auto n = r.count(10); n--;) {
            AbcBlock b;
            b.flags = r.u32();
            b.name = r.str();
            b.bytes = r.code();
            abcBlocks.push_back(std::move(b));
        }
        // Sounds are the last sections of the pack.
        for (auto n = r.count(21); n--;) {
            const auto id = r.u32();
            auto& s = sounds[id];
            s.rate = r.u32();
            s.channels = r.u8();
            s.frames = r.u32();
            if (s.channels < 1 || s.channels > 2) throw std::runtime_error("corrupt sound");
            s.z.resize(r.count(1));
            r.bytes(s.z, s.z.size());
        }
        const auto readPlay = [&r] {
            SoundPlayDef p;
            p.flags = r.u8();
            p.inPoint = r.u32();
            p.outPoint = r.u32();
            p.loops = r.u16();
            p.env.resize(r.u8());
            for (auto& e : p.env) {
                e.pos = r.u32();
                e.left = r.u16() / 32768.0f;
                e.right = r.u16() / 32768.0f;
            }
            return p;
        };
        for (auto n = r.count(19); n--;) {
            const auto timeline = r.u32();
            const auto frame = r.u32();
            const auto sound = r.u32();
            const auto play = readPlay();
            if (auto it = timelines.find(timeline); it != timelines.end() && frame < it->second.frames.size()) {
                it->second.frames[frame].sounds.emplace_back(sound, play);
            }
        }
        for (auto n = r.count(34); n--;) {
            const auto button = r.u32();
            ButtonDef dummy;
            auto it = buttons.find(button);
            auto& b = it != buttons.end() ? it->second : dummy;
            for (int i = 0; i < 4; ++i) {
                b.sound[i] = r.u16();
                b.soundPlay[i] = readPlay();
            }
        }
        if (!timelines.count(0)) throw std::runtime_error("movie pack has no main timeline");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

// ---------------------------------------------------------------- Clip

std::set<const Clip*> g_liveClips;
bool clipAlive(const Clip* c) { return g_liveClips.count(c) != 0; }

Clip::Clip(Player& player, const TimelineDef& timeline, std::uint32_t id, DisplayObject* holder, bool start)
    : player_(player), timeline_(timeline), id_(id), holder_(holder), bornTick_(player.tick) {
    g_liveClips.insert(this);
    if (start) this->start();
}

Clip::~Clip() {
    g_liveClips.erase(this);
    if (object_) object_->clip = nullptr;
    if (std::getenv("FP_TRACE_CLIPDEL")) std::fprintf(stderr, "[clipdel] clip=%p holder=%p char=%u children=%zu\n", (void*)this, (void*)holder_, unsigned(id_), children_.size());
    // Children kept alive by script must not point at a dead parent.
    for (auto& [depth, child] : children_) if (child && child->parent == this) child->parent = nullptr;
    if (streamHandle_ > 0) player_.audio.stop(streamHandle_);
}

std::shared_ptr<avm1::ClipObject> Clip::object() {
    if (!object_) {
        object_ = std::make_shared<avm1::ClipObject>();
        object_->clip = this;
        if (player_.vm) object_->proto = player_.vm->clipProto;
    }
    return object_;
}

int Clip::frameForLabel(const std::string& label) const {
    if (auto it = timeline_.labels.find(label); it != timeline_.labels.end()) return it->second;
    // Frame labels are case-insensitive in the Flash Player.
    for (const auto& [name, frame] : timeline_.labels) {
        if (name.size() == label.size() &&
            std::equal(name.begin(), name.end(), label.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
            return frame;
        }
    }
    return -1;
}

DisplayObject* Clip::childByName(const std::string& name) const {
    for (const auto& [depth, obj] : children_) {
        if (obj->name == name) return obj.get();
    }
    return nullptr;
}

DisplayObject* Clip::childAt(int depth) const {
    auto it = children_.find(depth);
    return it == children_.end() ? nullptr : it->second.get();
}

std::string Clip::currentLabel() const {
    std::string label;
    for (int f = 0; f <= current_ && f < totalFrames(); ++f) {
        if (!timeline_.frames[static_cast<std::size_t>(f)].label.empty()) label = timeline_.frames[static_cast<std::size_t>(f)].label;
    }
    return label;
}

DisplayObject* Clip::childAtIndex(int index) const {
    if (index < 0 || index >= numChildren()) return nullptr;
    auto it = children_.begin();
    std::advance(it, index);
    return it->second.get();
}

int Clip::indexOf(const DisplayObject* obj) const {
    int i = 0;
    for (const auto& [d, c] : children_) {
        if (c.get() == obj) return i;
        ++i;
    }
    return -1;
}

// Rebuild depth keys so that `ordered` is kept, reusing existing depths where possible.
static void rekey(std::map<int, std::shared_ptr<DisplayObject>>& children, std::vector<std::shared_ptr<DisplayObject>> ordered) {
    children.clear();
    int prev = -0x40000000;
    for (auto& obj : ordered) {
        int d = obj->depth;
        if (d <= prev) d = prev + 1;
        obj->depth = d;
        prev = d;
        children[d] = obj;
    }
}

void Clip::addChildAt(std::shared_ptr<DisplayObject> obj, int index) {
    if (!obj) return;
    std::vector<std::shared_ptr<DisplayObject>> ordered;
    for (auto& [d, c] : children_) ordered.push_back(c);
    if (index < 0 || index > static_cast<int>(ordered.size())) index = static_cast<int>(ordered.size());
    obj->depth = index < static_cast<int>(ordered.size()) ? ordered[static_cast<std::size_t>(index)]->depth
                                                          : (ordered.empty() ? 0 : ordered.back()->depth + 1);
    obj->parent = this;
    obj->dynamic = true;
    ordered.insert(ordered.begin() + index, obj);
    // Everything from the insertion point on gets a strictly increasing depth.
    for (std::size_t i = static_cast<std::size_t>(index) + 1; i < ordered.size(); ++i) {
        if (ordered[i]->depth <= ordered[i - 1]->depth) ordered[i]->depth = ordered[i - 1]->depth + 1;
    }
    rekey(children_, std::move(ordered));
}

std::shared_ptr<DisplayObject> Clip::detach(DisplayObject* obj) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->second.get() == obj) {
            auto keep = it->second;
            children_.erase(it);
            keep->parent = nullptr;
            return keep;
        }
    }
    return nullptr;
}

void Clip::setChildIndex(DisplayObject* obj, int index) {
    auto keep = detach(obj);
    if (!keep) return;
    keep->parent = this;
    std::vector<std::shared_ptr<DisplayObject>> ordered;
    for (auto& [d, c] : children_) ordered.push_back(c);
    index = std::clamp(index, 0, static_cast<int>(ordered.size()));
    keep->depth = index < static_cast<int>(ordered.size()) ? ordered[static_cast<std::size_t>(index)]->depth
                                                           : (ordered.empty() ? 0 : ordered.back()->depth + 1);
    ordered.insert(ordered.begin() + index, keep);
    for (std::size_t i = static_cast<std::size_t>(index) + 1; i < ordered.size(); ++i) {
        if (ordered[i]->depth <= ordered[i - 1]->depth) ordered[i]->depth = ordered[i - 1]->depth + 1;
    }
    rekey(children_, std::move(ordered));
}

Matrix Clip::worldMatrix() const {
    Matrix m;
    for (const Clip* c = this; c && c->holder_; c = c->parent()) m = c->holder_->matrix * m;
    return m;
}

void Clip::setupChild(DisplayObject& obj, std::uint16_t character) { player_.setupDisplay(obj, character, this); }

void Player::setupDisplay(DisplayObject& obj, std::uint16_t character, Clip* parent) {
    obj.character = character;
    obj.parent = parent;
    obj.clip.reset();
    obj.buttonClips.clear();
    if (vm2) avm2::as3PreAllocate(*this, obj); // the AS3 object must exist before its timeline children
    if (character == kEmptyClip) {
        obj.kind = DisplayObject::Kind::Sprite;
        obj.clip = std::make_unique<Clip>(*this, emptyTimeline(), character, &obj);
    } else if (movie.shapes.count(character)) {
        obj.kind = DisplayObject::Kind::Shape;
    } else if (auto et = movie.editTexts.find(character); et != movie.editTexts.end()) {
        obj.kind = DisplayObject::Kind::Text;
        obj.text = et->second.text;
    } else if (auto it = movie.timelines.find(character); it != movie.timelines.end() && character != 0) {
        obj.kind = DisplayObject::Kind::Sprite;
        // The clip must be reachable from its holder before frame 1 runs: scripts executing in it look it up.
        obj.clip = std::make_unique<Clip>(*this, it->second, character, &obj, false);
        obj.clip->start();
    } else if (auto bt = movie.buttons.find(character); bt != movie.buttons.end()) {
        obj.kind = DisplayObject::Kind::Button;
        for (const auto& rec : bt->second.records) {
            auto tl = movie.timelines.find(rec.character);
            obj.buttonClips.push_back(tl != movie.timelines.end() && rec.character != 0
                                          ? std::make_shared<Clip>(*this, tl->second, rec.character, nullptr)
                                          : nullptr);
        }
    } else if (obj.kind != DisplayObject::Kind::Text) {
        obj.kind = obj.kind == DisplayObject::Kind::Sprite ? DisplayObject::Kind::Unknown : obj.kind;
    }
}

Clip* Player::clipOf(const DisplayObject* obj) const {
    if (!obj) return nullptr;
    if (obj->clip) return obj->clip.get();
    if (obj == rootHolder.get()) return root.get();
    return nullptr;
}

bool Player::onStage(const DisplayObject* obj) const {
    for (int guard = 0; obj && guard < 256; ++guard) {
        if (obj == rootHolder.get()) return true;
        if (!obj->parent) return false;
        obj = obj->parent->holder();
    }
    return false;
}

void Clip::place(const PlaceCmd& cmd) {
    if (cmd.type == 2) { removeChild(cmd.depth); return; }
    const bool move = (cmd.flags & 0x08) != 0;
    const bool hasChar = (cmd.flags & 0x01) != 0;
    auto it = children_.find(cmd.depth);
    if (move) {
        if (it == children_.end()) return;
        DisplayObject& obj = *it->second;
        if (hasChar && obj.character != cmd.character) {
            obj.as3.reset(); // a different character gets its own ActionScript 3 object
            obj.as3Constructed = false;
            setupChild(obj, cmd.character);
            if (player_.vm2) player_.clipCreated(obj);
        }
        applyProperties(obj, cmd, obj.dynamicTransform);
        if (cmd.flags & 0x02) obj.cachedTransform = obj.dynamicTransform && obj.cachedTransform;
        return;
    }
    if (!hasChar) return;
    auto obj = std::make_shared<DisplayObject>();
    obj->depth = cmd.depth;
    applyProperties(*obj, cmd, false);
    obj->clipActions = cmd.clipActions;
    setupChild(*obj, cmd.character);
    auto* raw = obj.get();
    if (it != children_.end()) player_.clipRemoved(*it->second);
    children_[cmd.depth] = std::move(obj);
    player_.clipCreated(*raw);
}

void Clip::removeChild(int depth) {
    auto it = children_.find(depth);
    if (it == children_.end()) return;
    player_.clipRemoved(*it->second);
    children_.erase(it);
}

void Clip::swapDepths(int from, int to) {
    if (from == to) return;
    auto a = children_.find(from);
    if (a == children_.end()) return;
    auto moving = std::move(a->second);
    children_.erase(a);
    auto b = children_.find(to);
    if (b != children_.end()) {
        auto other = std::move(b->second);
        children_.erase(b);
        other->depth = from;
        other->dynamic = true;
        children_[from] = std::move(other);
    }
    moving->depth = to;
    moving->dynamic = true;
    children_[to] = std::move(moving);
}

DisplayObject* Clip::attach(std::uint16_t character, const std::string& name, int depth, bool dynamic) {
    auto obj = std::make_shared<DisplayObject>();
    obj->depth = depth;
    obj->name = name;
    obj->dynamic = dynamic;
    setupChild(*obj, character);
    auto* raw = obj.get();
    removeChild(depth);
    children_[depth] = std::move(obj);
    player_.clipCreated(*raw);
    return raw;
}

void Clip::enterFrame(int frame, bool incremental) {
    const auto mark = player_.queueMark();
    if (incremental) {
        for (const auto& cmd : timeline_.frames[static_cast<std::size_t>(frame)].cmds) place(cmd);
    } else {
        seek(frame);
    }
    current_ = frame;
    player_.frameEntered(*this, frame, mark);
    if (!player_.vm && !player_.vm2) runRecognisedActions(frame, 0);
}

// Rebuild the display list for `frame` from scratch, keeping instances whose depth and
// character are unchanged (so nested clips keep animating), like the Flash Player does.
// Script-created children are not part of the timeline and are kept as they are.
void Clip::seek(int frame) {
    std::map<int, SlotState> want;
    for (int f = 0; f <= frame; ++f) {
        for (const auto& cmd : timeline_.frames[static_cast<std::size_t>(f)].cmds) {
            if (cmd.type == 2) { want.erase(cmd.depth); continue; }
            const bool move = (cmd.flags & 0x08) != 0, hasChar = (cmd.flags & 0x01) != 0;
            auto it = want.find(cmd.depth);
            if (move) {
                if (it == want.end()) continue;
                if (hasChar) it->second.character = cmd.character;
                applyProperties(it->second, cmd, false);
                continue;
            }
            if (!hasChar) continue;
            SlotState s;
            s.character = cmd.character;
            applyProperties(s, cmd, false);
            s.clipActions = cmd.clipActions;
            want[cmd.depth] = std::move(s);
        }
    }
    std::map<int, std::shared_ptr<DisplayObject>> next;
    std::vector<DisplayObject*> created;
    for (auto& [depth, child] : children_) {
        if (child->dynamic) next[depth] = std::move(child);
    }
    for (auto& [depth, s] : want) {
        if (next.count(depth)) continue; // a script-owned object occupies this depth
        auto it = children_.find(depth);
        std::shared_ptr<DisplayObject> obj;
        if (it != children_.end() && it->second && it->second->character == s.character) {
            obj = std::move(it->second);
            if (!obj->dynamicTransform) {
                obj->matrix = s.matrix;
                obj->cxform = s.cxform;
                obj->cachedTransform = false;
            }
        } else {
            obj = std::make_shared<DisplayObject>();
            obj->depth = depth;
            obj->matrix = s.matrix;
            obj->cxform = s.cxform;
            obj->clipActions = s.clipActions;
            setupChild(*obj, s.character);
            created.push_back(obj.get());
        }
        obj->clipDepth = s.clipDepth;
        obj->ratio = s.ratio;
        obj->blendMode = s.blendMode;
        obj->name = s.name;
        obj->visible = s.visible;
        next[depth] = std::move(obj);
    }
    for (auto& [depth, child] : children_) {
        if (child) player_.clipRemoved(*child);
    }
    children_ = std::move(next);
    for (auto* obj : created) player_.clipCreated(*obj);
}

int Clip::resolve(const FrameAction& a) const {
    int f = a.frame;
    if (f < 0 && !a.label.empty()) f = frameForLabel(a.label);
    if (f < 0) return -1;
    return std::min(f, totalFrames() - 1);
}

void Clip::gotoFrame(int frame, bool play) {
    if (timeline_.frames.empty()) return;
    frame = std::clamp(frame, 0, totalFrames() - 1);
    playing_ = play;
    if (frame == current_) return;
    enterFrame(frame, frame == current_ + 1);
}

// Without script execution, literal stop()/play()/goto() calls recognised at build time drive the clip.
void Clip::runRecognisedActions(int frame, int depth) {
    if (depth > 8) return;
    for (const auto& a : timeline_.frames[static_cast<std::size_t>(frame)].actions) {
        switch (a.kind) {
            case Stop: if (!player_.ignoreStops) playing_ = false; break;
            case Play: playing_ = true; break;
            case GotoAndStop:
            case GotoAndPlay:
            case NextFrame:
            case PrevFrame: {
                const int target = a.kind == NextFrame ? current_ + 1 : a.kind == PrevFrame ? current_ - 1 : resolve(a);
                if (target < 0 || target >= totalFrames()) break;
                playing_ = a.kind == GotoAndPlay || player_.ignoreStops;
                if (target == current_) break;
                const auto mark = player_.queueMark();
                if (target == current_ + 1) {
                    for (const auto& cmd : timeline_.frames[static_cast<std::size_t>(target)].cmds) place(cmd);
                } else {
                    seek(target);
                }
                current_ = target;
                player_.frameEntered(*this, target, mark);
                runRecognisedActions(target, depth + 1);
                return;
            }
            default: break;
        }
    }
}

void Clip::advance(std::uint64_t tickId) {
    if (bornTick_ != tickId && playing_ && totalFrames() > 1) {
        const int next = current_ + 1 >= totalFrames() ? 0 : current_ + 1;
        enterFrame(next, next == current_ + 1);
    }
    // Collect first: advancing a child never touches this display list, but keep it robust.
    std::vector<Clip*> clips;
    for (auto& [depth, obj] : children_) {
        if (obj->clip) clips.push_back(obj->clip.get());
        for (auto& bc : obj->buttonClips) if (bc) clips.push_back(bc.get());
    }
    for (Clip* c : clips) c->advance(tickId);
}

int Movie::domainOf(std::uint32_t character) const {
    int domain = 0;
    for (const auto& [base, d] : domainBases) {
        if (character >= base) domain = d;
        else break;
    }
    return domain;
}

int Movie::mergeLoaded(Movie&& src, int domain) {
    // Highest character id in use here and in the source.
    auto maxKey = [](const auto& m, std::uint32_t mask) {
        std::uint32_t best = 0;
        for (const auto& [k, v] : m) {
            (void)v;
            const std::uint32_t id = k & mask;
            if (id < 0x10000) best = std::max(best, id);
        }
        return best;
    };
    auto maxId = [&](const Movie& m) {
        std::uint32_t best = 0;
        best = std::max({best, maxKey(m.timelines, 0xffff), maxKey(m.buttons, 0xffff), maxKey(m.fonts, 0xffff),
                         maxKey(m.editTexts, 0xffff), maxKey(m.shapes, 0xffff), maxKey(m.bitmaps, 0xffffffff),
                         maxKey(m.sounds, 0xffffffff)});
        for (const auto& [id, name] : m.symbols) { (void)name; if (id < 0x10000) best = std::max(best, id); }
        return best;
    };
    const std::uint32_t base = maxId(*this) + 1;
    const std::uint32_t srcMax = maxId(src);
    if (base + srcMax >= 0xfff0) return -1;

    // Gradient textures (ids >= 0x10000) are renumbered after the ones already here.
    std::uint32_t gradMax = 0xffff;
    for (const auto& [id, b] : bitmaps) { (void)b; if (id >= 0x10000) gradMax = std::max(gradMax, id); }
    const std::uint32_t gradShift = gradMax + 1 - 0x10000;
    auto texId = [&](std::uint32_t id) { return id >= 0x10000 ? id + gradShift : id + base; };
    auto soundId = [&](std::uint32_t id) {
        if (!id) return id;
        return id >= 0x10000 ? 0x10000 + (id - 0x10000 + base) : id + base; // streams: 0x10000 + timeline id
    };
    auto charId = [&](std::uint32_t id) { return id + base; };

    for (auto& [id, b] : src.bitmaps) bitmaps.emplace(texId(id), std::move(b));
    for (auto& [id, s] : src.sounds) sounds.emplace(soundId(id), std::move(s));
    for (auto& [key, sh] : src.shapes) {
        for (auto& m : sh.meshes) if (m.texture >= 0) m.texture = static_cast<std::int32_t>(texId(static_cast<std::uint32_t>(m.texture)));
        shapes.emplace(charId(key & 0xffff) | (key & 0xffff0000u), std::move(sh));
    }
    for (auto& [id, ratios] : src.morphRatios) morphRatios.emplace(static_cast<std::uint16_t>(charId(id)), std::move(ratios));
    for (auto& [id, tl] : src.timelines) {
        for (auto& fr : tl.frames) {
            for (auto& c : fr.cmds) {
                if (c.type == 1 && (c.flags & 0x01)) c.character = static_cast<std::uint16_t>(charId(c.character));
            }
            for (auto& s : fr.initScripts) s.first = static_cast<std::uint16_t>(charId(s.first));
            for (auto& s : fr.sounds) s.first = soundId(s.first);
        }
        timelines.emplace(charId(id), std::move(tl)); // the main timeline (0) becomes character `base`
    }
    for (auto& [id, b] : src.buttons) {
        for (auto& rec : b.records) rec.character = static_cast<std::uint16_t>(charId(rec.character));
        for (auto& s : b.sound) s = soundId(s);
        buttons.emplace(charId(id), std::move(b));
    }
    for (auto& [id, f] : src.fonts) fonts.emplace(charId(id), std::move(f));
    for (auto& [id, t] : src.editTexts) {
        if (t.font) t.font = static_cast<std::uint16_t>(charId(t.font));
        editTexts.emplace(charId(id), std::move(t));
    }
    for (auto& [id, name] : src.symbols) {
        symbols.emplace_back(charId(id), name);
        exports.emplace(name, static_cast<std::uint16_t>(charId(id)));
    }
    domainBases.emplace_back(base, domain);
    return static_cast<int>(base);
}

const ShapeDef* Movie::shape(std::uint16_t character, std::uint16_t ratio) const {
    std::uint32_t key = character;
    if (ratio) {
        if (auto m = morphRatios.find(character); m != morphRatios.end()) {
            const auto& rs = m->second;
            auto it = std::lower_bound(rs.begin(), rs.end(), ratio);
            if (it == rs.end() || (it != rs.begin() && ratio - *(it - 1) < *it - ratio)) --it;
            if (*it) key |= static_cast<std::uint32_t>(*it) << 16;
        }
    }
    auto it = shapes.find(key);
    return it == shapes.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- Geometry

bool Geometry::characterBounds(std::uint16_t character, const Clip* clip, const DisplayObject* obj, const Matrix& m,
                               float out[4]) const {
    bool any = false;
    if (const ShapeDef* sd = movie_.shape(character, obj ? obj->ratio : 0)) {
        const auto* b = sd->bounds;
        const float xs[2] = {float(b[0]), float(b[2])}, ys[2] = {float(b[1]), float(b[3])};
        for (float x : xs) for (float y : ys) {
            float px, py;
            m.apply(x, y, px, py);
            unionRect(out, any, px, py, px, py);
        }
    } else if (clip) {
        any = clipBounds(*clip, m, out);
    } else if (const EditTextDef* et = obj && obj->ownText ? obj->ownText.get()
                                     : (movie_.editTexts.count(character) ? &movie_.editTexts.at(character) : nullptr)) {
        const auto* b = et->bounds;
        const float xs[2] = {float(b[0]), float(b[2])}, ys[2] = {float(b[1]), float(b[3])};
        for (float x : xs) for (float y : ys) {
            float px, py;
            m.apply(x, y, px, py);
            unionRect(out, any, px, py, px, py);
        }
    } else if (auto bt = movie_.buttons.find(character); bt != movie_.buttons.end() && obj) {
        const auto& recs = bt->second.records;
        for (std::size_t i = 0; i < recs.size(); ++i) {
            if (!(recs[i].states & obj->buttonState)) continue;
            float r[4];
            const Clip* bc = i < obj->buttonClips.size() ? obj->buttonClips[i].get() : nullptr;
            if (characterBounds(recs[i].character, bc, nullptr, m * recs[i].matrix, r)) unionRect(out, any, r[0], r[1], r[2], r[3]);
        }
    }
    return any;
}

bool Geometry::bounds(const DisplayObject& obj, const Matrix& m, float out[4]) const {
    return characterBounds(obj.character, obj.clip.get(), &obj, m * obj.matrix, out);
}

bool Geometry::clipBounds(const Clip& clip, const Matrix& m, float out[4]) const {
    bool any = false;
    for (const auto& [depth, obj] : clip.children()) {
        if (!obj->visible || obj->clipDepth > 0) continue;
        float r[4];
        if (bounds(*obj, m, r)) unionRect(out, any, r[0], r[1], r[2], r[3]);
    }
    return any;
}

bool Geometry::characterHit(std::uint16_t character, const Clip* clip, const DisplayObject* obj, const Matrix& m,
                            float x, float y, bool exact, bool hitStates) const {
    if (const ShapeDef* sd = movie_.shape(character, obj ? obj->ratio : 0)) {
        Matrix inv;
        if (!m.invert(inv)) return false;
        float lx, ly;
        inv.apply(x, y, lx, ly);
        const auto* b = sd->bounds;
        if (lx < b[0] || lx > b[2] || ly < b[1] || ly > b[3]) return false;
        if (!exact) return true;
        for (const auto& mesh : sd->meshes) {
            const auto& v = mesh.vertices;
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const auto& p0 = v[static_cast<std::size_t>(mesh.indices[i])];
                const auto& p1 = v[static_cast<std::size_t>(mesh.indices[i + 1])];
                const auto& p2 = v[static_cast<std::size_t>(mesh.indices[i + 2])];
                const float d0 = (p1.x - p0.x) * (ly - p0.y) - (p1.y - p0.y) * (lx - p0.x);
                const float d1 = (p2.x - p1.x) * (ly - p1.y) - (p2.y - p1.y) * (lx - p1.x);
                const float d2 = (p0.x - p2.x) * (ly - p2.y) - (p0.y - p2.y) * (lx - p2.x);
                const bool neg = d0 < 0 || d1 < 0 || d2 < 0, pos = d0 > 0 || d1 > 0 || d2 > 0;
                if (!(neg && pos)) return true;
            }
        }
        return false;
    }
    if (clip) return clipHit(*clip, m, x, y, exact);
    // Text fields hit on their whole bounding box, like the Flash Player.
    if (const EditTextDef* et = obj && obj->ownText ? obj->ownText.get()
                               : (movie_.editTexts.count(character) ? &movie_.editTexts.at(character) : nullptr)) {
        Matrix inv;
        if (!m.invert(inv)) return false;
        float lx, ly;
        inv.apply(x, y, lx, ly);
        const auto* b = et->bounds;
        return lx >= b[0] && lx <= b[2] && ly >= b[1] && ly <= b[3];
    }
    if (auto bt = movie_.buttons.find(character); bt != movie_.buttons.end()) {
        const auto& recs = bt->second.records;
        const std::uint8_t mask = hitStates ? 0x08 : (obj ? obj->buttonState : 0x01);
        for (std::size_t i = 0; i < recs.size(); ++i) {
            if (!(recs[i].states & mask)) continue;
            const Clip* bc = obj && i < obj->buttonClips.size() ? obj->buttonClips[i].get() : nullptr;
            if (characterHit(recs[i].character, bc, nullptr, m * recs[i].matrix, x, y, exact, false)) return true;
        }
    }
    return false;
}

bool Geometry::hit(const DisplayObject& obj, const Matrix& m, float x, float y, bool exact, bool hitStates) const {
    if (!obj.visible) return false;
    return characterHit(obj.character, obj.clip.get(), &obj, m * obj.matrix, x, y, exact, hitStates);
}

bool Geometry::clipHit(const Clip& clip, const Matrix& m, float x, float y, bool exact) const {
    for (const auto& [depth, obj] : clip.children()) {
        if (obj->clipDepth > 0) continue;
        if (hit(*obj, m, x, y, exact, false)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- Player

Player::Player(const Movie& m) : movie(m), geometry(m) {
    if (movie.runScripts) vm = std::make_unique<avm1::VM>(*this);
    if (movie.runAVM2) {
        vm2 = std::make_unique<avm2::VM>(*this);
        for (const auto& b : movie.abcBlocks) {
            try {
                vm2->loadAbc(b.bytes, b.name);
            } catch (const std::exception& e) {
                std::cerr << "ABC block '" << b.name << "' failed to load: " << e.what() << "\n";
            }
        }
    }
}

Player::~Player() {
    root.reset(); // clips reference the VMs through their objects
    rootHolder.reset();
    as3Hover.reset();
    as3Pressed.reset();
    vm.reset();
    vm2.reset();
}

namespace {

std::string normaliseUrl(std::string u) {
    for (const char* scheme : {"file:///", "file://"}) {
        if (u.rfind(scheme, 0) == 0) { u = u.substr(std::strlen(scheme)); break; }
    }
    if (auto p = u.find("://"); p != std::string::npos) {
        auto slash = u.find('/', p + 3);
        u = slash == std::string::npos ? std::string() : u.substr(slash + 1);
    }
    if (auto q = u.find_first_of("?#"); q != std::string::npos) u.resize(q);
    for (auto& c : u) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (u.rfind("./", 0) == 0) u = u.substr(2);
    while (u.rfind("../", 0) == 0) u = u.substr(3);
    return u;
}

} // namespace

// Finds a bundled file for a script URL: the exact relative path first, then the path with its
// leading directories removed one by one (absolute and http URLs end up here). `canon` is the
// matched relative path without the suffix.
bool Player::resolveData(const std::string& url, const char* suffix, std::string& path, std::string& canon) {
    namespace fs = std::filesystem;
    if (dataIndex.empty() && !dataRoot.empty()) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dataRoot, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            auto rel = fs::relative(it->path(), dataRoot, ec).generic_string();
            for (auto& c : rel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            dataIndex.emplace(rel, it->path().string());
        }
        if (dataIndex.empty()) dataIndex.emplace("", "");
    }
    std::string key = normaliseUrl(url);
    while (true) {
        if (auto it = dataIndex.find(key + suffix); it != dataIndex.end() && !it->second.empty()) {
            path = it->second;
            canon = key;
            return true;
        }
        const auto slash = key.find('/');
        if (slash == std::string::npos) return false;
        key = key.substr(slash + 1);
    }
}

bool Player::readDataFile(const std::string& url, std::vector<std::uint8_t>& out) {
    std::string path, canon;
    if (!resolveData(url, "", path, canon)) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool Player::loadSwf(const std::string& url, int domain, LoadedSwf& out, std::string& error) {
    std::string path, canon;
    if (!resolveData(url, ".pack", path, canon)) {
        error = "no bundled pack for " + url;
        return false;
    }
    if (auto it = loadedSwfs.find(canon); it != loadedSwfs.end()) { out = it->second; return true; }
    Movie src;
    if (!src.load(path, error)) return false;
    const int dom = domain >= 0 ? domain : (vm2 ? vm2->newDomain() : 0);
    auto blocks = src.abcBlocks;
    const int base = movieRw().mergeLoaded(std::move(src), dom);
    if (base < 0) { error = "too many characters"; return false; }
    if (vm2) {
        for (const auto& b : blocks) {
            try { vm2->loadAbc(b.bytes, b.name, dom); }
            catch (const std::exception& e) { std::cerr << "ABC block '" << b.name << "' failed to load: " << e.what() << "\n"; }
        }
    }
    out = {base, dom};
    loadedSwfs[canon] = out;
    return true;
}

double Player::timeMs() const {
    if (virtualClock) return static_cast<double>(tick) * 1000.0 / movie.fps;
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - start).count();
}

std::size_t Player::queueMark() const { return vm ? vm->queue.size() : 0; }

void Player::start() {
    if (vm2) {
        avm2::as3Start(*this);
        return;
    }
    root = std::make_unique<Clip>(*this, movie.timelines.at(0), 0, nullptr);
    if (vm) vm->runQueue();
    updateTextFields();
}

void Player::updateTextFields() {
    if (!vm || !root) return;
    std::function<void(Clip&)> visit = [&](Clip& c) {
        for (auto& [d, obj] : c.children_) {
            if (obj->kind == DisplayObject::Kind::Text && movie.editTexts.count(obj->character) && !obj->ownText) {
                const auto& def = movie.editTexts.at(obj->character);
                if (!def.variable.empty()) {
                    auto ctx = vm->frameContext(c);
                    const auto v = vm->getVariable(ctx, def.variable);
                    if (!v.isUndefined()) obj->text = (def.flags & 8) ? stripHtml(vm->toString(v)) : vm->toString(v);
                }
            }
            if (obj->clip) visit(*obj->clip);
        }
    };
    try { visit(*root); } catch (const std::exception&) {}
}

int Player::playSound(std::uint32_t id, const SoundPlayDef& info, float volume, float pan) {
    const auto it = movie.sounds.find(id);
    if (it == movie.sounds.end()) return 0;
    if (info.flags & 0x01) { audio.stopSound(id); return 0; }
    if ((info.flags & 0x02) && audio.playingSound(id)) return 0;
    return audio.play(it->second, id, 0, info.loops, volume, pan, &info);
}

void Player::setButtonState(DisplayObject& button, std::uint8_t state) {
    const std::uint8_t old = button.buttonState;
    button.buttonState = state;
    if (old == state) return;
    int which = -1;
    if (old == 0x01 && state == 0x02) which = 0;       // roll over
    else if (old == 0x02 && state == 0x01) which = 1;  // roll out
    else if (old == 0x02 && state == 0x04) which = 2;  // press
    else if (old == 0x04 && state == 0x02) which = 3;  // release
    if (which < 0) return;
    const auto bt = movie.buttons.find(button.character);
    if (bt == movie.buttons.end() || !bt->second.sound[which]) return;
    playSound(bt->second.sound[which], bt->second.soundPlay[which]);
}

void Player::frameEntered(Clip& clip, int frame, std::size_t mark) {
    for (const auto& [id, info] : clip.timeline_.frames[static_cast<std::size_t>(frame)].sounds) {
        if (info.flags & 0x04) { // stream sound of this timeline
            if (clip.streamHandle_ > 0) audio.stop(clip.streamHandle_);
            const auto it = movie.sounds.find(id);
            clip.streamHandle_ = it == movie.sounds.end() ? -1 : audio.play(it->second, id, 0, 1, 1, 0, nullptr);
        } else {
            playSound(id, info);
        }
    }
    if (vm2) { avm2::as3FrameEntered(*this, clip, frame); return; }
    if (!vm) return;
    const auto& f = clip.timeline_.frames[static_cast<std::size_t>(frame)];
    std::size_t pos = mark;
    for (const auto& [sprite, code] : f.initScripts) {
        if (!initDone.insert(sprite).second) continue; // DoInitAction runs once per sprite
        vm->queueCode(code, clip, pos++);
    }
    for (const auto& code : f.scripts) vm->queueCode(code, clip, pos++);
}

void Player::clipCreated(DisplayObject& obj) {
    if (vm2) { avm2::as3ClipCreated(*this, obj); return; }
    if (vm && obj.kind == DisplayObject::Kind::Text && obj.parent) {
        // A bound text field initialises its variable when the variable is still undefined.
        const auto& def = movie.editTexts.at(obj.character);
        if (!def.variable.empty()) {
            auto ctx = vm->frameContext(*obj.parent);
            if (vm->getVariable(ctx, def.variable).isUndefined()) vm->setVariable(ctx, def.variable, avm1::Value(obj.text));
        }
    }
    if (!vm || !obj.clip) return;
    // Object.registerClass: the class is applied once this frame's DoInitAction blocks have run.
    if (!movie.exports.empty()) vm->queueClassInit(*obj.clip);
    for (const auto& ca : obj.clipActions) {
        if (ca.events & avm1::ClipEvent::Load) { vm->queueClipEvent(*obj.clip, avm1::ClipEvent::Load); break; }
    }
}

void Player::clipRemoved(DisplayObject& obj) {
    if (vm2) {
        avm2::as3ClipRemoved(*this, obj);
        if (as3Hover.get() == &obj) as3Hover.reset();
        if (as3Pressed.get() == &obj) as3Pressed.reset();
        return;
    }
    if (&obj == hoverButton) hoverButton = nullptr;
    if (&obj == pressedButton) pressedButton = nullptr;
    // Children can hold the tracked pointers too.
    std::function<void(Clip&)> scan = [&](Clip& c) {
        for (auto& [d, child] : c.children_) {
            if (child.get() == hoverButton) hoverButton = nullptr;
            if (child.get() == pressedButton) pressedButton = nullptr;
            if (child->clip) scan(*child->clip);
        }
    };
    if (obj.clip) scan(*obj.clip);
}

namespace {

// Visit every live clip (pre-order).
void forEachClip(Clip& clip, const std::function<void(Clip&)>& fn) {
    fn(clip);
    std::vector<Clip*> kids;
    for (const auto& [d, obj] : clip.children()) if (obj->clip) kids.push_back(obj->clip.get());
    for (Clip* k : kids) forEachClip(*k, fn);
}

bool hasHandler(avm1::VM& vm, Clip& clip, const char* name) {
    auto obj = clip.object();
    const avm1::Value v = vm.getMember(avm1::Value(std::static_pointer_cast<avm1::Object>(obj)), name);
    return v.isObject() && v.o->callable();
}

void queueClipEvents(avm1::VM& vm, Clip& root, std::uint32_t event, const char* handler) {
    forEachClip(root, [&](Clip& c) {
        if (auto* h = c.holder()) {
            for (const auto& ca : h->clipActions) {
                if (ca.events & event) { vm.queueClipEvent(c, event); break; }
            }
        }
        if (handler && hasHandler(vm, c, handler)) vm.queueHandler(c, handler);
    });
}

void notifyListeners(avm1::VM& vm, const std::vector<avm1::ObjectPtr>& listeners, const char* method) {
    const auto copy = listeners;
    for (const auto& l : copy) vm.callMethod(avm1::Value(l), method, {});
}

} // namespace

void Player::step() {
    ++tick;
    audio.advance(1000.0 / std::max(1.0f, movie.fps)); // keeps silent (captured) runs in time
    if (!root) return;
    if (vm2) {
        avm2::as3Step(*this);
        return;
    }
    if (!vm) {
        root->advance(tick);
        return;
    }
    queueClipEvents(*vm, *root, avm1::ClipEvent::EnterFrame, "onEnterFrame");
    root->advance(tick);
    vm->runQueue();
    vm->runIntervals(vm->nowMs());
    vm->runQueue();
    vm->pollSounds();
    vm->runQueue();
    updateTextFields();
}

void Player::keyEvent(int code, bool down) {
    if (code <= 0 || code >= 256) return;
    const bool wasDown = keys[code];
    keys[code] = down;
    if (down) lastKey = code;
    if (vm2) {
        if (!(down && wasDown)) avm2::as3KeyEvent(*this, code, down);
        return;
    }
    if (!vm || !root || (down && wasDown)) return; // ignore auto-repeat
    queueClipEvents(*vm, *root, down ? avm1::ClipEvent::KeyDown : avm1::ClipEvent::KeyUp, nullptr);
    try { notifyListeners(*vm, vm->keyListeners, down ? "onKeyDown" : "onKeyUp"); } catch (const std::exception&) {}
    if (down) {
        // Button key conditions use their own codes for non-printing keys.
        int buttonKey = 0;
        switch (code) {
            case 37: buttonKey = 1; break; case 39: buttonKey = 2; break; case 36: buttonKey = 3; break;
            case 35: buttonKey = 4; break; case 45: buttonKey = 5; break; case 46: buttonKey = 6; break;
            case 8: buttonKey = 8; break; case 13: buttonKey = 13; break; case 38: buttonKey = 14; break;
            case 40: buttonKey = 15; break; case 33: buttonKey = 16; break; case 34: buttonKey = 17; break;
            case 9: buttonKey = 18; break; case 27: buttonKey = 19; break;
            default: buttonKey = (code >= 'A' && code <= 'Z') ? code + 32 : code; break;
        }
        std::vector<DisplayObject*> matches;
        std::function<void(Clip&)> scan = [&](Clip& c) {
            for (const auto& [d, obj] : c.children_) {
                if (obj->kind == DisplayObject::Kind::Button) {
                    const auto& def = movie.buttons.at(obj->character);
                    for (const auto& [cond, code2] : def.actions) {
                        if (((cond >> 9) & 0x7f) == buttonKey) { matches.push_back(obj.get()); break; }
                    }
                }
                if (obj->clip) scan(*obj->clip);
            }
        };
        scan(*root);
        for (auto* b : matches) {
            const auto& def = movie.buttons.at(b->character);
            for (const auto& [cond, code2] : def.actions) {
                if (((cond >> 9) & 0x7f) == buttonKey) vm->queueCode(code2, *b->parent, vm->queue.size());
            }
        }
    }
    vm->runQueue();
}

// Topmost interactive object under (x, y) stage twips: a button, or a clip with mouse handlers.
DisplayObject* Player::buttonAt(Clip& clip, const Matrix& m, float x, float y) {
    for (auto it = clip.children_.rbegin(); it != clip.children_.rend(); ++it) {
        DisplayObject& obj = *it->second;
        if (!obj.visible || obj.clipDepth > 0) continue;
        const Matrix om = m * obj.matrix;
        if (obj.clip) {
            if (auto* inner = buttonAt(*obj.clip, om, x, y)) return inner;
            if (vm && (hasHandler(*vm, *obj.clip, "onPress") || hasHandler(*vm, *obj.clip, "onRelease") ||
                       hasHandler(*vm, *obj.clip, "onRollOver")) &&
                geometry.clipHit(*obj.clip, om, x, y, true)) {
                return &obj;
            }
        } else if (obj.kind == DisplayObject::Kind::Button && geometry.hit(obj, m, x, y, true, true)) {
            return &obj;
        }
    }
    return nullptr;
}

void Player::fireButton(DisplayObject& button, std::uint16_t mask) {
    if (!vm) return;
    if (button.kind == DisplayObject::Kind::Button) {
        const auto& def = movie.buttons.at(button.character);
        for (const auto& [cond, code] : def.actions) {
            if (cond & mask) vm->queueCode(code, *button.parent, vm->queue.size());
        }
    } else if (button.clip) {
        const char* handler = mask == 0x0004 ? "onPress" : mask == 0x0008 ? "onRelease" : mask == 0x0001 ? "onRollOver"
                              : mask == 0x0002 ? "onRollOut" : mask == 0x0040 ? "onReleaseOutside" : nullptr;
        if (handler && hasHandler(*vm, *button.clip, handler)) vm->queueHandler(*button.clip, handler);
    }
}

// BUTTONCONDACTION bits (low byte): 0x01 IdleToOverUp (rollOver), 0x02 OverUpToIdle (rollOut),
// 0x04 OverUpToOverDown (press), 0x08 OverDownToOverUp (release), 0x40 OutDownToIdle (releaseOutside).
void Player::mouseMove(float x, float y) {
    mouseX = x;
    mouseY = y;
    if (!root) return;
    if (vm2) { as3Mouse(true, 0); return; }
    DisplayObject* hit = buttonAt(*root, Matrix{}, x * 20, y * 20);
    if (hit != hoverButton) {
        if (hoverButton) {
            setButtonState(*hoverButton, 0x01);
            if (!mouseDown) fireButton(*hoverButton, 0x0002);
        }
        hoverButton = hit;
        if (hit) {
            setButtonState(*hit, mouseDown && hit == pressedButton ? 0x04 : 0x02);
            if (!mouseDown) fireButton(*hit, 0x0001);
        }
    }
    if (vm) {
        queueClipEvents(*vm, *root, avm1::ClipEvent::MouseMove, nullptr);
        try { notifyListeners(*vm, vm->mouseListeners, "onMouseMove"); } catch (const std::exception&) {}
        vm->runQueue();
    }
}

void Player::mouseButton(bool down) {
    mouseDown = down;
    if (!root) return;
    if (vm2) { as3Mouse(false, down ? 1 : -1); return; }
    DisplayObject* hit = buttonAt(*root, Matrix{}, mouseX * 20, mouseY * 20);
    if (down) {
        pressedButton = hit;
        hoverButton = hit;
        if (hit) { setButtonState(*hit, 0x04); fireButton(*hit, 0x0004); }
    } else {
        if (pressedButton) {
            if (hit == pressedButton) fireButton(*hit, 0x0008);
            else fireButton(*pressedButton, 0x0040);
            if (pressedButton) setButtonState(*pressedButton, hit == pressedButton ? 0x02 : 0x01);
        }
        pressedButton = nullptr;
        hoverButton = hit;
    }
    if (vm) {
        queueClipEvents(*vm, *root, down ? avm1::ClipEvent::MouseDown : avm1::ClipEvent::MouseUp, nullptr);
        try { notifyListeners(*vm, vm->mouseListeners, down ? "onMouseDown" : "onMouseUp"); } catch (const std::exception&) {}
        vm->runQueue();
    }
}

// ---------------------------------------------------------------- ActionScript 3 mouse

// Deepest interactive object under (x, y) in stage twips. Shapes are not interactive: hitting
// one makes its nearest interactive container the target. `hitSomething` reports any hit.
DisplayObject* Player::as3HitTest(Clip& clip, const Matrix& m, float x, float y, bool& hitSomething) {
    hitSomething = false;
    bool shapeHit = false;
    for (auto it = clip.children_.rbegin(); it != clip.children_.rend(); ++it) {
        DisplayObject& obj = *it->second;
        if (!obj.visible || obj.clipDepth > 0) continue;
        Matrix om = m * obj.matrix;
        if (obj.hasScroll) { Matrix shift; shift.tx = -obj.scroll[0] * 20; shift.ty = -obj.scroll[1] * 20; om = om * shift; }
        if (Clip* c = clipOf(&obj)) {
            bool childHit = false;
            DisplayObject* inner = as3HitTest(*c, om, x, y, childHit);
            if (!childHit) continue;
            hitSomething = true;
            if (!obj.mouseEnabled) {
                if (inner && obj.mouseChildren) return inner;
                continue; // the container ignores the mouse: look below it
            }
            if (inner && obj.mouseChildren) return inner;
            return &obj;
        }
        if (obj.kind == DisplayObject::Kind::Button) {
            if (geometry.hit(obj, m, x, y, true, true)) { hitSomething = true; if (obj.mouseEnabled) return &obj; }
            continue;
        }
        if (obj.kind == DisplayObject::Kind::Text) {
            float b[4];
            if (geometry.bounds(obj, m, b) && x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) {
                hitSomething = true;
                if (obj.mouseEnabled) return &obj;
            }
            continue;
        }
        // Plain artwork (shadows, vignettes) does not shield interactive siblings beneath it: keep
        // looking and fall back to the container only when nothing interactive is found.
        if (geometry.hit(obj, m, x, y, true, false)) shapeHit = true;
    }
    if (shapeHit) hitSomething = true;
    return nullptr;
}

void Player::as3Mouse(bool moved, int buttonChange) {
    bool any = false;
    DisplayObject* target = as3HitTest(*root, Matrix{}, mouseX * 20, mouseY * 20, any);
    if (!target && any) target = rootHolder.get();
    if (buttonChange == 1 && std::getenv("FP_TRACE_HIT")) {
        std::fprintf(stderr, "[hit] (%.0f,%.0f):", mouseX, mouseY);
        for (DisplayObject* d = target; d; d = d->parent ? d->parent->holder() : nullptr) std::fprintf(stderr, " <%s#%u", d->name.c_str(), unsigned(d->character));
        std::fprintf(stderr, "\n");
    }
    std::shared_ptr<DisplayObject> hit = target ? target->shared_from_this() : nullptr;
    if (hit != as3Hover) {
        auto old = as3Hover;
        as3Hover = hit;
        if (old) {
            if (old->kind == DisplayObject::Kind::Button) setButtonState(*old, 0x01);
            avm2::as3MouseEvent(*this, old.get(), "mouseOut", true, hit.get());
            avm2::as3MouseEvent(*this, old.get(), "rollOut", false, hit.get());
        }
        if (hit) {
            if (hit->kind == DisplayObject::Kind::Button) setButtonState(*hit, mouseDown && hit == as3Pressed ? 0x04 : 0x02);
            avm2::as3MouseEvent(*this, hit.get(), "mouseOver", true, old.get());
            avm2::as3MouseEvent(*this, hit.get(), "rollOver", false, old.get());
        }
    }
    if (moved && hit) avm2::as3MouseEvent(*this, hit.get(), "mouseMove", true, nullptr);
    if (buttonChange > 0) {
        as3Pressed = hit;
        if (hit) {
            if (hit->kind == DisplayObject::Kind::Button) setButtonState(*hit, 0x04);
            avm2::as3MouseEvent(*this, hit.get(), "mouseDown", true, nullptr);
        } else {
            avm2::as3MouseEvent(*this, rootHolder.get(), "mouseDown", true, nullptr);
        }
    } else if (buttonChange < 0) {
        auto pressed = as3Pressed;
        as3Pressed.reset();
        if (hit) {
            if (hit->kind == DisplayObject::Kind::Button) setButtonState(*hit, 0x02);
            avm2::as3MouseEvent(*this, hit.get(), "mouseUp", true, nullptr);
            if (pressed && pressed == hit) avm2::as3MouseEvent(*this, hit.get(), "click", true, nullptr);
        } else {
            avm2::as3MouseEvent(*this, rootHolder.get(), "mouseUp", true, nullptr);
        }
        if (pressed && pressed != hit && pressed->kind == DisplayObject::Kind::Button) setButtonState(*pressed, 0x01);
    }
}

// ---------------------------------------------------------------- Renderer

Renderer::Renderer(SDL_Renderer* r, Movie& movie, int width, int height)
    : sdl_(r), movie_(movie), width_(width), height_(height) {
    // Mask application: keep the content, scale its (premultiplied) colour and alpha by the mask's alpha.
    maskBlend_ = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
    // Layers accumulate premultiplied colour (standard blending onto transparent black).
    premultipliedBlend_ = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                                     SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
}

// Textures belong to the SDL renderer; SDL_DestroyRenderer releases them, and the
// Renderer may outlive it at shutdown, so nothing is destroyed here.
Renderer::~Renderer() = default;

SDL_Texture* Renderer::texture(std::int32_t id) {
    static const bool debug = std::getenv("FP_DEBUG_TEX") != nullptr;
    auto it = movie_.bitmaps.find(static_cast<std::uint32_t>(id));
    if (it == movie_.bitmaps.end()) {
        if (debug) std::cerr << "texture " << id << " not in pack\n";
        return nullptr;
    }
    auto& b = it->second;
    if (b.texture || b.failed) return b.texture;
    if (debug) std::cerr << "texture " << id << " " << b.width << "x" << b.height << " zlib=" << b.zlib.size() << "\n";
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(b.width) * static_cast<std::size_t>(b.height) * 4);
    uLongf len = static_cast<uLongf>(rgba.size());
    if (b.width <= 0 || b.height <= 0 ||
        uncompress(rgba.data(), &len, b.zlib.data(), static_cast<uLong>(b.zlib.size())) != Z_OK || len != rgba.size()) {
        b.failed = true;
        return nullptr;
    }
    b.texture = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, b.width, b.height);
    if (!b.texture) { b.failed = true; return nullptr; }
    SDL_UpdateTexture(b.texture, nullptr, rgba.data(), b.width * 4);
    SDL_SetTextureBlendMode(b.texture, SDL_BLENDMODE_BLEND);
    std::vector<std::uint8_t>().swap(b.zlib); // the GPU copy is all we need now
    return b.texture;
}

// SWF blend modes expressed as SDL blend factors. Modes SDL cannot express (difference, overlay,
// hard light, invert, alpha, erase) draw as normal.
SDL_BlendMode Renderer::blendFor(std::uint8_t mode) {
    const auto keepAlpha = [](SDL_BlendFactor sf, SDL_BlendFactor df, SDL_BlendOperation op) {
        return SDL_ComposeCustomBlendMode(sf, df, op, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    };
    switch (mode) {
        case 3: return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                                  SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD); // multiply
        case 4: return keepAlpha(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_BLENDOPERATION_ADD); // screen
        case 5: return keepAlpha(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_MAXIMUM);                    // lighten
        case 6: return keepAlpha(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_MINIMUM);                    // darken
        case 8: return keepAlpha(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);                  // add
        case 9: return keepAlpha(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT);         // subtract
        default: return SDL_BLENDMODE_BLEND;
    }
}

void Renderer::drawShape(const ShapeDef& shape, const Matrix& m, const ColorTransform& cx) {
    for (const auto& mesh : shape.meshes) {
        SDL_Texture* tex = nullptr;
        // Fully transparent fills only exist as hit areas.
        if (mesh.texture < 0 && !mesh.vertices.empty() && mesh.vertices[0].rgba[3] == 0 && cx.add[3] <= 0) continue;
        if (mesh.texture >= 0 && !(tex = texture(mesh.texture))) continue;
        scratch_.resize(mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            auto& o = scratch_[i];
            m.apply(v.x, v.y, o.position.x, o.position.y);
            std::uint8_t* c = &o.color.r;
            for (int k = 0; k < 4; ++k) {
                const float value = v.rgba[k] * cx.mul[k] + cx.add[k];
                c[k] = static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
            }
            // SDL_RenderGeometry rejects the whole call when a UV leaves [0,1]; padded gradients
            // and clipped bitmap fills are exactly their edge colour/texel beyond that range.
            o.tex_coord.x = std::clamp(v.u, 0.0f, 1.0f);
            o.tex_coord.y = std::clamp(v.v, 0.0f, 1.0f);
        }
        // Untextured geometry uses the renderer's draw blend mode, which defaults to NONE (alpha ignored).
        if (!tex) SDL_SetRenderDrawBlendMode(sdl_, activeBlend_);
        else SDL_SetTextureBlendMode(tex, activeBlend_);
        SDL_RenderGeometry(sdl_, tex, scratch_.data(), static_cast<int>(scratch_.size()), mesh.indices.data(),
                           static_cast<int>(mesh.indices.size()));
        trianglesDrawn += mesh.indices.size() / 3;
    }
}

void Renderer::drawCharacter(std::uint16_t character, const Clip* clip, const DisplayObject* obj, const Matrix& m,
                             const ColorTransform& cx) {
    if (const ShapeDef* sd = movie_.shape(character, obj ? obj->ratio : 0)) { drawShape(*sd, m, cx); return; }
    if (clip) { drawClip(*clip, m, cx); return; }
    if (obj && obj->kind == DisplayObject::Kind::Text) { drawText(*obj, m, cx); return; }
    // Text fields used directly as button state records have no display object.
    if (auto et = movie_.editTexts.find(character); !obj && et != movie_.editTexts.end()) {
        drawText(et->second, et->second.text, m, cx);
        return;
    }
    if (auto it = movie_.buttons.find(character); it != movie_.buttons.end()) {
        const std::uint8_t state = obj ? obj->buttonState : 0x01;
        const auto& recs = it->second.records;
        for (std::size_t i = 0; i < recs.size(); ++i) {
            if (!(recs[i].states & state)) continue;
            const Clip* bc = obj && i < obj->buttonClips.size() ? obj->buttonClips[i].get() : nullptr;
            drawCharacter(recs[i].character, bc, nullptr, m * recs[i].matrix, cx * recs[i].cxform);
        }
    }
}

void Renderer::drawObject(const DisplayObject& obj, const Matrix& m, const ColorTransform& cx) {
    if (!obj.visible || obj.clipDepth > 0) return; // masks are drawn by drawMasked
    const ColorTransform c = cx * obj.cxform;
    if (c.mul[3] <= 0 && c.add[3] <= 0) return; // fully transparent subtree
    if (obj.blendMode > 1) {
        const SDL_BlendMode saved = activeBlend_;
        activeBlend_ = blendFor(obj.blendMode);
        if (obj.hasScroll) drawScrolled(obj, m, c);
        else drawCharacter(obj.character, obj.clip.get(), &obj, m * obj.matrix, c);
        activeBlend_ = saved;
        return;
    }
    if (obj.hasScroll) { drawScrolled(obj, m, c); return; }
    drawCharacter(obj.character, obj.clip.get(), &obj, m * obj.matrix, c);
}

void Renderer::drawScrolled(const DisplayObject& obj, const Matrix& m, const ColorTransform& c) {
    // scrollRect: show the part of the contents inside the rectangle, with its corner at the origin.
    const Matrix local = m * obj.matrix;
    float x0, y0, x1, y1;
    local.apply(0, 0, x0, y0);
    local.apply(obj.scroll[2] * 20, obj.scroll[3] * 20, x1, y1);
    SDL_Rect rect{static_cast<int>(std::floor(std::min(x0, x1))), static_cast<int>(std::floor(std::min(y0, y1))), 0, 0};
    rect.w = static_cast<int>(std::ceil(std::max(x0, x1))) - rect.x;
    rect.h = static_cast<int>(std::ceil(std::max(y0, y1))) - rect.y;
    SDL_Rect previous{};
    const bool hadClip = SDL_RenderIsClipEnabled(sdl_) == SDL_TRUE;
    if (hadClip) {
        SDL_RenderGetClipRect(sdl_, &previous);
        SDL_Rect inter;
        if (!SDL_IntersectRect(&rect, &previous, &inter)) return;
        rect = inter;
    }
    SDL_RenderSetClipRect(sdl_, &rect);
    Matrix shift;
    shift.tx = -obj.scroll[0] * 20;
    shift.ty = -obj.scroll[1] * 20;
    drawCharacter(obj.character, obj.clip.get(), &obj, local * shift, c);
    SDL_RenderSetClipRect(sdl_, hadClip ? &previous : nullptr);
}

bool Renderer::renderToPixels(const DisplayObject& obj, const Matrix& m, int w, int h, std::vector<std::uint32_t>& out) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    SDL_Texture* tex = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!tex) return false;
    SDL_Texture* previous = SDL_GetRenderTarget(sdl_);
    SDL_SetRenderTarget(sdl_, tex);
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 0);
    SDL_RenderClear(sdl_);
    const int savedDepth = layerDepth_;
    layerDepth_ = 99; // masks fall back to unmasked drawing: the layer textures have the stage's size
    drawCharacter(obj.character, obj.clip.get(), &obj, m, ColorTransform{});
    layerDepth_ = savedDepth;
    out.assign(static_cast<std::size_t>(w) * h, 0);
    const bool ok = SDL_RenderReadPixels(sdl_, nullptr, SDL_PIXELFORMAT_ARGB8888, out.data(), w * 4) == 0;
    SDL_SetRenderTarget(sdl_, previous);
    SDL_DestroyTexture(tex);
    if (!ok) return false;
    for (auto& px : out) { // premultiplied (alpha-blended onto transparent) -> straight alpha
        const unsigned a = px >> 24;
        if (a == 0) { px = 0; continue; }
        if (a == 255) continue;
        auto un = [a](unsigned c) { return std::min(255u, (c * 255u + a / 2) / a); };
        px = (a << 24) | (un((px >> 16) & 0xff) << 16) | (un((px >> 8) & 0xff) << 8) | un(px & 0xff);
    }
    return true;
}

void Renderer::drawClip(const Clip& clip, const Matrix& m, const ColorTransform& cx) {
    const auto& children = clip.children_;
    for (auto it = children.begin(); it != children.end();) {
        const auto& obj = *it->second;
        if (obj.clipDepth > 0) {
            // A mask at depth d with clipDepth c clips the siblings at depths d+1..c.
            auto first = std::next(it), last = first;
            while (last != children.end() && last->first <= obj.clipDepth) ++last;
            drawMasked(obj, first, last, m, cx);
            it = last;
            continue;
        }
        drawObject(obj, m, cx);
        ++it;
    }
}

void Renderer::drawMasked(const DisplayObject& mask, ChildIt first, ChildIt last, const Matrix& m, const ColorTransform& cx) {
    if (!masksEnabled || layerDepth_ >= 6) { // fallback: draw the content unmasked
        for (auto it = first; it != last; ++it) drawObject(*it->second, m, cx);
        return;
    }
    if (static_cast<int>(layers_.size()) <= layerDepth_) {
        auto make = [&] { return SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width_, height_); };
        layers_.emplace_back(make(), make());
    }
    auto [content, maskLayer] = layers_[static_cast<std::size_t>(layerDepth_)];
    if (!content || !maskLayer) {
        for (auto it = first; it != last; ++it) drawObject(*it->second, m, cx);
        return;
    }
    ++layerDepth_;
    SDL_Texture* previous = SDL_GetRenderTarget(sdl_);

    SDL_SetRenderTarget(sdl_, content);
    SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 0);
    SDL_RenderClear(sdl_);
    for (auto it = first; it != last; ++it) drawObject(*it->second, m, cx);

    if (mask.visible) {
        SDL_SetRenderTarget(sdl_, maskLayer);
        SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 0);
        SDL_RenderClear(sdl_);
        ColorTransform opaque; // masks use shape coverage, not their colour or alpha
        opaque.add[3] = 255;
        drawCharacter(mask.character, mask.clip.get(), &mask, m * mask.matrix, opaque);
        SDL_SetRenderTarget(sdl_, content);
        SDL_SetTextureBlendMode(maskLayer, maskBlend_);
        SDL_RenderCopy(sdl_, maskLayer, nullptr, nullptr);
    }

    SDL_SetRenderTarget(sdl_, previous);
    SDL_SetTextureBlendMode(content, premultipliedBlend_);
    SDL_RenderCopy(sdl_, content, nullptr, nullptr);
    --layerDepth_;
}

// ---------------------------------------------------------------- text

std::string stripHtml(const std::string& html) {
    std::string out;
    for (std::size_t i = 0; i < html.size();) {
        if (html[i] == '<') {
            const auto close = html.find('>', i);
            if (close == std::string::npos) break;
            std::string tag = html.substr(i + 1, close - i - 1);
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if ((tag == "br" || tag == "br/" || tag == "/p") && !out.empty()) out.push_back('\n');
            i = close + 1;
            continue;
        }
        if (html[i] == '&') {
            static const std::pair<const char*, char> entities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'},
                                                                   {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '}};
            bool matched = false;
            for (const auto& [e, c] : entities) {
                if (html.compare(i, std::strlen(e), e) == 0) { out.push_back(c); i += std::strlen(e); matched = true; break; }
            }
            if (matched) continue;
        }
        out.push_back(html[i++]);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

namespace {

std::vector<std::uint32_t> decodeUtf8(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xf0 && i + 3 < s.size()) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3fu) << 12) | ((s[i + 2] & 0x3fu) << 6) | (s[i + 3] & 0x3fu); n = 4; }
        else if (c >= 0xe0 && i + 2 < s.size()) { cp = ((c & 0x0fu) << 12) | ((s[i + 1] & 0x3fu) << 6) | (s[i + 2] & 0x3fu); n = 3; }
        else if (c >= 0xc0 && i + 1 < s.size()) { cp = ((c & 0x1fu) << 6) | (s[i + 1] & 0x3fu); n = 2; }
        out.push_back(cp);
        i += n;
    }
    return out;
}

} // namespace

void Renderer::drawText(const DisplayObject& obj, const Matrix& m, const ColorTransform& cx) {
    if (!obj.ownText && !movie_.editTexts.count(obj.character)) return;
    drawText(obj.ownText ? *obj.ownText : movie_.editTexts.at(obj.character), obj.text, m, cx);
}

void Renderer::drawText(const EditTextDef& def, const std::string& text, const Matrix& m, const ColorTransform& cx) {
    if (text.empty() || def.height == 0) return;
    const FontDef* font = nullptr;
    if (auto it = movie_.fonts.find(def.font); it != movie_.fonts.end()) font = &it->second;
    if (!font && !movie_.fonts.empty()) font = &movie_.fonts.begin()->second; // device font: use any embedded outlines
    if (!font) return;

    const float scale = def.height / font->emSquare;
    const float ascent = font->ascent ? font->ascent * scale : def.height * 0.9f;
    const float descent = font->descent ? font->descent * scale : def.height * 0.25f;
    const float lineHeight = ascent + descent + def.leading;
    const float left = def.bounds[0] + 40.0f + def.leftMargin;
    const float width = static_cast<float>(def.bounds[2] - def.bounds[0]) - 80.0f - def.leftMargin - def.rightMargin;

    auto advanceOf = [&](std::uint32_t cp) -> float {
        auto g = font->byCode.find(cp);
        if (g != font->byCode.end()) return font->glyphs[g->second].advance * scale;
        return cp == ' ' ? def.height * 0.3f : def.height * 0.6f;
    };

    // Split into lines (explicit breaks, then word wrap).
    std::vector<std::vector<std::uint32_t>> lines(1);
    const auto chars = decodeUtf8((def.flags & 4) ? std::string(text.size(), '*') : text);
    for (auto cp : chars) {
        if (cp == '\r' || cp == '\n') { lines.emplace_back(); continue; }
        lines.back().push_back(cp);
    }
    if (def.flags & 1) {
        std::vector<std::vector<std::uint32_t>> wrapped;
        for (const auto& line : lines) {
            std::vector<std::uint32_t> cur;
            float w = 0;
            std::size_t lastSpace = std::string::npos;
            for (auto cp : line) {
                cur.push_back(cp);
                w += advanceOf(cp);
                if (cp == ' ') lastSpace = cur.size() - 1;
                if (w > width && cur.size() > 1) {
                    std::vector<std::uint32_t> rest;
                    if (lastSpace != std::string::npos) {
                        rest.assign(cur.begin() + static_cast<std::ptrdiff_t>(lastSpace + 1), cur.end());
                        cur.resize(lastSpace);
                    } else {
                        rest.push_back(cur.back());
                        cur.pop_back();
                    }
                    wrapped.push_back(cur);
                    cur = rest;
                    w = 0;
                    for (auto c : cur) w += advanceOf(c);
                    lastSpace = std::string::npos;
                }
            }
            wrapped.push_back(cur);
        }
        lines = std::move(wrapped);
    }

    const float base[4] = {float(def.color.r), float(def.color.g), float(def.color.b), float(def.color.a)};
    SDL_Color color;
    std::uint8_t* out = &color.r;
    for (int k = 0; k < 4; ++k) out[k] = static_cast<std::uint8_t>(std::clamp(base[k] * cx.mul[k] + cx.add[k], 0.0f, 255.0f));
    if (color.a == 0) return;
    SDL_SetRenderDrawBlendMode(sdl_, activeBlend_);

    float baseline = def.bounds[1] + 40.0f + ascent;
    for (const auto& line : lines) {
        float lineWidth = 0;
        for (auto cp : line) lineWidth += advanceOf(cp);
        float pen = left;
        if (def.align == 1) pen = left + width - lineWidth;
        else if (def.align == 2) pen = left + (width - lineWidth) / 2;
        for (auto cp : line) {
            auto g = font->byCode.find(cp);
            if (g != font->byCode.end() && !font->glyphs[g->second].indices.empty()) {
                const auto& glyph = font->glyphs[g->second];
                const Matrix gm = m * Matrix{scale, 0, 0, scale, pen, baseline};
                scratch_.resize(glyph.xy.size() / 2);
                for (std::size_t i = 0; i < scratch_.size(); ++i) {
                    auto& v = scratch_[i];
                    gm.apply(glyph.xy[i * 2], glyph.xy[i * 2 + 1], v.position.x, v.position.y);
                    v.color = color;
                    v.tex_coord = {0, 0};
                }
                SDL_RenderGeometry(sdl_, nullptr, scratch_.data(), static_cast<int>(scratch_.size()), glyph.indices.data(),
                                   static_cast<int>(glyph.indices.size()));
                trianglesDrawn += glyph.indices.size() / 3;
            }
            pen += advanceOf(cp);
        }
        baseline += lineHeight;
    }
}

} // namespace fp
