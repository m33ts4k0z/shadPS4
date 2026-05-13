// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <CLI/CLI.hpp>
#include <SDL3/SDL_messagebox.h>

#include <core/emulator_settings.h>
#include <core/emulator_state.h>
#include "common/config.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI CrashStackTraceHandler(EXCEPTION_POINTERS* ep) {
    // Write to a fixed file path so we don't depend on stderr / stdout still being usable.
    FILE* f = std::fopen("C:/Users/ff_be/AppData/Roaming/shadPS4/log/crash_trace.txt", "w");
    if (!f) {
        f = stderr;
    }
    auto code = ep->ExceptionRecord->ExceptionCode;
    auto addr = ep->ExceptionRecord->ExceptionAddress;
    std::fprintf(f, "\n!!! UNHANDLED EXCEPTION 0x%08lX at 0x%p !!!\n",
                 (unsigned long)code, addr);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        std::fprintf(f, "  Access violation: %s at 0x%p\n",
                     ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ"
                     : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE"
                     : "EXEC",
                     (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    auto* ctx = ep->ContextRecord;
#ifdef _M_X64
    std::fprintf(f, "  Registers:\n");
    std::fprintf(f, "    RIP=0x%016llX  RSP=0x%016llX  RBP=0x%016llX\n",
                 ctx->Rip, ctx->Rsp, ctx->Rbp);
    std::fprintf(f, "    RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX\n",
                 ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    std::fprintf(f, "    RSI=0x%016llX  RDI=0x%016llX  R8 =0x%016llX  R9 =0x%016llX\n",
                 ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9);
    std::fprintf(f, "    R10=0x%016llX  R11=0x%016llX  R12=0x%016llX  R13=0x%016llX\n",
                 ctx->R10, ctx->R11, ctx->R12, ctx->R13);
    std::fprintf(f, "    R14=0x%016llX  R15=0x%016llX\n",
                 ctx->R14, ctx->R15);
    // Try to dump 32 bytes of instruction stream at RIP. Wrap in __try in case it faults.
    std::fprintf(f, "  Instruction bytes at RIP:");
    __try {
        const auto* ip = reinterpret_cast<const unsigned char*>(ctx->Rip);
        for (int i = 0; i < 32; i++) {
            std::fprintf(f, " %02X", ip[i]);
        }
        std::fprintf(f, "\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(f, " <fault while reading>\n");
    }
#endif
    HANDLE proc = GetCurrentProcess();
    HANDLE thr = GetCurrentThread();
    static bool sym_init = false;
    if (!sym_init) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);
        sym_init = true;
    }
    // Proper StackWalk64 using the saved CONTEXT (host context at fault time).
    CONTEXT walk_ctx = *ctx;
    STACKFRAME64 frame{};
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
#ifdef _M_X64
    frame.AddrPC.Offset = ctx->Rip;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrStack.Offset = ctx->Rsp;
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
    char sym_buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    std::fprintf(f, "  Stack (StackWalk64 from fault context):\n");
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, proc, thr, &frame, &walk_ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        DWORD64 pc = frame.AddrPC.Offset;
        if (!pc) break;
        DWORD64 disp = 0;
        DWORD line_disp = 0;
        const char* name = "<unknown>";
        const char* file = "";
        DWORD line_no = 0;
        if (SymFromAddr(proc, pc, &disp, sym)) {
            name = sym->Name;
        }
        if (SymGetLineFromAddr64(proc, pc, &line_disp, &line)) {
            file = line.FileName;
            line_no = line.LineNumber;
        }
        std::fprintf(f, "    [%2d] 0x%016llX  %s+0x%llX  (%s:%lu)\n",
                     i, pc, name, disp, file, line_no);
    }
    std::fflush(f);
    if (f != stderr) {
        std::fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Vectored handler so we run BEFORE shadps4's signals.cpp VEH chain decides to swallow.
static LONG WINAPI CrashVectoredHandler(EXCEPTION_POINTERS* ep) {
    auto code = ep->ExceptionRecord->ExceptionCode;
    // Only handle hard crashes; ignore guest-tracked access violations that shadps4 will resolve.
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW) {
        static int dumped = 0;
        if (dumped++ == 0) {
            CrashStackTraceHandler(ep);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
#include <core/user_settings.h>

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    // Keep only the last-resort SEH filter — vectored handler was intercepting and preventing
    // shadps4's own VEH chain from running the cpu_patches handler.
    SetUnhandledExceptionFilter(CrashStackTraceHandler);
#endif

    CLI::App app{"shadPS4 Emulator CLI"};

    // ---- CLI state ----
    std::optional<std::string> gamePath;
    std::vector<std::string> gameArgs;
    std::optional<std::filesystem::path> overrideRoot;
    std::optional<int> waitPid;
    bool waitForDebugger = false;

    std::optional<std::string> fullscreenStr;
    bool ignoreGamePatch = false;
    bool showFps = false;
    bool configClean = false;
    bool configGlobal = false;
    bool bigPicture = false;

    std::optional<std::filesystem::path> addGameFolder;
    std::optional<std::filesystem::path> setAddonFolder;
    std::optional<std::string> patchFile;

    // ---- Options ----
    app.add_option("-g,--game", gamePath, "Game path or ID");
    app.add_option("-p,--patch", patchFile, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", ignoreGamePatch,
                 "Disable automatic loading of game patches");

    app.add_flag("-b,--big-picture", bigPicture, "Start in Big Picture Mode");

    // FULLSCREEN: behavior-identical
    app.add_option("-f,--fullscreen", fullscreenStr, "Fullscreen mode (true|false)");

    app.add_option("--override-root", overrideRoot)->check(CLI::ExistingDirectory);

    app.add_flag("--wait-for-debugger", waitForDebugger);
    app.add_option("--wait-for-pid", waitPid);

    app.add_flag("--show-fps", showFps);
    app.add_flag("--config-clean", configClean);
    app.add_flag("--config-global", configGlobal);
    app.add_flag("--log-append", Common::Log::g_should_append);

    app.add_option("--add-game-folder", addGameFolder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", setAddonFolder)->check(CLI::ExistingDirectory);

    // ---- Capture args after `--` verbatim ----
    app.allow_extras();
    app.parse_complete_callback([&]() {
        const auto& extras = app.remaining();
        if (!extras.empty()) {
            gameArgs = extras;
        }
    });

    // ---- No-args behavior ----
    if (argc == 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "This is a CLI application. Please use the QTLauncher for a GUI:\n"
                                 "https://github.com/shadps4-emu/shadps4-qtlauncher/releases",
                                 nullptr);
        std::cout << app.help();
        return -1;
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    // Start default log
    Common::Log::Setup("shad_log.txt");

    IPC::Instance().Init();

    auto emu_state = std::make_shared<EmulatorState>();
    EmulatorState::SetInstance(emu_state);
    UserSettings.Load();

    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::load(user_dir / "config.toml");

    // ---- Trophy key migration ----
    auto key_manager = KeyManager::GetInstance();
    key_manager->LoadFromFile();
    if (key_manager->GetAllKeys().TrophyKeySet.ReleaseTrophyKey.empty() &&
        !Config::getTrophyKey().empty()) {
        auto keys = key_manager->GetAllKeys();
        if (keys.TrophyKeySet.ReleaseTrophyKey.empty() && !Config::getTrophyKey().empty()) {
            keys.TrophyKeySet.ReleaseTrophyKey =
                KeyManager::HexStringToBytes(Config::getTrophyKey());
            key_manager->SetAllKeys(keys);
            key_manager->SaveToFile();
        }
    }

    // Load configurations
    std::shared_ptr<EmulatorSettingsImpl> emu_settings = std::make_shared<EmulatorSettingsImpl>();
    EmulatorSettingsImpl::SetInstance(emu_settings);
    emu_settings->Load();

    Common::Log::Shutdown();
    // Start configured log
    Common::Log::g_should_append |= EmulatorSettings.IsLogAppend();
    Common::Log::Setup("shad_log.txt");

    if (bigPicture) {
        BigPictureMode::Launch(argv[0]);
        return 0;
    }

    // ---- Utility commands ----
    if (addGameFolder) {
        EmulatorSettings.AddGameInstallDir(*addGameFolder);
        EmulatorSettings.Save();
        std::cout << "Game folder successfully saved.\n";
        return 0;
    }

    if (setAddonFolder) {
        EmulatorSettings.SetAddonInstallDir(*setAddonFolder);
        EmulatorSettings.Save();
        std::cout << "Addon folder successfully saved.\n";
        return 0;
    }

    if (!gamePath.has_value()) {
        if (!gameArgs.empty()) {
            gamePath = gameArgs.front();
            gameArgs.erase(gameArgs.begin());
        } else {
            std::cerr << "Error: Please provide a game path or ID.\n";
            return 1;
        }
    }
    if (!gameArgs.empty()) {
        if (gameArgs.front() == "--") {
            gameArgs.erase(gameArgs.begin());
        } else {
            std::cerr << "Error: unhandled flags\n";
            return 1;
        }
    }

    // ---- Apply flags ----
    if (patchFile)
        MemoryPatcher::patch_file = *patchFile;

    if (ignoreGamePatch)
        Core::FileSys::MntPoints::ignore_game_patches = true;

    if (fullscreenStr) {
        if (*fullscreenStr == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreenStr == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            std::cerr << "Error: Invalid argument for --fullscreen (use true|false)\n";
            return 1;
        }
    }

    if (showFps)
        EmulatorSettings.SetShowFpsCounter(true);

    if (configClean)
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);

    if (configGlobal)
        EmulatorSettings.SetConfigMode(ConfigMode::Global);

    // ---- Resolve game path or ID ----
    std::filesystem::path ebootPath(*gamePath);
    if (!std::filesystem::exists(ebootPath)) {
        bool found = false;
        constexpr int maxDepth = 5;
        for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
            if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, maxDepth)) {
                ebootPath = *foundPath;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Error: Game ID or file path not found: " << *gamePath << "\n";
            return 1;
        }
    }

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(ebootPath, gameArgs, overrideRoot);

    return 0;
}
