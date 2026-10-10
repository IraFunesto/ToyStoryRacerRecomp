#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "mod_memory.h"

/* Toy Story Racer widescreen: one opt-in feature with an "aspect" choice.
 * Fit starts from 16:9 and follows the window shape with no upper limit.
 * Fixed views use the same native-wide renderer. */
#define PKG "toystoryracer.widescreen"
#define FEATURE "widescreen"

/* The runtime only lets the driver vsync when the monitor refresh equals the
 * guest rate (within 2%); on a 120/144/165 Hz panel it presents unsynced and
 * fast motion tears. Keep the swap synced anyway: the runtime's wall-clock
 * pacer still holds the game speed and every frame waits for the next panel
 * refresh. OpenGL only (a no-op without a GL context). Not part of the mod
 * API, but this trusted plugin is linked into the same executable and VBlank
 * callbacks run on the thread that owns the GL context. */
extern void gl_renderer_set_swap_interval(int interval);
#define VSYNC_REASSERT_VBLANKS 60u
static int s_force_vsync = 0;
static uint32_t s_vblank;

/* CPU overclock (psx_cycles.c, percent). The wider view makes the game build
 * more geometry per frame; in two-player races the stock CPU time no longer
 * fits a frame and the game drops below 25 fps. Devices keep real time, so
 * game speed and music are unchanged; only the CPU gets more time per frame. */
extern uint32_t g_psx_cpu_overclock_pct;
extern uint32_t g_psx_cpu_overclock_override;   /* debug server, 0 = none */
/* Only races need it. Menus wait in tight loops, so an overclock there only
 * costs host time (at 400% the menus slowed down). A race draws its progress
 * bar every frame (also before the start and in split screen); the overclock
 * follows that and drops back to 100% within a second of leaving it.
 * Two-player Andy's House behind the sofa needs about 600% for 25 fps
 * (300%: 17, 500%: 24, 700%: 25); the recompiled code keeps up with that. */
#define OVERCLOCK_HOLD_VBLANKS 50u
static uint32_t s_overclock_pct = 100u;
static uint32_t s_last_race_vblank;
static int s_race_seen = 0;

/* Music changes. When the race music switches (countdown, laps) the game
 * pauses the CD and its main loop waits for the drive's "paused" reply, which
 * on hardware takes about two video frames: the game drops one frame each
 * time (the small hitch about two seconds into a race). cdrom.c lets a mod
 * cap that wait; 5000 cycles is what the drive answers when already paused.
 * Measured in 2P races: 2-3 hitches per 25 s with the hardware wait, 0 with
 * the cap. */
extern int g_psx_cd_pause_cycles_cap;

/* Native scenery renderer (tsr_native_scene.c), hooked at the section draw. */
#define SECTION_DRAW_FN 0x80043538u
void tsr_native_scene_section_draw(struct CPUState *cpu, uint32_t address);
void tsr_native_scene_builder_entry(struct CPUState *cpu, uint32_t address);
void tsr_native_scene_debug_vblank(void);
unsigned tsr_native_scene_debug_byte(unsigned off);
void tsr_native_model_draw(struct CPUState *cpu, uint32_t address);
void tsr_native_sprite_list_draw(struct CPUState *cpu, uint32_t address);
#define SECTION_LIST_FN 0x80011568u   /* per-view visible-section builder (races) */
#define SECTION_LIST_FN2 0x80010000u  /* the same, levels without areas */
void tsr_native_scene_activate(int enabled, int draw_distance);
int g_tsr_racing;   /* read by the native scenery renderer */
#define INTRO_STAGE 0x800A96B8u
static int s_skip_intros, s_intro_tapping;

/* Frame rate: game.toml makes the main loop's wait threshold (VBlanks per
 * frame, 2 = 25 fps on PAL) the first mod-memory word + 3; the game is
 * delta-timed, so 1 VBlank per frame (50 fps) keeps the game speed. The word
 * is (re)written every VBlank: -2 (50 fps) everywhere with the option on
 * (menus, previews, races), -1 (25 fps) with it off. Two-player races stay
 * at 25 unless the option is "50_2p": two views per frame at 50 fps need a
 * fast PC. A frame that needs longer simply takes two VBlanks, as before. */
#define FPS_WORD_ADDR 0x9F000000u
static uint32_t s_fps_word;
static int s_fps50, s_fps50_2p;   /* 50 fps; also in two-player races */
#define CD_PAUSE_FAST_CYCLES 5000

/* ---- race progress bar (HUD) size ---------------------------------------- */

/* The only in-race HUD is the progress bar at the bottom centre: a rope of
 * SPRTs and the racers' faces, flag and position label above it.
 *   0x80038364  draws the faces/flag/label (POLY_FT4 quads, built inline)
 *   0x80039E30  draws the rope: eight segments and two end caps, each through
 *               DrawSprite 0x8004DE64(x, y, u, v, w, h, r, g, b, tpage index,
 *               clut offset, mode, ot) -- a DR_TPAGE plus a 1:1 SPRT.
 * The master HUD routine calls the first and then the second, so the packets
 * written between their entries are the faces. A SPRT cannot be scaled, so a
 * rope segment is drawn here as a POLY_FT4 and the game's own call is left to
 * emit its DR_TPAGE and a zero-size (invisible) SPRT. Everything shrinks
 * about the bar's centre (x 256) and its base line (the rope function's
 * "addiu s3,zero,base" immediate, 207 or 215 with the lower-bar patch). */
#define HUD_FACES_FN    0x80038364u
#define HUD_ROPE_FN     0x80039E30u
#define HUD_ROPE_END    0x8003A048u
#define HUD_ROPE_BASE   0x80039E40u   /* addiu s3,zero,base */
#define DRAW_SPRITE_FN  0x8004DE64u
#define PRIM_CURSOR     0x800A9F4Cu   /* next free primitive */
#define PRIM_LIMIT      0x800A9DF8u   /* end of the current primitive buffer */
#define SPLIT_SCREEN    0x800A9D34u   /* byte, nonzero in two-player races */
#define TPAGE_TABLE     0x800DB070u   /* 16-byte entries: tpage, clut */
#define HUD_CENTRE_X    256

static int s_hud_percent = 100;
static uint32_t s_faces_start;        /* prim cursor at HUD_FACES_FN entry */

static int ram_address(uint32_t a) {
    a &= 0x1FFFFFFFu;
    return a >= 0x10000u && a < 0x200000u;
}

static int hud_active(void) {
    return s_hud_percent < 100 && psx_mod_read_byte(SPLIT_SCREEN) == 0u;
}

static int32_t hud_base_y(void) {
    return (int16_t)psx_mod_read_half(HUD_ROPE_BASE);
}

static int32_t scale_round(int32_t v) {
    int32_t q = v * s_hud_percent;
    return q >= 0 ? (q + 50) / 100 : -((-q + 50) / 100);
}

/* Edges, not lengths, are scaled so pieces that share an edge stay flush. */
static int32_t scale_x(int32_t x) { return HUD_CENTRE_X + scale_round(x - HUD_CENTRE_X); }
static int32_t scale_y(int32_t y, int32_t base) { return base + scale_round(y - base); }

/* libgpu AddPrim: link packet p in front of the OT slot. */
static void add_prim(uint32_t ot, uint32_t p) {
    uint32_t pt = psx_mod_read_word(p), ott = psx_mod_read_word(ot);
    psx_mod_write_word(p, (pt & 0xFF000000u) | (ott & 0x00FFFFFFu));
    psx_mod_write_word(ot, (ott & 0xFF000000u) | (p & 0x00FFFFFFu));
}

static void tsr_faces_entry(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    s_last_race_vblank = s_vblank;
    s_race_seen = 1;
    s_faces_start = hud_active() ? psx_mod_read_word(PRIM_CURSOR) : 0u;
}

static void scale_xy_word(uint32_t a, int32_t base) {
    uint32_t w = psx_mod_read_word(a);
    int32_t x = (int16_t)(w & 0xFFFFu), y = (int16_t)(w >> 16);
    x = scale_x(x);
    y = scale_y(y, base);
    psx_mod_write_word(a, ((uint32_t)y << 16) | ((uint32_t)x & 0xFFFFu));
}

/* Scale every 2D packet the faces routine wrote. Only packets around the bar
 * move: the routine may also draw labels elsewhere on screen. */
