#include "nds_core.h"
#include "nds_core_melonds.h"
#include "melonds_platform_alos.h"

#include "../../driver/gfx.h"
#include "../../driver/keyboard.h"
#include "../../driver/mouse.h"
#include "../../driver/serial.h"
#include "../../driver/timer.h"
#include "../../driver/vga.h"
#include "../tty.h"
#include "../fs/ramfs.h"
#include "../jack/font5x8.h"
#include "../jack/vm_store.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../memory/heap.h"

#include "../../third_party/melonds/src/Args.h"
#include "../../third_party/melonds/src/GPU_Soft.h"
#include "../../third_party/melonds/src/NDS.h"
#include "../../third_party/melonds/src/NDSCart.h"
#include "../../third_party/melonds/src/Platform.h"
#include "../../third_party/melonds/src/SPI_Firmware.h"

#include <ctime>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

extern "C" {
int serial_is_ready(void);
void serial_write(const char *s);
void serial_write_line(const char *s);
uint32_t heap_free(void);
uint32_t heap_largest_free(void);
}

namespace {

using melonDS::ARM7BIOSImage;
using melonDS::ARM9BIOSImage;
using melonDS::Firmware;
using melonDS::NDS;
using melonDS::NDSArgs;
using melonDS::NDSCart::NDSCartArgs;
using melonDS::NDSCart::ParseROM;

constexpr uint32_t kNoInputMask = 0x0FFFu;
constexpr int kNdsScreenW = 256;
constexpr int kNdsScreenH = 192;
constexpr int kCanvasW = 512;
constexpr int kCanvasH = 256;
constexpr int kScreenY = 14;
constexpr int kStatusY = 216;
constexpr int kDsPreferredW = 512;
constexpr int kDsPreferredH = 256;
constexpr uint8_t kColorBg = 0x00;
constexpr uint8_t kColorText = 0xFF;
constexpr uint8_t kColorDim = 0x92;
constexpr uint8_t kColorAccent = 0x1F;
constexpr uint8_t kColorPanel = 0x24;
constexpr uint8_t kColorBorder = 0xDB;
constexpr uint32_t kFastBootRevealFrame = 180;
constexpr uint32_t kFastBootHardLimit = 720;

static uint8_t g_nds_canvas[kCanvasW * kCanvasH];
static int g_nds_palette_ready = 0;
static char g_nds_boot_status[96];

struct PreparedSession {
    alos::nds::InternalRunContext ctx {};
    std::unique_ptr<NDS> nds;
};

static void nds_debug_log(const char *msg) {
    if (!msg || !*msg) return;
    if (serial_is_ready()) {
        serial_write("[ALOS-DS] ");
        serial_write_line(msg);
    }
}

static void nds_set_boot_status(const char *status) {
    if (!status) status = "";
    kstrncpy(g_nds_boot_status, status, sizeof(g_nds_boot_status) - 1);
    g_nds_boot_status[sizeof(g_nds_boot_status) - 1] = '\0';
    nds_debug_log(g_nds_boot_status);
}

static const char *find_blob_candidates(const char *const *vm_names,
                                        const char *const *ramfs_names,
                                        uint32_t *out_size) {
    if (vm_names) {
        for (int i = 0; vm_names[i]; i++) {
            uint32_t sz = 0;
            const char *data = vm_store_find(vm_names[i], &sz);
            if (data && sz > 0) {
                if (out_size) *out_size = sz;
                return data;
            }
        }
    }
    if (ramfs_names) {
        for (int i = 0; ramfs_names[i]; i++) {
            RamFSNode *n = ramfs_find(ramfs_names[i]);
            if (n && !ramfs_is_dir(n) && n->size > 0) {
                if (out_size) *out_size = n->size;
                return ramfs_data(n);
            }
        }
    }
    if (out_size) *out_size = 0;
    return nullptr;
}

template <typename TArray>
std::unique_ptr<TArray> load_exact_blob(const char *const *vm_names,
                                        const char *const *ramfs_names) {
    uint32_t size = 0;
    const char *blob = find_blob_candidates(vm_names, ramfs_names, &size);
    if (!blob || size != sizeof(TArray)) return nullptr;
    auto out = std::make_unique<TArray>();
    std::memcpy(out->data(), blob, out->size());
    return out;
}

static std::optional<Firmware> load_firmware_from_assets(void) {
    static const char *kVmNames[] = {
        "bios/nds/firmware.bin",
        "firmware.bin",
        nullptr
    };
    static const char *kFsNames[] = {
        "/bios/nds/firmware.bin",
        "/firmware.bin",
        nullptr
    };
    uint32_t size = 0;
    const char *blob = find_blob_candidates(kVmNames, kFsNames, &size);
    if (!blob || size == 0) return std::nullopt;
    Firmware firmware(reinterpret_cast<const melonDS::u8 *>(blob), size);
    if (!firmware.Buffer()) return std::nullopt;
    return firmware;
}

static std::optional<Firmware> load_firmware_from_runtime(const std::string &path) {
    melonDS::Platform::FileHandle *file;
    Firmware firmware = Firmware(0);

    if (path.empty()) return std::nullopt;
    file = melonDS::Platform::OpenFile(path, melonDS::Platform::FileMode::Read);
    if (!file) return std::nullopt;
    firmware = Firmware(file);
    melonDS::Platform::CloseFile(file);
    if (!firmware.Buffer()) return std::nullopt;
    return firmware;
}

static void firmware_copy_utf16(char16_t *dst,
                                size_t capacity,
                                uint16_t *out_len,
                                const char *src) {
    size_t len = 0;

    if (!dst || capacity == 0) return;
    std::memset(dst, 0, capacity * sizeof(char16_t));
    if (!src) {
        if (out_len) *out_len = 0;
        return;
    }
    while (src[len] && len < capacity) {
        dst[len] = static_cast<unsigned char>(src[len]);
        len++;
    }
    if (out_len) *out_len = static_cast<uint16_t>(len);
}

static bool firmware_mac_invalid(const Firmware::FirmwareHeader &header) {
    bool all_zero = true;
    bool all_ff = true;

    for (uint8_t byte : header.MacAddr) {
        if (byte != 0x00) all_zero = false;
        if (byte != 0xFF) all_ff = false;
    }
    return all_zero || all_ff;
}

static bool firmware_is_generated(const Firmware &firmware) {
    return firmware.GetHeader().Identifier == melonDS::GENERATED_FIRMWARE_IDENTIFIER;
}

static void firmware_apply_user_defaults(Firmware::UserData *data, uint16_t update_counter) {
    if (!data) return;

    data->FavoriteColor = 1;
    data->BirthdayMonth = 3;
    data->BirthdayDay = 22;
    data->TouchCalibrationADC1[0] = 0;
    data->TouchCalibrationADC1[1] = 0;
    data->TouchCalibrationPixel1[0] = 0;
    data->TouchCalibrationPixel1[1] = 0;
    data->TouchCalibrationADC2[0] = 255 << 4;
    data->TouchCalibrationADC2[1] = 191 << 4;
    data->TouchCalibrationPixel2[0] = 255;
    data->TouchCalibrationPixel2[1] = 191;
    data->Settings = static_cast<uint16_t>((data->Settings & ~0x0007u) |
                                           static_cast<uint16_t>(Firmware::Language::French));
    data->Settings |= static_cast<uint16_t>(Firmware::BacklightLevel::Max);
    firmware_copy_utf16(data->Nickname, 10, &data->NameLength, "ALOS");
    firmware_copy_utf16(data->Message, 26, &data->MessageLength, "ALOS DS");
    data->UpdateCounter = update_counter;
    if (data->ExtendedSettings.Unknown0 == 0x01) {
        data->ExtendedSettings.ExtendedLanguage = Firmware::Language::French;
        data->ExtendedSettings.SupportedLanguageMask = 0x7F;
    }
    data->UpdateChecksum();
}

static void firmware_apply_wifi_defaults(Firmware &firmware) {
    auto &access_points = firmware.GetAccessPoints();

    access_points[0] = Firmware::WifiAccessPoint(0);
    access_points[1] = Firmware::WifiAccessPoint();
    access_points[2] = Firmware::WifiAccessPoint();

    std::memset(access_points[0].SSID, 0, sizeof(access_points[0].SSID));
    std::memcpy(access_points[0].SSID, "ALOS-DS", 7);
    access_points[0].SSIDLength = 7;
    access_points[0].Status = Firmware::AccessPointStatus::Normal;
    access_points[0].ConnectionConfigured = 0x01;
    access_points[0].UpdateChecksum();
    access_points[1].UpdateChecksum();
    access_points[2].UpdateChecksum();
}

static void firmware_apply_alos_defaults(Firmware &firmware,
                                         bool repair_identity,
                                         bool repair_wifi) {
    auto &header = firmware.GetHeader();
    auto &user_data = firmware.GetUserData();
    const Firmware::UserData &effective = firmware.GetEffectiveUserData();
    uint16_t next_counter = effective.UpdateCounter ? static_cast<uint16_t>(effective.UpdateCounter + 1) : 1;

    if (repair_identity || !effective.ChecksumValid()) {
        Firmware::UserData user0 = effective.ChecksumValid() ? effective : Firmware::UserData(0);
        Firmware::UserData user1 = user0;
        firmware_apply_user_defaults(&user0, next_counter);
        firmware_apply_user_defaults(&user1, static_cast<uint16_t>(next_counter + 1));
        user_data[0] = user0;
        user_data[1] = user1;
    }

    if (repair_wifi) {
        firmware_apply_wifi_defaults(firmware);
    }

    if (firmware_mac_invalid(header)) {
        header.MacAddr = melonDS::DEFAULT_MAC;
        header.UpdateChecksum();
    }
    firmware.UpdateChecksums();
}

static std::optional<Firmware> load_firmware_blob(alos::nds::InternalRunContext *ctx) {
    (void)ctx;
    // Keep the DS boot path aligned with the last known-good ALOS state:
    // use the embedded firmware blob directly and avoid runtime rewriting
    // until the internal boot path is fully stable again.
    return load_firmware_from_assets();
}

static uint64_t hash_framebuffer(const uint32_t *pixels, size_t count) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;

