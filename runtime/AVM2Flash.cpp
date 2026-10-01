#include "AVM2.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace fp::avm2 {
namespace {

Value arg(const Args& a, std::size_t i) { return i < a.size() ? a[i] : Value(); }
double num(VM& vm, const Args& a, std::size_t i, double def = 0) { return i < a.size() && !a[i].isUndefined() ? vm.toNumber(a[i]) : def; }
bool flag(VM& vm, const Args& a, std::size_t i, bool def = false) { return i < a.size() && !a[i].isUndefined() ? vm.toBoolean(a[i]) : def; }

// ---------------------------------------------------------------- native state

struct EventData : NativeData {
    std::string type;
    bool bubbles = false, cancelable = false;
    Value target, currentTarget;
    int phase = 2;
    bool stop = false, stopNow = false, prevented = false;
    std::map<std::string, Value> extra; // subclass fields (keyCode, stageX, text, ...)
};

struct DisplayNative : NativeData {
    std::shared_ptr<DisplayObject> d;
    std::map<int, Value> frameScripts;
};

struct TimerData : NativeData {
    double delay = 1000;
    int repeatCount = 0, currentCount = 0;
    int timerId = 0;
};

struct SoundData : NativeData {
    std::uint32_t soundId = 0;
};
struct ChannelData : NativeData {
    int handle = 0;
    std::uint32_t soundId = 0;
    float volume = 1, pan = 0;
};
std::shared_ptr<SoundData> soundOf(const Value& v) {
    return v.isObject() ? std::dynamic_pointer_cast<SoundData>(v.o->native) : nullptr;
}
std::shared_ptr<ChannelData> channelOf(const Value& v) {
    return v.isObject() ? std::dynamic_pointer_cast<ChannelData>(v.o->native) : nullptr;
}
// Live SoundChannel objects by audio handle, to dispatch soundComplete.
std::map<int, std::weak_ptr<Object>>& channelRegistry() {
    static std::map<int, std::weak_ptr<Object>> m;
    return m;
}

std::shared_ptr<EventData> eventOf(const Value& v) {
    return v.isObject() ? std::dynamic_pointer_cast<EventData>(v.o->native) : nullptr;
}
std::shared_ptr<DisplayNative> displayNativeOf(const Value& v) {
    return v.isObject() ? std::dynamic_pointer_cast<DisplayNative>(v.o->native) : nullptr;
}
DisplayObject* disp(const Value& v) {
    auto n = displayNativeOf(v);
    return n ? n->d.get() : nullptr;
}
NativeData& dispatcherOf(Object& o) {
    if (!o.native) o.native = std::make_shared<NativeData>();
    return *o.native;
}

// Symbol linkage: class name -> character id (from SymbolClass).
std::map<std::string, std::uint16_t>& symbolIds(Player& player) {
    static std::map<const Player*, std::map<std::string, std::uint16_t>> cache;
    auto& m = cache[&player];
    if (m.empty()) {
        for (const auto& [id, name] : player.movie.symbols) {
            if (id == 0) continue;
            std::string qn = name;
            const auto dot = qn.rfind('.');
            if (dot != std::string::npos) qn = qn.substr(0, dot) + "::" + qn.substr(dot + 1);
            m[qn] = static_cast<std::uint16_t>(id);
        }
        m[""] = 0; // marker: built
    }
    return m;
}

bool classDerivesFrom(const Class* c, const char* name) {
    for (; c; c = c->super.get()) if (c->name == name && (c->ns.uri.rfind("flash.", 0) == 0)) return true;
    return false;
}

Value displayValue(VM& vm, DisplayObject* d) { return d ? Value(vm.objectFor(*d)) : Value::null(); }

Clip* clipOfValue(VM& vm, const Value& v) { return vm.player.clipOf(disp(v)); }

std::shared_ptr<EditTextDef> defaultTextDef() {
    auto def = std::make_shared<EditTextDef>();
    def->bounds[0] = 0; def->bounds[1] = 0; def->bounds[2] = 100 * 20; def->bounds[3] = 100 * 20;
    def->height = 12 * 20;
    def->color = {0, 0, 0, 255};
    def->flags = 32;
    return def;
}

EditTextDef& writableTextDef(VM& vm, DisplayObject& d) {
    if (!d.ownText) {
        auto it = vm.player.movie.editTexts.find(d.character);
        d.ownText = it != vm.player.movie.editTexts.end() ? std::make_shared<EditTextDef>(it->second) : defaultTextDef();
    }
    return *d.ownText;
}

const EditTextDef* textDef(VM& vm, const DisplayObject& d) {
    if (d.ownText) return d.ownText.get();
    auto it = vm.player.movie.editTexts.find(d.character);
    return it == vm.player.movie.editTexts.end() ? nullptr : &it->second;
}

// Build the propagation path (target first, then ancestors up to the stage).
std::vector<ObjectPtr> propagationPath(VM& vm, const ObjectPtr& target) {
    std::vector<ObjectPtr> path{target};
    DisplayObject* d = disp(Value(target));
    if (!d) return path;
    for (int guard = 0; guard < 256; ++guard) {
        if (d == vm.player.rootHolder.get()) {
            if (vm.stageObject) path.push_back(vm.stageObject);
            break;
        }
        if (!d->parent || !d->parent->holder()) break;
        d = d->parent->holder();
        path.push_back(vm.objectFor(*d));
    }
    return path;
}

void invokeListeners(VM& vm, const ObjectPtr& obj, const ObjectPtr& event, EventData& ev, bool capturePhase) {
    if (!obj->native) return;
    auto it = obj->native->listeners.find(ev.type);
    if (it == obj->native->listeners.end()) return;
    const auto list = it->second; // listeners may be added or removed while dispatching
    ev.currentTarget = Value(obj);
    for (const auto& l : list) {
        if (l.capture != capturePhase) continue;
        try {
            vm.call(l.fn, Value::null(), {Value(event)});
        } catch (const ScriptException& e) {
            vm.reportError(("listener " + ev.type).c_str(), e);
        }
        if (ev.stopNow) break;
    }
}

Value makePoint(VM& vm, double x, double y) {
    auto cls = vm.findClass("flash.geom::Point");
    return vm.construct(Value(cls), {Value(x), Value(y)});
}

Value makeRectangle(VM& vm, double x, double y, double w, double h) {
    auto cls = vm.findClass("flash.geom::Rectangle");
    return vm.construct(Value(cls), {Value(x), Value(y), Value(w), Value(h)});
}

} // namespace

// ---------------------------------------------------------------- VM display bridge

ClassPtr VM::classForCharacter(std::uint16_t character, DisplayObject::Kind kind) {
    for (const auto& [id, name] : player.movie.symbols) {
        if (id == character && character != 0) {
            std::string qn = name;
            const auto dot = qn.rfind('.');
            if (dot != std::string::npos) qn = qn.substr(0, dot) + "::" + qn.substr(dot + 1);
            if (auto c = findClass(qn)) return c;
        }
    }
    switch (kind) {
        case DisplayObject::Kind::Sprite: return findClass("flash.display::MovieClip");
        case DisplayObject::Kind::Button: return findClass("flash.display::SimpleButton");
        case DisplayObject::Kind::Text: return findClass("flash.text::TextField");
        case DisplayObject::Kind::Shape: return findClass("flash.display::Shape");
        default: return findClass("flash.display::Shape");
    }
}

ObjectPtr VM::objectFor(DisplayObject& d) {
    if (!d.as3) {
        pendingDisplay = d.shared_from_this();
        auto cls = classForCharacter(d.character, d.kind);
        auto obj = createInstance(cls);
        pendingDisplay.reset();
        d.as3 = obj;
        if (!d.as3Deferred && !d.as3Constructed) {
            d.as3Constructed = true;
            Args none;
            try { runConstructor(cls, obj, none); } catch (const ScriptException& e) { reportError("constructor", e); }
        }
    } else if (!d.as3Deferred && !d.as3Constructed) {
        d.as3Constructed = true;
        Args none;
        try { runConstructor(d.as3->cls, d.as3, none); } catch (const ScriptException& e) { reportError("constructor", e); }
    }
    return d.as3;
}

void VM::runFrameScript(const ObjectPtr& clipObj, int frame) {
    auto n = std::dynamic_pointer_cast<DisplayNative>(clipObj->native);
    if (!n) return;
    Clip* c = player.clipOf(n->d.get());
    if (!c || c->currentFrame() != frame) return; // the play head moved on: stale request
    auto it = n->frameScripts.find(frame);
    if (it == n->frameScripts.end() || !it->second.isObject()) return;
    try {
        call(it->second, Value(clipObj), {});
    } catch (const ScriptException& e) {
        reportError("frame script", e);
    }
}

bool VM::dispatchEvent(const ObjectPtr& target, const ObjectPtr& event) {
    auto ev = std::dynamic_pointer_cast<EventData>(event->native);
    if (!ev) return true;
    ev->target = Value(target);
    ev->stop = ev->stopNow = false;
    const auto path = ev->bubbles || true ? propagationPath(*this, target) : std::vector<ObjectPtr>{target};
    // Capture phase: stage down to the parent.
    ev->phase = 1;
    for (std::size_t i = path.size(); i-- > 1;) {
        invokeListeners(*this, path[i], event, *ev, true);
        if (ev->stop) return !ev->prevented;
    }
    ev->phase = 2;
    invokeListeners(*this, path[0], event, *ev, false);
    if (ev->stop || !ev->bubbles) return !ev->prevented;
    ev->phase = 3;
    for (std::size_t i = 1; i < path.size(); ++i) {
        invokeListeners(*this, path[i], event, *ev, false);
        if (ev->stop) break;
    }
    return !ev->prevented;
}

void VM::addListener(const ObjectPtr& target, const std::string& type, const Value& fn, int priority, bool capture) {
    if (!fn.isObject()) return;
    auto& list = dispatcherOf(*target).listeners[type];
    for (const auto& l : list) if (l.capture == capture && sameFunction(l.fn, fn)) return;
    Listener l{fn, priority, capture};
    auto pos = std::find_if(list.begin(), list.end(), [&](const Listener& x) { return x.priority < priority; });
    list.insert(pos, l);
    if (type == "enterFrame") {
        bool known = false;
        for (auto& w : enterFrameListeners) if (w.lock() == target) known = true;
        if (!known) enterFrameListeners.push_back(target);
    }
}

void VM::removeListener(const ObjectPtr& target, const std::string& type, const Value& fn, bool capture) {
    if (!target->native) return;
    auto it = target->native->listeners.find(type);
    if (it == target->native->listeners.end()) return;
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Listener& l) { return l.capture == capture && sameFunction(l.fn, fn); }), list.end());
}

ObjectPtr VM::makeEvent(const std::string& clsName, const std::string& type, bool bubbles) {
    auto cls = findClass(clsName);
    Value e = construct(Value(cls), {Value(type), Value(bubbles)});
    return e.o;
}

// ---------------------------------------------------------------- bridge functions

void as3PreAllocate(Player& player, DisplayObject& obj) {
    if (obj.as3) return; // created by `new` (or the root): that object owns this display object
    VM& vm = *player.vm2;
    vm.pendingDisplay = obj.shared_from_this();
    // Kind is not known yet: the class decides (symbol class or MovieClip for sprites).
    DisplayObject::Kind kind = player.movie.timelines.count(obj.character) ? DisplayObject::Kind::Sprite
                               : player.movie.buttons.count(obj.character) ? DisplayObject::Kind::Button
                               : player.movie.editTexts.count(obj.character) ? DisplayObject::Kind::Text
                                                                             : DisplayObject::Kind::Shape;
    auto cls = vm.classForCharacter(obj.character, kind);
    try {
        obj.as3 = vm.createInstance(cls);
    } catch (const ScriptException& e) {
        vm.reportError("allocate", e);
    }
    vm.pendingDisplay.reset();
    obj.as3Deferred = true;
    obj.as3Constructed = false;
}

void as3ClipCreated(Player& player, DisplayObject& obj) {
    VM& vm = *player.vm2;
    auto o = obj.as3 ? obj.as3 : nullptr;
    if (!o) { obj.as3Deferred = true; o = vm.objectFor(obj); }
    // Timeline instances become properties of their parent (declared slots, or dynamic).
    if (!obj.name.empty() && obj.parent) {
        if (DisplayObject* ph = obj.parent->holder()) {
            auto parentObj = vm.objectFor(*ph);
            Multiname mn = Multiname::publicName(obj.name);
            if (parentObj->traits && parentObj->traits->find(mn)) {
                try { vm.setProperty(Value(parentObj), mn, Value(o), true); } catch (const ScriptException&) {}
            } else if (parentObj->isDynamic) {
                parentObj->dynamic.set(obj.name, Value(o));
            }
        }
    }
    if (!obj.as3Constructed) {
        obj.as3Constructed = true;
        obj.as3Deferred = false;
        Args none;
        try { vm.runConstructor(o->cls, o, none); } catch (const ScriptException& e) { vm.reportError("constructor", e); }
    }
    obj.as3Deferred = false;
    if (player.onStage(&obj)) {
        vm.dispatchEvent(o, vm.makeEvent("flash.events::Event", "added", true));
        vm.dispatchEvent(o, vm.makeEvent("flash.events::Event", "addedToStage", false));
    }
}

void as3ClipRemoved(Player& player, DisplayObject& obj) {
    VM& vm = *player.vm2;
    if (!obj.as3) return;
    if (player.onStage(&obj)) {
        vm.dispatchEvent(obj.as3, vm.makeEvent("flash.events::Event", "removedFromStage", false));
    }
}

void as3FrameEntered(Player& player, Clip& clip, int frame) {
    DisplayObject* holder = clip.holder();
    if (!holder) return;
    VM& vm = *player.vm2;
    auto o = holder->as3 ? holder->as3 : vm.objectFor(*holder);
    vm.queue.push_back({o, frame});
}

