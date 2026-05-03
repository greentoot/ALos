#include <stddef.h>
#include <stdint.h>
#include <new>

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);
extern "C" void abort(void);

extern "C" {
void *__dso_handle = &__dso_handle;
}

void *operator new(size_t size) {
    if (size == 0) size = 1;
    void *p = malloc(size);
    if (p) return p;
    abort();
    __builtin_unreachable();
}

void *operator new[](size_t size) {
    if (size == 0) size = 1;
    void *p = malloc(size);
    if (p) return p;
    abort();
    __builtin_unreachable();
}

void operator delete(void *ptr) noexcept {
    if (ptr) free(ptr);
}

void operator delete[](void *ptr) noexcept {
    if (ptr) free(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    if (ptr) free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept {
    if (ptr) free(ptr);
}

void *operator new(size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    return malloc(size);
}

void *operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    return malloc(size);
}

void operator delete(void *ptr, const std::nothrow_t&) noexcept {
    if (ptr) free(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t&) noexcept {
    if (ptr) free(ptr);
}

extern "C" void __cxa_pure_virtual(void) {
    abort();
}

extern "C" void __cxa_deleted_virtual(void) {
    abort();
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *) {
    return 0;
}

extern "C" void __cxa_finalize(void *) {
}

extern "C" int __cxa_guard_acquire(uint64_t *guard) {
    return (guard && !(*guard & 1u)) ? 1 : 0;
}

extern "C" void __cxa_guard_release(uint64_t *guard) {
    if (guard) *guard |= 1u;
}

extern "C" void __cxa_guard_abort(uint64_t *guard) {
    if (guard) *guard &= ~1u;
}

extern "C" void *__cxa_begin_catch(void *exc) {
    return exc;
}

extern "C" void __cxa_end_catch(void) {
}

extern "C" void __cxa_throw(void *, void *, void (*)(void *)) {
    abort();
}

extern "C" void __cxa_rethrow(void) {
    abort();
}

extern "C" void __cxa_call_unexpected(void *) {
    abort();
}

extern "C" int __gxx_personality_v0(...) {
    return 0;
}

extern "C" void _Unwind_Resume(void *) {
    abort();
}

extern "C" int _Unwind_Resume_or_Rethrow(void *) {
    return 0;
}

extern "C" int _Unwind_DeleteException(void *) {
    return 0;
}

namespace std {
const nothrow_t nothrow = nothrow_t();

[[noreturn]] void __throw_bad_alloc() { abort(); }
[[noreturn]] void __throw_bad_array_new_length() { abort(); }
[[noreturn]] void __throw_length_error(const char *) { abort(); }
[[noreturn]] void __throw_logic_error(const char *) { abort(); }
[[noreturn]] void __throw_out_of_range(const char *) { abort(); }
[[noreturn]] void __throw_out_of_range_fmt(const char *, ...) { abort(); }
[[noreturn]] void __throw_invalid_argument(const char *) { abort(); }
[[noreturn]] void __throw_domain_error(const char *) { abort(); }
[[noreturn]] void __throw_runtime_error(const char *) { abort(); }
[[noreturn]] void __throw_range_error(const char *) { abort(); }
[[noreturn]] void __throw_overflow_error(const char *) { abort(); }
[[noreturn]] void __throw_underflow_error(const char *) { abort(); }
[[noreturn]] void __throw_bad_function_call() { abort(); }
}
