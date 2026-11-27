#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "mod_memory.h"

/* Toy Story Racer - instant loading.
 *
 * Every data load of the game goes through one loader (0x80064E34: mute the
 * drive, CdSearchFile, CdControlB(CdlSeekL), CdRead, CdReadSync), and through
 * the PsyQ CD library linked into the one EXE. This plugin answers four of
 * those calls itself:
 *
 *   CdSearchFile(fp, name)         position and size of the file on the disc
 *   CdControlB(Setloc/SeekL/SeekP) to a data file: remembered, no drive
 *                                  command, no seek time
 *   CdRead(n, buf, mode)           the sectors, as the drive delivers them,
 *                                  written to RAM as a completed CD DMA
 *   CdReadSync(mode, result)       0 (complete) after such a read
 *
 * so every load is instant and the game's own loader code still receives
 * exactly the bytes the drive would deliver (game.toml [runtime] disc_folder:
 * the disc itself is the extracted files next to the game). The streams --
 * music and character voices (XA), videos (STR) -- still play through the
 * drive in real time, from the positions answered here: positioning to them
 * and every other command (Setmode, Pause, Mute, ReadS...) reach it.
 *
 * The hooks are function-entry hooks listed in game.toml
 * (mod_function_entry_funcs): an answered call finishes the function with
 * psx_mod_finish_function(), the others run the library code unchanged. */

#define FN_SEARCH    0x80096168u   /* CdSearchFile (two lui/lw before its prologue) */
#define FN_CONTROLB  0x80099428u   /* CdControlB */
#define FN_READ      0x80099DE8u   /* CdRead */
#define FN_SYNC      0x80099F84u   /* CdReadSync */
#define STAT_IDLE    0x02u         /* drive status byte: motor on */
#define MAX_FILES    64

/* What the hooks remember between the game's calls -- the files the game
 * looked up (their sectors, whether a stream), the position of the last
 * answered seek, whether the last CdRead was answered here -- decides which
 * drive commands still reach the drive, so the disc's timing (and from it the
 * XA voices and the sound chip) depends on it. It lives in the mod memory:
 * saved with the game's state, so the PCs of an online game (all loading the
 * host's state) answer the same calls, and a rollback takes it back with the
 * rest (kept on the host, each PC's own history since its start made the
 * PCs' discs, voices and sound part). Allocated at the first use, after every
 * plugin's activation (the frame-rate word must be the first allocation). */
#define ST_NFOUND    0u
#define ST_POS       4u
#define ST_POS_VALID 8u
#define ST_READ_DONE 12u
#define ST_FOUND     16u            /* MAX_FILES x (lba, sectors, stream) */
#define ST_BYTES     (ST_FOUND + 12u * MAX_FILES)
static uint32_t s_mem;              /* its guest address (0: not yet, or no mod memory) */
static uint32_t s_host[ST_BYTES / 4u];   /* without mod memory: here */

static uint32_t st_get(uint32_t off) {
    if (!s_mem) s_mem = psx_mod_memory_alloc(ST_BYTES, 4u);
    return s_mem ? psx_mod_read_word(s_mem + off) : s_host[off / 4u];
}
static void st_set(uint32_t off, uint32_t v) {
    if (!s_mem) s_mem = psx_mod_memory_alloc(ST_BYTES, 4u);
    if (s_mem) { if (psx_mod_read_word(s_mem + off) != v) psx_mod_write_word(s_mem + off, v); }
    else s_host[off / 4u] = v;
}
#define F_LBA(i)     (ST_FOUND + 12u * (uint32_t)(i))
#define F_SECTORS(i) (F_LBA(i) + 4u)
#define F_STREAM(i)  (F_LBA(i) + 8u)

static uint8_t* s_buf;
static uint32_t s_buf_cap;
static char s_voice;              /* the voices' file letter, 0 = the game's own */
static int s_voice_checked;       /* the language's first file looked up on the disc */

static void remember(uint32_t lba, uint32_t size, int stream) {
    uint32_t sectors = (size + 2047u) / 2048u;
    uint32_t n = st_get(ST_NFOUND);
    if (n > MAX_FILES) n = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (st_get(F_LBA(i)) == lba) { st_set(F_SECTORS(i), sectors); st_set(F_STREAM(i), (uint32_t)stream); return; }
    if (n == MAX_FILES) n = 0;   /* a game this size looks up far fewer */
    st_set(F_LBA(n), lba);
    st_set(F_SECTORS(n), sectors);
    st_set(F_STREAM(n), (uint32_t)stream);
    st_set(ST_NFOUND, n + 1u);
}

