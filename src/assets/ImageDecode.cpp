// Built-in PNG and GIF decoders for the payloads allowed in DefineBitsJPEG2/3 (SWF 8+).
#include "flashport/Image.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <zlib.h>

namespace flashport {
namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

std::vector<std::uint8_t> inflateStream(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out;
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return out;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    std::uint8_t buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - zs.avail_out));
    }
    inflateEnd(&zs);
    return out;
}

} // namespace

bool decodePNG(const std::uint8_t* d, std::size_t n, ImageRGBA& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n < 8 || std::memcmp(d, sig, 8) != 0) return fail("not a PNG");
    int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    std::vector<std::uint8_t> idat, palette, trns;
    for (std::size_t pos = 8; pos + 12 <= n;) {
        const auto len = be32(d + pos);
        if (pos + 12 + len > n) break;
        const std::uint8_t* type = d + pos + 4;
        const std::uint8_t* c = d + pos + 8;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = static_cast<int>(be32(c));
            h = static_cast<int>(be32(c + 4));
            depth = c[8];
            color = c[9];
            interlace = c[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(c, c + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(c, c + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), c, c + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return fail("bad PNG size");
    const int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!channels || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16))
        return fail("unsupported PNG format");
    const std::size_t bpp = static_cast<std::size_t>(std::max(1, channels * depth / 8));
    const auto raw = inflateStream(idat);

    // Raw sample (16-bit kept whole) and sample scaled to 8 bits.
    auto rawSample = [&](const std::uint8_t* row, int x, int ch) -> int {
        if (depth == 16) return (row[(x * channels + ch) * 2] << 8) | row[(x * channels + ch) * 2 + 1];
        if (depth == 8) return row[x * channels + ch];
        const int bit = x * depth;
        return (row[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
    };
    auto sample8 = [&](const std::uint8_t* row, int x, int ch) -> int {
        const int v = rawSample(row, x, ch);
        if (depth == 16) return v >> 8;
        if (depth == 8 || color == 3) return v;
        return v * 255 / ((1 << depth) - 1);
    };

    out.width = w;
    out.height = h;
    out.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    static const int ax0[] = {0, 4, 0, 2, 0, 1, 0}, ay0[] = {0, 0, 4, 0, 2, 0, 1};
    static const int adx[] = {8, 8, 4, 4, 2, 2, 1}, ady[] = {8, 8, 8, 4, 4, 2, 2};
    std::size_t pos = 0;
    for (int pass = 0; pass < (interlace ? 7 : 1); ++pass) {
        const int x0 = interlace ? ax0[pass] : 0, y0 = interlace ? ay0[pass] : 0;
        const int dx = interlace ? adx[pass] : 1, dy = interlace ? ady[pass] : 1;
        const int pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        const std::size_t stride = (static_cast<std::size_t>(pw) * channels * depth + 7) / 8;
        std::vector<std::uint8_t> prev(stride, 0), cur(stride);
        for (int y = 0; y < ph; ++y) {
            if (pos + 1 + stride > raw.size()) return fail("PNG data truncated");
            const int filter = raw[pos++];
            for (std::size_t i = 0; i < stride; ++i) {
                const int a = i >= bpp ? cur[i - bpp] : 0;
                const int b = prev[i];
                const int c = i >= bpp ? prev[i - bpp] : 0;
                int v = raw[pos + i];
                switch (filter) {
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) / 2; break;
                    case 4: v += paeth(a, b, c); break;
                    default: break;
                }
                cur[i] = static_cast<std::uint8_t>(v);
            }
            pos += stride;
            const std::uint8_t* row = cur.data();
            for (int x = 0; x < pw; ++x) {
                std::uint8_t* px = &out.pixels[(static_cast<std::size_t>(y0 + y * dy) * w + (x0 + x * dx)) * 4];
                int r = 0, g = 0, b = 0, al = 255;
                switch (color) {
                    case 0:
                        r = g = b = sample8(row, x, 0);
                        if (trns.size() >= 2 && rawSample(row, x, 0) == ((trns[0] << 8) | trns[1])) al = 0;
                        break;
                    case 2:
                        r = sample8(row, x, 0);
                        g = sample8(row, x, 1);
                        b = sample8(row, x, 2);
                        if (trns.size() >= 6 && rawSample(row, x, 0) == ((trns[0] << 8) | trns[1]) &&
                            rawSample(row, x, 1) == ((trns[2] << 8) | trns[3]) &&
                            rawSample(row, x, 2) == ((trns[4] << 8) | trns[5]))
                            al = 0;
                        break;
                    case 3: {
                        const auto i = static_cast<std::size_t>(rawSample(row, x, 0));
                        if (3 * i + 2 < palette.size()) {
                            r = palette[3 * i];
                            g = palette[3 * i + 1];
                            b = palette[3 * i + 2];
                        }
                        if (i < trns.size()) al = trns[i];
                        break;
                    }
                    case 4:
                        r = g = b = sample8(row, x, 0);
                        al = sample8(row, x, 1);
                        break;
                    default:
                        r = sample8(row, x, 0);
                        g = sample8(row, x, 1);
                        b = sample8(row, x, 2);
                        al = sample8(row, x, 3);
                        break;
                }
                px[0] = static_cast<std::uint8_t>(r);
                px[1] = static_cast<std::uint8_t>(g);
                px[2] = static_cast<std::uint8_t>(b);
                px[3] = static_cast<std::uint8_t>(al);
            }
            std::swap(prev, cur);
        }
    }
    return true;
}

bool decodeGIF(const std::uint8_t* d, std::size_t n, ImageRGBA& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (n < 13 || std::memcmp(d, "GIF8", 4) != 0) return fail("not a GIF");
    const int w = d[6] | (d[7] << 8), h = d[8] | (d[9] << 8);
    if (w <= 0 || h <= 0) return fail("bad GIF size");
    std::size_t pos = 13;
    std::vector<std::uint8_t> globalPal;
    if (d[10] & 0x80) {
        const std::size_t sz = 3u << ((d[10] & 7) + 1);
        if (pos + sz > n) return fail("GIF palette truncated");
        globalPal.assign(d + pos, d + pos + sz);
        pos += sz;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    int transparent = -1;
    auto skipBlocks = [&] {
        while (pos < n && d[pos]) pos += 1 + d[pos];
        ++pos;
    };
    while (pos < n) {
        const auto tag = d[pos++];
        if (tag == 0x21) { // extension; graphic control carries the transparent index
            if (pos >= n) break;
            const auto label = d[pos++];
            if (label == 0xf9 && pos + 4 < n && d[pos] >= 4 && (d[pos + 1] & 1)) transparent = d[pos + 4];
            skipBlocks();
            continue;
        }
        if (tag != 0x2c) break;
        if (pos + 9 > n) return fail("GIF image truncated");
        const int ix = d[pos] | (d[pos + 1] << 8), iy = d[pos + 2] | (d[pos + 3] << 8);
        const int iw = d[pos + 4] | (d[pos + 5] << 8), ih = d[pos + 6] | (d[pos + 7] << 8);
        const auto flags = d[pos + 8];
        pos += 9;
        std::vector<std::uint8_t> pal = globalPal;
        if (flags & 0x80) {
            const std::size_t sz = 3u << ((flags & 7) + 1);
            if (pos + sz > n) return fail("GIF palette truncated");
            pal.assign(d + pos, d + pos + sz);
            pos += sz;
        }
        if (pos >= n || iw <= 0 || ih <= 0) return fail("GIF data truncated");
        const int minCode = d[pos++];
        if (minCode < 1 || minCode > 11) return fail("bad GIF LZW code size");
        std::vector<std::uint8_t> data;
        while (pos < n && d[pos]) {
            const std::size_t len = d[pos];
            if (pos + 1 + len > n) break;
            data.insert(data.end(), d + pos + 1, d + pos + 1 + len);
            pos += 1 + len;
        }

        // LZW
        const std::size_t total = static_cast<std::size_t>(iw) * ih;
        std::vector<std::uint8_t> indices;
        indices.reserve(total);
        const int clear = 1 << minCode, eoi = clear + 1;
        std::vector<std::uint16_t> prefix(4096, 0);
        std::vector<std::uint8_t> suffix(4096, 0), stack(4097);
        for (int i = 0; i < clear; ++i) suffix[i] = static_cast<std::uint8_t>(i);
        int codeSize = minCode + 1, next = clear + 2, old = -1;
        std::uint8_t first = 0;
        std::uint32_t acc = 0;
        int bits = 0;
        std::size_t bp = 0;
        while (indices.size() < total) {
            while (bits < codeSize && bp < data.size()) {
                acc |= std::uint32_t(data[bp++]) << bits;
                bits += 8;
            }
            if (bits < codeSize) break;
            const int code = static_cast<int>(acc & ((1u << codeSize) - 1));
            acc >>= codeSize;
            bits -= codeSize;
            if (code == clear) {
                codeSize = minCode + 1;
                next = clear + 2;
                old = -1;
                continue;
            }
            if (code == eoi) break;
            if (old < 0) {
                if (code >= clear) break;
                indices.push_back(static_cast<std::uint8_t>(code));
                old = code;
                first = static_cast<std::uint8_t>(code);
                continue;
            }
            int sp = 0, c = code;
            if (code >= next) {
                stack[sp++] = first;
                c = old;
            }
            while (c >= clear && sp < 4096) {
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = static_cast<std::uint8_t>(c);
            first = static_cast<std::uint8_t>(c);
            while (sp) indices.push_back(stack[--sp]);
            if (next < 4096) {
                prefix[next] = static_cast<std::uint16_t>(old);
                suffix[next] = first;
                ++next;
                if (next == (1 << codeSize) && codeSize < 12) ++codeSize;
            }
            old = code;
        }

        std::vector<int> rows;
        if (flags & 0x40) {
            for (int y = 0; y < ih; y += 8) rows.push_back(y);
            for (int y = 4; y < ih; y += 8) rows.push_back(y);
            for (int y = 2; y < ih; y += 4) rows.push_back(y);
            for (int y = 1; y < ih; y += 2) rows.push_back(y);
        } else {
            for (int y = 0; y < ih; ++y) rows.push_back(y);
        }
        for (std::size_t k = 0; k < indices.size() && k < total; ++k) {
            const int x = ix + static_cast<int>(k % iw), y = iy + rows[k / iw];
            const int idx = indices[k];
            if (x >= w || y >= h || idx == transparent) continue;
            const std::size_t pi = static_cast<std::size_t>(idx) * 3;
            if (pi + 2 >= pal.size()) continue;
            std::uint8_t* px = &out.pixels[(static_cast<std::size_t>(y) * w + x) * 4];
            px[0] = pal[pi];
            px[1] = pal[pi + 1];
            px[2] = pal[pi + 2];
            px[3] = 255;
        }
        return true; // Flash only shows the first frame
    }
    return fail("GIF has no image");
}

} // namespace flashport