void as3Start(Player& player) {
    VM& vm = *player.vm2;
    auto stageCls = vm.findClass("flash.display::Stage");
    vm.stageObject = vm.createInstance(stageCls);

    player.rootHolder = std::make_shared<DisplayObject>();
    player.rootHolder->kind = DisplayObject::Kind::Sprite;
    std::string docName;
    for (const auto& [id, name] : player.movie.symbols) if (id == 0) docName = name;
    const auto dot = docName.rfind('.');
    if (dot != std::string::npos) docName = docName.substr(0, dot) + "::" + docName.substr(dot + 1);
    ClassPtr docClass = docName.empty() ? nullptr : vm.findClass(docName);
    if (!docClass) docClass = vm.findClass("flash.display::MovieClip");
    std::cout << "AS3 document class: " << (docName.empty() ? "<none>" : docName) << (docClass ? "" : " (missing)") << "\n";

    vm.pendingDisplay = player.rootHolder;
    player.rootHolder->as3 = vm.createInstance(docClass);
    vm.pendingDisplay.reset();
    player.rootHolder->as3Deferred = true;
    player.root = std::make_unique<Clip>(player, player.movie.timelines.at(0), 0, player.rootHolder.get());
    player.rootHolder->as3Deferred = false;
    player.rootHolder->as3Constructed = true;
    Args none;
    try {
        vm.runConstructor(docClass, player.rootHolder->as3, none);
    } catch (const ScriptException& e) {
        vm.reportError("document class constructor", e);
    }
    vm.dispatchEvent(player.rootHolder->as3, vm.makeEvent("flash.events::Event", "addedToStage", false));
    vm.runQueue();
}

void as3PollSounds(Player& player);

void as3Step(Player& player) {
    VM& vm = *player.vm2;
    // 1. enterFrame to every display object listening (on the display list or not)
    std::vector<ObjectPtr> targets;
    vm.enterFrameListeners.erase(std::remove_if(vm.enterFrameListeners.begin(), vm.enterFrameListeners.end(), [&](const std::weak_ptr<Object>& w) {
        auto o = w.lock();
        if (!o || !o->native) return true;
        auto it = o->native->listeners.find("enterFrame");
        if (it == o->native->listeners.end() || it->second.empty()) return true;
        targets.push_back(o);
        return false;
    }), vm.enterFrameListeners.end());
    for (auto& t : targets) {
        auto ev = vm.makeEvent("flash.events::Event", "enterFrame", false);
        auto data = std::dynamic_pointer_cast<EventData>(ev->native);
        data->target = Value(t);
        data->phase = 2;
        invokeListeners(vm, t, ev, *data, false);
    }
    // 2. timelines advance: new children are constructed, frame scripts queued
    player.root->advance(player.tick);
    // 3. frame scripts
    vm.runQueue();
    // 4. timers
    vm.runTimers(vm.nowMs());
    vm.runQueue();
    // 5. sounds that finished (SoundChannel soundComplete)
    as3PollSounds(player);
    vm.runQueue();
}


void as3PollSounds(Player& player) {
    VM& vm = *player.vm2;
    for (int handle : player.audio.takeFinished()) {
        auto it = channelRegistry().find(handle);
        if (it == channelRegistry().end()) continue;
        auto channel = it->second.lock();
        channelRegistry().erase(it);
        if (!channel) continue;
        if (auto c = std::dynamic_pointer_cast<ChannelData>(channel->native)) c->handle = 0;
        vm.dispatchEvent(channel, vm.makeEvent("flash.events::Event", "soundComplete", false));
    }
}

void as3MouseEvent(Player& player, DisplayObject* target, const char* type, bool bubbles, DisplayObject* related) {
    if (!target) return;
    VM& vm = *player.vm2;
    auto o = vm.objectFor(*target);
    auto ev = vm.makeEvent("flash.events::MouseEvent", type, bubbles);
    auto data = std::dynamic_pointer_cast<EventData>(ev->native);
    data->extra["stageX"] = Value(static_cast<double>(player.mouseX));
    data->extra["stageY"] = Value(static_cast<double>(player.mouseY));
    Matrix inv;
    float lx = player.mouseX * 20, ly = player.mouseY * 20;
    if (Clip* c = player.clipOf(target)) {
        if (c->worldMatrix().invert(inv)) inv.apply(player.mouseX * 20, player.mouseY * 20, lx, ly);
    }
    data->extra["localX"] = Value(lx / 20.0);
    data->extra["localY"] = Value(ly / 20.0);
    data->extra["buttonDown"] = Value(player.mouseDown);
    data->extra["relatedObject"] = related ? Value(vm.objectFor(*related)) : Value::null();
    vm.dispatchEvent(o, ev);
    vm.runQueue();
}

void as3KeyEvent(Player& player, int keyCode, bool down) {
    VM& vm = *player.vm2;
    if (!vm.stageObject) return;
    auto ev = vm.makeEvent("flash.events::KeyboardEvent", down ? "keyDown" : "keyUp", true);
    auto data = std::dynamic_pointer_cast<EventData>(ev->native);
    data->extra["keyCode"] = Value(keyCode);
    data->extra["charCode"] = Value(keyCode >= 'A' && keyCode <= 'Z' ? keyCode + 32 : keyCode);
    ObjectPtr target = player.rootHolder && player.rootHolder->as3 ? player.rootHolder->as3 : vm.stageObject;
    vm.dispatchEvent(target, ev);
    vm.runQueue();
}

// ---------------------------------------------------------------- flash.* classes

