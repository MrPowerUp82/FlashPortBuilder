// E4X: the XML and XMLList classes (parsing, child/attribute access, iteration, serialisation).
#include "AVM2.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace fp::avm2 {
namespace {

struct XmlObject;

struct XNode : std::enable_shared_from_this<XNode> {
    enum class Kind { Element, Text, Attr, Comment, PI } kind = Kind::Element;
    std::string name;    // element / attribute name (qualified, as written)
    std::string value;   // text / attribute / comment content
    std::vector<std::shared_ptr<XNode>> attrs;
    std::vector<std::shared_ptr<XNode>> children;
    std::weak_ptr<XNode> parent;
    std::weak_ptr<Object> wrapper; // the AS3 object that represents this node (keeps identity)
};
using NodePtr = std::shared_ptr<XNode>;

struct XmlObject : Object {
    NodePtr node;
    bool nativeGet(VM& vm, const Multiname& mn, const Value* key, Value& out) override;
    bool nativeSet(VM& vm, const Multiname& mn, const Value* key, const Value& v) override;
    bool nativeHas(VM& vm, const Multiname& mn, const Value* key) override;
    bool nativePrimitive(VM& vm, Value& out) override;
    bool nativeItems(VM& vm, std::vector<Value>& out) override;
    bool nativeDescendants(VM& vm, const Multiname& mn, Value& out) override;
};

struct XmlListObject : Object {
    std::vector<NodePtr> items;
    bool nativeGet(VM& vm, const Multiname& mn, const Value* key, Value& out) override;
    bool nativeSet(VM& vm, const Multiname& mn, const Value* key, const Value& v) override;
    bool nativeHas(VM& vm, const Multiname& mn, const Value* key) override;
    bool nativePrimitive(VM& vm, Value& out) override;
    bool nativeItems(VM& vm, std::vector<Value>& out) override;
    bool nativeDescendants(VM& vm, const Multiname& mn, Value& out) override;
};

ClassPtr gXml, gXmlList;

Value arg(const Args& a, std::size_t i) { return i < a.size() ? a[i] : Value(); }

// ---------------------------------------------------------------- wrapping

Value wrap(VM& vm, const NodePtr& n) {
    if (!n) return Value::null();
    if (auto w = n->wrapper.lock()) return Value(w);
    auto o = std::dynamic_pointer_cast<XmlObject>(vm.createInstance(gXml));
    o->node = n;
    n->wrapper = o;
    return Value(std::static_pointer_cast<Object>(o));
}

Value makeList(VM& vm, std::vector<NodePtr> items) {
    auto o = std::dynamic_pointer_cast<XmlListObject>(vm.createInstance(gXmlList));
    o->items = std::move(items);
    return Value(std::static_pointer_cast<Object>(o));
}

NodePtr nodeOf(const Value& v) {
    if (!v.isObject()) return nullptr;
    auto x = std::dynamic_pointer_cast<XmlObject>(v.o);
    return x ? x->node : nullptr;
}

std::vector<NodePtr> nodesOf(const Value& v) {
    if (!v.isObject()) return {};
    if (auto x = std::dynamic_pointer_cast<XmlObject>(v.o)) return {x->node};
    if (auto l = std::dynamic_pointer_cast<XmlListObject>(v.o)) return l->items;
    return {};
}

// ---------------------------------------------------------------- parsing

std::string decodeEntities(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
        const std::string e = s.substr(i + 1, semi - i - 1);
        std::string rep;
        if (e == "lt") rep = "<"; else if (e == "gt") rep = ">"; else if (e == "amp") rep = "&";
        else if (e == "quot") rep = "\""; else if (e == "apos") rep = "'";
        else if (!e.empty() && e[0] == '#') {
            const long cp = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtol(e.c_str() + 2, nullptr, 16) : std::strtol(e.c_str() + 1, nullptr, 10);
            if (cp < 0x80) rep.push_back(static_cast<char>(cp));
            else if (cp < 0x800) { rep.push_back(static_cast<char>(0xc0 | (cp >> 6))); rep.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
            else if (cp < 0x10000) { rep.push_back(static_cast<char>(0xe0 | (cp >> 12))); rep.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); rep.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
            else { rep.push_back(static_cast<char>(0xf0 | (cp >> 18))); rep.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f))); rep.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); rep.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
        } else { out.push_back('&'); continue; }
        out += rep;
        i = semi;
    }
    return out;
}

