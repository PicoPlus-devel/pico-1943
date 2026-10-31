/*
 * pico1943 - 1943: The Battle of Midway and 1943 Kai: Midway Kaisen (Capcom,
 * 1987) for RP2350 boards.
 *
 * Boots the pico_shared framework, finds the ROM sets on the SD card, and runs
 * the board (c1943/) one frame per display frame. The core is plain C shared
 * with the host harness in hosttest/; this file wires it to the display, audio
 * and controller drivers and to the settings menu.
 *
 * Like the other arcade games of this family there is no ROM browser:
 *   - menu() is never called; only the settings menu (SELECT + START) is used.
 *   - each set is read from its own folder: /roms/arcade/1943 (MAME's 1943.zip,
 *     merged, or a split 1943u.zip; the US, Euro and Japanese sets all play)
 *     and /roms/arcade/1943KAI (1943kai.zip), as zips or unzipped. With both
 *     on the card a small menu chooses; its Quit game returns there.
 *   - the machine's data (868 KB) does not fit in SRAM: on boards with PSRAM
 *     it is built there at every start; on the others it is built in flash
 *     once, the first time a set is started: the board restarts and copies
 *     the set before the display starts. See "The flash store".
 *
 * The game is vertical. The Tate mode setting chooses between the picture
 * upright for a normal monitor (default) and the raster for a monitor turned
 * on its side; see c1943/c1943.h. Upright, HSTX shows every row of the
 * picture, scaled to the screen height by the video driver; PicoDVI leaves one
 * row in 16 out to fit its 240.
 *
 * Built from pico-galagino's main.cpp.
 */

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"

#include "ff.h"
#include "tusb.h"

#include "FrensHelpers.h"
#include "FrensFonts.h"
#include "gamepad.h"
#include "menu.h"
#include "menu_settings.h"
#include "nespad.h"
#include "settings.h"
#include "vumeter.h"
#include "wiipad.h"

#include "c1943.h"
#include "c1943_input.h"
#include "c1943_store.h"

// The standard 252 MHz clock (10x the 640x480 pixel clock). On HSTX boards
// the display has a clock of its own, and the settings menu offers Overclock:
// 384 MHz at 1.50 V, the HSTX overclock of pico_shared (as pico-outrun runs).
// The menu stores the choice in pico_shared's FlashParams and restarts the
// board; main() picks it up. PicoDVI ties the system clock to the pixel clock:
// there it stays at 252 MHz.
#define C1943_CLOCKFREQ_KHZ 252000
#define C1943_VOLTAGE VREG_VOLTAGE_1_20
#if HSTX
#define C1943_MAX_CLOCKFREQ_KHZ 384000
#define C1943_MAX_VOLTAGE VREG_VOLTAGE_1_50
#else
#define C1943_MAX_CLOCKFREQ_KHZ C1943_CLOCKFREQ_KHZ
#define C1943_MAX_VOLTAGE C1943_VOLTAGE
#endif

// Must be a power of two (util::RingBuffer asserts it). It holds the audio
// cushion (see pushAudio()) plus a whole frame of 735 samples. The I2S ring of
// pico_shared is given the same size in CMakeLists.txt.
#define AUDIOBUFFERSIZE 2048

#define SAMPLERATE 44100
#define SAMPLES_PER_FRAME (SAMPLERATE / 60) // 735

#define ROMROOT "/roms/arcade"
#define LASTSET_FILE ROMROOT "/1943/LASTSET.TXT"

// 1: print the time spent per part of a frame over the serial console every
// 5 seconds. For measuring a board.
#ifndef C1943_FRAME_STATS
#define C1943_FRAME_STATS 0
#endif

// 1: use the flash store on a board with PSRAM as well, as a board without it
// does, ignoring the PSRAM. For testing the boards without PSRAM on one that
// has it (the Fruit Jam).
#ifndef C1943_FLASH_STORE_TEST
#define C1943_FLASH_STORE_TEST 0
#endif

// The set menu starts the set selected after this long without input.
#define CHOOSER_TIMEOUT_FRAMES (10 * 60)

// Output gains in Q8 (256 = unity). The board's mix peaks at about half of
// full scale; the I2S DAC drives the amplifier or headphones directly and gets
// a quarter, as in pico-galagino.
#ifndef DVI_AUDIO_GAIN_Q8
#define DVI_AUDIO_GAIN_Q8 256
#endif
#ifndef EXT_AUDIO_GAIN_Q8
#define EXT_AUDIO_GAIN_Q8 64
#endif

// SRAM the core keeps for good: the CPUs' ROMs and the characters (96 KB).
// The tile maps stay in PSRAM or flash: a row of the picture reads 16 bytes of
// each map, and 32 rows the same ones, which the XIP cache keeps. Also the
// copy buffer of the flash store, before the display starts. A fixed arena, so
// that its cost shows at link time, and the heap the drivers need is never
// taken.
#ifndef C1943_ARENA_SIZE
#define C1943_ARENA_SIZE (96 * 1024)
#endif
static_assert(C1943_ARENA_SIZE >= C1943_STORE_TEMP, "the arena holds the flash store's copy buffer");

static uint32_t CPUFreqKHz = C1943_CLOCKFREQ_KHZ;

static int16_t audioBuf[SAMPLES_PER_FRAME];
static bool audioPending = false; // a frame's sound is made (or being made), not pushed yet

// The sound board on core1. On HSTX boards core1 only feeds the display from
// DMA interrupts and runs a background task in between (pico_shared's
// video_output_set_background_task), so the sound board runs there, a frame at
// a time: core0 runs the main CPU's frame and posts its latch writes, core1
// makes the frame's sound, and core0 pushes it at the start of the next frame,
// as it pushes its own in sync mode. On PicoDVI boards core1 encodes the
// display and the sound board runs on core0 after each frame (sync mode).
#if HSTX
#define C1943_SOUND_ON_CORE1 1
#else
#define C1943_SOUND_ON_CORE1 0
#endif
#if C1943_SOUND_ON_CORE1
extern "C" void video_output_set_background_task(void (*task)(void));
#endif

// What the board is showing.
enum class State
{
    NoSets,  // no SD card, or no complete ROM set: what is missing
    Chooser, // both sets are there: which one
    Game,    // a set is running
    Failed,  // a set could not be started: the reason, until a button
};
static State state = State::NoSets;

static bool sdOk = false;
static int available[C1943_SET_COUNT]; // per set: the variant that can be played, or -1
static int navailable = 0;
static c1943_files_t probed[C1943_SET_COUNT];
static int running = -1;
static bool showSettings = false;
static uint32_t fps = 0;
static uint64_t lastFrameUs = 0;
static c1943_pads_t pads;

