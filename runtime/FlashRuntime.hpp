#pragma once
// FlashPort runtime: plays a movie pack produced by FlashPortBuilder.
//
// Movie pack format (little-endian; str = u16 length + bytes; code = u32 length + AVM1 bytecode):
//   "FPK1" u32 version(=7)
//   i32 xmin xmax ymin ymax (twips)  f32 fps  u8 bg r g b  u8 swfVersion  u8 scriptMode (1 AVM1, 2 AVM2)
//   u32 bitmapCount  { u32 id, u16 w, u16 h, u32 zlen, zlib(RGBA straight alpha) }
//   u32 shapeCount, u32 rawSize, u32 zSize, zlib( shapeCount x { u32 id, i32 xmin ymin xmax ymax,
//                      u32 meshCount { i32 texture(-1 = none), u32 vcount
//                                      { f32 x, f32 y, u8 rgba[4], f32 u, f32 v }, u32 icount { u32 } } } )
//                      morph shapes: id = morph id (ratio 0) or morph id | ratio << 16
//   u32 timelineCount{ u32 id (0 = main), u32 frameCount
//                      { str label, u32 cmdCount { u8 type: 1 place | 2 remove, ... },
//                        u32 actionCount { u8 kind, i32 frame, str label },
//                        u32 scriptCount { code }, u32 initCount { u16 sprite, code } } }
//     place: u16 depth, u8 flags, u8 flags2 (0x01 blend), [0x01 u16 char] [0x02 f32 a b c d tx ty] [0x04 f32 mul[4] i16 add[4]]
//            [0x10 u16 clipDepth] [0x20 u16 ratio] [0x40 str name] [blend: u8 mode]; 0x08 = move, 0x80 = invisible;
//            then u32 clipActionCount { u32 events, u8 keyCode, code }
//     remove: u16 depth
//   u32 buttonCount  { u32 id, u8 trackAsMenu, u32 recordCount { u8 states, u16 char, u16 depth, f32 matrix[6],
//                      f32 mul[4], i16 add[4] }, u32 actionCount { u16 conditions, code } }
//   u32 symbolCount  { u32 id, str name }
//   u32 fontCount    { u16 id, f32 emSquare, i16 ascent descent leading,
//                      u32 glyphCount { u16 code, f32 advance, u32 vcount { f32 x, f32 y }, u32 icount { u32 } } }
//   u32 abcCount     { u32 flags, str name, code }   (AVM2 DoABC blocks, in tag order)
//   u32 soundCount   { u32 id, u32 rate, u8 channels, u32 frames, u32 zlen, zlib(s16 interleaved) }
//                    (DefineSound; a timeline's stream sound has id 0x10000 + timeline id)
//   u32 startCount   { u32 timeline, u32 frame, u32 sound, soundinfo }   (StartSound / stream start)
//   u32 buttonSounds { u32 button, 4 x { u16 sound (0 = none), soundinfo } }
//     soundinfo = u8 flags (1 stop, 2 no multiple, 4 stream, 8 has in point, 16 has out point),
//                 u32 inPoint, u32 outPoint, u16 loops, u8 envCount { u32 pos (44.1 kHz), u16 left, u16 right }
//   (pack version 6; sounds/starts/buttonSounds are the last sections)
//   u32 editTextCount{ u16 id, i32 xmin ymin xmax ymax, u16 font, u16 height, u8 rgba[4], u8 align,
//                      u16 leftMargin rightMargin indent, i16 leading, u8 flags, str variable, str text }
#include "Audio.hpp"
#include <SDL.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fp {

namespace avm1 {
class VM;
struct Object;
struct ClipObject;
} // namespace avm1
namespace avm2 {
class VM;
struct Object;
} // namespace avm2

struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Matrix operator*(const Matrix& o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }
    void apply(float x, float y, float& ox, float& oy) const { ox = a * x + c * y + tx; oy = b * x + d * y + ty; }
    bool invert(Matrix& out) const {
        const float det = a * d - b * c;
        if (det == 0) return false;
        out.a = d / det; out.b = -b / det; out.c = -c / det; out.d = a / det;
        out.tx = -(out.a * tx + out.c * ty);
        out.ty = -(out.b * tx + out.d * ty);
        return true;
    }
};

struct ColorTransform {
    float mul[4] = {1, 1, 1, 1};
    float add[4] = {0, 0, 0, 0};
    ColorTransform operator*(const ColorTransform& o) const {
        ColorTransform r;
        for (int i = 0; i < 4; ++i) {
            r.mul[i] = mul[i] * o.mul[i];
            r.add[i] = mul[i] * o.add[i] + add[i];
        }
        return r;
    }
};

