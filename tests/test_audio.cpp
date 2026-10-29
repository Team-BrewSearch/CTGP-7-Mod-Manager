#include "support/test_support.h"
#include "backend/audio.h"
#include "backend/sd_card.h"
#include <algorithm>
#include <cstring>

namespace {
namespace audio = mm::audio;
std::deque<long> reads;
vorbis_info info;
std::vector<ndspWaveBuf *> queued;
std::vector<u32> sample_counts, flush_sizes;
std::size_t stop_after = 1;
bool finish_immediately = true;
int init_calls = 0, exit_calls = 0, resets = 0;
Result init_result = 0;
std::FILE *opened = nullptr;
struct Fixture {
  test::Sandbox sandbox;
  Fixture() {
    CHECK(mm::sd::init_paths());
    reads.clear(); queued.clear(); sample_counts.clear(); flush_sizes.clear();
    info = {1, 32768};
    stop_after = 1;
    finish_immediately = true;
    init_calls = exit_calls = resets = 0;
    init_result = 0;
    audio::ndsp_ready = false;
    audio::should_stop = false;
    audio::loop_mode = false;
    for (std::size_t i = 0; i < audio::wave_memory.size(); ++i) {
      audio::wave_memory[i] = mm::sys::LinearBuffer{mm::cfg::AUDIO_BUF_BYTES};
      CHECK(bool(audio::wave_memory[i]));
      audio::wave_buffers[i].data_pcm16 = audio::wave_memory[i].as<s16>();
      audio::wave_buffers[i].status = NDSP_WBUF_DONE;
    }
  }
  ~Fixture() { audio::shutdown(); }
};
}
Result opendspInit() { ++init_calls; return init_result; }
void opendspExit() { ++exit_calls; }
void ndspChnReset(int) { ++resets; }
void ndspChnSetFormat(int, int) {}
void ndspChnSetRate(int, float) {}
void ndspChnSetMix(int, float *) {}
void ndspSetOutputMode(int) {}
void ndspSetMasterVol(float) {}
void DSP_FlushDataCache(void *, u32 size) { flush_sizes.push_back(size); }
void ndspChnWaveBufAdd(int, ndspWaveBuf *buffer) {
  queued.push_back(buffer);
  sample_counts.push_back(buffer->nsamples);
  buffer->status = finish_immediately ? NDSP_WBUF_DONE : NDSP_WBUF_QUEUED;
  if (queued.size() >= stop_after) audio::should_stop = true;
}
int ov_clear(OggVorbis_File *) {
  if (opened) { std::fclose(opened); opened = nullptr; }
  return 0;
}
int ov_open_callbacks(void *file, OggVorbis_File *, const char *, long, ov_callbacks) {
  opened = static_cast<std::FILE *>(file);
  return 0;
}
vorbis_info *ov_info(OggVorbis_File *, int) { return &info; }
long ov_read(OggVorbis_File *, char *out, int capacity, int *) {
  CHECK(!reads.empty());
  const long result = reads.front();
  reads.pop_front();
  CHECK(result <= capacity);
  if (result > 0) std::memset(out, 1, std::size_t(result));
  return result;
}
int ov_raw_seek(OggVorbis_File *, ogg_int64_t) { return 0; }

TEST(audio_play_resets_every_wave_buffer_when_changing_streams) {
  Fixture fixture;
  for (auto &buffer : audio::wave_buffers) buffer.status = NDSP_WBUF_QUEUED;
  audio::play(true);
  CHECK(audio::loop_mode);
  CHECK(std::ranges::all_of(audio::wave_buffers, [](const auto &buffer) { return buffer.status == NDSP_WBUF_DONE; }));
}
TEST(audio_cycles_through_all_configured_buffers) {
  Fixture fixture;
  stop_after = mm::cfg::AUDIO_BUFFERS + 1;
  for (std::size_t i = 0; i < stop_after; ++i) reads.push_back(mm::cfg::AUDIO_BUF_SAMPLES * sizeof(s16));
  audio::thread_main(nullptr);
  CHECK(queued.size() == stop_after);
  for (std::size_t i = 0; i < queued.size(); ++i) CHECK(queued[i] == &audio::wave_buffers[i % mm::cfg::AUDIO_BUFFERS]);
}
TEST(audio_partial_eof_queues_only_decoded_mono_samples) {
  Fixture fixture;
  reads = {8, 0};
  audio::thread_main(nullptr);
  CHECK(sample_counts == std::vector<u32>({4}));
  CHECK(flush_sizes == std::vector<u32>({8}));
}
TEST(audio_partial_stereo_eof_flushes_only_decoded_bytes) {
  Fixture fixture;
  info.channels = 2;
  reads = {16, 0};
  audio::thread_main(nullptr);
  // nsamples counts PCM16 halfwords, including both channels. Cache flushing
  // must cover those bytes exactly once, without another channel multiplier.
  CHECK(sample_counts == std::vector<u32>({8}));
  CHECK(flush_sizes == std::vector<u32>({16}));
}
TEST(audio_empty_eof_never_submits_a_zero_length_buffer) {
  Fixture fixture;
  reads = {0};
  audio::thread_main(nullptr);
  CHECK(queued.empty());
  CHECK(flush_sizes.empty());
}
TEST(audio_waits_for_intro_completion_before_reconfiguring_loop_playback) {
  Fixture fixture;
  test::write("romfs:/loop.ogg", "synthetic stream");
  reads = {8, 0};
  finish_immediately = false;
  stop_after = 100;
  test::on_sleep = [] { audio::should_stop = true; };
  audio::thread_main(nullptr);
  CHECK(test::sleeps == std::vector<std::int64_t>({5'000'000}));
  CHECK(resets == 1);
  CHECK(!audio::loop_mode);
}
TEST(audio_initializes_and_shuts_down_opendsp) {
  Fixture fixture;
  test::write("romfs:/intro.ogg", "synthetic stream");
  reads = {16};
  audio::init();
  CHECK(init_calls == 1);
  CHECK(audio::ndsp_ready);
  CHECK(audio::wave_buffers.size() == mm::cfg::AUDIO_BUFFERS);
  for (const auto &buffer : audio::wave_buffers) CHECK(buffer.data_pcm16 != nullptr);
  audio::shutdown();
  CHECK(exit_calls == 1);
  CHECK(!audio::ndsp_ready);
  for (const auto &memory : audio::wave_memory) CHECK(!bool(memory));
  audio::shutdown();
  CHECK(exit_calls == 1);
}
TEST(audio_failed_opendsp_initialization_does_not_call_exit) {
  Fixture fixture;
  init_result = -1;
  audio::init();
  CHECK(init_calls == 1);
  CHECK(!audio::ndsp_ready);
  audio::shutdown();
  CHECK(exit_calls == 0);
}