struct Parser {
    const std::string& s;
    std::size_t p = 0;
    std::string error;
    explicit Parser(const std::string& src) : s(src) {}

    bool startsWith(const char* t) const { return s.compare(p, std::strlen(t), t) == 0; }
    void skipSpace() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    std::string readName() {
        const auto b = p;
        while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '=' && s[p] != '>' && s[p] != '/' && s[p] != '<') ++p;
        return s.substr(b, p - b);
    }

    // Parses nodes until the matching close tag of `parent` (or the end of input).
    bool parseContent(const NodePtr& parent, bool ignoreWhitespace) {
        while (p < s.size()) {
            if (startsWith("</")) return true;
            if (startsWith("<!--")) {
                const auto e = s.find("-->", p + 4);
                if (e == std::string::npos) { error = "unterminated comment"; return false; }
                auto c = std::make_shared<XNode>();
                c->kind = XNode::Kind::Comment;
                c->value = s.substr(p + 4, e - p - 4);
                c->parent = parent;
                parent->children.push_back(c);
                p = e + 3;
            } else if (startsWith("<![CDATA[")) {
                const auto e = s.find("]]>", p + 9);
                if (e == std::string::npos) { error = "unterminated CDATA"; return false; }
                auto t = std::make_shared<XNode>();
                t->kind = XNode::Kind::Text;
                t->value = s.substr(p + 9, e - p - 9);
                t->parent = parent;
                parent->children.push_back(t);
                p = e + 3;
            } else if (startsWith("<?")) {
                const auto e = s.find("?>", p + 2);
                if (e == std::string::npos) { error = "unterminated processing instruction"; return false; }
                p = e + 2;
            } else if (startsWith("<!")) {
                const auto e = s.find('>', p);
                if (e == std::string::npos) { error = "unterminated declaration"; return false; }
                p = e + 1;
            } else if (s[p] == '<') {
                if (!parseElement(parent, ignoreWhitespace)) return false;
            } else {
                const auto e = s.find('<', p);
                const std::string raw = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
                p = e == std::string::npos ? s.size() : e;
                const bool blank = std::all_of(raw.begin(), raw.end(), [](unsigned char c) { return std::isspace(c); });
                if (blank && ignoreWhitespace) continue;
                auto t = std::make_shared<XNode>();
                t->kind = XNode::Kind::Text;
                t->value = decodeEntities(raw);
                t->parent = parent;
                parent->children.push_back(t);
            }
        }
        return true;
    }

    bool parseElement(const NodePtr& parent, bool ignoreWhitespace) {
        ++p; // '<'
        auto el = std::make_shared<XNode>();
        el->name = readName();
        if (el->name.empty()) { error = "missing element name"; return false; }
        el->parent = parent;
        while (true) {
            skipSpace();
            if (p >= s.size()) { error = "unterminated tag"; return false; }
            if (s[p] == '/' || s[p] == '>') break;
            auto a = std::make_shared<XNode>();
            a->kind = XNode::Kind::Attr;
            a->name = readName();
            skipSpace();
            if (p < s.size() && s[p] == '=') {
                ++p;
                skipSpace();
                if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                    const char q = s[p++];
                    const auto e = s.find(q, p);
                    if (e == std::string::npos) { error = "unterminated attribute"; return false; }
                    a->value = decodeEntities(s.substr(p, e - p));
                    p = e + 1;
                }
            }
            if (a->name.empty()) { error = "bad attribute"; return false; }
            a->parent = el;
            el->attrs.push_back(a);
        }
        parent->children.push_back(el);
        if (s[p] == '/') {
            p += 2; // "/>"
            return true;
        }
        ++p; // '>'
        if (!parseContent(el, ignoreWhitespace)) return false;
        if (!startsWith("</")) { error = "missing </" + el->name + ">"; return false; }
        p += 2;
        const std::string close = readName();
        if (close != el->name) { error = "mismatched </" + close + "> for <" + el->name + ">"; return false; }
        skipSpace();
        if (p < s.size() && s[p] == '>') ++p;
        return true;
    }
};