/* 1 when the sector starts or lies inside a data file the game looked up */
static int data_sector(uint32_t lba) {
    uint32_t n = st_get(ST_NFOUND);
    if (n > MAX_FILES) n = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t l = st_get(F_LBA(i));
        if (lba >= l && lba - l < st_get(F_SECTORS(i))) return !st_get(F_STREAM(i));
    }
    return 0;
}

static uint32_t bcd_to_bin(uint8_t v) { return (uint32_t)(v >> 4) * 10u + (v & 15u); }
static uint8_t bin_to_bcd(uint32_t v) { return (uint8_t)(((v / 10u) << 4) | (v % 10u)); }

static int ends_with(const char* s, const char* tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && strcmp(s + n - m, tail) == 0;
}

/* CdSearchFile(CdlFILE* fp, char* name), name like "\COURSE_A\HOUSE.DAT;1".
 * CdlFILE: CdlLOC pos (minute, second, sector in BCD, track), u_long size,
 * char name[16]. Returns fp. */
static void search_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t fp = cpu->gpr[4], name = cpu->gpr[5];
    char want[128];
    size_t n = 0;
    for (uint32_t a = name; n + 1 < sizeof want; ++a) {
        char c = (char)psx_mod_read_byte(a);
        if (!c || c == ';') break;
        if (c == '\\') c = '/';
        if (c == '/' && n == 0) continue;
        want[n++] = (char)toupper((unsigned char)c);
    }
    want[n] = '\0';
    uint32_t lba = 0, size = 0;
    /* The character voices in the chosen language: the game plays a line as
     * (file, XA channel) from AUDIO/x_CHAR1..4.XA, x = E (USA), I, S, D
     * (SLES-03398: Italian, Spanish, Dutch by language), F, G (SLES-03397:
     * French, German); every language's files
     * hold the same lines on the same channels. The other disc's files are
     * added to the extracted disc's layout. A language without a file (the
     * Spanish disc has CHAR1 only) answers it empty: those lines are silent,
     * as on that disc. Without the language's voices at all (the other disc
     * not installed) the game keeps its own. */
    if (s_voice && !s_voice_checked) {
        char first[] = "AUDIO/x_CHAR1.XA";
        uint32_t l, sz;
        first[6] = s_voice;
        s_voice_checked = 1;
        if (!psx_mod_disc_file_extent(first, &l, &sz)) {
            /* a language without its disc: the English voices, else the
             * game's own */
            first[6] = 'E';
            s_voice = psx_mod_disc_file_extent(first, &l, &sz) ? 'E' : 0;
        }
    }
    if (s_voice && n == 16 && strncmp(want, "AUDIO/", 6) == 0 && strncmp(want + 7, "_CHAR", 5) == 0 && want[12] >= '1' && want[12] <= '4' &&
        strcmp(want + 13, ".XA") == 0 && want[6] != s_voice) {
        char other[17];
        memcpy(other, want, sizeof other);
        other[6] = s_voice;
        if (!psx_mod_disc_file_extent(other, &lba, &size)) {
            for (uint32_t i = 0; i < 24u; ++i) psx_mod_write_byte(fp + i, 0u);
            cpu->gpr[2] = fp;
            psx_mod_finish_function();
            return;
        }
        fprintf(stdout, "TSR VOICES: %s from %s (LBA %u)\n", want, other, lba);
        memcpy(want, other, sizeof other);
    }
    if (!n || (!lba && !psx_mod_disc_file_extent(want, &lba, &size))) {
        st_set(ST_POS_VALID, 0);   /* not found: the library searches itself */
        return;
    }
    const int stream = ends_with(want, ".XA") || ends_with(want, ".STR");
    remember(lba, size, stream);
    uint32_t msf = lba + 150u;
    psx_mod_write_byte(fp + 0u, bin_to_bcd(msf / 4500u));
    psx_mod_write_byte(fp + 1u, bin_to_bcd(msf / 75u % 60u));
    psx_mod_write_byte(fp + 2u, bin_to_bcd(msf % 75u));
    psx_mod_write_byte(fp + 3u, 0u);
    psx_mod_write_word(fp + 4u, size);
    const char* base = strrchr(want, '/');
    base = base ? base + 1 : want;
    char fname[16];
    snprintf(fname, sizeof fname, "%s;1", base);
    size_t len = strlen(fname);
    for (uint32_t i = 0; i < 16u; ++i)
        psx_mod_write_byte(fp + 8u + i, i < len ? (uint8_t)fname[i] : 0u);
    cpu->gpr[2] = fp;
    psx_mod_finish_function();
}

