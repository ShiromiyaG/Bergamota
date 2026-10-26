// The audio DSP's firmware, as games see it: 24 voices that play their
// buffers (PCM8, PCM16, ADPCM) and report on them, and the mixer that
// brings their three intermediate mixes down to what the speakers play.
// After Zakuro's dsp_voices.rs and dsp_mixer.rs, a sample at a time (Rust,
// MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).
#ifndef RUNTIME_AUDIO_H
#define RUNTIME_AUDIO_H

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace runtime {

class Memory;

constexpr uint32_t AUDIO_RATE = 32728;     // samples a second
constexpr size_t FRAME_SAMPLES = 160;      // samples in one audio frame

class Audio {
public:
    // one audio frame: takes what the game changed, plays, reports, mixes,
    // and adds the frame to output (stereo, interleaved)
    void tick(Memory &memory);
    // what the speakers play, waiting for the frontend; a second at most
    std::vector<int16_t> output;

    enum class Format : uint8_t { Pcm8, Pcm16, Adpcm };
    using Frame = std::array<std::array<int16_t, 2>, FRAME_SAMPLES>;
    using QuadFrame = std::array<std::array<int32_t, FRAME_SAMPLES>, 4>;

    struct Buffer {
        uint32_t address = 0, length = 0;
        Format format = Format::Pcm16;
        bool stereo = false, looping = false;
        uint16_t id = 0;
        std::optional<std::array<int16_t, 2>> adpcm; // the history to start from
        bool from_queue = false;                       // queued ones report starting
        uint32_t play_position = 0;
        bool has_played = false;
    };

    struct Voice {
        bool enabled = false;
        uint16_t sync_count = 0;
        double rate = 1.0;
        Format format = Format::Pcm16;
        bool stereo = false;
        bool interpolate = true;
        // the first order filter (15 fraction bits) and the biquad (14)
        bool simple_on = false, biquad_on = false;
        int32_t b0 = 1 << 15, a1 = 0, y1[2]{};
        int32_t bq_b[3] = {1 << 14, 0, 0}, bq_a[2]{}, bq_x[2][2]{}, bq_y[2][2]{};
        float gain[3][4]{};
        std::optional<std::array<float, 4>> ramp[3]; // fading from, over the next frame
        int16_t coefficients[16]{};
        int16_t history[2]{};
        std::vector<Buffer> queue;
        std::optional<Buffer> playing;
        std::vector<int16_t> decoded; // an ADPCM buffer, decoded when it started
        uint32_t remaining = 0, length = 0;
        double fraction = 0;
        uint16_t current_buffer_id = 0, last_buffer_id = 0;
        bool buffer_update = false;
        uint32_t position = 0;
        Frame frame{};
        bool ran_out_at_frame_end = false;
    };

private:
    std::array<Voice, 24> voices_;
    // the mixer
    float volume_[3]{};
    bool aux_[2]{};
    bool mono_ = false;
    std::array<QuadFrame, 3> kept_{};
};

} // namespace runtime

#endif