// Parses a document or fragment; returns the nodes at top level.
bool parseXml(const std::string& src, bool ignoreWhitespace, std::vector<NodePtr>& out, std::string& error) {
    std::string text = src;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef && static_cast<unsigned char>(text[1]) == 0xbb &&
        static_cast<unsigned char>(text[2]) == 0xbf) text.erase(0, 3);
    Parser parser(text);
    auto holder = std::make_shared<XNode>();
    if (!parser.parseContent(holder, ignoreWhitespace) || parser.p < text.size()) {
        error = parser.error.empty() ? "unexpected content" : parser.error;
        return false;
    }
    out = holder->children;
    for (auto& n : out) n->parent.reset();
    return true;
}

// ---------------------------------------------------------------- serialisation

std::string escapeText(const std::string& s, bool attr) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += attr ? ">" : "&gt;"; break;
            case '"': out += attr ? "&quot;" : "\""; break;
            default: out.push_back(c);
        }
    }
    return out;
}

bool hasElementChildren(const NodePtr& n) {
    return std::any_of(n->children.begin(), n->children.end(), [](const NodePtr& c) { return c->kind == XNode::Kind::Element; });
}

std::string textContent(const NodePtr& n) {
    switch (n->kind) {
        case XNode::Kind::Text: case XNode::Kind::Attr: case XNode::Kind::Comment: case XNode::Kind::PI: return n->value;
        case XNode::Kind::Element: {
            std::string out;
            for (const auto& c : n->children) {
                if (c->kind == XNode::Kind::Text) out += c->value;
                else if (c->kind == XNode::Kind::Element) out += textContent(c);
            }
            return out;
        }
    }
    return {};
}

void serialise(const NodePtr& n, int indent, std::string& out) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    switch (n->kind) {
        case XNode::Kind::Text: out += pad + escapeText(n->value, false); return;
        case XNode::Kind::Attr: out += n->value; return;
        case XNode::Kind::Comment: out += pad + "<!--" + n->value + "-->"; return;
        case XNode::Kind::PI: out += pad + "<?" + n->value + "?>"; return;
        case XNode::Kind::Element: break;
    }
    out += pad + "<" + n->name;
    for (const auto& a : n->attrs) out += " " + a->name + "=\"" + escapeText(a->value, true) + "\"";
    if (n->children.empty()) { out += "/>"; return; }
    if (!hasElementChildren(n)) { // simple content stays on one line
        out += ">" + escapeText(textContent(n), false) + "</" + n->name + ">";
        return;
    }
    out += ">";
    for (const auto& c : n->children) {
        out += "\n";
        serialise(c, indent + 1, out);
    }
    out += "\n" + pad + "</" + n->name + ">";
}

std::string xmlString(const NodePtr& n) {
    std::string out;
    serialise(n, 0, out);
    return out;
}

// toString(): simple content gives its text, complex content its XML.
std::string nodeToString(const NodePtr& n) {
    if (n->kind != XNode::Kind::Element) return n->value;
    return hasElementChildren(n) ? xmlString(n) : textContent(n);
}

std::string listToString(const std::vector<NodePtr>& items) {
    if (items.size() == 1) return nodeToString(items[0]);
    const bool simple = std::none_of(items.begin(), items.end(), [](const NodePtr& n) { return n->kind == XNode::Kind::Element && hasElementChildren(n); });
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!simple && i) out += "\n";
        out += simple ? nodeToString(items[i]) : xmlString(items[i]);
    }
    return out;
}

// ---------------------------------------------------------------- queries

bool nameMatches(const NodePtr& n, const Multiname& mn, const std::string& name) {
    if (mn.anyName || name == "*") return true;
    if (n->name == name) return true;
    const auto colon = n->name.find(':'); // unqualified name matches the local part
    return colon != std::string::npos && n->name.compare(colon + 1, std::string::npos, name) == 0;
}

