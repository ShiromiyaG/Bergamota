// dsp::DSP. Translated from Zakuro's services/dsp.rs; the voices and the
// mixer are in audio.cpp.
// Zakuro: Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro.

#include "runtime/services.h"

#include "runtime/aac.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace runtime {

namespace {
constexpr uint32_t PIPE_AUDIO = 2, PIPE_BINARY = 3;
// where the firmware keeps its structures, in DSP data memory
constexpr uint16_t DSP_STRUCTURES[15] = {0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
                                         0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C};
} // namespace

void Services::dsp_signal(System &system) {
    // the firmware reports the registered mask back as satisfied; a driver
    // that reads 0 takes the wakeup for a spurious one
    dsp_.semaphore_value = dsp_.semaphore_mask;
    if (dsp_.semaphore_event) system.kernel.signal_event(*dsp_.semaphore_event);
    for (ObjectId e : dsp_.interrupt_events) system.kernel.signal_event(e);
}

// the binary pipe's messages, 32 bytes either way (as Citra lays them): the
// codec (1, AAC decoding) and the command (0 initialize, 1 decode), the
// result; a decode names where the ADTS frames are and how many bytes, and
// where each channel's PCM goes, all physical; its answer the rate (an index
// of RATES), the channels, the size taken and the samples a channel made
std::vector<uint8_t> Services::dsp_aac(System &system, const std::vector<uint8_t> &request) {
    constexpr size_t MESSAGE = 32;
    constexpr uint32_t FCRAM = 0x20000000, ERROR = 1;
    constexpr uint32_t RATES[9] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    std::vector<uint8_t> message(MESSAGE, 0);
    std::memcpy(message.data(), request.data(), std::min(request.size(), MESSAGE));
    auto half = [&](size_t at) { return uint32_t(message[at] | message[at + 1] << 8); };
    auto word = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, &message[at], 4);
        return v;
    };
    auto put = [&](std::vector<uint8_t> &to, size_t at, uint32_t value) { std::memcpy(&to[at], &value, 4); };
    const uint32_t codec = half(0), command = half(2);
    if (codec != 1) {
        put(message, 4, ERROR);
        return message;
    }
    if (!dsp_.aac) dsp_.aac = std::make_shared<AacDecoder>();
    if (command != 1) {
        // initialize (a new stream), shut down and the rest succeed
        if (command == 0) dsp_.aac->reset();
        put(message, 4, 0);
        return message;
    }
    const uint32_t source = word(8), size = std::min(word(12), 0x100000u), left = word(16), right = word(20);
    std::vector<uint8_t> answer(MESSAGE, 0);
    std::memcpy(answer.data(), message.data(), 4);
    // what Citra answers when decoding fails, which keeps titles going
    put(answer, 8, 0), put(answer, 12, 2), put(answer, 16, size), put(answer, 28, 1024);
    const uint8_t *data = source >= FCRAM ? system.memory.host(source, size) : nullptr;
    std::vector<int16_t> channels[2];
    uint32_t rate = 0;
    if (!data || !dsp_.aac->decode(data, size, channels, rate)) return answer;
    // each channel's samples where it goes, the GPU's copy of that memory
    // left behind
    const uint32_t targets[2] = {left, right};
    for (int c = 0; c < 2 && !channels[c].empty(); c++) {
        const uint32_t bytes = uint32_t(channels[c].size() * 2);
        if (targets[c] < FCRAM) continue;
        uint8_t *to = system.memory.host(targets[c], bytes);
        if (!to) continue;
        gpu_sync(system, system.memory.linear_base + (targets[c] - FCRAM), bytes);
        std::memcpy(to, channels[c].data(), bytes);
    }
    const uint32_t index = uint32_t(std::find(RATES, RATES + 9, rate) - RATES);
    put(answer, 8, index < 9 ? index : 0);
    put(answer, 12, channels[1].empty() ? 1 : 2);
    put(answer, 28, uint32_t(channels[0].size()));
    return answer;
}