using Code = std::shared_ptr<const std::vector<std::uint8_t>>;

struct Vertex { float x, y; std::uint8_t rgba[4]; float u, v; };
struct Mesh { std::int32_t texture = -1; std::vector<Vertex> vertices; std::vector<int> indices; };
struct ShapeDef { std::int32_t bounds[4]{}; std::vector<Mesh> meshes; };
struct BitmapDef { int width{}, height{}; std::vector<std::uint8_t> zlib; SDL_Texture* texture = nullptr; bool failed = false; };

struct ClipActionDef { std::uint32_t events{}; std::uint8_t keyCode{}; Code code; };

struct PlaceCmd {
    std::uint8_t type{};
    std::uint16_t depth{};
    std::uint8_t flags{};
    std::uint8_t flags2{}; // 0x01: blend mode present
    std::uint8_t blend{};  // SWF blend mode (0/1 normal, 3 multiply, 4 screen, ... 8 add)
    std::uint16_t character{};
    Matrix matrix;
    ColorTransform cxform;
    std::uint16_t clipDepth{}, ratio{};
    std::string name;
    std::vector<ClipActionDef> clipActions;
};

struct FrameAction { std::uint8_t kind{}; std::int32_t frame = -1; std::string label; };
enum ActionKind : std::uint8_t { Stop = 1, Play, GotoAndStop, GotoAndPlay, NextFrame, PrevFrame };

struct FrameDef {
    std::string label;
    std::vector<PlaceCmd> cmds;
    std::vector<FrameAction> actions;                        // used when scripts are not executed
    std::vector<Code> scripts;                               // DoAction
    std::vector<std::pair<std::uint16_t, Code>> initScripts; // DoInitAction (sprite id, code)
    std::vector<std::pair<std::uint32_t, SoundPlayDef>> sounds; // StartSound / stream start
};
struct TimelineDef { std::vector<FrameDef> frames; std::map<std::string, int> labels; };

struct ButtonRecord { std::uint8_t states{}; std::uint16_t character{}, depth{}; Matrix matrix; ColorTransform cxform; };
struct ButtonDef {
    bool trackAsMenu = false;
    std::vector<ButtonRecord> records;
    std::vector<std::pair<std::uint16_t, Code>> actions; // (BUTTONCONDACTION conditions, code)
    // DefineButtonSound: idle->over, over->idle, over->down, down->over (sound id 0 = none).
    std::uint32_t sound[4]{};
    SoundPlayDef soundPlay[4];
};

struct GlyphDef { std::uint16_t code{}; float advance{}; std::vector<float> xy; std::vector<int> indices; };
struct FontDef {
    float emSquare = 1024;
    std::int16_t ascent{}, descent{}, leading{};
    std::vector<GlyphDef> glyphs;
    std::map<std::uint32_t, std::size_t> byCode;
};
struct EditTextDef {
    std::int32_t bounds[4]{}; // xmin ymin xmax ymax (twips)
    std::uint16_t font{}, height{};
    SDL_Color color{0, 0, 0, 255};
    std::uint8_t align{}; // 0 left, 1 right, 2 center, 3 justify
    std::uint16_t leftMargin{}, rightMargin{}, indent{};
    std::int16_t leading{};
    std::uint8_t flags{}; // 1 wordWrap, 2 multiline, 4 password, 8 html, 16 border, 32 readOnly
    std::string variable, text;
};

struct Movie {
    std::int32_t stage[4]{}; // xmin xmax ymin ymax
    float fps = 24;
    SDL_Color background{255, 255, 255, 255};
    std::uint8_t swfVersion = 10;
    bool runScripts = false;  // AVM1 scripts
    bool runAVM2 = false;     // AVM2 (ActionScript 3) scripts
    struct AbcBlock { std::uint32_t flags{}; std::string name; Code bytes; };
    std::vector<AbcBlock> abcBlocks;
    std::map<std::uint32_t, BitmapDef> bitmaps;
    std::map<std::uint32_t, SoundDef> sounds;   // DefineSound id; a timeline's stream is 0x10000 + id
    std::map<std::uint32_t, ShapeDef> shapes;   // morph frames: id | ratio << 16
    std::map<std::uint16_t, std::vector<std::uint16_t>> morphRatios; // morph id -> tessellated ratios
    // Shape for a character; morph shapes use the tessellated ratio closest to `ratio`.
    const ShapeDef* shape(std::uint16_t character, std::uint16_t ratio = 0) const;
    std::map<std::uint32_t, TimelineDef> timelines;
    std::map<std::uint32_t, ButtonDef> buttons;
    std::vector<std::pair<std::uint32_t, std::string>> symbols;
    std::map<std::string, std::uint16_t> exports; // linkage name -> character id
    std::map<std::uint32_t, FontDef> fonts;
    std::map<std::uint32_t, EditTextDef> editTexts;

