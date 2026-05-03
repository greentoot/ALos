#include "platform_minimal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Args.h"
#include "GPU_Soft.h"
#include "NDS.h"
#include "NDSCart.h"
#include "SPI_Firmware.h"

namespace fs = std::filesystem;

using alos::melonds_min::InstallPlatformContext;
using alos::melonds_min::RunContext;
using melonDS::ARM7BIOSImage;
using melonDS::ARM9BIOSImage;
using melonDS::Firmware;
using melonDS::NDS;
using melonDS::NDSArgs;
using melonDS::NDSCart::NDSCartArgs;
using melonDS::NDSCart::ParseROM;
using melonDS::Platform::CloseFile;
using melonDS::Platform::FileLength;
using melonDS::Platform::FileRead;
using melonDS::Platform::FileRewind;
using melonDS::Platform::OpenFile;

namespace
{
constexpr uint32_t kNoInputMask = 0x0FFF;

struct Options
{
    fs::path project_root = fs::current_path();
    fs::path bios_dir;
    fs::path rom_path;
    fs::path save_path;
    fs::path firmware_source_path;
    fs::path firmware_save_path;
    fs::path dump_path;
    fs::path ipc_dir;
    int frames = 120;
    bool direct_boot = true;
    bool live = false;
    bool frames_explicit = false;
};

struct LiveInputState
{
    uint32_t magic;
    uint32_t version;
    uint32_t key_mask;
    uint32_t touch_active;
    uint32_t touch_x;
    uint32_t touch_y;
    uint32_t stop_requested;
};

struct LiveFrameHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t frame_id;
    uint32_t flags;
};

constexpr uint32_t kFrameMagic = 0x414C4652; // ALFR
constexpr uint32_t kInputMagic = 0x414C494E; // ALIN
constexpr uint32_t kLiveVersion = 1;

void PrintUsage()
{
    std::cout
        << "Usage: build/melonds_min/melonds_runner <rom.nds> [options]\n"
        << "Options:\n"
        << "  --project-root <path>    Project root (default: cwd)\n"
        << "  --bios-dir <path>        BIOS/firmware directory (default: bios/nds)\n"
        << "  --save <path>            Save output path (default: <rom>.sav)\n"
        << "  --firmware <path>        Source firmware path (default: bios/nds/firmware.bin)\n"
        << "  --firmware-save <path>   Writable firmware copy (default: build/melonds_min/firmware.runtime.bin)\n"
        << "  --dump <path>            PPM dump path (default: build/melonds_min/last_frame.ppm)\n"
        << "  --frames <n>             Number of frames to run (default: 120)\n"
        << "  --live                   Run continuously through IPC files in --ipc-dir\n"
        << "  --ipc-dir <path>         IPC directory (default: build/melonds_min/ipc)\n"
        << "  --firmware-boot          Try full firmware boot instead of direct boot\n"
        << "  --help                   Show this help\n";
}

std::string FileStemUtf8(const fs::path& path)
{
    return path.stem().u8string();
}

template <typename TArray>
std::unique_ptr<TArray> LoadExactBlob(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return nullptr;

    auto result = std::make_unique<TArray>();
    in.read(reinterpret_cast<char*>(result->data()), static_cast<std::streamsize>(result->size()));
    if (!in || in.gcount() != static_cast<std::streamsize>(result->size()))
        return nullptr;

    return result;
}

std::unique_ptr<uint8_t[]> LoadBinaryFile(const fs::path& path, uint32_t& size_out)
{
    size_out = 0;
    auto* handle = OpenFile(path.string(), melonDS::Platform::FileMode::Read);
    if (!handle)
        return nullptr;

    const auto file_len = static_cast<uint32_t>(FileLength(handle));
    FileRewind(handle);
    auto buffer = std::make_unique<uint8_t[]>(file_len);
    const auto read = FileRead(buffer.get(), file_len, 1, handle);
    CloseFile(handle);

    if (read != 1)
        return nullptr;

    size_out = file_len;
    return buffer;
}

std::optional<Firmware> LoadFirmwareImage(const fs::path& preferred_path, const fs::path& fallback_source)
{
    fs::path chosen = preferred_path;
    if (!chosen.empty() && fs::exists(chosen))
    {
        auto* handle = OpenFile(chosen.string(), melonDS::Platform::FileMode::Read);
        if (!handle)
            return std::nullopt;

        Firmware firmware(handle);
        CloseFile(handle);
        if (firmware.Buffer())
            return firmware;
        return std::nullopt;
    }

    if (!fallback_source.empty() && fs::exists(fallback_source))
    {
        auto* handle = OpenFile(fallback_source.string(), melonDS::Platform::FileMode::Read);
        if (!handle)
            return std::nullopt;

        Firmware firmware(handle);
        CloseFile(handle);
        if (firmware.Buffer())
            return firmware;
        return std::nullopt;
    }

    return Firmware(0);
}

