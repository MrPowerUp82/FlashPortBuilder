#include "flashport/Audio.hpp"
#include <algorithm>
#include <cstring>
#ifdef FLASHPORT_HAVE_MPG123
#include <mpg123.h>
#endif

namespace flashport {
namespace {

class MsbBits {
public:
    MsbBits(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}
    bool has(unsigned bits) const { return pos_ + bits <= n_ * 8; }
    std::uint32_t read(unsigned bits) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < bits; ++i, ++pos_) v = (v << 1) | ((d_[pos_ / 8] >> (7 - pos_ % 8)) & 1u);
        return v;
    }
private:
    const std::uint8_t* d_;
    std::size_t n_;
    std::size_t pos_ = 0;
};

} // namespace

std::vector<std::uint8_t> decodeAdpcm(const std::uint8_t* d, std::size_t n, unsigned channels) {
    static const int steps[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
        88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
        3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int idx2[] = {-1, 2};
    static const int idx3[] = {-1, -1, 2, 4};
    static const int idx4[] = {-1, -1, -1, -1, 2, 4, 6, 8};
    static const int idx5[] = {-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16};
    static const int* tables[] = {idx2, idx3, idx4, idx5};

    std::vector<std::uint8_t> pcm;
    MsbBits b(d, n);
    if (!b.has(2)) return pcm;
    const unsigned bits = b.read(2) + 2;
    const int* table = tables[bits - 2];
    const std::uint32_t signMask = 1u << (bits - 1);
    auto put = [&](int s) { pcm.push_back(static_cast<std::uint8_t>(s & 0xff)); pcm.push_back(static_cast<std::uint8_t>((s >> 8) & 0xff)); };

    while (b.has(22 * channels)) {
        int sample[2] = {0, 0}, index[2] = {0, 0};
        for (unsigned c = 0; c < channels; ++c) {
            sample[c] = static_cast<std::int16_t>(b.read(16));
            index[c] = static_cast<int>(b.read(6));
            put(sample[c]);
        }
        for (int i = 1; i < 4096 && b.has(bits * channels); ++i) {
            for (unsigned c = 0; c < channels; ++c) {
                const auto code = b.read(bits);
                const int step = steps[index[c]];
                int diff = step >> (bits - 1);
                for (unsigned k = 0; k + 1 < bits; ++k) {
                    if (code & (1u << (bits - 2 - k))) diff += step >> k;
                }
                sample[c] += (code & signMask) ? -diff : diff;
                sample[c] = std::clamp(sample[c], -32768, 32767);
                index[c] = std::clamp(index[c] + table[code & (signMask - 1)], 0, 88);
                put(sample[c]);
            }
        }
    }
    return pcm;
}

bool haveMp3Decoder() {
#ifdef FLASHPORT_HAVE_MPG123
    return true;
#else
    return false;
#endif
}

namespace {

bool decodeMp3(const std::uint8_t* data, std::size_t size, PCMSound& out, std::string* error) {
#ifdef FLASHPORT_HAVE_MPG123
    static const bool initialised = mpg123_init() == MPG123_OK;
    if (!initialised) { if (error) *error = "mpg123_init failed"; return false; }
    int err = 0;
    mpg123_handle* h = mpg123_new(nullptr, &err);
    if (!h) { if (error) *error = "mpg123_new failed"; return false; }
    mpg123_param(h, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
    mpg123_format_none(h);
    const long* rates = nullptr;
    std::size_t rateCount = 0;
    mpg123_rates(&rates, &rateCount);
    for (std::size_t i = 0; i < rateCount; ++i) mpg123_format(h, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_open_feed(h);

    bool gotFormat = false;
    std::vector<std::uint8_t> buf(65536);
    auto drain = [&] {
        while (true) {
            std::size_t done = 0;
            const int rc = mpg123_read(h, buf.data(), buf.size(), &done);
            if (rc == MPG123_NEW_FORMAT || (!gotFormat && done)) {
                long r = 0;
                int ch = 0, enc = 0;
                if (mpg123_getformat(h, &r, &ch, &enc) == MPG123_OK && r > 0) {
                    out.rate = static_cast<unsigned>(r);
                    out.channels = static_cast<unsigned>(ch);
                    gotFormat = true;
                }
            }
            if (done) {
                const auto* s = reinterpret_cast<const std::int16_t*>(buf.data());
                out.samples.insert(out.samples.end(), s, s + done / 2);
            }
            if (rc == MPG123_NEED_MORE || rc == MPG123_ERR || rc == MPG123_DONE) break;
            if (rc == MPG123_OK && done == 0) break;
        }
    };
    // Feed in chunks: the decoder buffers frames internally.
    constexpr std::size_t kChunk = 16384;
    for (std::size_t pos = 0; pos < size; pos += kChunk) {
        const int rc = mpg123_feed(h, data + pos, std::min(kChunk, size - pos));
        if (rc != MPG123_OK) break;
        drain();
    }
    mpg123_delete(h);
    if (!gotFormat || out.samples.empty()) { if (error) *error = "no MP3 frames decoded"; return false; }
    return true;
#else
    (void)data; (void)size; (void)out;
    if (error) *error = "built without libmpg123";
    return false;
#endif
}

} // namespace

bool decodeSwfSound(unsigned format, unsigned rate, bool is16, bool stereo, const std::uint8_t* data, std::size_t size,
                    unsigned seekSamples, PCMSound& out, std::string* error) {
    out = PCMSound{};
    const unsigned channels = stereo ? 2 : 1;
    switch (format) {
        case 0: case 3: { // PCM (format 0 is platform-endian; every real SWF is little-endian)
            out.rate = rate;
            out.channels = channels;
            if (is16) {
                out.samples.resize(size / 2);
                for (std::size_t i = 0; i < out.samples.size(); ++i)
                    out.samples[i] = static_cast<std::int16_t>(data[2 * i] | (data[2 * i + 1] << 8));
            } else {
                out.samples.resize(size);
                for (std::size_t i = 0; i < size; ++i) out.samples[i] = static_cast<std::int16_t>((int(data[i]) - 128) << 8);
            }
            break;
        }
        case 1: {
            const auto bytes = decodeAdpcm(data, size, channels);
            out.rate = rate;
            out.channels = channels;
            out.samples.resize(bytes.size() / 2);
            for (std::size_t i = 0; i < out.samples.size(); ++i)
                out.samples[i] = static_cast<std::int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
            break;
        }
        case 2:
            if (!decodeMp3(data, size, out, error)) return false;
            break;
        default:
            if (error) *error = "unsupported sound codec " + std::to_string(format);
            return false;
    }
    if (out.samples.empty()) { if (error) *error = "empty sound"; return false; }
    if (seekSamples && seekSamples < out.frames()) {
        out.samples.erase(out.samples.begin(), out.samples.begin() + static_cast<std::ptrdiff_t>(seekSamples * out.channels));
    }
    return true;
}

} // namespace flashport
