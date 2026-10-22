#pragma once

#include "framework.h"
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace Pattern {

// Scans for `pattern` under `mask`.
// A mask byte other than `x` matches any byte
// With `start` at 0, the scan covers the main module.
inline uintptr_t FindPattern(const char* pattern, const char* mask,
                             uintptr_t start = 0, size_t len = 0) {
    if (!start) {
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr),
                             &mi, sizeof(mi));
        start = (uintptr_t)mi.lpBaseOfDll;
        len   = mi.SizeOfImage;
    }

    size_t maskLen = strlen(mask);
    for (size_t i = 0; i <= len - maskLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < maskLen; ++j) {
            if (mask[j] == 'x' &&
                pattern[j] != *(char*)(start + i + j)) {
                found = false;
                break;
            }
        }
        if (found)
            return start + i;
    }
    return 0;
}

} // namespace Pattern
