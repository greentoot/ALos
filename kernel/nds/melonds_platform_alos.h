#ifndef KERNEL_NDS_MELONDS_PLATFORM_ALOS_H
#define KERNEL_NDS_MELONDS_PLATFORM_ALOS_H

#include <stdint.h>

#ifdef __cplusplus
#include <string>

namespace alos::nds {

struct InternalRunContext {
    const char *rom_name = nullptr;
    const void *rom_data = nullptr;
    uint32_t rom_size = 0;
    std::string local_root;
    std::string nds_save_path;
    std::string gba_save_path;
    std::string firmware_path;
    volatile int stop_requested = 0;
    volatile int stop_reason = 0;
};

void InstallPlatformContext(InternalRunContext *ctx);
InternalRunContext *GetPlatformContext();
void FillDefaultPlatformPaths(InternalRunContext *ctx, const char *rom_name);

} // namespace alos::nds
#endif

#endif
