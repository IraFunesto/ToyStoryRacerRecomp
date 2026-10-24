#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"

/* Toy Story Racer - instant memory card.
 *
 * The game's memory card code (the overlay FRONTEND\MEMORY.BIN) is Sony's
 * standard one: _card_info / _card_load / _card_write, whose results arrive
 * as SwCARD / HwCARD events polled with TestEvent, and the BIOS file
 * functions on the "buXY:" devices (_bu_init, firstfile, nextfile, open,
 * read, write, close, erase, format). The BIOS moves one 128-byte sector every two frames,
 * so checking the card took about 7 seconds and every load or save more.
 *
 * This plugin does those kernel calls itself, on the inserted card image and
 * with the card's own file system (header frame, directory frames 1-15 for
 * the fifteen 8 KB blocks, broken-sector list), and signals the event the
 * BIOS signals: every call completes at once, and the card image ends up as
 * the BIOS would have written it. Every other kernel call, and these calls on
 * any other device, still go to the BIOS.
 *
 * The events are the game's polled ones (mode "mark"): delivering one marks
 * every enabled event of that class and spec ready, as DeliverEvent does. */

#define EV_SWCARD     0xF4000001u
#define EV_HWCARD     0xF0000011u
#define EVSP_IOE      0x0004u      /* operation complete */
#define EVSP_TIMOUT   0x0100u      /* no card */
#define EVSP_NEW      0x2000u      /* new card: not written since inserted, or unformatted */
#define EVST_ENABLED  0x2000u
#define EVST_READY    0x4000u
#define EVMD_MARK     0x2000u
#define EVCB_SIZE     0x1Cu

#define O_WRITE       0x0002u
#define O_CREAT       0x0200u
#define O_ASYNC       0x8000u

#define FRAME         128u          /* a card sector */
#define BLOCK_FRAMES  64u           /* 8 KB block */
#define BLOCKS        15u           /* blocks 1-15; block 0 is the file system */
#define ST_FIRST      0x51u         /* directory frame: first block of a file */
#define ST_MIDDLE     0x52u
#define ST_LAST       0x53u
#define ST_FREE       0xA0u         /* 0xA1-0xA3: blocks of a deleted file */
#define NO_NEXT       0xFFFFu
#define NAME_LEN      20u
#define MAX_FD        16

typedef struct {
    int used, slot;
    uint32_t first;                 /* first block, 1-15 */
    uint32_t mode, size, pos;
} File;

static File s_files[MAX_FD];
static int s_new[2];                /* the card's "new" flag: set at power-on, cleared by a write */
static struct {
    int active, slot;
    uint32_t next;                  /* next directory frame to look at */
    char pattern[NAME_LEN + 1];
} s_find;

static uint32_t guest(uint32_t address) { return 0x80000000u | (address & 0x1FFFFFFFu); }

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void checksum(uint8_t* f) {
    uint8_t x = 0;
    for (uint32_t i = 0; i < FRAME - 1u; ++i) x ^= f[i];
    f[FRAME - 1u] = x;
}

/* DeliverEvent(class, spec) for the events the game polls. */
static void deliver(uint32_t cls, uint32_t spec) {
    uint32_t base = psx_mod_read_word(0x80000120u), size = psx_mod_read_word(0x80000124u);
    if (!base || size > 0x1000u * EVCB_SIZE) return;
    base = guest(base);
    for (uint32_t ev = base; ev + EVCB_SIZE <= base + size; ev += EVCB_SIZE)
        if (psx_mod_read_word(ev + 4u) == EVST_ENABLED && psx_mod_read_word(ev) == cls &&
            psx_mod_read_word(ev + 8u) == spec && psx_mod_read_word(ev + 12u) == EVMD_MARK)
            psx_mod_write_word(ev + 4u, EVST_READY);
}

