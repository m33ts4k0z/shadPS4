// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <codecvt>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "core/emulator_state.h"
#include "core/file_format/psf.h"
#include "memory_patcher.h"

namespace MemoryPatcher {

EXPORT uintptr_t g_eboot_address;
uint64_t g_eboot_image_size;
std::string g_game_serial;
std::string patch_file;
bool patches_applied = false;
std::vector<patchInfo> pending_patches;

std::string toHex(u64 value, size_t byteSize) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(byteSize * 2) << value;
    return ss.str();
}

std::string convertValueToHex(const std::string type, const std::string valueStr) {
    std::string result;

    if (type == "byte") {
        const u32 value = std::stoul(valueStr, nullptr, 16);
        result = toHex(value, 1);
    } else if (type == "bytes16") {
        const u32 value = std::stoul(valueStr, nullptr, 16);
        result = toHex(value, 2);
    } else if (type == "bytes32") {
        const u32 value = std::stoul(valueStr, nullptr, 16);
        result = toHex(value, 4);
    } else if (type == "bytes64") {
        const u64 value = std::stoull(valueStr, nullptr, 16);
        result = toHex(value, 8);
    } else if (type == "float32") {
        union {
            float f;
            uint32_t i;
        } floatUnion;
        floatUnion.f = std::stof(valueStr);
        result = toHex(std::byteswap(floatUnion.i), sizeof(floatUnion.i));
    } else if (type == "float64") {
        union {
            double d;
            uint64_t i;
        } doubleUnion;
        doubleUnion.d = std::stod(valueStr);
        result = toHex(std::byteswap(doubleUnion.i), sizeof(doubleUnion.i));
    } else if (type == "utf8") {
        std::vector<unsigned char> byteArray =
            std::vector<unsigned char>(valueStr.begin(), valueStr.end());
        byteArray.push_back('\0');
        std::stringstream ss;
        for (unsigned char c : byteArray) {
            ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        result = ss.str();
    } else if (type == "utf16") {
        std::wstring wide_str(valueStr.size(), L'\0');
        std::mbstowcs(&wide_str[0], valueStr.c_str(), valueStr.size());
        wide_str.resize(std::wcslen(wide_str.c_str()));

        std::u16string valueStringU16;

        for (wchar_t wc : wide_str) {
            if (wc <= 0xFFFF) {
                valueStringU16.push_back(static_cast<char16_t>(wc));
            } else {
                wc -= 0x10000;
                valueStringU16.push_back(static_cast<char16_t>(0xD800 | (wc >> 10)));
                valueStringU16.push_back(static_cast<char16_t>(0xDC00 | (wc & 0x3FF)));
            }
        }

        std::vector<unsigned char> byteArray;
        // convert to little endian
        for (char16_t ch : valueStringU16) {
            unsigned char low_byte = static_cast<unsigned char>(ch & 0x00FF);
            unsigned char high_byte = static_cast<unsigned char>((ch >> 8) & 0x00FF);

            byteArray.push_back(low_byte);
            byteArray.push_back(high_byte);
        }
        byteArray.push_back('\0');
        byteArray.push_back('\0');
        std::stringstream ss;

        for (unsigned char ch : byteArray) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
        result = ss.str();
    } else if (type == "bytes") {
        result = valueStr;
    } else if (type == "mask" || type == "mask_jump32") {
        result = valueStr;
    } else {
        LOG_INFO(Loader, "Error applying Patch, unknown type: {}", type);
    }
    return result;
}

void ApplyPendingPatches();

void ApplyPatchesFromXML(std::filesystem::path path) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());

    auto* param_sfo = Common::Singleton<PSF>::Instance();
    auto app_version = param_sfo->GetString("APP_VER").value_or("Unknown version");

    if (result) {
        auto patchXML = doc.child("Patch");
        for (pugi::xml_node_iterator it = patchXML.children().begin();
             it != patchXML.children().end(); ++it) {

            if (std::string(it->name()) == "Metadata") {
                if (std::string(it->attribute("isEnabled").value()) == "true") {
                    std::string currentPatchName = it->attribute("Name").value();
                    std::string metadataAppVer = it->attribute("AppVer").value();
                    bool versionMatches = metadataAppVer == app_version;

                    auto patchList = it->first_child();
                    for (pugi::xml_node_iterator patchLineIt = patchList.children().begin();
                         patchLineIt != patchList.children().end(); ++patchLineIt) {

                        std::string type = patchLineIt->attribute("Type").value();
                        if (!versionMatches && type != "mask" && type != "mask_jump32")
                            continue;

                        std::string address = patchLineIt->attribute("Address").value();
                        std::string patchValue = patchLineIt->attribute("Value").value();
                        std::string maskOffsetStr = patchLineIt->attribute("Offset").value();
                        std::string targetStr = "";
                        std::string sizeStr = "";
                        if (type == "mask_jump32") {
                            targetStr = patchLineIt->attribute("Target").value();
                            sizeStr = patchLineIt->attribute("Size").value();
                        } else {
                            patchValue = convertValueToHex(type, patchValue);
                        }

                        bool littleEndian = false;
                        if (type == "bytes16" || type == "bytes32" || type == "bytes64") {
                            littleEndian = true;
                        }

                        MemoryPatcher::PatchMask patchMask = MemoryPatcher::PatchMask::None;
                        int maskOffsetValue = 0;

                        if (type == "mask")
                            patchMask = MemoryPatcher::PatchMask::Mask;

                        if (type == "mask_jump32")
                            patchMask = MemoryPatcher::PatchMask::Mask_Jump32;

                        if ((type == "mask" || type == "mask_jump32") && !maskOffsetStr.empty()) {
                            maskOffsetValue = std::stoi(maskOffsetStr, 0, 10);
                        }

                        MemoryPatcher::PatchMemory(currentPatchName, address, patchValue, targetStr,
                                                   sizeStr, false, littleEndian, patchMask,
                                                   maskOffsetValue);
                    }
                }
            }
        }
    } else {
        LOG_ERROR(Loader, "Could not parse patch XML: {}", result.description());
    }
}