std::optional<std::vector<uint8_t>> LoadSaveFile(const fs::path& path)
{
    if (path.empty() || !fs::exists(path))
        return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty())
        return std::nullopt;

    return data;
}

uint64_t HashFramebuffer(const uint32_t* pixels, size_t count)
{
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;

    uint64_t hash = kOffset;
    const auto* bytes = reinterpret_cast<const uint8_t*>(pixels);
    const size_t size = count * sizeof(uint32_t);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

bool WriteCombinedPPM(const fs::path& path, const uint32_t* top, const uint32_t* bottom)
{
    if (!top || !bottom)
        return false;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    constexpr int width = 256;
    constexpr int height = 192 * 2;
    out << "P6\n" << width << " " << height << "\n255\n";

    auto write_screen = [&](const uint32_t* pixels) {
        for (int i = 0; i < 256 * 192; ++i)
        {
            const uint32_t px = pixels[i];
            const char rgb[3] = {
                static_cast<char>((px >> 16) & 0xFF),
                static_cast<char>((px >> 8) & 0xFF),
                static_cast<char>(px & 0xFF),
            };
            out.write(rgb, 3);
        }
    };

    write_screen(top);
    write_screen(bottom);
    return out.good();
}

bool ReadLiveInput(const fs::path& path, LiveInputState& state)
{
    if (!fs::exists(path))
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    in.read(reinterpret_cast<char*>(&state), sizeof(state));
    return in.good() && state.magic == kInputMagic && state.version == kLiveVersion;
}

bool WriteLiveInputDefault(const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    LiveInputState state {};
    state.magic = kInputMagic;
    state.version = kLiveVersion;
    state.key_mask = kNoInputMask;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(reinterpret_cast<const char*>(&state), sizeof(state));
    return out.good();
}

bool WriteLiveFrame(const fs::path& path,
                    const uint32_t* top,
                    const uint32_t* bottom,
                    uint32_t frame_id,
                    uint32_t flags)
{
    if (!top || !bottom)
        return false;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    LiveFrameHeader hdr {};
    hdr.magic = kFrameMagic;
    hdr.version = kLiveVersion;
    hdr.width = 256;
    hdr.height = 384;
    hdr.frame_id = frame_id;
    hdr.flags = flags;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    auto write_screen = [&](const uint32_t* pixels) {
        for (int i = 0; i < 256 * 192; ++i)
        {
            const uint32_t px = pixels[i];
            const char rgba[4] = {
                static_cast<char>((px >> 16) & 0xFF),
                static_cast<char>((px >> 8) & 0xFF),
                static_cast<char>(px & 0xFF),
                static_cast<char>(0xFF),
            };
            out.write(rgba, 4);
        }
    };

    write_screen(top);
    write_screen(bottom);
    return out.good();
}

bool ParseArgs(int argc, char** argv, Options& options, std::string& error)
{
    if (argc < 2)
    {
        error = "ROM .nds manquante.";
        return false;
    }

    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        auto require_value = [&](const char* name) -> const std::string* {
            if (i + 1 >= args.size())
            {
                error = std::string("Option sans valeur: ") + name;
                return nullptr;
            }
            return &args[++i];
        };

        if (arg == "--help")
        {
            PrintUsage();
            std::exit(0);
        }
        else if (arg == "--project-root")
        {
            if (const auto* v = require_value("--project-root"))
                options.project_root = *v;
            else
                return false;
        }
        else if (arg == "--bios-dir")
        {
            if (const auto* v = require_value("--bios-dir"))
                options.bios_dir = *v;
            else
                return false;
        }
        else if (arg == "--save")
        {
            if (const auto* v = require_value("--save"))
                options.save_path = *v;
            else
                return false;
        }
        else if (arg == "--firmware")
        {
            if (const auto* v = require_value("--firmware"))
                options.firmware_source_path = *v;
            else
                return false;
        }
        else if (arg == "--firmware-save")
        {
            if (const auto* v = require_value("--firmware-save"))
                options.firmware_save_path = *v;
            else
                return false;
        }
        else if (arg == "--dump")
        {
            if (const auto* v = require_value("--dump"))
                options.dump_path = *v;
            else
                return false;
        }
        else if (arg == "--frames")
        {
            if (const auto* v = require_value("--frames"))
            {
                options.frames = std::max(1, std::stoi(*v));
                options.frames_explicit = true;
            }
            else
                return false;
        }
        else if (arg == "--live")
        {
            options.live = true;
        }
        else if (arg == "--ipc-dir")
        {
            if (const auto* v = require_value("--ipc-dir"))
                options.ipc_dir = *v;
            else
                return false;
        }
        else if (arg == "--firmware-boot")
        {
            options.direct_boot = false;
        }
        else if (!arg.empty() && arg[0] != '-' && options.rom_path.empty())
        {
            options.rom_path = arg;
        }
        else
        {
            error = "Argument inconnu: " + arg;
            return false;
        }
    }

    if (options.rom_path.empty())
    {
        error = "ROM .nds manquante.";
        return false;
    }

    options.project_root = fs::weakly_canonical(options.project_root);
    if (options.bios_dir.empty())
        options.bios_dir = options.project_root / "bios" / "nds";
    if (options.dump_path.empty())
        options.dump_path = options.project_root / "build" / "melonds_min" / "last_frame.ppm";
    if (options.ipc_dir.empty())
        options.ipc_dir = options.project_root / "build" / "melonds_min" / "ipc";
    if (options.firmware_source_path.empty())
        options.firmware_source_path = options.bios_dir / "firmware.bin";
    if (options.firmware_save_path.empty())
        options.firmware_save_path = options.project_root / "build" / "melonds_min" / "firmware.runtime.bin";
    if (options.save_path.empty())
        options.save_path = options.rom_path.parent_path() / (FileStemUtf8(options.rom_path) + ".sav");
    if (options.live && !options.frames_explicit)
        options.frames = 0;

    return true;
}