    bool load(const std::string& path, std::string& error);
    // Merges a SWF loaded at run time (Loader.load): every character id of `src` is shifted by the
    // returned base so ids stay unique (its main timeline becomes character `base`). Returns -1
    // when the id space is exhausted. ABC blocks are not merged; the caller loads them.
    int mergeLoaded(Movie&& src, int domain);
    // AVM2 application domain of the SWF a character id came from (0 = the entry SWF).
    int domainOf(std::uint32_t character) const;
    std::vector<std::pair<std::uint32_t, int>> domainBases; // (first character id, domain), ascending
    int stageWidth() const { return (stage[1] - stage[0]) / 20; }
    int stageHeight() const { return (stage[3] - stage[2]) / 20; }
};

class Clip;
bool clipAlive(const Clip* c); // debugging aid (FP_TRACE_STACK)
struct Player;

std::string stripHtml(const std::string& html); // text content of an HTML text field value

struct DisplayObject : std::enable_shared_from_this<DisplayObject> {
    enum class Kind { Shape, Sprite, Button, Text, Unknown } kind = Kind::Unknown;
    DisplayObject() = default;
    DisplayObject(const DisplayObject&) = delete;
    DisplayObject& operator=(const DisplayObject&) = delete;
    ~DisplayObject();
    std::string text;                              // current text of a dynamic text field
    std::shared_ptr<avm1::Object> textObject;      // AVM1 TextField object (created on demand)
    // ActionScript 3 face of this display object.
    std::shared_ptr<avm2::Object> as3;
    bool as3Constructed = false; // constructor ran (or will be run by whoever allocated it)
    bool as3Deferred = false;    // allocated; the constructor runs once the timeline content exists
    bool mouseEnabled = true, mouseChildren = true, buttonMode = false;
    std::shared_ptr<EditTextDef> ownText;          // text fields created by script
    std::uint16_t character{};
    int depth{};
    Clip* parent = nullptr;
    Matrix matrix;
    ColorTransform cxform;
    std::uint16_t clipDepth{}, ratio{};
    std::uint8_t blendMode = 0; // SWF blend mode; 0/1 normal
    bool visible = true;
    bool hasScroll = false;        // scrollRect set: contents are cropped to scroll[2] x scroll[3] and shifted by scroll[0], scroll[1] (pixels)
    float scroll[4] = {0, 0, 0, 0};
    bool dynamic = false;          // created by script (attachMovie...): survives timeline gotos
    bool dynamicTransform = false; // transformed by script: the timeline no longer moves it (Flash behaviour)
    std::string name;
    std::vector<ClipActionDef> clipActions;
    std::unique_ptr<Clip> clip;                     // sprite instance
    std::vector<std::shared_ptr<Clip>> buttonClips; // sprite instances for button records (parallel to records)
    std::uint8_t buttonState = 0x01;                 // 0x01 up, 0x02 over, 0x04 down
    // _xscale/_yscale/_rotation as last set by script (Flash keeps them to avoid lossy decomposition).
    bool cachedTransform = false;
    double xscale = 100, yscale = 100, rotation = 0;

    void decompose();
    void recompose();
};

// A MovieClip instance: owns a display list and a play head.
class Clip {
public:
    Clip(Player& player, const TimelineDef& timeline, std::uint32_t id, DisplayObject* holder, bool start = true);
    void start() { if (!timeline_.frames.empty()) enterFrame(0, true); } // first frame (when built with start=false)
    ~Clip();
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;

    void advance(std::uint64_t tickId); // timeline step for this clip and its descendants
    void gotoFrame(int frame, bool play);
    void play() { playing_ = true; }
    void stop() { playing_ = false; }
    int currentFrame() const { return current_; }
    int totalFrames() const { return static_cast<int>(timeline_.frames.size()); }
    int frameForLabel(const std::string& label) const;
    bool playing() const { return playing_; }
    void setPlaying(bool p) { playing_ = p; }
    std::uint32_t id() const { return id_; }
    DisplayObject* holder() const { return holder_; }
    Clip* parent() const { return holder_ ? holder_->parent : nullptr; }
    const std::map<int, std::shared_ptr<DisplayObject>>& children() const { return children_; }
    DisplayObject* childByName(const std::string& name) const;
    DisplayObject* childAt(int depth) const;
    const TimelineDef& timeline() const { return timeline_; }
    std::string currentLabel() const;

