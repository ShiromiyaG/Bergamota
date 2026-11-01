// hid: the buttons, the circle pad, the touch screen; the motion sensors
// read a console held still and upright. After Zakuro's services/hid.rs,
// leaner.
// Zakuro: Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro.

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace runtime {

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

} // namespace runtime
