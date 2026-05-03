#include "melonds_platform_alos.h"

#include "../fs/persist.h"
#include "../fs/ramfs.h"
#include "../process/task.h"
#include "../jack/vm_store.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../../driver/timer.h"
#include "../../driver/serial.h"

#include "../../third_party/melonds/src/Platform.h"
#include "../../third_party/melonds/src/SPI_Firmware.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <utility>

extern "C" {
int task_create(const char *name, void (*entry)(void));
void task_kill(uint32_t pid);
Task *task_current(void);
}

namespace alos::nds {
static InternalRunContext *g_ctx = nullptr;

void InstallPlatformContext(InternalRunContext *ctx) {
    g_ctx = ctx;
}

InternalRunContext *GetPlatformContext() {
    return g_ctx;
}
} // namespace alos::nds

namespace {

using alos::nds::GetPlatformContext;
using alos::nds::InternalRunContext;

constexpr int kMutableBlobSlots = 24;
constexpr int kThreadSlots = 4;

struct MutableBlobSlot {
    bool used = false;
    std::string path;
    uint8_t *data = nullptr;
    uint32_t size = 0;
};

struct ReadBlob {
    const uint8_t *data = nullptr;
    uint32_t size = 0;
    bool writable = false;
};

static MutableBlobSlot g_mutable_blobs[kMutableBlobSlots];
static uint64_t g_start_us = 0;

static void EnsureStartClock() {
    if (g_start_us == 0) {
        g_start_us = (uint64_t)timer_ms() * 1000ull;
    }
}

static std::string SanitizePath(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        out.push_back(c == '\\' ? '/' : c);
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

static std::string DefaultLocalRoot() {
    InternalRunContext *ctx = GetPlatformContext();
    if (ctx && !ctx->local_root.empty()) return ctx->local_root;
    return "/var/melonds";
}

static std::string RomBaseName(const char *rom_name) {
    std::string base = rom_name ? rom_name : "rom";
    std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    if (base.empty()) base = "rom";
    for (char &c : base) {
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            c = '_';
        }
    }
    return base;
}

static bool EnsureDirTree(const std::string &path) {
    std::string norm = SanitizePath(path);
    std::size_t pos = 1;
    if (norm.empty() || norm[0] != '/') return false;
    while ((pos = norm.find('/', pos)) != std::string::npos) {
        std::string dir = norm.substr(0, pos);
        if (!dir.empty()) ramfs_mkdir(dir.c_str());
        pos++;
    }
    return true;
}

static std::string ResolvePath(const std::string &path) {
    std::string norm = SanitizePath(path);
    if (norm.empty()) return DefaultLocalRoot();
    if (!norm.empty() && norm[0] == '/') return norm;
    return DefaultLocalRoot() + "/" + norm;
}

static MutableBlobSlot *FindMutableBlob(const std::string &path) {
    for (auto &slot : g_mutable_blobs) {
        if (slot.used && slot.path == path) return &slot;
    }
    return nullptr;
}

static MutableBlobSlot *TakeMutableBlob(const std::string &path) {
    if (MutableBlobSlot *slot = FindMutableBlob(path)) return slot;
    for (auto &slot : g_mutable_blobs) {
        if (!slot.used) {
            slot.used = true;
            slot.path = path;
            slot.data = nullptr;
            slot.size = 0;
            return &slot;
        }
    }
    return nullptr;
}

static bool StoreMutableBlob(const std::string &path, const void *data, uint32_t size) {
    MutableBlobSlot *slot = TakeMutableBlob(path);
    uint8_t *copy;
    if (!slot) return false;
    copy = nullptr;
    if (size) {
        copy = new (std::nothrow) uint8_t[size];
        if (!copy) return false;
        if (data) std::memcpy(copy, data, size);
    }
    delete[] slot->data;
    slot->data = copy;
    slot->size = size;
    if (path.size() < RAMFS_MAX_PATH && size < RAMFS_MAX_SIZE) {
        EnsureDirTree(path);
        ramfs_create_bytes(path.c_str(), copy ? static_cast<const void *>(copy) : static_cast<const void *>(""), size);
    }
    persist_mark_dirty();
    return true;
}

static bool TryLoadReadBlob(const std::string &path, ReadBlob *out_blob) {
    std::string resolved = ResolvePath(path);
    std::string vm_name = resolved.size() > 1 && resolved[0] == '/' ? resolved.substr(1) : resolved;
    std::size_t slash = resolved.find_last_of('/');
    std::string basename = (slash == std::string::npos) ? resolved : resolved.substr(slash + 1);
    uint32_t size = 0;

    if (MutableBlobSlot *slot = FindMutableBlob(resolved)) {
        if (out_blob) {
            out_blob->data = slot->data;
            out_blob->size = slot->size;
            out_blob->writable = true;
        }
        return true;
    }

    if (RamFSNode *node = ramfs_find(resolved.c_str())) {
        if (!ramfs_is_dir(node)) {
            if (out_blob) {
                out_blob->data = reinterpret_cast<const uint8_t *>(ramfs_data(node));
                out_blob->size = node->size;
                out_blob->writable = node->type == RAMFS_NODE_FILE;
            }
            return true;
        }
    }

    const char *vm_blob = vm_store_find(resolved.c_str(), &size);
    if (!vm_blob || size == 0) vm_blob = vm_store_find(vm_name.c_str(), &size);
    if ((!vm_blob || size == 0) && !basename.empty()) vm_blob = vm_store_find(basename.c_str(), &size);
    if (!vm_blob || size == 0) return false;

    if (out_blob) {
        out_blob->data = reinterpret_cast<const uint8_t *>(vm_blob);
        out_blob->size = size;
        out_blob->writable = false;
    }
    return true;
}

static const char *LogPrefix(melonDS::Platform::LogLevel level) {
    using namespace melonDS::Platform;
    switch (level) {
        case Debug: return "DBG";
        case Info:  return "INF";
        case Warn:  return "WRN";
        case Error: return "ERR";
        default:    return "LOG";
    }
}

static bool FlushFileBuffer(const std::string &path, const uint8_t *data, uint32_t size) {
    if (path.empty()) return false;
    EnsureDirTree(path);
    return StoreMutableBlob(path, data, size);
}

} // namespace

