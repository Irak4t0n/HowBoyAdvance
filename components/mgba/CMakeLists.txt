idf_component_register(
  SRCS
    # ARM CPU emulation
    "arm/arm.c"
    "arm/decoder-arm.c"
    "arm/decoder-thumb.c"
    "arm/decoder.c"
    "arm/isa-arm.c"
    "arm/isa-thumb.c"

    # Core framework
    "core/bitmap-cache.c"
    "core/cache-set.c"
    "core/cheats.c"
    "core/config.c"
    "core/core.c"
    "core/interface.c"
    "core/library.c"
    "core/lockstep.c"
    "core/log.c"
    "core/map-cache.c"
    "core/mem-search.c"
    "core/rewind.c"
    "core/serialize.c"
    "core/sync.c"
    "core/tile-cache.c"
    "core/timing.c"
    "core/version.c"

    # GBA emulation
    "gba/audio.c"
    "gba/bios.c"
    "gba/cart/ereader.c"
    "gba/cart/gpio.c"
    "gba/cart/matrix.c"
    "gba/cart/vfame.c"
    "gba/cheats.c"
    "gba/cheats/codebreaker.c"
    "gba/cheats/gameshark.c"
    "gba/cheats/parv3.c"
    "gba/core.c"
    "gba/dma.c"
    "gba/gba.c"
    "gba/hle-bios.c"
    "gba/input.c"
    "gba/io.c"
    "gba/memory.c"
    "gba/overrides.c"
    "gba/renderers/common.c"
    "gba/renderers/software-bg.c"
    "gba/renderers/software-mode0.c"
    "gba/renderers/software-obj.c"
    "gba/renderers/video-software.c"
    "gba/savedata.c"
    "gba/serialize.c"
    "gba/sharkport.c"
    "gba/sio.c"
    "gba/sio/dolphin.c"
    "gba/sio/gbp.c"
    "gba/sio/joybus.c"
    "gba/sio/lockstep.c"
    "gba/timer.c"
    "gba/video.c"

    # Utilities
    "util/circle-buffer.c"
    "util/configuration.c"
    "util/convolve.c"
    "util/crc32.c"
    "util/formatting.c"
    "util/gbk-table.c"
    "util/hash.c"
    "util/memory.c"
    "util/patch-fast.c"
    "util/patch-ips.c"
    "util/patch-ups.c"
    "util/patch.c"
    "util/ring-fifo.c"
    "util/string.c"
    "util/table.c"
    "util/text-codec.c"
    "util/vfs.c"
    "util/vfs/vfs-dirent.c"
    "util/vfs/vfs-file.c"
    "util/vfs/vfs-mem.c"

    # Audio resampling
    "blip_buf/blip_buf.c"

    # Third-party
    "third-party/inih/ini.c"

  INCLUDE_DIRS
    "include"
    "include/mgba"
    "."

  PRIV_REQUIRES
    newlib
)

# Core defines for minimal embedded build
target_compile_definitions(${COMPONENT_LIB} PUBLIC
  MINIMAL_CORE=2
  M_CORE_GBA
  DISABLE_THREADING
  BUILD_STATIC
  HAVE_LOCALTIME_R
  HAVE_STRDUP
  _GNU_SOURCE
)

# Suppress warnings in third-party code; optimize for size
target_compile_options(${COMPONENT_LIB} PRIVATE
  -w
  -Os
  -ffunction-sections
  -fdata-sections
)
