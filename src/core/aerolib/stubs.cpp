// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <unordered_map>
#include "common/logging/log.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"

namespace Core::AeroLib {

// Helper to provide stub implementations for missing functions
//
// This works by pre-compiling generic stub functions ("slots"), and then
// on lookup, setting up the nid_entry they are matched with
//
// If it runs out of stubs with name information, it will return
// a default implementation without function name details

// Up to 512, larger values lead to more resolve stub slots
// and to longer compile / CI times
//
// Must match STUBS_LIST define
constexpr u32 MAX_STUBS = 16384;

u64 UnresolvedStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    return 0;
}

static u64 UnknownStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    return 0;
}

static const NidEntry* stub_nids[MAX_STUBS];
static std::string stub_nids_unknown[MAX_STUBS];

template <int stub_index>
static u64 CommonStub() {
    auto entry = stub_nids[stub_index];
    if (entry) {
        LOG_ERROR(Core, "Stub: {} (nid: {}) called, returning zero to {}", entry->name, entry->nid,
                  __builtin_return_address(0));
    } else {
        LOG_ERROR(Core, "Stub: Unknown (nid: {}) called, returning zero to {}",
                  stub_nids_unknown[stub_index], __builtin_return_address(0));
    }
    return 0;
}

static u32 UsedStubEntries;

#define XREP_1(x) &CommonStub<x>,

#define XREP_2(x) XREP_1(x) XREP_1(x + 1)
#define XREP_4(x) XREP_2(x) XREP_2(x + 2)
#define XREP_8(x) XREP_4(x) XREP_4(x + 4)
#define XREP_16(x) XREP_8(x) XREP_8(x + 8)
#define XREP_32(x) XREP_16(x) XREP_16(x + 16)
#define XREP_64(x) XREP_32(x) XREP_32(x + 32)
#define XREP_128(x) XREP_64(x) XREP_64(x + 64)
#define XREP_256(x) XREP_128(x) XREP_128(x + 128)
#define XREP_512(x) XREP_256(x) XREP_256(x + 256)
#define XREP_1024(x) XREP_512(x) XREP_512(x + 512)
#define XREP_2048(x) XREP_1024(x) XREP_1024(x + 1024)
#define XREP_4096(x) XREP_2048(x) XREP_2048(x + 2048)
#define XREP_8192(x) XREP_4096(x) XREP_4096(x + 4096)
#define XREP_16384(x) XREP_8192(x) XREP_8192(x + 8192)

#define STUBS_LIST XREP_16384(0)

static u64 (*stub_handlers[MAX_STUBS])() = {STUBS_LIST};

u64 GetStub(const char* nid) {
    // Dedup by NID. The linker calls GetStub once per *import reference*, but every
    // import of the same symbol should resolve to the same stub address — both for
    // correctness (vtables / type_info live in data and must compare equal) and to
    // avoid burning a slot per usage. GT7 in particular references the same C++ ABI
    // vtables (e.g. _ZTVN10__cxxabiv117__class_type_infoE) thousands of times.
    static std::unordered_map<std::string, u64> nid_to_stub;
    if (auto it = nid_to_stub.find(nid); it != nid_to_stub.end()) {
        return it->second;
    }

    if (UsedStubEntries >= MAX_STUBS) {
        LOG_ERROR(Core,
                  "Stub slot overflow (MAX_STUBS={}): no per-NID slot for {}; falling back to "
                  "shared UnknownStub. Bump MAX_STUBS to identify hot loops.",
                  MAX_STUBS, nid);
        nid_to_stub.emplace(nid, (u64)&UnknownStub);
        return (u64)&UnknownStub;
    }

    const auto entry = FindByNid(nid);
    if (!entry) {
        stub_nids_unknown[UsedStubEntries] = nid;
    } else {
        stub_nids[UsedStubEntries] = entry;
    }

    const u32 slot = UsedStubEntries++;
    if ((slot + 1) % 512 == 0 || slot + 1 == MAX_STUBS) {
        LOG_INFO(Core, "Stub slot usage: {}/{} ({} NID: {})", slot + 1, MAX_STUBS,
                 entry ? entry->name : "Unknown", nid);
    }
    const u64 addr = (u64)stub_handlers[slot];
    nid_to_stub.emplace(nid, addr);
    return addr;
}

} // namespace Core::AeroLib
