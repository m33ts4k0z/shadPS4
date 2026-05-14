// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <common/va_ctx.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal.h"
#include "libc_internal_io.h"
#include "libc_internal_math.h"
#include "libc_internal_memory.h"
#include "libc_internal_str.h"
#include "libc_internal_threads.h"
#include "printf.h"

namespace Libraries::LibcInternal {

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    RegisterlibSceLibcInternalMath(sym);
    RegisterlibSceLibcInternalStr(sym);
    RegisterlibSceLibcInternalMemory(sym);
    RegisterlibSceLibcInternalIo(sym);
    RegisterlibSceLibcInternalThreads(sym);
}

void ForceRegisterLib(Core::Loader::SymbolsResolver* sym) {
    // Used to forcibly enable HLEs for broken LLE functions. Real PS4 sprx modules
    // (libSceFont, libSceFreeType, libSceNgs2, libSceJson, etc) import libc memory
    // primitives — memcpy/memset/memmove/memcmp — at startup and on every glyph
    // rasterisation. Until those HLEs are bound, the linker falls through to AeroLib
    // zero-stubs and Sony FreeType silently mangles the glyph bitmaps it returns to
    // the game (which presents as GT Sport's tofu-text symptom).
    //
    // Force-register the memory subset only, for now. Adding Math/Str/Threads on top
    // breaks GT Sport's boot listener (PBS:99) — likely a subtle HLE-vs-sprx ABI
    // mismatch in one of those modules. Memory primitives are simple wrappers around
    // std::memcpy etc. so they're the safest subset to enable.
    RegisterlibSceLibcInternalMemory(sym);
    RegisterlibSceLibcInternalStr(sym);
    // Math omitted: registering all of libc_internal_math.cpp triggers a
    // regression of GT Sport's boot listener (PBS:99). All NIDs cross-check
    // against aerolib.inl — likely a subtle x87-vs-SSE precision difference
    // between glibc's std::sin and Sony's libSceLibcInternal sin. Investigate
    // individually later.
    RegisterlibSceLibcInternalThreads(sym);
    ForceRegisterlibSceLibcInternalIo(sym);
}
} // namespace Libraries::LibcInternal