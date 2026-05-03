#include "platform_minimal.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "Platform.h"
#include "SPI_Firmware.h"

namespace alos::melonds_min
{
static RunContext* g_ctx = nullptr;

void InstallPlatformContext(RunContext* ctx)
{
    g_ctx = ctx;
}

RunContext* GetPlatformContext()
{
    return g_ctx;
}
}

namespace
{
namespace fs = std::filesystem;

using alos::melonds_min::GetPlatformContext;
using alos::melonds_min::RunContext;

fs::path ResolveLocalPath(const std::string& path)
{
    fs::path input(path);
    if (input.is_absolute())
        return input;

    if (RunContext* ctx = GetPlatformContext())
    {
        if (!ctx->local_root.empty())
            return ctx->local_root / input;
        if (!ctx->project_root.empty())
            return ctx->project_root / input;
    }

    return fs::current_path() / input;
}

bool EnsureParentDir(const fs::path& path)
{
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (parent.empty())
        return true;
    if (fs::exists(parent))
        return true;
    return fs::create_directories(parent, ec);
}

bool WriteWholeFile(const fs::path& path, const void* data, size_t size)
{
    if (!EnsureParentDir(path))
        return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

std::string BuildMode(unsigned mode, bool exists)
{
    using namespace melonDS::Platform;

    const bool read = (mode & FileMode::Read) != 0;
    const bool write = (mode & FileMode::Write) != 0;
    const bool append = (mode & FileMode::Append) != 0;
    const bool text = (mode & FileMode::Text) != 0;
    const bool preserve = (mode & FileMode::Preserve) != 0;
    const bool no_create = (mode & FileMode::NoCreate) != 0;

    std::string result;
    if (append)
    {
        result = read ? "a+" : "a";
    }
    else if (write)
    {
        if (no_create || (preserve && exists))
            result = "r+";
        else if (read)
            result = "w+";
        else
            result = "w";
    }
    else
    {
        result = "r";
    }

    if (!text)
        result += 'b';

    return result;
}

const char* LogPrefix(melonDS::Platform::LogLevel level)
{
    using namespace melonDS::Platform;

    switch (level)
    {
    case Debug: return "DBG";
    case Info:  return "INF";
    case Warn:  return "WRN";
    case Error: return "ERR";
    default:    return "LOG";
    }
}

const auto kStartClock = std::chrono::steady_clock::now();
}

namespace melonDS::Platform
{
struct FileHandle
{
    std::FILE* stream = nullptr;
    std::string path;
};

struct Thread
{
    explicit Thread(std::function<void()> fn) : worker(std::move(fn)) {}
    std::thread worker;
};

struct Semaphore
{
    std::mutex mutex;
    std::condition_variable cv;
    int count = 0;
};

struct Mutex
{
    std::mutex mutex;
};

struct DynamicLibrary
{
    void* handle = nullptr;
};

struct AACDecoder
{
    int unused = 0;
};

void SignalStop(StopReason reason, void* userdata)
{
    if (auto* ctx = static_cast<RunContext*>(userdata))
    {
        ctx->stop_reason.store(static_cast<int>(reason));
        ctx->stop_requested.store(true);
    }
}

std::string GetLocalFilePath(const std::string& filename)
{
    return ResolveLocalPath(filename).string();
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    fs::path resolved(path);
    if (!resolved.is_absolute())
        resolved = fs::current_path() / resolved;

    const bool exists = fs::exists(resolved);
    if ((mode & FileMode::NoCreate) && !exists && (mode & FileMode::Write))
        return nullptr;

    if ((mode & FileMode::Write) && !EnsureParentDir(resolved))
        return nullptr;

    const std::string mode_str = BuildMode(mode, exists);
    std::FILE* fp = std::fopen(resolved.string().c_str(), mode_str.c_str());
    if (!fp)
        return nullptr;

    auto* handle = new FileHandle();
    handle->stream = fp;
    handle->path = resolved.string();
    return handle;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    return fs::exists(fs::path(name));
}

bool LocalFileExists(const std::string& name)
{
    return fs::exists(ResolveLocalPath(name));
}

bool CheckFileWritable(const std::string& filepath)
{
    fs::path path(filepath);
    if (fs::exists(path))
    {
        auto* file = OpenFile(filepath, FileMode::Append);
        if (!file)
            return false;
        return CloseFile(file);
    }

    if (!EnsureParentDir(path))
        return false;

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
        return false;
    out.close();
    std::error_code ec;
    fs::remove(path, ec);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath)
{
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file)
{
    if (!file)
        return false;

    bool ok = true;
    if (file->stream)
        ok = std::fclose(file->stream) == 0;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle* file)
{
    if (!file || !file->stream)
        return true;
    return std::feof(file->stream) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    if (!file || !file->stream)
        return false;
    return std::fgets(str, count, file->stream) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    if (!file || !file->stream)
        return 0;
    return static_cast<u64>(std::ftell(file->stream));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    if (!file || !file->stream)
        return false;

    int whence = SEEK_SET;
    switch (origin)
    {
    case FileSeekOrigin::Start: whence = SEEK_SET; break;
    case FileSeekOrigin::Current: whence = SEEK_CUR; break;
    case FileSeekOrigin::End: whence = SEEK_END; break;
    }
    return std::fseek(file->stream, static_cast<long>(offset), whence) == 0;
}

void FileRewind(FileHandle* file)
{
    if (file && file->stream)
        std::rewind(file->stream);
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    if (!file || !file->stream)
        return 0;
    return std::fread(data, static_cast<size_t>(size), static_cast<size_t>(count), file->stream);
}

bool FileFlush(FileHandle* file)
{
    if (!file || !file->stream)
        return false;
    return std::fflush(file->stream) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    if (!file || !file->stream)
        return 0;
    return std::fwrite(data, static_cast<size_t>(size), static_cast<size_t>(count), file->stream);
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    if (!file || !file->stream || !fmt)
        return 0;

    va_list args;
    va_start(args, fmt);
    const int written = std::vfprintf(file->stream, fmt, args);
    va_end(args);
    return written < 0 ? 0 : static_cast<u64>(written);
}

u64 FileLength(FileHandle* file)
{
    if (!file || !file->stream)
        return 0;

    const long pos = std::ftell(file->stream);
    std::fseek(file->stream, 0, SEEK_END);
    const long len = std::ftell(file->stream);
    std::fseek(file->stream, pos, SEEK_SET);
    return len < 0 ? 0 : static_cast<u64>(len);
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (!fmt)
        return;

    std::FILE* out = (level == Error || level == Warn) ? stderr : stdout;
    std::fprintf(out, "[melonDS:%s] ", LogPrefix(level));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(out, fmt, args);
    va_end(args);
    std::fflush(out);
}

Thread* Thread_Create(std::function<void()> func)
{
    return new Thread(std::move(func));
}

void Thread_Free(Thread* thread)
{
    if (!thread)
        return;

    if (thread->worker.joinable())
    {
        if (thread->worker.get_id() == std::this_thread::get_id())
            thread->worker.detach();
        else
            thread->worker.join();
    }
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread && thread->worker.joinable())
        thread->worker.join();
}

Semaphore* Semaphore_Create()
{
    return new Semaphore();
}

void Semaphore_Free(Semaphore* sema)
{
    delete sema;
}

void Semaphore_Reset(Semaphore* sema)
{
    if (!sema)
        return;

    std::lock_guard<std::mutex> lock(sema->mutex);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    if (!sema)
        return;

    std::unique_lock<std::mutex> lock(sema->mutex);
    sema->cv.wait(lock, [&] { return sema->count > 0; });
    sema->count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    if (!sema)
        return false;

    std::unique_lock<std::mutex> lock(sema->mutex);
    const auto ready = [&] { return sema->count > 0; };
    if (timeout_ms == 0)
    {
        if (!ready())
            return false;
    }
    else if (!sema->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready))
    {
        return false;
    }

