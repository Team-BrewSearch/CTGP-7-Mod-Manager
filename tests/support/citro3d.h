#pragma once
#include "3ds.h"
struct C3D_Tex { void *data = nullptr; };
inline constexpr int GPU_RGB565 = 0, GPU_LINEAR = 0;
inline bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, int) {
  tex->data = std::calloc(std::size_t(width) * height, 2);
  return tex->data != nullptr;
}
inline void C3D_TexSetFilter(C3D_Tex *, int, int) {}
inline void C3D_TexFlush(C3D_Tex *) {}
inline void C3D_TexDelete(C3D_Tex *tex) {
  std::free(tex->data);
  tex->data = nullptr;
}