    uint64_t hash = kOffset;
    const auto *bytes = reinterpret_cast<const uint8_t *>(pixels);
    for (size_t i = 0; i < count * sizeof(uint32_t); ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

static const char *safe_rom_name(const char *rom_name) {
    return (rom_name && *rom_name) ? rom_name : "rom.nds";
}

static void nds_setup_palette_332(void) {
    if (g_nds_palette_ready) return;
    for (int i = 0; i < 256; i++) {
        uint8_t r3 = static_cast<uint8_t>((i >> 5) & 0x7);
        uint8_t g3 = static_cast<uint8_t>((i >> 2) & 0x7);
        uint8_t b2 = static_cast<uint8_t>(i & 0x3);
        uint8_t r6 = static_cast<uint8_t>(r3 * 9);
        uint8_t g6 = static_cast<uint8_t>(g3 * 9);
        uint8_t b6 = static_cast<uint8_t>(b2 * 21);
        gfx_set_palette(static_cast<uint8_t>(i), r6, g6, b6);
    }
    g_nds_palette_ready = 1;
}

static inline uint8_t nds_rgb332(uint32_t px) {
    uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFFu);
    uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFFu);
    uint8_t b = static_cast<uint8_t>(px & 0xFFu);
    return static_cast<uint8_t>(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6));
}