    sema->count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    if (!sema || count <= 0)
        return;

    {
        std::lock_guard<std::mutex> lock(sema->mutex);
        sema->count += count;
    }
    sema->cv.notify_all();
}

Mutex* Mutex_Create()
{
    return new Mutex();
}

void Mutex_Free(Mutex* mutex)
{
    delete mutex;
}

void Mutex_Lock(Mutex* mutex)
{
    if (mutex)
        mutex->mutex.lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    if (mutex)
        mutex->mutex.unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return mutex ? mutex->mutex.try_lock() : false;
}

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - kStartClock).count());
}

u64 GetUSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - kStartClock).count());
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32, u32, void* userdata)
{
    auto* ctx = static_cast<RunContext*>(userdata);
    if (!ctx || ctx->nds_save_path.empty() || !savedata || savelen == 0)
        return;

    std::lock_guard<std::mutex> lock(ctx->io_mutex);
    if (!WriteWholeFile(ctx->nds_save_path, savedata, savelen))
        Log(Error, "Ecriture du save DS echouee: %s\n", ctx->nds_save_path.string().c_str());
}

void WriteGBASave(const u8* savedata, u32 savelen, u32, u32, void* userdata)
{
    auto* ctx = static_cast<RunContext*>(userdata);
    if (!ctx || ctx->gba_save_path.empty() || !savedata || savelen == 0)
        return;

    std::lock_guard<std::mutex> lock(ctx->io_mutex);
    if (!WriteWholeFile(ctx->gba_save_path, savedata, savelen))
        Log(Error, "Ecriture du save GBA echouee: %s\n", ctx->gba_save_path.string().c_str());
}