    // Index-based container API (ActionScript 3)
    int numChildren() const { return static_cast<int>(children_.size()); }
    DisplayObject* childAtIndex(int index) const;
    int indexOf(const DisplayObject* obj) const;
    void addChildAt(std::shared_ptr<DisplayObject> obj, int index); // index < 0: on top
    std::shared_ptr<DisplayObject> detach(DisplayObject* obj);
    void setChildIndex(DisplayObject* obj, int index);

    // Script API
    DisplayObject* attach(std::uint16_t character, const std::string& name, int depth, bool dynamic);
    void removeChild(int depth);
    void swapDepths(int from, int to);
    Matrix worldMatrix() const; // clip space -> stage twips
    std::shared_ptr<avm1::ClipObject> object(); // AVM1 object for this clip (created on demand)

private:
    void enterFrame(int frame, bool incremental);
    void seek(int frame);
    void runRecognisedActions(int frame, int depth);
    int resolve(const FrameAction& a) const;
    void place(const PlaceCmd& cmd);
    void setupChild(DisplayObject& obj, std::uint16_t character);

    Player& player_;
    const TimelineDef& timeline_;
    std::uint32_t id_;
    DisplayObject* holder_; // null for the root
    int current_ = -1;
    int streamHandle_ = -1; // audio handle of this timeline's stream sound
    bool playing_ = true;
    std::uint64_t bornTick_ = 0;
    std::map<int, std::shared_ptr<DisplayObject>> children_;
    std::shared_ptr<avm1::ClipObject> object_;
    friend class Renderer;
    friend struct Player;
};

class Geometry {
public:
    explicit Geometry(const Movie& m) : movie_(m) {}
    // Bounds (stage twips when `m` is a world matrix) of what `obj`/`clip` currently displays.
    bool bounds(const DisplayObject& obj, const Matrix& m, float out[4]) const;
    bool clipBounds(const Clip& clip, const Matrix& m, float out[4]) const;
    // Point (in the space of `m`) inside the object's geometry (triangles when `exact`).
    bool hit(const DisplayObject& obj, const Matrix& m, float x, float y, bool exact, bool hitStates) const;
    bool clipHit(const Clip& clip, const Matrix& m, float x, float y, bool exact) const;

private:
    bool characterBounds(std::uint16_t character, const Clip* clip, const DisplayObject* obj, const Matrix& m, float out[4]) const;
    bool characterHit(std::uint16_t character, const Clip* clip, const DisplayObject* obj, const Matrix& m, float x, float y,
                      bool exact, bool hitStates) const;
    const Movie& movie_;
};

struct Player {
    const Movie& movie;
    Geometry geometry;
    std::uint64_t tick = 0;
    bool ignoreStops = false;
    bool virtualClock = false; // timers follow ticks (deterministic runs) instead of the wall clock
    double timeMs() const;
    Audio audio;                   // declared before `root`: clips stop their streams when destroyed
    // Multi-file games: the other SWFs (as .pack) and data files bundled next to movie.pack.
    std::string dataRoot;
    struct LoadedSwf { int base = -1; int domain = 0; };
    // Loads data/<url>.pack into the running movie (ids shifted by `base`, ABC blocks added to the
    // AVM2 domain `domain`, or a fresh one when `domain` < 0). Repeated loads reuse the first copy.
    bool loadSwf(const std::string& url, int domain, LoadedSwf& out, std::string& error);
    bool readDataFile(const std::string& url, std::vector<std::uint8_t>& out);
    bool resolveData(const std::string& url, const char* suffix, std::string& path, std::string& canon);
    Movie& movieRw() { return const_cast<Movie&>(movie); }
    // Set by the host: renders a display object offscreen (BitmapData.draw).
    std::function<bool(const DisplayObject&, const Matrix&, int, int, std::vector<std::uint32_t>&)> renderObject;
    std::unique_ptr<Clip> root;
    std::unique_ptr<avm1::VM> vm;  // AVM1 interpreter (null unless the movie runs AVM1 scripts)
    std::unique_ptr<avm2::VM> vm2; // AVM2 interpreter (null unless the movie runs ActionScript 3)
    std::shared_ptr<DisplayObject> rootHolder; // display object whose clip is `root` (AS3 needs one)
    // startDrag(): the dragged object follows the mouse (AS3).
    std::shared_ptr<DisplayObject> dragObj;
    bool dragLock = false, dragBounded = false;
    float dragOff[2] = {0, 0}, dragRect[4] = {0, 0, 0, 0}; // grab offset (twips); bounds (pixels, parent space)
    void updateDrag();
    std::shared_ptr<DisplayObject> as3Hover, as3Pressed;
    bool keys[256] = {};
    int lastKey = 0;
    float mouseX = 0, mouseY = 0; // stage pixels
    bool mouseDown = false;
    DisplayObject* hoverButton = nullptr;
    DisplayObject* pressedButton = nullptr;
    std::map<std::string, LoadedSwf> loadedSwfs; // resolved pack path -> where it was merged
    std::map<std::string, std::string> dataIndex; // lower-case relative path -> real path
    std::set<std::uint16_t> initDone; // sprites whose DoInitAction already ran
    // Plays a pack sound (DefineSound id) and returns the audio handle (0 when it did not start).
    int playSound(std::uint32_t id, const SoundPlayDef& info, float volume = 1, float pan = 0);
    void setButtonState(DisplayObject& button, std::uint8_t state); // plays DefineButtonSound transitions

