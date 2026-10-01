#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <zlib.h>

namespace fp {

const std::vector<std::int16_t>& SoundDef::samples() const {
    if (loaded) return pcm;
    loaded = true;
    const std::size_t count = static_cast<std::size_t>(frames) * channels;
    std::vector<std::uint8_t> raw(count * 2);
    uLongf len = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &len, z.data(), static_cast<uLong>(z.size())) != Z_OK) len = 0;
    pcm.assign(count, 0);
    for (std::size_t i = 0; i < count && i * 2 + 1 < len; ++i) {
        pcm[i] = static_cast<std::int16_t>(raw[2 * i] | (raw[2 * i + 1] << 8));
    }
    std::vector<std::uint8_t>().swap(z);
    return pcm;
}

Audio::~Audio() {
    if (device_) SDL_CloseAudioDevice(device_);
}

void Audio::open(bool realDevice) {
    if (!realDevice) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::cerr << "Audio disabled: " << SDL_GetError() << "\n";
        return;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Audio::callback;
    want.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device_) {
        std::cerr << "Audio disabled: " << SDL_GetError() << "\n";
        return;
    }
    deviceRate_ = have.freq;
    if (std::getenv("FP_TRACE_AUDIO")) std::fprintf(stderr, "[audio] device open: %d Hz, %d channels\n", have.freq, int(have.channels));
    SDL_PauseAudioDevice(device_, 0);
}

void Audio::lock() { if (device_) SDL_LockAudioDevice(device_); }
void Audio::unlock() { if (device_) SDL_UnlockAudioDevice(device_); }

int Audio::play(const SoundDef& def, std::uint32_t soundId, double offsetMs, int loops, float volume, float pan,
                const SoundPlayDef* info) {
    const auto& pcm = def.samples();
    if (pcm.empty() || def.frames == 0) return 0;
    Voice v;
    v.soundId = soundId;
    v.def = &def;
    v.data = pcm.data();
    v.frames = def.frames;
    v.step = static_cast<double>(def.rate) / deviceRate_;
    v.start = 0;
    v.end = def.frames;
    if (info) {
        if ((info->flags & 0x08) && info->inPoint < def.frames) v.start = info->inPoint;
        if ((info->flags & 0x10) && info->outPoint > v.start && info->outPoint <= def.frames) v.end = info->outPoint;
    }
    v.pos = v.start + std::max(0.0, offsetMs) * def.rate / 1000.0;
    if (v.pos >= v.end) return 0;
    if (info) v.env = info->env;
    v.loopsLeft = std::max(1, loops);
    v.volume = volume;
    v.pan = pan;
    // Debug aid: FP_TRACE_AUDIO=1 logs every started sound.
    static const bool trace = std::getenv("FP_TRACE_AUDIO") != nullptr;
    if (trace) {
        int peak = 0;
        double sum = 0;
        for (auto x : pcm) { peak = std::max(peak, std::abs(int(x))); sum += double(x) * x; }
        std::fprintf(stderr, "[audio] play sound %u (%.2fs, loops %d, vol %.2f, peak %d, rms %.0f, %u Hz x%u)\n", soundId,
                     def.durationMs() / 1000.0, v.loopsLeft, volume, peak, std::sqrt(sum / std::max<std::size_t>(1, pcm.size())),
                     def.rate, unsigned(def.channels));
    }
    lock();
    v.handle = nextHandle_++;
    voices_.push_back(v);
    const int handle = v.handle;
    unlock();
    return handle;
}

void Audio::stop(int handle) {
    lock();
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.handle == handle; }), voices_.end());
    unlock();
}

void Audio::stopSound(std::uint32_t soundId) {
    lock();
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.soundId == soundId; }), voices_.end());
    unlock();
}

void Audio::stopAll() {
    lock();
    voices_.clear();
    unlock();
}

bool Audio::playing(int handle) {
    lock();
    const bool r = std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.handle == handle; });
    unlock();
    return r;
}

bool Audio::playingSound(std::uint32_t soundId) {
    lock();
    const bool r = std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.soundId == soundId; });
    unlock();
    return r;
}

double Audio::positionMs(int handle) {
    lock();
    double ms = 0;
    for (const auto& v : voices_) {
        if (v.handle == handle) { ms = v.pos * 1000.0 / v.def->rate; break; }
    }
    unlock();
    return ms;
}

void Audio::setVoice(int handle, float volume, float pan) {
    lock();
    for (auto& v : voices_) {
        if (v.handle == handle) { v.volume = volume; v.pan = pan; }
    }
    unlock();
}

