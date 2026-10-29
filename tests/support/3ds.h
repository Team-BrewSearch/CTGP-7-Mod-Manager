#pragma once
// Host adapter: exercise production code without a 3DS or background workers.
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using Result = std::int32_t;
constexpr bool R_FAILED(Result result) { return result < 0; }
inline constexpr u64 U64_MAX = std::numeric_limits<u64>::max();
using LightLock = std::mutex;
using RecursiveLock = std::recursive_mutex;
inline void LightLock_Init(LightLock *) {}
inline void LightLock_Lock(LightLock *lock) { lock->lock(); }
inline void LightLock_Unlock(LightLock *lock) { lock->unlock(); }
inline int LightLock_TryLock(LightLock *lock) { return lock->try_lock() ? 0 : 1; }
inline void RecursiveLock_Init(RecursiveLock *) {}
inline void RecursiveLock_Lock(RecursiveLock *lock) { lock->lock(); }
inline void RecursiveLock_Unlock(RecursiveLock *lock) { lock->unlock(); }
struct LightEvent {};
inline constexpr int RESET_STICKY = 0, RESET_ONESHOT = 1;
inline void LightEvent_Init(LightEvent *, int) {}
inline void LightEvent_Clear(LightEvent *) {}
inline void LightEvent_Signal(LightEvent *) {}
inline void LightEvent_WaitTimeout(LightEvent *, s64) { std::abort(); }
using Thread = void *;
using ThreadFunc = void (*)(void *);
// net::run_jobs has a synchronous fallback; persistent workers must not start.
inline Thread threadCreate(ThreadFunc, void *, std::size_t, int, int, bool) { return nullptr; }
inline void threadJoin(Thread, u64) {}
inline void threadFree(Thread) {}
inline void *linearAlloc(std::size_t bytes) { return std::malloc(bytes); }
inline void linearFree(void *data) { std::free(data); }
void svcSleepThread(s64 nanoseconds);
inline void GSPGPU_FlushDataCache(void *, std::size_t) {}
inline constexpr u32 KEY_DUP = 1u << 0, KEY_DDOWN = 1u << 1;
inline constexpr u32 KEY_DLEFT = 1u << 2, KEY_DRIGHT = 1u << 3;
inline constexpr u32 KEY_CPAD_UP = 1u << 4, KEY_CPAD_DOWN = 1u << 5;
inline constexpr u32 KEY_CPAD_LEFT = 1u << 6, KEY_CPAD_RIGHT = 1u << 7;
