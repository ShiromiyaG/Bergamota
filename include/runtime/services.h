// The services the games talk to, and their command buffers. Translated
// from Zakuro's services/ and kernel/ipc.rs (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-zakuro), cut to what USUM, XY and ORAS use: what
// they need is answered natively, what they only probe (online play, local
// wireless, SpotPass, the camera) as a 3DS with none of it would.

#ifndef RUNTIME_SERVICES_H
#define RUNTIME_SERVICES_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "runtime/system.h"

namespace runtime {

class Fs;

// a command header: command << 16 | normal parameters << 6 | translate ones
constexpr uint32_t header(uint32_t command, uint32_t normal, uint32_t translate) {
    return command << 16 | (normal & 0x3F) << 6 | (translate & 0x3F);
}

namespace descriptor {
// handles the receiver gets copies of, or that move to it
constexpr uint32_t handles(uint32_t count) { return (count - 1) << 26; }
constexpr uint32_t move_handles(uint32_t count) { return (count - 1) << 26 | 0x10; }
constexpr uint32_t static_buffer(uint32_t size, uint32_t id) { return size << 14 | id << 10 | 2; }
} // namespace descriptor

// a thread's command buffer, in its TLS
struct CommandBuffer {
    Memory &memory;
    uint32_t tls;

    uint32_t get(uint32_t index) const { return memory.read32(tls + map::TLS_COMMAND_BUFFER + index * 4); }
    void set(uint32_t index, uint32_t value) { memory.write32(tls + map::TLS_COMMAND_BUFFER + index * 4, value); }
    uint32_t command() const { return get(0) >> 16; }
    // success and these values
    void reply(uint32_t command, std::initializer_list<uint32_t> values = {});
    void reply_error(uint32_t command, uint32_t code);
    // success and a handle moved to the caller
    void reply_with_handle(uint32_t command, Handle handle);
    // the static buffer the receiver published for slot id: address, size
    std::pair<uint32_t, uint32_t> static_buffer(uint32_t id) const;
    // the bytes a request carries in the static buffer whose descriptor is
    // at word index, at most size of them
    std::vector<uint8_t> read_static(uint32_t index, uint32_t size) const;
};

// what the console says about itself through cfg
struct ConsoleSettings {
    uint8_t region = 1;   // 0 Japan, 1 the Americas, 2 Europe
    uint8_t language = 1; // English
    std::u16string username = u"Player";
    uint64_t console_id = 0x0000525533415A55;
};

class Services {
public:
    explicit Services(uint64_t program_id) : program_id_(program_id) {}

    // fs:USER, FSFile and FSDirectory go to it, when there is one
    Fs *fs = nullptr;
    ConsoleSettings console;

    // what the hardware does on its own as time passes: the DSP's audio
    // frames; called often from the loop
    void tick(System &system);

    // answers a request: SendSyncRequest's hook
    void request(System &system, Thread &thread, const std::string &service, uint32_t subhandle);

    // the commands a game made that nothing answers yet, counted
    const std::map<std::pair<std::string, uint32_t>, uint32_t> &unimplemented() const { return unimplemented_; }

private:
    struct Parameter {
        uint32_t sender, destination, signal;
        std::vector<uint8_t> buffer;
        std::optional<ObjectId> object;
    };

    bool srv(System &, CommandBuffer &);
    bool apt(System &, CommandBuffer &);
    bool cfg(System &, CommandBuffer &);
    bool dsp(System &, CommandBuffer &);
    void dsp_write_pipe(System &, uint32_t channel, const std::vector<uint8_t> &data);
    void dsp_signal(System &);
    void send_parameter(System &, Parameter parameter);
    void offline(CommandBuffer &, const std::string &service);
    void unimplemented(CommandBuffer &, const std::string &service);

    uint64_t program_id_;
    // srv
    std::optional<Handle> notification_semaphore_;
    // APT
    std::optional<Handle> apt_lock_, apt_notification_, apt_parameter_event_;
    std::optional<Parameter> parameter_;
    std::optional<uint32_t> library_applet_;
    std::vector<uint8_t> capture_info_;
    uint32_t cpu_time_limit_ = 0;
    // the DSP: no sound yet, but its firmware's answers and its frames
    struct Dsp {
        bool loaded = false, running = false;
        std::optional<ObjectId> semaphore_event;
        std::vector<ObjectId> interrupt_events;
        uint32_t semaphore_mask = 0, semaphore_value = 0;
        std::vector<uint8_t> pipes[8];
        uint64_t next_frame = 0;
    } dsp_;

    std::map<std::pair<std::string, uint32_t>, uint32_t> unimplemented_;
};

} // namespace runtime

#endif
