"""PlatformIO pre-build script: put LVGL's TLSF pool in PSRAM.

LVGL's builtin allocator reserves LV_MEM_SIZE (64 KB) of internal RAM in .bss.
On this board that starves the pull-OTA path (WiFi + mbedTLS need a large
internal block). Force-including src/lv_mem_psram.h defines
LV_MEM_POOL_ALLOC() -> heap_caps_malloc(..., MALLOC_CAP_SPIRAM), so the pool
lands in the 8 MB PSRAM instead.

The path is passed as an absolute token (no shell metacharacters) because a
function-like -D macro cannot survive PlatformIO's shell flag parsing.
"""

import os

Import("env")  # noqa: F821  (injected by PlatformIO)

_header = os.path.join(env["PROJECT_DIR"], "src", "lv_mem_psram.h")  # noqa: F821
# C/C++ compile flags only: CPPFLAGS/BUILD_FLAGS also reach the assembler
# (.S files cannot parse a C header) and the linker.
env.Append(CCFLAGS=["-include", _header])   # noqa: F821
env.Append(CXXFLAGS=["-include", _header])  # noqa: F821
