// The audio DSP's voices and mixer. After Zakuro's services/dsp_voices.rs
// and dsp_mixer.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-zakuro), slimmed to a sample at a time, as its references are.

#include "runtime/audio.h"
#include "runtime/memory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace runtime {

namespace {

// where the firmware's structures sit, as the ARM sees DSP data memory
constexpr uint32_t dsp(uint16_t address) { return map::DSP_RAM_VADDR + (uint32_t(address) << 1) + 0x40000; }
const uint32_t FRAME_COUNTER = dsp(0xBFFF), CONFIGURATIONS = dsp(0x9E92), STATUSES = dsp(0x8680),
               COEFFICIENTS = dsp(0xA792), MIXER = dsp(0x9430), FINAL_SAMPLES = dsp(0x8540),
               INTERMEDIATE = dsp(0x9492);
constexpr uint32_t REGION = 0x20000; // region 1 starts this far after region 0
constexpr uint32_t CONFIG_SIZE = 192, STATUS_SIZE = 12, COEFFICIENTS_SIZE = 32;
constexpr uint32_t AUX_MIX_SIZE = 4 * FRAME_SAMPLES * 4;

// a voice's dirty bits: which fields the game changed
enum : uint32_t {
    D_FORMAT = 1 << 0, D_STEREO = 1 << 1, D_COEFFICIENTS = 1 << 2, D_PARTIAL_EMBEDDED = 1 << 3,
    D_PARTIAL_RESET = 1 << 4, D_ENABLE = 1 << 16, D_INTERPOLATION = 1 << 17, D_RATE = 1 << 18,
    D_QUEUE = 1 << 19, D_PLAY_POSITION = 1 << 21, D_FILTERS = 1 << 22, D_SIMPLE = 1 << 23, D_BIQUAD = 1 << 24,
    D_GAIN = 1 << 25, D_SYNC = 1 << 28, D_RESET = 1 << 29, D_EMBEDDED = 1 << 30,
};
// offsets into a voice's configuration
enum : uint32_t {
    C_GAIN = 0x04, C_RATE = 0x34, C_INTERPOLATION = 0x38, C_FILTERS = 0x3A, C_SIMPLE = 0x3C, C_BIQUAD = 0x40,
    C_BUFFERS_DIRTY = 0x4A, C_BUFFERS = 0x4C, C_BUFFER_SIZE = 20, C_ENABLE = 0xA0, C_SYNC = 0xA2,
    C_PLAY_POSITION = 0xA4, C_ADDRESS = 0xAC, C_LENGTH = 0xB0, C_FLAGS1 = 0xB4, C_ADPCM_YN = 0xB8, C_FLAGS2 = 0xBC,
    C_BUFFER_ID = 0xBE,
};

// the DSP keeps 32-bit values high half first
uint32_t read_dsp32(Memory &m, uint32_t a) { return uint32_t(m.read16(a)) << 16 | m.read16(a + 2); }
void write_dsp32(Memory &m, uint32_t a, uint32_t v) {
    m.write16(a, uint16_t(v >> 16));
    m.write16(a + 2, uint16_t(v));
}
float read_float(Memory &m, uint32_t a) {
    float f = std::bit_cast<float>(m.read32(a));
    return std::isfinite(f) ? f : 0.0f;
}

// the region the game wrote last, the frame counters wrapping around
uint32_t current_region(Memory &m) {
    uint16_t first = m.read16(FRAME_COUNTER), second = m.read16(FRAME_COUNTER + REGION);
    if (first == 0xFFFF && second != 0xFFFE) return 1;
    if (second == 0xFFFF && first != 0xFFFE) return 0;
    return first > second ? 0 : 1;
}

int32_t clamp16(int32_t v) { return std::clamp(v, -32768, 32767); }
int32_t clamp16(float v) { return int32_t(std::clamp(v, -32768.0f, 32767.0f)); }

// GC ADPCM: frames of 8 bytes, a header and 14 samples of 4 bits, each
// predicted from the two before
void decode_adpcm(const uint8_t *data, uint32_t bytes, size_t count, const int16_t *c, int16_t history[2],
                  std::vector<int16_t> &out) {
    out.clear();
    out.reserve(count);
    int32_t yn1 = history[0], yn2 = history[1];
    for (uint32_t f = 0; f < bytes && out.size() < count; f += 8) {
        uint8_t header = data[f];
        int32_t scale = 1 << (header & 0xF), c1 = c[(header >> 4 & 7) * 2], c2 = c[(header >> 4 & 7) * 2 + 1];
        for (uint32_t i = 1; i < 8 && f + i < bytes; i++) {
            for (int nibble : {data[f + i] >> 4, data[f + i] & 0xF}) {
                if (out.size() == count) break;
                int32_t xn = (int32_t(uint32_t(nibble) << 28) >> 28) * scale;
                int32_t value = clamp16(((xn << 11) + 0x400 + c1 * yn1 + c2 * yn2) >> 11);
                yn2 = yn1;
                yn1 = value;
                out.push_back(int16_t(value));
            }
        }
    }
    history[0] = int16_t(yn1);
    history[1] = int16_t(yn2);
}

using Voice = Audio::Voice;
using Format = Audio::Format;

// starts the next buffer, the lowest id waiting
bool dequeue(Voice &v, Memory &m) {
    if (v.queue.empty()) return false;
    auto it = std::min_element(v.queue.begin(), v.queue.end(), [](auto &a, auto &b) { return a.id < b.id; });
    Audio::Buffer buffer = *it;
    if (buffer.looping) it->has_played = true;
    else v.queue.erase(it);
    // only the first time through starts at the play position
    uint32_t start = buffer.has_played ? 0 : std::min(buffer.play_position, buffer.length);
    v.length = buffer.length;
    v.remaining = buffer.length - start;
    v.position = start;
    v.fraction = 0;
    v.current_buffer_id = buffer.id;
    v.last_buffer_id = 0;
    v.buffer_update = buffer.from_queue && !buffer.has_played;
    if (buffer.adpcm) {
        v.history[0] = (*buffer.adpcm)[0];
        v.history[1] = (*buffer.adpcm)[1];
    }
    v.decoded.clear();
    if (buffer.format == Format::Adpcm) {
        uint32_t bytes = (buffer.length + 13) / 14 * 8;
        if (const uint8_t *data = m.host(buffer.address & ~3u, bytes)) {
            decode_adpcm(data, bytes, buffer.length, v.coefficients, v.history, v.decoded);
        }
    }
    v.playing = buffer;
    return true;
}

// one input sample of the buffer at hand, both channels
std::array<int16_t, 2> input(Voice &v, Memory &m, uint32_t index) {
    if (!v.playing) return {0, 0};
    const Audio::Buffer &b = *v.playing;
    index = std::min(index, b.length ? b.length - 1 : 0);
    uint32_t channels = b.stereo ? 2 : 1, at = b.address & ~3u;
    switch (b.format) {
    case Format::Adpcm: {
        int16_t s = index < v.decoded.size() ? v.decoded[index] : 0;
        return {s, s};
    }
    case Format::Pcm8: {
        const uint8_t *d = m.host(at + index * channels, channels);
        if (!d) return {0, 0};
        return {int16_t(int8_t(d[0]) * 256), int16_t(int8_t(d[channels - 1]) * 256)};
    }
    default: {
        const uint8_t *d = m.host(at + index * channels * 2, channels * 2);
        if (!d) return {0, 0};
        int16_t l, r;
        std::memcpy(&l, d, 2);
        std::memcpy(&r, d + (channels - 1) * 2, 2);
        return {l, r};
    }
    }
}

void filter(Voice &v, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (int ch = 0; ch < 2; ch++) {
            int32_t value = v.frame[i][ch];
            if (v.simple_on) {
                value = clamp16((v.b0 * value + v.a1 * v.y1[ch]) >> 15);
                v.y1[ch] = value;
            }
            if (v.biquad_on) {
                int32_t x0 = value;
                value = clamp16((v.bq_b[0] * x0 + v.bq_b[1] * v.bq_x[0][ch] + v.bq_b[2] * v.bq_x[1][ch] +
                                 v.bq_a[0] * v.bq_y[0][ch] + v.bq_a[1] * v.bq_y[1][ch]) >> 14);
                v.bq_x[1][ch] = v.bq_x[0][ch];
                v.bq_x[0][ch] = x0;
                v.bq_y[1][ch] = v.bq_y[0][ch];
                v.bq_y[0][ch] = value;
            }
            v.frame[i][ch] = int16_t(value);
        }
    }
}