static void scale_faces(uint32_t start, uint32_t end) {
    int32_t base = hud_base_y();
    uint32_t a = start;
    for (int guard = 0; a < end && guard < 512; ++guard) {
        uint32_t len = psx_mod_read_byte(a + 3u);
        uint32_t next = a + 4u + 4u * len;
        if (len == 0u || len > 16u || next > end) { a = next; if (len > 16u) break; continue; }
        uint32_t cmd = psx_mod_read_byte(a + 7u);
        uint32_t offs[4];
        int n = 0, tile = 0;
        switch (cmd & 0xFCu) {
        case 0x20: offs[0] = 8; offs[1] = 12; offs[2] = 16; n = 3; break;
        case 0x24: offs[0] = 8; offs[1] = 16; offs[2] = 24; n = 3; break;
        case 0x28: offs[0] = 8; offs[1] = 12; offs[2] = 16; offs[3] = 20; n = 4; break;
        case 0x2C: offs[0] = 8; offs[1] = 16; offs[2] = 24; offs[3] = 32; n = 4; break;
        case 0x30: offs[0] = 8; offs[1] = 16; offs[2] = 24; n = 3; break;
        case 0x34: offs[0] = 8; offs[1] = 20; offs[2] = 32; n = 3; break;
        case 0x38: offs[0] = 8; offs[1] = 16; offs[2] = 24; offs[3] = 32; n = 4; break;
        case 0x3C: offs[0] = 8; offs[1] = 20; offs[2] = 32; offs[3] = 44; n = 4; break;
        case 0x60: offs[0] = 8; n = 1; tile = 1; break;
        default: break;
        }
        if (n && 4u + 4u * len >= offs[n - 1] + 4u) {
            int32_t ymin = 0x7FFF, ymax = -0x8000;
            for (int i = 0; i < n; ++i) {
                int32_t y = (int16_t)psx_mod_read_half(a + offs[i] + 2u);
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
            }
            int32_t cy = (ymin + ymax) / 2;
            if (cy >= base - 48 && cy <= base + 24) {
                if (tile) {
                    int32_t x = (int16_t)psx_mod_read_half(a + 8u);
                    int32_t y = (int16_t)psx_mod_read_half(a + 10u);
                    int32_t w = psx_mod_read_half(a + 12u), h = psx_mod_read_half(a + 14u);
                    int32_t x0 = scale_x(x), x1 = scale_x(x + w);
                    int32_t y0 = scale_y(y, base), y1 = scale_y(y + h, base);
                    psx_mod_write_half(a + 8u, (uint16_t)x0);
                    psx_mod_write_half(a + 10u, (uint16_t)y0);
                    psx_mod_write_half(a + 12u, (uint16_t)(x1 - x0));
                    psx_mod_write_half(a + 14u, (uint16_t)(y1 - y0));
                } else {
                    for (int i = 0; i < n; ++i) scale_xy_word(a + offs[i], base);
                }
            }
        }
        a = next;
    }
}

static void tsr_rope_entry(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    uint32_t start = s_faces_start;
    s_faces_start = 0u;
    if (!start || !hud_active()) return;
    uint32_t end = psx_mod_read_word(PRIM_CURSOR);
    if (!ram_address(start) || !ram_address(end) || end <= start ||
        end - start > 0x2000u)
        return;
    scale_faces(start, end);
}

/* Rope segment: draw it as a scaled POLY_FT4 and turn the game's SPRT into a
 * zero-size one. The game's DR_TPAGE still runs first (it is linked in front
 * of both), so the texture page and blending are the stock ones. */
/* ---- menu prompts: anchor to the screen edges -----------------------------
 * The menus' button prompts (BACK / CONTINUE / character / track names around
 * the pad buttons, one per player) are drawn by the menu UI 0x80034E0C through
 * the generic text/icon drawer 0x80033A84. Each element drawn from the menu UI
 * is tagged as HUD anchored to the side of the screen it sits on (centre within
 * +-8% left alone), so the native-wide compositor moves it out with the
 * widescreen reveal, keeping the 4:3 layout's distance from the edge. */
#define UI_TEXT_FN   0x80033A84u
#define UI_MENU_FN   0x80034E0Cu
#define UI_MENU_END  0x800366CCu
/* The collectibles counter (army man + "0/200" + timer, bottom right, sliding
 * in from off screen) of 0x8003A048: always anchored right. */
#define UI_COUNTER_FN   0x8003A048u
#define UI_COUNTER_END  0x8003A420u
#define UI_ICON_FN      0x8004E28Cu
#define UI_CLOCK_RA     0x8003F288u
#define UI_PAUSE_RA     0x8003BB94u   /* "PAUSA" (text drawer caller) */
#define UI_FLAG_RA1     0x8003E850u   /* track preview flags (icon drawer callers) */
#define UI_FLAG_RA2     0x8003E9D4u
#define UI_GLOVE_RA1    0x8003E8D4u   /* battle preview gloves (text drawer callers) */
#define UI_GLOVE_RA2    0x8003EA5Cu
static uint32_t s_ui_start;
static int s_ui_pending, s_ui_edge;   /* edge: 0 = by position, else forced */
static uint32_t s_ui_str;   /* string of the pending menu element */
static uint32_t s_ui_maxlen;
static int s_ui_pause;   /* the pending element is "PAUSA" */
static int s_ui_num;     /* the pending element is a prompt's player number */
static int s_ui_legal;   /* the start screen's legal text / PREMI START */
static int s_ui_right;   /* level select's collectibles counter: anchored right */
static uint32_t s_sky_vb = 0x80000000u;   /* VBlank of the last cloud sky (title, level select) */
static int sky_recent(void) { return s_vblank - s_sky_vb < 30u; }
/* native scene: the menus' cloud sky is up (title, level select) */
int tsr_ui_sky_recent(void) { return sky_recent(); }
/* Front-end screens (title, menus, character and level select): the boot
 * path leaves stage 35, after the attract demo it stays 41; races and the
 * track preview (38/39) are not. */
static int menu_screen(void) {
    uint8_t st = psx_mod_read_byte(INTRO_STAGE);
    return !g_tsr_racing && st != 38u && st != 39u;
}
static uint32_t s_levels_vb = 0x80000000u;   /* VBlank of the last levels-available line */
#define UI_NUMBER_RA 0x80036188u
static int s_ui_scale_mid;   /* an unanchored element that still shrinks */   /* icon elements: bytes to take at most */
/* Button clusters of this frame's prompts (the pad glyphs 'x' 't' 'o' 's'
 * are drawn first): the texts around a cluster take its side, so a whole
 * prompt moves together even when an animated label crosses the middle. */
static int32_t s_cl_x[4];
static int s_cl_edge[4], s_ncl;
static int32_t s_cl_y[4];   /* cluster centre y (its glyphs) */
static int32_t s_rz[8][4];   /* right-anchored zones of the frame: x0 x1 y0 y1 */
static int s_rz_n;
static int32_t s_rz_ay;   /* the counter's vertical anchor this frame */
static uint32_t s_rz_ot;
static uint32_t s_cl_frame_buf;

/* The frame's anchored packets, re-tagged every VBlank: the compositor drops
 * a tag after a few frames, and while the game saves or loads (memory card)
 * it shows the same frame for longer, which then slid back to 4:3. The list
 * restarts at each DrawOTag. */
static uint32_t s_tagged[256];
static int8_t s_tagged_edge[256];
static int s_ntagged;
static void ui_tag(uint32_t p, int edge) {
    psx_mod_tag_hud_primitive(p, edge);
    if (edge && s_ntagged < 256) { s_tagged[s_ntagged] = p; s_tagged_edge[s_ntagged] = (int8_t)edge; s_ntagged++; }
}
static void ui_tags_refresh(void) {
    for (int i = 0; i < s_ntagged; i++) psx_mod_tag_hud_primitive(s_tagged[i], s_tagged_edge[i]);
}
/* Untag the last frame's elements: once they are gone (a screen
 * transition), other packets (the transition's star) take their addresses. */
static uint32_t s_ui_done_fb;   /* frame buffer of the last processed (or cleared) frame */
static void ui_tags_clear(void) {
    for (int i = 0; i < s_ntagged; i++) psx_mod_tag_hud_primitive(s_tagged[i], 0);
    s_ntagged = 0;
}

static int prim_x_range(uint32_t p, int32_t *lo, int32_t *hi) {
    uint32_t cmd = psx_mod_read_word(p + 4u) >> 24;
    static const uint8_t off_ft[] = { 8, 16, 24, 32 }, off_gt[] = { 8, 20, 32, 44 },
                         off_f[] = { 8, 12, 16, 20 };
    const uint8_t *o; int n;
    if (cmd >= 0x20u && cmd < 0x40u) {
        int quad = (cmd & 0x08u) != 0, tex = (cmd & 0x04u) != 0, gour = (cmd & 0x10u) != 0;
        n = quad ? 4 : 3;
        o = gour ? (tex ? off_gt : off_ft) : (tex ? off_ft : off_f);
        if (gour && !tex) o = off_ft;   /* G3/G4: colour, xy pairs */
    } else if (cmd >= 0x60u && cmd < 0x80u) {
        n = 1; o = off_ft;
    } else {
        return 0;
    }
    for (int k = 0; k < n; k++) {
        int32_t x = (int16_t)psx_mod_read_half(p + o[k]);
        if (x < *lo) *lo = x;
        if (x > *hi) *hi = x;
    }
    return 1;
}

/* A drawing primitive's bounding box (0 for packets without vertices). */
static int prim_box(uint32_t p, int32_t *x0, int32_t *x1, int32_t *y0, int32_t *y1) {
    uint32_t cmd = psx_mod_read_word(p + 4u) >> 24;
    static const uint8_t off_ft[] = { 8, 16, 24, 32 }, off_gt[] = { 8, 20, 32, 44 },
                         off_f[] = { 8, 12, 16, 20 };
    const uint8_t *o; int n;
    if (cmd >= 0x20u && cmd < 0x40u) {
        int quad = (cmd & 0x08u) != 0, tex = (cmd & 0x04u) != 0, gour = (cmd & 0x10u) != 0;
        n = quad ? 4 : 3;
        o = gour ? (tex ? off_gt : off_ft) : (tex ? off_ft : off_f);
    } else if (cmd >= 0x60u && cmd < 0x80u) {
        n = 1; o = off_ft;
    } else {
        return 0;
    }
    *x0 = *y0 = 0x7FFF; *x1 = *y1 = -0x7FFF;
    for (int k = 0; k < n; k++) {
        int32_t x = (int16_t)psx_mod_read_half(p + o[k]);
        int32_t y = (int16_t)psx_mod_read_half(p + o[k] + 2u);
        if (x < *x0) *x0 = x; if (x > *x1) *x1 = x;
        if (y < *y0) *y0 = y; if (y > *y1) *y1 = y;
    }
    return 1;
}