static bool nds_frame_is_flat_color(const uint32_t *pixels, uint32_t rgb24) {
    size_t i;
    if (!pixels) return true;
    for (i = 0; i < static_cast<size_t>(kNdsScreenW * kNdsScreenH); ++i) {
        if ((pixels[i] & 0x00FFFFFFu) != rgb24) return false;
    }
    return true;
}

static bool nds_frame_pair_is_blank(const uint32_t *top, const uint32_t *bottom) {
    return nds_frame_is_flat_color(top, 0x00FFFFFFu) &&
           nds_frame_is_flat_color(bottom, 0x00FFFFFFu);
}

static inline void canvas_put(int x, int y, uint8_t color) {
    if (x < 0 || y < 0 || x >= kCanvasW || y >= kCanvasH) return;
    g_nds_canvas[y * kCanvasW + x] = color;
}

static void canvas_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > kCanvasW) w = kCanvasW - x;
    if (y + h > kCanvasH) h = kCanvasH - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; ++yy) {
        uint8_t *dst = &g_nds_canvas[(y + yy) * kCanvasW + x];
        for (int xx = 0; xx < w; ++xx) dst[xx] = color;
    }
}

static void canvas_draw_char(int x, int y, char ch, uint8_t color) {
    const uint8_t *glyph = hack_font5x8[static_cast<uint8_t>(ch)];
    for (int gx = 0; gx < 5; ++gx) {
        uint8_t bits = glyph[gx];
        for (int gy = 0; gy < 8; ++gy) {
            if (bits & (1u << gy)) {
                canvas_put(x + gx, y + gy, color);
            }
        }
    }
}

static void canvas_draw_text(int x, int y, const char *text, uint8_t color) {
    if (!text) return;
    while (*text) {
        canvas_draw_char(x, y, *text, color);
        x += 6;
        text++;
    }
}

static void canvas_draw_screen(const uint32_t *pixels, int dx, int dy) {
    if (!pixels) return;
    for (int y = 0; y < kNdsScreenH; ++y) {
        uint8_t *dst = &g_nds_canvas[(dy + y) * kCanvasW + dx];
        const uint32_t *src = pixels + y * kNdsScreenW;
        for (int x = 0; x < kNdsScreenW; ++x) {
            dst[x] = nds_rgb332(src[x]);
        }
    }
}

static void canvas_draw_mouse_cursor(const MouseState *mouse) {
    int x;
    int y;
    uint8_t color;

    if (!mouse || (!mouse->ready && !mouse->present)) return;
    x = mouse->x;
    y = mouse->y;
    if (x < 256 || x >= 512 || y < kScreenY || y >= (kScreenY + kNdsScreenH)) return;

    color = (mouse->buttons & 1u) ? kColorAccent : kColorText;
    for (int dx = -4; dx <= 4; ++dx) {
        if (dx != 0) canvas_put(x + dx, y, color);
    }
    for (int dy = -4; dy <= 4; ++dy) {
        if (dy != 0) canvas_put(x, y + dy, color);
    }
    canvas_put(x, y, kColorBorder);
}