void childrenNamed(const NodePtr& n, const Multiname& mn, const std::string& name, std::vector<NodePtr>& out) {
    if (mn.anyName || name == "*") {
        out.insert(out.end(), n->children.begin(), n->children.end());
        return;
    }
    for (const auto& c : n->children) if (c->kind == XNode::Kind::Element && nameMatches(c, mn, name)) out.push_back(c);
}

void attributesNamed(const NodePtr& n, const Multiname& mn, const std::string& name, std::vector<NodePtr>& out) {
    for (const auto& a : n->attrs) if (nameMatches(a, mn, name)) out.push_back(a);
}

void descendantsNamed(const NodePtr& n, const Multiname& mn, const std::string& name, std::vector<NodePtr>& out) {
    for (const auto& c : n->children) {
        if (mn.attribute) attributesNamed(c, mn, name, out);
        else if (c->kind == XNode::Kind::Element && nameMatches(c, mn, name)) out.push_back(c);
        else if ((mn.anyName || name == "*") && c->kind != XNode::Kind::Element) out.push_back(c);
        if (c->kind == XNode::Kind::Element) descendantsNamed(c, mn, name, out);
    }
}

bool isIndex(const std::string& s, std::size_t& idx) {
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    idx = static_cast<std::size_t>(std::strtoull(s.c_str(), nullptr, 10));
    return true;
}

std::string keyName(VM& vm, const Multiname& mn, const Value* key) { return key ? vm.toString(*key) : mn.name; }

// Query shared by XML (one node) and XMLList (several): attribute, child or index access.
bool queryNodes(VM& vm, const std::vector<NodePtr>& nodes, bool isList, const Multiname& mn, const Value* key, Value& out) {
    const std::string name = keyName(vm, mn, key);
    std::size_t idx;
    if (isIndex(name, idx)) {
        if (isList) out = idx < nodes.size() ? wrap(vm, nodes[idx]) : Value();
        else out = idx == 0 && !nodes.empty() ? wrap(vm, nodes[0]) : Value();
        return true;
    }
    std::vector<NodePtr> found;
    for (const auto& n : nodes) {
        if (mn.attribute) attributesNamed(n, mn, name, found);
        else if (n->kind == XNode::Kind::Element) childrenNamed(n, mn, name, found);
    }
    out = makeList(vm, std::move(found));
    return true;
}

NodePtr newText(const std::string& v) {
    auto t = std::make_shared<XNode>();
    t->kind = XNode::Kind::Text;
    t->value = v;
    return t;
}

bool assignNode(VM& vm, const NodePtr& n, const Multiname& mn, const Value* key, const Value& v) {
    const std::string name = keyName(vm, mn, key);
    const std::string value = vm.toString(v);
    if (mn.attribute) {
        for (const auto& a : n->attrs) if (a->name == name) { a->value = value; return true; }
        auto a = std::make_shared<XNode>();
        a->kind = XNode::Kind::Attr;
        a->name = name;
        a->value = value;
        a->parent = n;
        n->attrs.push_back(a);
        return true;
    }
    for (const auto& c : n->children) {
        if (c->kind == XNode::Kind::Element && c->name == name) {
            c->children.clear();
            auto t = newText(value);
            t->parent = c;
            c->children.push_back(t);
            return true;
        }
    }
    auto el = std::make_shared<XNode>();
    el->name = name;
    el->parent = n;
    auto t = newText(value);
    t->parent = el;
    el->children.push_back(t);
    n->children.push_back(el);
    return true;
}

// ---------------------------------------------------------------- hooks

bool XmlObject::nativeGet(VM& vm, const Multiname& mn, const Value* key, Value& out) { return queryNodes(vm, {node}, false, mn, key, out); }
bool XmlObject::nativeSet(VM& vm, const Multiname& mn, const Value* key, const Value& v) { return node && assignNode(vm, node, mn, key, v); }
bool XmlObject::nativeHas(VM& vm, const Multiname& mn, const Value* key) {
    Value out;
    queryNodes(vm, {node}, false, mn, key, out);
    auto l = std::dynamic_pointer_cast<XmlListObject>(out.o);
    return l ? !l->items.empty() : out.isObject();
}
bool XmlObject::nativePrimitive(VM&, Value& out) { out = Value(nodeToString(node)); return true; }
bool XmlObject::nativeItems(VM&, std::vector<Value>& out) { out.push_back(Value(shared_from_this())); return true; }
bool XmlObject::nativeDescendants(VM& vm, const Multiname& mn, Value& out) {
    std::vector<NodePtr> found;
    descendantsNamed(node, mn, mn.name, found);
    out = makeList(vm, std::move(found));
    return true;
}