/* CdControlB(com, param, result): positioning to a data file only. The drive
 * is not told: the next CdRead is answered here. Returns 1. */
static void controlb_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t com = cpu->gpr[4] & 0xFFu, param = cpu->gpr[5], result = cpu->gpr[6];
    if ((com != 0x02u && com != 0x15u && com != 0x16u) || !param) return;
    uint32_t msf = bcd_to_bin(psx_mod_read_byte(param)) * 4500u +
                   bcd_to_bin(psx_mod_read_byte(param + 1u)) * 75u +
                   bcd_to_bin(psx_mod_read_byte(param + 2u));
    if (msf < 150u || !data_sector(msf - 150u)) {
        st_set(ST_POS_VALID, 0);
        return;
    }
    st_set(ST_POS, msf - 150u);
    st_set(ST_POS_VALID, 1);
    if (result) psx_mod_write_byte(result, STAT_IDLE);
    cpu->gpr[2] = 1u;
    psx_mod_finish_function();
}

/* CdRead(sectors, buf, mode): 2048-byte sectors from the remembered
 * position, delivered as the CD DMA would deliver them. Returns 1. */
static void read_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t n = cpu->gpr[4], buf = cpu->gpr[5], mode = cpu->gpr[6];
    if (!st_get(ST_POS_VALID) || !n || n > 1024u || (mode & 0x30u) || (buf & 3u)) {
        /* whole-sector modes and reads we did not position: the drive */
        st_set(ST_READ_DONE, 0);
        return;
    }
    const uint32_t pos = st_get(ST_POS);
    uint32_t bytes = n * 2048u;
    if (bytes > s_buf_cap) {
        free(s_buf);
        s_buf = (uint8_t*)malloc(bytes);
        s_buf_cap = s_buf ? bytes : 0u;
        if (!s_buf) return;
    }
    if (!psx_mod_read_cd_sectors(pos, n, s_buf) ||
        !psx_mod_dma_write_ram(buf, s_buf, bytes, (int)pos)) {
        fprintf(stdout, "TSR LOADING: read of %u sectors at LBA %u failed, left to the drive\n",
                n, pos);
        st_set(ST_POS_VALID, 0);
        st_set(ST_READ_DONE, 0);
        return;
    }
    st_set(ST_POS, pos + n);
    st_set(ST_READ_DONE, 1);
    cpu->gpr[2] = 1u;
    psx_mod_finish_function();
}

/* CdReadSync(mode, result): 0 = complete, after a read answered here. */
static void sync_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!st_get(ST_READ_DONE)) return;
    if (cpu->gpr[5]) psx_mod_write_byte(cpu->gpr[5], STAT_IDLE);
    cpu->gpr[2] = 0u;
    psx_mod_finish_function();
}

/* The streams too: a disc of files has no head to move, so the drive starts
 * a voice or a piece of music at once instead of the console's seek and spin
 * up (up to about 70 ms: a character's voice on the selection screen came
 * late); the sectors still arrive at the drive's rate. */
extern int g_psx_cd_fast_stream_start;

int g_tsr_files_on;   /* instant loading is on (tsr_online.c compares it with the other PC) */
/* the voices' file letter (0: the game's own): every PC of an online game
 * needs the same (tsr_online.c compares it) */
char tsr_files_voice(void) { return s_voice; }
static void loading_activate(void) {
    g_tsr_files_on = 1;
    char lang[16];
    memset(s_host, 0, sizeof s_host);   /* (the mod memory starts zeroed) */
    g_psx_cd_fast_stream_start = 1;
    /* the voices follow the quick start's language (none: the game's own) */
    s_voice = 0;
    if (psx_mod_option_value("toystoryracer.widescreen", "quick-start", "language", lang, sizeof lang)) {
        if (!strcmp(lang, "en")) s_voice = 'E';
        else if (!strcmp(lang, "fr")) s_voice = 'F';
        else if (!strcmp(lang, "de")) s_voice = 'G';
        else if (!strcmp(lang, "it")) s_voice = 'I';
        else if (!strcmp(lang, "es")) s_voice = 'S';
        else if (!strcmp(lang, "nl")) s_voice = 'D';
    }
    s_voice_checked = 0;
    fprintf(stdout, "TSR INSTANT LOADING ACTIVATED\n");
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_loading_plugin) {
    (void)psx_mod_register_activation_plugin("toystoryracer.files", loading_activate);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_SEARCH, search_entry);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_CONTROLB, controlb_entry);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_READ, read_entry);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_SYNC, sync_entry);
}
