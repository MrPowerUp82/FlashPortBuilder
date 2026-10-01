#include "flashport/Tessellator.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace flashport {
namespace {

constexpr double kEps = 0.01;          // twips
constexpr double kGradientHalf = 16384; // gradient square is -16384..16384

struct Seg {
    double x0, y0, x1, y1; // y0 < y1
    int wind;
    double xAt(double y) const { return x0 + (x1 - x0) * (y - y0) / (y1 - y0); }
};

struct Pt { double x, y; };

// Quadratic curves are flattened by error: a quadratic split into n segments deviates
// from its chords by at most |p0 - 2c + p1| / (4 n^2); keep that under ~0.4px (8 twips).
void flatten(const Edge& e, std::vector<Pt>& out) {
    if (!e.curved) { out.push_back({double(e.to.x), double(e.to.y)}); return; }
    const double dev = std::hypot(e.from.x - 2.0 * e.control.x + e.to.x, e.from.y - 2.0 * e.control.y + e.to.y) / 4.0;
    const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(dev / 8.0))), 1, 32);
    for (int i = 1; i <= n; ++i) {
        const double t = double(i) / n, it = 1 - t;
        out.push_back({it * it * e.from.x + 2 * it * t * e.control.x + t * t * e.to.x,
                       it * it * e.from.y + 2 * it * t * e.control.y + t * t * e.to.y});
    }
}

std::vector<Pt> polyline(const std::vector<Edge>& contour) {
    std::vector<Pt> pts{{double(contour.front().from.x), double(contour.front().from.y)}};
    for (const auto& e : contour) flatten(e, pts);
    return pts;
}

struct Inverse {
    double a, b, c, d, tx, ty;
    bool ok = false;
    Pt apply(double x, double y) const { return {a * x + c * y + tx, b * x + d * y + ty}; }
};

Inverse invert(const Matrix& m) {
    Inverse inv{};
    const double det = m.a * m.d - m.b * m.c;
    if (std::fabs(det) < 1e-12) return inv;
    inv.a = m.d / det;
    inv.b = -m.b / det;
    inv.c = -m.c / det;
    inv.d = m.a / det;
    inv.tx = -(inv.a * m.tx + inv.c * m.ty);
    inv.ty = -(inv.b * m.tx + inv.d * m.ty);
    inv.ok = true;
    return inv;
}

// How a style colours a vertex: flat colour, or a texture with affine UVs.
struct Painter {
    std::int32_t texture = -1;
    RGBA color;
    Inverse inv;
    double uScale = 1, vScale = 1, uOffset = 0, vOffset = 0;
    bool linear = false;

    MeshVertex vertex(double x, double y) const {
        MeshVertex v;
        v.x = static_cast<float>(x);
        v.y = static_cast<float>(y);
        if (texture < 0) {
            v.rgba[0] = color.r; v.rgba[1] = color.g; v.rgba[2] = color.b; v.rgba[3] = color.a;
            return v;
        }
        v.rgba[0] = v.rgba[1] = v.rgba[2] = v.rgba[3] = 255;
        const auto p = inv.apply(x, y);
        v.u = static_cast<float>(p.x * uScale + uOffset);
        v.v = linear ? 0.5f : static_cast<float>(p.y * vScale + vOffset);
        return v;
    }
};

Painter makePainter(const FillStyle& f, const std::map<std::uint16_t, BitmapInfo>& bitmaps, GradientAtlas& grads,
                    bool& drawable) {
    Painter p;
    drawable = true;
    switch (f.type) {
        case FillStyle::Type::Solid:
            p.color = f.color;
            drawable = f.color.a > 0;
            break;
        case FillStyle::Type::Linear:
        case FillStyle::Type::Radial:
        case FillStyle::Type::Focal:
            p.inv = invert(f.matrix);
            if (!p.inv.ok || f.stops.empty()) { drawable = false; break; }
            p.texture = static_cast<std::int32_t>(grads.textureFor(f));
            p.linear = f.type == FillStyle::Type::Linear;
            p.uScale = p.vScale = 1.0 / (2 * kGradientHalf);
            p.uOffset = p.vOffset = 0.5;
            break;
        case FillStyle::Type::Bitmap: {
            const auto it = bitmaps.find(f.bitmapId);
            p.inv = invert(f.matrix);
            if (it == bitmaps.end() || !p.inv.ok || it->second.width <= 0) { drawable = false; break; }
            p.texture = f.bitmapId;
            p.uScale = 1.0 / it->second.width;
            p.vScale = 1.0 / it->second.height;
            break;
        }
    }
    return p;
}