bool XmlListObject::nativeGet(VM& vm, const Multiname& mn, const Value* key, Value& out) { return queryNodes(vm, items, true, mn, key, out); }
bool XmlListObject::nativeSet(VM& vm, const Multiname& mn, const Value* key, const Value& v) {
    if (items.size() != 1) return false;
    return assignNode(vm, items[0], mn, key, v);
}
bool XmlListObject::nativeHas(VM& vm, const Multiname& mn, const Value* key) {
    Value out;
    queryNodes(vm, items, true, mn, key, out);
    if (auto l = std::dynamic_pointer_cast<XmlListObject>(out.o)) return !l->items.empty();
    return out.isObject();
}
bool XmlListObject::nativePrimitive(VM&, Value& out) { out = Value(listToString(items)); return true; }
bool XmlListObject::nativeItems(VM& vm, std::vector<Value>& out) {
    for (const auto& n : items) out.push_back(wrap(vm, n));
    if (std::getenv("FP_TRACE_XML")) std::fprintf(stderr, "[xml] list iteration: %zu items\n", items.size());
    return true;
}
bool XmlListObject::nativeDescendants(VM& vm, const Multiname& mn, Value& out) {
    std::vector<NodePtr> found;
    for (const auto& n : items) descendantsNamed(n, mn, mn.name, found);
    out = makeList(vm, std::move(found));
    return true;
}

// Converts a constructor / conversion argument into top-level nodes.
std::vector<NodePtr> toNodes(VM& vm, const Value& v) {
    if (v.isObject()) {
        auto nodes = nodesOf(v);
        if (!nodes.empty() || std::dynamic_pointer_cast<XmlListObject>(v.o)) return nodes;
    }
    if (v.isNullish()) return {};
    std::vector<NodePtr> out;
    std::string error;
    if (!parseXml(vm.toString(v), true, out, error)) vm.throwError("TypeError", "Error #1090: XML parser failure: " + error);
    return out;
}

} // namespace