/* HUD size option for the edge-anchored menu elements: shrink each packet
 * toward the screen corner it is anchored to (x toward its edge, y toward
 * the nearer of top/bottom), as the race HUD shrinks toward its corner. */
/* Menu elements: 85% of the race HUD size (they are bigger than the HUD). */
#define MENU_HUD_PERCENT (s_hud_percent * 85 / 100)
static void scale_packet_at(uint32_t p, int32_t ax, int32_t ay);
static void scale_packet(uint32_t p, int edge, int32_t ay) {
    int32_t w = (int32_t)psx_mod_display_width();
    if (w <= 0) w = 512;
    scale_packet_at(p, edge < 0 ? 0 : w, ay);
}
static void scale_packet_at(uint32_t p, int32_t ax, int32_t ay) {
    uint32_t cmd = psx_mod_read_word(p + 4u) >> 24;
    static const uint8_t off_ft[] = { 8, 16, 24, 32 }, off_gt[] = { 8, 20, 32, 44 },
                         off_f[] = { 8, 12, 16, 20 };
    const uint8_t *o; int n;
    if (cmd >= 0x20u && cmd < 0x40u) {
        int quad = (cmd & 0x08u) != 0, tex = (cmd & 0x04u) != 0, gour = (cmd & 0x10u) != 0;
        n = quad ? 4 : 3;
        o = gour ? (tex ? off_gt : off_ft) : (tex ? off_ft : off_f);
    } else if (cmd >= 0x40u && cmd < 0x60u && !(cmd & 0x08u)) {
        n = 2; o = (cmd & 0x10u) ? off_ft : off_f;
    } else {
        return;   /* sprites: their size cannot follow */
    }
    for (int k = 0; k < n; k++) {
        int32_t x = (int16_t)psx_mod_read_half(p + o[k]);
        int32_t y = (int16_t)psx_mod_read_half(p + o[k] + 2u);
        x = ax + (x - ax) * MENU_HUD_PERCENT / 100;
        y = ay + (y - ay) * MENU_HUD_PERCENT / 100;
        psx_mod_write_half(p + o[k], (uint16_t)x);
        psx_mod_write_half(p + o[k] + 2u, (uint16_t)y);
    }
}

static int32_t anchor_y(int32_t cy) {
    int32_t h = (int32_t)psx_mod_display_height();
    if (h <= 0 || h > 256) h = 256;   /* the menus draw in a 256-line frame */
    return cy < h / 2 ? 0 : h;
}

/* Find the packets an element wrote between start and end. Their sizes vary
 * and the game links them later, so recognise packet starts: a plausible
 * word count, a GPU primitive or draw-mode command, and a link pointing into
 * the frame's OT (base at 0x800A9B10) or into the element itself. */
#define UI_OT_ENTRIES 2200u
static int ui_collect(uint32_t start, uint32_t end, uint32_t *pk, int max) {
    uint32_t ot = psx_mod_read_word(0x800A9B10u) & 0x00FFFFFFu;
    int n = 0;
    for (uint32_t a = start; a + 8u <= end && n < max; ) {
        uint32_t tag = psx_mod_read_word(a), words = tag >> 24, nxt = tag & 0x00FFFFFFu;
        uint32_t cmd = psx_mod_read_word(a + 4u) >> 24;
        int okcmd = (cmd >= 0x20u && cmd < 0x80u) || (cmd >= 0xE1u && cmd <= 0xE6u);
        /* the link: an OT entry, or any packet of this frame's buffer
         * (an element links to whatever its OT entry held before) */
        uint32_t buf = psx_mod_read_word(0x800A9E34u) & 0x00FFFFFFu;
        uint32_t lim = psx_mod_read_word(PRIM_LIMIT) & 0x00FFFFFFu;
        int oklink = (nxt >= ot && nxt < ot + UI_OT_ENTRIES * 4u) ||
                     (nxt >= buf && nxt < lim && (nxt & 3u) == 0u);
        if (words >= 1u && words <= 16u && okcmd && (oklink || nxt == 0u || a == start) &&
            a + (words + 1u) * 4u <= end) {
            pk[n++] = a;
            a += (words + 1u) * 4u;
        } else {
            a += 4u;
        }
    }
    return n;
}

/* ---- deferred menu element processing ------------------------------------
 * Every element drawn by the hooked UI calls is recorded while the frame is
 * built and processed when the frame is complete (DrawOTag, or the next
 * frame starting in the other buffer), so which screen it is (character
 * select or not) is decided from the whole frame, never from draw order or
 * the previous frame. Menu prompt parts carry their prompt's own centre
 * (from the drawer's arguments): each prompt moves and shrinks as one. */
typedef struct {
    uint32_t start, end, ot, ra;
    int8_t edge;
    uint8_t num, pause, mid, right, has_c;
    int16_t pcx, pcy;
    int8_t rdx, rdy;   /* the part's offset direction from its prompt's centre */
} UiEl;
static UiEl s_el[192];
static int s_nel, s_f_num, s_f_levels, s_f_lvsel;
static uint32_t s_el_fb;
static int s_ui_has_c, s_ui_lvsel;
static uint32_t s_ui_ra;   /* the pending element's caller */   /* the pending element: level select only */
static int16_t s_ui_pcx, s_ui_pcy;
static int8_t s_ui_rdx, s_ui_rdy;
static void ui_process_frame(void);

static void ui_flush(void) {
    if (!s_ui_pending) return;
    s_ui_pending = 0;
    uint32_t start = s_ui_start, end = psx_mod_read_word(PRIM_CURSOR);
    /* an icon is one packet (+ its draw mode): do not take in whatever other
     * code draws next (the preview's letter-by-letter titles) */
    if (s_ui_maxlen && end > start + s_ui_maxlen) end = start + s_ui_maxlen;
    s_ui_maxlen = 0;
    UiEl e;
    e.ra = s_ui_ra; e.start = start; e.end = end; e.ot = psx_mod_read_word(0x800A9B10u);
    e.edge = (int8_t)s_ui_edge; e.num = (uint8_t)s_ui_num; e.pause = (uint8_t)s_ui_pause;
    e.mid = (uint8_t)s_ui_scale_mid; e.right = (uint8_t)s_ui_right;
    e.has_c = (uint8_t)s_ui_has_c; e.pcx = s_ui_pcx; e.pcy = s_ui_pcy;
    e.rdx = s_ui_rdx; e.rdy = s_ui_rdy;
    s_ui_num = 0; s_ui_pause = 0; s_ui_scale_mid = 0; s_ui_right = 0; s_ui_has_c = 0;
    /* a new frame in the other buffer: the recorded one is complete */
    uint32_t fb = psx_mod_read_word(0x800A9E34u);
    if (s_nel && fb != s_el_fb) ui_process_frame();
    /* the same buffer drawn again from its start: the game threw the frame
     * it was building away and builds it again over the same addresses (the
     * end-of-race camera cuts). The earlier records now point at the new
     * packets and would be processed twice (smaller, shifted prompts). */
    else if (s_nel && start < s_el[s_nel - 1].start) {
        s_nel = 0; s_f_num = 0; s_f_levels = 0; s_f_lvsel = 0;
        s_rz_n = 0;
    }
    s_el_fb = fb;
    if (e.num) s_f_num = 1;
    if (s_ui_lvsel) s_f_lvsel = 1;
    s_ui_lvsel = 0;
    if (s_nel < 192) s_el[s_nel++] = e;
}

#define PROMPT_SPREAD 20   /* percent */
static void move_packet(uint32_t p, int32_t dx, int32_t dy) {
    uint32_t cmd = psx_mod_read_word(p + 4u) >> 24;
    static const uint8_t off_ft[] = { 8, 16, 24, 32 }, off_gt[] = { 8, 20, 32, 44 },
                         off_f[] = { 8, 12, 16, 20 };
    const uint8_t *o; int n;
    if (cmd >= 0x20u && cmd < 0x40u) {
        int quad = (cmd & 0x08u) != 0, tex = (cmd & 0x04u) != 0, gour = (cmd & 0x10u) != 0;
        n = quad ? 4 : 3;
        o = gour ? (tex ? off_gt : off_ft) : (tex ? off_ft : off_f);
    } else if (cmd >= 0x40u && cmd < 0x60u && !(cmd & 0x08u)) {
        n = 2; o = (cmd & 0x10u) ? off_ft : off_f;
    } else if (cmd >= 0x60u && cmd < 0x80u) {
        n = 1; o = off_f;   /* sprites/rectangles: the top-left corner */
    } else {
        return;
    }
    for (int k = 0; k < n; k++) {
        psx_mod_write_half(p + o[k], (uint16_t)((int16_t)psx_mod_read_half(p + o[k]) + dx));
        psx_mod_write_half(p + o[k] + 2u, (uint16_t)((int16_t)psx_mod_read_half(p + o[k] + 2u) + dy));
    }
}