void CopyIfMissing(const fs::path& from, const fs::path& to)
{
    if (from.empty() || to.empty() || !fs::exists(from) || fs::exists(to))
        return;

    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}
}

int main(int argc, char** argv)
{
    Options options;
    std::string parse_error;
    if (!ParseArgs(argc, argv, options, parse_error))
    {
        std::cerr << "[melonds_runner] " << parse_error << "\n";
        PrintUsage();
        return 1;
    }

    if (!fs::exists(options.rom_path))
    {
        std::cerr << "[melonds_runner] ROM introuvable: " << options.rom_path << "\n";
        return 1;
    }

    const fs::path arm7_path = options.bios_dir / "biosnds7.rom";
    const fs::path arm9_path = options.bios_dir / "biosnds9.rom";

    auto arm7_bios = LoadExactBlob<ARM7BIOSImage>(arm7_path);
    auto arm9_bios = LoadExactBlob<ARM9BIOSImage>(arm9_path);
    if (!arm7_bios || !arm9_bios)
    {
        std::cerr << "[melonds_runner] BIOS DS manquants ou invalides dans " << options.bios_dir << "\n";
        return 1;
    }

    CopyIfMissing(options.firmware_source_path, options.firmware_save_path);
    auto firmware = LoadFirmwareImage(options.firmware_save_path, options.firmware_source_path);
    if (!firmware || !firmware->Buffer())
    {
        std::cerr << "[melonds_runner] Firmware DS invalide.\n";
        return 1;
    }

    uint32_t rom_size = 0;
    auto rom_data = LoadBinaryFile(options.rom_path, rom_size);
    if (!rom_data || rom_size == 0)
    {
        std::cerr << "[melonds_runner] Impossible de lire la ROM: " << options.rom_path << "\n";
        return 1;
    }

    RunContext ctx {};
    ctx.project_root = options.project_root;
    ctx.local_root = options.project_root;
    ctx.rom_path = fs::weakly_canonical(options.rom_path);
    ctx.nds_save_path = options.save_path;
    ctx.firmware_source_path = options.firmware_source_path;
    ctx.firmware_save_path = options.firmware_save_path;
    ctx.dump_path = options.dump_path;
    InstallPlatformContext(&ctx);

    std::unique_ptr<uint8_t[]> save_buffer;
    uint32_t save_length = 0;
    if (auto save_data = LoadSaveFile(ctx.nds_save_path))
    {
        save_length = static_cast<uint32_t>(save_data->size());
        save_buffer = std::make_unique<uint8_t[]>(save_length);
        std::copy(save_data->begin(), save_data->end(), save_buffer.get());
    }

    NDSCartArgs cart_args {};
    cart_args.SRAM = std::move(save_buffer);
    cart_args.SRAMLength = save_length;

    auto cart = ParseROM(std::move(rom_data), rom_size, &ctx, std::move(cart_args));
    if (!cart)
    {
        std::cerr << "[melonds_runner] ParseROM a echoue pour " << options.rom_path.filename() << "\n";
        return 1;
    }

    NDSArgs nds_args {};
    nds_args.ARM7BIOS = std::move(arm7_bios);
    nds_args.ARM9BIOS = std::move(arm9_bios);
    nds_args.Firmware = std::move(*firmware);
    nds_args.JIT = std::nullopt;

    auto nds = std::make_unique<NDS>(std::move(nds_args), &ctx);
    nds->SetRenderer(std::make_unique<melonDS::SoftRenderer>(*nds));
    nds->Reset();
    nds->SetNDSCart(std::move(cart));
    nds->SetKeyMask(kNoInputMask);
    nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);

    const std::time_t now_time = std::time(nullptr);
    const std::tm* tm = std::localtime(&now_time);
    if (tm)
    {
        nds->RTC.SetDateTime(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    const bool use_direct_boot = options.direct_boot || nds->NeedsDirectBoot();
    if (use_direct_boot)
        nds->SetupDirectBoot(options.rom_path.filename().string());

    nds->Start();

    void* top_raw = nullptr;
    void* bottom_raw = nullptr;
    int ran_frames = 0;
    uint32_t live_frame_id = 0;
    if (options.live)
    {
        WriteLiveInputDefault(options.ipc_dir / "input.bin");
    }

    auto next_present = std::chrono::steady_clock::now();
    while (options.live || ran_frames < options.frames)
    {
        LiveInputState input {};
        uint32_t key_mask = kNoInputMask;
        bool touch_active = false;
        uint16_t touch_x = 0;
        uint16_t touch_y = 0;

        if (options.live && ReadLiveInput(options.ipc_dir / "input.bin", input))
        {
            key_mask = input.key_mask;
            touch_active = input.touch_active != 0;
            touch_x = static_cast<uint16_t>(std::min<uint32_t>(255, input.touch_x));
            touch_y = static_cast<uint16_t>(std::min<uint32_t>(191, input.touch_y));
            if (input.stop_requested)
            {
                ctx.stop_requested = true;
                break;
            }
        }

        nds->SetKeyMask(key_mask);
        if (touch_active)
            nds->TouchScreen(touch_x, touch_y);
        else
            nds->ReleaseScreen();

        nds->RunFrame();
        ++ran_frames;
        if (ctx.stop_requested.load())
            break;

        if (options.live)
        {
            if (nds->GPU.GetFramebuffers(&top_raw, &bottom_raw) && top_raw && bottom_raw)
            {
                WriteLiveFrame(options.ipc_dir / "frame.bin",
                               static_cast<const uint32_t*>(top_raw),
                               static_cast<const uint32_t*>(bottom_raw),
                               ++live_frame_id,
                               1);
            }
            next_present += std::chrono::milliseconds(16);
            std::this_thread::sleep_until(next_present);
        }
    }

    if (!nds->GPU.GetFramebuffers(&top_raw, &bottom_raw) || !top_raw || !bottom_raw)
    {
        std::cerr << "[melonds_runner] Aucun framebuffer disponible apres execution.\n";
        return 1;
    }

    auto* top = static_cast<const uint32_t*>(top_raw);
    auto* bottom = static_cast<const uint32_t*>(bottom_raw);
    const uint64_t top_hash = HashFramebuffer(top, 256 * 192);
    const uint64_t bottom_hash = HashFramebuffer(bottom, 256 * 192);

    if (options.live)
    {
        WriteLiveFrame(options.ipc_dir / "frame.bin", top, bottom, live_frame_id, 0);
    }

    if (!WriteCombinedPPM(ctx.dump_path, top, bottom))
    {
        std::cerr << "[melonds_runner] Echec dump PPM: " << ctx.dump_path << "\n";
        return 1;
    }

    std::cout
        << "[melonds_runner] ROM       : " << ctx.rom_path << "\n"
        << "[melonds_runner] Save      : " << ctx.nds_save_path << "\n"
        << "[melonds_runner] Firmware  : " << ctx.firmware_save_path << "\n"
        << "[melonds_runner] Frames    : " << ran_frames << "/" << options.frames << "\n"
        << "[melonds_runner] Boot      : " << (use_direct_boot ? "direct" : "firmware") << "\n"
        << "[melonds_runner] Top hash  : 0x" << std::hex << std::setw(16) << std::setfill('0') << top_hash << "\n"
        << "[melonds_runner] Bottom    : 0x" << std::hex << std::setw(16) << std::setfill('0') << bottom_hash << "\n"
        << std::dec
        << "[melonds_runner] Dump      : " << ctx.dump_path << "\n";

    return 0;
}
