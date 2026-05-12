// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::DeviceService {

s32 PS4_SYSV_ABI sceDeviceServiceInitialize();
s32 PS4_SYSV_ABI sceDeviceServiceTerminate();
s32 PS4_SYSV_ABI sceDeviceServiceGetEventState(s32* state);
s32 PS4_SYSV_ABI sceDeviceServiceGetGeneration();
s32 PS4_SYSV_ABI sceDeviceServiceQueryDeviceInfo_();
s32 PS4_SYSV_ABI sceMbusGetDeviceInfoByConditionForDeviceService();

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::DeviceService