void WriteFirmware(const Firmware& firmware, u32, u32, void* userdata)
{
    auto* ctx = static_cast<RunContext*>(userdata);
    if (!ctx || ctx->firmware_save_path.empty() || !firmware.Buffer() || firmware.Length() == 0)
        return;

    std::lock_guard<std::mutex> lock(ctx->io_mutex);
    if (!WriteWholeFile(ctx->firmware_save_path, firmware.Buffer(), firmware.Length()))
        Log(Error, "Ecriture du firmware DS echouee: %s\n", ctx->firmware_save_path.string().c_str());
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void*)
{
    Log(Info, "RTC mis a jour: %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);
}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool yuv, void*)
{
    if (!frame)
        return;

    if (yuv)
        std::memset(frame, 0, static_cast<size_t>(width * height / 2) * sizeof(u32));
    else
        std::memset(frame, 0, static_cast<size_t>(width * height) * sizeof(u32));
}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16* data, int maxlength, void*)
{
    if (data && maxlength > 0)
        std::memset(data, 0, static_cast<size_t>(maxlength) * sizeof(s16));
    return 0;
}

AACDecoder* AAC_Init()
{
    return nullptr;
}

void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*)
{
    return false;
}

void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*)
{
    return 0.0f;
}

DynamicLibrary* DynamicLibrary_Load(const char* lib)
{
    if (!lib)
        return nullptr;

    auto* dyn = new DynamicLibrary();
#ifdef _WIN32
    dyn->handle = nullptr;
#else
    dyn->handle = dlopen(lib, RTLD_LAZY);
#endif
    if (!dyn->handle)
    {
        delete dyn;
        return nullptr;
    }
    return dyn;
}

void DynamicLibrary_Unload(DynamicLibrary* lib)
{
    if (!lib)
        return;
#ifndef _WIN32
    if (lib->handle)
        dlclose(lib->handle);
#endif
    delete lib;
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name)
{
    if (!lib || !name)
        return nullptr;
#ifdef _WIN32
    return nullptr;
#else
    return dlsym(lib->handle, name);
#endif
}
}