static void nds_prepare_live_canvas(const char *rom_name) {
    std::memset(g_nds_canvas, 0, sizeof(g_nds_canvas));
    canvas_fill_rect(0, 0, kCanvasW, kCanvasH, kColorBg);
    canvas_fill_rect(0, kScreenY - 2, kCanvasW, 196, kColorPanel);
    canvas_fill_rect(255, kScreenY - 2, 2, 196, kColorBorder);
    canvas_fill_rect(0, kStatusY, kCanvasW, kCanvasH - kStatusY, kColorPanel);
    canvas_draw_text(8, 3, "NINTENDO DS", kColorText);
    canvas_draw_text(170, 3, safe_rom_name(rom_name), kColorAccent);
    canvas_draw_text(8, kScreenY + kNdsScreenH + 4, "GAUCHE: ECRAN HAUT", kColorText);
    canvas_draw_text(270, kScreenY + kNdsScreenH + 4, "DROITE: ECRAN BAS", kColorText);
    canvas_draw_text(8, kStatusY + 4, "ESC/F1 quitter  SOURIS=stylet  TAB tactile", kColorText);
    canvas_draw_text(8, kStatusY + 14, "FLECHES d-pad  ENTREE start  RETOUR select", kColorDim);
    canvas_draw_text(8, kStatusY + 24, "X=A  Z=B  Q=L  S=R  W=X  C=Y", kColorDim);
}

static void nds_update_live_screens(const uint32_t *top,
                                    const uint32_t *bottom,
                                    const MouseState *mouse) {
    canvas_draw_screen(top, 0, kScreenY);
    canvas_draw_screen(bottom, 256, kScreenY);
    canvas_draw_mouse_cursor(mouse);
}

static void nds_update_live_frame_counter(uint32_t frame_counter) {
    char line[96];
    canvas_fill_rect(430, kStatusY + 24, 80, 10, kColorPanel);
    ksprintf(line, "frame %u", (unsigned)frame_counter);
    canvas_draw_text(430, kStatusY + 24, line, kColorAccent);
}

static void nds_compose_booting(const char *rom_name,
                                const char *status,
                                uint32_t frame_counter) {
    char line[96];

    std::memset(g_nds_canvas, 0, sizeof(g_nds_canvas));
    canvas_fill_rect(0, 0, kCanvasW, kCanvasH, kColorBg);
    canvas_fill_rect(16, 20, kCanvasW - 32, kCanvasH - 40, kColorPanel);
    canvas_fill_rect(16, 20, kCanvasW - 32, 2, kColorBorder);
    canvas_fill_rect(16, kCanvasH - 22, kCanvasW - 32, 2, kColorBorder);
    canvas_fill_rect(16, 20, 2, kCanvasH - 40, kColorBorder);
    canvas_fill_rect(kCanvasW - 18, 20, 2, kCanvasH - 40, kColorBorder);
    canvas_draw_text(28, 34, "NINTENDO DS", kColorText);
    canvas_draw_text(28, 52, safe_rom_name(rom_name), kColorAccent);
    canvas_draw_text(28, 86, "Initialisation du core interne...", kColorText);
    if (status && *status) {
        canvas_draw_text(28, 104, status, kColorDim);
    } else {
        canvas_draw_text(28, 104, "Attente des framebuffers DS", kColorDim);
    }
    canvas_draw_text(28, 138, "Le boot peut prendre quelques dizaines de frames.", kColorDim);
    canvas_draw_text(28, 150, "ESC/F1 pour quitter.", kColorDim);
    ksprintf(line, "frame %u", (unsigned)frame_counter);
    canvas_draw_text(28, 188, line, kColorAccent);
}

static void nds_present_boot_status(const char *rom_name, uint32_t frame_counter) {
    nds_compose_booting(rom_name, g_nds_boot_status, frame_counter);
    gfx_blit_indexed_512x256(g_nds_canvas);
}

static void nds_try_restore_save(alos::nds::InternalRunContext *ctx,
                                 NDSCartArgs *cart_args) {
    melonDS::Platform::FileHandle *file;
    uint32_t size;
    auto save = std::unique_ptr<melonDS::u8[]>();

    if (!ctx || !cart_args || ctx->nds_save_path.empty()) return;
    file = melonDS::Platform::OpenFile(ctx->nds_save_path, melonDS::Platform::FileMode::Read);
    if (!file) return;

    size = static_cast<uint32_t>(melonDS::Platform::FileLength(file));
    if (!size) {
        melonDS::Platform::CloseFile(file);
        return;
    }

    save = std::make_unique<melonDS::u8[]>(size);
    if (!save) {
        melonDS::Platform::CloseFile(file);
        return;
    }
    if (melonDS::Platform::FileRead(save.get(), size, 1, file) != 1) {
        melonDS::Platform::CloseFile(file);
        return;
    }
    melonDS::Platform::CloseFile(file);
    cart_args->SRAM = std::move(save);
    cart_args->SRAMLength = size;
}

