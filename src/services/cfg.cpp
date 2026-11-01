// cfg: the console's configuration. Translated from Zakuro's services/cfg.rs.
// Zakuro: Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro.

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace runtime {

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

} // namespace runtime