void play_frame(Voice &v, Memory &m) {
    v.frame = {};
    bool follows_on = v.ran_out_at_frame_end && !v.queue.empty();
    v.ran_out_at_frame_end = false;
    if (v.remaining == 0 && !follows_on) {
        if (dequeue(v, m)) return;
        // out of buffers: the voice switches off and reports the buffer it
        // finished as the last one, a game waits on that
        v.enabled = false;
        v.buffer_update = true;
        v.last_buffer_id = v.current_buffer_id;
        v.current_buffer_id = 0;
        v.position = 0;
        return;
    }
    v.position = v.length - v.remaining;
    size_t output = 0;
    while (output < FRAME_SAMPLES) {
        if (v.remaining == 0 && !dequeue(v, m)) break;
        uint32_t at = v.length - v.remaining;
        auto x0 = input(v, m, at);
        if (!v.interpolate || v.fraction == 0) {
            v.frame[output] = x0;
        } else {
            auto x1 = input(v, m, at + 1);
            for (int ch = 0; ch < 2; ch++) v.frame[output][ch] = int16_t(x0[ch] + (x1[ch] - double(x0[ch])) * v.fraction);
        }
        output++;
        double in = v.fraction + v.rate;
        uint32_t consumed = uint32_t(std::min<double>(std::floor(in), v.remaining));
        v.fraction = std::max(in - consumed, 0.0);
        v.remaining -= consumed;
    }
    v.ran_out_at_frame_end = v.remaining == 0 && output == FRAME_SAMPLES;
    filter(v, output);
}

