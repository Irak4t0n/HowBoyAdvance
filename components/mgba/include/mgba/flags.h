#ifndef FLAGS_H
#define FLAGS_H

// Minimal core: strip debugger, input maps, directory scanning
#ifndef MINIMAL_CORE
#define MINIMAL_CORE 2
#endif

// Use 32-bit color natively (we convert to display format ourselves)
// Do NOT define COLOR_16_BIT — we want 32-bit XBGR8 output from mGBA

// Disable threading (we manage our own tasks via FreeRTOS)
#define DISABLE_THREADING

// Only build GBA core, no GB
#define M_CORE_GBA

// Disable all optional features
/* #undef M_CORE_GB */
/* #undef BUILD_GL */
/* #undef BUILD_GLES2 */
/* #undef BUILD_GLES3 */
/* #undef ENABLE_SCRIPTING */
/* #undef USE_DEBUGGERS */
/* #undef USE_EDITLINE */
/* #undef USE_ELF */
/* #undef USE_EPOXY */
/* #undef USE_FFMPEG */
/* #undef USE_GDB_STUB */
/* #undef USE_LIBAV */
/* #undef USE_LIBAVRESAMPLE */
/* #undef USE_LIBSWRESAMPLE */
/* #undef USE_LIBZIP */
/* #undef USE_LUA */
/* #undef USE_LZMA */
/* #undef USE_MINIZIP */
/* #undef USE_PNG */
/* #undef USE_PTHREADS */
/* #undef USE_SQLITE3 */
/* #undef USE_ZLIB */
/* #undef FIXED_ROM_BUFFER */

// HAVE flags for ESP-IDF / newlib
#define HAVE_LOCALTIME_R
#define HAVE_STRDUP
/* #undef HAVE_CRC32 */
/* #undef HAVE_POPCOUNT32 */
/* #undef HAVE_PTHREAD_NP_H */
/* #undef HAVE_PTHREAD_SETNAME_NP */
/* #undef HAVE_STRLCPY */
/* #undef HAVE_STRTOF_L */
/* #undef HAVE_XLOCALE */

#endif
