#pragma once

#include "framework.h"

namespace Hook {

// Returns the length of one common prologue opcode.
// Returns 0 when the opcode is not recognised.
inline int InsnLength(const uint8_t* p) {
    uint8_t op = p[0];

    // push or pop of a register, nop, ret, or int3.
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3 || op == 0xCC)
        return 1;
    // push imm8.
    if (op == 0x6A) return 2;
    // push imm32, call rel32, jmp rel32, or mov eax from a disp32 address.
    if (op == 0x68 || op == 0xE8 || op == 0xE9 || op == 0xA1) return 5;

    // ModRM forms, including the x87 opcodes 0xD8 through 0xDF.
    // Those x87 ops use the same ModRM sizes as the integer forms below.
    if (op == 0x8B || op == 0x89 || op == 0x3B || op == 0x85 || op == 0x87 ||
        op == 0x01 || op == 0x03 || op == 0x29 || op == 0x2B || op == 0x31 ||
        op == 0x33 || op == 0x39 || op == 0xFF ||
        (op >= 0xD8 && op <= 0xDF)) {
        uint8_t modrm = p[1];
        uint8_t mod = modrm >> 6;
        uint8_t rm  = modrm & 7;
        int base = 2; // Opcode byte plus the ModRM byte.
        if (mod == 3) return base;                    // Both operands are registers.
        if (mod == 0 && rm == 5) return base + 4;     // The address is an absolute disp32.
        if (mod == 0 && rm == 4) return base + 1;     // SIB with no displacement.
        if (mod == 0) return base;                    // Register indirect.
        if (mod == 1 && rm == 4) return base + 1 + 1; // SIB plus a disp8.
        if (mod == 1) return base + 1;                // Register plus a disp8.
        if (mod == 2 && rm == 4) return base + 1 + 4; // SIB plus a disp32.
        if (mod == 2) return base + 4;                // Register plus a disp32.
        return 0;
    }

    // sub, add, or cmp of a register with an imm8 or an imm32.
    // A memory operand with a SIB or a displacement is longer than this
    if (op == 0x83) return 3;
    if (op == 0x81) return 6;
    // Moves an imm32 into a register.
    if (op >= 0xB8 && op <= 0xBF) return 5;
    // Segment prefix. The length includes the prefixed opcode.
    if (op == 0x64 || op == 0x65 || op == 0x26 || op == 0x2E ||
        op == 0x36 || op == 0x3E) {
        int inner = InsnLength(p + 1);
        return inner ? 1 + inner : 0;
    }
    // lea uses the same ModRM sizes as the integer forms above.
    if (op == 0x8D) {
        uint8_t modrm = p[1];
        uint8_t mod = modrm >> 6; uint8_t rm = modrm & 7;
        int base = 2;
        if (mod == 0 && rm == 5) return base + 4;
        if (mod == 0 && rm == 4) return base + 1;
        if (mod == 0) return base;
        if (mod == 1 && rm == 4) return base + 2;
        if (mod == 1) return base + 1;
        if (mod == 2 && rm == 4) return base + 5;
        if (mod == 2) return base + 4;
        if (mod == 3) return base;
        return 0;
    }
    // test al, imm8, or test eax, imm32.
    if (op == 0xA8) return 2;
    if (op == 0xA9) return 5;
    // ret imm16.
    if (op == 0xC2) return 3;

    return 0;
}

// Walks complete instructions until at least `minBytes` are covered.
// Returns 0 when an opcode is not recognised.
inline int ComputeStolenBytes(const void* target, int minBytes = 5) {
    const uint8_t* p = (const uint8_t*)target;
    int total = 0;
    while (total < minBytes) {
        int len = InsnLength(p + total);
        if (len == 0) return 0;
        total += len;
    }
    return total;
}

// Installs a trampoline hook.
// When `forcedStolen` is positive, that many bytes are copied instead of a decoded prologue.
inline bool Install(void* target, void* detour, void** original,
                    int forcedStolen = 0) {
    int stolenBytes = forcedStolen > 0
        ? forcedStolen
        : ComputeStolenBytes(target, 5);
    if (stolenBytes == 0 || stolenBytes > 32)
        return false;

    DWORD oldProtect;
    if (!VirtualProtect(target, stolenBytes + 16,
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    uint8_t* trampoline = (uint8_t*)VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        VirtualProtect(target, stolenBytes + 16, oldProtect, &oldProtect);
        return false;
    }

    memcpy(trampoline, target, stolenBytes);

    // A copied rel32 call or jump must still aim at the original destination.
    // a conditional jump is not decoded, so the stolen byte walk already fails on one.
    for (int off = 0; off < stolenBytes; ) {
        const uint8_t* src = (const uint8_t*)target + off;
        int len = InsnLength(src);
        if (len == 0) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            VirtualProtect(target, stolenBytes + 16, oldProtect, &oldProtect);
            return false;
        }
        if ((src[0] == 0xE8 || src[0] == 0xE9) && off + 5 <= stolenBytes) {
            int32_t rel = *(const int32_t*)(src + 1);
            const uint8_t* oldNext = src + 5;
            uint8_t* newNext = trampoline + off + 5;
            *(int32_t*)(trampoline + off + 1) =
                (int32_t)((oldNext + rel) - newNext);
        }
        off += len;
    }

    // Jump from the trampoline back to the first instruction after the stolen bytes.
    trampoline[stolenBytes] = 0xE9;
    *(int32_t*)(trampoline + stolenBytes + 1) =
        (int32_t)((uint8_t*)target + stolenBytes -
                  (trampoline + stolenBytes + 5));

    // Overwrite the target with a jump to the detour, and nop the remaining stolen bytes.
    *(uint8_t*)target = 0xE9;
    *(int32_t*)((uint8_t*)target + 1) =
        (int32_t)((uint8_t*)detour - ((uint8_t*)target + 5));
    for (int i = 5; i < stolenBytes; ++i)
        *((uint8_t*)target + i) = 0x90;

    VirtualProtect(target, stolenBytes + 16, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, stolenBytes);
    *original = trampoline;
    return true;
}

// Writes `len` bytes at `addr`.
inline void Patch(uintptr_t addr, const void* data, size_t len) {
    DWORD old;
    VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, data, len);
    VirtualProtect((void*)addr, len, old, &old);
}

} // namespace Hook