void alos::nds::FillDefaultPlatformPaths(InternalRunContext *ctx, const char *rom_name) {
    if (!ctx) return;
    ctx->rom_name = rom_name;
    if (ctx->local_root.empty()) ctx->local_root = "/var/melonds";
    std::string base = RomBaseName(rom_name);
    ctx->nds_save_path = ctx->local_root + "/saves/" + base + ".sav";
    ctx->gba_save_path = ctx->local_root + "/saves/" + base + ".gba.sav";
    ctx->firmware_path = ctx->local_root + "/firmware/runtime.bin";
    EnsureDirTree(ctx->nds_save_path);
    EnsureDirTree(ctx->gba_save_path);
    EnsureDirTree(ctx->firmware_path);
}

namespace melonDS::Platform {

struct FileHandle {
    std::string path;
    uint8_t *buffer = nullptr;
    uint32_t size = 0;
    uint32_t capacity = 0;
    uint32_t pos = 0;
    bool writable = false;
    bool dirty = false;
};

struct Thread {
    explicit Thread(std::function<void()> fn) : func(std::move(fn)) {}
    std::function<void()> func;
    volatile int started = 0;
    volatile int finished = 0;
    int slot = -1;
    uint32_t pid = 0;
};

static Thread *g_threads[kThreadSlots] = {};

static int AllocateThreadSlot() {
    for (int i = 0; i < kThreadSlots; ++i) {
        if (!g_threads[i]) return i;
    }
    return -1;
}

static void ThreadEntryCommon(int slot);
static void ThreadEntry0(void) { ThreadEntryCommon(0); }
static void ThreadEntry1(void) { ThreadEntryCommon(1); }
static void ThreadEntry2(void) { ThreadEntryCommon(2); }
static void ThreadEntry3(void) { ThreadEntryCommon(3); }

static void (*const kThreadEntries[kThreadSlots])(void) = {
    ThreadEntry0,
    ThreadEntry1,
    ThreadEntry2,
    ThreadEntry3,
};

struct Semaphore {
    int count = 0;
};

struct Mutex {
    volatile int locked = 0;
};

struct DynamicLibrary {
    int unused = 0;
};

struct AACDecoder {
    int unused = 0;
};

void SignalStop(StopReason reason, void *userdata) {
    InternalRunContext *ctx = static_cast<InternalRunContext *>(userdata);
    if (!ctx) ctx = GetPlatformContext();
    if (!ctx) return;
    ctx->stop_reason = static_cast<int>(reason);
    ctx->stop_requested = 1;
}

std::string GetLocalFilePath(const std::string &filename) {
    return ResolvePath(filename);
}

FileHandle *OpenFile(const std::string &path, FileMode mode) {
    ReadBlob existing;
    std::string resolved = ResolvePath(path);
    bool want_write = (mode & FileMode::Write) != 0;
    bool want_append = (mode & FileMode::Append) != 0;
    bool want_preserve = (mode & FileMode::Preserve) != 0;
    bool no_create = (mode & FileMode::NoCreate) != 0;
    bool exists = TryLoadReadBlob(resolved, &existing);
    uint32_t initial_size = 0;
    uint8_t *initial = nullptr;
    auto *handle = new (std::nothrow) FileHandle();

    if (!handle) return nullptr;
    handle->path = resolved;
    handle->writable = want_write || want_append;

    if (handle->writable) {
        if (!exists && no_create) {
            delete handle;
            return nullptr;
        }
        if (exists && (want_preserve || (mode & FileMode::Read) != 0 || want_append)) {
            initial_size = existing.size;
        }
        handle->capacity = initial_size ? initial_size : 64;
        initial = new (std::nothrow) uint8_t[handle->capacity];
        if (!initial) {
            delete handle;
            return nullptr;
        }
        if (initial_size && existing.data) std::memcpy(initial, existing.data, initial_size);
        handle->buffer = initial;
        handle->size = initial_size;
        handle->pos = want_append ? handle->size : 0;
        handle->dirty = !exists && (want_write || want_append);
        return handle;
    }

    if (!exists) {
        delete handle;
        return nullptr;
    }
    if (existing.size) {
        initial = new (std::nothrow) uint8_t[existing.size];
        if (!initial) {
            delete handle;
            return nullptr;
        }
        std::memcpy(initial, existing.data, existing.size);
    }
    handle->buffer = initial;
    handle->size = existing.size;
    handle->capacity = existing.size;
    handle->pos = 0;
    return handle;
}

FileHandle *OpenLocalFile(const std::string &path, FileMode mode) {
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string &name) {
    return TryLoadReadBlob(name, nullptr);
}

bool LocalFileExists(const std::string &name) {
    return TryLoadReadBlob(GetLocalFilePath(name), nullptr);
}

bool CheckFileWritable(const std::string &filepath) {
    std::string resolved = ResolvePath(filepath);
    EnsureDirTree(resolved);
    return true;
}

bool CheckLocalFileWritable(const std::string &filepath) {
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle *file) {
    bool ok = true;
    if (!file) return false;
    if (file->writable && file->dirty) {
        ok = FlushFileBuffer(file->path, file->buffer, file->size);
    }
    delete[] file->buffer;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle *file) {
    return !file || file->pos >= file->size;
}

bool FileReadLine(char *str, int count, FileHandle *file) {
    int i = 0;
    if (!str || count <= 0 || !file) return false;
    if (IsEndOfFile(file)) return false;
    while (i + 1 < count && file->pos < file->size) {
        char c = static_cast<char>(file->buffer[file->pos++]);
        str[i++] = c;
        if (c == '\n') break;
    }
    str[i] = '\0';
    return i > 0;
}

u64 FilePosition(FileHandle *file) {
    return file ? file->pos : 0;
}

bool FileSeek(FileHandle *file, s64 offset, FileSeekOrigin origin) {
    s64 next;
    if (!file) return false;
    switch (origin) {
        case FileSeekOrigin::Start: next = offset; break;
        case FileSeekOrigin::Current: next = static_cast<s64>(file->pos) + offset; break;
        case FileSeekOrigin::End: next = static_cast<s64>(file->size) + offset; break;
        default: return false;
    }
    if (next < 0) next = 0;
    if (static_cast<u64>(next) > file->size) next = static_cast<s64>(file->size);
    file->pos = static_cast<uint32_t>(next);
    return true;
}

void FileRewind(FileHandle *file) {
    if (file) file->pos = 0;
}

u64 FileRead(void *data, u64 size, u64 count, FileHandle *file) {
    u64 bytes;
    u64 avail;
    if (!file || !data || size == 0 || count == 0) return 0;
    bytes = size * count;
    avail = (file->pos < file->size) ? (file->size - file->pos) : 0;
    if (bytes > avail) bytes = avail - (avail % size);
    if (bytes == 0) return 0;
    std::memcpy(data, file->buffer + file->pos, static_cast<size_t>(bytes));
    file->pos += static_cast<uint32_t>(bytes);
    return bytes / size;
}

bool FileFlush(FileHandle *file) {
    if (!file) return false;
    if (!file->writable || !file->dirty) return true;
    if (!FlushFileBuffer(file->path, file->buffer, file->size)) return false;
    file->dirty = false;
    return true;
}

u64 FileWrite(const void *data, u64 size, u64 count, FileHandle *file) {
    u64 bytes;
    uint64_t need;
    uint32_t new_cap;
    uint8_t *new_buf;
    if (!file || !file->writable || !data || size == 0 || count == 0) return 0;
    bytes = size * count;
    need = static_cast<uint64_t>(file->pos) + bytes;
    if (need > file->capacity) {
        new_cap = file->capacity ? file->capacity : 64u;
        while (new_cap < need) new_cap *= 2u;
        new_buf = new (std::nothrow) uint8_t[new_cap];
        if (!new_buf) return 0;
        if (file->buffer && file->size) std::memcpy(new_buf, file->buffer, file->size);
        delete[] file->buffer;
        file->buffer = new_buf;
        file->capacity = new_cap;
    }
    std::memcpy(file->buffer + file->pos, data, static_cast<size_t>(bytes));
    file->pos += static_cast<uint32_t>(bytes);
    if (file->pos > file->size) file->size = file->pos;
    file->dirty = true;
    return count;
}

u64 FileWriteFormatted(FileHandle *file, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int len;
    if (!file || !fmt) return 0;
    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0) return 0;
    return FileWrite(buf, 1, static_cast<u64>(len), file);
}

