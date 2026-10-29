#pragma once
#include "core/config.h"
#include "core/system.h"
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <array>
#include <cstddef>
namespace mm {
namespace thumbs {
extern Tex3DS_SubTexture sub_texture;
void build_sub_texture() noexcept;
struct TextureSlot {
  C3D_Tex texture{};
  int mod_id = 0;
  u32 last_used = 0;
};
struct Request {
  int mod_id = 0;
  std::array<char, cfg::THUMB_URL_MAX> url{};
};
struct Publication {
  sys::LinearBuffer pixels;
  sys::Event ack{true};
  int mod_id = 0;
  bool ready = false;
  bool ok = false;
};
struct WorkerContext {
  int index = 0;
};
extern sys::Mutex lock;
extern sys::Event wake;
extern std::array<TextureSlot, cfg::THUMB_SLOTS> slots;
extern std::array<Publication, cfg::THUMB_WORKERS> publications;
extern std::array<sys::Thread, cfg::THUMB_WORKERS> workers;
extern std::array<WorkerContext, cfg::THUMB_WORKERS> worker_contexts;
extern std::array<Request, cfg::THUMB_QUEUE> queue;
extern int queue_size;
extern std::array<int, cfg::THUMB_WORKERS> in_flight;
extern std::array<std::pair<int, int>, cfg::THUMB_FAIL_RING> failures;
extern int failure_cursor;
extern std::array<int, cfg::CARDS_PER_PAGE> wanted;
extern int wanted_count;
extern u32 frame_counter;
extern bool ready;
[[nodiscard]] bool is_wanted(int mod_id) noexcept;
[[nodiscard]] bool is_resident(int mod_id) noexcept;
/** Return whether the failure ring contains mod_id; zero is always treated as failed. */
[[nodiscard]] bool has_failed(int mod_id) noexcept;
/**
 * Record a nonzero mod failure, capping its count at THUMB_FAIL_RETRY.
 * New IDs overwrite the oldest ring slot when full; zero is ignored.
 */
void mark_failed(int mod_id) noexcept;
/**
 * Remove failure records whose count reached THUMB_FAIL_RETRY, allowing requeueing.
 * Keep records with lower counts; callers must coordinate access to failure state.
 */
void evict_stale_failures() noexcept;
[[nodiscard]] bool is_in_flight(int mod_id) noexcept;
[[nodiscard]] TextureSlot *pick_slot() noexcept;
void rebuild_wanted() noexcept;
void rebuild_queue() noexcept;
/**
 * Produce queued thumbnails using the WorkerContext at argument until quit is requested.
 * Publish each success or failure and wait for UI acknowledgement before reusing pixels.
 */
void worker_main(void *argument);
/**
 * Consume thumbnail publications, update textures and failures, and queue visible requests.
 * Periodically evict failure records at the retry threshold. Call on the render
 * thread; do nothing until initialized.
 */
void tick();
/** Return whether initialized workers have an unconsumed publication, including failures. */
[[nodiscard]] bool has_arrivals();
[[nodiscard]] C3D_Tex *texture_for(int mod_id) noexcept;
bool init();
void shutdown();
}
}