void mix_into(Voice &v, std::array<Audio::QuadFrame, 3> &mixes) {
    std::optional<std::array<float, 4>> ramps[3] = {v.ramp[0], v.ramp[1], v.ramp[2]};
    for (auto &r : v.ramp) r.reset();
    if (!v.enabled) return;
    for (int mix = 0; mix < 3; mix++) {
        for (int ch = 0; ch < 4; ch++) {
            float to = v.gain[mix][ch], from = ramps[mix] ? (*ramps[mix])[ch] : to;
            if (from == to && to == 0.0f) continue;
            for (size_t i = 0; i < FRAME_SAMPLES; i++) {
                float gain = from == to ? to : from + (to - from) * (float(i) / (FRAME_SAMPLES - 1));
                mixes[mix][ch][i] += int32_t(gain * float(v.frame[i][ch & 1]));
            }
        }
    }
}

// applies what the game changed in a voice, then clears its dirty word
void parse_config(Voice &v, Memory &m, uint32_t base, uint32_t coefficients) {
    uint32_t flags = m.read32(base);
    if (!flags) return;
    if (flags & D_RESET) v = Voice{};
    if (flags & D_PARTIAL_RESET) {
        v.queue.clear();
        v.ran_out_at_frame_end = false;
    }
    if (flags & D_ENABLE) {
        v.enabled = m.read8(base + C_ENABLE) != 0;
        v.ran_out_at_frame_end = false;
    }
    if (flags & D_SYNC) v.sync_count = m.read16(base + C_SYNC);
    if (flags & D_RATE) {
        double rate = read_float(m, base + C_RATE);
        v.rate = rate > 0 ? rate : 1.0;
    }
    if (flags & D_INTERPOLATION) v.interpolate = m.read8(base + C_INTERPOLATION) != 2;
    if (flags & D_COEFFICIENTS) {
        for (uint32_t i = 0; i < 16; i++) v.coefficients[i] = int16_t(m.read16(coefficients + i * 2));
    }
    for (uint32_t mix = 0; mix < 3; mix++) {
        if (!(flags & (D_GAIN << mix))) continue;
        v.ramp[mix] = std::array<float, 4>{v.gain[mix][0], v.gain[mix][1], v.gain[mix][2], v.gain[mix][3]};
        for (uint32_t ch = 0; ch < 4; ch++) v.gain[mix][ch] = read_float(m, base + C_GAIN + (mix * 4 + ch) * 4);
    }
    if (flags & D_FILTERS) {
        // a filter switched off forgets its state and coefficients
        uint16_t on = m.read16(base + C_FILTERS);
        if (!(on & 1) || !v.simple_on) {
            v.b0 = 1 << 15;
            v.a1 = 0;
            v.y1[0] = v.y1[1] = 0;
        }
        if (!(on & 2) || !v.biquad_on) {
            Voice fresh;
            std::memcpy(v.bq_b, fresh.bq_b, sizeof v.bq_b);
            std::memset(v.bq_a, 0, sizeof v.bq_a);
            std::memset(v.bq_x, 0, sizeof v.bq_x);
            std::memset(v.bq_y, 0, sizeof v.bq_y);
        }
        v.simple_on = on & 1;
        v.biquad_on = on & 2;
    }
    auto half = [&](uint32_t offset) { return int32_t(int16_t(m.read16(base + offset))); };
    if (flags & D_SIMPLE) {
        v.simple_on = true;
        v.b0 = half(C_SIMPLE);
        v.a1 = half(C_SIMPLE + 2);
    }
    if (flags & D_BIQUAD) {
        v.biquad_on = true;
        v.bq_a[0] = half(C_BIQUAD + 2);
        v.bq_a[1] = half(C_BIQUAD);
        v.bq_b[0] = half(C_BIQUAD + 8);
        v.bq_b[1] = half(C_BIQUAD + 6);
        v.bq_b[2] = half(C_BIQUAD + 4);
    }
    // the embedded buffer brings its format along
    uint16_t flags1 = m.read16(base + C_FLAGS1);
    if (flags & (D_FORMAT | D_EMBEDDED)) {
        uint32_t f = flags1 >> 2 & 3;
        v.format = f == 0 ? Format::Pcm8 : f == 2 ? Format::Adpcm : Format::Pcm16;
    }
    if (flags & (D_STEREO | D_EMBEDDED)) v.stereo = (flags1 & 3) == 2;
    uint32_t play_position = flags & D_PLAY_POSITION ? read_dsp32(m, base + C_PLAY_POSITION) : 0;
    // the game lengthened the buffer playing
    if (flags & D_PARTIAL_EMBEDDED) {
        uint32_t played = v.length - v.remaining;
        v.length = std::max(read_dsp32(m, base + C_LENGTH), played);
        v.remaining = v.length - played;
        if (v.playing) v.playing->length = v.length;
    }
    if (flags & D_EMBEDDED) {
        uint16_t flags2 = m.read16(base + C_FLAGS2);
        Audio::Buffer b;
        b.address = read_dsp32(m, base + C_ADDRESS);
        b.length = read_dsp32(m, base + C_LENGTH);
        b.format = v.format;
        b.stereo = v.stereo;
        b.looping = flags2 & 2;
        b.id = m.read16(base + C_BUFFER_ID);
        if (flags2 & 1) b.adpcm = std::array<int16_t, 2>{int16_t(m.read16(base + C_ADPCM_YN)), int16_t(m.read16(base + C_ADPCM_YN + 2))};
        b.play_position = play_position;
        v.queue.push_back(b);
    }
    if (flags & D_QUEUE) {
        uint16_t queued = m.read16(base + C_BUFFERS_DIRTY);
        for (uint32_t slot = 0; slot < 4; slot++) {
            if (!(queued & (1 << slot))) continue;
            uint32_t at = base + C_BUFFERS + slot * C_BUFFER_SIZE;
            Audio::Buffer b;
            b.length = read_dsp32(m, at + 4);
            if (!b.length) continue;
            b.address = read_dsp32(m, at);
            b.format = v.format;
            b.stereo = v.stereo;
            b.looping = m.read8(at + 15) != 0;
            b.id = m.read16(at + 16);
            if (m.read8(at + 14)) b.adpcm = std::array<int16_t, 2>{int16_t(m.read16(at + 10)), int16_t(m.read16(at + 12))};
            b.from_queue = true;
            v.queue.push_back(b);
        }
        m.write16(base + C_BUFFERS_DIRTY, 0);
    }
    m.write32(base, 0);
}

