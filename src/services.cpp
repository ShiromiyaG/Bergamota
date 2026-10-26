// the services the games talk to. Native: what they need to run. Offline:
// what they only probe (online play, local wireless, SpotPass, infrared,
// camera), answered as a 3DS with no network and no camera would.

#include "runtime/runtime.h"

#include <array>
#include <cstring>

namespace {

struct Service {
    const char *name;
    runtime_answer answer;
};

constexpr std::array SERVICES{
    Service{"srv:", RUNTIME_NATIVE},
    Service{"APT:A", RUNTIME_NATIVE},
    Service{"APT:U", RUNTIME_NATIVE},
    Service{"cfg:u", RUNTIME_NATIVE},
    Service{"cfg:s", RUNTIME_NATIVE},
    Service{"cfg:i", RUNTIME_NATIVE},
    Service{"fs:USER", RUNTIME_NATIVE},
    Service{"ldr:ro", RUNTIME_NATIVE},
    Service{"hid:USER", RUNTIME_NATIVE},
    Service{"gsp::Gpu", RUNTIME_NATIVE},
    Service{"dsp::DSP", RUNTIME_NATIVE},
    Service{"y2r:u", RUNTIME_NATIVE},
    Service{"ptm:u", RUNTIME_NATIVE},
    Service{"ac:u", RUNTIME_OFFLINE},
    Service{"ndm:u", RUNTIME_OFFLINE},
    Service{"frd:u", RUNTIME_OFFLINE},
    Service{"nwm::UDS", RUNTIME_OFFLINE},
    Service{"soc:U", RUNTIME_OFFLINE},
    Service{"http:C", RUNTIME_OFFLINE},
    Service{"ssl:C", RUNTIME_OFFLINE},
    Service{"boss:U", RUNTIME_OFFLINE},
    Service{"ir:USER", RUNTIME_OFFLINE},
    Service{"cam:u", RUNTIME_OFFLINE},
};

} // namespace

extern "C" runtime_answer runtime_service_answer(const char *name) {
    for (const auto &s : SERVICES) {
        if (std::strcmp(s.name, name) == 0) {
            return s.answer;
        }
    }
    return RUNTIME_UNKNOWN;
}