void Services::dsp_write_pipe(System &system, uint32_t channel, const std::vector<uint8_t> &data) {
    if (channel == PIPE_BINARY && !data.empty()) {
        // a request for the AAC decoder, answered at once
        dsp_.pipes[PIPE_BINARY] = dsp_aac(system, data);
        dsp_signal(system);
        return;
    }
    if (channel != PIPE_AUDIO || data.empty()) return;
    switch (data[0]) {
    case 0: // initialize: the firmware answers with where its structures are
        for (auto &p : dsp_.pipes) p.clear();
        dsp_.pipes[PIPE_AUDIO].push_back(15);
        dsp_.pipes[PIPE_AUDIO].push_back(0);
        for (uint16_t a : DSP_STRUCTURES) {
            dsp_.pipes[PIPE_AUDIO].push_back(uint8_t(a));
            dsp_.pipes[PIPE_AUDIO].push_back(uint8_t(a >> 8));
        }
        dsp_.running = true;
        break;
    case 1: // shutdown
        for (auto &p : dsp_.pipes) p.clear();
        dsp_.running = false;
        break;
    case 2: dsp_.running = true; break;  // wakeup
    case 3: dsp_.running = false; break; // sleep
    default: break;
    }
    dsp_signal(system);
}

bool Services::dsp(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    uint32_t command = b.command();
    switch (command) {
    case 0x0001: b.reply(command, {dsp_.running ? 0u : 1u}); return true; // RecvData
    case 0x0002: case 0x0004: case 0x000B: b.reply(command, {1}); return true;
    case 0x0003: case 0x0005: case 0x0007: case 0x0009: case 0x000A: case 0x0013: case 0x0014: case 0x0020:
        b.reply(command);
        return true;
    case 0x0006: case 0x001F: case 0x0021: b.reply(command, {0}); return true; // no headphones, not occupied
    case 0x0008: b.reply(command, {dsp_.semaphore_value}); return true;
    case 0x000C: // ConvertProcessAddressFromDspDram
        b.reply(command, {map::DSP_RAM_VADDR + (b.get(1) << 1) + 0x40000});
        return true;
    case 0x000D: { // WriteProcessPipe(channel, size, <buffer>)
        uint32_t channel = b.get(1), size = std::min(b.get(2), 0x1000u);
        std::vector<uint8_t> data(size);
        if (size) system.memory.read_bytes(b.get(4), data.data(), size);
        dsp_write_pipe(system, channel, data);
        b.reply(command);
        return true;
    }
    case 0x000E: case 0x0010: { // ReadPipe, ReadPipeIfPossible(channel, peer, size)
        uint32_t channel = b.get(1) & 7, size = b.get(3) & 0xFFFF;
        auto [destination, capacity] = b.static_buffer(0);
        auto &pipe = dsp_.pipes[channel];
        uint32_t count = std::min<uint32_t>(size, static_cast<uint32_t>(pipe.size()));
        if (destination && count) system.memory.write_bytes(destination, pipe.data(), count);
        pipe.erase(pipe.begin(), pipe.begin() + count);
        b.set(0, header(command, 2, 2));
        b.set(1, result::SUCCESS);
        b.set(2, count);
        b.set(3, descriptor::static_buffer(count, 0));
        b.set(4, destination);
        return true;
    }
    case 0x000F: // GetPipeReadableSize
        b.reply(command, {static_cast<uint32_t>(dsp_.pipes[b.get(1) & 7].size())});
        return true;
    case 0x0011: // LoadComponent: taken, and answered by the firmware's stand-in
        dsp_.loaded = true;
        b.set(0, header(command, 2, 2));
        b.set(1, result::SUCCESS);
        b.set(2, 1);
        b.set(3, 0x100000 << 4 | 0xA);
        b.set(4, 0);
        return true;
    case 0x0012: // UnloadComponent
        dsp_.loaded = dsp_.running = false;
        b.reply(command);
        return true;
    case 0x0015: { // RegisterInterruptEvents(interrupt, channel, event): kept whatever the title does with its handle
        if (auto id = k.resolve(b.get(4))) {
            if (std::find(dsp_.interrupt_events.begin(), dsp_.interrupt_events.end(), *id) == dsp_.interrupt_events.end()) {
                k.create_handle(*id); // a hold of the service's own
                dsp_.interrupt_events.push_back(*id);
            }
        }
        b.reply(command);
        return true;
    }
    case 0x0016: { // GetSemaphoreEventHandle: a handle of the title's own to the one event
        Handle handle;
        if (dsp_.semaphore_event) {
            handle = k.create_handle(*dsp_.semaphore_event);
        } else {
            auto e = k.create_event(ResetType::OneShot);
            k.create_handle(e.first); // the service's own hold
            dsp_.semaphore_event = e.first;
            handle = e.second;
        }
        b.set(0, header(command, 1, 2));
        b.set(1, result::SUCCESS);
        b.set(2, descriptor::handles(1));
        b.set(3, handle);
        return true;
    }
    case 0x0017: // SetSemaphoreMask
        dsp_.semaphore_mask = b.get(1);
        b.reply(command);
        return true;
    default:
        return false;
    }
}

} // namespace runtime
