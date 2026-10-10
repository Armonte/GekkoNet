#pragma once

// [security] OS CSPRNG for the session magic. std::rand() (15 bits on MSVCRT, seeded from time()) was the only
// secret protecting Inputs/Disconnect/... from an off-path spoofer; that is brute-forceable in well under a second.
// Fails CLOSED: if the OS gives no entropy we abort rather than fall back to something guessable.

#include "gekko_types.h"

#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdio>
#endif

namespace Gekko {
    // Fills `buf` with `len` cryptographically secure bytes. Returns false only if the OS refused.
    inline bool SecureRandomBytes(void* buf, u32 len) {
#if defined(_WIN32)
        // RtlGenRandom (SystemFunction036), resolved at runtime so no extra import library is needed on any toolchain.
        using Fn = BOOLEAN(WINAPI*)(PVOID, ULONG);
        static Fn fn = [] {
            HMODULE m = LoadLibraryA("advapi32.dll");
            return m ? (Fn)(void*)GetProcAddress(m, "SystemFunction036") : (Fn)nullptr;
        }();
        return fn && fn(buf, len) != 0;
#else
        FILE* f = std::fopen("/dev/urandom", "rb");
        if (!f) return false;
        const bool ok = std::fread(buf, 1, len, f) == len;
        std::fclose(f);
        return ok;
#endif
    }

    // A nonzero 64-bit session magic (0 means "no magic yet" everywhere in the backend).
    inline u64 SecureSessionMagic() {
        for (int attempt = 0; attempt < 8; attempt++) {
            u64 v = 0;
            if (!SecureRandomBytes(&v, sizeof v)) break;
            if (v != 0) return v;
        }
        std::abort();
    }
}
