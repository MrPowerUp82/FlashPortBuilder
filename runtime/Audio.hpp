#pragma once
// FlashPort audio: decoded PCM sounds from the movie pack mixed through SDL (or silently
// advanced when no audio device is available, e.g. in scripted captures).
#include <SDL.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fp {

// One sound of the pack. The PCM stays zlib-compressed until the first play.
struct SoundDef {
    std::uint32_t rate = 44100;
    std::uint8_t channels = 1;
    std::uint32_t frames = 0;
    mutable std::vector<std::uint8_t> z;
    mutable std::vector<std::int16_t> pcm;
    mutable bool loaded = false;
    const std::vector<std::int16_t>& samples() const;
    double durationMs() const { return rate ? frames * 1000.0 / rate : 0.0; }
};

// SOUNDINFO of a StartSound tag or button sound.
struct SoundPlayDef {
    std::uint8_t flags = 0;          // 0x01 stop, 0x02 no multiple, 0x04 stream, 0x08 has in point, 0x10 has out point
    std::uint32_t inPoint = 0, outPoint = 0; // samples at the sound's own rate
    std::uint16_t loops = 1;
    struct Env { std::uint32_t pos; float left, right; }; // pos in 44.1 kHz samples, levels 0..1
    std::vector<Env> env;
};

class Audio {
public:
    ~Audio();
    // Opens the device; without one (or when `realDevice` is false) time is advanced by advance().
    void open(bool realDevice);
    bool hasDevice() const { return device_ != 0; }

    // Starts a sound; `loops` is the total number of plays (0x7fffffff = forever). Returns a handle.
    int play(const SoundDef& def, std::uint32_t soundId, double offsetMs, int loops, float volume, float pan,
             const SoundPlayDef* info = nullptr);
    void stop(int handle);
    void stopSound(std::uint32_t soundId);
    void stopAll();
    bool playing(int handle);
    bool playingSound(std::uint32_t soundId);
    double positionMs(int handle);
    void setVoice(int handle, float volume, float pan);
    // Handles of sounds that ran to their end since the last call.
    std::vector<int> takeFinished();
    // Virtual mode: moves every voice forward by `ms` of audio.
    void advance(double ms);
    // Virtual mode (captures): keeps what advance() mixes so it can be saved as a WAV for checks.
    void record() { recording_ = true; }
    bool saveWav(const std::string& path) const;

    float masterVolume = 1.0f;

private:
    struct Voice {
        int handle = 0;
        std::uint32_t soundId = 0;
        const SoundDef* def = nullptr;
        const std::int16_t* data = nullptr;
        std::uint32_t frames = 0;
        double pos = 0, step = 1;
        std::uint32_t start = 0, end = 0;
        int loopsLeft = 1;
        float volume = 1, pan = 0;
        std::vector<SoundPlayDef::Env> env;
        std::size_t envIdx = 0;
    };
    static void callback(void* user, Uint8* stream, int len);
    void mix(std::int16_t* out, int frames);
    void lock();
    void unlock();

    SDL_AudioDeviceID device_ = 0;
    int deviceRate_ = 44100;
    std::vector<Voice> voices_;
    std::vector<int> finished_;
    int nextHandle_ = 1;
    bool recording_ = false;
    std::vector<std::int16_t> recorded_;
};

} // namespace fp