u64 FileLength(FileHandle *file) {
    return file ? file->size : 0;
}

void Log(LogLevel level, const char *fmt, ...) {
    char msg[512];
    char line[576];
    va_list ap;
    if (!fmt) return;

    // The internal DS core emits a lot of Info/Debug traces during boot.
    // Dumping those through the kernel text console in graphics mode tanks
    // performance badly, so keep only the important diagnostics.
    if (level != Warn && level != Error) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ksprintf(line, "[melonDS:%s] %s", LogPrefix(level), msg);
    if (serial_is_ready()) {
        serial_write(line);
    }
}

static void ThreadEntryCommon(int slot) {
    Thread *thread;

    if (slot < 0 || slot >= kThreadSlots) {
        task_kill(task_current()->pid);
        for (;;) __asm__ volatile ("hlt");
    }

    thread = g_threads[slot];
    if (!thread) {
        task_kill(task_current()->pid);
        for (;;) __asm__ volatile ("hlt");
    }

    thread->started = 1;
    if (thread->func) thread->func();
    thread->finished = 1;
    g_threads[slot] = nullptr;
    task_kill(task_current()->pid);
    for (;;) __asm__ volatile ("hlt");
}

void Thread_Wait(Thread *thread);

Thread *Thread_Create(std::function<void()> func) {
    auto *thread = new (std::nothrow) Thread(std::move(func));
    int slot;
    int pid;

    if (!thread) return nullptr;

    slot = AllocateThreadSlot();
    if (slot < 0) {
        delete thread;
        return nullptr;
    }

    thread->slot = slot;
    g_threads[slot] = thread;
    pid = task_create("nds-th", kThreadEntries[slot]);
    if (pid < 0) {
        g_threads[slot] = nullptr;
        delete thread;
        return nullptr;
    }

    thread->pid = (uint32_t)pid;
    return thread;
}

