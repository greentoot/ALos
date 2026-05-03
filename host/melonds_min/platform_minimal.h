#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

namespace alos::melonds_min
{
struct RunContext
{
    std::filesystem::path project_root;
    std::filesystem::path local_root;
    std::filesystem::path rom_path;
    std::filesystem::path nds_save_path;
    std::filesystem::path gba_save_path;
    std::filesystem::path firmware_source_path;
    std::filesystem::path firmware_save_path;
    std::filesystem::path dump_path;

    std::atomic<bool> stop_requested {false};
    std::atomic<int> stop_reason {0};

    std::mutex io_mutex;
};

void InstallPlatformContext(RunContext* ctx);
RunContext* GetPlatformContext();
}