static bool WriteEbootBytes(u8* dst, const u8* src, size_t n) {
#if defined(_WIN32)
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), dst, src, n, &written) || written != n) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
#else
    std::memcpy(dst, src, n);
    return true;
#endif
}

static const u8* FindSignature(const u8* haystack, size_t hay_len, const u8* needle,
                               size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) {
        return nullptr;
    }
    for (const u8* p = haystack; p <= haystack + hay_len - needle_len; ++p) {
        if (std::memcmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return nullptr;
}

// GT Sport (CUSA02168): the game's analytics telemetry path tries to virtual-call into
// a `GameAnalytics::Telemetry::Exception` shared_ptr that its factory returned empty.
// The factory hits its build() early-exit path (because shadps4's HLE chain returns 0
// somewhere deep inside `telemetry_event_builder.cxx::build`), and the calling
// `report_error_via_analytics` function does not null-check the result before
// `mov rax, [rdi]; call [rax+0x18]`.
// Skip past the unchecked virtual call by forcing the `test al, al; jne` that follows
// to fall through to the success/cleanup path. We do this by patching the failing
// `mov rax, [rdi]; call [rax+0x18]` (6 bytes) with `mov al, 1` + 4 nops, so the test
// passes and the function exits via its normal cleanup path.
static void ApplyGtSportTelemetryBypass() {
    if (g_game_serial != "CUSA02168") {
        return;
    }
    if (g_eboot_address == 0 || g_eboot_image_size == 0) {
        return;
    }

    // 17-byte signature: `mov rdi,[rbp-0xd0]; mov rax,[rdi]; call [rax+0x18]; test al,al; jne +0x43`
    static constexpr std::array<u8, 17> kSig = {0x48, 0x8b, 0xbd, 0x30, 0xff, 0xff, 0xff, 0x48,
                                                0x8b, 0x07, 0xff, 0x50, 0x18, 0x84, 0xc0, 0x75,
                                                0x43};
    const u8* const begin = reinterpret_cast<const u8*>(g_eboot_address);
    const u8* match = FindSignature(begin, g_eboot_image_size, kSig.data(), kSig.size());
    if (match == nullptr) {
        LOG_WARNING(Loader,
                    "GT Sport telemetry bypass: signature not found (build changed?); "
                    "the analytics-report crash may still trigger.");
        return;
    }

    // Overwrite bytes 7..12 (the failing virtual call) with `b0 01 90 90 90 90`
    // (mov al,1 ; 4x nop). The subsequent `test al, al; jne` then takes the
    // success branch into the function's stack-canary cleanup + ret.
    u8* const patch_at = const_cast<u8*>(match) + 7;
    static constexpr std::array<u8, 6> kNop6 = {0xb0, 0x01, 0x90, 0x90, 0x90, 0x90};
    if (!WriteEbootBytes(patch_at, kNop6.data(), kNop6.size())) {
        LOG_ERROR(Loader, "GT Sport telemetry bypass: WriteProcessMemory failed at {}",
                  fmt::ptr(patch_at));
        return;
    }
    LOG_INFO(Loader, "GT Sport telemetry bypass: patched analytics-report virtual call at {}",
             fmt::ptr(patch_at));
}

// GT Sport (CUSA02168) DIAGNOSTIC PROBE: place int3 at the very first instruction of
// the AdHoc unary-operator throw branch, so when `is_supported_operand` returns
// false the SEH filter (CrashStackTraceHandler in main.cpp) dumps r15 et al.
// At that PC r15 still holds the operand pointer (the throw path opens with
// `mov rdi, r15; call ...`), so the dump tells us exactly which Variant/HObject
// pointer was nil. One-shot diagnostic — game dies at the throw.
static void ApplyGtSportAdhocThrowProbe() {
    if (g_game_serial != "CUSA02168") {
        return;
    }
    if (g_eboot_address == 0 || g_eboot_image_size == 0) {
        return;
    }

    // Same 14-byte unary-throw site signature. Throw branch starts at sig+0x53 with
    // `4c 89 ff` (mov rdi, r15) then `e8 96 87 02 00` (call 0x8017cca30). Instead of
    // tripping the int3 BEFORE mov rdi,r15 (which would let the cpu walk past int3
    // through `89 ff = mov edi, edi`, zero-extending RDI and corrupting the operand
    // pointer), let mov rdi, r15 execute first and place int3 at the call's e8 byte
    // (sig+0x56). At trap time both r15 AND rdi hold the operand pointer.
    static constexpr std::array<u8, 14> kSig = {0x84, 0xc0, 0x74, 0x4f, 0x49, 0x8b, 0x07,
                                                0x48, 0x8d, 0xb5, 0x78, 0xff, 0xff, 0xff};
    const u8* const begin = reinterpret_cast<const u8*>(g_eboot_address);
    const u8* match = FindSignature(begin, g_eboot_image_size, kSig.data(), kSig.size());
    if (match == nullptr) {
        LOG_WARNING(Loader, "GT Sport adhoc throw probe: signature not found");
        return;
    }
    u8* const trap_at = const_cast<u8*>(match) + 0x56;
    static constexpr std::array<u8, 1> kInt3 = {0xcc};
    if (!WriteEbootBytes(trap_at, kInt3.data(), kInt3.size())) {
        LOG_ERROR(Loader, "GT Sport adhoc throw probe: WriteProcessMemory failed at {}",
                  fmt::ptr(trap_at));
        return;
    }
    LOG_INFO(Loader,
             "GT Sport adhoc throw probe: int3 armed at {} (throw path entry); on trap, "
             "see crash_trace.txt for r15 = operand pointer.",
             fmt::ptr(trap_at));
}

// GT Sport (CUSA02168) DIAGNOSTIC PROBE 2: int3 at the entry of
// `HObject::throw_nil_as_helper(this, line)` at runtime VA 0x801796eb0. This helper
// formats "nil object cannot be used as %s." with the type name and then calls
// `throw_adhoc_error(this, line, msg)` -- which never returns. We want to identify
// WHICH of the ~600 call sites near the AdHoc dispatcher fires first for our boot
// cascade, so we can NOP that one `test/jne/lea/lea/mov esi,line/call` site rather
// than try to bypass the universal nil-check (which would just crash the next deref).
//
// On int3 trap the procdump-captured context has the caller's `e8 disp32 call`
// return-address sitting at `[rsp+0]` (the int3 fires BEFORE the function's
// `push rbp` prologue, so rsp still points at the caller's return slot).
//
// The signature `4c 8d 7d b0 41 89 f6 48 89 fb` (the arg-setup
// `lea r15,[rbp-0x50]; mov r14d,esi; mov rbx,rdi`) is unique in eboot text and sits
// at function_start + 0x16. We back off by 0x16 to reach the `push rbp` byte.
static void ApplyGtSportAdhocNilHelperProbe() {
    if (g_game_serial != "CUSA02168") {
        return;
    }
    if (g_eboot_address == 0 || g_eboot_image_size == 0) {
        return;
    }

    static constexpr std::array<u8, 10> kSig = {0x4c, 0x8d, 0x7d, 0xb0, 0x41, 0x89, 0xf6,
                                                0x48, 0x89, 0xfb};
    const u8* const begin = reinterpret_cast<const u8*>(g_eboot_address);
    const u8* match = FindSignature(begin, g_eboot_image_size, kSig.data(), kSig.size());
    if (match == nullptr) {
        LOG_WARNING(Loader, "GT Sport adhoc nil-helper probe: signature not found");
        return;
    }
    u8* const fn_start = const_cast<u8*>(match) - 0x16;
    static constexpr std::array<u8, 1> kInt3 = {0xcc};
    if (!WriteEbootBytes(fn_start, kInt3.data(), kInt3.size())) {
        LOG_ERROR(Loader, "GT Sport adhoc nil-helper probe: WriteProcessMemory failed at {}",
                  fmt::ptr(fn_start));
        return;
    }
    LOG_INFO(Loader,
             "GT Sport adhoc nil-helper probe: int3 armed at {} "
             "(HObject::throw_nil_as_helper entry). On trap, [rsp+0] = caller of throw.",
             fmt::ptr(fn_start));
}

// GT Sport (CUSA02168): even after the analytics-report crash is bypassed, the AdHoc
// VM is still throwing `invalid operand ((nil)) to unary operator __not__` at the
// post-boot listener `ProductBootScreen.ad:99`. The throw site is the AdHoc VM's
// generic unary-operator dispatch (m_unary_operator.cpp:50): it calls an
// `is_supported_operand` check; on `false` it `je`s into the throw path. NOP that
// `je` so the dispatch always falls through to the operator's `apply()` virtual
// call, which for `__not__` on a nil/empty operand returns `true` (Lua-like
// semantic) instead of throwing.
static void ApplyGtSportAdhocNotNilTolerate() {
    if (g_game_serial != "CUSA02168") {
        return;
    }
    if (g_eboot_address == 0 || g_eboot_image_size == 0) {
        return;
    }

    // 14-byte signature: `test al,al; je +0x4f; mov rax,[r15]; lea rsi,[rbp-0x88]`
    static constexpr std::array<u8, 14> kSig = {0x84, 0xc0, 0x74, 0x4f, 0x49, 0x8b, 0x07,
                                                0x48, 0x8d, 0xb5, 0x78, 0xff, 0xff, 0xff};
    const u8* const begin = reinterpret_cast<const u8*>(g_eboot_address);
    const u8* match = FindSignature(begin, g_eboot_image_size, kSig.data(), kSig.size());
    if (match == nullptr) {
        LOG_WARNING(Loader, "GT Sport adhoc not-nil bypass: signature not found");
        return;
    }
    u8* const patch_at = const_cast<u8*>(match) + 2;  // skip past test al, al
    static constexpr std::array<u8, 2> kNop2 = {0x90, 0x90};
    if (!WriteEbootBytes(patch_at, kNop2.data(), kNop2.size())) {
        LOG_ERROR(Loader, "GT Sport adhoc not-nil bypass: WriteProcessMemory failed at {}",
                  fmt::ptr(patch_at));
        return;
    }
    LOG_INFO(Loader, "GT Sport adhoc not-nil bypass: patched unary __not__ throw at {}",
             fmt::ptr(patch_at));
}

// GT Sport (CUSA02168): bypass the listener-dispatch h_object.h:262 nil throw at
// AdHoc interpreter VA 0x80179bb23. The opcode pops an HObject, dies on nil. Convert
// `je 0x80179d0a1` (6 bytes `0f 84 78 15 00 00`) into
// `jmp 0x80179b9cc` + nop (5+1 bytes `e9 a4 fe ff ff 90`) so a nil operand falls
// back to the main dispatch loop instead of throwing.
//
// Signature: `mov rbx,[rdx+rsi*8+0x18]; test rbx,rbx; je 0x80179d0a1` (14 bytes).
// Patch site is at sig+8 (the `0f 84` byte of the je).
//
// New rel32 for the jmp: 0x80179b9cc - (0x80179bb23 + 5) = -0x15c => bytes
// `a4 fe ff ff`. Final 6 bytes: `e9 a4 fe ff ff 90`.
static void ApplyGtSportAdhocNilOnNilContinue() {
    if (g_game_serial != "CUSA02168") {
        return;
    }
    if (g_eboot_address == 0 || g_eboot_image_size == 0) {
        return;
    }

    static constexpr std::array<u8, 14> kSig = {0x48, 0x8b, 0x5c, 0xf2, 0x18, 0x48, 0x85,
                                                0xdb, 0x0f, 0x84, 0x78, 0x15, 0x00, 0x00};
    const u8* const begin = reinterpret_cast<const u8*>(g_eboot_address);
    const u8* match = FindSignature(begin, g_eboot_image_size, kSig.data(), kSig.size());
    if (match == nullptr) {
        LOG_WARNING(Loader, "GT Sport adhoc nil-on-nil-continue: signature not found");
        return;
    }
    u8* const patch_at = const_cast<u8*>(match) + 8;  // start of `je rel32` (0f 84 ..)
    static constexpr std::array<u8, 6> kJmpNop = {0xe9, 0xa4, 0xfe, 0xff, 0xff, 0x90};
    if (!WriteEbootBytes(patch_at, kJmpNop.data(), kJmpNop.size())) {
        LOG_ERROR(Loader,
                  "GT Sport adhoc nil-on-nil-continue: WriteProcessMemory failed at {}",
                  fmt::ptr(patch_at));
        return;
    }
    LOG_INFO(Loader,
             "GT Sport adhoc nil-on-nil-continue: patched je throw -> jmp dispatch at {}",
             fmt::ptr(patch_at));
}

void OnGameLoaded() {
    ApplyGtSportTelemetryBypass();
    // Verified load-bearing: even with the file_system.cpp truncate-via-Create
    // fix, the boot script still throws `invalid operand ((nil)) to unary
    // operator __not__` at PBS:99 in v0.15.x. The pre-bisect-baseline (Jul
    // 2025) commit 499451bb doesn't hit this throw, so something further
    // downstream of the file-truncate semantics also regressed between then
    // and now. This bypass patches the `je throw_unsupported_operand`
    // instruction so the `__not__` on nil returns true and the loop
    // continues, letting the boot reach onBootSequenceDone::End and the
    // EULA window render.
    ApplyGtSportAdhocNotNilTolerate();
    // ApplyGtSportAdhocNilOnNilContinue() crashes the game very early (process
    // exits during font loading with only ~2 flips). The patched site at
    // 0x80179bb23 must also be hit by legitimate (non-listener) code paths
    // and the `jmp dispatch_loop` corrupts state for those callers. Keep it
    // disabled; need to find a more narrowly-targeted fix.
    // ApplyGtSportAdhocNilOnNilContinue();
    // Probes no longer needed — root cause is in the script's __not__-on-nil
    // call, and the bypasses above handle it.
    // ApplyGtSportAdhocNilHelperProbe();
    // ApplyGtSportAdhocThrowProbe();

    std::filesystem::path patch_dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    if (!patch_file.empty()) {

        auto file_path = (patch_dir / patch_file).native();
        if (std::filesystem::exists(patch_file)) {
            ApplyPatchesFromXML(patch_file);
        } else {
            ApplyPatchesFromXML(file_path);
        }
    } else if (EmulatorState::GetInstance()->IsAutoPatchesLoadEnabled()) {
        for (auto const& repo : std::filesystem::directory_iterator(patch_dir)) {
            if (!repo.is_directory()) {
                continue;
            }
            std::ifstream json_file{repo.path() / "files.json"};
            nlohmann::json available_patches = nlohmann::json::parse(json_file);
            std::filesystem::path game_patch_file;
            for (auto const& [filename, serials] : available_patches.items()) {
                if (std::find(serials.begin(), serials.end(), g_game_serial) != serials.end()) {
                    game_patch_file = repo.path() / filename;
                    break;
                }
            }
            if (std::filesystem::exists(game_patch_file)) {
                ApplyPatchesFromXML(game_patch_file);
            }
        }
    }
    ApplyPendingPatches();
}

void AddPatchToQueue(patchInfo patchToAdd) {
    if (patches_applied) {
        PatchMemory(patchToAdd.modNameStr, patchToAdd.offsetStr, patchToAdd.valueStr,
                    patchToAdd.targetStr, patchToAdd.sizeStr, patchToAdd.isOffset,
                    patchToAdd.littleEndian, patchToAdd.patchMask, patchToAdd.maskOffset);
        return;
    }
    pending_patches.push_back(patchToAdd);
}

void ApplyPendingPatches() {
    patches_applied = true;
    for (size_t i = 0; i < pending_patches.size(); ++i) {
        const patchInfo& currentPatch = pending_patches[i];

        if (currentPatch.gameSerial != "*" && currentPatch.gameSerial != g_game_serial)
            continue;

        PatchMemory(currentPatch.modNameStr, currentPatch.offsetStr, currentPatch.valueStr,
                    currentPatch.targetStr, currentPatch.sizeStr, currentPatch.isOffset,
                    currentPatch.littleEndian, currentPatch.patchMask, currentPatch.maskOffset);
    }

    pending_patches.clear();
}

void PatchMemory(std::string modNameStr, std::string offsetStr, std::string valueStr,
                 std::string targetStr, std::string sizeStr, bool isOffset, bool littleEndian,
                 PatchMask patchMask, int maskOffset) {
    // Send a request to modify the process memory.
    void* cheatAddress = nullptr;

    if (patchMask == PatchMask::None) {
        if (isOffset) {
            cheatAddress = reinterpret_cast<void*>(g_eboot_address + std::stoi(offsetStr, 0, 16));
        } else {
            cheatAddress =
                reinterpret_cast<void*>(g_eboot_address + (std::stoi(offsetStr, 0, 16) - 0x400000));
        }
    }

    if (patchMask == PatchMask::Mask) {
        cheatAddress = reinterpret_cast<void*>(PatternScan(offsetStr) + maskOffset);
    }

    if (patchMask == PatchMask::Mask_Jump32) {
        int jumpSize = std::stoi(sizeStr);

        constexpr int MAX_PATTERN_LENGTH = 256;
        if (jumpSize < 5) {
            LOG_ERROR(Loader, "Jump size must be at least 5 bytes");
            return;
        }
        if (jumpSize > MAX_PATTERN_LENGTH) {
            LOG_ERROR(Loader, "Jump size must be no more than {} bytes.", MAX_PATTERN_LENGTH);
            return;
        }

        // Find the base address using "Address"
        uintptr_t baseAddress = PatternScan(offsetStr);
        if (baseAddress == 0) {
            LOG_ERROR(Loader, "PatternScan failed for mask_jump32 with pattern: {}", offsetStr);
            return;
        }
        uintptr_t patchAddress = baseAddress + maskOffset;

        // Fills the original region (jumpSize bytes) with NOPs
        std::vector<u8> nopBytes(jumpSize, 0x90);
        std::memcpy(reinterpret_cast<void*>(patchAddress), nopBytes.data(), nopBytes.size());

        // Use "Target" to locate the start of the code cave
        uintptr_t jump_target = PatternScan(targetStr);
        if (jump_target == 0) {
            LOG_ERROR(Loader, "PatternScan failed to Target with pattern: {}", targetStr);
            return;
        }

        // Converts the Value attribute to a byte array (payload)
        std::vector<u8> payload;
        for (size_t i = 0; i < valueStr.length(); i += 2) {

            std::string tempStr = valueStr.substr(i, 2);
            const char* byteStr = tempStr.c_str();
            char* endPtr;
            unsigned int byteVal = std::strtoul(byteStr, &endPtr, 16);

            if (endPtr != byteStr + 2) {
                LOG_ERROR(Loader, "Invalid byte in Value: {}", valueStr.substr(i, 2));
                return;
            }
            payload.push_back(static_cast<u8>(byteVal));
        }

        // Calculates the end of the code cave (where the return jump will be inserted)
        uintptr_t code_cave_end = jump_target + payload.size();

        // Write the payload to the code cave, from jump_target
        std::memcpy(reinterpret_cast<void*>(jump_target), payload.data(), payload.size());

        // Inserts the initial jump in the original region to divert to the code cave
        u8 jumpInstruction[5];
        jumpInstruction[0] = 0xE9;
        s32 relJump = static_cast<s32>(jump_target - patchAddress - 5);
        std::memcpy(&jumpInstruction[1], &relJump, sizeof(relJump));
        std::memcpy(reinterpret_cast<void*>(patchAddress), jumpInstruction,
                    sizeof(jumpInstruction));

        // Inserts jump back at the end of the code cave to resume execution after patching
        u8 jumpBack[5];
        jumpBack[0] = 0xE9;
        // Calculates the relative offset to return to the instruction immediately following the
        // overwritten region
        s32 target_return = static_cast<s32>((patchAddress + jumpSize) - (code_cave_end + 5));
        std::memcpy(&jumpBack[1], &target_return, sizeof(target_return));
        std::memcpy(reinterpret_cast<void*>(code_cave_end), jumpBack, sizeof(jumpBack));

        LOG_INFO(Loader,
                 "Applied Patch mask_jump32: {}, PatchAddress: {:#x}, JumpTarget: {:#x}, "
                 "CodeCaveEnd: {:#x}, JumpSize: {}",
                 modNameStr, patchAddress, jump_target, code_cave_end, jumpSize);
        return;
    }

    if (cheatAddress == nullptr) {
        LOG_ERROR(Loader, "Failed to get address for patch {}", modNameStr);
        return;
    }

    std::vector<unsigned char> bytePatch;

    for (size_t i = 0; i < valueStr.length(); i += 2) {
        unsigned char byte =
            static_cast<unsigned char>(std::strtol(valueStr.substr(i, 2).c_str(), nullptr, 16));

        bytePatch.push_back(byte);
    }

    if (littleEndian) {
        std::reverse(bytePatch.begin(), bytePatch.end());
    }

    std::memcpy(cheatAddress, bytePatch.data(), bytePatch.size());

    LOG_INFO(Loader, "Applied patch: {}, Offset: {}, Value: {}", modNameStr,
             (uintptr_t)cheatAddress, valueStr);
}

static std::vector<int32_t> PatternToByte(const std::string& pattern) {
    std::vector<int32_t> bytes;
    const char* start = pattern.data();
    const char* end = start + pattern.size();

    for (const char* current = start; current < end; ++current) {
        if (*current == '?') {
            ++current;
            if (*current == '?')
                ++current;
            bytes.push_back(-1);
        } else {
            bytes.push_back(strtoul(current, const_cast<char**>(&current), 16));
        }
    }

    return bytes;
}

uintptr_t PatternScan(const std::string& signature) {
    std::vector<int32_t> patternBytes = PatternToByte(signature);
    const auto scanBytes = static_cast<uint8_t*>((void*)g_eboot_address);

    const int32_t* sigPtr = patternBytes.data();
    const size_t sigSize = patternBytes.size();

    uint32_t foundResults = 0;
    for (uint32_t i = 0; i < g_eboot_image_size - sigSize; ++i) {
        bool found = true;
        for (uint32_t j = 0; j < sigSize; ++j) {
            if (scanBytes[i + j] != sigPtr[j] && sigPtr[j] != -1) {
                found = false;
                break;
            }
        }

        if (found) {
            foundResults++;
            return reinterpret_cast<uintptr_t>(&scanBytes[i]);
        }
    }

    return 0;
}

} // namespace MemoryPatcher