void Thread_Free(Thread *thread) {
    if (!thread) return;
    Thread_Wait(thread);
    if (thread->slot >= 0 && thread->slot < kThreadSlots && g_threads[thread->slot] == thread) {
        g_threads[thread->slot] = nullptr;
    }
    delete thread;
}

void Thread_Wait(Thread *thread) {
    if (!thread) return;
    while (!thread->finished) {
        __asm__ volatile ("hlt");
    }
}

Semaphore *Semaphore_Create() {
    return new (std::nothrow) Semaphore();
}

void Semaphore_Free(Semaphore *sema) {
    delete sema;
}

void Semaphore_Reset(Semaphore *sema) {
    if (sema) sema->count = 0;
}

void Semaphore_Wait(Semaphore *sema) {
    if (!sema) return;
    while (sema->count <= 0) {
        __asm__ volatile ("hlt");
    }
    sema->count--;
}

bool Semaphore_TryWait(Semaphore *sema, int timeout_ms) {
    uint32_t start;
    if (!sema) return false;
    if (sema->count > 0) {
        sema->count--;
        return true;
    }
    if (timeout_ms == 0) return false;
    start = timer_ms();
    while ((timer_ms() - start) < static_cast<uint32_t>(timeout_ms)) {
        if (sema->count > 0) {
            sema->count--;
            return true;
        }
        __asm__ volatile ("hlt");
    }
    return false;
}

void Semaphore_Post(Semaphore *sema, int count) {
    if (!sema || count <= 0) return;
    sema->count += count;
}

Mutex *Mutex_Create() {
    return new (std::nothrow) Mutex();
}