void installFlashImpl(VM& vm) {
    auto obj = vm.objectClass;

    // ---- flash.events
    auto dispatcher = vm.defineNativeClass("flash.events", "EventDispatcher", obj);
    ClassBuilder{vm, dispatcher}
        .init([](VM&, Object& o) { if (!o.native) o.native = std::make_shared<NativeData>(); })
        .method("addEventListener", [](VM& vm, const Value& self, Args& a) {
            if (!self.isObject()) return Value();
            vm.addListener(self.o, vm.toString(arg(a, 0)), arg(a, 1), static_cast<int>(num(vm, a, 3)), flag(vm, a, 2));
            return Value();
        })
        .method("removeEventListener", [](VM& vm, const Value& self, Args& a) {
            if (self.isObject()) vm.removeListener(self.o, vm.toString(arg(a, 0)), arg(a, 1), flag(vm, a, 2));
            return Value();
        })
        .method("dispatchEvent", [](VM& vm, const Value& self, Args& a) {
            const Value e = arg(a, 0);
            if (!self.isObject() || !e.isObject()) return Value(false);
            return Value(vm.dispatchEvent(self.o, e.o));
        })
        .method("hasEventListener", [](VM& vm, const Value& self, Args& a) {
            if (!self.isObject() || !self.o->native) return Value(false);
            auto it = self.o->native->listeners.find(vm.toString(arg(a, 0)));
            return Value(it != self.o->native->listeners.end() && !it->second.empty());
        })
        .method("willTrigger", [](VM& vm, const Value& self, Args& a) {
            if (!self.isObject() || !self.o->native) return Value(false);
            auto it = self.o->native->listeners.find(vm.toString(arg(a, 0)));
            return Value(it != self.o->native->listeners.end() && !it->second.empty());
        });

    auto event = vm.defineNativeClass("flash.events", "Event", obj);
    ClassBuilder eb{vm, event};
    eb.init([](VM&, Object& o) { o.native = std::make_shared<EventData>(); });
    eb.ctor([](VM& vm, const Value& self, Args& a) {
        if (auto e = eventOf(self)) {
            e->type = vm.toString(arg(a, 0));
            e->bubbles = flag(vm, a, 1);
            e->cancelable = flag(vm, a, 2);
        }
        return Value();
    });
    eb.getter("type", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return e ? Value(e->type) : Value(); })
        .getter("bubbles", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return Value(e && e->bubbles); })
        .getter("cancelable", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return Value(e && e->cancelable); })
        .getter("target", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return e ? e->target : Value::null(); })
        .getter("currentTarget", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return e ? e->currentTarget : Value::null(); })
        .getter("eventPhase", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return Value(e ? e->phase : 2); })
        .method("stopPropagation", [](VM&, const Value& self, Args&) { if (auto e = eventOf(self)) e->stop = true; return Value(); })
        .method("stopImmediatePropagation", [](VM&, const Value& self, Args&) { if (auto e = eventOf(self)) e->stop = e->stopNow = true; return Value(); })
        .method("preventDefault", [](VM&, const Value& self, Args&) { if (auto e = eventOf(self)) e->prevented = true; return Value(); })
        .method("isDefaultPrevented", [](VM&, const Value& self, Args&) { auto e = eventOf(self); return Value(e && e->prevented); })
        .method("clone", [](VM& vm, const Value& self, Args&) {
            auto e = eventOf(self);
            if (!e) return Value();
            Value copy = vm.construct(Value(self.o->cls), {Value(e->type), Value(e->bubbles), Value(e->cancelable)});
            if (auto c = eventOf(copy)) c->extra = e->extra;
            return copy;
        })
        .method("toString", [](VM&, const Value& self, Args&) {
            auto e = eventOf(self);
            return Value("[" + std::string(self.isObject() && self.o->cls ? self.o->cls->name : "Event") + " type=\"" + (e ? e->type : "") + "\"]");
        })
        .method("formatToString", [](VM& vm, const Value& self, Args& a) { return Value("[" + vm.toString(arg(a, 0)) + " type=\"" + vm.toString(vm.getPublic(self, "type")) + "\"]"); });
    for (auto [n, v] : std::initializer_list<std::pair<const char*, const char*>>{
             {"ENTER_FRAME", "enterFrame"}, {"EXIT_FRAME", "exitFrame"}, {"FRAME_CONSTRUCTED", "frameConstructed"},
             {"ADDED", "added"}, {"ADDED_TO_STAGE", "addedToStage"}, {"REMOVED", "removed"},
             {"REMOVED_FROM_STAGE", "removedFromStage"}, {"COMPLETE", "complete"}, {"INIT", "init"}, {"OPEN", "open"},
             {"CLOSE", "close"}, {"CHANGE", "change"}, {"RESIZE", "resize"}, {"ACTIVATE", "activate"},
             {"DEACTIVATE", "deactivate"}, {"RENDER", "render"}, {"SELECT", "select"}, {"SOUND_COMPLETE", "soundComplete"},
             {"UNLOAD", "unload"}, {"CANCEL", "cancel"}, {"CONNECT", "connect"}, {"SCROLL", "scroll"},
             {"MOUSE_LEAVE", "mouseLeave"}, {"FULLSCREEN", "fullScreen"}, {"ID3", "id3"}, {"TAB_CHILDREN_CHANGE", "tabChildrenChange"},
             {"TAB_ENABLED_CHANGE", "tabEnabledChange"}, {"TAB_INDEX_CHANGE", "tabIndexChange"}}) {
        eb.constant(n, Value(v));
    }

    // Event subclasses: their extra fields live in EventData::extra.
    auto eventSubclass = [&](const char* name, std::initializer_list<std::pair<const char*, const char*>> consts,
                             std::initializer_list<const char*> fields, std::initializer_list<const char*> ctorFields) {
        auto c = vm.defineNativeClass("flash.events", name, event);
        ClassBuilder b{vm, c};
        for (auto [n, v] : consts) b.constant(n, Value(v));
        for (const char* f : fields) {
            const std::string field = f;
            b.property(field,
                [field](VM&, const Value& self, Args&) { auto e = eventOf(self); if (!e) return Value(); auto it = e->extra.find(field); return it == e->extra.end() ? Value() : it->second; },
                [field](VM&, const Value& self, Args& a) { if (auto e = eventOf(self)) e->extra[field] = arg(a, 0); return Value(); });
        }
        std::vector<std::string> cf(ctorFields.begin(), ctorFields.end());
        b.ctor([cf](VM& vm, const Value& self, Args& a) {
            if (auto e = eventOf(self)) {
                e->type = vm.toString(arg(a, 0));
                e->bubbles = flag(vm, a, 1);
                e->cancelable = flag(vm, a, 2);
                for (std::size_t i = 0; i < cf.size(); ++i) if (a.size() > i + 3) e->extra[cf[i]] = a[i + 3];
            }
            return Value();
        });
        return c;
    };
    auto mouseEvent = eventSubclass("MouseEvent",
        {{"CLICK", "click"}, {"DOUBLE_CLICK", "doubleClick"}, {"MOUSE_DOWN", "mouseDown"}, {"MOUSE_UP", "mouseUp"},
         {"MOUSE_MOVE", "mouseMove"}, {"MOUSE_OVER", "mouseOver"}, {"MOUSE_OUT", "mouseOut"}, {"ROLL_OVER", "rollOver"},
         {"ROLL_OUT", "rollOut"}, {"MOUSE_WHEEL", "mouseWheel"}},
        {"localX", "localY", "stageX", "stageY", "relatedObject", "ctrlKey", "altKey", "shiftKey", "buttonDown", "delta"},
        {"localX", "localY", "relatedObject", "ctrlKey", "altKey", "shiftKey", "buttonDown", "delta"});
    ClassBuilder{vm, mouseEvent}.method("updateAfterEvent", [](VM&, const Value&, Args&) { return Value(); });
    eventSubclass("KeyboardEvent", {{"KEY_DOWN", "keyDown"}, {"KEY_UP", "keyUp"}},
                  {"charCode", "keyCode", "keyLocation", "ctrlKey", "altKey", "shiftKey"},
                  {"charCode", "keyCode", "keyLocation", "ctrlKey", "altKey", "shiftKey"});
    eventSubclass("TimerEvent", {{"TIMER", "timer"}, {"TIMER_COMPLETE", "timerComplete"}}, {}, {});
    auto textEvent = eventSubclass("TextEvent", {{"LINK", "link"}, {"TEXT_INPUT", "textInput"}}, {"text"}, {"text"});
    auto errorEvent = vm.defineNativeClass("flash.events", "ErrorEvent", textEvent);
    ClassBuilder{vm, errorEvent}.constant("ERROR", Value("error")).getter("errorID", [](VM&, const Value&, Args&) { return Value(0); });
    auto ioError = vm.defineNativeClass("flash.events", "IOErrorEvent", errorEvent);
    ClassBuilder{vm, ioError}.constant("IO_ERROR", Value("ioError")).constant("NETWORK_ERROR", Value("networkError"))
        .constant("DISK_ERROR", Value("diskError")).constant("VERIFY_ERROR", Value("verifyError"));
    auto secError = vm.defineNativeClass("flash.events", "SecurityErrorEvent", errorEvent);
    ClassBuilder{vm, secError}.constant("SECURITY_ERROR", Value("securityError"));
    auto asyncError = vm.defineNativeClass("flash.events", "AsyncErrorEvent", errorEvent);
    ClassBuilder{vm, asyncError}.constant("ASYNC_ERROR", Value("asyncError"));
    eventSubclass("HTTPStatusEvent", {{"HTTP_STATUS", "httpStatus"}, {"HTTP_RESPONSE_STATUS", "httpResponseStatus"}}, {"status"}, {"status"});
    eventSubclass("ProgressEvent", {{"PROGRESS", "progress"}, {"SOCKET_DATA", "socketData"}}, {"bytesLoaded", "bytesTotal"}, {"bytesLoaded", "bytesTotal"});
    eventSubclass("FocusEvent", {{"FOCUS_IN", "focusIn"}, {"FOCUS_OUT", "focusOut"}, {"KEY_FOCUS_CHANGE", "keyFocusChange"}, {"MOUSE_FOCUS_CHANGE", "mouseFocusChange"}},
                  {"relatedObject", "shiftKey", "keyCode"}, {"relatedObject", "shiftKey", "keyCode"});
    eventSubclass("StatusEvent", {{"STATUS", "status"}}, {"code", "level"}, {"code", "level"});
    eventSubclass("NetStatusEvent", {{"NET_STATUS", "netStatus"}}, {"info"}, {"info"});
    eventSubclass("ContextMenuEvent", {{"MENU_ITEM_SELECT", "menuItemSelect"}, {"MENU_SELECT", "menuSelect"}}, {"mouseTarget", "contextMenuOwner"}, {"mouseTarget", "contextMenuOwner"});
    eventSubclass("DataEvent", {{"DATA", "data"}, {"UPLOAD_COMPLETE_DATA", "uploadCompleteData"}}, {"data"}, {"data"});
    auto eventPhase = vm.defineNativeClass("flash.events", "EventPhase", obj);
    ClassBuilder{vm, eventPhase}.constant("CAPTURING_PHASE", Value(1)).constant("AT_TARGET", Value(2)).constant("BUBBLING_PHASE", Value(3));
    vm.defineNativeClass("flash.events", "IEventDispatcher", obj)->isInterface = true;

    // ---- flash.geom
    auto point = vm.defineNativeClass("flash.geom", "Point", obj);
    ClassBuilder pb{vm, point};
    pb.ctor([](VM& vm, const Value& self, Args& a) {
        if (self.isObject()) { self.o->dynamic.set("x", Value(num(vm, a, 0))); self.o->dynamic.set("y", Value(num(vm, a, 1))); }
        return Value();
    });
    auto px = [](VM& vm, const Value& p) { return vm.toNumber(vm.getPublic(p, "x")); };
    auto py = [](VM& vm, const Value& p) { return vm.toNumber(vm.getPublic(p, "y")); };
    pb.getter("length", [px, py](VM& vm, const Value& self, Args&) { return Value(std::hypot(px(vm, self), py(vm, self))); })
        .method("add", [px, py](VM& vm, const Value& self, Args& a) { return makePoint(vm, px(vm, self) + px(vm, arg(a, 0)), py(vm, self) + py(vm, arg(a, 0))); })
        .method("subtract", [px, py](VM& vm, const Value& self, Args& a) { return makePoint(vm, px(vm, self) - px(vm, arg(a, 0)), py(vm, self) - py(vm, arg(a, 0))); })
        .method("clone", [px, py](VM& vm, const Value& self, Args&) { return makePoint(vm, px(vm, self), py(vm, self)); })
        .method("equals", [px, py](VM& vm, const Value& self, Args& a) { return Value(px(vm, self) == px(vm, arg(a, 0)) && py(vm, self) == py(vm, arg(a, 0))); })
        .method("offset", [px, py](VM& vm, const Value& self, Args& a) {
            vm.setPublic(self, "x", Value(px(vm, self) + num(vm, a, 0)));
            vm.setPublic(self, "y", Value(py(vm, self) + num(vm, a, 1)));
            return Value();
        })
        .method("normalize", [px, py](VM& vm, const Value& self, Args& a) {
            const double l = std::hypot(px(vm, self), py(vm, self));
            if (l > 0) {
                const double k = num(vm, a, 0, 1) / l;
                vm.setPublic(self, "x", Value(px(vm, self) * k));
                vm.setPublic(self, "y", Value(py(vm, self) * k));
            }
            return Value();
        })
        .method("toString", [px, py](VM& vm, const Value& self, Args&) {
            return Value("(x=" + vm.toString(Value(px(vm, self))) + ", y=" + vm.toString(Value(py(vm, self))) + ")");
        })
        .staticMethod("distance", [px, py](VM& vm, const Value&, Args& a) {
            return Value(std::hypot(px(vm, arg(a, 0)) - px(vm, arg(a, 1)), py(vm, arg(a, 0)) - py(vm, arg(a, 1))));
        })
        .staticMethod("interpolate", [px, py](VM& vm, const Value&, Args& a) {
            const double f = num(vm, a, 2);
            return makePoint(vm, px(vm, arg(a, 1)) + (px(vm, arg(a, 0)) - px(vm, arg(a, 1))) * f,
                             py(vm, arg(a, 1)) + (py(vm, arg(a, 0)) - py(vm, arg(a, 1))) * f);
        })
        .staticMethod("polar", [](VM& vm, const Value&, Args& a) {
            const double len = num(vm, a, 0), ang = num(vm, a, 1);
            return makePoint(vm, len * std::cos(ang), len * std::sin(ang));
        });
    point->sealed = false;

    auto rect = vm.defineNativeClass("flash.geom", "Rectangle", obj);
    ClassBuilder rb{vm, rect};
    rb.ctor([](VM& vm, const Value& self, Args& a) {
        if (self.isObject()) {
            self.o->dynamic.set("x", Value(num(vm, a, 0)));
            self.o->dynamic.set("y", Value(num(vm, a, 1)));
            self.o->dynamic.set("width", Value(num(vm, a, 2)));
            self.o->dynamic.set("height", Value(num(vm, a, 3)));
        }
        return Value();
    });
    auto rf = [](VM& vm, const Value& r, const char* f) { return vm.toNumber(vm.getPublic(r, f)); };
    rb.getter("left", [rf](VM& vm, const Value& s, Args&) { return Value(rf(vm, s, "x")); })
        .getter("top", [rf](VM& vm, const Value& s, Args&) { return Value(rf(vm, s, "y")); })
        .getter("right", [rf](VM& vm, const Value& s, Args&) { return Value(rf(vm, s, "x") + rf(vm, s, "width")); })
        .getter("bottom", [rf](VM& vm, const Value& s, Args&) { return Value(rf(vm, s, "y") + rf(vm, s, "height")); })
        .method("contains", [rf](VM& vm, const Value& s, Args& a) {
            const double x = num(vm, a, 0), y = num(vm, a, 1);
            return Value(x >= rf(vm, s, "x") && x < rf(vm, s, "x") + rf(vm, s, "width") && y >= rf(vm, s, "y") && y < rf(vm, s, "y") + rf(vm, s, "height"));
        })
        .method("containsPoint", [rf](VM& vm, const Value& s, Args& a) {
            const double x = vm.toNumber(vm.getPublic(arg(a, 0), "x")), y = vm.toNumber(vm.getPublic(arg(a, 0), "y"));
            return Value(x >= rf(vm, s, "x") && x < rf(vm, s, "x") + rf(vm, s, "width") && y >= rf(vm, s, "y") && y < rf(vm, s, "y") + rf(vm, s, "height"));
        })
        .method("intersects", [rf](VM& vm, const Value& s, Args& a) {
            const Value o = arg(a, 0);
            return Value(rf(vm, s, "x") < rf(vm, o, "x") + rf(vm, o, "width") && rf(vm, o, "x") < rf(vm, s, "x") + rf(vm, s, "width") &&
                         rf(vm, s, "y") < rf(vm, o, "y") + rf(vm, o, "height") && rf(vm, o, "y") < rf(vm, s, "y") + rf(vm, s, "height"));
        })
        .method("clone", [rf](VM& vm, const Value& s, Args&) { return makeRectangle(vm, rf(vm, s, "x"), rf(vm, s, "y"), rf(vm, s, "width"), rf(vm, s, "height")); })
        .method("isEmpty", [rf](VM& vm, const Value& s, Args&) { return Value(rf(vm, s, "width") <= 0 || rf(vm, s, "height") <= 0); })
        .method("toString", [rf](VM& vm, const Value& s, Args&) {
            return Value("(x=" + vm.toString(Value(rf(vm, s, "x"))) + ", y=" + vm.toString(Value(rf(vm, s, "y"))) + ", w=" +
                         vm.toString(Value(rf(vm, s, "width"))) + ", h=" + vm.toString(Value(rf(vm, s, "height"))) + ")");
        });
    rect->sealed = false;

    auto matrix = vm.defineNativeClass("flash.geom", "Matrix", obj);
    ClassBuilder mb{vm, matrix};
    mb.ctor([](VM& vm, const Value& self, Args& a) {
        if (self.isObject()) {
            const char* f[] = {"a", "b", "c", "d", "tx", "ty"};
            const double def[] = {1, 0, 0, 1, 0, 0};
            for (int i = 0; i < 6; ++i) self.o->dynamic.set(f[i], Value(num(vm, a, static_cast<std::size_t>(i), def[i])));
        }
        return Value();
    });
    auto getM = [](VM& vm, const Value& m) {
        return Matrix{static_cast<float>(vm.toNumber(vm.getPublic(m, "a"))), static_cast<float>(vm.toNumber(vm.getPublic(m, "b"))),
                      static_cast<float>(vm.toNumber(vm.getPublic(m, "c"))), static_cast<float>(vm.toNumber(vm.getPublic(m, "d"))),
                      static_cast<float>(vm.toNumber(vm.getPublic(m, "tx"))), static_cast<float>(vm.toNumber(vm.getPublic(m, "ty")))};
    };
    auto setM = [](VM& vm, const Value& m, const Matrix& v) {
        vm.setPublic(m, "a", Value(static_cast<double>(v.a))); vm.setPublic(m, "b", Value(static_cast<double>(v.b)));
        vm.setPublic(m, "c", Value(static_cast<double>(v.c))); vm.setPublic(m, "d", Value(static_cast<double>(v.d)));
        vm.setPublic(m, "tx", Value(static_cast<double>(v.tx))); vm.setPublic(m, "ty", Value(static_cast<double>(v.ty)));
    };
    mb.method("identity", [setM](VM& vm, const Value& s, Args&) { setM(vm, s, Matrix{}); return Value(); })
        .method("translate", [getM, setM](VM& vm, const Value& s, Args& a) {
            auto m = getM(vm, s);
            m.tx += static_cast<float>(num(vm, a, 0)); m.ty += static_cast<float>(num(vm, a, 1));
            setM(vm, s, m);
            return Value();
        })
        .method("scale", [getM, setM](VM& vm, const Value& s, Args& a) {
            const float sx = static_cast<float>(num(vm, a, 0)), sy = static_cast<float>(num(vm, a, 1));
            setM(vm, s, Matrix{sx, 0, 0, sy, 0, 0} * getM(vm, s));
            return Value();
        })
        .method("rotate", [getM, setM](VM& vm, const Value& s, Args& a) {
            const float r = static_cast<float>(num(vm, a, 0));
            setM(vm, s, Matrix{std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0} * getM(vm, s));
            return Value();
        })
        .method("concat", [getM, setM](VM& vm, const Value& s, Args& a) { setM(vm, s, getM(vm, arg(a, 0)) * getM(vm, s)); return Value(); })
        .method("invert", [getM, setM](VM& vm, const Value& s, Args&) { Matrix inv; if (getM(vm, s).invert(inv)) setM(vm, s, inv); return Value(); })
        .method("transformPoint", [getM](VM& vm, const Value& s, Args& a) {
            float x, y;
            getM(vm, s).apply(static_cast<float>(vm.toNumber(vm.getPublic(arg(a, 0), "x"))), static_cast<float>(vm.toNumber(vm.getPublic(arg(a, 0), "y"))), x, y);
            return makePoint(vm, x, y);
        })
        .method("clone", [getM](VM& vm, const Value& s, Args&) {
            const auto m = getM(vm, s);
            return vm.construct(vm.getPublic(Value(vm.findClass("flash.geom::Matrix")), "constructor").isUndefined() ? Value(vm.findClass("flash.geom::Matrix")) : Value(vm.findClass("flash.geom::Matrix")),
                                {Value(static_cast<double>(m.a)), Value(static_cast<double>(m.b)), Value(static_cast<double>(m.c)), Value(static_cast<double>(m.d)),
                                 Value(static_cast<double>(m.tx)), Value(static_cast<double>(m.ty))});
        });
    matrix->sealed = false;

    auto colorTransform = vm.defineNativeClass("flash.geom", "ColorTransform", obj);
    ClassBuilder cb{vm, colorTransform};
    const char* ctFields[] = {"redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier", "redOffset", "greenOffset", "blueOffset", "alphaOffset"};
    cb.ctor([ctFields](VM& vm, const Value& self, Args& a) {
        if (self.isObject()) {
            const double def[] = {1, 1, 1, 1, 0, 0, 0, 0};
            for (int i = 0; i < 8; ++i) self.o->dynamic.set(ctFields[i], Value(num(vm, a, static_cast<std::size_t>(i), def[i])));
        }
        return Value();
    });
    cb.property("color",
        [](VM& vm, const Value& s, Args&) {
            const int r = static_cast<int>(vm.toNumber(vm.getPublic(s, "redOffset"))), g = static_cast<int>(vm.toNumber(vm.getPublic(s, "greenOffset"))),
                      b = static_cast<int>(vm.toNumber(vm.getPublic(s, "blueOffset")));
            return Value(static_cast<double>(((r & 255) << 16) | ((g & 255) << 8) | (b & 255)));
        },
        [](VM& vm, const Value& s, Args& a) {
            const std::uint32_t c = vm.toUint32(arg(a, 0));
            vm.setPublic(s, "redMultiplier", Value(0)); vm.setPublic(s, "greenMultiplier", Value(0)); vm.setPublic(s, "blueMultiplier", Value(0));
            vm.setPublic(s, "redOffset", Value(static_cast<double>((c >> 16) & 255)));
            vm.setPublic(s, "greenOffset", Value(static_cast<double>((c >> 8) & 255)));
            vm.setPublic(s, "blueOffset", Value(static_cast<double>(c & 255)));
            return Value();
        });
    colorTransform->sealed = false;

    auto readCx = [ctFields](VM& vm, const Value& v) {
        ColorTransform c;
        for (int i = 0; i < 4; ++i) c.mul[i] = static_cast<float>(vm.toNumber(vm.getPublic(v, ctFields[i])));
        for (int i = 0; i < 4; ++i) c.add[i] = static_cast<float>(vm.toNumber(vm.getPublic(v, ctFields[i + 4])));
        return c;
    };
    auto writeCx = [ctFields](VM& vm, const ColorTransform& c) {
        Args a;
        for (int i = 0; i < 4; ++i) a.emplace_back(static_cast<double>(c.mul[i]));
        for (int i = 0; i < 4; ++i) a.emplace_back(static_cast<double>(c.add[i]));
        (void)ctFields;
        return vm.construct(Value(vm.findClass("flash.geom::ColorTransform")), a);
    };

    // Transform: a view onto a display object's matrix and colour transform.
    struct TransformData : NativeData { std::weak_ptr<DisplayObject> d; };
    auto transform = vm.defineNativeClass("flash.geom", "Transform", obj);
    ClassBuilder{vm, transform}
        .init([](VM&, Object& o) { o.native = std::make_shared<TransformData>(); })
        .ctor([](VM&, const Value& self, Args& a) {
            auto t = std::dynamic_pointer_cast<TransformData>(self.o->native);
            if (auto* d = disp(arg(a, 0))) t->d = d->shared_from_this();
            return Value();
        })
        .property("colorTransform",
            [writeCx](VM& vm, const Value& s, Args&) {
                auto t = std::dynamic_pointer_cast<TransformData>(s.o->native);
                auto d = t ? t->d.lock() : nullptr;
                return writeCx(vm, d ? d->cxform : ColorTransform{});
            },
            [readCx](VM& vm, const Value& s, Args& a) {
                auto t = std::dynamic_pointer_cast<TransformData>(s.o->native);
                if (auto d = t ? t->d.lock() : nullptr) d->cxform = readCx(vm, arg(a, 0));
                return Value();
            })
        .property("matrix",
            [](VM& vm, const Value& s, Args&) {
                auto t = std::dynamic_pointer_cast<TransformData>(s.o->native);
                auto d = t ? t->d.lock() : nullptr;
                const Matrix m = d ? d->matrix : Matrix{};
                return vm.construct(Value(vm.findClass("flash.geom::Matrix")),
                                    {Value(static_cast<double>(m.a)), Value(static_cast<double>(m.b)), Value(static_cast<double>(m.c)),
                                     Value(static_cast<double>(m.d)), Value(m.tx / 20.0), Value(m.ty / 20.0)});
            },
            [getM](VM& vm, const Value& s, Args& a) {
                auto t = std::dynamic_pointer_cast<TransformData>(s.o->native);
                if (auto d = t ? t->d.lock() : nullptr) {
                    Matrix m = getM(vm, arg(a, 0));
                    m.tx *= 20; m.ty *= 20;
                    d->matrix = m;
                    d->cachedTransform = false;
                    d->dynamicTransform = true;
                }
                return Value();
            })
        .getter("concatenatedMatrix", [](VM& vm, const Value& s, Args&) {
            auto t = std::dynamic_pointer_cast<TransformData>(s.o->native);
            auto d = t ? t->d.lock() : nullptr;
            Matrix m = d ? d->matrix : Matrix{};
            if (d && d->parent) m = d->parent->worldMatrix() * m;
            return vm.construct(Value(vm.findClass("flash.geom::Matrix")),
                                {Value(static_cast<double>(m.a)), Value(static_cast<double>(m.b)), Value(static_cast<double>(m.c)),
                                 Value(static_cast<double>(m.d)), Value(m.tx / 20.0), Value(m.ty / 20.0)});
        });

    // ---- flash.display
    auto displayObject = vm.defineNativeClass("flash.display", "DisplayObject", dispatcher);
    ClassBuilder dob{vm, displayObject};
    dob.init([](VM& vm, Object& o) {
        auto n = std::make_shared<DisplayNative>();
        auto self = o.shared_from_this();
        if (vm.pendingDisplay) {
            n->d = vm.pendingDisplay;
            vm.pendingDisplay.reset();
            n->d->as3 = self;
        } else {
            // Created by `new`: build the display object, with the symbol's timeline when the class is linked.
            auto d = std::make_shared<DisplayObject>();
            n->d = d;
            d->as3 = self;
            d->as3Constructed = true; // VM::construct runs the constructor right after this
            std::uint16_t character = 0;
            auto& ids = symbolIds(vm.player);
            for (Class* c = o.cls.get(); c && !character; c = c->super.get()) {
                auto it = ids.find(c->qualifiedName());
                if (it != ids.end() && it->second) character = it->second;
            }
            o.native = n; // children built below may look the parent up
            if (character) {
                vm.player.setupDisplay(*d, character, nullptr);
            } else if (classDerivesFrom(o.cls.get(), "Sprite") || classDerivesFrom(o.cls.get(), "Loader") ||
                       classDerivesFrom(o.cls.get(), "Stage")) {
                vm.player.setupDisplay(*d, 0xffff, nullptr);
            } else if (classDerivesFrom(o.cls.get(), "TextField")) {
                d->kind = DisplayObject::Kind::Text;
                d->ownText = defaultTextDef();
            } else if (classDerivesFrom(o.cls.get(), "SimpleButton")) {
                d->kind = DisplayObject::Kind::Button;
            } else {
                d->kind = DisplayObject::Kind::Shape;
            }
        }
        o.native = n;
    });
    auto numProp = [&](const char* name, std::function<double(DisplayObject&)> get, std::function<void(DisplayObject&, double)> set) {
        dob.property(name,
            [get](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d ? get(*d) : 0.0); },
            [set](VM& vm, const Value& s, Args& a) {
                auto* d = disp(s);
                const double v = vm.toNumber(arg(a, 0));
                if (d && !std::isnan(v)) set(*d, v);
                return Value();
            });
    };
    numProp("x", [](DisplayObject& d) { return d.matrix.tx / 20.0; }, [](DisplayObject& d, double v) { d.matrix.tx = static_cast<float>(v * 20); d.dynamicTransform = true; });
    numProp("y", [](DisplayObject& d) { return d.matrix.ty / 20.0; }, [](DisplayObject& d, double v) { d.matrix.ty = static_cast<float>(v * 20); d.dynamicTransform = true; });
    numProp("scaleX", [](DisplayObject& d) { d.decompose(); return d.xscale / 100.0; },
            [](DisplayObject& d, double v) { d.decompose(); d.xscale = v * 100; d.recompose(); d.dynamicTransform = true; });
    numProp("scaleY", [](DisplayObject& d) { d.decompose(); return d.yscale / 100.0; },
            [](DisplayObject& d, double v) { d.decompose(); d.yscale = v * 100; d.recompose(); d.dynamicTransform = true; });
    numProp("rotation", [](DisplayObject& d) { d.decompose(); return d.rotation; },
            [](DisplayObject& d, double v) { d.decompose(); d.rotation = std::remainder(v, 360.0); d.recompose(); d.dynamicTransform = true; });
    numProp("alpha", [](DisplayObject& d) { return static_cast<double>(d.cxform.mul[3]); }, [](DisplayObject& d, double v) { d.cxform.mul[3] = static_cast<float>(v); });
    auto sizeProp = [&](const char* name, bool width) {
        dob.property(name,
            [width](VM& vm, const Value& s, Args&) {
                auto* d = disp(s);
                if (!d) return Value(0);
                float b[4];
                if (!vm.player.geometry.bounds(*d, Matrix{}, b)) return Value(0);
                // bounds() applies the object's own matrix: size in the parent's space
                return Value((width ? b[2] - b[0] : b[3] - b[1]) / 20.0);
            },
            [width](VM& vm, const Value& s, Args& a) {
                auto* d = disp(s);
                const double v = vm.toNumber(arg(a, 0));
                if (!d || std::isnan(v)) return Value();
                float b[4];
                if (!vm.player.geometry.bounds(*d, Matrix{}, b)) return Value();
                const double cur = (width ? b[2] - b[0] : b[3] - b[1]) / 20.0;
                if (cur <= 0) return Value();
                d->decompose();
                if (width) d->xscale *= v / cur; else d->yscale *= v / cur;
                d->recompose();
                d->dynamicTransform = true;
                return Value();
            });
    };
    sizeProp("width", true);
    sizeProp("height", false);
    dob.property("visible",
        [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d && d->visible); },
        [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->visible = vm.toBoolean(arg(a, 0)); return Value(); });
    dob.property("name",
        [](VM&, const Value& s, Args&) { auto* d = disp(s); return d ? Value(d->name) : Value::null(); },
        [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->name = vm.toString(arg(a, 0)); return Value(); });
    dob.getter("parent", [](VM& vm, const Value& s, Args&) {
        auto* d = disp(s);
        if (!d) return Value::null();
        if (d == vm.player.rootHolder.get()) return vm.stageObject ? Value(vm.stageObject) : Value::null();
        if (!d->parent || !d->parent->holder()) return Value::null();
        return displayValue(vm, d->parent->holder());
    });
    dob.getter("root", [](VM& vm, const Value& s, Args&) {
        auto* d = disp(s);
        return vm.player.onStage(d) && vm.player.rootHolder ? displayValue(vm, vm.player.rootHolder.get()) : Value::null();
    });
    dob.getter("stage", [](VM& vm, const Value& s, Args&) {
        auto* d = disp(s);
        return vm.player.onStage(d) && vm.stageObject ? Value(vm.stageObject) : Value::null();
    });
    auto mouseCoord = [](bool x) {
        return [x](VM& vm, const Value& s, Args&) {
            auto* d = disp(s);
            Matrix m = d && d->parent ? d->parent->worldMatrix() * d->matrix : (d ? d->matrix : Matrix{});
            Matrix inv;
            float lx = vm.player.mouseX * 20, ly = vm.player.mouseY * 20;
            if (m.invert(inv)) inv.apply(vm.player.mouseX * 20, vm.player.mouseY * 20, lx, ly);
            return Value((x ? lx : ly) / 20.0);
        };
    };
    dob.getter("mouseX", mouseCoord(true)).getter("mouseY", mouseCoord(false));
    dob.getter("transform", [](VM& vm, const Value& s, Args&) { return vm.construct(Value(vm.findClass("flash.geom::Transform")), {s}); });
    dob.setter("transform", [readCx, getM](VM& vm, const Value& s, Args& a) {
        if (auto* d = disp(s)) {
            d->cxform = readCx(vm, vm.getPublic(arg(a, 0), "colorTransform"));
            Matrix m = getM(vm, vm.getPublic(arg(a, 0), "matrix"));
            m.tx *= 20; m.ty *= 20;
            d->matrix = m;
            d->cachedTransform = false;
        }
        return Value();
    });
    // Accepted but not rendered.
    for (const char* stored : {"filters", "blendMode", "cacheAsBitmap", "mask", "scrollRect", "opaqueBackground", "scale9Grid", "accessibilityProperties"}) {
        const std::string key = std::string("__") + stored;
        dob.property(stored,
            [key](VM&, const Value& s, Args&) { Value* v = s.isObject() ? s.o->dynamic.find(key) : nullptr; return v ? *v : Value::null(); },
            [key](VM&, const Value& s, Args& a) { if (s.isObject()) s.o->dynamic.set(key, arg(a, 0)); return Value(); });
    }
    dob.getter("loaderInfo", [](VM& vm, const Value&, Args&) {
        static ObjectPtr info;
        if (!info) info = vm.construct(Value(vm.findClass("flash.display::LoaderInfo")), {}).o;
        return Value(info);
    });
    dob.method("localToGlobal", [](VM& vm, const Value& s, Args& a) {
        auto* d = disp(s);
        Clip* c = vm.player.clipOf(d);
        Matrix m = c ? c->worldMatrix() : (d && d->parent ? d->parent->worldMatrix() * d->matrix : Matrix{});
        float x, y;
        m.apply(static_cast<float>(vm.toNumber(vm.getPublic(arg(a, 0), "x")) * 20), static_cast<float>(vm.toNumber(vm.getPublic(arg(a, 0), "y")) * 20), x, y);
        return makePoint(vm, x / 20.0, y / 20.0);
    });
    dob.method("globalToLocal", [](VM& vm, const Value& s, Args& a) {
        auto* d = disp(s);
        Clip* c = vm.player.clipOf(d);
        Matrix m = c ? c->worldMatrix() : (d && d->parent ? d->parent->worldMatrix() * d->matrix : Matrix{});
        Matrix inv;
        float x = static_cast<float>(vm.toNumber(vm.getPublic(arg(a, 0), "x")) * 20), y = static_cast<float>(vm.toNumber(vm.getPublic(arg(a, 0), "y")) * 20), ox = x, oy = y;
        if (m.invert(inv)) inv.apply(x, y, ox, oy);
        return makePoint(vm, ox / 20.0, oy / 20.0);
    });
    auto boundsIn = [](VM& vm, const Value& s, Args& a) {
        auto* d = disp(s);
        if (!d) return makeRectangle(vm, 0, 0, 0, 0);
        Matrix world = d->parent ? d->parent->worldMatrix() : Matrix{};
        Matrix toSpace;
        if (auto* space = disp(arg(a, 0))) {
            Clip* sc = vm.player.clipOf(space);
            Matrix sw = sc ? sc->worldMatrix() : (space->parent ? space->parent->worldMatrix() * space->matrix : space->matrix);
            Matrix inv;
            if (sw.invert(inv)) toSpace = inv;
        }
        float b[4];
        if (!vm.player.geometry.bounds(*d, toSpace * world, b)) return makeRectangle(vm, 0, 0, 0, 0);
        return makeRectangle(vm, b[0] / 20.0, b[1] / 20.0, (b[2] - b[0]) / 20.0, (b[3] - b[1]) / 20.0);
    };
    dob.method("getBounds", boundsIn).method("getRect", boundsIn);
    dob.method("hitTestPoint", [](VM& vm, const Value& s, Args& a) {
        auto* d = disp(s);
        if (!d) return Value(false);
        Matrix world = d->parent ? d->parent->worldMatrix() : Matrix{};
        const float x = static_cast<float>(num(vm, a, 0) * 20), y = static_cast<float>(num(vm, a, 1) * 20);
        if (flag(vm, a, 2)) return Value(vm.player.geometry.hit(*d, world, x, y, true, false));
        float b[4];
        return Value(vm.player.geometry.bounds(*d, world, b) && x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]);
    });
    dob.method("hitTestObject", [](VM& vm, const Value& s, Args& a) {
        auto* d = disp(s);
        auto* o = disp(arg(a, 0));
        if (!d || !o) return Value(false);
        float b1[4], b2[4];
        if (!vm.player.geometry.bounds(*d, d->parent ? d->parent->worldMatrix() : Matrix{}, b1)) return Value(false);
        if (!vm.player.geometry.bounds(*o, o->parent ? o->parent->worldMatrix() : Matrix{}, b2)) return Value(false);
        return Value(b1[0] <= b2[2] && b2[0] <= b1[2] && b1[1] <= b2[3] && b2[1] <= b1[3]);
    });

    auto interactive = vm.defineNativeClass("flash.display", "InteractiveObject", displayObject);
    ClassBuilder ib{vm, interactive};
    ib.property("mouseEnabled",
        [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d && d->mouseEnabled); },
        [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->mouseEnabled = vm.toBoolean(arg(a, 0)); return Value(); });
    for (const char* stored : {"doubleClickEnabled", "tabEnabled", "tabIndex", "focusRect", "contextMenu", "accessibilityImplementation"}) {
        const std::string key = std::string("__") + stored;
        ib.property(stored,
            [key](VM&, const Value& s, Args&) { Value* v = s.isObject() ? s.o->dynamic.find(key) : nullptr; return v ? *v : Value(); },
            [key](VM&, const Value& s, Args& a) { if (s.isObject()) s.o->dynamic.set(key, arg(a, 0)); return Value(); });
    }

    auto container = vm.defineNativeClass("flash.display", "DisplayObjectContainer", interactive);
    ClassBuilder ctb{vm, container};
    auto addChildAt = [](VM& vm, const Value& self, const Value& child, int index) -> Value {
        Clip* c = clipOfValue(vm, self);
        auto* cd = disp(child);
        if (!c || !cd) vm.throwError("TypeError", "addChild: parameter child must be non-null");
        auto keep = cd->shared_from_this();
        const bool wasOnStage = vm.player.onStage(cd);
        if (cd->parent) {
            if (cd->parent == c && index >= 0) {
                c->setChildIndex(cd, std::min(index, c->numChildren() - 1));
                return child;
            }
            if (cd->parent == c && index < 0) {
                c->setChildIndex(cd, c->numChildren() - 1);
                return child;
            }
            cd->parent->detach(cd);
        }
        c->addChildAt(keep, index);
        vm.dispatchEvent(child.o, vm.makeEvent("flash.events::Event", "added", true));
        if (!wasOnStage && vm.player.onStage(cd)) {
            // addedToStage goes to the object and all of its descendants
            std::function<void(DisplayObject&)> notify = [&](DisplayObject& o) {
                vm.dispatchEvent(vm.objectFor(o), vm.makeEvent("flash.events::Event", "addedToStage", false));
                if (Clip* oc = vm.player.clipOf(&o)) {
                    std::vector<std::shared_ptr<DisplayObject>> kids;
                    for (auto& [dd, k] : oc->children()) kids.push_back(k);
                    for (auto& k : kids) notify(*k);
                }
            };
            notify(*cd);
        }
        return child;
    };
    auto removeChild = [](VM& vm, const Value& self, DisplayObject* cd) -> Value {
        Clip* c = clipOfValue(vm, self);
        if (!c || !cd || cd->parent != c) vm.throwError("ArgumentError", "The supplied DisplayObject must be a child of the caller.");
        auto keep = cd->shared_from_this();
        Value childV(vm.objectFor(*cd));
        vm.dispatchEvent(childV.o, vm.makeEvent("flash.events::Event", "removed", true));
        if (vm.player.onStage(cd)) vm.dispatchEvent(childV.o, vm.makeEvent("flash.events::Event", "removedFromStage", false));
        c->detach(cd);
        if (vm.player.as3Hover.get() == cd) vm.player.as3Hover.reset();
        return childV;
    };
    ctb.getter("numChildren", [](VM& vm, const Value& s, Args&) { Clip* c = clipOfValue(vm, s); return Value(c ? c->numChildren() : 0); })
        .method("addChild", [addChildAt](VM& vm, const Value& s, Args& a) { return addChildAt(vm, s, arg(a, 0), -1); })
        .method("addChildAt", [addChildAt](VM& vm, const Value& s, Args& a) { return addChildAt(vm, s, arg(a, 0), vm.toInt32(arg(a, 1))); })
        .method("removeChild", [removeChild](VM& vm, const Value& s, Args& a) { return removeChild(vm, s, disp(arg(a, 0))); })
        .method("removeChildAt", [removeChild](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            DisplayObject* d = c ? c->childAtIndex(vm.toInt32(arg(a, 0))) : nullptr;
            if (!d) vm.throwError("RangeError", "The supplied index is out of bounds.");
            return removeChild(vm, s, d);
        })
        .method("removeChildren", [removeChild](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            if (!c) return Value();
            const int from = static_cast<int>(num(vm, a, 0)), to = static_cast<int>(num(vm, a, 1, 0x7fffffff));
            std::vector<DisplayObject*> victims;
            for (int i = from; i <= std::min(to, c->numChildren() - 1); ++i) victims.push_back(c->childAtIndex(i));
            for (auto* d : victims) removeChild(vm, s, d);
            return Value();
        })
        .method("getChildAt", [](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            DisplayObject* d = c ? c->childAtIndex(vm.toInt32(arg(a, 0))) : nullptr;
            if (!d) vm.throwError("RangeError", "The supplied index is out of bounds.");
            return displayValue(vm, d);
        })
        .method("getChildByName", [](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            DisplayObject* d = c ? c->childByName(vm.toString(arg(a, 0))) : nullptr;
            return displayValue(vm, d);
        })
        .method("getChildIndex", [](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            const int i = c ? c->indexOf(disp(arg(a, 0))) : -1;
            if (i < 0) vm.throwError("ArgumentError", "The supplied DisplayObject must be a child of the caller.");
            return Value(i);
        })
        .method("setChildIndex", [](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            auto* d = disp(arg(a, 0));
            if (c && d && d->parent == c) c->setChildIndex(d, vm.toInt32(arg(a, 1)));
            return Value();
        })
        .method("swapChildren", [](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            auto* d1 = disp(arg(a, 0));
            auto* d2 = disp(arg(a, 1));
            if (!c || !d1 || !d2) return Value();
            const int i1 = c->indexOf(d1), i2 = c->indexOf(d2);
            if (i1 < 0 || i2 < 0) return Value();
            c->setChildIndex(d1, i2);
            c->setChildIndex(d2, i1);
            return Value();
        })
        .method("swapChildrenAt", [](VM& vm, const Value& s, Args& a) {
            Clip* c = clipOfValue(vm, s);
            if (!c) return Value();
            const int i1 = vm.toInt32(arg(a, 0)), i2 = vm.toInt32(arg(a, 1));
            auto* d1 = c->childAtIndex(i1);
            auto* d2 = c->childAtIndex(i2);
            if (!d1 || !d2) return Value();
            c->setChildIndex(d1, i2);
            c->setChildIndex(d2, i1);
            return Value();
        })
        .method("contains", [](VM& vm, const Value& s, Args& a) {
            auto* self = disp(s);
            for (auto* d = disp(arg(a, 0)); d; d = d->parent ? d->parent->holder() : nullptr) if (d == self) return Value(true);
            (void)vm;
            return Value(false);
        })
        .property("mouseChildren",
            [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d && d->mouseChildren); },
            [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->mouseChildren = vm.toBoolean(arg(a, 0)); return Value(); })
        .property("tabChildren", [](VM&, const Value&, Args&) { return Value(true); }, [](VM&, const Value&, Args&) { return Value(); })
        .method("getObjectsUnderPoint", [](VM& vm, const Value&, Args&) { return Value(vm.newArray()); })
        .method("areInaccessibleObjectsUnderPoint", [](VM&, const Value&, Args&) { return Value(false); });

    // Graphics: drawing is accepted but not rendered.
    auto graphics = vm.defineNativeClass("flash.display", "Graphics", obj);
    for (const char* m : {"clear", "beginFill", "beginGradientFill", "beginBitmapFill", "lineStyle", "lineGradientStyle", "moveTo",
                          "lineTo", "curveTo", "cubicCurveTo", "endFill", "drawRect", "drawRoundRect", "drawCircle", "drawEllipse",
                          "drawPath", "drawTriangles", "copyFrom"}) {
        ClassBuilder{vm, graphics}.method(m, [](VM&, const Value&, Args&) { return Value(); });
    }
    auto graphicsGetter = [](VM& vm, const Value& s, Args&) {
        if (!s.isObject()) return Value::null();
        if (Value* g = s.o->dynamic.find("__graphics")) return *g;
        Value g = vm.construct(Value(vm.findClass("flash.display::Graphics")), {});
        s.o->dynamic.set("__graphics", g);
        return g;
    };

    auto sprite = vm.defineNativeClass("flash.display", "Sprite", container);
    ClassBuilder sb{vm, sprite};
    sb.getter("graphics", graphicsGetter)
        .property("buttonMode",
            [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d && d->buttonMode); },
            [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->buttonMode = vm.toBoolean(arg(a, 0)); return Value(); })
        .property("useHandCursor", [](VM&, const Value&, Args&) { return Value(true); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("hitArea", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("soundTransform", [](VM& vm, const Value&, Args&) { return vm.construct(Value(vm.findClass("flash.media::SoundTransform")), {}); },
                  [](VM&, const Value&, Args&) { return Value(); })
        .getter("dropTarget", [](VM&, const Value&, Args&) { return Value::null(); })
        .method("startDrag", [](VM&, const Value&, Args&) { return Value(); })
        .method("stopDrag", [](VM&, const Value&, Args&) { return Value(); });

    auto movieClip = vm.defineNativeClass("flash.display", "MovieClip", sprite);
    ClassBuilder mcb{vm, movieClip};
    auto gotoFrame = [](VM& vm, const Value& s, const Value& frame, bool play) {
        Clip* c = clipOfValue(vm, s);
        if (!c) return;
        int f = -1;
        if (frame.isString()) {
            f = c->frameForLabel(frame.s);
            if (f < 0) {
                const double n = vm.toNumber(frame);
                if (!std::isnan(n)) f = static_cast<int>(n) - 1;
                else vm.throwError("ArgumentError", "Frame label " + frame.s + " not found in scene.");
            }
        } else {
            f = vm.toInt32(frame) - 1;
        }
        c->gotoFrame(std::max(0, f), play);
    };
    mcb.getter("currentFrame", [](VM& vm, const Value& s, Args&) { Clip* c = clipOfValue(vm, s); return Value(c ? c->currentFrame() + 1 : 1); })
        .getter("totalFrames", [](VM& vm, const Value& s, Args&) { Clip* c = clipOfValue(vm, s); return Value(c ? c->totalFrames() : 1); })
        .getter("framesLoaded", [](VM& vm, const Value& s, Args&) { Clip* c = clipOfValue(vm, s); return Value(c ? c->totalFrames() : 1); })
        .getter("currentLabel", [](VM& vm, const Value& s, Args&) {
            Clip* c = clipOfValue(vm, s);
            const auto l = c ? c->currentLabel() : std::string();
            return l.empty() ? Value::null() : Value(l);
        })
        .getter("currentFrameLabel", [](VM& vm, const Value& s, Args&) {
            Clip* c = clipOfValue(vm, s);
            if (!c) return Value::null();
            const auto& l = c->timeline().frames[static_cast<std::size_t>(c->currentFrame())].label;
            return l.empty() ? Value::null() : Value(l);
        })
        .getter("currentLabels", [](VM& vm, const Value& s, Args&) {
            Clip* c = clipOfValue(vm, s);
            auto arr = vm.newArray();
            if (c) {
                auto cls = vm.findClass("flash.display::FrameLabel");
                for (std::size_t i = 0; i < c->timeline().frames.size(); ++i) {
                    const auto& l = c->timeline().frames[i].label;
                    if (!l.empty()) arr->items.push_back(vm.construct(Value(cls), {Value(l), Value(static_cast<double>(i + 1))}));
                }
            }
            return Value(arr);
        })
        .getter("isPlaying", [](VM& vm, const Value& s, Args&) { Clip* c = clipOfValue(vm, s); return Value(c && c->playing()); })
        .property("enabled", [](VM&, const Value&, Args&) { return Value(true); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("trackAsMenu", [](VM&, const Value&, Args&) { return Value(false); }, [](VM&, const Value&, Args&) { return Value(); })
        .getter("currentScene", [](VM& vm, const Value&, Args&) { auto o = vm.newObject(); o->dynamic.set("name", Value("Scene 1")); return Value(o); })
        .method("play", [](VM& vm, const Value& s, Args&) { if (Clip* c = clipOfValue(vm, s)) c->play(); return Value(); })
        .method("stop", [](VM& vm, const Value& s, Args&) { if (Clip* c = clipOfValue(vm, s)) if (!vm.player.ignoreStops) c->stop(); return Value(); })
        .method("gotoAndPlay", [gotoFrame](VM& vm, const Value& s, Args& a) { gotoFrame(vm, s, arg(a, 0), true); return Value(); })
        .method("gotoAndStop", [gotoFrame](VM& vm, const Value& s, Args& a) { gotoFrame(vm, s, arg(a, 0), vm.player.ignoreStops); return Value(); })
        .method("nextFrame", [](VM& vm, const Value& s, Args&) { if (Clip* c = clipOfValue(vm, s)) c->gotoFrame(c->currentFrame() + 1, false); return Value(); })
        .method("prevFrame", [](VM& vm, const Value& s, Args&) { if (Clip* c = clipOfValue(vm, s)) c->gotoFrame(c->currentFrame() - 1, false); return Value(); })
        .method("nextScene", [](VM&, const Value&, Args&) { return Value(); })
        .method("prevScene", [](VM&, const Value&, Args&) { return Value(); })
        .method("addFrameScript", [](VM& vm, const Value& s, Args& a) {
            auto n = displayNativeOf(s);
            if (!n) return Value();
            for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                const int frame = vm.toInt32(a[i]);
                if (a[i + 1].isObject()) n->frameScripts[frame] = a[i + 1];
                else n->frameScripts.erase(frame);
            }
            return Value();
        });
    auto frameLabel = vm.defineNativeClass("flash.display", "FrameLabel", obj);
    ClassBuilder{vm, frameLabel}.ctor([](VM& vm, const Value& s, Args& a) {
        s.o->dynamic.set("name", Value(vm.toString(arg(a, 0))));
        s.o->dynamic.set("frame", arg(a, 1));
        return Value();
    });

    auto shape = vm.defineNativeClass("flash.display", "Shape", displayObject);
    ClassBuilder{vm, shape}.getter("graphics", graphicsGetter);
    vm.defineNativeClass("flash.display", "MorphShape", displayObject);
    vm.defineNativeClass("flash.display", "StaticText", displayObject);
    auto bitmap = vm.defineNativeClass("flash.display", "Bitmap", displayObject);
    ClassBuilder{vm, bitmap}.property("bitmapData", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("smoothing", [](VM&, const Value&, Args&) { return Value(false); }, [](VM&, const Value&, Args&) { return Value(); });
    auto bitmapData = vm.defineNativeClass("flash.display", "BitmapData", obj);
    ClassBuilder{vm, bitmapData}.ctor([](VM& vm, const Value& s, Args& a) {
        s.o->dynamic.set("width", Value(num(vm, a, 0)));
        s.o->dynamic.set("height", Value(num(vm, a, 1)));
        return Value();
    });
    for (const char* m : {"draw", "dispose", "fillRect", "copyPixels", "setPixel", "setPixel32", "lock", "unlock", "applyFilter", "colorTransform"}) {
        ClassBuilder{vm, bitmapData}.method(m, [](VM&, const Value&, Args&) { return Value(); });
    }
    ClassBuilder{vm, bitmapData}.method("getPixel", [](VM&, const Value&, Args&) { return Value(0); }).method("getPixel32", [](VM&, const Value&, Args&) { return Value(0); });
    bitmapData->sealed = false;

    auto simpleButton = vm.defineNativeClass("flash.display", "SimpleButton", interactive);
    ClassBuilder{vm, simpleButton}
        .property("enabled", [](VM&, const Value&, Args&) { return Value(true); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("useHandCursor", [](VM&, const Value&, Args&) { return Value(true); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("trackAsMenu", [](VM&, const Value&, Args&) { return Value(false); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("upState", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("overState", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("downState", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("hitTestState", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); })
        .property("soundTransform", [](VM&, const Value&, Args&) { return Value::null(); }, [](VM&, const Value&, Args&) { return Value(); });

    auto stage = vm.defineNativeClass("flash.display", "Stage", container);
    ClassBuilder stb{vm, stage};
    stb.getter("stageWidth", [](VM& vm, const Value&, Args&) { return Value(vm.player.movie.stageWidth()); })
        .getter("stageHeight", [](VM& vm, const Value&, Args&) { return Value(vm.player.movie.stageHeight()); })
        .property("frameRate", [](VM& vm, const Value&, Args&) { return Value(static_cast<double>(vm.player.movie.fps)); }, [](VM&, const Value&, Args&) { return Value(); })
        .getter("numChildren", [](VM&, const Value&, Args&) { return Value(1); })
        .method("getChildAt", [](VM& vm, const Value&, Args&) { return displayValue(vm, vm.player.rootHolder.get()); })
        .method("addChild", [](VM& vm, const Value&, Args& a) {
            // Children added to the stage are drawn above the root.
            if (auto* d = disp(arg(a, 0)); d && vm.player.root) {
                if (d->parent) d->parent->detach(d);
                vm.player.root->addChildAt(d->shared_from_this(), -1);
            }
            return arg(a, 0);
        })
        .method("removeChild", [](VM& vm, const Value&, Args& a) {
            if (auto* d = disp(arg(a, 0)); d && d->parent) d->parent->detach(d);
            (void)vm;
            return arg(a, 0);
        })
        .method("invalidate", [](VM&, const Value&, Args&) { return Value(); })
        .getter("stage", [](VM& vm, const Value&, Args&) { return Value(vm.stageObject); });
    for (auto [n, v] : std::initializer_list<std::pair<const char*, Value>>{
             {"scaleMode", Value("showAll")}, {"align", Value("")}, {"quality", Value("HIGH")}, {"focus", Value::null()},
             {"displayState", Value("normal")}, {"showDefaultContextMenu", Value(true)}, {"stageFocusRect", Value(true)},
             {"fullScreenSourceRect", Value::null()}, {"mouseChildren", Value(true)}, {"tabChildren", Value(true)}}) {
        const std::string key = std::string("__") + n;
        const Value def = v;
        stb.property(n,
            [key, def](VM&, const Value& s, Args&) { Value* v = s.isObject() ? s.o->dynamic.find(key) : nullptr; return v ? *v : def; },
            [key](VM&, const Value& s, Args& a) { if (s.isObject()) s.o->dynamic.set(key, arg(a, 0)); return Value(); });
    }
    stb.getter("fullScreenWidth", [](VM& vm, const Value&, Args&) { return Value(vm.player.movie.stageWidth()); })
        .getter("fullScreenHeight", [](VM& vm, const Value&, Args&) { return Value(vm.player.movie.stageHeight()); });
    // The stage has no display object of its own.
    stage->nativeInit = [](VM&, Object& o) { o.native = std::make_shared<DisplayNative>(); };

    for (auto [cls, consts] : std::initializer_list<std::pair<const char*, std::initializer_list<std::pair<const char*, const char*>>>>{
             {"StageScaleMode", {{"EXACT_FIT", "exactFit"}, {"NO_BORDER", "noBorder"}, {"NO_SCALE", "noScale"}, {"SHOW_ALL", "showAll"}}},
             {"StageAlign", {{"TOP", "T"}, {"BOTTOM", "B"}, {"LEFT", "L"}, {"RIGHT", "R"}, {"TOP_LEFT", "TL"}, {"TOP_RIGHT", "TR"},
                             {"BOTTOM_LEFT", "BL"}, {"BOTTOM_RIGHT", "BR"}}},
             {"StageQuality", {{"LOW", "low"}, {"MEDIUM", "medium"}, {"HIGH", "high"}, {"BEST", "best"}}},
             {"StageDisplayState", {{"NORMAL", "normal"}, {"FULL_SCREEN", "fullScreen"}, {"FULL_SCREEN_INTERACTIVE", "fullScreenInteractive"}}},
             {"BlendMode", {{"NORMAL", "normal"}, {"ADD", "add"}, {"MULTIPLY", "multiply"}, {"SCREEN", "screen"}, {"LAYER", "layer"},
                            {"ALPHA", "alpha"}, {"ERASE", "erase"}, {"OVERLAY", "overlay"}, {"HARDLIGHT", "hardlight"}, {"DIFFERENCE", "difference"}}},
             {"GradientType", {{"LINEAR", "linear"}, {"RADIAL", "radial"}}},
             {"SpreadMethod", {{"PAD", "pad"}, {"REFLECT", "reflect"}, {"REPEAT", "repeat"}}},
             {"LineScaleMode", {{"NORMAL", "normal"}, {"NONE", "none"}, {"HORIZONTAL", "horizontal"}, {"VERTICAL", "vertical"}}},
             {"CapsStyle", {{"NONE", "none"}, {"ROUND", "round"}, {"SQUARE", "square"}}},
             {"JointStyle", {{"BEVEL", "bevel"}, {"MITER", "miter"}, {"ROUND", "round"}}},
             {"PixelSnapping", {{"NEVER", "never"}, {"ALWAYS", "always"}, {"AUTO", "auto"}}}}) {
        auto c = vm.defineNativeClass("flash.display", cls, obj);
        for (auto [n, v] : consts) ClassBuilder{vm, c}.constant(n, Value(v));
    }

    // Loader / LoaderInfo: external content cannot be loaded; requests fail asynchronously.
    auto loaderInfo = vm.defineNativeClass("flash.display", "LoaderInfo", dispatcher);
    ClassBuilder lib{vm, loaderInfo};
    lib.getter("url", [](VM&, const Value&, Args&) { return Value("file:///game.swf"); })
        .getter("loaderURL", [](VM&, const Value&, Args&) { return Value("file:///game.swf"); })
        .getter("bytesLoaded", [](VM&, const Value&, Args&) { return Value(1000000); })
        .getter("bytesTotal", [](VM&, const Value&, Args&) { return Value(1000000); })
        .getter("frameRate", [](VM& vm, const Value&, Args&) { return Value(static_cast<double>(vm.player.movie.fps)); })
        .getter("width", [](VM& vm, const Value&, Args&) { return Value(vm.player.movie.stageWidth()); })
        .getter("height", [](VM& vm, const Value&, Args&) { return Value(vm.player.movie.stageHeight()); })
        .getter("swfVersion", [](VM& vm, const Value&, Args&) { return Value(static_cast<int>(vm.player.movie.swfVersion)); })
        .getter("actionScriptVersion", [](VM&, const Value&, Args&) { return Value(3); })
        .getter("contentType", [](VM&, const Value&, Args&) { return Value("application/x-shockwave-flash"); })
        .getter("parameters", [](VM& vm, const Value& s, Args&) {
            if (Value* p = s.o->dynamic.find("__params")) return *p;
            Value p(vm.newObject());
            s.o->dynamic.set("__params", p);
            return p;
        })
        .getter("content", [](VM& vm, const Value&, Args&) { return displayValue(vm, vm.player.rootHolder.get()); })
        .getter("loader", [](VM&, const Value&, Args&) { return Value::null(); })
        .getter("applicationDomain", [](VM& vm, const Value&, Args&) { return vm.getPublic(Value(vm.findClass("flash.system::ApplicationDomain")), "currentDomain"); })
        .getter("sharedEvents", [](VM& vm, const Value& s, Args&) {
            if (Value* p = s.o->dynamic.find("__shared")) return *p;
            Value p = vm.construct(Value(vm.findClass("flash.events::EventDispatcher")), {});
            s.o->dynamic.set("__shared", p);
            return p;
        })
        .getter("sameDomain", [](VM&, const Value&, Args&) { return Value(true); })
        .getter("childAllowsParent", [](VM&, const Value&, Args&) { return Value(true); })
        .getter("parentAllowsChild", [](VM&, const Value&, Args&) { return Value(true); });

    // Queue an ioError on an EventDispatcher after the current frame.
    auto failLater = [](VM& vm, const ObjectPtr& target) {
        VM::Timer t;
        t.once = true;
        t.next = vm.nowMs() + 50;
        t.id = vm.nextTimerId++;
        t.fn = Value(vm.newFunction([target](VM& vm, const Value&, Args&) {
            auto ev = vm.makeEvent("flash.events::IOErrorEvent", "ioError", false);
            if (auto e = std::dynamic_pointer_cast<EventData>(ev->native)) e->extra["text"] = Value("Error #2032: Stream Error (offline)");
            vm.dispatchEvent(target, ev);
            return Value();
        }));
        vm.timers[t.id] = t;
    };
    auto loader = vm.defineNativeClass("flash.display", "Loader", container);
    ClassBuilder{vm, loader}
        .getter("contentLoaderInfo", [](VM& vm, const Value& s, Args&) {
            if (Value* p = s.o->dynamic.find("__info")) return *p;
            Value p = vm.construct(Value(vm.findClass("flash.display::LoaderInfo")), {});
            s.o->dynamic.set("__info", p);
            return p;
        })
        .getter("content", [](VM&, const Value&, Args&) { return Value::null(); })
        .method("load", [failLater](VM& vm, const Value& s, Args&) {
            vm.warnOnce("Loader.load: external content is not available offline");
            failLater(vm, vm.getPublic(s, "contentLoaderInfo").o);
            return Value();
        })
        .method("loadBytes", [failLater](VM& vm, const Value& s, Args&) {
            vm.warnOnce("Loader.loadBytes: nested SWF loading is not supported");
            failLater(vm, vm.getPublic(s, "contentLoaderInfo").o);
            return Value();
        })
        .method("unload", [](VM&, const Value&, Args&) { return Value(); })
        .method("unloadAndStop", [](VM&, const Value&, Args&) { return Value(); })
        .method("close", [](VM&, const Value&, Args&) { return Value(); });

    // ---- flash.text
    auto textField = vm.defineNativeClass("flash.text", "TextField", interactive);
    ClassBuilder tfb{vm, textField};
    tfb.property("text",
            [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d ? d->text : std::string()); },
            [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->text = vm.toString(arg(a, 0)); return Value(); })
        .property("htmlText",
            [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d ? d->text : std::string()); },
            [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->text = stripHtml(vm.toString(arg(a, 0))); return Value(); })
        .getter("length", [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d ? static_cast<double>(d->text.size()) : 0.0); })
        .method("appendText", [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) d->text += vm.toString(arg(a, 0)); return Value(); })
        .method("replaceText", [](VM& vm, const Value& s, Args& a) {
            if (auto* d = disp(s)) {
                const auto b = static_cast<std::size_t>(std::max(0, vm.toInt32(arg(a, 0)))), e = static_cast<std::size_t>(std::max(0, vm.toInt32(arg(a, 1))));
                if (b <= d->text.size()) d->text.replace(b, std::min(e, d->text.size()) - b, vm.toString(arg(a, 2)));
            }
            return Value();
        })
        .property("textColor",
            [](VM& vm, const Value& s, Args&) {
                auto* d = disp(s);
                const auto* def = d ? textDef(vm, *d) : nullptr;
                return Value(def ? static_cast<double>((def->color.r << 16) | (def->color.g << 8) | def->color.b) : 0.0);
            },
            [](VM& vm, const Value& s, Args& a) {
                if (auto* d = disp(s)) {
                    auto& def = writableTextDef(vm, *d);
                    const std::uint32_t c = vm.toUint32(arg(a, 0));
                    def.color = {static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c), 255};
                }
                return Value();
            })
        .property("wordWrap",
            [](VM& vm, const Value& s, Args&) { auto* d = disp(s); const auto* def = d ? textDef(vm, *d) : nullptr; return Value(def && (def->flags & 1)); },
            [](VM& vm, const Value& s, Args& a) { if (auto* d = disp(s)) { auto& def = writableTextDef(vm, *d); def.flags = static_cast<std::uint8_t>(vm.toBoolean(arg(a, 0)) ? def.flags | 1 : def.flags & ~1); } return Value(); })
        .getter("textWidth", [](VM& vm, const Value& s, Args&) { auto* d = disp(s); const auto* def = d ? textDef(vm, *d) : nullptr; return Value(def && d ? d->text.size() * def->height / 20.0 * 0.55 : 0.0); })
        .getter("textHeight", [](VM& vm, const Value& s, Args&) { auto* d = disp(s); const auto* def = d ? textDef(vm, *d) : nullptr; return Value(def ? def->height / 20.0 * 1.2 : 0.0); })
        .getter("numLines", [](VM&, const Value& s, Args&) { auto* d = disp(s); return Value(d ? 1.0 + static_cast<double>(std::count(d->text.begin(), d->text.end(), '\n')) : 1.0); })
        .method("setTextFormat", [](VM& vm, const Value& s, Args& a) {
            auto* d = disp(s);
            const Value fmt = arg(a, 0);
            if (!d || !fmt.isObject()) return Value();
            auto& def = writableTextDef(vm, *d);
            const Value color = vm.getPublic(fmt, "color");
            if (!color.isNullish()) {
                const std::uint32_t c = vm.toUint32(color);
                def.color = {static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c), def.color.a};
            }
            const Value size = vm.getPublic(fmt, "size");
            if (!size.isNullish()) def.height = static_cast<std::uint16_t>(vm.toNumber(size) * 20);
            const Value align = vm.getPublic(fmt, "align");
            if (align.isString()) def.align = align.s == "right" ? 1 : align.s == "center" ? 2 : align.s == "justify" ? 3 : 0;
            return Value();
        })
        .method("getTextFormat", [](VM& vm, const Value&, Args&) { return vm.construct(Value(vm.findClass("flash.text::TextFormat")), {}); })
        .property("defaultTextFormat", [](VM& vm, const Value&, Args&) { return vm.construct(Value(vm.findClass("flash.text::TextFormat")), {}); },
                  [](VM& vm, const Value& s, Args& a) { return vm.callPublic(s, "setTextFormat", {arg(a, 0)}); });
    for (const char* stored : {"selectable", "autoSize", "embedFonts", "multiline", "type", "border", "borderColor", "background",
                               "backgroundColor", "maxChars", "restrict", "antiAliasType", "gridFitType", "sharpness", "thickness",
                               "condenseWhite", "displayAsPassword", "mouseWheelEnabled", "styleSheet", "scrollV", "scrollH"}) {
        const std::string key = std::string("__") + stored;
        tfb.property(stored,
            [key](VM&, const Value& s, Args&) { Value* v = s.isObject() ? s.o->dynamic.find(key) : nullptr; return v ? *v : Value(); },
            [key](VM&, const Value& s, Args& a) { if (s.isObject()) s.o->dynamic.set(key, arg(a, 0)); return Value(); });
    }
    auto textFormat = vm.defineNativeClass("flash.text", "TextFormat", obj);
    ClassBuilder{vm, textFormat}.ctor([](VM&, const Value& s, Args& a) {
        const char* fields[] = {"font", "size", "color", "bold", "italic", "underline", "url", "target", "align", "leftMargin", "rightMargin", "indent", "leading"};
        for (std::size_t i = 0; i < 13; ++i) s.o->dynamic.set(fields[i], i < a.size() ? a[i] : Value::null());
        return Value();
    });
    for (auto [cls, consts] : std::initializer_list<std::pair<const char*, std::initializer_list<std::pair<const char*, const char*>>>>{
             {"TextFieldAutoSize", {{"NONE", "none"}, {"LEFT", "left"}, {"RIGHT", "right"}, {"CENTER", "center"}}},
             {"TextFormatAlign", {{"LEFT", "left"}, {"RIGHT", "right"}, {"CENTER", "center"}, {"JUSTIFY", "justify"}}},
             {"TextFieldType", {{"DYNAMIC", "dynamic"}, {"INPUT", "input"}}},
             {"AntiAliasType", {{"NORMAL", "normal"}, {"ADVANCED", "advanced"}}},
             {"GridFitType", {{"NONE", "none"}, {"PIXEL", "pixel"}, {"SUBPIXEL", "subpixel"}}}}) {
        auto c = vm.defineNativeClass("flash.text", cls, obj);
        for (auto [n, v] : consts) ClassBuilder{vm, c}.constant(n, Value(v));
    }
    auto font = vm.defineNativeClass("flash.text", "Font", obj);
    ClassBuilder{vm, font}.staticMethod("registerFont", [](VM&, const Value&, Args&) { return Value(); })
        .staticMethod("enumerateFonts", [](VM& vm, const Value&, Args&) { return Value(vm.newArray()); });

    // ---- flash.utils
    vm.defineGlobalFunction("flash.utils", "getTimer", [](VM& vm, const Value&, Args&) { return Value(std::floor(vm.nowMs())); });
    auto addTimeout = [](bool once) {
        return [once](VM& vm, const Value&, Args& a) {
            VM::Timer t;
            t.fn = arg(a, 0);
            t.period = std::max(1.0, num(vm, a, 1));
            t.next = vm.nowMs() + t.period;
            t.once = once;
            t.args.assign(a.size() > 2 ? a.begin() + 2 : a.end(), a.end());
            t.id = vm.nextTimerId++;
            vm.timers[t.id] = t;
            return Value(t.id);
        };
    };
    vm.defineGlobalFunction("flash.utils", "setTimeout", addTimeout(true));
    vm.defineGlobalFunction("flash.utils", "setInterval", addTimeout(false));
    auto clearTimer = [](VM& vm, const Value&, Args& a) { vm.timers.erase(vm.toInt32(arg(a, 0))); return Value(); };
    vm.defineGlobalFunction("flash.utils", "clearTimeout", clearTimer);
    vm.defineGlobalFunction("flash.utils", "clearInterval", clearTimer);
    vm.defineGlobalFunction("flash.utils", "getQualifiedClassName", [](VM& vm, const Value&, Args& a) {
        const Value v = arg(a, 0);
        ClassPtr c = v.isObject() ? std::dynamic_pointer_cast<Class>(v.o) : nullptr;
        if (!c) c = vm.classOf(v);
        return c ? Value(c->qualifiedName()) : Value("null");
    });
    vm.defineGlobalFunction("flash.utils", "getQualifiedSuperclassName", [](VM& vm, const Value&, Args& a) {
        const Value v = arg(a, 0);
        ClassPtr c = v.isObject() ? std::dynamic_pointer_cast<Class>(v.o) : nullptr;
        if (!c) c = vm.classOf(v);
        return c && c->super ? Value(c->super->qualifiedName()) : Value::null();
    });
    vm.defineGlobalFunction("flash.utils", "getDefinitionByName", [](VM& vm, const Value&, Args& a) {
        auto c = vm.findClass(vm.toString(arg(a, 0)));
        if (!c) vm.throwError("ReferenceError", "Variable " + vm.toString(arg(a, 0)) + " is not defined.");
        return Value(c);
    });
    vm.defineGlobalFunction("flash.utils", "describeType", [](VM& vm, const Value&, Args&) { return Value(vm.newObject()); });
    vm.defineGlobalFunction("flash.utils", "escapeMultiByte", [](VM& vm, const Value&, Args& a) { return Value(vm.toString(arg(a, 0))); });
    vm.defineNativeClass("flash.utils", "Proxy", obj)->sealed = false;
    vm.defineGlobal("flash.utils", "flash_proxy", Value("http://www.adobe.com/2006/actionscript/flash/proxy"));
    auto endian = vm.defineNativeClass("flash.utils", "Endian", obj);
    ClassBuilder{vm, endian}.constant("BIG_ENDIAN", Value("bigEndian")).constant("LITTLE_ENDIAN", Value("littleEndian"));

    auto timerCls = vm.defineNativeClass("flash.utils", "Timer", dispatcher);
    ClassBuilder tb{vm, timerCls};
    tb.init([](VM&, Object& o) { o.native = std::make_shared<TimerData>(); });
    auto timerOf = [](const Value& v) { return v.isObject() ? std::dynamic_pointer_cast<TimerData>(v.o->native) : nullptr; };
    tb.ctor([timerOf](VM& vm, const Value& s, Args& a) {
        if (auto t = timerOf(s)) { t->delay = num(vm, a, 0, 1000); t->repeatCount = static_cast<int>(num(vm, a, 1)); }
        return Value();
    });
    tb.method("start", [timerOf](VM& vm, const Value& s, Args&) {
        auto t = timerOf(s);
        if (!t || t->timerId) return Value();
        VM::Timer vt;
        vt.timer = s.o;
        vt.period = std::max(1.0, t->delay);
        vt.next = vm.nowMs() + vt.period;
        vt.id = vm.nextTimerId++;
        std::weak_ptr<Object> weak = s.o;
        vt.fn = Value(vm.newFunction([weak, timerOf](VM& vm, const Value&, Args&) {
            auto self = weak.lock();
            if (!self) return Value();
            auto td = timerOf(Value(self));
            if (!td) return Value();
            ++td->currentCount;
            vm.dispatchEvent(self, vm.makeEvent("flash.events::TimerEvent", "timer", false));
            if (td->repeatCount > 0 && td->currentCount >= td->repeatCount) {
                vm.timers.erase(td->timerId);
                td->timerId = 0;
                vm.dispatchEvent(self, vm.makeEvent("flash.events::TimerEvent", "timerComplete", false));
            }
            return Value();
        }));
        t->timerId = vt.id;
        vm.timers[vt.id] = vt;
        return Value();
    });
    tb.method("stop", [timerOf](VM& vm, const Value& s, Args&) {
        if (auto t = timerOf(s)) { vm.timers.erase(t->timerId); t->timerId = 0; }
        return Value();
    });
    tb.method("reset", [timerOf](VM& vm, const Value& s, Args&) {
        if (auto t = timerOf(s)) { vm.timers.erase(t->timerId); t->timerId = 0; t->currentCount = 0; }
        return Value();
    });
    tb.getter("running", [timerOf](VM&, const Value& s, Args&) { auto t = timerOf(s); return Value(t && t->timerId != 0); });
    tb.getter("currentCount", [timerOf](VM&, const Value& s, Args&) { auto t = timerOf(s); return Value(t ? t->currentCount : 0); });
    tb.property("delay",
        [timerOf](VM&, const Value& s, Args&) { auto t = timerOf(s); return Value(t ? t->delay : 0.0); },
        [timerOf](VM& vm, const Value& s, Args& a) {
            if (auto t = timerOf(s)) {
                t->delay = num(vm, a, 0, 1000);
                auto it = vm.timers.find(t->timerId);
                if (it != vm.timers.end()) it->second.period = std::max(1.0, t->delay);
            }
            return Value();
        });
    tb.property("repeatCount",
        [timerOf](VM&, const Value& s, Args&) { auto t = timerOf(s); return Value(t ? t->repeatCount : 0); },
        [timerOf](VM& vm, const Value& s, Args& a) { if (auto t = timerOf(s)) t->repeatCount = vm.toInt32(arg(a, 0)); return Value(); });

    // ---- flash.media: sounds of the movie pack play through the audio mixer
    auto soundTransform = vm.defineNativeClass("flash.media", "SoundTransform", obj);
    ClassBuilder{vm, soundTransform}.ctor([](VM& vm, const Value& s, Args& a) {
        s.o->dynamic.set("volume", Value(num(vm, a, 0, 1)));
        s.o->dynamic.set("pan", Value(num(vm, a, 1)));
        return Value();
    });
    soundTransform->sealed = false;
    // volume/pan of a SoundTransform value (defaults 1 / 0)
    auto mixOf = [](VM& vm, const Value& t, float& volume, float& pan) {
        volume = 1;
        pan = 0;
        if (!t.isObject()) return;
        const Value v = vm.getPublic(t, "volume"), p = vm.getPublic(t, "pan");
        if (!v.isUndefined()) volume = static_cast<float>(std::max(0.0, vm.toNumber(v)));
        if (!p.isUndefined()) pan = static_cast<float>(std::clamp(vm.toNumber(p), -1.0, 1.0));
    };
    auto newTransform = [](VM& vm, float volume, float pan) {
        return vm.construct(Value(vm.findClass("flash.media::SoundTransform")), {Value(static_cast<double>(volume)), Value(static_cast<double>(pan))});
    };

    auto soundChannel = vm.defineNativeClass("flash.media", "SoundChannel", dispatcher);
    ClassBuilder{vm, soundChannel}
        .init([](VM&, Object& o) { o.native = std::make_shared<ChannelData>(); })
        .method("stop", [](VM& vm, const Value& self, Args&) {
            if (auto c = channelOf(self)) {
                if (c->handle) vm.player.audio.stop(c->handle);
                c->handle = 0;
            }
            return Value();
        })
        .getter("position", [](VM& vm, const Value& self, Args&) {
            auto c = channelOf(self);
            return Value(c && c->handle ? vm.player.audio.positionMs(c->handle) : 0.0);
        })
        .getter("leftPeak", [](VM&, const Value&, Args&) { return Value(0); })
        .getter("rightPeak", [](VM&, const Value&, Args&) { return Value(0); })
        .property("soundTransform",
                  [newTransform](VM& vm, const Value& self, Args&) {
                      auto c = channelOf(self);
                      return newTransform(vm, c ? c->volume : 1.0f, c ? c->pan : 0.0f);
                  },
                  [mixOf](VM& vm, const Value& self, Args& a) {
                      if (auto c = channelOf(self)) {
                          mixOf(vm, arg(a, 0), c->volume, c->pan);
                          if (c->handle) vm.player.audio.setVoice(c->handle, c->volume, c->pan);
                      }
                      return Value();
                  });

    auto sound = vm.defineNativeClass("flash.media", "Sound", dispatcher);
    ClassBuilder{vm, sound}
        .init([](VM&, Object& o) { o.native = std::make_shared<SoundData>(); })
        .ctor([](VM& vm, const Value& self, Args&) {
            // An embedded sound is a Sound subclass exported under the DefineSound's class name.
            auto d = soundOf(self);
            if (!d) return Value();
            auto& ids = symbolIds(vm.player);
            for (Class* c = self.o->cls.get(); c; c = c->super.get()) {
                const auto it = ids.find(c->qualifiedName());
                if (it != ids.end() && vm.player.movie.sounds.count(it->second)) { d->soundId = it->second; break; }
            }
            return Value();
        })
        .method("play", [mixOf](VM& vm, const Value& self, Args& a) {
            auto channel = vm.construct(Value(vm.findClass("flash.media::SoundChannel")), {});
            auto d = soundOf(self);
            auto c = channelOf(channel);
            if (!d || !c || !d->soundId) return channel;
            const auto it = vm.player.movie.sounds.find(d->soundId);
            if (it == vm.player.movie.sounds.end()) return channel;
            mixOf(vm, arg(a, 2), c->volume, c->pan);
            const double start = a.empty() ? 0.0 : vm.toNumber(a[0]);
            const int loops = a.size() > 1 ? static_cast<int>(vm.toNumber(a[1])) : 0;
            c->soundId = d->soundId;
            c->handle = vm.player.audio.play(it->second, d->soundId, std::isnan(start) ? 0.0 : start, std::max(0, loops) + 1,
                                             c->volume, c->pan);
            if (c->handle) channelRegistry()[c->handle] = channel.o;
            return channel;
        })
        .method("load", [](VM&, const Value&, Args&) { return Value(); })
        .method("close", [](VM&, const Value&, Args&) { return Value(); })
        .getter("length", [](VM& vm, const Value& self, Args&) {
            auto d = soundOf(self);
            const auto it = d ? vm.player.movie.sounds.find(d->soundId) : vm.player.movie.sounds.end();
            return Value(it == vm.player.movie.sounds.end() ? 0.0 : it->second.durationMs());
        })
        .getter("bytesLoaded", [](VM&, const Value&, Args&) { return Value(1000); })
        .getter("bytesTotal", [](VM&, const Value&, Args&) { return Value(1000); })
        .getter("isBuffering", [](VM&, const Value&, Args&) { return Value(false); })
        .getter("url", [](VM&, const Value&, Args&) { return Value::null(); })
        .getter("id3", [](VM& vm, const Value&, Args&) { return Value(vm.newObject()); });
    auto soundMixer = vm.defineNativeClass("flash.media", "SoundMixer", obj);
    ClassBuilder{vm, soundMixer}.staticMethod("stopAll", [](VM& vm, const Value&, Args&) { vm.player.audio.stopAll(); return Value(); })
        .staticMethod("computeSpectrum", [](VM&, const Value&, Args&) { return Value(); })
        .staticGetter("soundTransform", [newTransform](VM& vm, const Value&, Args&) { return newTransform(vm, vm.player.audio.masterVolume, 0.0f); });
    {
        Trait t;
        t.ns = Namespace::pub();
        t.name = "soundTransform";
        t.kind = Trait::Kind::Accessor;
        t.setter.native = [mixOf](VM& vm, const Value&, Args& a) {
            float volume, pan;
            mixOf(vm, arg(a, 0), volume, pan);
            vm.player.audio.masterVolume = volume;
            return Value();
        };
        soundMixer->staticTraits->add(t);
    }
    auto soundLoaderContext = vm.defineNativeClass("flash.media", "SoundLoaderContext", obj);
    soundLoaderContext->sealed = false;

    // ---- flash.net (offline)
    auto urlRequest = vm.defineNativeClass("flash.net", "URLRequest", obj);
    ClassBuilder{vm, urlRequest}.ctor([](VM& vm, const Value& s, Args& a) {
        s.o->dynamic.set("url", Value(a.empty() ? std::string() : vm.toString(a[0])));
        s.o->dynamic.set("method", Value("GET"));
        s.o->dynamic.set("data", Value::null());
        s.o->dynamic.set("requestHeaders", Value(vm.newArray()));
        s.o->dynamic.set("contentType", Value("application/x-www-form-urlencoded"));
        return Value();
    });
    urlRequest->sealed = false;
    auto urlVariables = vm.defineNativeClass("flash.net", "URLVariables", obj);
    ClassBuilder{vm, urlVariables}.method("decode", [](VM& vm, const Value& s, Args& a) {
        const std::string src = vm.toString(arg(a, 0));
        std::size_t start = 0;
        while (start < src.size()) {
            auto amp = src.find('&', start);
            const auto pair = src.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) s.o->dynamic.set(pair.substr(0, eq), Value(pair.substr(eq + 1)));
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
        return Value();
    }).method("toString", [](VM& vm, const Value& s, Args&) {
        std::string out;
        for (const auto& e : s.o->dynamic.entries) if (e.alive) out += (out.empty() ? "" : "&") + e.key + "=" + vm.toString(e.value);
        return Value(out);
    });
    urlVariables->sealed = false;
    auto urlRequestHeader = vm.defineNativeClass("flash.net", "URLRequestHeader", obj);
    ClassBuilder{vm, urlRequestHeader}.ctor([](VM& vm, const Value& s, Args& a) {
        s.o->dynamic.set("name", Value(vm.toString(arg(a, 0))));
        s.o->dynamic.set("value", Value(vm.toString(arg(a, 1))));
        return Value();
    });
    auto urlLoader = vm.defineNativeClass("flash.net", "URLLoader", dispatcher);
    ClassBuilder{vm, urlLoader}
        .method("load", [failLater](VM& vm, const Value& s, Args&) { vm.warnOnce("URLLoader.load: network is offline"); failLater(vm, s.o); return Value(); })
        .method("close", [](VM&, const Value&, Args&) { return Value(); })
        .property("data", [](VM&, const Value& s, Args&) { Value* v = s.o->dynamic.find("__data"); return v ? *v : Value(); },
                  [](VM&, const Value& s, Args& a) { s.o->dynamic.set("__data", arg(a, 0)); return Value(); })
        .property("dataFormat", [](VM&, const Value&, Args&) { return Value("text"); }, [](VM&, const Value&, Args&) { return Value(); })
        .getter("bytesLoaded", [](VM&, const Value&, Args&) { return Value(0); })
        .getter("bytesTotal", [](VM&, const Value&, Args&) { return Value(0); });
    for (auto [cls, consts] : std::initializer_list<std::pair<const char*, std::initializer_list<std::pair<const char*, const char*>>>>{
             {"URLRequestMethod", {{"GET", "GET"}, {"POST", "POST"}}},
             {"URLLoaderDataFormat", {{"TEXT", "text"}, {"BINARY", "binary"}, {"VARIABLES", "variables"}}}}) {
        auto c = vm.defineNativeClass("flash.net", cls, obj);
        for (auto [n, v] : consts) ClassBuilder{vm, c}.constant(n, Value(v));
    }
    vm.defineGlobalFunction("flash.net", "navigateToURL", [](VM& vm, const Value&, Args& a) {
        std::cout << "navigateToURL (ignored): " << vm.toString(vm.getPublic(arg(a, 0), "url")) << "\n";
        return Value();
    });
    vm.defineGlobalFunction("flash.net", "sendToURL", [](VM&, const Value&, Args&) { return Value(); });
    vm.defineGlobalFunction("flash.net", "registerClassAlias", [](VM&, const Value&, Args&) { return Value(); });
    auto localConnection = vm.defineNativeClass("flash.net", "LocalConnection", dispatcher);
    for (const char* m : {"connect", "send", "close", "allowDomain", "allowInsecureDomain"}) {
        ClassBuilder{vm, localConnection}.method(m, [](VM&, const Value&, Args&) { return Value(); });
    }
    ClassBuilder{vm, localConnection}.getter("domain", [](VM&, const Value&, Args&) { return Value("localhost"); })
        .property("client", [](VM&, const Value& s, Args&) { Value* v = s.o->dynamic.find("__client"); return v ? *v : Value::null(); },
                  [](VM&, const Value& s, Args& a) { s.o->dynamic.set("__client", arg(a, 0)); return Value(); });
    auto sharedObject = vm.defineNativeClass("flash.net", "SharedObject", dispatcher);
    ClassBuilder{vm, sharedObject}
        .staticMethod("getLocal", [](VM& vm, const Value&, Args& a) {
            static std::map<std::string, ObjectPtr> store;
            auto& so = store[vm.toString(arg(a, 0))];
            if (!so) {
                so = vm.construct(Value(vm.findClass("flash.net::SharedObject")), {}).o;
                so->dynamic.set("__data", Value(vm.newObject()));
            }
            return Value(so);
        })
        .getter("data", [](VM& vm, const Value& s, Args&) {
            Value* v = s.o->dynamic.find("__data");
            if (v) return *v;
            Value d(vm.newObject());
            s.o->dynamic.set("__data", d);
            return d;
        })
        .method("flush", [](VM&, const Value&, Args&) { return Value("flushed"); })
        .method("clear", [](VM& vm, const Value& s, Args&) { s.o->dynamic.set("__data", Value(vm.newObject())); return Value(); })
        .method("close", [](VM&, const Value&, Args&) { return Value(); })
        .method("setProperty", [](VM& vm, const Value& s, Args& a) { vm.setPublic(vm.getPublic(s, "data"), vm.toString(arg(a, 0)), arg(a, 1)); return Value(); })
        .getter("size", [](VM&, const Value&, Args&) { return Value(0); });

    // ---- flash.system / external / ui
    auto security = vm.defineNativeClass("flash.system", "Security", obj);
    for (const char* m : {"allowDomain", "allowInsecureDomain", "loadPolicyFile", "showSettings"}) {
        ClassBuilder{vm, security}.staticMethod(m, [](VM&, const Value&, Args&) { return Value(); });
    }
    ClassBuilder{vm, security}.staticGetter("sandboxType", [](VM&, const Value&, Args&) { return Value("localTrusted"); })
        .constant("LOCAL_TRUSTED", Value("localTrusted")).constant("REMOTE", Value("remote"))
        .constant("LOCAL_WITH_FILE", Value("localWithFile")).constant("LOCAL_WITH_NETWORK", Value("localWithNetwork"));
    auto capabilities = vm.defineNativeClass("flash.system", "Capabilities", obj);
    for (auto [n, v] : std::initializer_list<std::pair<const char*, Value>>{
             {"playerType", Value("StandAlone")}, {"version", Value("WIN 11,2,202,0")}, {"os", Value("Windows 10")},
             {"manufacturer", Value("Adobe Windows")}, {"language", Value("en")}, {"screenResolutionX", Value(1920)},
             {"screenResolutionY", Value(1080)}, {"isDebugger", Value(false)}, {"hasAudio", Value(true)},
             {"serverString", Value("")}, {"cpuArchitecture", Value("x86")}, {"screenDPI", Value(96)}}) {
        const Value val = v;
        ClassBuilder{vm, capabilities}.staticGetter(n, [val](VM&, const Value&, Args&) { return val; });
    }
    auto appDomain = vm.defineNativeClass("flash.system", "ApplicationDomain", obj);
    ClassBuilder{vm, appDomain}
        .staticGetter("currentDomain", [](VM& vm, const Value&, Args&) {
            static ObjectPtr domain;
            if (!domain) domain = vm.construct(Value(vm.findClass("flash.system::ApplicationDomain")), {}).o;
            return Value(domain);
        })
        .method("getDefinition", [](VM& vm, const Value&, Args& a) {
            auto c = vm.findClass(vm.toString(arg(a, 0)));
            if (!c) vm.throwError("ReferenceError", "Variable " + vm.toString(arg(a, 0)) + " is not defined.");
            return Value(c);
        })
        .method("hasDefinition", [](VM& vm, const Value&, Args& a) { return Value(vm.findClass(vm.toString(arg(a, 0))) != nullptr); });
    for (const char* n : {"LoaderContext", "SecurityDomain", "SecurityPanel", "IME"}) vm.defineNativeClass("flash.system", n, obj)->sealed = false;
    auto system = vm.defineNativeClass("flash.system", "System", obj);
    ClassBuilder{vm, system}.staticGetter("totalMemory", [](VM&, const Value&, Args&) { return Value(64.0 * 1024 * 1024); })
        .staticMethod("gc", [](VM&, const Value&, Args&) { return Value(); })
        .staticMethod("setClipboard", [](VM&, const Value&, Args&) { return Value(); })
        .staticMethod("pause", [](VM&, const Value&, Args&) { return Value(); })
        .staticMethod("resume", [](VM&, const Value&, Args&) { return Value(); });
    vm.defineGlobalFunction("flash.system", "fscommand", [](VM&, const Value&, Args&) { return Value(); });
    auto external = vm.defineNativeClass("flash.external", "ExternalInterface", obj);
    ClassBuilder{vm, external}.staticGetter("available", [](VM&, const Value&, Args&) { return Value(false); })
        .staticGetter("objectID", [](VM&, const Value&, Args&) { return Value::null(); })
        .staticMethod("call", [](VM&, const Value&, Args&) { return Value::null(); })
        .staticMethod("addCallback", [](VM&, const Value&, Args&) { return Value(); });
    auto mouse = vm.defineNativeClass("flash.ui", "Mouse", obj);
    ClassBuilder{vm, mouse}.staticMethod("hide", [](VM&, const Value&, Args&) { SDL_ShowCursor(SDL_DISABLE); return Value(); })
        .staticMethod("show", [](VM&, const Value&, Args&) { SDL_ShowCursor(SDL_ENABLE); return Value(); });
    auto keyboard = vm.defineNativeClass("flash.ui", "Keyboard", obj);
    for (auto [n, v] : std::initializer_list<std::pair<const char*, int>>{
             {"BACKSPACE", 8}, {"TAB", 9}, {"ENTER", 13}, {"SHIFT", 16}, {"CONTROL", 17}, {"CAPS_LOCK", 20}, {"ESCAPE", 27},
             {"SPACE", 32}, {"PAGE_UP", 33}, {"PAGE_DOWN", 34}, {"END", 35}, {"HOME", 36}, {"LEFT", 37}, {"UP", 38},
             {"RIGHT", 39}, {"DOWN", 40}, {"INSERT", 45}, {"DELETE", 46}}) {
        ClassBuilder{vm, keyboard}.constant(n, Value(v));
    }
    for (char c = 'A'; c <= 'Z'; ++c) ClassBuilder{vm, keyboard}.constant(std::string(1, c), Value(static_cast<int>(c)));
    auto contextMenu = vm.defineNativeClass("flash.ui", "ContextMenu", dispatcher);
    ClassBuilder{vm, contextMenu}.ctor([](VM& vm, const Value& s, Args&) {
        s.o->dynamic.set("customItems", Value(vm.newArray()));
        s.o->dynamic.set("builtInItems", Value(vm.newObject()));
        return Value();
    }).method("hideBuiltInItems", [](VM&, const Value&, Args&) { return Value(); });
    contextMenu->sealed = false;
    auto contextMenuItem = vm.defineNativeClass("flash.ui", "ContextMenuItem", dispatcher);
    ClassBuilder{vm, contextMenuItem}.ctor([](VM& vm, const Value& s, Args& a) { s.o->dynamic.set("caption", Value(vm.toString(arg(a, 0)))); return Value(); });
    contextMenuItem->sealed = false;
    auto mouseCursor = vm.defineNativeClass("flash.ui", "MouseCursor", obj);
    for (auto [n, v] : std::initializer_list<std::pair<const char*, const char*>>{{"AUTO", "auto"}, {"ARROW", "arrow"}, {"BUTTON", "button"}, {"HAND", "hand"}, {"IBEAM", "ibeam"}}) {
        ClassBuilder{vm, mouseCursor}.constant(n, Value(v));
    }

    // ---- flash.filters: constructed and stored, not rendered
    auto bitmapFilter = vm.defineNativeClass("flash.filters", "BitmapFilter", obj);
    bitmapFilter->sealed = false;
    for (const char* n : {"GlowFilter", "DropShadowFilter", "BlurFilter", "ColorMatrixFilter", "BevelFilter", "GradientGlowFilter",
                          "GradientBevelFilter", "ConvolutionFilter", "DisplacementMapFilter"}) {
        auto c = vm.defineNativeClass("flash.filters", n, bitmapFilter);
        c->sealed = false;
        ClassBuilder{vm, c}.method("clone", [](VM&, const Value& s, Args&) { return s; });
    }
    auto quality = vm.defineNativeClass("flash.filters", "BitmapFilterQuality", obj);
    ClassBuilder{vm, quality}.constant("LOW", Value(1)).constant("MEDIUM", Value(2)).constant("HIGH", Value(3));
}

} // namespace fp::avm2