// ---------------------------------------------------------------------------
// Settings menu wiring.
//
// Positional, indexed by MenuSettingsIndex - append only, never reorder.
// 1 = shown, 0 = hidden, -1 = never shown. 0 and -1 differ only for Exit, Save/
// Restore state and Reset, which the in-game menu shows unless they are -1.
// C++ designated initializers may not skip members, so every entry up to the
// last one used is listed.
//
// Two tables: with both sets on the card "Quit game" returns to the set menu;
// with one there is nothing to quit to.
// ---------------------------------------------------------------------------
#define C1943_VISIBILITY(canQuit)                                                       \
    {                                                                                   \
        [MOPT_EXIT_GAME] = (canQuit) ? 1 : -1, /* back to the set menu */               \
        [MOPT_RESET_GAME] = 1,                                                          \
        [MOPT_REBOOT_TO_LOADER] = BOOTLOADER_BUILD, /* the bootloader's picker */       \
        [MOPT_SAVE_RESTORE_STATE] = -1,             /* no save states */                \
        [MOPT_SCREENMODE] = HSTX, /* PicoDVI has only one: no scanlines */              \
        [MOPT_SCANLINES] = 0, /* covered by the screen modes */                         \
        [MOPT_SCANLINE_TYPE] = HSTX,                                                    \
        [MOPT_FPS_OVERLAY] = 1,                                                         \
        [MOPT_AUDIO_ENABLE] = 1,                                                        \
        [MOPT_FRAMESKIP] = 0,                                                           \
        [MOPT_DISPLAY_MODE] = HSTX && ENABLEDVI,                                        \
        [MOPT_EXTERNAL_AUDIO] = EXT_AUDIO_IS_ENABLED,                                   \
        [MOPT_FONT_COLOR] = 0,                                                          \
        [MOPT_FONT_BACK_COLOR] = 0,                                                     \
        [MOPT_FRUITJAM_VUMETER] = ENABLE_VU_METER,                                      \
        [MOPT_FRUITJAM_VOLUME_CONTROL] = (HW_CONFIG == 8),                              \
        [MOPT_DMG_PALETTE] = 0,            /* Game Boy */                               \
        [MOPT_BORDER_MODE] = 0,            /* Game Boy */                               \
        [MOPT_RAPID_FIRE_ON_A] = 1,        /* button 1: fire */                         \
        [MOPT_RAPID_FIRE_ON_B] = 0,        /* button 2 is the special attack */         \
        [MOPT_AUTO_INSERT_FDS_DISK_A] = 0, /* Famicom Disk System */                    \
        [MOPT_AUTO_SWAP_FDS_DISK] = 0,     /* Famicom Disk System */                    \
        [MOPT_FDS_DISK_SWAP] = 0,          /* Famicom Disk System */                    \
        [MOPT_OVERCLOCK] = HSTX ? 2 : 0,   /* 2: in-game as well, 384 MHz (see above) */ \
        [MOPT_FM_AUDIO] = 0,               /* Master System YM2413 */                   \
        [MOPT_ENTER_BOOTSEL_MODE] = 1,                                                  \
        [MOPT_CONTROLLER_TEST] = 1,                                                     \
        [MOPT_RECENT_GAMES] = 0,  /* ROM browser only */                                \
        [MOPT_USB_DRIVE_MODE] = 0, /* shown regardless: FRENS_FORCE_USB_MSC_IN_SETTINGS */ \
        [MOPT_CASSETTE] = 0,      /* TI-99/4A */                                        \
        [MOPT_DISK] = 0,          /* TI-99/4A */                                        \
        [MOPT_SERIAL_KEYBOARD] = 0, /* TI-99/4A */                                      \
        [MOPT_SPRITE_LIMIT] = 0,  /* NES */                                             \
        [MOPT_MENU_OVERSCAN] = 0, /* shown regardless: listed below the menu colors */  \
        [MOPT_GENESIS_PAD] = 0,   /* Genesis */                                         \
        [MOPT_NES_PALETTE] = 0,   /* NES */                                             \
        [MOPT_BUTTON_LAYOUT] = 0, /* NES */                                             \
        [MOPT_TATE_MODE] = 1,     /* the game is vertical */                            \
    }

int8_t g_settings_visibility_1943_quit[MOPT_COUNT] = C1943_VISIBILITY(1);
int8_t g_settings_visibility_1943[MOPT_COUNT] = C1943_VISIBILITY(0);

// The 8:7 modes stretch NES pixels; 1943's are square-ish, so only the 1:1
// modes are offered. PicoDVI has no scanlines: on the upright picture (Tate mode
// off) they do not display well.
const uint8_t g_available_screen_modes_1943[] = {
    0, // SCANLINE_8_7
    0, // NOSCANLINE_8_7
    HSTX, // SCANLINE_1_1
    1,    // NOSCANLINE_1_1
};

// ---------------------------------------------------------------------------
// Framebuffer access: the one place the two video back-ends differ.
// Both are a contiguous 16-bit buffer, RGB555 on HSTX and RGB444 on PicoDVI,
// of 320x240 pixels. Except on HSTX with Tate off: there the canvas is as tall
// as the picture, TALL_W x 256, and the driver scales its rows to the 480
// lines of the display (hstx_setFrameSize), so no row is left out. The picture
// sits in the middle, between columns of 16 pixels that hold the FPS counter.
// It fits in the same memory.
// ---------------------------------------------------------------------------
#if HSTX
#define TALL_W 256
static_assert(TALL_W * C1943_H <= SCREENWIDTH * SCREENHEIGHT, "the tall canvas must fit the framebuffer");
#endif
static int fbWidth = SCREENWIDTH; // the canvas, and its stride
static int fbHeight = SCREENHEIGHT;

// Switches between the 320x240 canvas and the tall one. Returns true when it
// changed, which leaves the screen black.
static bool setTallCanvas(bool tall)
{
#if HSTX
    const int w = tall ? TALL_W : SCREENWIDTH;
    const int h = tall ? C1943_H : SCREENHEIGHT;
    if (w == fbWidth && h == fbHeight)
        return false;
    // read with the other stride, the old picture would show as noise
    memset(hstx_getframebuffer(), 0, SCREENWIDTH * SCREENHEIGHT * sizeof(uint16_t));
    if (!hstx_setFrameSize(w, h))
        panic("hstx_setFrameSize(%d, %d) failed\n", w, h);
    fbWidth = w;
    fbHeight = h;
    return true;
#else
    (void)tall;
    return false;
#endif
}

static inline uint16_t *fbLine(int line)
{
#if HSTX
    return hstx_getlineFromFramebuffer(line);
#else
    return &Frens::framebuffer[line * SCREENWIDTH];
#endif
}

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if HSTX
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
#else
    return (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
#endif
}

// ---------------------------------------------------------------------------
// Text on the framebuffer, for the set menu, the error screens and the FPS
// overlay. pico_shared's putText() only works inside its own menus, but the
// 8x8 font is exported.
// ---------------------------------------------------------------------------
static void screenClear(uint16_t bg)
{
    setTallCanvas(false);
    for (int y = 0; y < SCREENHEIGHT; y++)
    {
        uint16_t *dst = fbLine(y);
        for (int x = 0; x < SCREENWIDTH; x++)
            dst[x] = bg;
    }
}

// col/row in 8x8 cells of the canvas (40x30 on the 320x240 one); col < 0
// centres the text.
static void screenText(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    const int cols = fbWidth / FONT_CHAR_WIDTH;
    const int len = (int)strlen(s);
    if (col < 0)
        col = (len < cols) ? (cols - len) / 2 : 0;
    for (int i = 0; i < len && col + i < cols; i++)
    {
        char c = s[i];
        if (c < FONT_FIRST_ASCII || c >= FONT_FIRST_ASCII + FONT_N_CHARS)
            c = ' ';
        for (int line = 0; line < FONT_CHAR_HEIGHT; line++)
        {
            uint16_t *dst = fbLine(row * FONT_CHAR_HEIGHT + line) + (col + i) * FONT_CHAR_WIDTH;
            // leftmost pixel in the low bit
            char slice = getcharslicefrom8x8font(c, line);
            for (int bit = 0; bit < FONT_CHAR_WIDTH; bit++, slice >>= 1)
                *dst++ = (slice & 1) ? fg : bg;
        }
    }
}

#define SCR_BG rgb(0, 0, 0)
#define SCR_FG rgb(200, 200, 200)
#define SCR_HI rgb(255, 64, 64)
#define SCR_FILE rgb(255, 255, 255)

