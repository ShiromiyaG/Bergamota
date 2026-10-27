// The services' dispatch, srv and APT. Translated from Zakuro's
// services/mod.rs, srv.rs and apt.rs (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-zakuro).

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
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
    static const char *names[] = {"nwm::UDS", "dlp:SRVR", "http:C", "nim:aoc", "nim:s",
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
    auto start = std::chrono::steady_clock::now();
    CommandBuffer buffer{system.memory, thread.tls};
    auto &spent = time_spent[{service, buffer.command()}];
    struct Timer {
        double &into;
        std::chrono::steady_clock::time_point start;
        ~Timer() { into += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    } timer{spent, start};
    usage[{service, buffer.command()}]++;
    bool handled = false;
    if (service == "srv:" || service == "srv:pm") handled = srv(system, buffer);
    else if (service == "APT:U" || service == "APT:A" || service == "APT:S") handled = apt(system, buffer);
    else if (service == "cfg:u" || service == "cfg:s" || service == "cfg:i") handled = cfg(system, buffer);
    else if (service == "dsp::DSP") handled = dsp(system, buffer);
    else if (service == "ldr:ro") handled = ldr_ro(system, buffer);
    else if (service == "gsp::Gpu" && gpu) handled = gsp(system, buffer);
    else if (service == "hid:USER" || service == "hid:SPVR") handled = hid(system, buffer);
    else if (service == "y2r:u") handled = y2r(system, buffer);
    else if (service == "ssl:C" || service == "frd:u" || service == "frd:a" || service == "boss:U" ||
             service == "boss:P" || service == "ndm:u")
        handled = online(system, buffer, service);
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
    // err:f Throw: what the game says went wrong, as Zakuro's err.rs reads it
    if (service == "err:f" && command == 0x0001) {
        uint32_t kind = buffer.get(1) & 0xFF;
        std::fprintf(stderr, "runtime: the game reported a fatal error: type %u, result 0x%08X, pc 0x%08X\n", kind,
                     buffer.get(2), buffer.get(3));
        if (kind == 3) { // a CPU exception, with the registers
            std::fprintf(stderr, "  exception %u, fault 0x%08X\n", buffer.get(5) & 0xFF, buffer.get(6));
            for (uint32_t i = 0; i < 8; i++) std::fprintf(stderr, "  r%u = 0x%08X\n", i, buffer.get(7 + i));
        }
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
// mixer are in audio.cpp.
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
    uint64_t now = system.ticks();
    // the screens refresh 60 times a second (the 3DS's 59.83)
    if (gpu && gsp_.shared) {
        constexpr uint64_t FRAME = CPU_CLOCK_HZ * 100 / 5983;
        if (!gsp_.next_vblank) gsp_.next_vblank = now + FRAME;
        if (now >= gsp_.next_vblank) {
            gsp_.next_vblank = std::max(gsp_.next_vblank + FRAME, now);
            gsp_refresh(system);
            vblanks++;
            hid_update(system);
            gsp_interrupt(system, 2); // top screen
            gsp_interrupt(system, 3); // bottom screen
        }
    }
    // the end of each audio frame signals the firmware's events
    if (!dsp_.running) return;
    if (!dsp_.next_frame) dsp_.next_frame = now;
    if (now < dsp_.next_frame) return;
    dsp_.next_frame += DSP_FRAME_TICKS;
    if (dsp_.next_frame < now) dsp_.next_frame = now + DSP_FRAME_TICKS; // fell far behind
    audio.tick(system.memory);
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

// gsp::Gpu. After Zakuro's services/gsp.rs.
namespace {
constexpr uint32_t GSP_SHARED_SIZE = 0x1000;
enum : uint8_t { PSC0, PSC1, PDC0, PDC1, PPF, P3D, DMA };
} // namespace

// where the game mapped the shared block, 0 before it did
uint32_t Services::gsp_shared(System &system) const {
    if (!gsp_.shared) return 0;
    auto *block = std::get_if<SharedMemory>(system.kernel.object(*gsp_.shared));
    return block && block->mapped_at ? *block->mapped_at : 0;
}

// an address the game hands gsp, in VRAM or its linear heap, as the
// physical one the GPU takes
uint32_t Services::gsp_physical(System &system, uint32_t vaddr) const {
    uint32_t linear = system.memory.linear_base;
    if (vaddr >= map::VRAM_VADDR && vaddr < map::VRAM_VADDR + map::VRAM_SIZE) return map::VRAM_PADDR + (vaddr - map::VRAM_VADDR);
    if (vaddr >= linear && vaddr - linear < 0x10000000) return map::FCRAM_PADDR + (vaddr - linear);
    return vaddr;
}

// a range of the game's memory made right for something other than the GPU
void Services::gpu_sync(System &system, uint32_t vaddr, uint32_t size) {
    if (gpu && size) gpu->sync(gsp_physical(system, vaddr), size);
}

// an interrupt into the client's queue (in the shared block), and its event
void Services::gsp_interrupt(System &system, uint8_t id) {
    uint32_t queue = gsp_shared(system);
    if (!queue) return;
    Memory &m = system.memory;
    uint32_t index = m.read8(queue), count = m.read8(queue + 1);
    if (count >= 0x34) {
        m.write8(queue + 2, 1); // full: the error flag, and it is dropped
        return;
    }
    m.write8(queue + 0xC + (index + count) % 0x34, id);
    m.write8(queue + 1, uint8_t(count + 1));
    if (gsp_.event) system.kernel.signal_event(*gsp_.event);
}

// the screens take the pictures queued for them in the shared block; a
// ring held until now runs
void Services::gsp_refresh(System &system) {
    uint32_t base = gsp_shared(system);
    if (!base) return;
    Memory &m = system.memory;
    for (uint32_t screen = 0; screen < 2; screen++) {
        uint32_t record = base + 0x200 + screen * 0x40, head = m.read32(record);
        if (!((head >> 8) & 0xFF)) continue;
        uint32_t index = head & 0xFF, entry = record + 4 + index * 0x1C;
        gpu->framebuffer(screen, index, gsp_physical(system, m.read32(entry + 4)), gsp_physical(system, m.read32(entry + 8)),
                         m.read32(entry + 0xC), m.read32(entry + 0x10));
        m.write32(record, index); // taken: the flag clears
        if (screen == 0) pictures++;
    }
    gsp_.waiting[0].reset();
    gsp_.waiting[1].reset();
    if (gsp_.held) {
        gsp_.held = false;
        gsp_queue(system);
    }
}

// the GX commands the game queued in the shared block's ring
void Services::gsp_queue(System &system) {
    uint32_t base = gsp_shared(system);
    if (!base) return;
    Memory &m = system.memory;
    uint32_t ring = base + 0x800;
    for (;;) {
        uint32_t control = m.read32(ring), index = control & 0xFF, count = (control >> 8) & 0xFF;
        if (!count) break;
        uint32_t entry = ring + 0x20 + (index % 15) * 0x20, id = m.read32(entry) & 0xFF, d[7];
        for (uint32_t i = 0; i < 7; i++) d[i] = m.read32(entry + 4 + i * 4);
        m.write32(ring, (index + 1) % 15 | (count - 1) << 8); // taken before it runs
        switch (id) {
        case 0x00: { // RequestDma(source, destination, size)
            gpu_sync(system, d[0], d[2]);
            gpu_sync(system, d[1], d[2]);
            std::vector<uint8_t> data(d[2]);
            m.read_bytes(d[0], data.data(), d[2]);
            m.write_bytes(d[1], data.data(), d[2]);
            gsp_interrupt(system, DMA);
            break;
        }
        case 0x01: // ProcessCommandList(address, size)
            gpu->command_list(gsp_physical(system, d[0] & ~7u), d[1] & ~3u);
            gsp_interrupt(system, P3D);
            break;
        case 0x02: // MemoryFill: two banks
            for (uint32_t bank = 0; bank < 2; bank++) {
                uint32_t start = d[bank * 3], value = d[bank * 3 + 1], end = d[bank * 3 + 2];
                if (!start || end <= start) continue;
                uint32_t control2 = d[6] >> (bank * 16), width = (control2 & 0x300) == 0 ? 2 : (control2 & 0x300) == 0x100 ? 3 : 4;
                uint32_t p = gsp_physical(system, start);
                gpu->memory_fill(p, p + (end - start), value, width);
                gsp_interrupt(system, bank ? PSC1 : PSC0);
            }
            break;
        case 0x03: // DisplayTransfer
            gpu->display_transfer(gsp_physical(system, d[0]), gsp_physical(system, d[1]), d[2], d[3], d[4]);
            gsp_interrupt(system, PPF);
            break;
        case 0x04: // TextureCopy
            gpu->texture_copy(gsp_physical(system, d[0]), gsp_physical(system, d[1]), d[2], d[3], d[4]);
            gsp_interrupt(system, PPF);
            break;
        default: break; // FlushCacheRegions and the rest
        }
    }
}

bool Services::gsp(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    Memory &m = system.memory;
    uint32_t command = b.command();
    switch (command) {
    case 0x0001: case 0x0002: { // WriteHWRegs(offset, size, <buffer>), WithMask(…, <mask>)
        uint32_t offset = b.get(1), size = std::min(b.get(2), 0x80u), source = b.get(4), mask = b.get(6);
        for (uint32_t i = 0; offset < 0x420000 && i < size / 4; i++) {
            uint32_t value = m.read32(source + i * 4), reg = offset + i * 4;
            if (command == 0x0002) {
                uint32_t mk = m.read32(mask + i * 4);
                value = (gpu->read_register(reg) & ~mk) | (value & mk);
            }
            gpu->write_register(reg, value);
        }
        b.reply(command);
        return true;
    }
    case 0x0003: { // WriteHWRegRepeat: one register, many times
        for (uint32_t i = 0; i < b.get(2) / 4; i++) gpu->write_register(b.get(1), m.read32(b.get(4) + i * 4));
        b.reply(command);
        return true;
    }
    case 0x0004: { // ReadHWRegs(offset, size)
        for (uint32_t i = 0; i < b.get(2) / 4; i++) m.write32(b.get(4) + i * 4, gpu->read_register(b.get(1) + i * 4));
        b.reply(command);
        return true;
    }
    case 0x0005: // SetBufferSwap(screen, active, left, right, stride, format)
        gpu->framebuffer(b.get(1), b.get(2), gsp_physical(system, b.get(3)), gsp_physical(system, b.get(4)), b.get(5), b.get(6));
        b.reply(command);
        return true;
    case 0x0006: // SetCommandList
        gpu->command_list(gsp_physical(system, b.get(1) & ~7u), b.get(2) & ~3u);
        gsp_interrupt(system, P3D);
        b.reply(command);
        return true;
    case 0x0007: { // RequestDma
        gpu_sync(system, b.get(1), b.get(3));
        gpu_sync(system, b.get(2), b.get(3));
        std::vector<uint8_t> data(b.get(3));
        m.read_bytes(b.get(1), data.data(), b.get(3));
        m.write_bytes(b.get(2), data.data(), b.get(3));
        gsp_interrupt(system, DMA);
        b.reply(command);
        return true;
    }
    case 0x000C: { // TriggerCmdReqQueue: unless the game drew past what the screens can show
        uint32_t base = gsp_shared(system);
        if (base && !gsp_.held) {
            for (uint32_t screen = 0; screen < 2; screen++) {
                uint32_t record = base + 0x200 + screen * 0x40, head = m.read32(record);
                if (!((head >> 8) & 0xFF)) continue;
                std::pair<uint32_t, uint32_t> now{head & 0xFF, m.read32(record + 4 + (head & 0xFF) * 0x1C + 4)};
                if (!gsp_.waiting[screen]) gsp_.waiting[screen] = now;
                else if (*gsp_.waiting[screen] != now) gsp_.held = true;
            }
        }
        if (!gsp_.held) gsp_queue(system);
        b.reply(command);
        return true;
    }
    case 0x000D: // SetDisplayTransfer
        gpu->display_transfer(gsp_physical(system, b.get(1)), gsp_physical(system, b.get(2)), b.get(3), b.get(4), b.get(5));
        gsp_interrupt(system, PPF);
        b.reply(command);
        return true;
    case 0x000E: // SetTextureCopy
        gpu->texture_copy(gsp_physical(system, b.get(1)), gsp_physical(system, b.get(2)), b.get(3), b.get(4), b.get(5));
        gsp_interrupt(system, PPF);
        b.reply(command);
        return true;
    case 0x000F: // SetMemoryFill(start0, value0, end0, start1, value1, end1, control0, control1)
        for (uint32_t bank = 0; bank < 2; bank++) {
            uint32_t start = b.get(1 + bank * 3), value = b.get(2 + bank * 3), end = b.get(3 + bank * 3), control = b.get(7 + bank);
            if (!start || end <= start) continue;
            uint32_t width = (control & 0x300) == 0 ? 2 : (control & 0x300) == 0x100 ? 3 : 4, p = gsp_physical(system, start);
            gpu->memory_fill(p, p + (end - start), value, width);
            gsp_interrupt(system, bank ? PSC1 : PSC0);
        }
        b.reply(command);
        return true;
    case 0x0013: { // RegisterInterruptRelayQueue(flags, event): the client's slot and the shared block
        if (gsp_.event) k.release_hold(*gsp_.event);
        gsp_.event = k.resolve(b.get(3));
        if (gsp_.event) k.hold(*gsp_.event);
        if (!gsp_.shared) {
            auto block = m.allocate(Region::Base, GSP_SHARED_SIZE);
            std::memset(m.host(block->paddr, GSP_SHARED_SIZE), 0, GSP_SHARED_SIZE);
            gsp_.shared = k.insert(SharedMemory{"GSP", 0, GSP_SHARED_SIZE, block->paddr, std::nullopt});
            k.hold(*gsp_.shared);
        }
        // 0x2A07 with the success: the GPU is set up already
        b.set(0, header(command, 2, 2));
        b.set(1, 0x2A07);
        b.set(2, 0); // the first client's slot
        b.set(3, descriptor::handles(1));
        b.set(4, k.create_handle(*gsp_.shared));
        return true;
    }
    case 0x0014: // UnregisterInterruptRelayQueue
        if (gsp_.event) k.release_hold(*gsp_.event);
        gsp_.event.reset();
        b.reply(command);
        return true;
    case 0x0012: // GetPerfLog
        b.reply(command, {0});
        return true;
    case 0x0008: case 0x0009: case 0x001F: // the caches: memory is coherent here
    case 0x000A: case 0x000B: // RegisterInterruptEvents, SetLcdForceBlack
    case 0x0010: case 0x0011: case 0x0015: case 0x0016: case 0x0017: // QoS, perf log, rights
    case 0x0019: case 0x001A: case 0x001B: case 0x001C: case 0x001E: // VRAM areas, reset, LED, priorities
        b.reply(command);
        return true;
    default:
        return false;
    }
}

// hid: the buttons, the circle pad, the touch screen; the motion sensors
// read a console held still and upright. After Zakuro's services/hid.rs,
// leaner.
namespace {
constexpr uint32_t HID_SHARED_SIZE = 0x2B0;
constexpr uint32_t PAD_BASE = 0x00, TOUCH_BASE = 0xA8, ACCELEROMETER_BASE = 0x108, GYROSCOPE_BASE = 0x158;
} // namespace

bool Services::hid(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    uint32_t command = b.command();
    switch (command) {
    case 0x000A: { // GetIPCHandles: the shared block and five events
        if (!hid_.shared) {
            auto block = system.memory.allocate(Region::Base, HID_SHARED_SIZE);
            std::memset(system.memory.host(block->paddr, 0x1000), 0, 0x1000);
            hid_.shared = k.insert(SharedMemory{"HID", 0, HID_SHARED_SIZE, block->paddr, std::nullopt});
            k.hold(*hid_.shared);
            for (int i = 0; i < 5; i++) { // pad or touch twice, accelerometer, gyroscope, debug pad
                auto e = k.create_event(ResetType::OneShot);
                hid_.events.push_back(e.first);
            }
        }
        b.set(0, header(command, 1, 7));
        b.set(1, result::SUCCESS);
        b.set(2, descriptor::handles(6));
        b.set(3, k.create_handle(*hid_.shared));
        for (uint32_t i = 0; i < 5; i++) b.set(4 + i, k.create_handle(hid_.events[i]));
        return true;
    }
    case 0x0011: hid_.accelerometer_users++; b.reply(command); return true;
    case 0x0012: hid_.accelerometer_users -= hid_.accelerometer_users > 0; b.reply(command); return true;
    case 0x0013: hid_.gyroscope_users++; b.reply(command); return true;
    case 0x0014: hid_.gyroscope_users -= hid_.gyroscope_users > 0; b.reply(command); return true;
    case 0x0015: { // GetGyroscopeLowRawToDpsCoefficient
        float c = 14.375f;
        uint32_t bits;
        std::memcpy(&bits, &c, 4);
        b.reply(command, {bits});
        return true;
    }
    case 0x0016: b.reply(command, {0, 0, 0, 0, 0}); return true; // GetGyroscopeLowCalibrateParam
    case 0x0017: b.reply(command, {0x3F}); return true;          // GetSoundVolume
    default: return false;
    }
}

// a frame of input into the shared block's rings, as the HID module writes it
void Services::hid_update(System &system) {
    if (!hid_.shared) return;
    auto *block = std::get_if<SharedMemory>(system.kernel.object(*hid_.shared));
    uint8_t *base = block ? system.memory.host(block->paddr, HID_SHARED_SIZE) : nullptr;
    if (!base) return;
    auto put32 = [&](uint32_t at, uint32_t v) { std::memcpy(base + at, &v, 4); };
    auto put16 = [&](uint32_t at, uint16_t v) { std::memcpy(base + at, &v, 2); };
    uint64_t tick = system.ticks() & 0x7FFFFFFFFFFFFFFFull;
    // a section's header: the tick its ring last started over at and the one before, and its index
    auto section = [&](uint32_t at, int which, uint32_t index) {
        if (index == 0) {
            hid_.reset[which][1] = hid_.reset[which][0];
            hid_.reset[which][0] = tick;
        }
        put32(at, uint32_t(hid_.reset[which][0]));
        put32(at + 4, uint32_t(hid_.reset[which][0] >> 32));
        put32(at + 8, uint32_t(hid_.reset[which][1]));
        put32(at + 12, uint32_t(hid_.reset[which][1] >> 32));
        put32(at + 0x10, index);
    };

    // the circle pad pushed far enough counts as a direction
    uint32_t buttons = input.buttons & 0x00000FFF;
    if (input.circle_x > 0.5f) buttons |= 1u << 28;
    if (input.circle_x < -0.5f) buttons |= 1u << 29;
    if (input.circle_y > 0.5f) buttons |= 1u << 30;
    if (input.circle_y < -0.5f) buttons |= 1u << 31;
    uint32_t pressed = buttons & ~hid_.previous, released = ~buttons & hid_.previous;
    hid_.previous = buttons;

    hid_.pad = (hid_.pad + 1) % 8;
    section(PAD_BASE, 0, hid_.pad);
    put32(PAD_BASE + 0x1C, buttons);
    uint32_t entry = PAD_BASE + 0x28 + hid_.pad * 0x10;
    put32(entry, buttons);
    put32(entry + 4, pressed);
    put32(entry + 8, released);
    put16(entry + 12, uint16_t(int16_t(std::clamp(input.circle_x, -1.0f, 1.0f) * 150)));
    put16(entry + 14, uint16_t(int16_t(std::clamp(input.circle_y, -1.0f, 1.0f) * 150)));

    hid_.touch = (hid_.touch + 1) % 8;
    section(TOUCH_BASE, 1, hid_.touch);
    entry = TOUCH_BASE + 0x20 + hid_.touch * 8;
    put16(entry, input.touch ? input.touch->first : 0);
    put16(entry + 2, input.touch ? input.touch->second : 0);
    put32(entry + 4, input.touch.has_value());

    for (size_t i = 0; i < 2 && i < hid_.events.size(); i++) system.kernel.signal_event(hid_.events[i]);

    // a console held still and upright: one g down, no turning
    if (hid_.accelerometer_users) {
        hid_.accelerometer = (hid_.accelerometer + 1) % 8;
        section(ACCELEROMETER_BASE, 2, hid_.accelerometer);
        for (uint32_t at : {ACCELEROMETER_BASE + 0x18, ACCELEROMETER_BASE + 0x20 + hid_.accelerometer * 6}) {
            put16(at, 0);
            put16(at + 2, uint16_t(int16_t(-512)));
            put16(at + 4, 0);
        }
        system.kernel.signal_event(hid_.events[2]);
    }
    if (hid_.gyroscope_users) {
        hid_.gyroscope = (hid_.gyroscope + 1) % 32;
        section(GYROSCOPE_BASE, 3, hid_.gyroscope);
        for (uint32_t at : {GYROSCOPE_BASE + 0x18, GYROSCOPE_BASE + 0x20 + hid_.gyroscope * 6}) {
            put16(at, 0);
            put16(at + 2, 0);
            put16(at + 4, 0);
        }
        system.kernel.signal_event(hid_.events[3]);
    }
}

// y2r: the movies' YUV to RGB, with the hardware's arithmetic (as Citra
// worked it out). After Zakuro's services/y2r.rs, leaner: a pixel at a
// time, no rotation (USUM's movies need none).
namespace {
// the standard conversions: BT.601 and 709, full range and scaled
constexpr int32_t Y2R_STANDARD[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B},
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B},
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},
};

uint32_t morton(uint32_t x, uint32_t y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

// samples from a buffer read in transfers, a gap skipped after each; a
// 16-bit sample keeps its low byte
std::vector<uint8_t> gather(Memory &m, uint32_t address, uint32_t transfer, uint32_t gap, size_t count, uint32_t width) {
    std::vector<uint8_t> out;
    out.reserve(count);
    transfer = std::max(transfer, width);
    std::vector<uint8_t> piece(transfer);
    while (out.size() < count) {
        m.read_bytes(address, piece.data(), transfer);
        for (uint32_t i = 0; i < transfer && out.size() < count; i += width) out.push_back(piece[i]);
        address += transfer + gap;
    }
    return out;
}

int32_t channel(int32_t sum) { return std::clamp(int32_t(int16_t(std::clamp(sum >> 8, -32768, 32767))), 0, 255); }
} // namespace

void Services::y2r_convert(System &system) {
    Y2r &s = y2r_;
    uint32_t width = s.width, lines = s.lines;
    if (!width || !lines || width % 8) return;
    Memory &m = system.memory;
    size_t pixels = size_t(width) * lines;
    uint32_t sample = s.input == 2 || s.input == 3 ? 2 : 1;
    std::vector<uint8_t> Y, U, V;
    uint32_t chroma_lines = 1; // lines sharing a chroma line: 4:2:2 one, 4:2:0 two
    if (s.input <= 3) {
        bool half_lines = s.input == 1 || s.input == 3;
        chroma_lines = half_lines ? 2 : 1;
        Y = gather(m, s.y.address, s.y.transfer, s.y.gap, pixels, sample);
        U = gather(m, s.u.address, s.u.transfer, s.u.gap, pixels / (2 * chroma_lines), sample);
        V = gather(m, s.v.address, s.v.transfer, s.v.gap, pixels / (2 * chroma_lines), sample);
    } else { // YUYV, interleaved
        std::vector<uint8_t> all = gather(m, s.yuyv.address, s.yuyv.transfer, s.yuyv.gap, pixels * 2, 1);
        for (size_t i = 0; i + 3 < all.size(); i += 4) {
            Y.push_back(all[i]);
            U.push_back(all[i + 1]);
            Y.push_back(all[i + 2]);
            V.push_back(all[i + 3]);
        }
    }
    const int32_t *c = s.coefficients;
    int32_t red = (c[5] + 0x18) * 8, green = (c[6] + 0x18) * 8, blue = (c[7] + 0x18) * 8;
    uint32_t bytes = s.output == 0 ? 4 : s.output == 1 ? 3 : 2;
    std::vector<uint8_t> out(pixels * bytes);
    for (uint32_t y = 0; y < lines; y++) {
        uint32_t chroma_row = s.input <= 3 ? (y / chroma_lines) * (width / 2) : y * (width / 2);
        for (uint32_t x = 0; x < width; x++) {
            int32_t luma = int16_t(c[0]) * int32_t(Y[size_t(y) * width + x]);
            int32_t u = U[chroma_row + x / 2], v = V[chroma_row + x / 2];
            uint32_t r = channel(luma + int16_t(c[1]) * v + red);
            uint32_t g = channel(luma + green - int16_t(c[2]) * v - int16_t(c[3]) * u);
            uint32_t b = channel(luma + int16_t(c[4]) * u + blue);
            uint32_t value;
            switch (s.output) {
            case 0: value = (s.alpha & 0xFF) | b << 8 | g << 16 | r << 24; break; // alpha, then blue, green, red
            case 1: value = b | g << 8 | r << 16; break;
            case 2: value = (s.alpha >= 0x80) | (b >> 3) << 1 | (g >> 3) << 6 | (r >> 3) << 11; break;
            default: value = (b >> 3) | (g >> 2) << 5 | (r >> 3) << 11; break;
            }
            // in rows, or in 8x8 tiles as textures are
            size_t at = s.tiled ? (size_t(y / 8) * width * 8 + (x / 8) * 64 + morton(x % 8, y % 8)) : size_t(y) * width + x;
            std::memcpy(&out[at * bytes], &value, bytes);
        }
    }
    uint32_t transfer = std::max(s.out.transfer, 1u), address = s.out.address;
    // the movie's frames go where textures were read from: the GPU drops them
    gpu_sync(system, address, uint32_t((out.size() + transfer - 1) / transfer * (transfer + s.out.gap)));
    for (size_t i = 0; i < out.size(); i += transfer) {
        uint32_t n = uint32_t(std::min<size_t>(transfer, out.size() - i));
        m.write_bytes(address, &out[i], n);
        address += transfer + s.out.gap;
    }
}

bool Services::y2r(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    Y2r &s = y2r_;
    uint32_t command = b.command(), w = b.get(1);
    if (!s.event) {
        s.event = k.create_event(ResetType::OneShot).first;
        k.hold(*s.event);
    }
    auto coefficients = [&](const int32_t *c) {
        b.reply(command, {uint32_t(uint16_t(c[0])) | uint32_t(c[1]) << 16, uint32_t(uint16_t(c[2])) | uint32_t(c[3]) << 16,
                          uint32_t(uint16_t(c[4])) | uint32_t(c[5]) << 16, uint32_t(uint16_t(c[6])) | uint32_t(c[7]) << 16});
    };
    switch (command) {
    case 0x0001: s.input = w & 0xFF; break;
    case 0x0002: b.reply(command, {s.input}); return true;
    case 0x0003: s.output = w & 0xFF; break;
    case 0x0004: b.reply(command, {s.output}); return true;
    case 0x0005: s.rotation = w & 0xFF; break;
    case 0x0006: b.reply(command, {s.rotation}); return true;
    case 0x0007: s.tiled = (w & 0xFF) != 0; break;
    case 0x0008: b.reply(command, {s.tiled}); return true;
    case 0x000F: // GetTransferEndEvent
        b.set(0, header(command, 1, 2));
        b.set(1, result::SUCCESS);
        b.set(2, descriptor::handles(1));
        b.set(3, k.create_handle(*s.event));
        return true;
    case 0x0010: case 0x0011: case 0x0012: case 0x0013: case 0x0018: { // SetSending Y/U/V/YUYV, SetReceiving(address, size, transfer, gap)
        Y2rBuffer &buf = command == 0x0010 ? s.y : command == 0x0011 ? s.u : command == 0x0012 ? s.v : command == 0x0013 ? s.yuyv : s.out;
        buf = Y2rBuffer{w, b.get(3) & 0xFFFF, b.get(4) & 0xFFFF};
        break;
    }
    case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x0019: b.reply(command, {1}); return true; // done at once
    case 0x001A: s.width = w & 0xFFFF; break;
    case 0x001B: b.reply(command, {s.width}); return true;
    case 0x001C: s.lines = w & 0xFFFF; break;
    case 0x001D: b.reply(command, {s.lines}); return true;
    case 0x001E: // SetCoefficient: eight halfwords
        for (int i = 0; i < 8; i++) s.coefficients[i] = int16_t(b.get(1 + i / 2) >> (16 * (i % 2)));
        break;
    case 0x001F: coefficients(s.coefficients); return true;
    case 0x0020: s.standard = w & 3; std::memcpy(s.coefficients, Y2R_STANDARD[s.standard], sizeof s.coefficients); break;
    case 0x0021: coefficients(Y2R_STANDARD[w & 3]); return true;
    case 0x0022: s.alpha = w & 0xFFFF; break;
    case 0x0023: b.reply(command, {s.alpha}); return true;
    case 0x0025: b.reply(command, {0, 0, 0, 0, 0, 0, 0, 0}); return true;
    case 0x0026: // StartConversion: done at once
        y2r_convert(system);
        k.signal_event(*s.event);
        break;
    case 0x0028: case 0x002A: b.reply(command, {0}); return true; // not busy; PingProcess
    case 0x0029: { // SetPackageParameter(formats…, size, coefficient and alpha)
        uint32_t second = b.get(2), third = b.get(3);
        s.input = w & 0xFF, s.output = (w >> 8) & 0xFF, s.rotation = (w >> 16) & 0xFF, s.tiled = (w >> 24) & 0xFF;
        s.width = second & 0xFFFF, s.lines = second >> 16;
        s.standard = third & 3;
        std::memcpy(s.coefficients, Y2R_STANDARD[s.standard], sizeof s.coefficients);
        s.alpha = third >> 16;
        break;
    }
    case 0x002B: case 0x002C: { // DriverInitialize, DriverFinalize
        auto event = s.event;
        s = Y2r{};
        s.event = event;
        break;
    }
    case 0x002D:
        b.reply(command, {s.input | s.output << 8 | s.rotation << 16 | uint32_t(s.tiled) << 24, s.width | s.lines << 16,
                          s.standard | s.alpha << 16});
        return true;
    default:
        if (command < 0x0001 || command > 0x002D) return false;
        if (command % 2 == 0 && s.other.count(command - 1)) {
            b.reply(command, {s.other[command - 1]}); // a getter of what only reads back
            return true;
        }
        s.other[command] = w; // dithering and the rest
        break;
    }
    b.reply(command);
    return true;
}

// what works offline of the online services, as a console with no network
// answers: SSL sets itself up (USUM stops saving when it does not), the
// friend list is there and empty, SpotPass has nothing
bool Services::online(System &, CommandBuffer &b, const std::string &service) {
    uint32_t command = b.command();
    if (service == "frd:u" || service == "frd:a") {
        if (command == 0x0003 || command == 0x0023) { // Login, and the one Zakuro answers likewise
            b.reply_error(command, NOT_CONNECTED);
            return true;
        }
    }
    // success, and zeros where the game reads its outputs
    b.reply(command);
    for (uint32_t i = 2; i < 8; i++) b.set(i, 0);
    return true;
}

} // namespace runtime