    explicit Player(const Movie& m);
    ~Player();
    void start();
    void step();                // one frame
    void updateTextFields();    // pull variable-bound text field contents from their timelines
    void keyEvent(int flashKeyCode, bool down);
    void mouseMove(float x, float y);
    void mouseButton(bool down);

    // Display objects outside any timeline (ActionScript 3 `new`).
    void setupDisplay(DisplayObject& obj, std::uint16_t character, Clip* parent);
    Clip* clipOf(const DisplayObject* obj) const;
    bool onStage(const DisplayObject* obj) const;

    // Hooks used by Clip.
    void frameEntered(Clip& clip, int frame, std::size_t queueMark);
    void clipCreated(DisplayObject& obj);
    void clipRemoved(DisplayObject& obj);
    std::size_t queueMark() const;

private:
    DisplayObject* buttonAt(Clip& clip, const Matrix& m, float x, float y);
    void fireButton(DisplayObject& button, std::uint16_t conditionMask);
    void as3Mouse(bool moved, int buttonChange); // buttonChange: 0 none, 1 down, -1 up
    DisplayObject* as3HitTest(Clip& clip, const Matrix& m, float x, float y, bool& hitSomething);
};

class Renderer {
public:
    Renderer(SDL_Renderer* r, Movie& movie, int width, int height);
    ~Renderer();
    void drawClip(const Clip& clip, const Matrix& m, const ColorTransform& cx);
    // Renders what `obj` displays (its own transform ignored) with `m` (twips -> bitmap pixels) into
    // a w x h straight-alpha ARGB buffer; used by BitmapData.draw.
    bool renderToPixels(const DisplayObject& obj, const Matrix& m, int w, int h, std::vector<std::uint32_t>& out);
    bool bounds(const Clip& clip, const Matrix& m, float out[4]) const { return Geometry(movie_).clipBounds(clip, m, out); }
    std::uint64_t trianglesDrawn = 0;
    bool masksEnabled = true;

private:
    SDL_BlendMode activeBlend_ = SDL_BLENDMODE_BLEND; // blend mode of the object being drawn
    static SDL_BlendMode blendFor(std::uint8_t mode);
    using ChildIt = std::map<int, std::shared_ptr<DisplayObject>>::const_iterator;
    void drawMasked(const DisplayObject& mask, ChildIt first, ChildIt last, const Matrix& m, const ColorTransform& cx);
    void drawObject(const DisplayObject& obj, const Matrix& m, const ColorTransform& cx);
    void drawScrolled(const DisplayObject& obj, const Matrix& m, const ColorTransform& cx);
    void drawCharacter(std::uint16_t character, const Clip* clip, const DisplayObject* obj, const Matrix& m,
                       const ColorTransform& cx);
    void drawShape(const ShapeDef& shape, const Matrix& m, const ColorTransform& cx);
    void drawText(const DisplayObject& obj, const Matrix& m, const ColorTransform& cx);
    void drawText(const EditTextDef& def, const std::string& text, const Matrix& m, const ColorTransform& cx);
    SDL_Texture* texture(std::int32_t id);

    SDL_Renderer* sdl_;
    Movie& movie_;
    std::vector<SDL_Vertex> scratch_;
    int width_, height_;
    std::vector<std::pair<SDL_Texture*, SDL_Texture*>> layers_;
    int layerDepth_ = 0;
    SDL_BlendMode maskBlend_{}, premultipliedBlend_{};
};

} // namespace fp