void write_status(Voice &v, Memory &m, uint32_t base) {
    m.write8(base, v.enabled);
    m.write8(base + 1, v.buffer_update);
    v.buffer_update = false;
    m.write16(base + 2, v.sync_count);
    write_dsp32(m, base + 4, v.position);
    m.write16(base + 8, v.current_buffer_id);
    m.write16(base + 10, v.last_buffer_id);
}

} // namespace

void Audio::tick(Memory &m) {
    uint32_t read = current_region(m), write = 1 - read;

    // the voices
    std::array<QuadFrame, 3> mixes{};
    for (uint32_t i = 0; i < voices_.size(); i++) {
        Voice &v = voices_[i];
        parse_config(v, m, CONFIGURATIONS + read * REGION + i * CONFIG_SIZE, COEFFICIENTS + read * REGION + i * COEFFICIENTS_SIZE);
        if (v.enabled) play_frame(v, m);
        mix_into(v, mixes);
        write_status(v, m, STATUSES + write * REGION + i * STATUS_SIZE);
    }

    // the mixer's configuration
    uint32_t config = MIXER + read * REGION, flags = m.read32(config);
    if (flags) {
        if (flags & 1 << 16) volume_[0] = read_float(m, config + 0x04);
        for (uint32_t aux = 0; aux < 2; aux++) {
            if (flags & 1 << (24 + aux)) volume_[aux + 1] = read_float(m, config + 0x08 + aux * 4);
            if (flags & 1 << (8 + aux)) aux_[aux] = m.read16(config + 0x28 + aux * 2) != 0;
        }
        if (flags & 1 << 26) mono_ = m.read16(config + 0x16) == 0; // surround comes out as stereo
        m.write32(config, 0);
    }
    // mixes 1 and 2 go out to the game for its effects, what it made of the
    // last ones comes back
    kept_[0] = mixes[0];
    for (uint32_t aux = 0; aux < 2; aux++) {
        uint32_t from = INTERMEDIATE + read * REGION + aux * AUX_MIX_SIZE, to = INTERMEDIATE + write * REGION + aux * AUX_MIX_SIZE;
        if (!aux_[aux]) {
            kept_[aux + 1] = mixes[aux + 1];
            continue;
        }
        m.read_bytes(from, kept_[aux + 1].data(), AUX_MIX_SIZE);
        m.write_bytes(to, mixes[aux + 1].data(), AUX_MIX_SIZE);
    }
    Frame frame{};
    for (uint32_t mix = 0; mix < 3; mix++) {
        float volume = volume_[mix];
        if (volume == 0.0f) continue;
        for (size_t i = 0; i < FRAME_SAMPLES; i++) {
            float a = kept_[mix][0][i] * volume, b = kept_[mix][1][i] * volume, c = kept_[mix][2][i] * volume,
                  d = kept_[mix][3][i] * volume;
            int32_t left, right;
            if (mono_) left = right = clamp16((a + b + c + d) / 2);
            else {
                left = clamp16(a + c);
                right = clamp16(b + d);
            }
            frame[i][0] = int16_t(clamp16(frame[i][0] + left));
            frame[i][1] = int16_t(clamp16(frame[i][1] + right));
        }
    }
    m.write_bytes(FINAL_SAMPLES + write * REGION, frame.data(), FRAME_SAMPLES * 4);

    output.insert(output.end(), &frame[0][0], &frame[0][0] + FRAME_SAMPLES * 2);
    constexpr size_t MAX = AUDIO_RATE * 2;
    if (output.size() > MAX) output.erase(output.begin(), output.begin() + (output.size() - MAX));
}

} // namespace runtime
