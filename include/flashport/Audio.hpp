#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flashport {

// Decoded sound: interleaved signed 16-bit samples.
struct PCMSound {
    unsigned rate = 0;
    unsigned channels = 1;
    std::vector<std::int16_t> samples;
    std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// SWF ADPCM (IMA-like, 2..5 bits per sample, 4096-sample packets) -> little-endian 16-bit PCM bytes.
std::vector<std::uint8_t> decodeAdpcm(const std::uint8_t* data, std::size_t size, unsigned channels);

// True when an MP3 decoder (libmpg123) was compiled in.
bool haveMp3Decoder();

// Decode a DefineSound / SoundStreamBlock payload. `format` is the SWF sound format (0/3 PCM,
// 1 ADPCM, 2 MP3); `rate` is the Hz value from the flags. `seekSamples` (MP3 latency) is dropped
// from the start. Returns false for unsupported codecs (Nellymoser, Speex) or corrupt data.
bool decodeSwfSound(unsigned format, unsigned rate, bool is16, bool stereo, const std::uint8_t* data, std::size_t size,
                    unsigned seekSamples, PCMSound& out, std::string* error = nullptr);

} // namespace flashport