static void ui_process(const UiEl *el, int charsel) {
    uint32_t start = el->start, end = el->end;
    if (!ram_address(start) || !ram_address(end) || end <= start || end - start > 0x2000u) return;
    uint32_t pk[128];
    int n = ui_collect(start, end, pk, 128);
    if (n <= 0) return;
    if (el->has_c) {
        /* a prompt part is a few small glyphs around its prompt's centre:
         * whatever the game draws next (the screen transition's star) is
         * not taken in with it */
        int m = 0, last = -1;
        for (int k = 0; k < n; k++) {
            int32_t x0, x1, y0, y1;
            if (!prim_box(pk[k], &x0, &x1, &y0, &y1)) { pk[m++] = pk[k]; continue; }
            int32_t dx = (x0 + x1) / 2 - el->pcx, dy = (y0 + y1) / 2 - el->pcy;
            /* labels as wide as the screen (long level names), side labels
             * further out on their own side */
            int32_t dxl = el->rdx < 0 ? -360 : -260, dxh = el->rdx > 0 ? 360 : 260;
            if (dx < dxl || dx > dxh || dy < -50 || dy > 50 || x1 - x0 > 72 || y1 - y0 > 72) continue;
            pk[m++] = pk[k]; last = m;
        }
        n = last;   /* draw modes after the last glyph are not its own */
        if (n <= 0) return;
    }
    int32_t lo = 0x7FFF, hi = -0x7FFF;
    int any = 0;
    for (int k = 0; k < n; k++) any |= prim_x_range(pk[k], &lo, &hi);
    if (!any) return;
    int32_t w = (int32_t)psx_mod_display_width();
    if (w <= 0) w = 512;
    /* y range of the drawing primitives only (a draw-mode packet has no
     * vertex: its +10 is not a y) */
    int32_t ylo2 = 0x7FFF, yhi2 = -0x7FFF;
    for (int k = 0; k < n; k++) {
        int32_t xl = 0x7FFF, xh = -0x7FFF;
        if (!prim_x_range(pk[k], &xl, &xh)) continue;
        int32_t y = (int16_t)psx_mod_read_half(pk[k] + 10u);
        if (y < ylo2) ylo2 = y; if (y > yhi2) yhi2 = y;
    }
    int32_t cy = (ylo2 + yhi2) / 2;
    int32_t c = (lo + hi) / 2;
    int unanch = el->edge == 2;   /* an element that is never anchored */
    /* a wide centre band: the preview's bouncing title letters stay put */
    int edge = c < w * 35 / 100 ? -1 : c > w * 65 / 100 ? 1 : 0;
    int32_t sx = w / 2, sy = cy;   /* centre of an unanchored shrink */
    int32_t ayc = cy;              /* y deciding the vertical anchor */
    if (el->edge == 1 || el->edge == -1) {
        edge = el->edge;
    } else if (el->edge == 2) {
        /* anchored only when it is the level select's collectibles counter
         * (army man, "0/200") */
        edge = el->right ? 1 : 0;
        if (edge) unanch = 0;
    } else if (el->has_c) {
        /* a menu prompt part: its prompt's centre decides for all its parts.
         * On the character select the prompt follows the character's head:
         * never anchored, shrunk about the prompt's centre */
        int32_t pcx = el->pcx;
        edge = charsel ? 0 : pcx < w * 35 / 100 ? -1 : pcx > w * 65 / 100 ? 1 : 0;
        sx = pcx; sy = el->pcy; ayc = el->pcy;
    }
    if (!edge || unanch) {
        /* not anchored: clear any tag left at these addresses by an earlier
         * frame's packet (tags are kept per address for a few frames) */
        for (int k = 0; k < n; k++) psx_mod_tag_hud_primitive(pk[k], 0);
        /* menu and preview texts in the middle shrink like the anchored
         * ones (prompts about their centre, others about the screen's
         * centre line at their own height) */
        if (el->edge != 2 || el->mid)
            for (int k = 0; k < n; k++) scale_packet_at(pk[k], sx, sy);
        /* prompts left in the middle (language, new game, character
         * select): spread a little, to the spacing of the menu's corner
         * prompts */
        if (el->has_c && (el->rdx || el->rdy)) {
            int32_t ox = el->rdx * 40 * PROMPT_SPREAD / 100 * MENU_HUD_PERCENT / 100;
            int32_t oy = el->rdy * 25 * PROMPT_SPREAD / 100 * MENU_HUD_PERCENT / 100;
            for (int k = 0; k < n; k++) move_packet(pk[k], ox, oy);
        }
        return;
    }
    int pause = el->pause;
    int counter = el->edge == 1 && !pause;
    int32_t ay = counter && s_rz_n ? s_rz_ay : anchor_y(ayc);
    if (counter && !s_rz_n) s_rz_ay = ay;   /* the counter keeps one anchor */
    if (pause) {
        /* PAUSA: moved down to the bottom-right corner */
        int32_t dy = 218 - yhi2;
        static const uint8_t off[4] = { 8, 16, 24, 32 };
        for (int k = 0; k < n; k++) {
            uint32_t cmd = psx_mod_read_word(pk[k] + 4u) >> 24;
            if ((cmd & 0xFCu) != 0x2Cu && (cmd & 0xFCu) != 0x3Cu) continue;
            for (int v = 0; v < 4; v++) {
                uint32_t o = (cmd & 0xFCu) == 0x3Cu ? 8u + (uint32_t)v * 12u : off[v];
                psx_mod_write_half(pk[k] + o + 2u, (uint16_t)((int16_t)psx_mod_read_half(pk[k] + o + 2u) + dy));
            }
        }
        ay = 256;
    }
    for (int k = 0; k < n; k++) {
        ui_tag(pk[k], edge);
        scale_packet(pk[k], edge, ay);
    }
    if (edge > 0 && counter) {   /* the counter's zone (for its alarm clock) */
        s_rz_ot = el->ot;   /* the OT this frame builds */
        /* one union box per frame */
        if (!s_rz_n) { s_rz[0][0] = lo; s_rz[0][1] = hi; s_rz[0][2] = ylo2; s_rz[0][3] = yhi2; s_rz_n = 1; }
        else {
            if (lo < s_rz[0][0]) s_rz[0][0] = lo;
            if (hi > s_rz[0][1]) s_rz[0][1] = hi;
            if (ylo2 < s_rz[0][2]) s_rz[0][2] = ylo2;
            if (yhi2 > s_rz[0][3]) s_rz[0][3] = yhi2;
        }
    }
}

/* the recorded frame's elements */
static void ui_process_frame(void) {
    /* character select: its prompts follow the characters' heads. Two
     * players show a number in each prompt, one player the "N levels
     * available" line; the level select (2 players: numbers too) draws its
     * own collectibles counter, or level names in its prompts */
    int charsel = menu_screen() && !s_f_lvsel && (s_f_num || s_f_levels);
    ui_tags_clear();
    s_ui_done_fb = psx_mod_read_word(0x800A9E34u);
    for (int i = 0; i < s_nel; i++) ui_process(&s_el[i], charsel);
    s_nel = 0; s_f_num = 0; s_f_levels = 0; s_f_lvsel = 0;
}

/* At the start of a frame (the previous one complete): UI packets drawn by
 * other code next to a right-anchored element (the alarm clock beside the
 * counter's timer) join it. Walks the front OT entry's chain. */
static void ui_zone_pass(void) {
    if (!s_rz_n) return;
    uint32_t ot = s_rz_ot;   /* the OT being drawn */
    if (!ram_address(ot)) { s_rz_n = 0; return; }
    uint32_t p = psx_mod_read_word(ot + 2080u * 4u) & 0x00FFFFFFu;
    for (int guard = 0; guard < 600 && p && p != 0x00FFFFFFu; guard++) {
        uint32_t a = 0x80000000u | p;
        if (!ram_address(a)) break;
        int32_t lo = 0x7FFF, hi = -0x7FFF;
        if (prim_x_range(a, &lo, &hi)) {
            int32_t cx = (lo + hi) / 2, cy = (int16_t)psx_mod_read_half(a + 10u);
            for (int z = 0; z < s_rz_n; z++)
                if (cx >= s_rz[z][0] - 24 && cx <= s_rz[z][1] + 24 &&
                    cy >= s_rz[z][2] - 24 && cy <= s_rz[z][3] + 24) {
                    ui_tag(a, 1);
                    scale_packet(a, 1, s_rz_ay);
                    break;
                }
        }
        p = psx_mod_read_word(a) & 0x00FFFFFFu;
    }
    s_rz_n = 0;
}

/* Main loop, once per frame right after the VBlank wait: the previous frame's
 * packets are complete (a VBlank can arrive in the middle of an element). */
#define FRAME_START_FN 0x800723A0u
static void sky_split_flush(void);
static void tsr_frame_start_entry(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    sky_split_flush();
    ui_flush();
}

/* Never fall back to a pillarboxed 4:3 picture after the boot logos: every
 * screen of this game is a 3D scene or a full-screen backdrop (FMV keeps its
 * own 4:3 rule). */
static int tsr_world_scene(void) {
    return psx_mod_read_byte(INTRO_STAGE) >= 5u;
}

static void cloud_flush(void);
/* DrawOTag: the frame's packets are complete, nothing drawn yet. */
static void tsr_linked_list_hook(void) {
    cloud_flush();
    sky_split_flush();
    ui_flush();
    if (s_nel) ui_process_frame();
    else {
        /* a new frame without menu elements (DrawOTag can come more than
         * once per frame: only the frame buffer tells a new frame) */
        uint32_t fb = psx_mod_read_word(0x800A9E34u);
        if (fb != s_ui_done_fb && menu_screen()) { ui_tags_clear(); s_ui_done_fb = fb; }
    }
    ui_zone_pass();
}

