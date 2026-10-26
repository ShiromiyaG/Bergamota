// The services' dispatch, srv and APT. Translated from Zakuro's
// services/mod.rs, srv.rs and apt.rs (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-zakuro).

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace runtime {

namespace {

// a parameter's signals; Wakeup starts a title
constexpr uint32_t SIGNAL_WAKEUP = 1, SIGNAL_REQUEST = 2, SIGNAL_RESPONSE = 3, SIGNAL_WAKEUP_BY_EXIT = 10;
constexpr uint32_t APPLICATION = 0x300, ANY_LIBRARY_APPLET = 0x400;
constexpr uint32_t NO_PARAMETER = 0xC8A0CFFC;
constexpr uint32_t NOT_CONNECTED = 0xC8809C0C, UDS_WIRELESS_OFF = 0xC9411002;

bool network(const std::string &name) {
    static const char *names[] = {"frd:u", "frd:a", "nwm::UDS", "dlp:SRVR", "http:C", "nim:aoc", "nim:s",
                                  "nim:u", "ac:u", "ac:i", "olv:u", "act:u", "act:a"};
    return std::any_of(std::begin(names), std::end(names), [&](const char *n) { return name == n; });
}

} // namespace

void CommandBuffer::reply(uint32_t command, std::initializer_list<uint32_t> values) {
    set(0, header(command, static_cast<uint32_t>(values.size()) + 1, 0));
    set(1, result::SUCCESS);
    uint32_t i = 2;
    for (uint32_t v : values) set(i++, v);
}

void CommandBuffer::reply_error(uint32_t command, uint32_t code) {
    set(0, header(command, 1, 0));
    set(1, code);
}

void CommandBuffer::reply_with_handle(uint32_t command, Handle handle) {
    set(0, header(command, 1, 2));
    set(1, result::SUCCESS);
    set(2, descriptor::move_handles(1));
    set(3, handle);
}

std::pair<uint32_t, uint32_t> CommandBuffer::static_buffer(uint32_t id) const {
    uint32_t base = tls + 0x180 + id * 8;
    return {memory.read32(base + 4), (memory.read32(base) >> 14) & 0x3FFFF};
}

std::vector<uint8_t> CommandBuffer::read_static(uint32_t index, uint32_t size) const {
    uint32_t desc = get(index), pointer = get(index + 1);
    std::vector<uint8_t> data(std::min(size, (desc >> 14) & 0x3FFFF));
    if (pointer && !data.empty()) memory.read_bytes(pointer, data.data(), static_cast<uint32_t>(data.size()));
    return data;
}

void Services::request(System &system, Thread &thread, const std::string &service, uint32_t subhandle) {
    CommandBuffer buffer{system.memory, thread.tls};
    bool handled = false;
    if (service == "srv:" || service == "srv:pm") handled = srv(system, buffer);
    else if (service == "APT:U" || service == "APT:A" || service == "APT:S") handled = apt(system, buffer);
    else if (service == "cfg:u" || service == "cfg:s" || service == "cfg:i") handled = cfg(system, buffer);
    else if (service == "dsp::DSP") handled = dsp(system, buffer);
    else if (service == "ldr:ro") handled = ldr_ro(system, buffer);
    else if (fs && (service == "fs:USER" || service == "FSFile" || service == "FSDirectory"))
        handled = fs->handle(system, thread, buffer, service, subhandle);
    if (handled) return;
    if (network(service)) offline(buffer, service);
    else unimplemented(buffer, service);
}

void Services::offline(CommandBuffer &buffer, const std::string &service) {
    uint32_t command = buffer.command();
    unimplemented_[{service, command}]++;
    // local wireless answers as with wireless switched off, the one failure
    // every title copes with; the rest "not connected"
    buffer.reply_error(command, service == "nwm::UDS" || service == "dlp:SRVR" ? UDS_WIRELESS_OFF : NOT_CONNECTED);
}

void Services::unimplemented(CommandBuffer &buffer, const std::string &service) {
    uint32_t command = buffer.command();
    if (unimplemented_[{service, command}]++ == 0) {
        std::fprintf(stderr, "runtime: unimplemented %s command 0x%04X\n", service.c_str(), command);
    }
    // success, and zeros where the title reads its outputs
    buffer.reply(command);
    for (uint32_t i = 2; i < 8; i++) buffer.set(i, 0);
}

bool Services::srv(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    switch (b.command()) {
    case 0x0001: // RegisterClient
        b.reply(0x0001);
        return true;
    case 0x0002: // EnableNotification: a semaphore the caller waits on
        if (!notification_semaphore_) notification_semaphore_ = k.create_handle(k.insert(Semaphore{0, 16}));
        b.reply_with_handle(0x0002, *notification_semaphore_);
        return true;
    case 0x0005: { // GetServiceHandle(name[8], length, flags)
        char name[9] = {};
        uint32_t words[2] = {b.get(1), b.get(2)};
        std::memcpy(name, words, 8);
        name[std::min<uint32_t>(b.get(3), 8)] = 0;
        b.reply_with_handle(0x0005, k.create_handle(k.insert(ClientSession{name, 0})));
        return true;
    }
    case 0x0009: case 0x000A: // Subscribe, Unsubscribe
        b.reply(b.command());
        return true;
    case 0x000B: // ReceiveNotification: none
        b.reply(0x000B, {0});
        return true;
    case 0x000E: // IsServiceRegistered: everything a title asks for answers
        b.reply(0x000E, {1});
        return true;
    default:
        return false;
    }
}

void Services::send_parameter(System &system, Parameter parameter) {
    parameter_ = std::move(parameter);
    if (apt_parameter_event_) {
        if (auto id = system.kernel.resolve(*apt_parameter_event_)) system.kernel.signal_event(*id);
    }
}

bool Services::apt(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    uint32_t command = b.command();
    switch (command) {
    case 0x0001: { // GetLockHandle(flags): attributes, power state, the lock
        uint32_t attributes = b.get(1);
        if (!apt_lock_) apt_lock_ = k.create_handle(k.insert(Mutex{}));
        b.set(0, header(0x0001, 3, 2));
        b.set(1, result::SUCCESS);
        b.set(2, attributes);
        b.set(3, 0); // the power button is not pressed
        b.set(4, descriptor::handles(1));
        b.set(5, *apt_lock_);
        return true;
    }
    case 0x0002: { // Initialize(app id, attributes): the notification and parameter events
        auto notification = k.create_event(ResetType::OneShot);
        auto parameter = k.create_event(ResetType::OneShot);
        apt_notification_ = notification.second;
        apt_parameter_event_ = parameter.second;
        // the application starts by being handed a Wakeup parameter
        parameter_ = Parameter{0, b.get(1), SIGNAL_WAKEUP, {}, std::nullopt};
        k.signal_event(parameter.first);
        b.set(0, header(0x0002, 1, 3));
        b.set(1, result::SUCCESS);
        b.set(2, descriptor::handles(2));
        b.set(3, notification.second);
        b.set(4, parameter.second);
        return true;
    }
    case 0x0003: case 0x0004: // Enable, Finalize
        b.reply(command);
        return true;
    case 0x0005: // GetAppletManInfo
        b.reply(0x0005, {0, 0, 0x101, APPLICATION});
        return true;
    case 0x0006: // GetAppletInfo: title id, media type, registered, loaded, attributes
        b.reply(0x0006, {uint32_t(program_id_), uint32_t(program_id_ >> 32), 0, 1, 1, 0});
        return true;
    case 0x0009: { // IsRegistered: a library applet only while one is prepared
        bool registered = b.get(1) != ANY_LIBRARY_APPLET || library_applet_.has_value();
        b.reply(0x0009, {registered});
        return true;
    }
    case 0x000B: // InquireNotification: none
        b.reply(0x000B, {0});
        return true;
    case 0x000C: { // SendParameter(sender, destination, signal, size, handle, buffer)
        uint32_t destination = b.get(2), signal = b.get(3);
        std::vector<uint8_t> data = b.read_static(7, b.get(4));
        if (library_applet_ == destination && signal == SIGNAL_REQUEST) {
            // the applet answers; no applet runs, so no capture block either
            send_parameter(system, Parameter{destination, APPLICATION, SIGNAL_RESPONSE, {}, std::nullopt});
        }
        b.reply(0x000C);
        return true;
    }
    case 0x000D: case 0x000E: { // ReceiveParameter, GlanceParameter
        if (!parameter_) {
            b.reply_error(command, NO_PARAMETER);
            return true;
        }
        Parameter p = *parameter_;
        if (command == 0x000D) parameter_.reset();
        auto [pointer, capacity] = b.static_buffer(0);
        uint32_t size = std::min<uint32_t>(static_cast<uint32_t>(p.buffer.size()), capacity);
        Handle handle = p.object ? k.create_handle(*p.object) : 0;
        b.set(0, header(command, 4, 4));
        b.set(1, result::SUCCESS);
        b.set(2, p.sender);
        b.set(3, p.signal);
        b.set(4, size);
        b.set(5, descriptor::move_handles(1));
        b.set(6, handle);
        b.set(7, descriptor::static_buffer(size, 0));
        b.set(8, pointer);
        if (size && pointer) system.memory.write_bytes(pointer, p.buffer.data(), size);
        return true;
    }
    case 0x000F: // CancelParameter
        parameter_.reset();
        b.reply(0x000F, {1});
        return true;
    case 0x0016: case 0x0018: // PreloadLibraryApplet, PrepareToStartLibraryApplet
        library_applet_ = b.get(1);
        b.reply(command);
        return true;
    case 0x001E: { // StartLibraryApplet: no applet runs, it closes at once
        uint32_t applet = b.get(1);
        std::vector<uint8_t> data = b.read_static(5, b.get(2));
        library_applet_.reset();
        send_parameter(system, Parameter{applet, APPLICATION, SIGNAL_WAKEUP_BY_EXIT,
                                         std::vector<uint8_t>(data.size()), std::nullopt});
        b.reply(command);
        return true;
    }
    case 0x0015: case 0x0017: case 0x0019: case 0x001B: case 0x001F: // the launching machinery
    case 0x003E: case 0x003F: // ReplySleepQuery, ReplySleepNotificationComplete
    case 0x0043: // NotifyToWait
    case 0x0055: // SetScreenCapPostPermission
        b.reply(command);
        return true;
    case 0x0040: // SendCaptureBufferInfo(size, buffer)
        capture_info_ = b.read_static(2, b.get(1));
        b.reply(0x0040);
        return true;
    case 0x0041: { // ReceiveCaptureBufferInfo(size)
        auto [pointer, capacity] = b.static_buffer(0);
        uint32_t size = std::min({static_cast<uint32_t>(capture_info_.size()), b.get(1), capacity});
        if (size && pointer) system.memory.write_bytes(pointer, capture_info_.data(), size);
        b.set(0, header(0x0041, 2, 2));
        b.set(1, result::SUCCESS);
        b.set(2, size);
        b.set(3, descriptor::static_buffer(size, 0));
        b.set(4, pointer);
        return true;
    }
    case 0x0035: case 0x0046: case 0x0047: // ReceiveDeliverArg and the hand-off machinery
        b.reply(command, {0, 0});
        return true;
    case 0x004B: // AppletUtility
        b.reply(0x004B, {0});
        return true;
    case 0x004F: // SetAppCpuTimeLimit
        cpu_time_limit_ = b.get(2);
        b.reply(0x004F);
        return true;
    case 0x0050: // GetAppCpuTimeLimit
        b.reply(0x0050, {cpu_time_limit_});
        return true;
    case 0x0056: // GetScreenCapPostPermission
        b.reply(0x0056, {1});
        return true;
    case 0x0058: // GetProgramId
        b.reply(command, {uint32_t(program_id_), uint32_t(program_id_ >> 32)});
        return true;
    case 0x0101: case 0x0102: // CheckNew3DSApp, CheckNew3DS
        b.reply(command, {0});
        return true;
    case 0x0103: // the platform: a 3DS
        b.reply(command, {1});
        return true;
    default:
        return false;
    }
}

// cfg: the console's configuration. Translated from Zakuro's services/cfg.rs.
bool Services::cfg(System &system, CommandBuffer &b) {
    uint32_t command = b.command();
    switch (command) {
    case 0x0001: { // GetConfigInfoBlk2(size, block id, <out>)
        uint32_t size = b.get(1), block = b.get(2), out = b.get(4);
        std::vector<uint8_t> data(size);
        switch (block) {
        case 0x00090000: // the console's number, which local wireless tells consoles by
            std::memcpy(data.data(), &console.console_id, std::min<size_t>(size, 8));
            break;
        case 0x000A0000: // the user's name: UTF-16, ten characters and the end
            for (size_t i = 0; i < console.username.size() && i < 10 && i * 2 + 1 < size; i++) {
                data[i * 2] = uint8_t(console.username[i]);
                data[i * 2 + 1] = uint8_t(console.username[i] >> 8);
            }
            break;
        case 0x000A0001: // birthday: month, day
        case 0x000D0000: // EULA version
            if (size >= 2) data[0] = data[1] = 1;
            break;
        case 0x000A0002: // the language
            if (size) data[0] = console.language;
            break;
        case 0x000B0000: case 0x000B0001: // the country, in the last byte
            if (size >= 4) data[3] = console.region == 1 ? 49 : 110;
            break;
        case 0x00070001: // sound output: stereo
            if (size) data[0] = 1;
            break;
        case 0x00050005: { // the stereo camera, eight floats (zeros make the 3D camera NaN)
            const float camera[8] = {62.0f, 289.0f, 76.8f, 46.08f, 10.0f, 5.0f, 55.58f, 21.57f};
            std::memcpy(data.data(), camera, std::min<size_t>(size, sizeof camera));
            break;
        }
        default: break; // parental controls off, 3D settings, the rest: zeros
        }
        if (size) system.memory.write_bytes(out, data.data(), size);
        b.reply(command);
        return true;
    }
    case 0x0002: // SecureInfoGetRegion
        b.reply(command, {console.region});
        return true;
    case 0x0003: // GenHashConsoleUnique
        b.reply(command, {uint32_t(console.console_id), uint32_t(console.console_id >> 32)});
        return true;
    case 0x0004: // GetRegionCanadaUSA
        b.reply(command, {console.region == 1});
        return true;
    case 0x0005: // GetSystemModel: an Old 3DS
        b.reply(command, {0});
        return true;
    case 0x0006: // GetModelNintendo2DS: not a 2DS
        b.reply(command, {1});
        return true;
    default:
        return false;
    }
}

// dsp::DSP. Translated from Zakuro's services/dsp.rs; the voices and the
// mixer (the sound itself) come later.
namespace {
constexpr uint32_t PIPE_AUDIO = 2, PIPE_BINARY = 3;
// where the firmware keeps its structures, in DSP data memory
constexpr uint16_t DSP_STRUCTURES[15] = {0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
                                         0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C};
// an audio frame: 160 samples at 32728 Hz
constexpr uint64_t DSP_FRAME_TICKS = CPU_CLOCK_HZ * 160 / 32728;
} // namespace

void Services::dsp_signal(System &system) {
    // the firmware reports the registered mask back as satisfied; a driver
    // that reads 0 takes the wakeup for a spurious one
    dsp_.semaphore_value = dsp_.semaphore_mask;
    if (dsp_.semaphore_event) system.kernel.signal_event(*dsp_.semaphore_event);
    for (ObjectId e : dsp_.interrupt_events) system.kernel.signal_event(e);
}

void Services::dsp_write_pipe(System &system, uint32_t channel, const std::vector<uint8_t> &data) {
    if (channel == PIPE_BINARY && !data.empty()) {
        // the AAC decoder: not there yet, it answers nothing
        dsp_.pipes[PIPE_BINARY].clear();
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

void Services::tick(System &system) {
    // the end of each audio frame signals the firmware's events
    if (!dsp_.running) return;
    uint64_t now = system.ticks();
    if (!dsp_.next_frame) dsp_.next_frame = now;
    if (now < dsp_.next_frame) return;
    dsp_.next_frame += DSP_FRAME_TICKS;
    if (dsp_.next_frame < now) dsp_.next_frame = now + DSP_FRAME_TICKS; // fell far behind
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

// ldr:ro, loading the game's modules. After Zakuro's services/ldr_ro.rs.
bool Services::ldr_ro(System &system, CommandBuffer &b) {
    Memory &m = system.memory;
    uint32_t command = b.command();
    constexpr uint32_t NOT_LOADED = 0xD9012402;
    switch (command) {
    case 0x0001: { // Initialize(CRS buffer, size, address)
        uint32_t source = b.get(1), size = b.get(2), address = b.get(3);
        uint32_t base = address && address != source && m.mirror(address, source, size) ? address : source;
        if (cro.initialize(m, base, size)) b.reply(command);
        else b.reply_error(command, NOT_LOADED);
        return true;
    }
    case 0x0002: case 0x0003: // LoadCRR, UnloadCRR: a signature check nobody needs
    case 0x0006: case 0x0007: case 0x0008: // Link, Unlink, Shutdown: linked on loading already
        b.reply(command);
        return true;
    case 0x0004: case 0x0009: { // LoadCRO(buffer, address, size, data, 0, data size, bss, bss size, auto link, fix level, crr)
        uint32_t source = b.get(1), address = b.get(2), size = b.get(3);
        // the module runs from memory of its own: ldr:ro takes the buffer's
        // pages from the game, which may reuse them while the module runs
        uint32_t base = source;
        if (address && address != source) {
            if (auto block = m.allocate(Region::System, size)) {
                std::vector<uint8_t> bytes(size);
                m.read_bytes(source, bytes.data(), size);
                std::memcpy(m.host(block->paddr, size), bytes.data(), size);
                m.map(address, block->paddr, block->size, READ | WRITE | EXECUTE, MemoryState::Code);
                copies_[address] = Copy{source, *block, size};
                base = address;
            }
        }
        auto fixed = cro.load(m, base, size, b.get(4), b.get(6), b.get(7), b.get(8), (b.get(9) & 0xFF) != 0, b.get(10));
        if (!fixed) {
            b.reply_error(command, NOT_LOADED);
            return true;
        }
        auto copy = copies_.find(base);
        if (copy != copies_.end()) {
            // the game copies the module's relocated data out of its buffer
            // once this returns: on the console the two are the same memory
            std::vector<uint8_t> bytes(size);
            m.read_bytes(base, bytes.data(), size);
            m.write_bytes(copy->second.buffer, bytes.data(), size);
            // what fixing left stays, the rest goes back
            uint32_t kept = (*fixed ? *fixed : size) + map::PAGE_MASK & ~map::PAGE_MASK;
            copy->second.fixed = *fixed ? *fixed : size;
            if (kept < copy->second.block.size) {
                Block tail{copy->second.block.paddr + kept, copy->second.block.size - kept};
                m.unmap(base, copy->second.block.size);
                m.map(base, copy->second.block.paddr, kept, READ | WRITE | EXECUTE, MemoryState::Code);
                copy->second.block.size = kept;
                m.free(tail);
            }
        }
        if (on_module) on_module(cro.modules().back().name, base);
        b.reply(command, {*fixed});
        return true;
    }
    case 0x0005: { // UnloadCRO(address)
        uint32_t address = b.get(1);
        for (const auto &module : cro.modules()) {
            if (module.base == address && on_module) on_module(module.name, 0);
        }
        cro.unload(m, address);
        auto copy = copies_.find(address);
        if (copy != copies_.end()) {
            // the module, as ldr:ro left it, back in the game's buffer
            uint32_t size = std::min(copy->second.fixed, copy->second.block.size);
            std::vector<uint8_t> bytes(size);
            m.read_bytes(address, bytes.data(), size);
            m.write_bytes(copy->second.buffer, bytes.data(), size);
            m.unmap(address, copy->second.block.size);
            m.free(copy->second.block);
            copies_.erase(copy);
        }
        b.reply(command);
        return true;
    }
    default:
        return false;
    }
}

} // namespace runtime
