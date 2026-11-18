/* SPDX-License-Identifier: Apache-2.0
 * Implementation unit for miniaudio (public domain / MIT-0, fetched by CMake) with stb_vorbis
 * (public domain / MIT, from miniaudio's extras) for Ogg Vorbis decoding. Built with third-party
 * warning settings; VICTUS code only includes miniaudio.h. victus_stb_vorbis.c is stb_vorbis v1.22
 * with VICTUS's comment-header fixes, generated at configure time (cmake/VictusDependencies.cmake). */
#define STB_VORBIS_HEADER_ONLY
#include "victus_stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "victus_stb_vorbis.c"
