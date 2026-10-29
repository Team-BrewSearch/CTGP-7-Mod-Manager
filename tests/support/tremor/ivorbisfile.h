#pragma once
#include <cstddef>
#include <cstdint>
using ogg_int64_t = std::int64_t;
struct OggVorbis_File {};
struct vorbis_info { int channels = 1; long rate = 32768; };
struct ov_callbacks {
  std::size_t (*read_func)(void *, std::size_t, std::size_t, void *);
  int (*seek_func)(void *, ogg_int64_t, int);
  int (*close_func)(void *);
  long (*tell_func)(void *);
};
int ov_clear(OggVorbis_File *);
int ov_open_callbacks(void *, OggVorbis_File *, const char *, long, ov_callbacks);
vorbis_info *ov_info(OggVorbis_File *, int);
long ov_read(OggVorbis_File *, char *, int, int *);
int ov_raw_seek(OggVorbis_File *, ogg_int64_t);
