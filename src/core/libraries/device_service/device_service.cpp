// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "device_service.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::DeviceService {

// libSceDeviceService surfaces external-hardware events (wheels, USB peripherals,
// bluetooth devices) to titles. We have no real device-event source, so all of
// these are stubs. The important one is sceDeviceServiceGetEventState — GT7
// polls it in a hot loop on the update thread, and an unconditional 0 makes the
// game treat "success, event delivered" as "the event you wanted hasn't arrived
// yet" and re-poll forever. Returning a non-zero status with the out-parameter
// cleared to 0 satisfies callers that read the state field directly and breaks
// the polling loop on callers that branch on the return code.

s32 PS4_SYSV_ABI sceDeviceServiceInitialize() {
    LOG_INFO(Lib_DeviceService, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceDeviceServiceTerminate() {
    LOG_INFO(Lib_DeviceService, "(STUBBED) called");
    return ORBIS_OK;
}

// Real PS4 signature is (s32 event_id, s32* state) — GT Sport calls this with
// event_id in rdi and a valid out-pointer in rsi. The previous one-arg form
// treated event_id as the out-pointer and wrote *(int*)1 — instant AV.
s32 PS4_SYSV_ABI sceDeviceServiceGetEventState(s32 event_id, s32* state) {
    if (state != nullptr) {
        *state = 0;
    }
    LOG_TRACE(Lib_DeviceService, "(STUBBED) called event_id={}", event_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceDeviceServiceGetGeneration() {
    LOG_INFO(Lib_DeviceService, "(STUBBED) called");
    return 1;
}

s32 PS4_SYSV_ABI sceDeviceServiceQueryDeviceInfo_() {
    LOG_INFO(Lib_DeviceService, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMbusGetDeviceInfoByConditionForDeviceService() {
    LOG_INFO(Lib_DeviceService, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("84fDxStrG44", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceInitialize);
    LIB_FUNCTION("Uq8uW74rVpU", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceTerminate);
    LIB_FUNCTION("9ddRUOV8Q5A", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceGetEventState);
    LIB_FUNCTION("oFon+A5v1z8", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceGetGeneration);
    LIB_FUNCTION("UNMEa+5lrUA", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceQueryDeviceInfo_);
    LIB_FUNCTION("UWh5t-hCbzQ", "libSceDeviceService", 1, "libSceMbus",
                 sceMbusGetDeviceInfoByConditionForDeviceService);
}

} // namespace Libraries::DeviceService
