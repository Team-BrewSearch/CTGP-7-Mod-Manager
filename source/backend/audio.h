#pragma once
#include "core/config.h"
#include "core/system.h"
#include <3ds.h>
#include <3ds/ndsp/ndsp.h>
#include <tremor/ivorbisfile.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <span>
namespace mm {
namespace audio {
extern OggVorbis_File stream;
extern bool loop_mode;
extern std::atomic<bool> should_stop;
extern long sample_rate;
extern int channel_count;
extern bool ndsp_ready;
extern std::array<ndspWaveBuf, cfg::AUDIO_BUFFERS> wave_buffers;
extern std::array<sys::LinearBuffer, cfg::AUDIO_BUFFERS> wave_memory;
extern sys::Thread worker;
extern const ov_callbacks CALLBACKS;
std::size_t ogg_read(void *dest, std::size_t size, std::size_t count, void *source);
int ogg_seek(void *source, ogg_int64_t offset, int whence);
int ogg_close(void *source);
long ogg_tell(void *source);
/**
 * Replace the current Vorbis stream with the file at path, closing the old stream.
 * Return false if opening the file or initializing the decoder fails.
 */
[[nodiscard]] bool load(const char *path);
/**
 * Set loop mode and configure DSP channel 0 for the loaded stream.
 * Reset queued buffers when stream information is available; otherwise return
 * without configuring the channel. Audio is queued separately by the worker.
 */
void play(bool loop);
[[nodiscard]] int decode_frames(std::span<s16> destination, bool &reached_end);
/**
 * Stream the loaded intro, then repeat romfs:/loop.ogg until should_stop is set.
 * Stop early if loading the loop or seeking back to its start fails.
 */
void thread_main(void *);
/**
 * Initialize OpenDSP and start the intro playback worker.
 * Return early on DSP, stream, allocation, or thread failure; shutdown releases
 * any resources retained after partial initialization.
 */
void init();
/** Request playback stop, join the worker, and release the DSP, stream, and buffers. */
void shutdown();
}
}