std::vector<int> Audio::takeFinished() {
    lock();
    std::vector<int> out;
    out.swap(finished_);
    unlock();
    return out;
}

void Audio::advance(double ms) {
    if (device_) return;
    std::vector<std::int16_t> scratch(2048 * 2);
    auto frames = static_cast<long>(ms * deviceRate_ / 1000.0);
    while (frames > 0) {
        const int n = static_cast<int>(std::min<long>(frames, 2048));
        mix(scratch.data(), n);
        if (recording_) recorded_.insert(recorded_.end(), scratch.begin(), scratch.begin() + n * 2);
        frames -= n;
    }
}

bool Audio::saveWav(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(recorded_.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(static_cast<std::uint32_t>(deviceRate_)); u32(static_cast<std::uint32_t>(deviceRate_) * 4); u16(4); u16(16);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(recorded_.data()), bytes);
    return static_cast<bool>(f);
}

void Audio::callback(void* user, Uint8* stream, int len) {
    static_cast<Audio*>(user)->mix(reinterpret_cast<std::int16_t*>(stream), len / 4);
}

// Stereo 16-bit output; linear interpolation resamples each voice to the device rate.
void Audio::mix(std::int16_t* out, int frames) {
    std::vector<std::int32_t> acc(static_cast<std::size_t>(frames) * 2, 0);
    for (auto& v : voices_) {
        const int ch = v.def->channels;
        const float left = v.volume * (v.pan > 0 ? 1.0f - v.pan : 1.0f) * masterVolume;
        const float right = v.volume * (v.pan < 0 ? 1.0f + v.pan : 1.0f) * masterVolume;
        for (int i = 0; i < frames; ++i) {
            if (v.pos >= v.end) {
                if (v.loopsLeft > 1) {
                    --v.loopsLeft;
                    v.pos = v.start + (v.pos - v.end);
                    if (v.pos >= v.end) v.pos = v.start;
                    v.envIdx = 0;
                } else {
                    break;
                }
            }
            const auto idx = static_cast<std::uint32_t>(v.pos);
            const float frac = static_cast<float>(v.pos - idx);
            const std::uint32_t next = idx + 1 < v.end ? idx + 1 : idx;
            float l, r;
            if (ch == 1) {
                l = r = v.data[idx] + (v.data[next] - v.data[idx]) * frac;
            } else {
                l = v.data[idx * 2] + (v.data[next * 2] - v.data[idx * 2]) * frac;
                r = v.data[idx * 2 + 1] + (v.data[next * 2 + 1] - v.data[idx * 2 + 1]) * frac;
            }
            float envL = 1, envR = 1;
            if (!v.env.empty()) {
                // Piecewise-linear envelope over the sound's position (in 44.1 kHz samples).
                const double pos44 = v.pos * 44100.0 / v.def->rate;
                while (v.envIdx + 1 < v.env.size() && pos44 >= v.env[v.envIdx + 1].pos) ++v.envIdx;
                const auto& e0 = v.env[v.envIdx];
                if (v.envIdx + 1 < v.env.size() && pos44 > e0.pos) {
                    const auto& e1 = v.env[v.envIdx + 1];
                    const float t = static_cast<float>((pos44 - e0.pos) / std::max<double>(1.0, double(e1.pos) - e0.pos));
                    envL = e0.left + (e1.left - e0.left) * t;
                    envR = e0.right + (e1.right - e0.right) * t;
                } else {
                    envL = e0.left;
                    envR = e0.right;
                }
            }
            acc[static_cast<std::size_t>(i) * 2] += static_cast<std::int32_t>(l * left * envL);
            acc[static_cast<std::size_t>(i) * 2 + 1] += static_cast<std::int32_t>(r * right * envR);
            v.pos += v.step;
        }
    }
    // Soft knee above 75% of full scale: many overlapping effects saturate gently instead of hard clipping.
    constexpr float kKnee = 24576.0f, kHeadroom = 32767.0f - kKnee;
    for (std::size_t i = 0; i < acc.size(); ++i) {
        const float x = static_cast<float>(acc[i]);
        const float a = std::fabs(x);
        const float y = a <= kKnee ? x : std::copysign(kKnee + kHeadroom * std::tanh((a - kKnee) / kHeadroom), x);
        out[i] = static_cast<std::int16_t>(std::clamp(y, -32768.0f, 32767.0f));
    }
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (it->pos >= it->end && it->loopsLeft <= 1) {
            finished_.push_back(it->handle);
            it = voices_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace fp
