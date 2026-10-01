#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace flashport {

// Straight (non-premultiplied) 8-bit RGBA image.
struct ImageRGBA {
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels; // width * height * 4
};

std::vector<std::uint8_t> encodePNG(const ImageRGBA& img);

// Decode baseline/progressive JPEG to RGBA. Returns false when no JPEG decoder was
// compiled in (FLASHPORT_HAVE_TURBOJPEG) or the data is corrupt.
bool decodeJPEG(const std::vector<std::uint8_t>& jpeg, ImageRGBA& out, std::string* error = nullptr);
bool haveJPEGDecoder();

// Built-in decoders for the PNG/GIF payloads allowed in DefineBitsJPEG2/3 (SWF 8+).
bool decodePNG(const std::uint8_t* data, std::size_t size, ImageRGBA& out, std::string* error = nullptr);
bool decodeGIF(const std::uint8_t* data, std::size_t size, ImageRGBA& out, std::string* error = nullptr);

} // namespace flashport