static void setDir(int set, char *buf, size_t n)
{
    snprintf(buf, n, ROMROOT "/%s", c1943_sets[set].folder);
}

// No SD card, or no complete ROM set: the folders, and what is missing.
static void drawNoSetsScreen()
{
    char buf[48];
    int row = 1;

    screenClear(SCR_BG);
    screenText(-1, row++, "pico1943", SCR_HI, SCR_BG);
    row++;
    if (!sdOk)
    {
        screenText(1, row++, "No SD card found.", SCR_HI, SCR_BG);
    }
    else
    {
        screenText(1, row++, "No complete ROM set found.", SCR_HI, SCR_BG);
        row++;
        screenText(1, row++, "Copy MAME's 1943.zip to", SCR_FG, SCR_BG);
        screenText(3, row++, ROMROOT "/1943", SCR_FILE, SCR_BG);
        screenText(1, row++, "or 1943kai.zip to", SCR_FG, SCR_BG);
        screenText(3, row++, ROMROOT "/1943KAI", SCR_FILE, SCR_BG);
        screenText(1, row++, "as a zip or unzipped.", SCR_FG, SCR_BG);
        for (int s = 0; s < C1943_SET_COUNT && row < 24; s++)
        {
            if (!probed[s])
                continue;
            // a set partly there: what it lacks
            const c1943_set_info_t *si = &c1943_sets[s];
            const int v = c1943_variant_nearest((c1943_set_id_t)s, probed[s]);
            const c1943_files_t missing = c1943_variant_files((c1943_set_id_t)s, v) & ~probed[s];
            row++;
            snprintf(buf, sizeof(buf), "%s lacks:", si->folder);
            screenText(1, row++, buf, SCR_FG, SCR_BG);
            int shown = 0, n = 0;
            for (int i = 0; i < si->nfiles; i++)
            {
                if (!(missing & ((c1943_files_t)1 << i)))
                    continue;
                n++;
                if (shown < 6 && row < 25)
                {
                    snprintf(buf, sizeof(buf), "  %s", si->files[i].name);
                    screenText(1, row++, buf, SCR_FILE, SCR_BG);
                    shown++;
                }
            }
            if (n > shown && row < 25)
            {
                snprintf(buf, sizeof(buf), "  ...and %d more", n - shown);
                screenText(1, row++, buf, SCR_FG, SCR_BG);
            }
        }
    }
    screenText(1, 26, "SELECT+START opens settings, which", SCR_HI, SCR_BG);
    screenText(1, 27, "can enter USB drive mode.", SCR_HI, SCR_BG);
}

// A set whose files looked complete could not be started.
static void drawFailedScreen(int set, const char *why)
{
    char buf[48];
    screenClear(SCR_BG);
    snprintf(buf, sizeof(buf), "%s", c1943_sets[set].folder);
    screenText(1, 2, buf, SCR_HI, SCR_BG);
    screenText(1, 4, "could not be started:", SCR_FG, SCR_BG);
    screenText(1, 6, why, SCR_FILE, SCR_BG);
    screenText(1, 27, "Press START to continue.", SCR_HI, SCR_BG);
}

// A set is about to be copied into flash: the board restarts for it.
static void drawCopyScreen(int set)
{
    screenClear(SCR_BG);
    screenText(1, 9, c1943_sets[set].title, SCR_HI, SCR_BG);
    screenText(1, 11, "runs from flash. Its ROM set is", SCR_FG, SCR_BG);
    screenText(1, 12, "copied there once: the board", SCR_FG, SCR_BG);
    screenText(1, 13, "restarts and copies it, which", SCR_FG, SCR_BG);
    screenText(1, 14, "takes less than a minute.", SCR_FG, SCR_BG);
}

// ---------------------------------------------------------------------------
// The set menu, when both sets are on the card.
// ---------------------------------------------------------------------------
static int chooserSel = 0;
static int chooserIdle = 0;
static bool chooserDrawn = false;
static uint32_t chooserPrev = ~0u; // last frame's buttons: held ones do not count

static int readLastSet()
{
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    int set = 0;
    char c = '0';
    UINT br = 0;
    if (fil && f_open(fil, LASTSET_FILE, FA_READ) == FR_OK)
    {
        if (f_read(fil, &c, 1, &br) == FR_OK && br == 1 && c >= '0' && c < '0' + C1943_SET_COUNT)
            set = c - '0';
        f_close(fil);
    }
    Frens::f_free(fil);
    return set;
}