static void tsr_ui_text_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    sky_split_flush();
    ui_flush();
    uint32_t ra = cpu->gpr[31];
    s_ui_ra = ra;
    if (ra >= UI_COUNTER_FN && ra < UI_COUNTER_END) {
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_pending = 1;
        s_ui_edge = 1;
        return;
    }
    if ((ra >= UI_MENU_FN && ra < UI_MENU_END) || ra == UI_PAUSE_RA) {
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_pending = 1;
        s_ui_edge = ra == UI_PAUSE_RA ? 1 : 0;
        s_ui_pause = ra == UI_PAUSE_RA;
        s_ui_str = cpu->gpr[4];
        /* the player number in the middle of a prompt (character select,
         * preview, pause) */
        s_ui_num = ra == UI_NUMBER_RA;
        /* the bottom label of a prompt: a level name (2-player level
         * select) is built in a buffer past the program's data, a
         * character's name is the program's own string */
        if (ra == 0x80036218u && cpu->gpr[4] >= 0x800C0000u) s_ui_lvsel = 1;
        /* the prompt's centre from the drawer's (x, y): each part is drawn
         * at a fixed offset from it */
        if (ra != UI_PAUSE_RA) {
            int32_t px = (int32_t)cpu->gpr[5], py = (int32_t)cpu->gpr[6];
            int dx = 0, dy = 0;
            switch (ra) {
            case 0x80036218u: case 0x800362C8u: dy = 1; break;    /* bottom */
            case 0x8003635Cu: case 0x8003640Cu: dy = -1; break;   /* top */
            case 0x800364A4u: case 0x80036554u: dx = 1; break;    /* right */
            case 0x800365ECu: case 0x800366A0u: dx = -1; break;   /* left */
            default: break;                                       /* number */
            }
            px -= dx * 40; py -= dy * 25;
            s_ui_rdx = (int8_t)dx; s_ui_rdy = (int8_t)dy;
            s_ui_has_c = 1; s_ui_pcx = (int16_t)px; s_ui_pcy = (int16_t)py;
        }
    } else if (ra == UI_GLOVE_RA1 || ra == UI_GLOVE_RA2) {
        /* the battle preview's boxing gloves (a font glyph, one per player):
         * by side, like the race preview's flags */
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_pending = 1;
        s_ui_edge = 0;
        s_ui_str = 0;
        s_ui_maxlen = 96u;
        s_ui_lvsel = 1;   /* not the character select */
    } else {
        /* any other text (titles, credits): never anchored; clear stale tags */
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_pending = 1;
        s_ui_edge = 2;
        s_ui_str = 0;
        /* shrunk with the menus, except the race HUD's own texts (scaled
         * by the race HUD code) and the start screen's legal text */
        if (ra == 0x8003B558u) s_f_levels = 1;   /* "N LIVELLI DISPONIBILI" */
        s_ui_legal = ra == 0x8003CB98u || ra == 0x80037100u;
        s_ui_right = ra == 0x8003F4B8u;   /* level select "0/200" */
        if (ra == 0x8003F4B8u || ra == 0x8003F640u || ra == 0x8003F9B4u) s_ui_lvsel = 1;
        s_ui_scale_mid = ra != 0x8007A934u && ra != 0x80038CB8u && ra != 0x80038D1Cu &&
                         ra != 0x8003CB98u && ra != 0x80037100u;
    }
}

/* Title/menu sky: 0x8004B448 scrolls its clouds over a 1024-wide period
 * (x = (pos mod 1024) - 256); in the widest aspects the margins reach past
 * that span. Each cloud packet is copied one period left/right when the copy
 * lands on screen. */
#define CLOUD_RA1 0x8004B614u
#define CLOUD_RA2 0x8004B70Cu
static uint32_t s_cloud_start, s_cloud_fb;
static int s_cloud_pending;

static void cloud_flush(void) {
    if (!s_cloud_pending) return;
    s_cloud_pending = 0;
    uint32_t p = s_cloud_start, end = psx_mod_read_word(PRIM_CURSOR);
    /* only while the same frame is being built (else its packets are
     * already drawn and the cursor is in the next frame's buffer) */
    if (psx_mod_read_word(0x800A9E34u) != s_cloud_fb) return;
    if (!ram_address(p) || end < p || end - p > 0x100u) return;
    int32_t m = psx_mod_widescreen_x_margin();
    if (m <= 0) return;
    int32_t w = (int32_t)psx_mod_display_width();
    if (w <= 0) w = 512;
    for (; p + 40u <= end; ) {
        uint32_t tag = psx_mod_read_word(p), words = tag >> 24;
        uint32_t cmd = psx_mod_read_word(p + 4u) >> 24;
        if (words == 0u || words > 16u) break;
        /* clouds are never HUD: clear a tag an earlier frame's menu element
         * left at this address (it shifted the cloud by the margin) */
        psx_mod_tag_hud_primitive(p, 0);
        if (words == 9u && (cmd & 0xFCu) == 0x2Cu) {
            int32_t lo = 0x7FFF, hi = -0x7FFF;
            prim_x_range(p, &lo, &hi);
            for (int dir = -1; dir <= 1; dir += 2) {
                int32_t dx = dir * 1024;
                if (hi + dx < -m || lo + dx > w + m) continue;
                uint32_t q = psx_mod_read_word(PRIM_CURSOR);
                if (!ram_address(q) || q + 40u >= psx_mod_read_word(PRIM_LIMIT)) return;
                for (uint32_t k = 4u; k < 40u; k += 4u) psx_mod_write_word(q + k, psx_mod_read_word(p + k));
                static const uint8_t off[4] = { 8, 16, 24, 32 };
                for (int k = 0; k < 4; k++) {
                    int32_t x = (int16_t)psx_mod_read_half(q + off[k]) + dx;
                    psx_mod_write_half(q + off[k], (uint16_t)x);
                }
                /* linked right after the original in its OT chain */
                uint32_t t = psx_mod_read_word(p);
                psx_mod_write_word(q, (9u << 24) | (t & 0x00FFFFFFu));
                psx_mod_write_word(p, (t & 0xFF000000u) | (q & 0x00FFFFFFu));
                psx_mod_write_word(PRIM_CURSOR, q + 40u);
                psx_mod_tag_hud_primitive(q, 0);
            }
        }
        p += (words + 1u) * 4u;
    }
}

/* ---- sky dome cap ----------------------------------------------------------
 * 0x80011C18 (a0 block list, a1 count) draws the sky of the outdoor tracks: a
 * dome of Gouraud cells around the camera (rotation-only matrix), 8 sectors in
 * two rings. The upper ring stops about 46 degrees above the horizon and the
 * dome has no top: in 4:3 that edge is never seen, but the corners of a wide
 * view with the camera rolled (the track preview flights) look past it, onto
 * the black background. Widescreen only, the cap is filled in: a fan of
 * triangles from the upper edge to the zenith, coloured like the edge and
 * linked into the sky's OT slot before the game adds its cells (the cap does
 * not overlap them, so the order does not matter).
 * Block (44 bytes): +0 vertex base, +4 cells (12 bytes: four int16 vertex
 * offsets, flag pointer), +12 four corners (int16 x y z), +38 cell count.
 * Vertex (12 bytes): int16 x y z, pad, colour word. */
#define SKY_DRAW_FN    0x80011C18u
#define SKY_OT_OFFSET  4152u   /* the sky's OT entry: OT base + 1038 * 4 */
#define CAP_MAX_RING   256

typedef struct { int32_t x, y, z; uint32_t rgb; float az; } CapVert;

/* The current GTE matrix (the game's own setup for the dome): view space,
 * then the RTPS projection. */
typedef struct { float x, y, z, r, g, b; } CapPt;
static void cap_view(const struct CPUState *cpu, int32_t vx, int32_t vy, int32_t vz,
                     uint32_t rgb, CapPt *o) {
    const uint32_t *c = cpu->gte_ctrl;
    float r11 = (int16_t)(c[0] & 0xFFFFu), r12 = (int16_t)(c[0] >> 16);
    float r13 = (int16_t)(c[1] & 0xFFFFu), r21 = (int16_t)(c[1] >> 16);
    float r22 = (int16_t)(c[2] & 0xFFFFu), r23 = (int16_t)(c[2] >> 16);
    float r31 = (int16_t)(c[3] & 0xFFFFu), r32 = (int16_t)(c[3] >> 16);
    float r33 = (int16_t)(c[4] & 0xFFFFu);
    o->x = (int32_t)c[5] + (r11 * vx + r12 * vy + r13 * vz) / 4096.0f;
    o->y = (int32_t)c[6] + (r21 * vx + r22 * vy + r23 * vz) / 4096.0f;
    o->z = (int32_t)c[7] + (r31 * vx + r32 * vy + r33 * vz) / 4096.0f;
    o->r = (float)(rgb & 0xFFu); o->g = (float)((rgb >> 8) & 0xFFu); o->b = (float)((rgb >> 16) & 0xFFu);
}
static void cap_screen(const struct CPUState *cpu, const CapPt *p, int32_t *sx, int32_t *sy) {
    const uint32_t *c = cpu->gte_ctrl;
    float h = (float)(c[26] & 0xFFFFu);
    float div = h / p->z;
    float x = (int32_t)c[24] / 65536.0f + p->x * div, y = (int32_t)c[25] / 65536.0f + p->y * div;
    if (x < -1024.0f) x = -1024.0f; if (x > 1023.0f) x = 1023.0f;
    if (y < -1024.0f) y = -1024.0f; if (y > 1023.0f) y = 1023.0f;
    *sx = (int32_t)floorf(x); *sy = (int32_t)floorf(y);
}

/* One cap triangle in view space: clipped at the near plane (z = h/2, where
 * the GTE's quotient limit is reached), split while it is larger on screen
 * than a GPU polygon may be (1023 x 511), then linked into the OT. */