/* A sector the card has listed as broken lives in its replacement (36-55). */
static uint32_t remap(int slot, uint32_t sector) {
    if (sector >= 16u && sector < 56u) return sector;
    uint8_t f[FRAME];
    for (uint32_t i = 0; i < 20u; ++i)
        if (psx_mod_card_read_sector(slot, 16u + i, f) && get32(f) == sector) return 36u + i;
    return sector;
}

/* Every write to a card clears its "new" flag. */
static int sector_write(int slot, uint32_t sector, const uint8_t* f) {
    if (!psx_mod_card_write_sector(slot, sector, f)) return 0;
    s_new[slot & 1] = 0;
    return 1;
}

static int formatted(int slot) {
    uint8_t f[FRAME];
    return psx_mod_card_read_sector(slot, 0u, f) && f[0] == 'M' && f[1] == 'C';
}

static int frame_read(int slot, uint32_t sector, uint8_t* f) {
    return psx_mod_card_read_sector(slot, remap(slot, sector), f);
}

static int frame_write(int slot, uint32_t sector, const uint8_t* f) {
    return sector_write(slot, remap(slot, sector), f);
}

static void frame_name(const uint8_t* f, char* name) {
    memcpy(name, f + 10, NAME_LEN);
    name[NAME_LEN] = '\0';
}

/* "buXY:NAME": the card slot X (multitap port Y = 0 only), NAME copied out;
 * -1 for any other device. */
static int card_path(uint32_t address, char* name) {
    char s[32];
    uint32_t n = 0;
    for (; n + 1u < sizeof s; ++n)
        if (!(s[n] = (char)psx_mod_read_byte(guest(address + n)))) break;
    s[n] = '\0';
    if (n < 5u || s[0] != 'b' || s[1] != 'u' || (s[2] != '0' && s[2] != '1') ||
        s[3] != '0' || s[4] != ':') return -1;
    strncpy(name, s + 5, NAME_LEN);
    name[NAME_LEN] = '\0';
    return s[2] - '0';
}

/* The first block of the file called name, 0 when there is none. */
static uint32_t find_file(int slot, const char* name, uint8_t* f) {
    char have[NAME_LEN + 1];
    for (uint32_t b = 1; b <= BLOCKS; ++b) {
        if (!frame_read(slot, b, f) || get32(f) != ST_FIRST) continue;
        frame_name(f, have);
        if (strcmp(have, name) == 0) return b;
    }
    return 0;
}

/* The card sector holding byte pos of the file starting at block first. */
static uint32_t file_sector(int slot, uint32_t first, uint32_t pos) {
    uint32_t frame = pos / FRAME, b = first;
    uint8_t f[FRAME];
    for (uint32_t k = frame / BLOCK_FRAMES; k; --k) {
        if (!frame_read(slot, b, f)) return 0;
        uint32_t next = (uint32_t)f[8] | (uint32_t)f[9] << 8;
        if (next >= BLOCKS) return 0;
        b = next + 1u;
    }
    return b * BLOCK_FRAMES + frame % BLOCK_FRAMES;
}

/* A0:AB _card_info(port), A0:AC _card_load(port): SwCARD "complete", "new
 * card" while the card has not been written since power-on (and, for
 * _card_load, while it is unformatted), or "time out" for an empty slot. */
static int card_info(struct CPUState* cpu, int load) {
    const uint32_t port = cpu->gpr[4];
    if (port & 0xEFu) return 0;
    const int slot = (int)(port >> 4);
    deliver(EV_SWCARD, !psx_mod_card_present(slot) ? EVSP_TIMOUT
                       : s_new[slot] || (load && !formatted(slot)) ? EVSP_NEW : EVSP_IOE);
    cpu->gpr[2] = 1u;
    return 1;
}

/* A0:70 _bu_init(): the BIOS reads both cards' file systems into its own
 * cache (unused here: every file call is answered from the card) and writes
 * each formatted card's header frame to its write-test frame 63, as done
 * here; an unformatted card is left alone, still "new". */