static void writeLastSet(int set)
{
    if (!sdOk || readLastSet() == set)
        return;
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    const char c = (char)('0' + set);
    UINT bw = 0;
    if (fil && f_open(fil, LASTSET_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
    {
        f_write(fil, &c, 1, &bw);
        f_close(fil);
    }
    Frens::f_free(fil);
}

static void drawChooser()
{
    char buf[48];
    if (!chooserDrawn)
    {
        screenClear(SCR_BG);
        screenText(-1, 6, "pico1943", SCR_HI, SCR_BG);
        screenText(-1, 20, "UP/DOWN chooses, A or START plays", SCR_FG, SCR_BG);
        chooserDrawn = true;
    }
    int row = 10;
    for (int s = 0; s < C1943_SET_COUNT; s++)
    {
        if (available[s] < 0)
            continue;
        const bool sel = (s == chooserSel);
        snprintf(buf, sizeof(buf), "%c %s", sel ? '>' : ' ', c1943_sets[s].title);
        screenText(4, row, buf, sel ? SCR_FILE : SCR_FG, SCR_BG);
        row += 2;
    }
    const int left = (CHOOSER_TIMEOUT_FRAMES - chooserIdle + 59) / 60;
    snprintf(buf, sizeof(buf), "Starts in %2d s", left);
    screenText(-1, 24, buf, SCR_FG, SCR_BG);
}

// ---------------------------------------------------------------------------
// ROM loading through FatFs
// ---------------------------------------------------------------------------
static int ffListDir(void *ctx, const char *dir, c1943_dir_cb cb, void *arg)
{
    (void)ctx;
    DIR *d = (DIR *)Frens::f_malloc(sizeof(DIR));
    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    int ok = 0;
    if (d && fno && f_opendir(d, dir) == FR_OK)
    {
        ok = 1;
        while (f_readdir(d, fno) == FR_OK && fno->fname[0])
            cb(arg, fno->fname, (uint32_t)fno->fsize, (fno->fattrib & AM_DIR) ? 1 : 0);
        f_closedir(d);
    }
    Frens::f_free(fno);
    Frens::f_free(d);
    return ok;
}

static void *ffOpen(void *ctx, const char *path)
{
    (void)ctx;
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (fil && f_open(fil, path, FA_READ) == FR_OK)
        return fil;
    Frens::f_free(fil);
    return nullptr;
}

static uint32_t ffSize(void *ctx, void *file)
{
    (void)ctx;
    return (uint32_t)f_size((FIL *)file);
}

static int ffReadAt(void *ctx, void *file, uint32_t ofs, void *buf, uint32_t len)
{
    (void)ctx;
    UINT br = 0;
    if (f_lseek((FIL *)file, ofs) != FR_OK || f_read((FIL *)file, buf, len, &br) != FR_OK)
        return -1;
    return (int)br;
}

static void ffClose(void *ctx, void *file)
{
    (void)ctx;
    f_close((FIL *)file);
    Frens::f_free(file);
}

// The loader's scratch buffers go where f_malloc puts them: PSRAM on the boards
// that have it (see pico-galagino's main.cpp for why).
static void *ffAlloc(void *ctx, uint32_t size)
{
    (void)ctx;
    return Frens::f_malloc(size);
}

static void ffFree(void *ctx, void *p)
{
    (void)ctx;
    Frens::f_free(p);
}

static const c1943_io_t romIo = {nullptr, ffListDir, ffOpen, ffSize, ffReadAt, ffClose, ffAlloc, ffFree};

// ---------------------------------------------------------------------------
// Memory: the arena (SRAM) and PSRAM
// ---------------------------------------------------------------------------
static uint8_t __attribute__((aligned(4))) arena[C1943_ARENA_SIZE];
static uint32_t arenaUsed = 0;

// What the core keeps in SRAM: from the arena, while it lasts.
static void *fastAlloc(uint32_t size)
{
    size = (size + 3) & ~3u;
    if (arenaUsed + size > sizeof(arena))
        return nullptr;
    void *p = arena + arenaUsed;
    arenaUsed += size;
    return p;
}

static void *psramAlloc(uint32_t size)
{
    return Frens::f_malloc(size);
}

static void psramFree(void *p)
{
    Frens::f_free(p);
}

static bool usePsram()
{
    return Frens::isPsramEnabled() && !C1943_FLASH_STORE_TEST;
}

// ---------------------------------------------------------------------------
// The flash store (boards without PSRAM).
//
// The machine's data is built in flash there; see c1943/c1943_store.c. A set
// is copied the first time it is started: startSet() notes it in a watchdog
// scratch register and restarts the board, and initAll() runs prepareFlash()
// once the SD card is mounted, before the display starts, so flash is erased
// and programmed with nothing else running. main() then starts the set at
// once.
//
// The store is the flash pico_shared sets aside for roms on such boards, after
// the program (ROM_FILE_ADDR, maxRomSize), as large as the chip itself
// reports; CMakeLists.txt sets PICO_FLASH_SIZE_BYTES to 16 MB so that the
// SDK's flash functions accept every offset of a larger chip.
//
// scratch[0..1]: pico1943's own (the SDK uses 4..7 to reboot, pico_shared 6
// and 7 for pico-bootLoader). They survive the watchdog restart, also when
// pico-bootLoader resumes the application, and are cleared by a power cycle.
// The restart is made with watchdog_enable(): pico-bootLoader resumes the
// application only after that. initAll() takes the same mark for a ROM picked
// in pico_shared's browser, and flashes the ROM that browser picked last (of
// any emulator) right where the store is; main() clears the mark before
// initAll() (clearRestartMark()).
// ---------------------------------------------------------------------------
static constexpr uint32_t PENDING_COPY_MAGIC = 0x31393433u; // "1943"
static int copiedSet = -1; // the set copied at this boot, to start at once
static bool storeOpen = false;

static int takePendingCopy(int *variant)
{
    if (watchdog_hw->scratch[0] != PENDING_COPY_MAGIC)
        return -1;
    const uint32_t v = watchdog_hw->scratch[1];
    watchdog_hw->scratch[0] = 0;
    const int set = (int)(v & 0xff);
    *variant = (int)((v >> 8) & 0xff);
    return set < C1943_SET_COUNT ? set : -1;
}

// Before initAll(): no restart of pico1943 is a ROM picked in the browser, not
// one for a copy into flash, nor the one that applies the Overclock setting
// (pico_shared's writeFlashParamsToFlash()).
static void clearRestartMark()
{
    watchdog_hw->scratch[4] = 0; // the SDK's watchdog_enable() mark
}

static void flashErase(uint32_t offset, uint32_t len)
{
    Frens::flashEraseSafe(offset, len);
}

static void flashProgram(uint32_t offset, const uint8_t *data, uint32_t len)
{
    Frens::flashProgramSafe(offset, data, len);
}

static void flashCopying(int set)
{
    static bool led = false, told = false;
    led = !led;
    Frens::blinkLed(led);
    if (!told)
        printf("Copying %s to flash\n", c1943_sets[set].title);
    told = true;
}

static void openFlashStore()
{
#if C1943_FLASH_STORE_TEST
    // pico_shared leaves these at 0 with PSRAM; place them as it does without
    if (Frens::isPsramEnabled())
    {
        const uint32_t exp = Frens::storage_get_flash_jedec_id() & 0xff;
        ROM_FILE_ADDR = FLASHPARAM_ADDRESS + FLASH_SECTOR_SIZE;
        maxRomSize = (int)(XIP_BASE + (exp < 32 ? (1u << exp) : 0) - FLASH_RESERVED_TOP - ROM_FILE_ADDR);
    }
#else
    if (Frens::isPsramEnabled())
        return;
#endif
    if (storeOpen || !ROM_FILE_ADDR || maxRomSize <= 0)
        return;
    // the store starts on a 64 KB block, for the erases
    uintptr_t start = (ROM_FILE_ADDR + 0xffff) & ~(uintptr_t)0xffff;
    uintptr_t end = ROM_FILE_ADDR + (uint32_t)maxRomSize;
    if (end > XIP_BASE + 16 * 1024 * 1024) // the most XIP reaches
        end = XIP_BASE + 16 * 1024 * 1024;
    if (end <= start)
        return;
    c1943_flash_t fl;
    fl.base = (const uint8_t *)start;
    fl.offset = (uint32_t)(start - XIP_BASE);
    fl.size = (uint32_t)(end - start) & ~0xffffu;
    fl.erase = flashErase;
    fl.program = flashProgram;
    fl.copying = flashCopying;
    c1943_store_open(&fl);
    storeOpen = true;
    printf("Flash store: %lu KB at 0x%08lx, %d slot(s)\n", (unsigned long)(fl.size / 1024), (unsigned long)start,
           c1943_store_slots());
}

// initAll()'s pre-video-init task: a set waiting to be copied is copied now.
static void prepareFlash()
{
    openFlashStore();
    int variant = 0;
    const int set = takePendingCopy(&variant);
    if (set < 0 || !storeOpen)
        return;
    char dir[48];
    setDir(set, dir, sizeof(dir));
    const uint64_t t0 = Frens::time_us();
    c1943_store_copy((c1943_set_id_t)set, variant, &romIo, dir, arena, sizeof(arena));
    copiedSet = set;
    printf("Copied %s to flash in %lu ms: %s\n", c1943_sets[set].title,
           (unsigned long)((Frens::time_us() - t0) / 1000),
           c1943_store_variant((c1943_set_id_t)set) == variant ? "ok" : "FAILED");
}

// Restarts the board to copy the set into flash; see above.
static void __attribute__((noreturn)) copyAndRestart(int set, int variant)
{
    drawCopyScreen(set);
    sleep_ms(1500); // long enough to read
    watchdog_hw->scratch[1] = (uint32_t)set | ((uint32_t)variant << 8);
    watchdog_hw->scratch[0] = PENDING_COPY_MAGIC;
    // watchdog_enable(), not watchdog_reboot(0,0,0), for pico-bootLoader
    watchdog_enable(1, false);
    while (true)
        tight_loop_contents();
}

// The sets that can be played: complete on the card, or in the flash store.
static void probeSets()
{
    navailable = 0;
    char dir[48];
    for (int s = 0; s < C1943_SET_COUNT; s++)
    {
        available[s] = -1;
        probed[s] = 0;
        if (sdOk)
        {
            setDir(s, dir, sizeof(dir));
            probed[s] = c1943_probe((c1943_set_id_t)s, &romIo, dir);
            available[s] = c1943_variant_found((c1943_set_id_t)s, probed[s]);
        }
        const int stored = storeOpen ? c1943_store_variant((c1943_set_id_t)s) : -1;
        if (available[s] < 0 && stored >= 0)
            available[s] = stored; // in flash, the card not needed
        if (available[s] >= 0)
        {
            navailable++;
            printf("%-28s %s (%s)%s\n", c1943_sets[s].title, c1943_sets[s].variant_set[available[s]],
                   c1943_sets[s].variant_name[available[s]], stored >= 0 ? ", in flash" : "");
        }
        else
        {
            printf("%-28s not found (%d files of it)\n", c1943_sets[s].title, __builtin_popcountll(probed[s]));
        }
    }
}

// ---------------------------------------------------------------------------
// Starting a set
// ---------------------------------------------------------------------------
static void enterChooser();
static void waitSound();

// The data of the set started last, built in PSRAM: freed before another set
// is loaded (the machine is not running then).
static const uint8_t *psramParts[C1943_P_COUNT];

static void freePsramParts()
{
    for (int p = 0; p < C1943_P_COUNT; p++)
    {
        if (psramParts[p])
            psramFree((void *)psramParts[p]);
        psramParts[p] = nullptr;
    }
}

static void startSet(int set)
{
    const int v = available[set];
    char dir[48];
    setDir(set, dir, sizeof(dir));
    const uint8_t *parts[C1943_P_COUNT];
    const uint64_t t0 = Frens::time_us();
    if (usePsram())
    {
        waitSound();
        freePsramParts();
        const c1943_files_t got = c1943_load((c1943_set_id_t)set, v, &romIo, dir, psramAlloc, psramFree, parts);
        if (got != c1943_variant_files((c1943_set_id_t)set, v))
        {
            drawFailedScreen(set, "Its files could not be read.");
            state = State::Failed;
            return;
        }
        for (int p = 0; p < C1943_P_COUNT; p++)
            psramParts[p] = parts[p];
    }
    else
    {
        // from the flash store, copied there first if it is not, or holds
        // another variant than the card
        if (c1943_store_variant((c1943_set_id_t)set) != v)
        {
            if (sdOk && c1943_store_room())
                copyAndRestart(set, v);
            drawFailedScreen(set, "It does not fit in flash.");
            state = State::Failed;
            return;
        }
        if (!c1943_store_parts((c1943_set_id_t)set, parts))
        {
            drawFailedScreen(set, "Its copy in flash is bad.");
            state = State::Failed;
            return;
        }
    }
    waitSound(); // the sound board is not running on core1 while it is set up
    arenaUsed = 0;
    c1943_start((c1943_set_id_t)set, parts, fastAlloc);
    c1943_sound_reset();
    running = set;
    writeLastSet(set);
    printf("Started %s (%s) in %lu ms; %lu bytes kept in SRAM\n", c1943_sets[set].title,
           c1943_sets[set].variant_set[v], (unsigned long)((Frens::time_us() - t0) / 1000),
           (unsigned long)c1943_fast_bytes());
    c1943_pads_reset(&pads);
    state = State::Game;
}

static void enterChooser()
{
    running = -1;
    chooserIdle = 0;
    chooserDrawn = false;
    chooserPrev = ~0u;
    chooserSel = readLastSet();
    if (available[chooserSel] < 0)
        for (int s = 0; s < C1943_SET_COUNT; s++)
            if (available[s] >= 0)
                chooserSel = s;
    c1943_pads_reset(&pads);
    state = State::Chooser;
}

// Where the board goes after booting, or when a game is quit.
static void enterSets()
{
    if (navailable == 0)
    {
        state = State::NoSets;
        drawNoSetsScreen();
    }
    else if (navailable == 1)
    {
        for (int s = 0; s < C1943_SET_COUNT; s++)
            if (available[s] >= 0)
                startSet(s);
    }
    else
    {
        enterChooser();
    }
}

// ---------------------------------------------------------------------------
// Audio: one frame of mono samples to whichever sink is active.
//
// See pico-galagino's main.cpp (from pico-mooncresta): the PicoDVI and I2S
// rings keep a cushion of AUDIO_CUSHION samples in front of each frame's
// burst, refilled with silence when the sink has drained, and a frame pushes
// one sample less or more when the level drifts; the HSTX data-island queue is
// clocked with the frames and only gets a small cushion when it has run dry.
// A frame's sound is pushed at the start of the next frame, right after the
// pace. Runs from SRAM.
// ---------------------------------------------------------------------------
#define AUDIO_CUSHION 512   // ~12 ms
#define AUDIO_TRIM_BAND 192 // drift tolerated before a frame is trimmed
#define HSTX_CUSHION 32     // ~0.7 ms, 8 packets

static inline int16_t applyGain(int x, int gainQ8)
{
    int32_t v = (x * gainQ8) >> 8;
    if (v > 32767)
        v = 32767;
    else if (v < -32768)
        v = -32768;
    return (int16_t)v;
}

// The two ring sinks: the I2S DAC (ext) or, on PicoDVI, the HDMI audio ring.
static inline int ringQueued(bool ext)
{
#if EXT_AUDIO_IS_ENABLED
    if (ext)
        return I2S_AUDIO_RING_SIZE - 1 - EXT_AUDIO_GET_FREE();
#endif
#if !HSTX
    return (int)dvi_->getAudioRingBuffer().getFullReadableSize();
#else
    return 0;
#endif
}

static inline void ringPut(bool ext, int16_t s)
{
#if EXT_AUDIO_IS_ENABLED
    if (ext)
    {
        EXT_AUDIO_ENQUEUE_SAMPLE(s, s);
        return;
    }
#endif
#if !HSTX
    auto &ring = dvi_->getAudioRingBuffer();
    if (ring.getWritableSize() == 0)
        return; // full: dropped
    *ring.getWritePointer() = {s, s};
    ring.advanceWritePointer(1);
#endif
}

static void __noinline __not_in_flash_func(pushAudio)(const int16_t *buf, int n)
{
    const bool mute = !settings.flags.audioEnabled;

#if EXT_AUDIO_IS_ENABLED
#if HSTX
    const bool ext = settings.flags.useExtAudio || Frens::isHeadPhoneJackConnected();
#else
    const bool ext = settings.flags.useExtAudio;
#endif
#else
    const bool ext = false;
#endif

#if HSTX
    if (!ext)
    {
        if (hstx_di_queue_get_level() == 0)
        {
            for (int i = 0; i < HSTX_CUSHION; i++)
                hstx_push_audio_sample(0, 0);
        }
        for (int i = 0; i < n; i++)
        {
            int16_t s = mute ? 0 : buf[i];
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter)
                addSampleToVUMeter(s);
#endif
            int16_t g = applyGain(s, DVI_AUDIO_GAIN_Q8);
            hstx_push_audio_sample(g, g);
        }
        return;
    }
#endif

    // the cushion and the drift trim, see above
    const int queued = ringQueued(ext);
    int repeat = 0;
    if (queued < AUDIO_CUSHION / 4)
    {
        for (int i = queued; i < AUDIO_CUSHION; i++)
            ringPut(ext, 0);
    }
    else if (queued > AUDIO_CUSHION + AUDIO_TRIM_BAND)
    {
        n--;
    }
    else if (queued < AUDIO_CUSHION - AUDIO_TRIM_BAND)
    {
        repeat = 1;
    }

    int16_t s = 0;
    for (int i = 0; i < n; i++)
    {
        s = mute ? 0 : buf[i];
#if ENABLE_VU_METER
        if (settings.flags.enableVUMeter)
            addSampleToVUMeter(s);
#endif
        ringPut(ext, applyGain(s, ext ? EXT_AUDIO_GAIN_Q8 : DVI_AUDIO_GAIN_Q8));
    }
    if (repeat)
        ringPut(ext, applyGain(s, ext ? EXT_AUDIO_GAIN_Q8 : DVI_AUDIO_GAIN_Q8));
}

// ---------------------------------------------------------------------------
// The sound board on core1 (HSTX), see "The sound board on core1" above.
//
// Frames are handed over through two slots, frame i in slot i & 1, with
// three counters: core0 copies a frame's latch writes into its slot and raises
// `posted`; core1 makes the sound of each frame posted and raises `done`;
// core0 pushes the sound of the frames done and raises `pushed`. core0 posts
// frame k at the end of its own work on it and pushes frame k-1 at the start
// of frame k+1: core1 has a frame and a half for each frame, while it also
// serves the display's interrupts. A slot is only written by core1 between
// its post and its sound being done, and by core0 otherwise.
// ---------------------------------------------------------------------------
static bool soundOnCore1 = false;
#if C1943_SOUND_ON_CORE1
static c1943_event_t core1Events[2][C1943_MAX_EVENTS];
static int core1Nevents[2];
static int16_t core1Audio[2][SAMPLES_PER_FRAME];
static volatile uint32_t soundPosted = 0, soundDone = 0;
static uint32_t soundPushed = 0; // core0's own

#if C1943_FRAME_STATS
static void statsStartCounter();
#endif

static void __not_in_flash_func(soundTask)()
{
#if C1943_FRAME_STATS
    static bool counting = false; // the cycle counter is per core
    if (!counting)
    {
        statsStartCounter();
        counting = true;
    }
#endif
    const uint32_t done = soundDone;
    if (done == soundPosted)
        return;
    __dmb();
    const int slot = done & 1;
    c1943_sound_frame(core1Events[slot], core1Nevents[slot], core1Audio[slot], SAMPLES_PER_FRAME);
    __dmb();
    soundDone = done + 1;
}

// Waits until core1 has made the sound of frames up to `frames`. A core1 that
// does not answer (it should within a frame) makes the sound go back to core0.
static void __not_in_flash_func(waitSoundDone)(uint32_t frames)
{
    const uint64_t t0 = Frens::time_us();
    while ((int32_t)(soundDone - frames) < 0)
    {
        if (Frens::time_us() - t0 > 100000)
        {
            printf("Sound on core1 does not answer: on core0 from now on\n");
            video_output_set_background_task(nullptr);
            soundOnCore1 = false;
            soundDone = soundPosted;
            break;
        }
        tight_loop_contents();
    }
    __dmb();
}

// core1 idle, nothing pending: before the sound board is reset or set up,
// and around the settings menu. The sound not pushed yet is dropped.
static void waitSound()
{
    if (!soundOnCore1)
        return;
    waitSoundDone(soundPosted);
    soundPushed = soundPosted;
}

static void __not_in_flash_func(postSound)(const c1943_event_t *events, int n)
{
    if (soundPosted - soundPushed >= 2)
    {
        // the slot is still taken (not pushed yet): drop that frame's sound
        waitSoundDone(soundPushed + 1);
        soundPushed++;
    }
    const int slot = soundPosted & 1;
    c1943_event_t *dst = core1Events[slot];
    for (int i = 0; i < n; i++)
        dst[i] = events[i];
    core1Nevents[slot] = n;
    __dmb();
    soundPosted = soundPosted + 1;
}

// The sound to push at the start of a frame: the frame before the one posted
// last, or nothing when there is none yet.
static const int16_t *__not_in_flash_func(core1SoundToPush)()
{
    if (soundPosted - soundPushed < 2)
        return nullptr;
    waitSoundDone(soundPushed + 1);
    if (!soundOnCore1)
        return nullptr;
    const int16_t *audio = core1Audio[soundPushed & 1];
    soundPushed++;
    return audio;
}
#else
static void waitSound()
{
}
#endif

// The sound of the frame just run: made here, or handed to core1.
static void __not_in_flash_func(makeSound)()
{
    const c1943_event_t *events;
    const int n = c1943_take_events(&events);
#if C1943_SOUND_ON_CORE1
    if (soundOnCore1)
    {
        postSound(events, n);
        return;
    }
#endif
    c1943_sound_frame(events, n, audioBuf, SAMPLES_PER_FRAME);
    audioPending = true;
}

// At the start of a frame: the sound of an earlier frame to the audio sinks.
static void __not_in_flash_func(pushFrameSound)()
{
#if C1943_SOUND_ON_CORE1
    if (soundOnCore1)
    {
        const int16_t *audio = core1SoundToPush();
        if (audio)
            pushAudio(audio, SAMPLES_PER_FRAME);
        return;
    }
#endif
    if (audioPending)
    {
        pushAudio(audioBuf, SAMPLES_PER_FRAME);
        audioPending = false;
    }
}

// ---------------------------------------------------------------------------
// Video: draw the frame the machine latched last, plus the FPS digits. Called
// right after the frame pace returns, i.e. at the start of output VBLANK, so
// the single-buffered framebuffer is rewritten ahead of scan-out. The borders
// are painted only when they need it.
// ---------------------------------------------------------------------------
static bool bordersValid = false;
static int bordersOrient = -1;
static bool fpsShown = false;
static bool skipDraw = false; // the last frame ran late: this one is not drawn

#if C1943_FRAME_STATS
#include "hardware/structs/m33.h"
#include "hardware/clocks.h"

// c1943_internal.h's profiling: cycles per part of a frame, from the DWT cycle
// counter
extern "C" uint32_t c1943_prof[8];
extern "C" uint32_t __not_in_flash_func(c1943_cycles)(void)
{
    return m33_hw->dwt_cyccnt;
}

static void statsStartCounter()
{
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

static struct
{
    uint32_t frames, emu_sum, emu_max, draw_sum, draw_max;
    uint32_t idle_sum, idle_min, late, skipped; // paceFrame(): the wait for the display
} stats = {0, 0, 0, 0, 0, 0, UINT32_MAX, 0, 0};

static void statsAdd(uint32_t emu, uint32_t draw)
{
    stats.emu_sum += emu;
    stats.draw_sum += draw;
    if (emu > stats.emu_max)
        stats.emu_max = emu;
    if (draw > stats.draw_max)
        stats.draw_max = draw;
    if (++stats.frames == 300)
    {
        printf("frame stats: emulate %lu us (max %lu), draw %lu us (max %lu), %lu fps\n",
               (unsigned long)(stats.emu_sum / stats.frames), (unsigned long)stats.emu_max,
               (unsigned long)(stats.draw_sum / stats.frames), (unsigned long)stats.draw_max, (unsigned long)fps);
        const uint32_t mhz = clock_get_hz(clk_sys) / 1000000;
        printf("  idle us: avg %lu min %lu, late frames %lu, frames not drawn %lu\n",
               (unsigned long)(stats.idle_sum / stats.frames), (unsigned long)stats.idle_min,
               (unsigned long)stats.late, (unsigned long)stats.skipped);
        printf("  parts us: main %lu, sound board %lu (FM %lu, SSG %lu), draw %lu\n",
               (unsigned long)(c1943_prof[0] / mhz / stats.frames), (unsigned long)(c1943_prof[1] / mhz / stats.frames),
               (unsigned long)(c1943_prof[2] / mhz / stats.frames), (unsigned long)(c1943_prof[3] / mhz / stats.frames),
               (unsigned long)(c1943_prof[4] / mhz / stats.frames));
        memset(c1943_prof, 0, sizeof(uint32_t) * 8);
        const c1943_stats_t *st = c1943_stats();
        printf("  machine: pc %04x/%04x, idle cycles %lu/%lu, bank switches %lu\n", st->main_pc, st->sound_pc,
               (unsigned long)st->main_idle_cycles, (unsigned long)st->sound_idle_cycles,
               (unsigned long)st->bank_switches);
        memset(&stats, 0, sizeof(stats));
        stats.idle_min = UINT32_MAX;
    }
}
#endif

static uint32_t lastDrawUs = 0;

static void __noinline __not_in_flash_func(presentFrame)()
{
    if (state != State::Game)
        return;
    const uint64_t t0 = Frens::time_us();
    c1943_orient_t orient = (c1943_orient_t)settings.flags.tateMode;
    const bool showFps = settings.flags.displayFrameRate;

    // Tate off on HSTX: the tall canvas
    if (setTallCanvas(HSTX && orient == C1943_ORIENT_UPRIGHT))
        bordersValid = false;
    if (fbWidth != SCREENWIDTH)
        orient = C1943_ORIENT_UPRIGHT_FULL;

    if (!bordersValid || orient != bordersOrient || (fpsShown && !showFps))
    {
        c1943_render_borders(orient, fbLine(0), fbWidth);
        bordersValid = true;
        bordersOrient = orient;
        skipDraw = false;
    }
    fpsShown = showFps;

    if (!skipDraw)
        c1943_render(orient, fbLine(0), fbWidth);
#if C1943_FRAME_STATS
    else
        stats.skipped++;
#endif

    if (showFps)
    {
        char s[3] = {(char)('0' + (fps / 10) % 10), (char)('0' + fps % 10), 0};
        // In the left border: one cell in from the corner a TV that overscans
        // cuts off when upright (48 px), at its edge in tate (16 px) and in the
        // 16-px column of the tall canvas.
        screenText(orient == C1943_ORIENT_UPRIGHT ? 1 : 0, 1, s, rgb(255, 255, 255), rgb(0, 0, 0));
    }
    lastDrawUs = (uint32_t)(Frens::time_us() - t0);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// One GPIO port's pad in io::GamePadState button bits; see pico-outrun's
// nespadGameBits() for why the pad type is read as "SNES or not".
static uint32_t nespadGameBits(uint16_t n, uint8_t type)
{
    typedef io::GamePadState::Button B;
    uint32_t b = 0;
    if (n & (1u << 2)) b |= B::SELECT;
    if (n & (1u << 3)) b |= B::START;
    if (n & (1u << 4)) b |= B::UP;
    if (n & (1u << 5)) b |= B::DOWN;
    if (n & (1u << 6)) b |= B::LEFT;
    if (n & (1u << 7)) b |= B::RIGHT;
    if (type != NESPAD_TYPE_SNES)
    {
        if (n & (1u << 0)) b |= B::A;
        if (n & (1u << 1)) b |= B::B;
    }
    else
    {
        if (n & (1u << 0)) b |= B::B;
        if (n & (1u << 1)) b |= B::Y;
        if (n & (1u << 8)) b |= B::A;
        if (n & (1u << 9)) b |= B::X;
    }
    return b;
}

// io::GamePadState buttons to c1943_input.h's pad bits.
static uint32_t padBits(uint32_t b)
{
    typedef io::GamePadState::Button B;
    uint32_t p = 0;
    if (b & B::A) p |= C1943_PAD_A;
    if (b & B::B) p |= C1943_PAD_B;
    if (b & B::X) p |= C1943_PAD_X;
    if (b & B::Y) p |= C1943_PAD_Y;
    if (b & B::SELECT) p |= C1943_PAD_SELECT;
    if (b & B::START) p |= C1943_PAD_START;
    if (b & B::UP) p |= C1943_PAD_UP;
    if (b & B::DOWN) p |= C1943_PAD_DOWN;
    if (b & B::LEFT) p |= C1943_PAD_LEFT;
    if (b & B::RIGHT) p |= C1943_PAD_RIGHT;
    return p;
}

// The two players' controllers. Player 1: the first USB pad, the first GPIO
// pad and the Wii pad; player 2: the second USB pad and the second GPIO pad.
static void readPads(uint16_t wii, uint32_t *p1, uint32_t *p2)
{
    typedef io::GamePadState::Button B;
    uint32_t a = 0, b = 0;
    auto &gp = io::getCurrentGamePadState(0);
    if (gp.connected)
        a |= gp.buttons;
    auto &gp2 = io::getCurrentGamePadState(1);
    if (gp2.connected)
        b |= gp2.buttons;
#if NES_PIN_CLK != -1
    a |= nespadGameBits(nespad_states_ext[0], nespad_padtype[0]);
#endif
#if NES_PIN_CLK_1 != -1
    b |= nespadGameBits(nespad_states_ext[1], nespad_padtype[1]);
#endif
    if (wii & (1u << 0)) a |= B::A;
    if (wii & (1u << 1)) a |= B::B;
    if (wii & (1u << 2)) a |= B::SELECT;
    if (wii & (1u << 3)) a |= B::START;
    if (wii & (1u << 4)) a |= B::UP;
    if (wii & (1u << 5)) a |= B::DOWN;
    if (wii & (1u << 6)) a |= B::LEFT;
    if (wii & (1u << 7)) a |= B::RIGHT;
    if (wii & (1u << 8)) a |= B::X;
    if (wii & (1u << 9)) a |= B::Y;
    *p1 = padBits(a);
    *p2 = padBits(b);
}

// ---------------------------------------------------------------------------
// Wii Classic controller (I2C): started here before the game and retried
// about once a second while no pad answers. From pico-mooncresta.
// ---------------------------------------------------------------------------
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
static void wiipadPoll()
{
    static int frames = 0;
    if (wiipad_is_connected() || ++frames < 60)
        return;
    frames = 0;
    wiipad_begin();
}
#endif

// ---------------------------------------------------------------------------
// Frame pacing: one game frame per displayed frame, from the display's frame
// counter (see pico-galagino's main.cpp). A frame that overruns a displayed
// frame is caught up by starting the next one at once, so no sound is lost,
// and the next picture is not drawn, which gives the time back; one more than
// a frame behind, the missed frames are dropped.
// ---------------------------------------------------------------------------
static inline uint32_t displayFrameCounter()
{
#if HSTX
    return hstx_getframecounter();
#else
    return dvi_->getFrameCounter();
#endif
}

static void paceFrame(bool init)
{
    static uint32_t target = 0;
    if (init)
    {
        target = displayFrameCounter();
        skipDraw = false;
        return;
    }
    target++;
    const int32_t lag = (int32_t)(displayFrameCounter() - target);
#if C1943_FRAME_STATS
    const uint64_t w0 = Frens::time_us();
#endif
    skipDraw = false;
    if (lag < 0)
    {
        while ((int32_t)(displayFrameCounter() - target) < 0)
            __compiler_memory_barrier(); // the counter is advanced by core1
    }
    else if (lag > 0)
    {
        target = displayFrameCounter();
        skipDraw = true;
#if C1943_FRAME_STATS
        stats.late++;
#endif
    }
#if C1943_FRAME_STATS
    const uint32_t idle = (uint32_t)(Frens::time_us() - w0);
    stats.idle_sum += idle;
    if (idle < stats.idle_min)
        stats.idle_min = idle;
#endif
}

// ---------------------------------------------------------------------------
// Once per frame
// ---------------------------------------------------------------------------
static void processPerFrame()
{
    paceFrame(false);

    // the sound of an earlier frame; see pushAudio() and makeSound()
    pushFrameSound();

    presentFrame();

    Frens::pollHeadPhoneJack();

#if ENABLE_VU_METER
    // Fruit Jam Button 2 toggles the VU meter, as in the sibling emulators.
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        FrensSettings::savesettings();
        turnOffAllLeds();
    }
#endif

    nespad_read_start();
    Frens::blinkLed((displayFrameCounter() >> 5) & 1);
    nespad_read_finish();

    tuh_task();
    uint16_t wii = 0;
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipadPoll();
    wii = wiipad_read(); // boards without the Wii port do not link wiipad at all
#endif

    uint32_t p1 = 0, p2 = 0;
    readPads(wii, &p1, &p2);
    int settingsCombo = 0, fpsToggle = 0;
    // no rapid fire on button 2: each press is a special attack, which costs
    // energy (the flag is shared with the other arcade games)
    const uint32_t in = c1943_pads_map(&pads, p1, p2, settings.flags.rapidFireOnA, 0, &settingsCombo, &fpsToggle);
    if (fpsToggle)
        settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
    if (settingsCombo)
        showSettings = true;

    if (showSettings)
    {
        showSettings = false;
        waitSound(); // core1 idle: Reset Game resets the sound board from here
        audioPending = false;
        g_settings_visibility =
            (state == State::Game && navailable > 1) ? g_settings_visibility_1943_quit : g_settings_visibility_1943;
        setTallCanvas(false); // the settings menu draws on the 320x240 canvas
        int rval = showSettingsMenu(true);

        // menu.cpp saves only down the SAVE path; `settings` only ever holds
        // committed values, so writing again can never persist a cancelled edit.
        FrensSettings::savesettings();
        scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);
        EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

        // Without a set there is nothing to return to. Reboot, so a set just
        // copied onto the card in USB drive mode is picked up.
        if (state == State::NoSets)
        {
            printf("Rebooting to look for the ROM sets again...\n");
            watchdog_reboot(0, 0, 0);
            while (true)
                tight_loop_contents();
        }
        if (state == State::Game)
        {
            if (rval == 3 && navailable > 1) // Quit game: back to the set menu
                enterChooser();
            else if (rval == 5) // Reset Game
            {
                c1943_reset();
                c1943_sound_reset();
            }
        }
        else if (state == State::Chooser)
        {
            chooserDrawn = false;
        }
        else if (state == State::Failed)
        {
            enterSets();
        }
        c1943_pads_reset(&pads);
        bordersValid = false; // the menu drew over the whole screen
        paceFrame(true);
        lastFrameUs = Frens::time_us();
        return;
    }

    switch (state)
    {
    case State::Game:
    {
        const uint64_t t0 = Frens::time_us();
        c1943_run_frame(in);
        makeSound();
#if C1943_FRAME_STATS
        statsAdd((uint32_t)(Frens::time_us() - t0), lastDrawUs);
#else
        (void)t0;
#endif
        break;
    }
    case State::Chooser:
    {
        // silence: pushed as audioBuf, core1 making nothing
        memset(audioBuf, 0, sizeof(audioBuf));
        if (!soundOnCore1)
            audioPending = true;
        const uint32_t pushed = p1 & ~chooserPrev;
        chooserPrev = p1;
        if (p1)
            chooserIdle = 0;
        if (pushed & (C1943_PAD_UP | C1943_PAD_DOWN))
        {
            const int dir = (pushed & C1943_PAD_UP) ? -1 : 1;
            int s = chooserSel;
            for (int k = 0; k < C1943_SET_COUNT; k++)
            {
                s = (s + dir + C1943_SET_COUNT) % C1943_SET_COUNT;
                if (available[s] >= 0)
                    break;
            }
            chooserSel = s;
        }
        drawChooser();
        if ((pushed & (C1943_PAD_A | C1943_PAD_START)) || ++chooserIdle >= CHOOSER_TIMEOUT_FRAMES)
        {
            startSet(chooserSel);
            bordersValid = false;
            paceFrame(true);
        }
        break;
    }
    case State::Failed:
        if (p1 & C1943_PAD_START)
        {
            enterSets();
            bordersValid = false;
        }
        break;
    case State::NoSets:
        break;
    }

    if (settings.flags.displayFrameRate)
    {
        uint64_t now = Frens::time_us();
        uint64_t dt = now - lastFrameUs;
        lastFrameUs = now;
        if (dt > 0)
            fps = (uint32_t)((1000000 + dt / 2) / dt);
    }
}

int main()
{
    // The clock: Overclock's, when the settings menu stored it in FlashParams
    // (see C1943_CLOCKFREQ_KHZ). With the two limits equal (PicoDVI) there is
    // nothing to choose; initAll() reconciles the setting with FlashParams.
    vreg_voltage voltage = C1943_VOLTAGE;
    Frens::setOverclockLimits(C1943_CLOCKFREQ_KHZ, C1943_MAX_CLOCKFREQ_KHZ, C1943_VOLTAGE, C1943_MAX_VOLTAGE);
    const Frens::FlashParams *flashParams = (const Frens::FlashParams *)FLASHPARAM_ADDRESS;
    if (Frens::validateFlashParams(*flashParams))
    {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
    }
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);

    printf("==========================================================================================\n");
    printf("pico1943 %s\n", SWVERSION);
    printf("Build date: %s %s\n", __DATE__, __TIME__);
    printf("HW_CONFIG=%d  HSTX=%d  CPU freq: %lu kHz\n", HW_CONFIG, HSTX, (unsigned long)(clock_get_hz(clk_sys) / 1000));
    printf("==========================================================================================\n");

#if C1943_FRAME_STATS
    statsStartCounter();
#endif
    FrensSettings::initSettings(FrensSettings::ARCADE);
    c1943_init(HSTX ? C1943_FMT_RGB555 : C1943_FMT_RGB444, SAMPLERATE);

    // No ROM is ever selected through the browser; the sets are read below.
    // A set waiting to be copied into flash is copied inside initAll(), before
    // the display starts (see "The flash store").
    char dummyRom[FF_MAX_LFN];
    dummyRom[0] = 0;
    clearRestartMark();
    Frens::setPreVideoInitTask(prepareFlash);
    sdOk = Frens::initAll(dummyRom, CPUFreqKHz, 0, 0, AUDIOBUFFERSIZE, false, true);
    Frens::setPreVideoInitTask(nullptr);
    int unused;
    takePendingCopy(&unused); // without a card it was not taken: forget it

    if (sdOk)
    {
        // loadsettings() inside initAll resets every setting when
        // settings.currentDir does not exist. There is no ROM browser to
        // create it, so make it (FR_EXIST later on is fine) and load again.
        f_mkdir("/roms");
        f_mkdir(ROMROOT);
        f_mkdir(ROMROOT "/1943");
        f_mkdir(ROMROOT "/1943KAI");
        FrensSettings::loadsettings();
    }
    // All arcade games share /settings_ARC.dat and with it currentDir. Keep it
    // at /roms/arcade, which every arcade game creates.
    strcpy(settings.currentDir, ROMROOT);
    g_settings_visibility = g_settings_visibility_1943;
    g_available_screen_modes = g_available_screen_modes_1943;
    if (!g_available_screen_modes[static_cast<int>(settings.screenMode)])
        settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);
    // Apply the saved DAC volume now; otherwise it only takes effect after the
    // settings menu has been opened and closed. No-op without a TLV320.
    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    // after initAll(), so after the DAC: see wiipadPoll()
    if (!wiipad_is_connected())
        wiipad_begin();
#endif

#if C1943_SOUND_ON_CORE1
    video_output_set_background_task(soundTask);
    soundOnCore1 = true;
#endif
    printf("Sound board on core%d\n", soundOnCore1 ? 1 : 0);

    openFlashStore(); // without a card initAll() did not; the sets in it still play
    probeSets();
    if (copiedSet >= 0 && available[copiedSet] >= 0)
        startSet(copiedSet); // just copied into flash: started straight away
    else
        enterSets();

    paceFrame(true);
    lastFrameUs = Frens::time_us();
    while (true)
        processPerFrame();
}