typedef struct { uint32_t cur, lim, ot; float near; } CapOut;
static void cap_emit(const struct CPUState *cpu, CapOut *co, const CapPt *a, const CapPt *b,
                     const CapPt *c, int depth) {
    const CapPt *in[3] = { a, b, c };
    CapPt out[4];
    int m = 0;
    for (int k = 0; k < 3; k++) {
        const CapPt *p = in[k], *q = in[(k + 1) % 3];
        int pin = p->z >= co->near, qin = q->z >= co->near;
        if (pin) out[m++] = *p;
        if (pin != qin) {
            float t = (co->near - p->z) / (q->z - p->z);
            CapPt *o = &out[m++];
            o->x = p->x + (q->x - p->x) * t; o->y = p->y + (q->y - p->y) * t; o->z = co->near;
            o->r = p->r + (q->r - p->r) * t; o->g = p->g + (q->g - p->g) * t; o->b = p->b + (q->b - p->b) * t;
        }
    }
    for (int t = 0; t + 2 < m; t++) {   /* fan: 3 or 4 vertices */
        const CapPt *v[3] = { &out[0], &out[t + 1], &out[t + 2] };
        int32_t sx[3], sy[3];
        for (int k = 0; k < 3; k++) cap_screen(cpu, v[k], &sx[k], &sy[k]);
        int32_t x0 = sx[0], x1 = sx[0], y0 = sy[0], y1 = sy[0];
        for (int k = 1; k < 3; k++) {
            if (sx[k] < x0) x0 = sx[k]; if (sx[k] > x1) x1 = sx[k];
            if (sy[k] < y0) y0 = sy[k]; if (sy[k] > y1) y1 = sy[k];
        }
        if ((x1 - x0 > 1000 || y1 - y0 > 500) && depth < 5) {
            CapPt mid[3];
            for (int k = 0; k < 3; k++) {
                const CapPt *p = v[k], *q = v[(k + 1) % 3];
                mid[k].x = (p->x + q->x) * 0.5f; mid[k].y = (p->y + q->y) * 0.5f; mid[k].z = (p->z + q->z) * 0.5f;
                mid[k].r = (p->r + q->r) * 0.5f; mid[k].g = (p->g + q->g) * 0.5f; mid[k].b = (p->b + q->b) * 0.5f;
            }
            cap_emit(cpu, co, v[0], &mid[0], &mid[2], depth + 1);
            cap_emit(cpu, co, &mid[0], v[1], &mid[1], depth + 1);
            cap_emit(cpu, co, &mid[2], &mid[1], v[2], depth + 1);
            cap_emit(cpu, co, &mid[0], &mid[1], &mid[2], depth + 1);
            continue;
        }
        if (!ram_address(co->cur) || co->cur + 28u + 36u > co->lim) return;
        uint32_t head = psx_mod_read_word(co->ot);
        psx_mod_write_word(co->cur, (6u << 24) | (head & 0x00FFFFFFu));
        for (int k = 0; k < 3; k++) {
            uint32_t rgb = (uint32_t)v[k]->r | ((uint32_t)v[k]->g << 8) | ((uint32_t)v[k]->b << 16);
            psx_mod_write_word(co->cur + 4u + (uint32_t)k * 8u, (k == 0 ? 0x30000000u : 0u) | rgb);
            psx_mod_write_word(co->cur + 8u + (uint32_t)k * 8u, ((uint32_t)(uint16_t)sy[k] << 16) | (uint16_t)sx[k]);
        }
        psx_mod_write_word(co->ot, (head & 0xFF000000u) | (co->cur & 0x00FFFFFFu));
        co->cur += 28u;
    }
}

static int cap_cmp(const void *a, const void *b) {
    float d = ((const CapVert *)a)->az - ((const CapVert *)b)->az;
    return d < 0.0f ? -1 : d > 0.0f ? 1 : 0;
}

static void sky_cap(struct CPUState *cpu) {
    uint32_t list = cpu->gpr[4];
    int count = (int32_t)cpu->gpr[5];
    if (!ram_address(list) || count <= 0 || count > 64) return;
    static CapVert ring[CAP_MAX_RING];
    int n = 0;
    int32_t top = 0;
    /* the upper edge: the highest corners of the dome */
    for (int b = 0; b < count; b++)
        for (int k = 0; k < 4; k++) {
            int32_t y = (int16_t)psx_mod_read_half(list + (uint32_t)b * 44u + 14u + (uint32_t)k * 6u);
            if (y < top) top = y;
        }
    if (top >= 0) return;
    for (int b = 0; b < count && n < CAP_MAX_RING; b++) {
        uint32_t blk = list + (uint32_t)b * 44u;
        int top_corners = 0;
        for (int k = 0; k < 4; k++)
            top_corners += (int16_t)psx_mod_read_half(blk + 14u + (uint32_t)k * 6u) == top;
        if (top_corners < 2) continue;
        uint32_t base = psx_mod_read_word(blk), cells = psx_mod_read_word(blk + 4u);
        int ncell = (int16_t)psx_mod_read_half(blk + 38u);
        if (!ram_address(base) || !ram_address(cells) || ncell <= 0 || ncell > 256) continue;
        for (int ci = 0; ci < ncell && n < CAP_MAX_RING; ci++)
            for (int k = 0; k < 4 && n < CAP_MAX_RING; k++) {
                uint32_t v = base + (uint32_t)(int16_t)psx_mod_read_half(cells + (uint32_t)ci * 12u + (uint32_t)k * 2u);
                if ((int16_t)psx_mod_read_half(v + 2u) != top) continue;
                /* neighbouring blocks repeat their shared edge vertices */
                int32_t x = (int16_t)psx_mod_read_half(v), z = (int16_t)psx_mod_read_half(v + 4u);
                int dup = 0;
                for (int s = 0; s < n && !dup; s++) dup = ring[s].x == x && ring[s].z == z;
                if (dup) continue;
                CapVert *cv = &ring[n++];
                cv->x = x;
                cv->y = top;
                cv->z = z;
                cv->rgb = psx_mod_read_word(v + 8u) & 0x00FFFFFFu;
                cv->az = atan2f((float)cv->x, (float)cv->z);
            }
    }
    if (n < 3) return;
    qsort(ring, (size_t)n, sizeof ring[0], cap_cmp);
    /* the zenith: above the dome's centre, coloured like the edge on average */
    uint32_t sr = 0, sg = 0, sb = 0;
    float rad = 0.0f;
    for (int i = 0; i < n; i++) {
        sr += ring[i].rgb & 0xFFu; sg += (ring[i].rgb >> 8) & 0xFFu; sb += (ring[i].rgb >> 16) & 0xFFu;
        rad += sqrtf((float)ring[i].x * ring[i].x + (float)ring[i].z * ring[i].z);
    }
    uint32_t zrgb = (sr / (uint32_t)n) | ((sg / (uint32_t)n) << 8) | ((sb / (uint32_t)n) << 16);
    int32_t zy = top - (int32_t)(rad / (float)n);
    uint32_t ot = psx_mod_read_word(0x800A9B10u) + SKY_OT_OFFSET;
    if (!ram_address(ot)) return;
    CapOut co;
    co.cur = psx_mod_read_word(PRIM_CURSOR);
    co.lim = psx_mod_read_word(PRIM_LIMIT);
    co.ot = ot;
    co.near = (float)(cpu->gte_ctrl[26] & 0xFFFFu) * 0.5f;
    if (co.near < 16.0f) co.near = 16.0f;
    CapPt zen;
    cap_view(cpu, 0, zy, 0, zrgb, &zen);
    for (int i = 0; i < n; i++) {
        const CapVert *a = &ring[i], *b = &ring[(i + 1) % n];
        CapPt pa, pb;
        cap_view(cpu, a->x, a->y, a->z, a->rgb, &pa);
        cap_view(cpu, b->x, b->y, b->z, b->rgb, &pb);
        cap_emit(cpu, &co, &pa, &pb, &zen, 0);
    }
    psx_mod_write_word(PRIM_CURSOR, co.cur);
}

/* The dome's own cells: in a wide view the cells near the screen edges
 * project far wider than in 4:3, and the GPU drops any triangle more than
 * 1023 pixels wide or 511 tall (as the PlayStation does), which left black
 * wedges in the sky. After the game has drawn the dome, each such cell
 * (POLY_G4 in the sky's OT entry) is cut into a grid of smaller quads with
 * the same corners and colours, linked in its place. */
static uint32_t s_sky_split_start, s_sky_split_fb;
static int s_sky_split_pending;

static int g4_tri_oversize(const int32_t *x, const int32_t *y, int a, int b, int c) {
    int32_t x0 = x[a], x1 = x[a], y0 = y[a], y1 = y[a];
    const int v[2] = { b, c };
    for (int k = 0; k < 2; k++) {
        if (x[v[k]] < x0) x0 = x[v[k]]; if (x[v[k]] > x1) x1 = x[v[k]];
        if (y[v[k]] < y0) y0 = y[v[k]]; if (y[v[k]] > y1) y1 = y[v[k]];
    }
    return x1 - x0 > 1023 || y1 - y0 > 511;
}

