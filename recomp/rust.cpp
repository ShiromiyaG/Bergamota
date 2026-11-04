// io::Error's message for an error the system gave, the way Rust's standard
// library writes it: on Windows FormatMessageW's text, in the system's
// language, its line end trimmed, elsewhere strerror's, and then the code.
// Written for the translation of 3dsrecomp (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-3dsrecomp).

#include "rust.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstring>
#endif

namespace recomp::rust {

std::string os_error(int code) {
    std::string detail;
#ifdef _WIN32
    // std's sys::os::error_string, an NTSTATUS from ntdll's table
    wchar_t buffer[2048];
    HMODULE module = nullptr;
    DWORD flags = 0, number = DWORD(code);
    if (number & 0x10000000) {
        module = GetModuleHandleW(L"NTDLL.DLL");
        if (module) {
            number ^= 0x10000000;
            flags = FORMAT_MESSAGE_FROM_HMODULE;
        }
    }
    DWORD length = FormatMessageW(flags | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, module, number,
                                  0, buffer, DWORD(sizeof buffer / sizeof *buffer), nullptr);
    if (length == 0) {
        detail = "OS Error " + std::to_string(int(number)) + " (FormatMessageW() returned error " +
                 std::to_string(GetLastError()) + ")";
    } else {
        int size = WideCharToMultiByte(CP_UTF8, 0, buffer, int(length), nullptr, 0, nullptr, nullptr);
        detail.resize(size_t(size));
        WideCharToMultiByte(CP_UTF8, 0, buffer, int(length), detail.data(), size, nullptr, nullptr);
        detail = std::string(trim_end(detail));
    }
#else
    detail = std::strerror(code);
#endif
    return detail + " (os error " + std::to_string(code) + ")";
}

} // namespace recomp::rust