void addQuad(Mesh& m, const Painter& p, Pt a, Pt b, Pt c, Pt d) {
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    m.vertices.push_back(p.vertex(a.x, a.y));
    m.vertices.push_back(p.vertex(b.x, b.y));
    m.vertices.push_back(p.vertex(c.x, c.y));
    m.vertices.push_back(p.vertex(d.x, d.y));
    for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

class SlabFiller {
public:
    SlabFiller(Mesh& mesh, const Painter& painter, bool evenOdd) : mesh_(mesh), p_(painter), evenOdd_(evenOdd) {}

    void fill(std::vector<Seg> segs) {
        if (segs.empty()) return;
        std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.y0 < b.y0; });
        std::vector<double> ys;
        for (const auto& s : segs) { ys.push_back(s.y0); ys.push_back(s.y1); }
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end(), [](double a, double b) { return std::fabs(a - b) < kEps; }), ys.end());

        std::vector<const Seg*> active;
        std::size_t next = 0;
        for (std::size_t k = 0; k + 1 < ys.size(); ++k) {
            const double ya = ys[k], yb = ys[k + 1];
            active.erase(std::remove_if(active.begin(), active.end(), [&](const Seg* s) { return s->y1 <= ya + kEps; }),
                         active.end());
            while (next < segs.size() && segs[next].y0 <= ya + kEps) {
                if (segs[next].y1 > ya + kEps) active.push_back(&segs[next]);
                ++next;
            }
            if (active.size() >= 2) slab(ya, yb, active);
        }
        flushAll();
    }

private:
    struct X { double a, b; int wind; const Seg* seg; };

    void slab(double ya, double yb, const std::vector<const Seg*>& active) {
        // Edges that cross inside the slab: collect every crossing between neighbours (in
        // one pass) and split the slab there, so each sub-slab has a consistent edge order.
        std::vector<X> xs = edgesAt(ya, yb, active);
        std::vector<double> cuts;
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
            const double da = xs[i].a - xs[i + 1].a, db = xs[i].b - xs[i + 1].b;
            if ((da > kEps && db < -kEps) || (da < -kEps && db > kEps)) {
                const double yc = ya + da / (da - db) * (yb - ya);
                if (yc > ya + kEps && yc < yb - kEps) cuts.push_back(yc);
            }
        }
        if (cuts.empty()) { emit(ya, yb, xs); return; }
        std::sort(cuts.begin(), cuts.end());
        cuts.push_back(yb);
        double y0 = ya;
        for (double y1 : cuts) {
            if (y1 - y0 <= kEps) continue;
            emit(y0, y1, edgesAt(y0, y1, active));
            y0 = y1;
        }
    }

    static std::vector<X> edgesAt(double ya, double yb, const std::vector<const Seg*>& active) {
        std::vector<X> xs;
        xs.reserve(active.size());
        for (const auto* s : active) xs.push_back({s->xAt(ya), s->xAt(yb), s->wind, s});
        std::sort(xs.begin(), xs.end(), [](const X& l, const X& r) { return l.a + l.b < r.a + r.b; });
        return xs;
    }

    // Spans bounded by the same (left, right) edge pair in consecutive slabs are merged
    // into one trapezoid: the edges are straight, so the union is still a trapezoid.
    // This keeps the triangle count proportional to the number of edges, not slabs x spans.
    struct Open { double top, bottom; };
    using Key = std::pair<const Seg*, const Seg*>;

    void emit(double ya, double yb, const std::vector<X>& xs) {
        std::vector<Key> spans;
        int winding = 0;
        const X* left = nullptr;
        for (const auto& x : xs) {
            const bool wasInside = evenOdd_ ? (winding & 1) != 0 : winding != 0;
            winding += evenOdd_ ? 1 : x.wind;
            const bool inside = evenOdd_ ? (winding & 1) != 0 : winding != 0;
            if (!wasInside && inside) left = &x;
            else if (wasInside && !inside && left) {
                if (x.a - left->a > 1e-3 || x.b - left->b > 1e-3) spans.emplace_back(left->seg, x.seg);
                left = nullptr;
            }
        }
        for (const auto& key : spans) {
            auto it = open_.find(key);
            if (it != open_.end() && std::fabs(it->second.bottom - ya) <= kEps) {
                it->second.bottom = yb;
            } else {
                if (it != open_.end()) flush(it->first, it->second);
                open_[key] = {ya, yb};
            }
        }
        // Anything not continued into this slab is finished.
        for (auto it = open_.begin(); it != open_.end();) {
            if (std::fabs(it->second.bottom - yb) > kEps) {
                flush(it->first, it->second);
                it = open_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void flush(const Key& key, const Open& o) {
        const auto* l = key.first;
        const auto* r = key.second;
        addQuad(mesh_, p_, {l->xAt(o.top), o.top}, {r->xAt(o.top), o.top}, {r->xAt(o.bottom), o.bottom},
                {l->xAt(o.bottom), o.bottom});
    }

public:
    void flushAll() {
        for (const auto& [key, o] : open_) flush(key, o);
        open_.clear();
    }

private:
    Mesh& mesh_;
    const Painter& p_;
    bool evenOdd_;
    std::map<Key, Open> open_;
};

void strokePolyline(Mesh& m, const Painter& p, const std::vector<Pt>& pts, double width) {
    const double hw = width / 2;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const double dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) continue;
        const double ux = dx / len * hw, uy = dy / len * hw; // square caps also cover joins
        const double nx = -uy, ny = ux;
        const Pt a{pts[i].x - ux, pts[i].y - uy}, b{pts[i + 1].x + ux, pts[i + 1].y + uy};
        addQuad(m, p, {a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny});
    }
}