static void sky_split_flush(void) {
    if (!s_sky_split_pending) return;
    s_sky_split_pending = 0;
    /* only while the same frame is being built */
    if (psx_mod_read_word(0x800A9E34u) != s_sky_split_fb) return;
    uint32_t start = s_sky_split_start, end = psx_mod_read_word(PRIM_CURSOR);
    uint32_t lim = psx_mod_read_word(PRIM_LIMIT), cur = end;
    if (!ram_address(start) || end < start) return;
    /* the dome's cells: consecutive 36-byte POLY_G4 packets from where the
     * call started, each linked to the one before it */
    for (uint32_t a = start; a + 36u <= end && a < start + 1024u * 36u; a += 36u) {
        uint32_t tag = psx_mod_read_word(a);
        uint32_t next = tag & 0x00FFFFFFu;
        uint32_t cmd = psx_mod_read_word(a + 4u) >> 24;
        if ((tag >> 24) != 8u || (cmd & 0xFDu) != 0x38u) break;
        if (a != start && next != ((a - 36u) & 0x00FFFFFFu)) break;
        {
            int32_t x[4], y[4];
            uint32_t col[4];
            for (int k = 0; k < 4; k++) {
                col[k] = psx_mod_read_word(a + 4u + (uint32_t)k * 8u) & 0x00FFFFFFu;
                uint32_t xy = psx_mod_read_word(a + 8u + (uint32_t)k * 8u);
                x[k] = (int16_t)(xy & 0xFFFFu); y[k] = (int16_t)(xy >> 16);
            }
            if (g4_tri_oversize(x, y, 0, 1, 2) || g4_tri_oversize(x, y, 2, 1, 3)) {
                int32_t x0 = x[0], x1 = x[0], y0 = y[0], y1 = y[0];
                for (int k = 1; k < 4; k++) {
                    if (x[k] < x0) x0 = x[k]; if (x[k] > x1) x1 = x[k];
                    if (y[k] < y0) y0 = y[k]; if (y[k] > y1) y1 = y[k];
                }
                int nx = (x1 - x0) / 600 + 1, ny = (y1 - y0) / 300 + 1;
                if (nx > 8) nx = 8; if (ny > 8) ny = 8;
                if (cur + (uint32_t)(nx * ny) * 36u + 64u > lim) break;
                /* bilinear over the quad: v0 v1 top, v2 v3 bottom */
                uint32_t link = next;
                for (int j = 0; j < ny; j++)
                    for (int i = 0; i < nx; i++) {
                        uint32_t q = cur;
                        cur += 36u;
                        for (int k = 0; k < 4; k++) {
                            float u = (float)(i + (k & 1)) / nx, v = (float)(j + (k >> 1)) / ny;
                            float w0 = (1 - u) * (1 - v), w1 = u * (1 - v), w2 = (1 - u) * v, w3 = u * v;
                            int32_t qx = (int32_t)floorf(w0 * x[0] + w1 * x[1] + w2 * x[2] + w3 * x[3] + 0.5f);
                            int32_t qy = (int32_t)floorf(w0 * y[0] + w1 * y[1] + w2 * y[2] + w3 * y[3] + 0.5f);
                            uint32_t rgb = 0;
                            for (int s = 0; s < 24; s += 8) {
                                float c = w0 * ((col[0] >> s) & 0xFFu) + w1 * ((col[1] >> s) & 0xFFu) +
                                          w2 * ((col[2] >> s) & 0xFFu) + w3 * ((col[3] >> s) & 0xFFu);
                                rgb |= ((uint32_t)(c + 0.5f) & 0xFFu) << s;
                            }
                            psx_mod_write_word(q + 4u + (uint32_t)k * 8u, (k == 0 ? (cmd << 24) : 0u) | rgb);
                            psx_mod_write_word(q + 8u + (uint32_t)k * 8u,
                                               ((uint32_t)(uint16_t)qy << 16) | (uint16_t)qx);
                        }
                        psx_mod_write_word(q, (8u << 24) | (link & 0x00FFFFFFu));
                        link = q & 0x00FFFFFFu;
                    }
                /* the cell itself becomes an empty link to the new quads */
                psx_mod_write_word(a, link & 0x00FFFFFFu);
            }
        }
    }
    psx_mod_write_word(PRIM_CURSOR, cur);
}

static void tsr_sky_draw_entry(struct CPUState *cpu, uint32_t address) {
    (void)address;
    sky_split_flush();   /* the other view's dome (two players) */
    if (psx_mod_widescreen_x_margin() <= 0) return;   /* 4:3: as the game draws it */
    sky_cap(cpu);
    s_sky_split_start = psx_mod_read_word(PRIM_CURSOR);
    s_sky_split_fb = psx_mod_read_word(0x800A9E34u);
    s_sky_split_pending = 1;
}

/* 0x8004DB54: called by the sky routine right after its cloud loop, still in
 * the same frame: the last cloud's copies are made there. */
#define SKY_END_FN 0x8004DB54u
static void tsr_sky_end_entry(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    s_sky_vb = s_vblank;
    cloud_flush();
}

static void tsr_ui_icon_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t ra = cpu->gpr[31];
    sky_split_flush();
    cloud_flush();
    if (ra == CLOUD_RA1 || ra == CLOUD_RA2) {
        s_cloud_start = psx_mod_read_word(PRIM_CURSOR);
        s_cloud_fb = psx_mod_read_word(0x800A9E34u);
        s_cloud_pending = 1;
    }
    ui_flush();
    /* the counter's army man, and the alarm clock next to the timer (drawn
     * by the menu screen 0x8003B8BC) */
    if ((ra >= UI_COUNTER_FN && ra < UI_COUNTER_END) || ra == UI_CLOCK_RA) {
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_pending = 1;
        s_ui_edge = 1;
        s_ui_maxlen = 64u;
    } else if (ra == UI_FLAG_RA1 || ra == UI_FLAG_RA2) {
        /* the track preview's chequered flags (one per player): by side */
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_lvsel = 1;   /* not the character select (after the attract
                           * demo the preview's stage number differs) */
        s_ui_pending = 1;
        s_ui_edge = 0;
        s_ui_str = 0;
        s_ui_maxlen = 64u;
    } else if (ra != CLOUD_RA1 && ra != CLOUD_RA2) {
        /* other icons: never anchored; clear stale tags */
        s_ui_start = psx_mod_read_word(PRIM_CURSOR);
        s_ui_pending = 1;
        s_ui_edge = 2;
        s_ui_str = 0;
        s_ui_maxlen = 64u;
        s_ui_legal = 0;
        s_ui_right = ra == 0x8003F5ACu;   /* level select army man */
        if (ra == 0x8003F5ACu || ra == 0x8003F1A0u || ra == 0x8003FD6Cu) s_ui_lvsel = 1;
        /* the toy blocks beside "1 LIVELLO DISPONIBILE" shrink with it */
        /* (the level select's dresser, blocks and level cards are scenery:
         * left at their size) */
        s_ui_scale_mid = ra == 0x8003B6C0u || ra == 0x8003B73Cu ||
                         ra == 0x8003F1A0u;   /* locked level: Mr. Potato Head */
    }
}

static void tsr_draw_sprite_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t ra = cpu->gpr[31];
    if (ra < HUD_ROPE_FN || ra >= HUD_ROPE_END || !hud_active()) return;
    uint32_t sp = cpu->gpr[29];
    uint32_t p = psx_mod_read_word(PRIM_CURSOR);
    if (!ram_address(p) || !ram_address(sp)) return;
    /* Leave room for the game's own DR_TPAGE + SPRT (its 104-byte check). */
    if (p + 40u + 104u >= psx_mod_read_word(PRIM_LIMIT)) return;

    int32_t x = (int16_t)cpu->gpr[4], y = (int16_t)cpu->gpr[5];
    uint32_t u = cpu->gpr[6] & 0xFFu, v = cpu->gpr[7] & 0xFFu;
    int32_t w = psx_mod_read_half(sp + 16u), h = psx_mod_read_half(sp + 20u);
    uint32_t rgb = psx_mod_read_byte(sp + 24u) |
                   ((uint32_t)psx_mod_read_byte(sp + 28u) << 8) |
                   ((uint32_t)psx_mod_read_byte(sp + 32u) << 16);
    uint32_t index = psx_mod_read_half(sp + 36u);
    uint32_t clut_add = psx_mod_read_half(sp + 40u);
    uint32_t mode = psx_mod_read_half(sp + 44u);
    uint32_t ot = psx_mod_read_word(sp + 48u);
    if (!ram_address(ot) || w <= 0 || h <= 0) return;
    uint32_t entry = TPAGE_TABLE + ((index & 0xFFFFu) << 4);
    uint32_t clut = (psx_mod_read_half(entry + 2u) + clut_add) & 0xFFFFu;
    uint32_t page = (psx_mod_read_half(entry) + mode) & 0xFFFFu;
    uint32_t cmd = mode == 96u ? 0x2Cu : 0x2Eu;   /* DrawSprite: 96 = opaque */

    int32_t base = hud_base_y();
    /* A quad's far UV is the texel edge: u + w maps the whole sprite. One
     * that ends on the page edge (256) loses that column instead. */
    uint32_t u1 = u + (uint32_t)w, v1 = v + (uint32_t)h;
    int32_t qw = w, qh = h;
    if (u1 > 0xFFu) { qw -= (int32_t)(u1 - 0xFFu); u1 = 0xFFu; }
    if (v1 > 0xFFu) { qh -= (int32_t)(v1 - 0xFFu); v1 = 0xFFu; }
    int32_t x0 = scale_x(x), x1 = scale_x(x + qw);
    int32_t y0 = scale_y(y, base), y1 = scale_y(y + qh, base);
    uint32_t xy0 = ((uint32_t)y0 << 16) | ((uint32_t)x0 & 0xFFFFu);
    uint32_t xy1 = ((uint32_t)y0 << 16) | ((uint32_t)x1 & 0xFFFFu);
    uint32_t xy2 = ((uint32_t)y1 << 16) | ((uint32_t)x0 & 0xFFFFu);
    uint32_t xy3 = ((uint32_t)y1 << 16) | ((uint32_t)x1 & 0xFFFFu);

    psx_mod_write_byte(p + 3u, 9);
    psx_mod_write_word(p + 4u, (cmd << 24) | rgb);
    psx_mod_write_word(p + 8u, xy0);
    psx_mod_write_word(p + 12u, (clut << 16) | (v << 8) | u);
    psx_mod_write_word(p + 16u, xy1);
    psx_mod_write_word(p + 20u, (page << 16) | (v << 8) | u1);
    psx_mod_write_word(p + 24u, xy2);
    psx_mod_write_word(p + 28u, (v1 << 8) | u);
    psx_mod_write_word(p + 32u, xy3);
    psx_mod_write_word(p + 36u, (v1 << 8) | u1);
    psx_mod_write_word(PRIM_CURSOR, p + 40u);
    add_prim(ot, p);

    psx_mod_write_half(sp + 16u, 0);   /* the game's SPRT: zero size */
    psx_mod_write_half(sp + 20u, 0);
}