static void nds_set_initial_datetime(NDS *nds) {
    std::time_t now;
    std::tm tmv {};
    if (!nds) return;
    now = std::time(nullptr);
#if defined(_WIN32)
    std::tm *tmp = std::localtime(&now);
    if (!tmp) return;
    tmv = *tmp;
#else
    if (!localtime_r(&now, &tmv)) return;
#endif
    nds->RTC.SetDateTime(tmv.tm_year + 1900,
                         tmv.tm_mon + 1,
                         tmv.tm_mday,
                         tmv.tm_hour,
                         tmv.tm_min,
                         tmv.tm_sec);
}

static inline void nds_press_bit(uint32_t *mask, uint32_t bit) {
    if (mask) *mask &= ~(1u << bit);
}

static void nds_apply_input_key(uint8_t key, uint32_t *mask, int *quit, int *touch_center) {
    switch (key) {
        case KEY_ESCAPE:
        case KEY_F1:
            if (quit) *quit = 1;
            break;
        case KEY_RIGHT: nds_press_bit(mask, 4); break;
        case KEY_LEFT:  nds_press_bit(mask, 5); break;
        case KEY_UP:    nds_press_bit(mask, 6); break;
        case KEY_DOWN:  nds_press_bit(mask, 7); break;
        case KEY_BACKSPACE: nds_press_bit(mask, 2); break;
        case KEY_ENTER: nds_press_bit(mask, 3); break;
        case KEY_TAB:
        case ' ':
            if (touch_center) *touch_center = 1;
            break;
        case 'x': case 'X': case 'k': case 'K': nds_press_bit(mask, 0); break;
        case 'z': case 'Z': case 'j': case 'J': nds_press_bit(mask, 1); break;
        case 's': case 'S': nds_press_bit(mask, 8); break;
        case 'a': case 'A': case 'q': case 'Q': nds_press_bit(mask, 9); break;
        case 'w': case 'W': nds_press_bit(mask, 10); break;
        case 'c': case 'C': nds_press_bit(mask, 11); break;
        default:
            break;
    }
}

static uint32_t nds_sample_keys(int *quit, int *touch_center) {
    uint32_t mask = kNoInputMask;
    uint8_t held = keyboard_current_key();
    nds_apply_input_key(held, &mask, quit, touch_center);
    while (keyboard_available()) {
        nds_apply_input_key(keyboard_getkey(), &mask, quit, touch_center);
    }
    return mask;
}