static int bu_init(struct CPUState* cpu) {
    uint8_t f[FRAME], old[FRAME];
    for (int slot = 0; slot < 2; ++slot) {
        if (!formatted(slot) || !psx_mod_card_read_sector(slot, 0u, f) ||
            !psx_mod_card_read_sector(slot, 63u, old)) continue;
        if (memcmp(f, old, FRAME) == 0) s_new[slot] = 0;
        else if (sector_write(slot, 63u, f)) psx_mod_card_flush(slot);
    }
    cpu->gpr[2] = 1u;
    return 1;
}

/* B0:4F _card_read / B0:4E _card_write(port, sector, buffer): one raw sector,
 * HwCARD "complete" or "time out". */
static int card_sector(struct CPUState* cpu, int write) {
    uint32_t port = cpu->gpr[4], sector = cpu->gpr[5], buf = cpu->gpr[6];
    if ((port & 0xEFu) || sector >= 1024u) return 0;
    const int slot = (int)(port >> 4);
    uint8_t f[FRAME], old[FRAME];
    int ok = psx_mod_card_read_sector(slot, sector, old);
    if (ok && write) {
        for (uint32_t i = 0; i < FRAME; ++i) f[i] = psx_mod_read_byte(guest(buf + i));
        if (memcmp(f, old, FRAME) == 0) s_new[slot] = 0;
        else if ((ok = sector_write(slot, sector, f))) psx_mod_card_flush(slot);
    } else if (ok) {
        for (uint32_t i = 0; i < FRAME; ++i) psx_mod_write_byte(guest(buf + i), old[i]);
    }
    deliver(EV_HWCARD, ok ? EVSP_IOE : EVSP_TIMOUT);
    cpu->gpr[2] = 1u;
    return 1;
}

/* B0:32 open(path, mode): mode 0x200 creates the file with (mode >> 16)
 * blocks, which must not exist yet. Returns a file number or -1. */
static int bu_open(struct CPUState* cpu) {
    char name[NAME_LEN + 1];
    const int slot = card_path(cpu->gpr[4], name);
    if (slot < 0) return 0;
    const uint32_t mode = cpu->gpr[5];
    cpu->gpr[2] = 0xFFFFFFFFu;
    int fd = 2;
    while (fd < MAX_FD && s_files[fd].used) ++fd;
    if (fd == MAX_FD || !name[0] || !psx_mod_card_present(slot)) return 1;
    uint8_t f[FRAME];
    uint32_t first = find_file(slot, name, f), size = first ? get32(f + 4) : 0u;
    if (mode & O_CREAT) {
        uint32_t blocks = mode >> 16 ? mode >> 16 : 1u, list[BLOCKS], n = 0;
        if (first || blocks > BLOCKS) return 1;
        for (uint32_t b = 1; b <= BLOCKS && n < blocks; ++b)
            if (frame_read(slot, b, f) && (get32(f) & 0xF0u) == ST_FREE) list[n++] = b;
        if (n < blocks) return 1;
        for (uint32_t i = 0; i < n; ++i) {
            memset(f, 0, FRAME);
            put32(f, i == 0 ? ST_FIRST : i + 1 == n ? ST_LAST : ST_MIDDLE);
            if (i == 0) {
                put32(f + 4, blocks * BLOCK_FRAMES * FRAME);
                memcpy(f + 10, name, strlen(name));
            }
            const uint32_t next = i + 1 < n ? list[i + 1] - 1u : NO_NEXT;
            f[8] = (uint8_t)next; f[9] = (uint8_t)(next >> 8);
            checksum(f);
            if (!frame_write(slot, list[i], f)) return 1;
        }
        psx_mod_card_flush(slot);
        first = list[0];
        size = blocks * BLOCK_FRAMES * FRAME;
    } else if (!first) {
        return 1;
    }
    s_files[fd] = (File){ 1, slot, first, mode, size, 0 };
    cpu->gpr[2] = (uint32_t)fd;
    return 1;
}