static void tsr_vblank(void) {
    ++s_vblank;
    ui_tags_refresh();
    tsr_native_scene_debug_vblank();
    if (s_fps_word)
        psx_mod_write_word(s_fps_word, (s_fps50 &&
                                        (psx_mod_read_byte(SPLIT_SCREEN) == 0u || s_fps50_2p ||
                                         tsr_native_scene_debug_byte(27u) == 1u)) ? 0xFFFFFFFEu : 0xFFFFFFFFu);
    if (s_skip_intros) {
        /* Boot stage at 0x800A96B8: 2-3 Disney Interactive logo, 4 the
         * Traveller's Tales logo, 5 language choice + notices, 35 menus.
         * The logos are real-time scenes, not MDEC movies, so the
         * framework's FMV skip does not see them: tap START (press 4,
         * release 8 VBlanks) while they run and stop before the language
         * screen, which START would answer. */
        uint8_t stage = psx_mod_read_byte(INTRO_STAGE);
        if (stage >= 2u && stage <= 4u) {
            psx_mod_set_pad_override((s_vblank % 12u) < 4u ? 0xFFF7 : 0xFFFF);
            s_intro_tapping = 1;
        } else if (s_intro_tapping) {
            psx_mod_set_pad_override(-1);
            s_intro_tapping = 0;
            if (stage >= 5u) s_skip_intros = 0;   /* once per boot */
        }
    }
    {
        /* In a race: the progress bar was drawn recently (one player) or the
         * split-screen flag is set (two players draw a different bar). */
        int racing = (s_race_seen &&
                      s_vblank - s_last_race_vblank <= OVERCLOCK_HOLD_VBLANKS) ||
                     psx_mod_read_byte(SPLIT_SCREEN) != 0u;
        g_tsr_racing = racing;
        /* everywhere (the menus' 3D scenes need it at 50 fps too) */
        uint32_t want = s_overclock_pct;
        if (g_psx_cpu_overclock_override) want = g_psx_cpu_overclock_override;
        if (g_psx_cpu_overclock_pct != want) g_psx_cpu_overclock_pct = want;
    }
    /* Re-assert periodically: the runtime resets the interval whenever the
     * window changes display or the panel refresh changes. */
    if (s_force_vsync && s_vblank % VSYNC_REASSERT_VBLANKS == 1u)
        gl_renderer_set_swap_interval(1);
}

static void tsr_widescreen_activate(void) {
    char aspect[16], vsync[8], hud[16], oc[16], pool[16], smooth[16], music[16], native[16], fmv[16], fps[16];
    unsigned num = 16u, den = 9u;
    int fit = 1;

    /* First mod-memory allocation: the frame-rate word the recompiled main
     * loop reads at 0x9F000000 (see game.toml). */
    if (!s_fps_word) {
        s_fps_word = psx_mod_memory_alloc(4u, 4u);
        if (s_fps_word != FPS_WORD_ADDR) {
            fprintf(stdout, "toystoryracer: frame-rate word at %08X, expected %08X; 50 fps unavailable\n",
                    s_fps_word, FPS_WORD_ADDR);
            s_fps_word = 0;
        }
    }
    if (s_fps_word) psx_mod_write_word(s_fps_word, 0xFFFFFFFFu);
    psx_mod_set_linked_list_hook(tsr_linked_list_hook);
    psx_mod_set_world_scene_predicate(tsr_world_scene);
    s_fps50 = s_fps_word && psx_mod_option_value(PKG, FEATURE, "frame_rate", fps, sizeof fps) &&
              (strcmp(fps, "50") == 0 || strcmp(fps, "50_2p") == 0);
    s_fps50_2p = s_fps50 && strcmp(fps, "50_2p") == 0;

    if (!psx_mod_option_value(PKG, FEATURE, "aspect", aspect, sizeof aspect))
        strcpy(aspect, "fit");

    if (strcmp(aspect, "16-9") == 0) { fit = 0; }
    else if (strcmp(aspect, "21-9") == 0) { num = 21u; fit = 0; }
    else if (strcmp(aspect, "32-9") == 0) { num = 32u; fit = 0; }

    s_force_vsync = 1;
    if (psx_mod_option_value(PKG, FEATURE, "high_refresh_vsync", vsync, sizeof vsync) &&
        strcmp(vsync, "off") == 0)
        s_force_vsync = 0;

    s_hud_percent = 100;
    if (psx_mod_option_value(PKG, FEATURE, "hud_size", hud, sizeof hud)) {
        int pct = 0;
        if (sscanf(hud, "%d", &pct) == 1 && pct >= 50 && pct <= 100)
            s_hud_percent = pct;
    }

    g_psx_cpu_overclock_pct = 100u;
    s_overclock_pct = 100u;
    s_race_seen = 0;
    if (psx_mod_option_value(PKG, FEATURE, "cpu_overclock", oc, sizeof oc)) {
        int pct = 0;
        if (sscanf(oc, "%d", &pct) == 1 && pct >= 100 && pct <= 800)
            s_overclock_pct = (uint32_t)pct;
    }

    /* Near-polygon pool (experimental): the manifest's "expanded" patches move
     * the pool into the upper 6 MB, which exists only with the 8 MB main RAM
     * map. Must be requested here, before memory_init(). */
    int pool_expanded = psx_mod_option_value(PKG, FEATURE, "polygon_pool", pool, sizeof pool) &&
                        strcmp(pool, "expanded") == 0;
    if (pool_expanded)
        (void)psx_mod_set_main_ram_8mb(1);

    /* Frame smoothing: the game shows a new frame every second VBlank (25 fps
     * on PAL). The OpenGL presenter can crossfade completed frames up to the
     * display rate; FLIP spreads each fade over the real flip period, and the
     * motion-adaptive blend cuts cleanly on large changes to avoid trails.
     * Presentation only: game speed, timers and audio are unchanged. */
    int smoothing = psx_mod_option_value(PKG, FEATURE, "frame_smoothing", smooth, sizeof smooth) &&
                    strcmp(smooth, "blend") == 0;
    if (smoothing) {
        (void)psx_mod_set_frame_interpolation(0u);
        (void)psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE);
        (void)psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP);
    }

    int smooth_music = !(psx_mod_option_value(PKG, FEATURE, "music_change", music, sizeof music) &&
                         strcmp(music, "original") == 0);
    g_psx_cd_pause_cycles_cap = smooth_music ? CD_PAUSE_FAST_CYCLES : -1;

    int native_scene = psx_mod_option_value(PKG, FEATURE, "native_scenery", native, sizeof native) &&
                       strcmp(native, "on") == 0;
    tsr_native_scene_activate(native_scene, 0);

    /* Skip videos: the framework holds START while an FMV (MDEC + XA)
     * plays, so the intro movies end at once and the menus come up. */
    int skip_fmv = !(psx_mod_option_value(PKG, FEATURE, "skip_videos", fmv, sizeof fmv) &&
                     strcmp(fmv, "off") == 0);
    (void)psx_mod_set_auto_skip_fmv(skip_fmv);
    s_skip_intros = skip_fmv;

    fprintf(stdout, "TOY STORY RACER WIDESCREEN PLUGIN ACTIVATED (%s, HUD %d%%, vsync %s, CPU %u%%, pool %s, smoothing %s, music change %s, native scenery %s, %s fps)\n",
            aspect, s_hud_percent, s_force_vsync ? "forced" : "runtime",
            (unsigned)s_overclock_pct, pool_expanded ? "4600/8MB" : "2300", smoothing ? "blend" : "off",
            smooth_music ? "smooth" : "original", native_scene ? "on" : "off", s_fps50_2p ? "50 (also 2P)" : s_fps50 ? "50" : "25");
    s_vblank = 0;
    s_faces_start = 0;
    (void)psx_mod_set_fixed_display_aspect(num, den);
    if (fit)
        (void)psx_mod_set_adaptive_display_aspect(0u, 0u);
}

PSX_MOD_CONSTRUCTOR(psx_register_toy_story_racer_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin(
        "toystoryracer.widescreen", tsr_widescreen_activate);
    (void)psx_mod_register_vblank_plugin(
        "toystoryracer.widescreen", tsr_vblank);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", HUD_FACES_FN, tsr_faces_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", HUD_ROPE_FN, tsr_rope_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", DRAW_SPRITE_FN, tsr_draw_sprite_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", SECTION_DRAW_FN, tsr_native_scene_section_draw);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", SECTION_LIST_FN, tsr_native_scene_builder_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", SECTION_LIST_FN2, tsr_native_scene_builder_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", 0x8002207Cu, tsr_native_model_draw);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", 0x8002274Cu, tsr_native_model_draw);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", 0x80023F58u, tsr_native_model_draw);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", 0x80024568u, tsr_native_model_draw);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", 0x80012040u, tsr_native_sprite_list_draw);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", UI_TEXT_FN, tsr_ui_text_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", FRAME_START_FN, tsr_frame_start_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", UI_ICON_FN, tsr_ui_icon_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", SKY_END_FN, tsr_sky_end_entry);
    (void)psx_mod_register_function_entry_plugin(
        "toystoryracer.widescreen", SKY_DRAW_FN, tsr_sky_draw_entry);
}