static int nds_prepare_session(const char *rom_name,
                               const void *rom_data,
                               uint32_t rom_size,
                               PreparedSession *session,
                               char *errbuf,
                               uint32_t errbuf_size) {
    static const char *kVmArm7[] = {
        "bios/nds/biosnds7.rom",
        "biosnds7.rom",
        nullptr
    };
    static const char *kFsArm7[] = {
        "/bios/nds/biosnds7.rom",
        "/biosnds7.rom",
        nullptr
    };
    static const char *kVmArm9[] = {
        "bios/nds/biosnds9.rom",
        "biosnds9.rom",
        nullptr
    };
    static const char *kFsArm9[] = {
        "/bios/nds/biosnds9.rom",
        "/biosnds9.rom",
        nullptr
    };

    std::unique_ptr<ARM7BIOSImage> arm7_bios;
    std::unique_ptr<ARM9BIOSImage> arm9_bios;
    std::optional<Firmware> firmware;
    std::unique_ptr<melonDS::u8[]> rom_copy;
    std::unique_ptr<melonDS::NDSCart::CartCommon> cart;
    NDSCartArgs cart_args {};
    NDSArgs nds_args {};

    if (!session) {
        if (errbuf && errbuf_size) {
            kstrncpy(errbuf, "session DS interne invalide", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
        }
        return -100;
    }

    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!rom_data || rom_size == 0) {
        if (errbuf && errbuf_size) {
            kstrncpy(errbuf, "ROM DS vide pour le core interne", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
        }
        return -1;
    }

    if (heap_largest_free() < rom_size || heap_free() < (rom_size + (16u * 1024u * 1024u))) {
        if (errbuf && errbuf_size) {
            ksprintf(errbuf,
                     "Memoire insuffisante pour ROM DS: libre=%u KiB bloc=%u KiB rom=%u KiB",
                     (unsigned)(heap_free() / 1024u),
                     (unsigned)(heap_largest_free() / 1024u),
                     (unsigned)(rom_size / 1024u));
        }
        return -5;
    }

    alos::nds::FillDefaultPlatformPaths(&session->ctx, safe_rom_name(rom_name));
    session->ctx.rom_name = safe_rom_name(rom_name);
    session->ctx.rom_data = rom_data;
    session->ctx.rom_size = rom_size;
    session->ctx.stop_requested = 0;
    session->ctx.stop_reason = 0;
    alos::nds::InstallPlatformContext(&session->ctx);

    nds_set_boot_status("Chargement des BIOS DS");
    nds_present_boot_status(rom_name, 0);

    arm7_bios = load_exact_blob<ARM7BIOSImage>(kVmArm7, kFsArm7);
    arm9_bios = load_exact_blob<ARM9BIOSImage>(kVmArm9, kFsArm9);
    if (!arm7_bios || !arm9_bios) {
        if (errbuf && errbuf_size) {
            kstrncpy(errbuf, "BIOS DS manquants pour le core interne", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
        }
        return -2;
    }

    nds_set_boot_status("Chargement du firmware DS");
    nds_present_boot_status(rom_name, 0);

    firmware = load_firmware_blob(&session->ctx);
    if (!firmware || !firmware->Buffer()) {
        if (errbuf && errbuf_size) {
            kstrncpy(errbuf, "Firmware DS invalide pour le core interne", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
        }
        return -3;
    }

    nds_set_boot_status("Copie de la ROM en memoire");
    nds_present_boot_status(rom_name, 0);
    rom_copy = std::make_unique<melonDS::u8[]>(rom_size);
    std::memcpy(rom_copy.get(), rom_data, rom_size);
    nds_try_restore_save(&session->ctx, &cart_args);

    nds_set_boot_status("Analyse de la cartouche DS");
    nds_present_boot_status(rom_name, 0);
    cart = ParseROM(std::move(rom_copy), rom_size, &session->ctx, std::move(cart_args));
    if (!cart) {
        if (errbuf && errbuf_size) {
            ksprintf(errbuf, "ParseROM a echoue pour %s", safe_rom_name(rom_name));
        }
        return -4;
    }

    nds_set_boot_status("Initialisation du coeur melonDS");
    nds_present_boot_status(rom_name, 0);
    nds_args.ARM7BIOS = std::move(arm7_bios);
    nds_args.ARM9BIOS = std::move(arm9_bios);
    nds_args.Firmware = std::move(*firmware);
    nds_args.JIT = std::nullopt;

    session->nds = std::make_unique<NDS>(std::move(nds_args), &session->ctx);
    nds_set_boot_status("Activation du rendu logiciel");
    nds_present_boot_status(rom_name, 0);
    session->nds->SetRenderer(std::make_unique<melonDS::SoftRenderer>(*session->nds));
    {
        melonDS::RendererSettings render_settings {};
        render_settings.ScaleFactor = 1;
        render_settings.Threaded = false;
        render_settings.HiresCoordinates = false;
        render_settings.BetterPolygons = false;
        session->nds->GetRenderer().SetRenderSettings(render_settings);
    }
    nds_set_boot_status("Reset Nintendo DS");
    nds_present_boot_status(rom_name, 0);
    session->nds->Reset();
    nds_set_boot_status("Insertion de la cartouche");
    nds_present_boot_status(rom_name, 0);
    session->nds->SetNDSCart(std::move(cart));
    session->nds->SetKeyMask(kNoInputMask);
    session->nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
    nds_set_initial_datetime(session->nds.get());
    nds_set_boot_status("Configuration du direct boot");
    nds_present_boot_status(rom_name, 0);
    session->nds->SetupDirectBoot(std::string(safe_rom_name(rom_name)));
    nds_set_boot_status("Demarrage du CPU Nintendo DS");
    nds_present_boot_status(rom_name, 0);
    session->nds->Start();
    nds_set_boot_status("Demarrage OK, attente des premiers ecrans");
    nds_present_boot_status(rom_name, 0);
    return 0;
}

static int nds_fetch_framebuffers(NDS *nds,
                                  const uint32_t **top,
                                  const uint32_t **bottom,
                                  char *errbuf,
                                  uint32_t errbuf_size) {
    void *top_raw = nullptr;
    void *bottom_raw = nullptr;

    if (!nds || !top || !bottom) return -1;
    if (!nds->GPU.GetFramebuffers(&top_raw, &bottom_raw) || !top_raw || !bottom_raw) {
        if (errbuf && errbuf_size) {
            kstrncpy(errbuf, "Aucun framebuffer DS disponible", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
        }
        return -2;
    }
    *top = static_cast<const uint32_t *>(top_raw);
    *bottom = static_cast<const uint32_t *>(bottom_raw);
    return 0;
}

static void nds_finish_run_ui(int graphics_active, const char *summary) {
    if (!graphics_active) return;
    gfx_restore_text_mode();
    vga_init();
    tty_init();
    if (summary && *summary) tty_write(summary);
    else tty_write("Retour au shell ALOS.");
}

} // namespace

int nds_core_available(void) {
    return 1;
}

const char *nds_core_name(void) {
    return "melonDS-internal-video";
}

int nds_core_run_rom(const char *rom_name,
                     const void *rom_data,
                     uint32_t rom_size,
                     char *errbuf,
                     uint32_t errbuf_size) {
    PreparedSession session;
    MouseState mouse {};
    const uint32_t *top = nullptr;
    const uint32_t *bottom = nullptr;
    uint64_t top_hash = 0;
    uint64_t bottom_hash = 0;
    uint32_t frame_counter = 0;
    uint32_t pace_base_ms = 0;
    uint32_t pace_base_frame = 0;
    int quit = 0;
    int graphics_active = 0;
    int saw_frame = 0;
    int showing_live = 0;
    int live_ui_ready = 0;
    int skipped_renders = 0;
    int ret = -1;
    char summary[192];

    gfx_set_preferred_resolution(kDsPreferredW, kDsPreferredH);
    gfx_set_mode13();
    nds_setup_palette_332();
    gfx_clear(kColorBg);
    mouse_set_bounds(kCanvasW, kCanvasH);
    mouse_set_position(256 + (kNdsScreenW / 2), kScreenY + (kNdsScreenH / 2));
    graphics_active = 1;
    nds_set_boot_status("Preparation du coeur interne");
    nds_present_boot_status(rom_name, 0);
    if (nds_prepare_session(rom_name, rom_data, rom_size, &session, errbuf, errbuf_size) != 0) {
        nds_set_boot_status((errbuf && errbuf[0]) ? errbuf : "Echec de preparation DS");
        nds_present_boot_status(rom_name, 0);
        ret = -1;
        goto done;
    }

    nds_debug_log("run: switch to frame loop");
    pace_base_ms = timer_ms();
    pace_base_frame = 0;
    nds_present_boot_status(rom_name, 0);

    while (!quit && session.nds) {
        int touch_center = 0;
        int touch_active = 0;
        int touch_x = 128;
        int touch_y = 96;
        uint32_t key_mask = nds_sample_keys(&quit, &touch_center);
        int frame_is_blank = 0;

        session.nds->SetKeyMask(key_mask);
        mouse_get_state(&mouse);
        if ((mouse.buttons & 1u) &&
            mouse.x >= 256 && mouse.x < 512 &&
            mouse.y >= kScreenY && mouse.y < (kScreenY + kNdsScreenH)) {
            touch_active = 1;
            touch_x = mouse.x - 256;
            touch_y = mouse.y - kScreenY;
        } else if (touch_center) {
            touch_active = 1;
        }
        if (touch_active) session.nds->TouchScreen(static_cast<uint16_t>(touch_x), static_cast<uint16_t>(touch_y));
        else session.nds->ReleaseScreen();

        if (frame_counter < 4) nds_debug_log("run: RunFrame");
        session.nds->RunFrame();
        frame_counter++;

        if (nds_fetch_framebuffers(session.nds.get(), &top, &bottom, errbuf, errbuf_size) == 0) {
            if (!saw_frame) nds_debug_log("run: first framebuffer");
            saw_frame = 1;
            frame_is_blank = nds_frame_pair_is_blank(top, bottom);

                if (!showing_live) {
                    if (!frame_is_blank || frame_counter >= kFastBootHardLimit) {
                        showing_live = 1;
                        pace_base_ms = timer_ms();
                        pace_base_frame = frame_counter;
                    } else {
                    char wait_line[96];
                    ksprintf(wait_line,
                             "Boot Nintendo DS en cours... %u frames calculees",
                             (unsigned)frame_counter);
                    nds_set_boot_status(wait_line);
                    if ((frame_counter < 8) || ((frame_counter & 0xFu) == 0)) {
                        nds_present_boot_status(rom_name, frame_counter);
                    }
                }
            }

            if (showing_live && (frame_counter >= kFastBootRevealFrame || !frame_is_blank)) {
                uint32_t frames_since_sync = frame_counter - pace_base_frame;
                uint32_t target_ms = pace_base_ms + ((frames_since_sync * 1000u) / 60u);
                uint32_t now_ms = timer_ms();
                int32_t lateness = (int32_t)(now_ms - target_ms);
                int skip_render = 0;

                /* When emulation falls behind, keep simulating but drop some
                 * visual blits so gameplay stays closer to real speed. */
                if (live_ui_ready && frame_counter >= 8) {
                    skip_render = ((frame_counter & 1u) != 0u);
                }
                if (lateness > 48) {
                    skip_render = ((frame_counter & 3u) != 0u);
                } else if (lateness > 16) {
                    skip_render = ((frame_counter & 1u) != 0u);
                }

                if (!skip_render) {
                    int full_redraw = 0;
                    if (!live_ui_ready) {
                        nds_prepare_live_canvas(rom_name);
                        live_ui_ready = 1;
                        full_redraw = 1;
                    }
                    nds_update_live_screens(top, bottom, &mouse);
                    if (full_redraw || frame_counter < 8 || ((frame_counter & 0xFu) == 0u)) {
                        nds_update_live_frame_counter(frame_counter);
                        gfx_blit_indexed_512x256(g_nds_canvas);
                    } else {
                        gfx_blit_indexed_512x256_rect(g_nds_canvas, 0, kScreenY, kCanvasW, kNdsScreenH);
                    }
                } else {
                    skipped_renders++;
                }
            } else if (!showing_live || frame_is_blank) {
                if ((frame_counter < 8) || ((frame_counter & 0xFu) == 0)) {
                    nds_present_boot_status(rom_name, frame_counter);
                }
            }
        } else {
            nds_set_boot_status("Boot Nintendo DS en cours...");
            nds_present_boot_status(rom_name, frame_counter);
            if (!session.nds->IsRunning() && frame_counter > 120) {
                if (errbuf && errbuf_size) {
                    ksprintf(errbuf,
                             "Aucun framebuffer DS disponible apres %u frames",
                             (unsigned)frame_counter);
                }
                ret = -2;
                goto done;
            }
        }

        if (session.ctx.stop_requested) quit = 1;

        if (!showing_live) {
            continue;
        }

        if ((frame_counter - pace_base_frame) >= 6u) {
            uint32_t target_ms = pace_base_ms + (((frame_counter - pace_base_frame) * 1000u) / 60u);
            while ((int32_t)(target_ms - timer_ms()) > 0) {
                __asm__ volatile ("hlt");
            }
            if ((int32_t)(timer_ms() - target_ms) > 250) {
                pace_base_ms = timer_ms();
                pace_base_frame = frame_counter;
            } else {
                pace_base_ms = target_ms;
                pace_base_frame = frame_counter;
            }
        }
    }

    if (!saw_frame) {
        if (errbuf && errbuf_size && !errbuf[0]) {
            ksprintf(errbuf,
                     "Aucune image DS produite (%u frames)",
                     (unsigned)frame_counter);
        }
        ret = -3;
        goto done;
    }

    if (nds_fetch_framebuffers(session.nds.get(), &top, &bottom, errbuf, errbuf_size) != 0) {
        ret = -3;
        goto done;
    }

    top_hash = hash_framebuffer(top, static_cast<size_t>(kNdsScreenW * kNdsScreenH));
    bottom_hash = hash_framebuffer(bottom, static_cast<size_t>(kNdsScreenW * kNdsScreenH));
    if (errbuf && errbuf_size) {
        ksprintf(errbuf,
                 "Session DS interne fermee: %s frames=%u top=%x bottom=%x",
                 safe_rom_name(rom_name),
                 (unsigned)frame_counter,
                 (uint32_t)(top_hash & 0xFFFFFFFFu),
                 (uint32_t)(bottom_hash & 0xFFFFFFFFu));
    }
    ret = 0;

done:
    nds_debug_log("run: leaving");
    if (session.nds && session.nds->IsRunning()) {
        session.nds->Stop();
    }
    gfx_set_preferred_resolution(0, 0);
    if (ret == 0) {
        ksprintf(summary,
                 "Retour DS: %s (%u frames, %d redraws sautes)",
                 safe_rom_name(rom_name),
                 (unsigned)frame_counter,
                 skipped_renders);
    } else {
        kstrncpy(summary,
                 (errbuf && errbuf_size && errbuf[0]) ? errbuf : "Retour DS: erreur interne",
                 sizeof(summary) - 1);
        summary[sizeof(summary) - 1] = '\0';
    }
    nds_finish_run_ui(graphics_active, summary);
    return ret;
}

int nds_core_boot_probe(const char *rom_name,
                        const void *rom_data,
                        uint32_t rom_size,
                        uint32_t frames,
                        char *errbuf,
                        uint32_t errbuf_size) {
    PreparedSession session;
    const uint32_t *top = nullptr;
    const uint32_t *bottom = nullptr;
    uint64_t top_hash;
    uint64_t bottom_hash;

    if (nds_prepare_session(rom_name, rom_data, rom_size, &session, errbuf, errbuf_size) != 0) {
        return -1;
    }

    if (frames == 0) frames = 1;
    for (uint32_t i = 0; i < frames; ++i) {
        session.nds->RunFrame();
        if (session.ctx.stop_requested) break;
    }

    if (nds_fetch_framebuffers(session.nds.get(), &top, &bottom, errbuf, errbuf_size) != 0) {
        return -5;
    }

    top_hash = hash_framebuffer(top, static_cast<size_t>(kNdsScreenW * kNdsScreenH));
    bottom_hash = hash_framebuffer(bottom, static_cast<size_t>(kNdsScreenW * kNdsScreenH));

    if (errbuf && errbuf_size) {
        ksprintf(errbuf,
                 "Probe melonDS OK: %s frames=%u top=%x bottom=%x",
                 safe_rom_name(rom_name),
                 frames,
                 (uint32_t)(top_hash & 0xFFFFFFFFu),
                 (uint32_t)(bottom_hash & 0xFFFFFFFFu));
    }
    return 0;
}
