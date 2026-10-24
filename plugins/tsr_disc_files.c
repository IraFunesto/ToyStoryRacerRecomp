#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "cpu_state.h"
#include "mod_plugins.h"

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

/* The files the game looked up: their sectors, and whether a stream. */
typedef struct {
    uint32_t lba, sectors;
    int stream;
} Found;

static Found s_found[MAX_FILES];
static int s_nfound;
static uint32_t s_pos;            /* position of the last answered seek */
static int s_pos_valid;
static int s_read_done;           /* the last CdRead was answered here */
static uint8_t* s_buf;
static uint32_t s_buf_cap;

static void remember(uint32_t lba, uint32_t size, int stream) {
    uint32_t sectors = (size + 2047u) / 2048u;
    for (int i = 0; i < s_nfound; ++i)
        if (s_found[i].lba == lba) { s_found[i].sectors = sectors; s_found[i].stream = stream; return; }
    if (s_nfound == MAX_FILES) s_nfound = 0;   /* a game this size looks up far fewer */
    s_found[s_nfound].lba = lba;
    s_found[s_nfound].sectors = sectors;
    s_found[s_nfound].stream = stream;
    ++s_nfound;
}

/* 1 when the sector starts or lies inside a data file the game looked up */
static int data_sector(uint32_t lba) {
    for (int i = 0; i < s_nfound; ++i)
        if (lba >= s_found[i].lba && lba - s_found[i].lba < s_found[i].sectors)
            return !s_found[i].stream;
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
    if (!n || !psx_mod_disc_file_extent(want, &lba, &size)) {
        s_pos_valid = 0;   /* not found: the library searches itself */
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
        s_pos_valid = 0;
        return;
    }
    s_pos = msf - 150u;
    s_pos_valid = 1;
    if (result) psx_mod_write_byte(result, STAT_IDLE);
    cpu->gpr[2] = 1u;
    psx_mod_finish_function();
}

/* CdRead(sectors, buf, mode): 2048-byte sectors from the remembered
 * position, delivered as the CD DMA would deliver them. Returns 1. */
static void read_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t n = cpu->gpr[4], buf = cpu->gpr[5], mode = cpu->gpr[6];
    if (!s_pos_valid || !n || n > 1024u || (mode & 0x30u) || (buf & 3u)) {
        /* whole-sector modes and reads we did not position: the drive */
        s_read_done = 0;
        return;
    }
    uint32_t bytes = n * 2048u;
    if (bytes > s_buf_cap) {
        free(s_buf);
        s_buf = (uint8_t*)malloc(bytes);
        s_buf_cap = s_buf ? bytes : 0u;
        if (!s_buf) return;
    }
    if (!psx_mod_read_cd_sectors(s_pos, n, s_buf) ||
        !psx_mod_dma_write_ram(buf, s_buf, bytes, (int)s_pos)) {
        fprintf(stdout, "TSR LOADING: read of %u sectors at LBA %u failed, left to the drive\n",
                n, s_pos);
        s_pos_valid = s_read_done = 0;
        return;
    }
    s_pos += n;
    s_read_done = 1;
    cpu->gpr[2] = 1u;
    psx_mod_finish_function();
}

/* CdReadSync(mode, result): 0 = complete, after a read answered here. */
static void sync_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_read_done) return;
    if (cpu->gpr[5]) psx_mod_write_byte(cpu->gpr[5], STAT_IDLE);
    cpu->gpr[2] = 0u;
    psx_mod_finish_function();
}

static void loading_activate(void) {
    s_nfound = 0;
    s_pos_valid = s_read_done = 0;
    fprintf(stdout, "TSR INSTANT LOADING ACTIVATED\n");
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_loading_plugin) {
    (void)psx_mod_register_activation_plugin("toystoryracer.files", loading_activate);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_SEARCH, search_entry);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_CONTROLB, controlb_entry);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_READ, read_entry);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.files", FN_SYNC, sync_entry);
}
