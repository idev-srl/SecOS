/* SecOS - LVGL 9.2 configuration for SWP clients of the secwm compositor.
 * SPDX-License-Identifier: MIT
 *
 * Everything not set here takes LVGL's default (src/lv_conf_internal.h).
 * LVGL brings its own allocator, string and sprintf (LV_STDLIB_BUILTIN), so
 * the port needs almost nothing from the SecOS libc. No OS layer, no file
 * system, no image decoders, no external libraries: software rendering into a
 * 32-bit buffer that the client sends to secwm with SWP PUT requests.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (768 * 1024U)

#define LV_USE_OS               LV_OS_NONE
#define LV_DEF_REFR_PERIOD      33
#define LV_DPI_DEF              110

#define LV_USE_DRAW_SW          1
#define LV_USE_DRAW_SW_ASM      LV_DRAW_SW_ASM_NONE
#define LV_DRAW_SW_COMPLEX      1

#define LV_USE_LOG              0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_USE_FLOAT            0
#define LV_USE_SYSMON           0
#define LV_USE_PERF_MONITOR     0
#define LV_USE_MEM_MONITOR      0

#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT    1
#define LV_THEME_DEFAULT_DARK   1

/* Not used by the SecOS clients. */
#define LV_USE_FS_STDIO         0
#define LV_USE_FS_POSIX         0
#define LV_USE_LODEPNG          0
#define LV_USE_LIBPNG           0
#define LV_USE_BMP              0
#define LV_USE_TJPGD            0
#define LV_USE_GIF              0
#define LV_USE_QRCODE           0
#define LV_USE_BARCODE          0
#define LV_USE_FREETYPE         0
#define LV_USE_TINY_TTF         0
#define LV_USE_RLOTTIE          0
#define LV_USE_VECTOR_GRAPHIC   0
#define LV_USE_THORVG_INTERNAL  0
#define LV_USE_LZ4_INTERNAL     0
#define LV_USE_SNAPSHOT         0
#define LV_USE_MONKEY           0
#define LV_USE_GRIDNAV          0
#define LV_USE_FRAGMENT         0
#define LV_USE_IMGFONT          0
#define LV_USE_IME_PINYIN       0
#define LV_USE_FILE_EXPLORER    0
#define LV_USE_LINUX_FBDEV      0
#define LV_USE_EVDEV            0
#define LV_USE_SDL              0
#define LV_BUILD_EXAMPLES       0

#endif