void Mutex_Free(Mutex *mutex) {
    delete mutex;
}

void Mutex_Lock(Mutex *mutex) {
    if (!mutex) return;
    while (__sync_lock_test_and_set(&mutex->locked, 1)) {
        __asm__ volatile ("hlt");
    }
}

void Mutex_Unlock(Mutex *mutex) {
    if (!mutex) return;
    __sync_lock_release(&mutex->locked);
}

bool Mutex_TryLock(Mutex *mutex) {
    if (!mutex) return false;
    return __sync_lock_test_and_set(&mutex->locked, 1) == 0;
}

void Sleep(u64 usecs) {
    uint64_t target;
    EnsureStartClock();
    target = GetUSCount() + usecs;
    while (GetUSCount() < target) {
        __asm__ volatile ("hlt");
    }
}

u64 GetMSCount() {
    EnsureStartClock();
    return static_cast<u64>(timer_ms());
}

u64 GetUSCount() {
    EnsureStartClock();
    return static_cast<u64>(timer_ms()) * 1000ull;
}

void WriteNDSSave(const u8 *savedata, u32 savelen, u32, u32, void *userdata) {
    InternalRunContext *ctx = static_cast<InternalRunContext *>(userdata);
    if (!ctx) ctx = GetPlatformContext();
    if (!ctx || ctx->nds_save_path.empty() || !savedata) return;
    StoreMutableBlob(ctx->nds_save_path, savedata, savelen);
}

void WriteGBASave(const u8 *savedata, u32 savelen, u32, u32, void *userdata) {
    InternalRunContext *ctx = static_cast<InternalRunContext *>(userdata);
    if (!ctx) ctx = GetPlatformContext();
    if (!ctx || ctx->gba_save_path.empty() || !savedata) return;
    StoreMutableBlob(ctx->gba_save_path, savedata, savelen);
}

void WriteFirmware(const Firmware &firmware, u32, u32, void *userdata) {
    InternalRunContext *ctx = static_cast<InternalRunContext *>(userdata);
    if (!ctx) ctx = GetPlatformContext();
    if (!ctx || ctx->firmware_path.empty() || !firmware.Buffer()) return;
    StoreMutableBlob(ctx->firmware_path, firmware.Buffer(), firmware.Length());
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void *) {
    (void)year;
    (void)month;
    (void)day;
    (void)hour;
    (void)minute;
    (void)second;
}

void MP_Begin(void *) {}
void MP_End(void *) {}
int MP_SendPacket(u8 *, int, u64, void *) { return 0; }
int MP_RecvPacket(u8 *, u64 *, void *) { return 0; }
int MP_SendCmd(u8 *, int, u64, void *) { return 0; }
int MP_SendReply(u8 *, int, u64, u16, void *) { return 0; }
int MP_SendAck(u8 *, int, u64, void *) { return 0; }
int MP_RecvHostPacket(u8 *, u64 *, void *) { return 0; }
u16 MP_RecvReplies(u8 *, u64, u16, void *) { return 0; }

int Net_SendPacket(u8 *, int, void *) { return 0; }
int Net_RecvPacket(u8 *, void *) { return 0; }

void Camera_Start(int, void *) {}
void Camera_Stop(int, void *) {}
void Camera_CaptureFrame(int, u32 *frame, int width, int height, bool, void *) {
    if (!frame || width <= 0 || height <= 0) return;
    std::memset(frame, 0, static_cast<size_t>(width * height) * sizeof(u32));
}

void Mic_Start(void *) {}
void Mic_Stop(void *) {}
int Mic_ReadInput(s16 *data, int maxlength, void *) {
    if (data && maxlength > 0) {
        std::memset(data, 0, static_cast<size_t>(maxlength) * sizeof(s16));
    }
    return 0;
}

AACDecoder *AAC_Init() {
    return nullptr;
}

void AAC_DeInit(AACDecoder *) {}
bool AAC_Configure(AACDecoder *, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder *, const void *, int, void *, int) { return false; }

bool Addon_KeyDown(KeyType, void *) { return false; }
void Addon_RumbleStart(u32, void *) {}
void Addon_RumbleStop(void *) {}
float Addon_MotionQuery(MotionQueryType, void *) { return 0.0f; }

DynamicLibrary *DynamicLibrary_Load(const char *) {
    return nullptr;
}

void DynamicLibrary_Unload(DynamicLibrary *lib) {
    delete lib;
}

void *DynamicLibrary_LoadFunction(DynamicLibrary *, const char *) {
    return nullptr;
}

} // namespace melonDS::Platform