/* B0:34 read / B0:35 write(fd, buffer, bytes): whole sectors from the file
 * position; returns the bytes moved or -1. */
static int bu_transfer(struct CPUState* cpu, int write) {
    const uint32_t fd = cpu->gpr[4], buf = cpu->gpr[5];
    if (fd >= (uint32_t)MAX_FD || !s_files[fd].used) return 0;
    File* fl = &s_files[fd];
    uint32_t n = cpu->gpr[6];
    cpu->gpr[2] = 0xFFFFFFFFu;
    if (n % FRAME || (write && !(fl->mode & O_WRITE))) return 1;
    n = fl->pos >= fl->size ? 0u : n < fl->size - fl->pos ? n : fl->size - fl->pos;
    uint8_t f[FRAME];
    for (uint32_t done = 0; done < n; done += FRAME) {
        const uint32_t sector = file_sector(fl->slot, fl->first, fl->pos + done);
        if (!sector) return 1;
        if (write) {
            for (uint32_t i = 0; i < FRAME; ++i) f[i] = psx_mod_read_byte(guest(buf + done + i));
            if (!frame_write(fl->slot, sector, f)) return 1;
        } else {
            if (!frame_read(fl->slot, sector, f)) return 1;
            for (uint32_t i = 0; i < FRAME; ++i) psx_mod_write_byte(guest(buf + done + i), f[i]);
        }
    }
    fl->pos += n;
    if (write) psx_mod_card_flush(fl->slot);
    if (n) deliver(EV_HWCARD, EVSP_IOE);
    if (fl->mode & O_ASYNC) deliver(EV_SWCARD, EVSP_IOE);
    cpu->gpr[2] = n;
    return 1;
}

/* B0:36 close(fd) */
static int bu_close(struct CPUState* cpu) {
    const uint32_t fd = cpu->gpr[4];
    if (fd >= (uint32_t)MAX_FD || !s_files[fd].used) return 0;
    s_files[fd].used = 0;
    cpu->gpr[2] = fd;
    return 1;
}

/* The next directory entry matching the firstfile pattern ('?' one
 * character, '*' the rest), written as a DIRENTRY: name[20], attr, size,
 * next, head (first sector), system[4]. Returns the entry address or 0. */
static int name_match(const char* pattern, const char* name) {
    for (;; ++pattern, ++name) {
        if (*pattern == '*') return 1;
        if (!*pattern) return !*name;
        if (!*name || (*pattern != '?' && *pattern != *name)) return 0;
    }
}

static uint32_t find_next(uint32_t entry) {
    uint8_t f[FRAME];
    char name[NAME_LEN + 1];
    for (; s_find.active && s_find.next <= BLOCKS; ++s_find.next) {
        const uint32_t b = s_find.next;
        if (!frame_read(s_find.slot, b, f) || get32(f) != ST_FIRST) continue;
        frame_name(f, name);
        if (!name_match(s_find.pattern, name)) continue;
        ++s_find.next;
        for (uint32_t i = 0; i < NAME_LEN; ++i) psx_mod_write_byte(guest(entry + i), (uint8_t)name[i]);
        psx_mod_write_word(guest(entry + 0x14u), ST_FIRST & 0xF0u);
        psx_mod_write_word(guest(entry + 0x18u), get32(f + 4));
        psx_mod_write_word(guest(entry + 0x1Cu), 0u);
        psx_mod_write_word(guest(entry + 0x20u), b * BLOCK_FRAMES);
        psx_mod_write_word(guest(entry + 0x24u), 0u);
        return entry;
    }
    return 0u;
}

