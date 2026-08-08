#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

// miniaudio discovers the decoder through STB_VORBIS_INCLUDE_STB_VORBIS_H;
// compile stb_vorbis' implementation after miniaudio has consumed its API.
#include <stb_vorbis.c>
