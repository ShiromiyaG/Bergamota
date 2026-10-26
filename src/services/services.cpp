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

} // namespace runtime