/* B0:42 firstfile(pattern, entry) */
static int bu_firstfile(struct CPUState* cpu) {
    const int slot = card_path(cpu->gpr[4], s_find.pattern);
    s_find.active = slot >= 0 && psx_mod_card_present(slot);
    if (slot < 0) return 0;
    s_find.slot = slot;
    s_find.next = 1;
    cpu->gpr[2] = find_next(cpu->gpr[5]);
    return 1;
}

/* B0:43 nextfile(entry), after a firstfile on a card */
static int bu_nextfile(struct CPUState* cpu) {
    if (!s_find.active) return 0;
    cpu->gpr[2] = find_next(cpu->gpr[4]);
    return 1;
}

/* B0:45 erase(path): the file's blocks become "deleted". Returns 1 or 0. */
static int bu_erase(struct CPUState* cpu) {
    char name[NAME_LEN + 1];
    const int slot = card_path(cpu->gpr[4], name);
    if (slot < 0) return 0;
    uint8_t f[FRAME];
    uint32_t b = psx_mod_card_present(slot) ? find_file(slot, name, f) : 0u;
    cpu->gpr[2] = b ? 1u : 0u;
    for (uint32_t k = 0; b && k < BLOCKS; ++k) {
        if (!frame_read(slot, b, f)) break;
        const uint32_t next = (uint32_t)f[8] | (uint32_t)f[9] << 8;
        put32(f, ST_FREE | (get32(f) & 0x0Fu));
        checksum(f);
        frame_write(slot, b, f);
        b = next < BLOCKS ? next + 1u : 0u;
    }
    psx_mod_card_flush(slot);
    return 1;
}

/* B0:41 format(path): an empty file system: the header, the free directory
 * frames and an empty broken-sector list (frames 0-35, as the BIOS writes
 * them). Returns 1 or 0. */
static int bu_format(struct CPUState* cpu) {
    char name[NAME_LEN + 1];
    const int slot = card_path(cpu->gpr[4], name);
    if (slot < 0) return 0;
    cpu->gpr[2] = 0u;
    if (!psx_mod_card_present(slot)) return 1;
    uint8_t f[FRAME];
    for (uint32_t s = 0; s < 36u; ++s) {
        memset(f, 0, FRAME);
        if (s == 0) {
            f[0] = 'M'; f[1] = 'C';
        } else {
            put32(f, s <= BLOCKS ? ST_FREE : 0xFFFFFFFFu);
            f[8] = f[9] = 0xFF;
        }
        checksum(f);
        if (!sector_write(slot, s, f)) return 1;
    }
    psx_mod_card_flush(slot);
    cpu->gpr[2] = 1u;
    return 1;
}

static int card_call(struct CPUState* cpu, uint32_t vector, uint32_t function) {
    if (vector == 0xA0u)
        return function == 0x70u ? bu_init(cpu)
             : function == 0xABu ? card_info(cpu, 0)
             : function == 0xACu ? card_info(cpu, 1) : 0;
    if (vector != 0xB0u) return 0;
    switch (function) {
    case 0x32u: return bu_open(cpu);
    case 0x34u: return bu_transfer(cpu, 0);
    case 0x35u: return bu_transfer(cpu, 1);
    case 0x36u: return bu_close(cpu);
    case 0x41u: return bu_format(cpu);
    case 0x42u: return bu_firstfile(cpu);
    case 0x43u: return bu_nextfile(cpu);
    case 0x45u: return bu_erase(cpu);
    case 0x4Eu: return card_sector(cpu, 1);
    case 0x4Fu: return card_sector(cpu, 0);
    default:    return 0;
    }
}

static void memcard_activate(void) {
    memset(s_files, 0, sizeof s_files);
    memset(&s_find, 0, sizeof s_find);
    s_new[0] = s_new[1] = 1;
    psx_mod_set_bios_call_hook(card_call);
    fprintf(stdout, "TSR INSTANT MEMORY CARD ACTIVATED\n");
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_memcard_plugin) {
    (void)psx_mod_register_activation_plugin("toystoryracer.memcard", memcard_activate);
}