void installXml(VM& vm) {
    auto xml = vm.defineNativeClass("", "XML", vm.objectClass);
    auto list = vm.defineNativeClass("", "XMLList", vm.objectClass);
    gXml = xml;
    gXmlList = list;
    xml->sealed = false;
    list->sealed = false;
    xml->allocator = [](VM&) -> ObjectPtr { return std::make_shared<XmlObject>(); };
    list->allocator = [](VM&) -> ObjectPtr { return std::make_shared<XmlListObject>(); };

    // new XML(source): a single element (text for plain strings).
    ClassBuilder{vm, xml}.ctor([](VM& vm, const Value& self, Args& a) {
        auto o = std::dynamic_pointer_cast<XmlObject>(self.o);
        auto nodes = toNodes(vm, arg(a, 0));
        // The first element is the document; a plain string without markup becomes a text node.
        auto it = std::find_if(nodes.begin(), nodes.end(), [](const NodePtr& n) { return n->kind == XNode::Kind::Element; });
        o->node = it != nodes.end() ? *it : (nodes.empty() ? newText("") : nodes[0]);
        o->node->wrapper = o;
        return Value();
    });
    xml->callAsFunction = [](VM& vm, Args& a) {
        auto nodes = toNodes(vm, arg(a, 0));
        auto it = std::find_if(nodes.begin(), nodes.end(), [](const NodePtr& n) { return n->kind == XNode::Kind::Element; });
        if (it != nodes.end()) return wrap(vm, *it);
        return nodes.empty() ? Value::null() : wrap(vm, nodes[0]);
    };
    ClassBuilder{vm, list}.ctor([](VM& vm, const Value& self, Args& a) {
        std::dynamic_pointer_cast<XmlListObject>(self.o)->items = toNodes(vm, arg(a, 0));
        return Value();
    });
    list->callAsFunction = [](VM& vm, Args& a) { return makeList(vm, toNodes(vm, arg(a, 0))); };

    // Methods shared by both classes work on "the nodes this object stands for".
    for (const auto& cls : {xml, list}) {
        const bool isList = cls == list;
        auto nodesFor = [](const Value& self) { return nodesOf(self); };
        ClassBuilder b{vm, cls};
        b.method("toString", [nodesFor](VM&, const Value& self, Args&) { return Value(listToString(nodesFor(self))); });
        b.method("toXMLString", [nodesFor](VM&, const Value& self, Args&) {
            std::string out;
            const auto nodes = nodesFor(self);
            for (std::size_t i = 0; i < nodes.size(); ++i) out += (i ? "\n" : "") + xmlString(nodes[i]);
            return Value(out);
        });
        b.method("valueOf", [](VM&, const Value& self, Args&) { return self; });
        b.method("length", [nodesFor, isList](VM&, const Value& self, Args&) { return Value(static_cast<double>(isList ? nodesFor(self).size() : 1)); });
        b.method("children", [nodesFor](VM& vm, const Value& self, Args&) {
            std::vector<NodePtr> out;
            for (const auto& n : nodesFor(self)) out.insert(out.end(), n->children.begin(), n->children.end());
            return makeList(vm, std::move(out));
        });
        b.method("elements", [nodesFor](VM& vm, const Value& self, Args& a) {
            std::vector<NodePtr> out;
            const std::string name = a.empty() ? "*" : vm.toString(a[0]);
            Multiname mn = Multiname::publicName(name);
            for (const auto& n : nodesFor(self)) {
                for (const auto& c : n->children) if (c->kind == XNode::Kind::Element && nameMatches(c, mn, name)) out.push_back(c);
            }
            return makeList(vm, std::move(out));
        });
        b.method("child", [nodesFor](VM& vm, const Value& self, Args& a) {
            std::vector<NodePtr> out;
            const std::string name = vm.toString(arg(a, 0));
            std::size_t idx;
            const auto nodes = nodesFor(self);
            if (isIndex(name, idx)) {
                std::vector<NodePtr> all;
                for (const auto& n : nodes) all.insert(all.end(), n->children.begin(), n->children.end());
                if (idx < all.size()) out.push_back(all[idx]);
                return makeList(vm, std::move(out));
            }
            Multiname mn = Multiname::publicName(name);
            for (const auto& n : nodes) childrenNamed(n, mn, name, out);
            return makeList(vm, std::move(out));
        });
        b.method("attribute", [nodesFor](VM& vm, const Value& self, Args& a) {
            std::vector<NodePtr> out;
            const std::string name = vm.toString(arg(a, 0));
            Multiname mn = Multiname::publicName(name);
            for (const auto& n : nodesFor(self)) attributesNamed(n, mn, name, out);
            return makeList(vm, std::move(out));
        });
        b.method("attributes", [nodesFor](VM& vm, const Value& self, Args&) {
            std::vector<NodePtr> out;
            for (const auto& n : nodesFor(self)) out.insert(out.end(), n->attrs.begin(), n->attrs.end());
            return makeList(vm, std::move(out));
        });
        b.method("text", [nodesFor](VM& vm, const Value& self, Args&) {
            std::vector<NodePtr> out;
            for (const auto& n : nodesFor(self)) for (const auto& c : n->children) if (c->kind == XNode::Kind::Text) out.push_back(c);
            return makeList(vm, std::move(out));
        });
        b.method("descendants", [nodesFor](VM& vm, const Value& self, Args& a) {
            std::vector<NodePtr> out;
            const std::string name = a.empty() ? "*" : vm.toString(a[0]);
            Multiname mn = Multiname::publicName(name);
            for (const auto& n : nodesFor(self)) descendantsNamed(n, mn, name, out);
            return makeList(vm, std::move(out));
        });
        b.method("name", [nodesFor](VM&, const Value& self, Args&) {
            const auto nodes = nodesFor(self);
            return nodes.empty() || nodes[0]->kind == XNode::Kind::Text ? Value::null() : Value(nodes[0]->name);
        });
        b.method("localName", [nodesFor](VM&, const Value& self, Args&) {
            const auto nodes = nodesFor(self);
            if (nodes.empty() || nodes[0]->kind == XNode::Kind::Text) return Value::null();
            const auto& n = nodes[0]->name;
            const auto colon = n.find(':');
            return Value(colon == std::string::npos ? n : n.substr(colon + 1));
        });
        b.method("nodeKind", [nodesFor](VM&, const Value& self, Args&) {
            const auto nodes = nodesFor(self);
            if (nodes.empty()) return Value("text");
            switch (nodes[0]->kind) {
                case XNode::Kind::Element: return Value("element");
                case XNode::Kind::Attr: return Value("attribute");
                case XNode::Kind::Comment: return Value("comment");
                case XNode::Kind::PI: return Value("processing-instruction");
                default: return Value("text");
            }
        });
        b.method("parent", [nodesFor](VM& vm, const Value& self, Args&) {
            const auto nodes = nodesFor(self);
            if (nodes.empty()) return Value();
            auto p = nodes[0]->parent.lock();
            return p ? wrap(vm, p) : Value();
        });
        b.method("hasSimpleContent", [nodesFor](VM&, const Value& self, Args&) {
            const auto nodes = nodesFor(self);
            return Value(std::none_of(nodes.begin(), nodes.end(), [](const NodePtr& n) { return n->kind == XNode::Kind::Element && hasElementChildren(n); }));
        });
        b.method("hasComplexContent", [nodesFor](VM&, const Value& self, Args&) {
            const auto nodes = nodesFor(self);
            return Value(std::any_of(nodes.begin(), nodes.end(), [](const NodePtr& n) { return n->kind == XNode::Kind::Element && hasElementChildren(n); }));
        });
        b.method("hasOwnProperty", [nodesFor](VM& vm, const Value& self, Args& a) {
            const std::string name = vm.toString(arg(a, 0));
            Multiname mn = Multiname::publicName(name);
            for (const auto& n : nodesFor(self)) {
                std::vector<NodePtr> found;
                childrenNamed(n, mn, name, found);
                if (!found.empty()) return Value(true);
            }
            return Value(false);
        });
        b.method("copy", [nodesFor, isList](VM& vm, const Value& self, Args&) {
            std::function<NodePtr(const NodePtr&)> clone = [&](const NodePtr& n) {
                auto c = std::make_shared<XNode>(*n);
                c->wrapper.reset();
                c->parent.reset();
                for (auto& a : c->attrs) { a = clone(a); a->parent = c; }
                for (auto& ch : c->children) { ch = clone(ch); ch->parent = c; }
                return c;
            };
            std::vector<NodePtr> out;
            for (const auto& n : nodesFor(self)) out.push_back(clone(n));
            if (isList) return makeList(vm, std::move(out));
            return out.empty() ? Value::null() : wrap(vm, out[0]);
        });
        b.method("normalize", [](VM&, const Value& self, Args&) { return self; });
        b.method("contains", [nodesFor](VM&, const Value& self, Args& a) {
            const auto mine = nodesFor(self), other = nodesOf(arg(a, 0));
            return Value(!other.empty() && std::find(mine.begin(), mine.end(), other[0]) != mine.end());
        });
        b.method("appendChild", [nodesFor](VM& vm, const Value& self, Args& a) {
            const auto nodes = nodesFor(self);
            if (nodes.empty()) return self;
            auto add = nodesOf(arg(a, 0));
            if (add.empty() && !arg(a, 0).isNullish()) add.push_back(newText(vm.toString(arg(a, 0))));
            for (auto& c : add) { c->parent = nodes[0]; nodes[0]->children.push_back(c); }
            return self;
        });
    }
    // XML-only static settings the framework code writes to.
    for (const char* flag : {"ignoreWhitespace", "ignoreComments", "ignoreProcessingInstructions", "prettyPrinting"}) {
        xml->dynamic.set(flag, Value(true));
    }
    xml->dynamic.set("prettyIndent", Value(2));
}

} // namespace fp::avm2