RGBA sampleStops(const std::vector<GradientStop>& stops, double ratio) {
    if (ratio <= stops.front().ratio) return stops.front().color;
    if (ratio >= stops.back().ratio) return stops.back().color;
    for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
        const auto& s0 = stops[i];
        const auto& s1 = stops[i + 1];
        if (ratio > s1.ratio) continue;
        const double span = std::max(1, s1.ratio - s0.ratio);
        const double t = (ratio - s0.ratio) / span;
        auto mix = [&](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(a + (b - a) * t)); };
        return {mix(s0.color.r, s1.color.r), mix(s0.color.g, s1.color.g), mix(s0.color.b, s1.color.b), mix(s0.color.a, s1.color.a)};
    }
    return stops.back().color;
}

} // namespace

std::uint32_t GradientAtlas::textureFor(const FillStyle& f) {
    const bool linear = f.type == FillStyle::Type::Linear;
    std::ostringstream key;
    key << (linear ? 'L' : 'R');
    for (const auto& s : f.stops) key << ',' << int(s.ratio) << ':' << int(s.color.r) << '.' << int(s.color.g) << '.'
                                      << int(s.color.b) << '.' << int(s.color.a);
    if (auto it = byKey_.find(key.str()); it != byKey_.end()) return it->second;

    ImageRGBA img;
    img.width = linear ? 256 : 128;
    img.height = linear ? 1 : 128;
    img.pixels.resize(static_cast<std::size_t>(img.width * img.height) * 4);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            double ratio;
            if (linear) {
                ratio = (x + 0.5) / img.width * 255.0;
            } else {
                const double dx = (x + 0.5) / img.width * 2 - 1, dy = (y + 0.5) / img.height * 2 - 1;
                ratio = std::min(1.0, std::hypot(dx, dy)) * 255.0;
            }
            const auto c = sampleStops(f.stops, ratio);
            auto* px = &img.pixels[static_cast<std::size_t>(y * img.width + x) * 4];
            px[0] = c.r; px[1] = c.g; px[2] = c.b; px[3] = c.a;
        }
    }
    const auto id = nextId_++;
    textures_[id] = std::move(img);
    byKey_[key.str()] = id;
    return id;
}

std::vector<Mesh> tessellateShape(const Shape& shape, const std::map<std::uint16_t, BitmapInfo>& bitmaps,
                                  GradientAtlas& gradients) {
    std::vector<Mesh> meshes;
    auto meshFor = [&](std::int32_t texture) -> Mesh& {
        if (meshes.empty() || meshes.back().texture != texture) {
            meshes.emplace_back();
            meshes.back().texture = texture;
        }
        return meshes.back();
    };

    for (const auto& g : shape.groups) {
        for (const auto& path : g.fillPaths) {
            if (path.style >= g.fills.size()) continue;
            bool drawable = false;
            const auto painter = makePainter(g.fills[path.style], bitmaps, gradients, drawable);
            if (!drawable) continue;
            std::vector<Seg> segs;
            for (const auto& contour : path.contours) {
                auto pts = polyline(contour);
                pts.push_back(pts.front()); // implicit close for open contours
                for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                    const auto& a = pts[i];
                    const auto& b = pts[i + 1];
                    if (std::fabs(a.y - b.y) < kEps) continue;
                    if (a.y < b.y) segs.push_back({a.x, a.y, b.x, b.y, 1});
                    else segs.push_back({b.x, b.y, a.x, a.y, -1});
                }
            }
            SlabFiller(meshFor(painter.texture), painter, shape.evenOdd).fill(std::move(segs));
        }
        for (const auto& path : g.linePaths) {
            if (path.style >= g.lines.size()) continue;
            const auto& l = g.lines[path.style];
            bool drawable = true;
            Painter painter;
            if (l.hasFill) painter = makePainter(l.fill, bitmaps, gradients, drawable);
            else { painter.color = l.color; drawable = l.color.a > 0; }
            if (!drawable) continue;
            const double width = l.width == 0 ? 20.0 : std::max<double>(l.width, 20.0); // hairline = 1px
            auto& mesh = meshFor(painter.texture);
            for (const auto& contour : path.contours) strokePolyline(mesh, painter, polyline(contour), width);
        }
    }
    meshes.erase(std::remove_if(meshes.begin(), meshes.end(), [](const Mesh& m) { return m.indices.empty(); }),
                 meshes.end());
    return meshes;
}

} // namespace flashport
