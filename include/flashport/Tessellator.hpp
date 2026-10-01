#pragma once
#include "flashport/Image.hpp"
#include "flashport/Shape.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flashport {

struct MeshVertex {
    float x{}, y{};          // twips, shape space
    std::uint8_t rgba[4]{};  // straight alpha
    float u{}, v{};
};

// Triangles sharing one texture (-1 = untextured, vertex colours only).
struct Mesh {
    std::int32_t texture = -1;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Gradient fills become small textures; identical gradients share one.
class GradientAtlas {
public:
    explicit GradientAtlas(std::uint32_t firstId) : nextId_(firstId) {}
    std::uint32_t textureFor(const FillStyle& fill);
    const std::map<std::uint32_t, ImageRGBA>& textures() const { return textures_; }

private:
    std::uint32_t nextId_;
    std::map<std::string, std::uint32_t> byKey_;
    std::map<std::uint32_t, ImageRGBA> textures_;
};

// Convert the shape's fill contours (even-odd or nonzero) and strokes into triangle
// meshes, in drawing order. Bitmap fills reference the bitmap's character id as texture.
std::vector<Mesh> tessellateShape(const Shape& shape, const std::map<std::uint16_t, BitmapInfo>& bitmaps,
                                  GradientAtlas& gradients);

} // namespace flashport
