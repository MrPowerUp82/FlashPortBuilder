#include "flashport/Image.hpp"
#include <stdexcept>
#include <zlib.h>
#ifdef FLASHPORT_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace flashport {
namespace {

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

void chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data) {
    put32(png, static_cast<std::uint32_t>(data.size()));
    const auto start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    const auto crc = crc32(0L, png.data() + start, static_cast<uInt>(png.size() - start));
    put32(png, static_cast<std::uint32_t>(crc));
}

} // namespace

std::vector<std::uint8_t> encodePNG(const ImageRGBA& img) {
    std::vector<std::uint8_t> raw;
    const std::size_t stride = static_cast<std::size_t>(img.width) * 4;
    raw.reserve((stride + 1) * static_cast<std::size_t>(img.height));
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0); // filter: none
        const auto* row = img.pixels.data() + static_cast<std::size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        throw std::runtime_error("PNG deflate failed");
    }
    z.resize(zlen);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(img.width));
    put32(ihdr, static_cast<std::uint32_t>(img.height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, no filter, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

bool haveJPEGDecoder() {
#ifdef FLASHPORT_HAVE_TURBOJPEG
    return true;
#else
    return false;
#endif
}

bool decodeJPEG(const std::vector<std::uint8_t>& jpeg, ImageRGBA& out, std::string* error) {
#ifdef FLASHPORT_HAVE_TURBOJPEG
    tjhandle h = tjInitDecompress();
    if (!h) { if (error) *error = "tjInitDecompress failed"; return false; }
    int w = 0, hgt = 0, subsamp = 0, cs = 0;
    auto* src = const_cast<unsigned char*>(jpeg.data());
    const auto len = static_cast<unsigned long>(jpeg.size());
    bool ok = tjDecompressHeader3(h, src, len, &w, &hgt, &subsamp, &cs) == 0 && w > 0 && hgt > 0;
    if (ok) {
        out.width = w;
        out.height = hgt;
        out.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(hgt) * 4, 0);
        // Warnings (e.g. premature end of data) still produce a usable image, like Flash does.
        if (tjDecompress2(h, src, len, out.pixels.data(), w, 0, hgt, TJPF_RGBA, TJFLAG_ACCURATEDCT) != 0 &&
            tjGetErrorCode(h) == TJERR_FATAL) {
            ok = false;
        }
    }
    if (!ok && error) *error = tjGetErrorStr2(h);
    tjDestroy(h);
    return ok;
#else
    (void)jpeg; (void)out;
    if (error) *error = "built without libjpeg-turbo";
    return false;
#endif
}

} // namespace flashport
