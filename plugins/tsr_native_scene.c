/*
 * Toy Story Racer - native scenery renderer (experimental).
 *
 * The game draws the track scenery with hand-written MIPS: per visible
 * section it transforms the section's object on the GTE, clips the polygons
 * against the near plane into a small pool, subdivides near ones and links
 * integer-precision GT3/GT4 packets into the ordering table (OT).
 *
 * This file replaces that work. At the entry of the section draw routine
 * (0x80043538: a0 = section list, a1 = count, a2 = OT base, a3 = pass) it
 * sets the count to 0, so the game draws no scenery and uses none of its
 * pool, and instead walks the whole level itself: every object is
 * transformed in floating point with the same camera matrix and projection
 * the game uses, clipped in 3D, and emitted as GT3 packets into a mod packet
 * arena, linked into the game's own OT slots (same depth -> slot table), so
 * karts and pick-ups still sort against the scenery exactly as before. Each
 * packet carries the exact screen position and 1/z of its vertices (the
 * arena's precision suffix), so the GPU draws it without the PS1's integer
 * snapping or affine texture warp, and the draw distance is no longer bound
 * to the 4:3 visibility lists or the polygon pool.
 *
 * Level format (Traveller's Tales engine, see juanmv94's
 * TravellersTalesPSXCollisionViewer):
 *   0x800A9F5C -> sections, 20 bytes: x,y,z int32, radius u16, flags u8
 *                 (0 ends the list), u8, ptr to a 40-byte record whose +20
 *                 points at the section's scenario item.
 *   item (24 bytes): x,y,z int32, rx,ry,rz int16, zbias u8, flags u8, obj
 *   item (32 bytes): x,y,z int32, rx,ry,rz int16, sx,sy,sz int16, u16,u16, obj
 *   obj: int32 vertex count (negative = special material, n+1 extra words),
 *        vertices {x,y,z int16, colour u16}, face groups {flags u8,
 *        texmap u8, count u8, pad} ending with 0xFFFF; faces: 4 index bytes,
 *        + 8 UV bytes when textured (flags & 0x10 = untextured).
 *        flags & 1 = triangle (p2,p3,p4), else quad p1..p4 (cyclic),
 *        flags & 2 = double sided, flags & 0x60 = blend mode.
 *   0x800DB070: texture table, 16 bytes per texmap: tpage, clut base.
 * Transform (checked against the GTE projection ring): camera-space
 * v = Mcam * (Rx*Ry*Rz * S * vertex + item - campos), with Mcam the int16
 * matrix at view+32 (its first row carries the widescreen X scale) and the
 * camera position at view+80, view = 0x1F800374.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "mod_plugins.h"
#include "mod_memory.h"
#include "cpu_state.h"

#define SECTIONS_PTR   0x800A9F5Cu
#define TEX_TABLE      0x800DB070u
#define VIEW           0x1F800374u
#define ZTAB_PTR       0x800A9D38u   /* depth -> OT byte offset table (u16 per 4 bytes) */
#define ZFAR_VAR       0x800A9D6Cu   /* farthest valid depth index */
#define ZBIAS_TABLE    0x8009EF60u   /* per-item depth bias (+80) */
#define BUFFER_VAR     0x800A9E34u   /* current frame buffer base (double buffer) */
#define BUFFER0        0x80193518u
#define FLIP_COUNT     0x800A9DF8u
#define GAME_STAGE     0x800A96B8u   /* 35 menus, 36 loading, 38 track/race */

#define MAX_OBJS     256
#define MAX_VERTS    256
#define MAX_ITEMS    512
#define MAX_FACES    16384
#define ARENA_BYTES  (4u * 1024u * 1024u - 4096u)
#define PACKET_BYTES 80u             /* tag + GT3 (9 words) + precision suffix (10 words) */
#define NEAR_Z       24.0f
#define GUARD_X0    -1000.0f
#define GUARD_X1     1020.0f
#define GUARD_Y0     -500.0f
#define GUARD_Y1      740.0f

typedef struct { float x, y, z; float r, g, b; float u, v; } NVert;

typedef struct {
    uint8_t n;           /* 3 or 4 */
    uint8_t idx[4];
    uint8_t uv[4][2];
    uint8_t flags, texmap;
    uint8_t pal;         /* palette: u0/64 + (v0/64)*4 of the stored first UV */
} NFace;

typedef struct {
    uint32_t addr;
    int nverts;
    int16_t v[MAX_VERTS][3];
    uint16_t col[MAX_VERTS];
    int first_face, nfaces;
} NObj;

typedef struct {
    float pos[3];
    float m[9];          /* rotation * scale */
    float cx, cy, cz, radius;   /* section bounding sphere */
    int obj;
    int zbias;
    int sec;             /* section index (visibility bitmap bit) */
    uint32_t item_addr, rec_addr;
    int rotated;         /* matrix comes from the 40-byte record (re-read each frame) */
    int kind;            /* 0 mesh, 2 screen-facing sprites, 3 yaw-facing sprites */
    int spr_first, spr_count;
} NItem;

static NObj   s_objs[MAX_OBJS];
static int    s_nobjs;
static NFace  s_faces[MAX_FACES];
static int    s_nfaces;
static NItem  s_items[MAX_ITEMS];
static int    s_nitems;
static uint32_t s_level_sig;

/* Sprite groups (section types 2/3): int32 count, count x {x,y,z int32}
 * positions, then per sprite an int32 quad count and 44-byte quads:
 * 4 x {x,y int16} corners, int16 pad, u16 (texmap & 0x1F | blend & 0x60),
 * 4 x {u,v}, 4 x {r,g,b,pad}. */
typedef struct { float pos[3]; int q_first, q_n; } NSprite;
typedef struct { int16_t xy[4][2]; uint8_t uv[4][2]; uint8_t col[4][3]; uint8_t tm; } NSpriteQuad;
#define MAX_SPRITES 4096
#define MAX_SQUADS  8192
static NSprite     s_sprites[MAX_SPRITES];
static NSpriteQuad s_squads[MAX_SQUADS];
static int         s_nsprites, s_nsquads;
typedef struct { uint32_t addr; int first, count; } NSpriteObj;
static NSpriteObj  s_sprobjs[256];
static int         s_nsprobjs;
static uint16_t s_tex_tpage[32], s_tex_clut[32];

static int      s_enabled;
static uint32_t s_arena[2];
static uint8_t *s_arena_host[2];
static uint32_t s_arena_used[2];
static int      s_last_buf = -1;
static uint32_t s_debug_mode_addr;
static uint32_t s_frame, s_views[4];
static int      s_nviews;
extern int      g_tsr_racing;
int tsr_ui_sky_recent(void);   /* plugin: the menus' cloud sky drawn lately */
static float    s_draw_distance = 0.0f;   /* 0 = whole level */
static uint32_t s_stat_tris, s_stat_frames;
static uint64_t s_emit_us, s_emit_n;
static uint32_t s_far_cache_ztab, s_far_cache_lim, s_far_cache_off;
#define FLAT_BANK_OPAQUE 0x7E01u
#define FLAT_BANK_SEMI   0x7E02u
static int s_flat_bank_ok;
/* Smooth round halo (128x128, 15-bit direct) replacing the lamps' 32x32
 * glow texture, whose rim is dark grey instead of black: added over the
 * smooth native walls at high resolution it showed the quad's square. */
#define GLOW_BANK        0x7E03u
#define GLOW_BANK2       0x7E04u   /* the 64x64 halo (cellar lamp) */
#define GLOW_SIZE        128u
static int s_glow_bank_ok;
static int s_force_glow;      /* emit_face: draw the next face from this glow bank (0 = no) */
/* Halos are drawn without the native depth test (HTP1), in the game's own
 * OT slot: like the game's, they brighten whatever the painter order puts
 * under them (the boxes around a lamp catch its glare). */

static inline uint32_t rd32(uint32_t a) { return psx_mod_read_word(a); }
static inline uint16_t rd16(uint32_t a) { return psx_mod_read_half(a); }
static inline uint8_t  rd8(uint32_t a)  { return psx_mod_read_byte(a); }

/* ---- level parse ---------------------------------------------------------- */

static int find_or_load_obj(uint32_t addr) {
    for (int i = 0; i < s_nobjs; i++)
        if (s_objs[i].addr == addr) return i;
    if (s_nobjs >= MAX_OBJS) return -1;
    int32_t n = (int32_t)rd32(addr);
    int special = n < 0;
    if (special) return -1;   /* special material (mirror/env data): left to the game */
    if (n <= 0 || n > MAX_VERTS) return -1;
    NObj *o = &s_objs[s_nobjs];
    o->addr = addr;
    o->nverts = n;
    uint32_t p = addr + 4u;
    for (int i = 0; i < n; i++, p += 8u) {
        o->v[i][0] = (int16_t)rd16(p);
        o->v[i][1] = (int16_t)rd16(p + 2u);
        o->v[i][2] = (int16_t)rd16(p + 4u);
        o->col[i]  = rd16(p + 6u);
    }
    if (special) p += (uint32_t)(n + 1) * 4u;
    o->first_face = s_nfaces;
    for (int guard = 0; guard < 256 && rd16(p) != 0xFFFFu; guard++) {
        uint8_t flags = rd8(p), texmap = rd8(p + 1u), cnt = rd8(p + 2u);
        if (rd8(p + 3u) >= 0xF0u) break;
        p += 4u;
        for (int f = 0; f < cnt; f++) {
            if (s_nfaces >= MAX_FACES) return -1;
            NFace *fc = &s_faces[s_nfaces++];
            for (int k = 0; k < 4; k++) fc->idx[k] = rd8(p + (uint32_t)k);
            p += 4u;
            if (!(flags & 0x10u)) {
                for (int k = 0; k < 4; k++) {
                    fc->uv[k][0] = rd8(p + (uint32_t)k * 2u);
                    fc->uv[k][1] = rd8(p + (uint32_t)k * 2u + 1u);
                }
                p += 8u;
            } else {
                memset(fc->uv, 0, sizeof fc->uv);
            }
            fc->flags = flags;
            fc->texmap = texmap;
            /* the game (0x80017110): stored first UV pair, before the
             * triangle shift below */
            fc->pal = (uint8_t)((fc->uv[0][0] >> 6) + (fc->uv[0][1] >> 6) * 4);
            if (flags & 1u) {     /* triangle: vertices p1..p3, UVs of p2..p4 */
                fc->n = 3;
                for (int k = 0; k < 3; k++) {
                    fc->uv[k][0] = fc->uv[k + 1][0];
                    fc->uv[k][1] = fc->uv[k + 1][1];
                }
            } else {
                fc->n = 4;
            }
            int bad = 0;
            for (int k = 0; k < fc->n; k++) if (fc->idx[k] >= n) bad = 1;
            if (bad) s_nfaces--;
        }
    }
    o->nfaces = s_nfaces - o->first_face;
    return s_nobjs++;
}

static void euler_matrix(int rx, int ry, int rz, float sx, float sy, float sz, float *m) {
    const float k = 6.28318530718f / 4096.0f;
    float a = rx * k, b = ry * k, c = rz * k;
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
    /* Rx * Ry * Rz */
    float r[9] = {
        cb * cc,                 -cb * sc,                 sb,
        sa * sb * cc + ca * sc,  -sa * sb * sc + ca * cc, -sa * cb,
        -ca * sb * cc + sa * sc,  ca * sb * sc + sa * cc,  ca * cb };
    for (int i = 0; i < 3; i++) {
        m[i * 3 + 0] = r[i * 3 + 0] * sx;
        m[i * 3 + 1] = r[i * 3 + 1] * sy;
        m[i * 3 + 2] = r[i * 3 + 2] * sz;
    }
}

static int load_sprite_obj(uint32_t addr, int *first, int *count) {
    for (int i = 0; i < s_nsprobjs; i++)
        if (s_sprobjs[i].addr == addr) { *first = s_sprobjs[i].first; *count = s_sprobjs[i].count; return 1; }
    if (s_nsprobjs >= 256) return 0;
    int32_t ns = (int32_t)rd32(addr);
    if (ns <= 0 || ns > 64 || s_nsprites + ns > MAX_SPRITES) return 0;
    int f = s_nsprites;
    uint32_t p = addr + 4u;
    for (int k = 0; k < ns; k++, p += 12u) {
        NSprite *sp = &s_sprites[f + k];
        for (int j = 0; j < 3; j++) sp->pos[j] = (float)(int32_t)rd32(p + (uint32_t)j * 4u);
    }
    for (int k = 0; k < ns; k++) {
        int32_t cnt = (int32_t)rd32(p);
        p += 4u;
        if (cnt < 0 || cnt > 64 || s_nsquads + cnt > MAX_SQUADS) return 0;
        NSprite *sp = &s_sprites[f + k];
        sp->q_first = s_nsquads; sp->q_n = cnt;
        for (int q = 0; q < cnt; q++, p += 44u) {
            NSpriteQuad *sq = &s_squads[s_nsquads++];
            for (int j = 0; j < 4; j++) {
                sq->xy[j][0] = (int16_t)rd16(p + (uint32_t)j * 4u);
                sq->xy[j][1] = (int16_t)rd16(p + (uint32_t)j * 4u + 2u);
                sq->uv[j][0] = rd8(p + 20u + (uint32_t)j * 2u);
                sq->uv[j][1] = rd8(p + 21u + (uint32_t)j * 2u);
                sq->col[j][0] = rd8(p + 28u + (uint32_t)j * 4u);
                sq->col[j][1] = rd8(p + 29u + (uint32_t)j * 4u);
                sq->col[j][2] = rd8(p + 30u + (uint32_t)j * 4u);
            }
            sq->tm = (uint8_t)rd16(p + 18u);
        }
    }
    s_nsprites += ns;
    s_sprobjs[s_nsprobjs].addr = addr; s_sprobjs[s_nsprobjs].first = f; s_sprobjs[s_nsprobjs].count = ns;
    s_nsprobjs++;
    *first = f; *count = ns;
    return 1;
}

static uint32_t level_signature(uint32_t sec) {
    uint32_t h = sec;
    for (uint32_t i = 0; i < 16u; i++) h = h * 31u + rd32(sec + i * 4u);
    /* every section's type and record: the parse must not keep a level
     * that was still being loaded (type bytes change after the first
     * frames; the menu scene's characters were left to the game) */
    for (uint32_t n = 0; n < 1024u; n++) {
        uint32_t r = sec + n * 20u, f = rd8(r + 14u);
        if (f == 0u) break;
        h = h * 31u + f;
        h = h * 31u + rd32(r + 16u);
    }
    return h;
}

/* Sections the native path draws itself (1) or leaves to the game (0). */
#define MAX_SECTIONS 1024
static uint8_t  s_sec_native[MAX_SECTIONS];
static uint8_t  s_sec_sprite[MAX_SECTIONS];
static int      s_no_sprites;   /* debug: leave sprite groups to the game */
static int      s_no_depth;     /* debug: painter order only (HTP1, no marker) */
static int      s_no_split;     /* debug: no depth-range subdivision */
static int      s_no_expand;    /* debug: no crack-closing edge expansion */
static float    s_near = NEAR_Z;   /* near clip plane (debug byte +3: value * 4) */
static int      s_false_colour; /* debug: flat colour per section index */
static int      s_no_cull;      /* debug: no back-face culling */
static int      s_pal_override = -1;    /* models: palette from vertex 0 */
static int      s_pal_scenery = -1;     /* scenery faces: the game's palette rule */
static int      s_dbg_nosuffix;
static unsigned s_dbg_mflags;    /* debug byte +23: 1 negate z, 2 transpose, 4 flip cull */
static uint32_t s_fan_ot;        /* forced OT entry for shading-fan pieces */
static int      s_poly_depth;    /* split level of the polygon being emitted */
static int      s_split_max = 5; /* debug +24: max split level (0 = 5) */
static unsigned s_under_rule;
static unsigned s_split_rule;    /* debug +26: 1 no shade split, 2 no near-crossing depth split, 4 no other depth split */    /* debug +25: 1 underlay only on unsplit polygons */
static uint32_t s_vb_tris;       /* triangles since the last VBlank (debug +28) */
static int      s_underlay_on = 1;   /* crack underlay: races only (menu characters are scenery) */
static int      s_far_reject;
static int      s_early_opaque = 1;   /* opaque native faces first (debug +42 = 1: off) */
static int      s_ztab_half;     /* models: depth table indexed by halfword */
static int      s_cull_flip;     /* models: the game's NCLIP sign flag (0x800A9DAC) */
static float    s_qmul = 1.0f;   /* model depth scale (debug byte +22: value / 16) */
static float    s_zsf3, s_zsf4;         /* models: GTE depth scales / 4096 (0 = scenery rule) */
static int      s_semi_override = -1;   /* models: blend flag from the command */
static int      s_cull;         /* emit_tri: drop back-facing triangles */
static void     freeze_restore(void);
static uint32_t s_sec_base, s_sec_count;
static int16_t  s_sec_item[MAX_SECTIONS];   /* native item index, -1 = left to the game */

/*
 * Section types (record byte 14 & 0x6F), as dispatched by the game's table
 * at 0x8002B890:
 *   1: 24-byte item (zbias +18, rotation flags +19, object +20)
 *   9: 32-byte item (scale +18, zbias +24, rotation flags +25, object +28)
 *   others: billboards / special objects -> left to the game.
 * Rotation: if the item's angles are non-zero the game uses the 3x3 matrix
 * stored in the 40-byte record (int16, 4096 = 1.0); rotation flags & 7 select
 * an animated matrix table instead (left to the game for now). Unrotated
 * type-9 items scale the object axes by the item's scale.
 */
#define SKIPLOG(why) ((void)0)
static int load_level(void) {
    uint32_t sec = rd32(SECTIONS_PTR);
    if ((sec & 0xFF000000u) != 0x80000000u) return 0;
    uint32_t sig = level_signature(sec);
    if (sig == s_level_sig && s_sec_base == sec) return s_nitems > 0;
    s_nobjs = s_nfaces = s_nitems = 0;
    s_nsprites = s_nsquads = s_nsprobjs = 0;
    s_level_sig = sig;
    s_sec_base = sec;
    memset(s_sec_native, 0, sizeof s_sec_native);
    memset(s_sec_sprite, 0, sizeof s_sec_sprite);
    memset(s_sec_item, 0xFF, sizeof s_sec_item);

    uint32_t n = 0, skipped = 0;
    for (; n < MAX_SECTIONS; n++) {
        uint32_t r = sec + n * 20u;
        uint8_t flags = rd8(r + 14u);
        if (flags == 0) break;
        if (s_nitems >= MAX_ITEMS) { skipped++; continue; }
        /* 0x40 only routes the section to the builder's second list; the
         * game dispatches 0x41/0x49 to the same handlers as 1/9. */
        uint32_t type = flags & 0x2Fu;
        uint32_t rec = rd32(r + 16u);
        if ((rec & 0xFF000000u) != 0x80000000u ||
            (type != 1u && type != 9u && type != 2u && type != 3u)) { SKIPLOG("type"); skipped++; continue; }
        uint32_t it = rd32(rec + 20u);
        if ((it & 0xFF000000u) != 0x80000000u) { skipped++; continue; }
        NItem *ni = &s_items[s_nitems];
        ni->kind = 0;
        if (type == 2u || type == 3u) {
            /* sprite group: 24-byte item, angles +12, zbias +18, object +20 */
            uint32_t sobj = rd32(it + 20u);
            int sf = 0, sc = 0;
            if ((sobj & 0xFF000000u) != 0x80000000u || !load_sprite_obj(sobj, &sf, &sc)) { skipped++; continue; }
            ni->kind = (int)type;
            ni->spr_first = sf; ni->spr_count = sc;
            ni->zbias = rd8(it + 18u) & 15;
            ni->item_addr = it; ni->rec_addr = rec; ni->rotated = 0;
            ni->obj = 0;
            ni->cx = (float)(int32_t)rd32(r);
            ni->cy = (float)(int32_t)rd32(r + 4u);
            ni->cz = (float)(int32_t)rd32(r + 8u);
            ni->radius = (float)rd16(r + 12u);
            ni->sec = (int)n;
            s_sec_native[n] = 1;
            s_sec_item[n] = (int16_t)s_nitems;
            s_sec_sprite[n] = 1;
            s_nitems++;
            continue;
        }
        ni->pos[0] = (float)(int32_t)rd32(it);
        ni->pos[1] = (float)(int32_t)rd32(it + 4u);
        ni->pos[2] = (float)(int32_t)rd32(it + 8u);
        int rotated = rd32(it + 12u) != 0u || rd16(it + 16u) != 0u;
        uint8_t rotf;
        uint32_t obj;
        float sx = 1.0f, sy = 1.0f, sz = 1.0f;
        if (type == 9u) {
            int16_t a = (int16_t)rd16(it + 18u), b = (int16_t)rd16(it + 20u), c = (int16_t)rd16(it + 22u);
            sx = a / 4096.0f; sy = b / 4096.0f; sz = c / 4096.0f;
            ni->zbias = rd8(it + 24u) & 15;
            rotf = rd8(it + 25u);
            obj = rd32(it + 28u);
        } else {
            ni->zbias = rd8(it + 18u) & 15;
            rotf = rd8(it + 19u);
            obj = rd32(it + 20u);
        }
        if (rotf & 7u) { SKIPLOG("animated"); skipped++; continue; }          /* animated matrix */
        ni->item_addr = it; ni->rec_addr = rec; ni->rotated = rotated;
        if (rotated) {
            for (int k = 0; k < 9; k++) ni->m[k] = (int16_t)rd16(rec + (uint32_t)k * 2u) / 4096.0f;
        } else {
            memset(ni->m, 0, sizeof ni->m);
            ni->m[0] = type == 9u ? sx : 1.0f;
            ni->m[4] = type == 9u ? sy : 1.0f;
            ni->m[8] = type == 9u ? sz : 1.0f;
        }
        ni->cx = (float)(int32_t)rd32(r);
        ni->cy = (float)(int32_t)rd32(r + 4u);
        ni->cz = (float)(int32_t)rd32(r + 8u);
        ni->radius = (float)rd16(r + 12u);
        ni->obj = (obj & 0xFF000000u) == 0x80000000u ? find_or_load_obj(obj) : -1;
        if (ni->obj < 0) { SKIPLOG("object"); skipped++; continue; }
        ni->sec = (int)n;
        s_sec_native[n] = 1;
        s_sec_item[n] = (int16_t)s_nitems;
        s_nitems++;
    }
    s_sec_count = n;
    for (int t = 0; t < 32; t++) {
        s_tex_tpage[t] = rd16(TEX_TABLE + (uint32_t)t * 16u);
        s_tex_clut[t]  = rd16(TEX_TABLE + (uint32_t)t * 16u + 2u);
    }
    fprintf(stdout, "tsr native scene: level %08X, %u sections: %d native items, %u left to the game, %d objects, %d faces\n",
            sec, n, s_nitems, skipped, s_nobjs, s_nfaces);
    fflush(stdout);
    return 1;
}

/* ---- emission ------------------------------------------------------------- */

typedef struct {
    float m[9];          /* camera matrix (row-major) */
    float cam[3];
    float h, ofx, ofy;
    uint32_t ot, ztab, zfar;
    uint32_t zbias[16];
    int rect_on;         /* reject faces wholly outside the area's screen rect */
    float rx0, ry0, rx1, ry1;
    uint32_t marker_entry;   /* OT slot of the view's depth-clear marker */
    uint32_t bank;       /* host texture bank for the next packets (0 = VRAM) */
    uint8_t *out;
    uint32_t out_addr, out_used, out_cap;
} Ctx;

Ctx *g_tsr_ctx;

static int clip_poly(NVert *in, int n, NVert *out, int plane) {
    /* plane 0: z >= NEAR; 1..4: screen guard band (x0, x1, y0, y1) in view space */
    int m = 0;
    for (int i = 0; i < n; i++) {
        NVert *a = &in[i], *b = &in[(i + 1) % n];
        float da, db;
        Ctx *c = g_tsr_ctx;
        switch (plane) {
        case 0: da = a->z - s_near; db = b->z - s_near; break;
        case 1: da = c->h * a->x + (c->ofx - GUARD_X0) * a->z; db = c->h * b->x + (c->ofx - GUARD_X0) * b->z; break;
        case 2: da = -c->h * a->x + (GUARD_X1 - c->ofx) * a->z; db = -c->h * b->x + (GUARD_X1 - c->ofx) * b->z; break;
        case 3: da = c->h * a->y + (c->ofy - GUARD_Y0) * a->z; db = c->h * b->y + (c->ofy - GUARD_Y0) * b->z; break;
        default: da = -c->h * a->y + (GUARD_Y1 - c->ofy) * a->z; db = -c->h * b->y + (GUARD_Y1 - c->ofy) * b->z; break;
        }
        if (m >= 14) break;   /* never past the 16-vertex buffers */
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            NVert *o = &out[m++];
            o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = a->z + (b->z - a->z) * t;
            o->r = a->r + (b->r - a->r) * t; o->g = a->g + (b->g - a->g) * t; o->b = a->b + (b->b - a->b) * t;
            o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
        }
    }
    return m;
}

static inline void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void putf(uint8_t *p, float v) { memcpy(p, &v, 4); }

static inline uint32_t pack_xy(float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    if (ix < -1024) ix = -1024; if (ix > 1023) ix = 1023;
    if (iy < -1024) iy = -1024; if (iy > 1023) iy = 1023;
    return ((uint32_t)(iy & 0xFFFF) << 16) | ((uint32_t)ix & 0xFFFFu);
}

static inline uint32_t pack_col(const NVert *v) {
    int r = (int)(v->r + 0.5f), g = (int)(v->g + 0.5f), b = (int)(v->b + 0.5f);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

static inline uint32_t pack_uv(const NVert *v) {
    int u = (int)(v->u + 0.5f), w = (int)(v->v + 0.5f);
    if (u < 0) u = 0; if (u > 255) u = 255; if (w < 0) w = 0; if (w > 255) w = 255;
    return (uint32_t)u | ((uint32_t)w << 8);
}

/* Emit one triangle (camera-space vertices, already clipped). */
/* Link only into the view's own OT (base .. farthest slot the depth table
 * can name): never let a bad index write a link anywhere else in RAM. */
static inline int ot_ok(const Ctx *c, uint32_t e) {
    return e >= c->ot && e <= c->ot + s_far_cache_off && ((e - c->ot) & 3u) == 0u;
}

static int back_facing(const NVert *a, const NVert *b, const NVert *d) {
    float ax = b->x - a->x, ay = b->y - a->y, az = b->z - a->z;
    float bx = d->x - a->x, by = d->y - a->y, bz = d->z - a->z;
    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    return (nx * a->x + ny * a->y + nz * a->z >= 0.0f) ^ s_cull_flip;
}

static void emit_tri(Ctx *c, const NVert *a, const NVert *b, const NVert *d,
                     uint32_t cmd, uint16_t clut, uint16_t tpage, uint32_t ot_entry, int depth) {
    if (c->out_used + PACKET_BYTES > c->out_cap) return;
    if (depth == 0 && s_cull && back_facing(a, b, d)) return;
    /* Opaque native triangles are all drawn first, as one run right after
     * the view's depth clear (the next slot after the marker's): the native
     * depth sorts them, and everything the game still draws (shadows,
     * antenna lines, HUD) and the blended native faces come later, depth
     * tested. In their depth-sorted slots they interleaved with those and
     * cut the renderer's batches into hundreds of pieces (GPU load). */
    /* Drawn early, faces lose the game's slot order, which decided ties
     * between coplanar faces (a photo on its frame, decals): nudge each one
     * nearer by a hair per slot it was nearer in the game's order, so the
     * face the game drew later still wins an exact tie. */
    float slot_nudge = 1.0f;
    if (s_early_opaque && cmd == 0x34u && !s_force_glow && !s_no_depth &&
        c->marker_entry >= c->ot + 4u) {
        if (ot_entry >= c->ot && ot_entry <= c->marker_entry)
            slot_nudge = 1.0f + 4e-6f * (float)((c->marker_entry - ot_entry) >> 2);
        ot_entry = c->marker_entry - 4u;
    }
    const NVert *v[3] = { a, b, d };
    float sx[3], sy[3], q[3], qmax = 0.0f;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < 3; i++) {
        float iz = 1.0f / v[i]->z;
        sx[i] = c->ofx + c->h * v[i]->x * iz;
        sy[i] = c->ofy + c->h * v[i]->y * iz;
        q[i] = iz;
        if (iz > qmax) qmax = iz;
        if (sx[i] < minx) minx = sx[i]; if (sx[i] > maxx) maxx = sx[i];
        if (sy[i] < miny) miny = sy[i]; if (sy[i] > maxy) maxy = sy[i];
    }
    if (maxx - minx >= 1023.0f || maxy - miny >= 511.0f) {
        if (depth >= 8) return;
        /* PS1 size limit: split the longest edge in view space and recurse. */
        float l0 = (sx[0]-sx[1])*(sx[0]-sx[1]) + (sy[0]-sy[1])*(sy[0]-sy[1]);
        float l1 = (sx[1]-sx[2])*(sx[1]-sx[2]) + (sy[1]-sy[2])*(sy[1]-sy[2]);
        float l2 = (sx[2]-sx[0])*(sx[2]-sx[0]) + (sy[2]-sy[0])*(sy[2]-sy[0]);
        int e = (l0 >= l1 && l0 >= l2) ? 0 : (l1 >= l2 ? 1 : 2);
        const NVert *p = v[e], *s = v[(e + 1) % 3], *o = v[(e + 2) % 3];
        NVert mid;
        mid.x = (p->x + s->x) * 0.5f; mid.y = (p->y + s->y) * 0.5f; mid.z = (p->z + s->z) * 0.5f;
        mid.r = (p->r + s->r) * 0.5f; mid.g = (p->g + s->g) * 0.5f; mid.b = (p->b + s->b) * 0.5f;
        mid.u = (p->u + s->u) * 0.5f; mid.v = (p->v + s->v) * 0.5f;
        emit_tri(c, p, &mid, o, cmd, clut, tpage, ot_entry, depth + 1);
        emit_tri(c, &mid, s, o, cmd, clut, tpage, ot_entry, depth + 1);
        return;
    }
    /* Close the hairline cracks between neighbouring faces that do not
     * share exact vertices (separately placed sections, T-junctions): the
     * PS1 snaps both to the same integer pixels, exact positions leave a
     * sub-pixel gap that shows the black clear colour at high resolution.
     * Opaque triangles get an underlay: the same triangle with every edge
     * pushed out by 0.4 pixel (mitred corners, capped) and a hair farther
     * in depth, so it only shows through gaps; the exact triangle on top
     * keeps every texel where it was. Blended faces and models (their
     * silhouettes would grow a fringe of neighbouring texels) are left alone. */
    int under = 0;
    float ux[3], uy[3];
    if (cmd == 0x34u && !s_no_expand && !s_ztab_half && s_underlay_on && !((s_under_rule & 1u) && s_poly_depth) && c->out_used + 2u * PACKET_BYTES <= c->out_cap) {
        const float d = 0.4f;
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        if (fabsf(area) > 1e-4f) {
            float sgn = area > 0.0f ? 1.0f : -1.0f, nx[3], ny[3];
            for (int i = 0; i < 3; i++) {     /* outward normal of edge i -> i+1 */
                int j = (i + 1) % 3;
                float ex = sx[j] - sx[i], ey = sy[j] - sy[i], l = sqrtf(ex * ex + ey * ey);
                if (l < 1e-6f) { nx[i] = ny[i] = 0.0f; continue; }
                nx[i] = sgn * ey / l; ny[i] = -sgn * ex / l;
            }
            for (int i = 0; i < 3; i++) {     /* vertex i joins edges i-1 and i */
                int h = (i + 2) % 3;
                float mx = nx[h] + nx[i], my = ny[h] + ny[i], dot = 1.0f + nx[h] * nx[i] + ny[h] * ny[i];
                float k = dot > 0.1f ? d / dot : 0.0f;
                float dx = mx * k, dy = my * k, len = sqrtf(dx * dx + dy * dy);
                if (len > 3.0f * d) { dx *= 3.0f * d / len; dy *= 3.0f * d / len; }
                ux[i] = sx[i] + dx; uy[i] = sy[i] + dy;
            }
            under = 1;
        }
    }
    uint8_t *pk = c->out + c->out_used;
    uint32_t addr = c->out_addr + c->out_used;
    uint32_t prev = rd32(ot_entry);
    if ((cmd & 0xFCu) == 0x30u) {
        /* untextured (gouraud) triangle: plain G3, integer positions */
        put32(pk + 0,  (prev & 0x00FFFFFFu) | (6u << 24));
        put32(pk + 4,  (cmd << 24) | pack_col(a));
        put32(pk + 8,  pack_xy(sx[0], sy[0]));
        put32(pk + 12, pack_col(b));
        put32(pk + 16, pack_xy(sx[1], sy[1]));
        put32(pk + 20, pack_col(d));
        put32(pk + 24, pack_xy(sx[2], sy[2]));
        if (ot_ok(c, ot_entry)) psx_mod_write_word(ot_entry, (prev & 0xFF000000u) | (addr & 0x00FFFFFFu));
        c->out_used += PACKET_BYTES;
        s_stat_tris++; s_vb_tris++;
        return;
    }
    for (int pass = under ? 0 : 1; pass < 2; pass++) {
        const float *px = pass ? sx : ux, *py = pass ? sy : uy;
        const float qs = pass ? 1.0f : 0.997f;   /* underlay: a hair farther */
        pk = c->out + c->out_used;
        addr = c->out_addr + c->out_used;
        prev = rd32(ot_entry);
        put32(pk + 0,  (prev & 0x00FFFFFFu) | (9u << 24));
        put32(pk + 4,  (cmd << 24) | pack_col(a));
        put32(pk + 8,  pack_xy(px[0], py[0]));
        put32(pk + 12, ((uint32_t)clut << 16) | pack_uv(a));
        put32(pk + 16, pack_col(b) | ((c->bank & 0xFFu) << 24));
        put32(pk + 20, pack_xy(px[1], py[1]));
        put32(pk + 24, ((uint32_t)tpage << 16) | pack_uv(b));
        put32(pk + 28, pack_col(d) | ((c->bank >> 8) << 24));
        put32(pk + 32, pack_xy(px[2], py[2]));
        put32(pk + 36, pack_uv(d));
        /* HTP2: native-depth triangle (q = near/z, absolute); HTP3 also
         * starts the view's depth buffer. */
        put32(pk + 40, s_dbg_nosuffix ? 0u : (s_no_depth || s_force_glow) ? 0x48545031u : 0x48545032u);
        for (int i = 0; i < 3; i++) {
            float qa = q[i] * NEAR_Z * qs * s_qmul * slot_nudge;
            if (qa > 1.0f) qa = 1.0f;
            if (qa < 1e-6f) qa = 1e-6f;
            putf(pk + 44 + i * 4, qa);
        }
        for (int i = 0; i < 3; i++) {
            putf(pk + 56 + i * 8, px[i]);
            putf(pk + 60 + i * 8, py[i]);
        }
        /* the exact triangle is linked last, so the GPU draws it first */
        if (ot_ok(c, ot_entry)) psx_mod_write_word(ot_entry, (prev & 0xFF000000u) | (addr & 0x00FFFFFFu));
        c->out_used += PACKET_BYTES;
        s_stat_tris++; s_vb_tris++;
    }
}

static inline void lerp_vert(NVert *o, const NVert *a, const NVert *b, float t) {
    o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = a->z + (b->z - a->z) * t;
    o->r = a->r + (b->r - a->r) * t; o->g = a->g + (b->g - a->g) * t; o->b = a->b + (b->b - a->b) * t;
    o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
}

/*
 * Depth-sort one polygon (3 or 4 cyclic camera-space vertices) into the OT.
 * The OT sorts whole polygons by their average depth, like the PS1 painter's
 * algorithm; a large polygon reaching from near the camera far away (a wall
 * the kart backs into) then sorts behind small objects that are really behind
 * it. The game avoids that by splitting near polygons into pieces with their
 * own depth; do the same: split while the depth range is large and the
 * piece is still big on screen (small far pieces gain nothing; a big floor
 * at a distance used to explode into ~1000 triangles). Blended faces (the
 * kitchen floor's reflection layer) only when they reach behind the near
 * plane: each blended batch costs a framebuffer copy at high resolution,
 * and they are depth tested against the native scenery anyway.
 */
static void emit_poly(Ctx *c, const NVert *poly, int n, uint32_t cmd, uint16_t clut,
                      uint16_t tpage, uint32_t zt, int depth) {
    float zmin = 1e30f, zmax = -1e30f, zsum = 0.0f;
    for (int k = 0; k < n; k++) {
        if (poly[k].z < zmin) zmin = poly[k].z;
        if (poly[k].z > zmax) zmax = poly[k].z;
        zsum += poly[k].z > s_near ? poly[k].z : s_near;
    }
    if (zmax < s_near) return;
    float znear = zmin > s_near ? zmin : s_near;
    /* screen extent (width + height, canonical pixels); unbounded when the
     * polygon reaches behind the near plane */
    float extent = 1e9f;
    if (zmin > s_near) {
        float sxmin = 1e9f, sxmax = -1e9f, symin = 1e9f, symax = -1e9f;
        for (int k = 0; k < n; k++) {
            float iz = 1.0f / poly[k].z;
            float px = c->h * poly[k].x * iz, py = c->h * poly[k].y * iz;
            if (px < sxmin) sxmin = px; if (px > sxmax) sxmax = px;
            if (py < symin) symin = py; if (py > symax) symax = py;
        }
        extent = (sxmax - sxmin) + (symax - symin);
    }
    /* Gouraud quads: the PS1 game subdivides near quads at edge midpoints,
     * which shades them bilinearly; one quad drawn as two triangles shows a
     * light or dark wedge along its diagonal instead. Split quads whose
     * vertex colours differ while they are big on screen. */
    int shade_split = 0;
    if (n == 4 && depth < 4 && zmin > s_near) {
        float cmin[3] = { 1e9f, 1e9f, 1e9f }, cmax[3] = { -1e9f, -1e9f, -1e9f };
        for (int k = 0; k < 4; k++) {
            const float cv[3] = { poly[k].r, poly[k].g, poly[k].b };
            for (int j = 0; j < 3; j++) {
                if (cv[j] < cmin[j]) cmin[j] = cv[j];
                if (cv[j] > cmax[j]) cmax[j] = cv[j];
            }
        }
        float crange = 0.0f;
        for (int j = 0; j < 3; j++) if (cmax[j] - cmin[j] > crange) crange = cmax[j] - cmin[j];
        shade_split = crange >= 12.0f && extent > 48.0f;
    }
    if (s_split_rule & 1u) shade_split = 0;
    if ((s_split_rule & 2u) && extent >= 1e9f) extent = 0.0f;
    if ((s_split_rule & 4u) && extent < 1e9f) extent = 0.0f;
    /* (opaque faces drawn early need no depth order at all) */
    int depth_split = zmax - zmin > 64.0f && zmax > znear * 1.3f && extent > 64.0f &&
                      (cmd != 0x36u || extent >= 1e9f) &&
                      !(s_early_opaque && cmd == 0x34u && !s_force_glow && !s_no_depth);
    if (!s_no_split && shade_split && !(depth_split && depth < s_split_max)) {
        /* Gouraud wedge fix: four triangles around the centre (average of
         * the corners = the bilinear centre). Recursive midpoint splitting
         * looked the same but cost hundreds of pieces per big floor tile
         * (kitchen, 2P slowdown). */
        NVert ctr = poly[0], t[3];
        lerp_vert(&ctr, &poly[0], &poly[2], 0.5f);
        NVert m13;
        lerp_vert(&m13, &poly[1], &poly[3], 0.5f);
        lerp_vert(&ctr, &ctr, &m13, 0.5f);
        /* all four in the quad's own OT slot, so they stay one run of
         * packets: pieces in neighbouring slots interleave with the floor's
         * blended reflection layer and every opaque/blended switch costs the
         * renderer a framebuffer copy (kitchen: GPU 79% -> see notes) */
        int otz = s_zsf3 > 0.0f ? (int)(zsum * s_zsf4) : (int)(zsum * 0.0625f);
        if (otz < 1) otz = 1;
        if ((uint32_t)otz >= c->zfar) otz = (int)c->zfar - 1;
        uint32_t fan_ot = c->ot + rd16(zt + (uint32_t)otz * (s_ztab_half ? 2u : 4u));
        if (fan_ot == c->marker_entry && fan_ot >= c->ot + 4u) fan_ot -= 4u;
        for (int k = 0; k < 4; k++) {
            t[0] = poly[k]; t[1] = poly[(k + 1) & 3]; t[2] = ctr;
            s_fan_ot = fan_ot;
            emit_poly(c, t, 3, cmd, clut, tpage, zt, depth + 1);
            s_fan_ot = 0;
        }
        return;
    }
    if (!s_no_split && depth < s_split_max && depth_split) {
        NVert m[5];
        if (n == 4) {
            lerp_vert(&m[0], &poly[0], &poly[1], 0.5f);
            lerp_vert(&m[1], &poly[1], &poly[2], 0.5f);
            lerp_vert(&m[2], &poly[2], &poly[3], 0.5f);
            lerp_vert(&m[3], &poly[3], &poly[0], 0.5f);
            lerp_vert(&m[4], &m[0], &m[2], 0.5f);
            NVert q[4];
            q[0] = poly[0]; q[1] = m[0]; q[2] = m[4]; q[3] = m[3];
            emit_poly(c, q, 4, cmd, clut, tpage, zt, depth + 1);
            q[0] = m[0]; q[1] = poly[1]; q[2] = m[1]; q[3] = m[4];
            emit_poly(c, q, 4, cmd, clut, tpage, zt, depth + 1);
            q[0] = m[4]; q[1] = m[1]; q[2] = poly[2]; q[3] = m[2];
            emit_poly(c, q, 4, cmd, clut, tpage, zt, depth + 1);
            q[0] = m[3]; q[1] = m[4]; q[2] = m[2]; q[3] = poly[3];
            emit_poly(c, q, 4, cmd, clut, tpage, zt, depth + 1);
        } else {
            lerp_vert(&m[0], &poly[0], &poly[1], 0.5f);
            lerp_vert(&m[1], &poly[1], &poly[2], 0.5f);
            lerp_vert(&m[2], &poly[2], &poly[0], 0.5f);
            NVert t[3];
            t[0] = poly[0]; t[1] = m[0]; t[2] = m[2]; emit_poly(c, t, 3, cmd, clut, tpage, zt, depth + 1);
            t[0] = m[0]; t[1] = poly[1]; t[2] = m[1]; emit_poly(c, t, 3, cmd, clut, tpage, zt, depth + 1);
            t[0] = m[2]; t[1] = m[1]; t[2] = poly[2]; emit_poly(c, t, 3, cmd, clut, tpage, zt, depth + 1);
            t[0] = m[0]; t[1] = m[1]; t[2] = m[2]; emit_poly(c, t, 3, cmd, clut, tpage, zt, depth + 1);
        }
        return;
    }
    s_poly_depth = depth;
    /* depth slot: the game's AVSZ (average z / 4) through its table */
    /* the game's AVSZ: scenery average/4; models use the GTE's ZSF3/ZSF4 */
    int otz = s_zsf3 > 0.0f ? (int)(zsum * (n == 4 ? s_zsf4 : s_zsf3)) : (int)(zsum / (float)n * 0.25f);
    if (otz < 1) otz = 1;
    if ((uint32_t)otz >= c->zfar) {
        if (s_far_reject) return;   /* debug +43: the game drops polygons past the far limit */
        otz = (int)c->zfar - 1;
    }
    /* scenery indexes the table by even halfwords (otz * 4 bytes); the
     * model renderers by halfword (otz * 2 bytes) */
    uint32_t ot_entry = c->ot + rd16(zt + (uint32_t)otz * (s_ztab_half ? 2u : 4u));
    /* the depth-clear marker must stay the first packet drawn of the view */
    if (ot_entry == c->marker_entry && ot_entry >= c->ot + 4u) ot_entry -= 4u;
    if (s_fan_ot) ot_entry = s_fan_ot;   /* a shading fan piece: its quad's slot */

    /* A quad gains at most one vertex per clip plane: 4 + 5 = 9. */
    NVert b1[16], b2[16];
    memcpy(b1, poly, sizeof(NVert) * (size_t)n);
    int need = 0;
    for (int k = 0; k < n; k++) {
        float iz = poly[k].z > 1.0f ? 1.0f / poly[k].z : 1.0f;
        float px = c->ofx + c->h * poly[k].x * iz, py = c->ofy + c->h * poly[k].y * iz;
        if (poly[k].z < s_near || px < GUARD_X0 || px > GUARD_X1 || py < GUARD_Y0 || py > GUARD_Y1) need = 1;
    }
    if (need) {
        n = clip_poly(b1, n, b2, 0); if (n < 3) return;
        n = clip_poly(b2, n, b1, 1); if (n < 3) return;
        n = clip_poly(b1, n, b2, 2); if (n < 3) return;
        n = clip_poly(b2, n, b1, 3); if (n < 3) return;
        n = clip_poly(b1, n, b2, 4); if (n < 3) return;
        memcpy(b1, b2, sizeof(NVert) * (size_t)n);
    }
    if (n == 4 && !need) {
        /* Split a quad along the diagonal the PS1 uses (its vertex order is
         * p1 p2 p4 p3, triangles p1 p2 p4 and p2 p4 p3): with differing
         * vertex colours the other diagonal shades a visibly different
         * crease into walls. */
        emit_tri(c, &b1[0], &b1[1], &b1[3], cmd, clut, tpage, ot_entry, 0);
        emit_tri(c, &b1[1], &b1[2], &b1[3], cmd, clut, tpage, ot_entry, 0);
        return;
    }
    for (int k = 1; k + 1 < n; k++)
        emit_tri(c, &b1[0], &b1[k], &b1[k + 1], cmd, clut, tpage, ot_entry, 0);
}

/*
 * The view's depth buffer must be cleared before the first native triangle
 * the GPU draws, and the GPU draws the OT from its farthest slot (highest
 * address, ClearOTagR/DrawOTag) to the nearest, each slot's last-linked
 * packet first. So link a zero-area HTP3 marker last into the farthest slot
 * any native triangle can use.
 */
static void find_marker_slot(Ctx *c) {
    uint32_t lim, maxbias = 0;
    for (int i = 0; i < 16; i++) if (c->zbias[i] > maxbias && c->zbias[i] < 0x1000u) maxbias = c->zbias[i];
    lim = c->zfar + 80u + maxbias;   /* every index emit_poly can use */
    if (lim > 0x4000u) lim = 0x4000u;
    if (s_far_cache_ztab != c->ztab || s_far_cache_lim != lim) {
        s_far_cache_ztab = c->ztab; s_far_cache_lim = lim; s_far_cache_off = 0;
        for (uint32_t i = 0; i < lim; i++) {
            uint32_t o = rd16(c->ztab + i * 4u);
            if (o > s_far_cache_off) s_far_cache_off = o;
        }
    }
    c->marker_entry = c->ot + s_far_cache_off;
}

static void emit_view_start_marker(Ctx *c) {
    if (s_no_depth || c->out_used + PACKET_BYTES > c->out_cap) return;
    uint32_t ot_entry = c->marker_entry;
    uint8_t *pk = c->out + c->out_used;
    uint32_t addr = c->out_addr + c->out_used;
    uint32_t prev = rd32(ot_entry);
    uint32_t xy = pack_xy(c->ofx, c->ofy);
    put32(pk + 0,  (prev & 0x00FFFFFFu) | (9u << 24));
    put32(pk + 4,  0x34u << 24);
    put32(pk + 8,  xy);
    put32(pk + 12, (uint32_t)s_tex_clut[0] << 16);
    put32(pk + 16, 0);
    put32(pk + 20, xy);
    put32(pk + 24, (uint32_t)s_tex_tpage[0] << 16);
    put32(pk + 28, 0);
    put32(pk + 32, xy);
    put32(pk + 36, 0);
    put32(pk + 40, 0x48545033u);   /* HTP3: view start, clear depth */
    for (int i = 0; i < 3; i++) putf(pk + 44 + i * 4, 1.0f);
    for (int i = 0; i < 3; i++) { putf(pk + 56 + i * 8, c->ofx); putf(pk + 60 + i * 8, c->ofy); }
    if (ot_ok(c, ot_entry)) psx_mod_write_word(ot_entry, (prev & 0xFF000000u) | (addr & 0x00FFFFFFu));
    c->out_used += PACKET_BYTES;
}

/* Near reject, back-face cull, texture state and depth-sorted emission of
 * one face (3 or 4 cyclic camera-space vertices). */
static void emit_face(Ctx *c, NVert *poly, int n, uint8_t flags, uint8_t texmap,
                      const uint8_t (*uv)[2], uint32_t zt) {
    float zsum = 0.0f;
    int any = 0;
    for (int k = 0; k < n; k++) { zsum += poly[k].z; if (poly[k].z >= s_near) any = 1; }
    if (!any) return;
    (void)zsum;
    /* Back-face cull (single-sided groups), view-space normal test. Quads
     * are often not planar (a vent's bent wall): one half may face the
     * camera while the other does not, so they are culled per triangle in
     * emit_tri instead; a whole-face test on three corners left holes the
     * game does not have. */
    s_cull = !(flags & 2u) && !s_no_cull;
    if (s_cull && n == 3 && back_facing(&poly[0], &poly[1], &poly[2])) return;
    /* The game draws the other areas' sections only through their screen
     * rect (the doorway they are seen through): a face wholly to one side of
     * it is rejected (object renderers, clip rect at 0x1F800060). Faces
     * reaching behind the near plane are kept. */
    if (c->rect_on) {
        float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        int behind = 0;
        for (int k = 0; k < n && !behind; k++) {
            if (poly[k].z < s_near) { behind = 1; break; }
            float iz = 1.0f / poly[k].z;
            float px = c->ofx + c->h * poly[k].x * iz, py = c->ofy + c->h * poly[k].y * iz;
            if (px < minx) minx = px; if (px > maxx) maxx = px;
            if (py < miny) miny = py; if (py > maxy) maxy = py;
        }
        if (!behind && (maxx < c->rx0 || minx >= c->rx1 || maxy < c->ry0 || miny >= c->ry1)) return;
    }
    /* blend bits go straight into the tpage; the packet is semi-
     * transparent unless both bits are set (0x60 = opaque). */
    if (s_false_colour) flags = (uint8_t)((flags & 0x03u) | 0x70u);   /* opaque, untextured */
    uint32_t abr = (uint32_t)flags & 0x60u;
    int semi = s_semi_override >= 0 ? s_semi_override : ((abr + 0x20u) & 0x60u) != 0u;
    uint16_t tpage = (uint16_t)(s_tex_tpage[texmap & 31] | abr);
    uint16_t clut = s_tex_clut[texmap & 31];
    int textured = !(flags & 0x10u);
    if (textured) {
        int u0 = 255, v0 = 255;
        for (int k = 0; k < n; k++) {
            if (uv[k][0] < u0) u0 = uv[k][0];
            if (uv[k][1] < v0) v0 = uv[k][1];
        }
        int pal;
        int xh = u0 >> 2;
        if ((texmap & 31) < 4 && v0 < 32 && xh < 8)       pal = (v0 / 8) * 4 + xh / 2;
        else if ((texmap & 31) < 4 && v0 < 64 && xh < 16) pal = (v0 / 16) * 4 + xh / 4;
        else if ((texmap & 31) < 4 && v0 < 128 && xh < 32) pal = (v0 / 32) * 4 + xh / 8;
        else pal = (v0 / 64) * 4 + (u0 / 64);
        if (s_pal_scenery >= 0) {
            /* scenery: the game adds it only when the texture entry's word
             * +8 is zero (0x80017100) */
            if (rd16(TEX_TABLE + (uint32_t)(texmap & 31) * 16u + 8u) == 0u)
                clut = (uint16_t)(s_tex_clut[texmap & 31] + s_pal_scenery);
        } else if (s_pal_override >= 0) {
            /* models: the renderers add the palette index to the CLUT
             * whatever the texture depth (8-bit title logos too) */
            clut = (uint16_t)(clut + s_pal_override);
        } else if (((s_tex_tpage[texmap & 31] >> 7) & 3) == 0) {
            clut = (uint16_t)(clut + pal);
        }
    }
    uint32_t cmd = textured ? (semi ? 0x36u : 0x34u) : (semi ? 0x32u : 0x30u);
    c->bank = 0;
    if (s_force_glow) {
        c->bank = (uint32_t)s_force_glow;
        tpage = (uint16_t)(0x100u | abr);   /* 15-bit direct texels */
        clut = 0;
    }
    if (!textured && s_flat_bank_ok) {
        /* Untextured (gouraud) face: draw it as a textured triangle over a
         * host bank of 0x4210 texels (brightness 128 = the vertex colour
         * unchanged; STP set for blended faces), so it gets the exact
         * positions and the native depth too. */
        c->bank = semi ? FLAT_BANK_SEMI : FLAT_BANK_OPAQUE;
        tpage = (uint16_t)(0x100u | abr);   /* 15-bit direct texels */
        clut = 0;
        cmd = semi ? 0x36u : 0x34u;
        /* The renderer modulates as texel/31 * colour * 2: texel 16 gives
         * 32/31 of the colour, a visibly lighter face next to the game's
         * own shading. Scale the colours back by 31/32. */
        for (int k = 0; k < n; k++) {
            poly[k].r *= 0.96875f; poly[k].g *= 0.96875f; poly[k].b *= 0.96875f;
            poly[k].u = poly[k].v = 0.0f;   /* inside the 16x16 bank */
        }
    }
    emit_poly(c, poly, n, cmd, clut, tpage, zt, 0);
}

/* Debug: encode an index in a flat colour, 3 bits per channel (16 + 32 * k),
 * readable back from a screenshot. */
static void false_colour(NVert *v, int n) {
    v->u = v->v = 0.0f;   /* drawn from the 16x16 flat bank */
    v->r = (float)(16 + 32 * (n & 7));
    v->g = (float)(16 + 32 * ((n >> 3) & 7));
    v->b = (float)(16 + 32 * ((n >> 6) & 7));
}

/* Sprite group: centres follow the item rotation (camera * Rx*Ry*Rz), each
 * quad is spread by a billboard basis -- type 2 screen-aligned
 * (diag(6553/4096, 1, 0), the widescreen X scale), type 3 turned to face the
 * camera about Y (camera * Ry(-yaw), yaw at 0x800A9F62) -- as the game's
 * handlers 0x80043C10 / 0x800439F0 set up for 0x8004938C / 0x80049EBC. */
/* ---- lamp halo occlusion ----------------------------------------------------
 * The halo is drawn without the depth test (the things around a lamp catch
 * its glare, as in the game), so it also showed through the walls between
 * the camera and the lamp. Like a lens flare, its brightness follows how much
 * of the lamp itself is in sight: segments from the camera to a few points
 * around the lamp's centre are tested against the level's scenery, leaving
 * out what is right at the lamp (its own fixture and shade). View space:
 * the camera is the origin. */
#define HALO_SKIP    200.0f   /* scenery this close to the lamp does not hide it */
#define HALO_SPREAD   48.0f   /* sample points around the centre */

static int seg_hits_item(const Ctx *c, const NItem *it, const float *p, float t1) {
    const NObj *o = &s_objs[it->obj];
    /* the section's bounding sphere against the segment */
    float dx = it->cx - c->cam[0], dy = it->cy - c->cam[1], dz = it->cz - c->cam[2];
    float sc[3];
    for (int i = 0; i < 3; i++) sc[i] = c->m[i * 3] * dx + c->m[i * 3 + 1] * dy + c->m[i * 3 + 2] * dz;
    float r = it->radius * 1.7f + 64.0f;
    float pp = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
    float tt = (sc[0] * p[0] + sc[1] * p[1] + sc[2] * p[2]) / pp;
    if (tt < 0.0f) tt = 0.0f;
    if (tt > t1) tt = t1;
    float ex = sc[0] - p[0] * tt, ey = sc[1] - p[1] * tt, ez = sc[2] - p[2] * tt;
    if (ex * ex + ey * ey + ez * ez > r * r) return 0;
    /* object -> camera transform, as emit_item */
    float pos[3], mrot[9], A[9], t[3];
    pos[0] = (float)(int32_t)rd32(it->item_addr);
    pos[1] = (float)(int32_t)rd32(it->item_addr + 4u);
    pos[2] = (float)(int32_t)rd32(it->item_addr + 8u);
    if (rd32(it->item_addr + 12u) != 0u || rd16(it->item_addr + 16u) != 0u) {
        for (int k = 0; k < 9; k++) mrot[k] = (int16_t)rd16(it->rec_addr + (uint32_t)k * 2u) / 4096.0f;
    } else {
        memcpy(mrot, it->m, sizeof mrot);
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            A[i * 3 + j] = c->m[i * 3 + 0] * mrot[0 * 3 + j] + c->m[i * 3 + 1] * mrot[1 * 3 + j] +
                           c->m[i * 3 + 2] * mrot[2 * 3 + j];
        t[i] = c->m[i * 3 + 0] * (pos[0] - c->cam[0]) + c->m[i * 3 + 1] * (pos[1] - c->cam[1]) +
               c->m[i * 3 + 2] * (pos[2] - c->cam[2]);
    }
    static float vx[MAX_VERTS], vy[MAX_VERTS], vz[MAX_VERTS];
    for (int k = 0; k < o->nverts; k++) {
        float x = o->v[k][0], y = o->v[k][1], z = o->v[k][2];
        vx[k] = A[0] * x + A[1] * y + A[2] * z + t[0];
        vy[k] = A[3] * x + A[4] * y + A[5] * z + t[1];
        vz[k] = A[6] * x + A[7] * y + A[8] * z + t[2];
    }
    for (int fi = 0; fi < o->nfaces; fi++) {
        const NFace *f = &s_faces[o->first_face + fi];
        if ((((uint32_t)f->flags & 0x60u) + 0x20u) & 0x60u) continue;   /* blended (glass): no */
        for (int k = 0; k + 2 < f->n; k++) {
            int i0 = f->idx[0], i1 = f->idx[k + 1], i2 = f->idx[k + 2];
            float ax = vx[i0], ay = vy[i0], az = vz[i0];
            float e1x = vx[i1] - ax, e1y = vy[i1] - ay, e1z = vz[i1] - az;
            float e2x = vx[i2] - ax, e2y = vy[i2] - ay, e2z = vz[i2] - az;
            if (!(f->flags & 2u)) {
                /* single-sided, seen from behind: not drawn, hides nothing */
                float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
                if (nx * ax + ny * ay + nz * az >= 0.0f) continue;
            }
            /* segment (origin .. p) against the triangle */
            float hx = p[1] * e2z - p[2] * e2y, hy = p[2] * e2x - p[0] * e2z, hz = p[0] * e2y - p[1] * e2x;
            float det = e1x * hx + e1y * hy + e1z * hz;
            if (det > -1e-4f && det < 1e-4f) continue;
            float inv = 1.0f / det;
            float u = -(ax * hx + ay * hy + az * hz) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            float qx = -(ay * e1z - az * e1y), qy = -(az * e1x - ax * e1z), qz = -(ax * e1y - ay * e1x);
            float v = (p[0] * qx + p[1] * qy + p[2] * qz) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            float th = (e2x * qx + e2y * qy + e2z * qz) * inv;
            if (th > 0.02f && th < t1) return 1;
        }
    }
    return 0;
}

/* 0 = the lamp is hidden, 1 = in full sight */
static float halo_visibility(const Ctx *c, const float *ce) {
    static const float off[5][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    int seen = 0;
    for (int s = 0; s < 5; s++) {
        float p[3] = { ce[0] + off[s][0] * HALO_SPREAD, ce[1] + off[s][1] * HALO_SPREAD, ce[2] };
        float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        float t1 = len > 1.0f ? 1.0f - HALO_SKIP / len : 0.0f;
        int hit = 0;
        if (t1 > 0.05f)
            for (int k = 0; k < s_nitems && !hit; k++)
                if (!s_items[k].kind && s_items[k].obj >= 0) hit = seg_hits_item(c, &s_items[k], p, t1);
        seen += !hit;
    }
    return (float)seen / 5.0f;
}

static void emit_sprites(Ctx *c, const NItem *it) {
    float pos[3];
    pos[0] = (float)(int32_t)rd32(it->item_addr);
    pos[1] = (float)(int32_t)rd32(it->item_addr + 4u);
    pos[2] = (float)(int32_t)rd32(it->item_addr + 8u);
    int rx = (int16_t)rd16(it->item_addr + 12u), ry = (int16_t)rd16(it->item_addr + 14u),
        rz = (int16_t)rd16(it->item_addr + 16u);
    float R[9], S[9], B[9], t[3];
    euler_matrix(rx, ry, rz, 1.0f, 1.0f, 1.0f, R);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            S[i * 3 + j] = c->m[i * 3 + 0] * R[0 * 3 + j] + c->m[i * 3 + 1] * R[1 * 3 + j] +
                           c->m[i * 3 + 2] * R[2 * 3 + j];
        t[i] = c->m[i * 3 + 0] * (pos[0] - c->cam[0]) + c->m[i * 3 + 1] * (pos[1] - c->cam[1]) +
               c->m[i * 3 + 2] * (pos[2] - c->cam[2]);
    }
    memset(B, 0, sizeof B);
    if (it->kind == 2) {
        B[0] = 6553.0f / 4096.0f; B[4] = 1.0f;
    } else {
        float Y[9];
        int yaw = (int)rd16(0x800A9F62u);
        euler_matrix(0, -yaw, 0, 1.0f, 1.0f, 1.0f, Y);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 2; j++)
                B[i * 3 + j] = c->m[i * 3 + 0] * Y[0 * 3 + j] + c->m[i * 3 + 1] * Y[1 * 3 + j] +
                               c->m[i * 3 + 2] * Y[2 * 3 + j];
    }
    uint32_t zt = c->ztab + (c->zbias[it->zbias] + 0u) * 4u;
    for (int k = 0; k < it->spr_count; k++) {
        const NSprite *sp = &s_sprites[it->spr_first + k];
        float ce[3];
        for (int i = 0; i < 3; i++)
            ce[i] = S[i * 3 + 0] * sp->pos[0] + S[i * 3 + 1] * sp->pos[1] + S[i * 3 + 2] * sp->pos[2] + t[i];
        for (int q = 0; q < sp->q_n; q++) {
            const NSpriteQuad *sq = &s_squads[sp->q_first + q];
            NVert poly[4];
            for (int j = 0; j < 4; j++) {
                float x = sq->xy[j][0], y = sq->xy[j][1];
                poly[j].x = ce[0] + B[0] * x + B[1] * y;
                poly[j].y = ce[1] + B[3] * x + B[4] * y;
                poly[j].z = ce[2] + B[6] * x + B[7] * y;
                poly[j].r = sq->col[j][0]; poly[j].g = sq->col[j][1]; poly[j].b = sq->col[j][2];
                poly[j].u = sq->uv[j][0]; poly[j].v = sq->uv[j][1];
                if (s_false_colour) false_colour(&poly[j], it->sec);
            }
            uint8_t fl = (uint8_t)((sq->tm & 0x60u) | 0x02u);
            /* The lamps' additive halos: texmap 1, texels (0..31, 192..223),
             * and texmap 2, texels (0..63, 128..191). */
            int glow = 0, gu1 = 0, gv0 = 0, gv1 = 0;
            if (s_glow_bank_ok && !s_false_colour) {
                if ((sq->tm & 0x7Fu) == 0x21u) { glow = (int)GLOW_BANK; gu1 = 31; gv0 = 192; gv1 = 223; }
                else if ((sq->tm & 0x7Fu) == 0x22u) { glow = (int)GLOW_BANK2; gu1 = 63; gv0 = 128; gv1 = 191; }
            }
            for (int j = 0; j < 4 && glow; j++)
                if ((sq->uv[j][0] != 0 && sq->uv[j][0] != gu1) || (sq->uv[j][1] != gv0 && sq->uv[j][1] != gv1)) glow = 0;
            if (glow) {
                /* dimmed by what stands between the camera and the lamp */
                float seen = halo_visibility(c, ce);
                if (seen <= 0.0f) continue;
                for (int j = 0; j < 4; j++) {
                    poly[j].u = sq->uv[j][0] ? (float)(GLOW_SIZE - 1u) : 0.0f;
                    poly[j].v = sq->uv[j][1] == gv1 ? (float)(GLOW_SIZE - 1u) : 0.0f;
                    poly[j].r *= seen; poly[j].g *= seen; poly[j].b *= seen;
                }
                s_force_glow = glow;
                emit_face(c, poly, 4, fl, (uint8_t)(sq->tm & 0x1Fu), sq->uv, zt);
                s_force_glow = 0;
                continue;
            }
            emit_face(c, poly, 4, fl, (uint8_t)(sq->tm & 0x1Fu), sq->uv, zt);
        }
    }
}

/* One natively drawn section (mesh or sprite group). */
static void emit_item(Ctx *c, const NItem *it) {
    const NObj *o = it->kind ? NULL : &s_objs[it->obj];
    /* bounding sphere in camera space */
    float dx = it->cx - c->cam[0], dy = it->cy - c->cam[1], dz = it->cz - c->cam[2];
    float sz = c->m[6] * dx + c->m[7] * dy + c->m[8] * dz;
    float r = it->radius * 1.7f + 64.0f;   /* row 0 may be scaled by ~1.6 */
    if (sz + r < s_near) return;
    if (s_draw_distance > 0.0f && sz - r > s_draw_distance) return;
    float sx = c->m[0] * dx + c->m[1] * dy + c->m[2] * dz;
    float sy = c->m[3] * dx + c->m[4] * dy + c->m[5] * dz;
    if (sz + r > 0.0f) {
        if (c->h * (sx - r) > (GUARD_X1 - c->ofx) * (sz + r)) return;
        if (c->h * (sx + r) < (GUARD_X0 - c->ofx) * (sz + r)) return;
        if (c->h * (sy - r) > (GUARD_Y1 - c->ofy) * (sz + r)) return;
        if (c->h * (sy + r) < (GUARD_Y0 - c->ofy) * (sz + r)) return;
    }
    if (it->kind) { emit_sprites(c, it); return; }
    /* combined object -> camera transform */
    /* Doors, lifts and other animated scenery move their item position
     * or record matrix at run time: refresh both every frame. */
    float pos[3], mrot[9];
    pos[0] = (float)(int32_t)rd32(it->item_addr);
    pos[1] = (float)(int32_t)rd32(it->item_addr + 4u);
    pos[2] = (float)(int32_t)rd32(it->item_addr + 8u);
    if (rd32(it->item_addr + 12u) != 0u || rd16(it->item_addr + 16u) != 0u) {
        for (int k = 0; k < 9; k++) mrot[k] = (int16_t)rd16(it->rec_addr + (uint32_t)k * 2u) / 4096.0f;
    } else {
        memcpy(mrot, it->m, sizeof mrot);
    }
    float A[9], t[3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            A[i * 3 + j] = c->m[i * 3 + 0] * mrot[0 * 3 + j] +
                           c->m[i * 3 + 1] * mrot[1 * 3 + j] +
                           c->m[i * 3 + 2] * mrot[2 * 3 + j];
        t[i] = c->m[i * 3 + 0] * (pos[0] - c->cam[0]) +
               c->m[i * 3 + 1] * (pos[1] - c->cam[1]) +
               c->m[i * 3 + 2] * (pos[2] - c->cam[2]);
    }
    static float vx[MAX_VERTS], vy[MAX_VERTS], vz[MAX_VERTS];
    for (int k = 0; k < o->nverts; k++) {
        float x = o->v[k][0], y = o->v[k][1], z = o->v[k][2];
        vx[k] = A[0] * x + A[1] * y + A[2] * z + t[0];
        vy[k] = A[3] * x + A[4] * y + A[5] * z + t[1];
        vz[k] = A[6] * x + A[7] * y + A[8] * z + t[2];
    }
    uint32_t zt = c->ztab + (c->zbias[it->zbias] + 0u) * 4u;
    for (int fi = 0; fi < o->nfaces; fi++) {
        const NFace *f = &s_faces[o->first_face + fi];
        NVert poly[4];
        for (int k = 0; k < f->n; k++) {
            int id = f->idx[k];
            NVert *pv = &poly[k];
            pv->x = vx[id]; pv->y = vy[id]; pv->z = vz[id];
            uint16_t col = o->col[id];
            pv->r = (float)(((col >> 10) & 31) << 3);
            pv->g = (float)(((col >> 5) & 31) << 3);
            pv->b = (float)((col & 31) << 3);
            pv->u = f->uv[k][0]; pv->v = f->uv[k][1];
            if (s_false_colour) false_colour(pv, s_false_colour == 2 ? fi : it->sec);
        }
        s_pal_scenery = (f->flags & 0x1Fu) == 0u ? f->pal : -1;   /* renderer type 0 (0x80016DFC) */
        emit_face(c, poly, f->n, f->flags, f->texmap, f->uv, zt);
        s_pal_scenery = -1;
    }
}

/* Host texture banks, defined once the renderer can take them (it may not
 * be up yet when the plugin is activated). */
static void ensure_banks(void) {
    if (s_flat_bank_ok || !psx_mod_texture_banks_supported()) return;
    {
        static uint16_t px[2][16 * 16];
        for (int i = 0; i < 16 * 16; i++) { px[0][i] = 0x4210u; px[1][i] = 0xC210u; }
        s_flat_bank_ok = psx_mod_define_texture_bank(FLAT_BANK_OPAQUE, 16u, 16u, px[0]) &&
                         psx_mod_define_texture_bank(FLAT_BANK_SEMI, 16u, 16u, px[1]);
        /* Halo: the original's grey ramp (mean 5-bit level by radius in its
         * 32 texels, 23 at the centre, 2 from radius 14 out to the corners),
         * smooth and falling to nothing at the inscribed circle; STP set
         * (blended). */
        static const float rr[] = { 0.0f, 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f, 11.5f, 12.5f, 13.5f, 15.5f };
        static const float lv[] = { 23, 23, 22, 21, 19, 17, 15.9f, 14.4f, 12.7f, 11.4f, 9.8f, 8.2f, 6.8f, 5.0f, 3.4f, 0 };
        /* The second halo (64 texels, texmap 2 at 0..63, 128..191): an even
         * ramp from 30 at the centre to 1 at radius 31, then 1 out to the
         * corners (the same faint square). */
        static const float rr2[] = { 0.0f, 0.5f, 4.5f, 8.5f, 12.5f, 16.5f, 20.5f, 24.5f, 28.5f, 30.5f, 32.0f };
        static const float lv2[] = { 30.2f, 30.2f, 26.7f, 22.9f, 19.5f, 15.6f, 11.7f, 7.6f, 3.5f, 1.4f, 0 };
        static uint16_t gp[2][GLOW_SIZE * GLOW_SIZE];
        for (int bk = 0; bk < 2; bk++) {
            const float *R = bk ? rr2 : rr, *L = bk ? lv2 : lv;
            int nr = bk ? (int)(sizeof rr2 / sizeof rr2[0]) : (int)(sizeof rr / sizeof rr[0]);
            float texels = bk ? 64.0f : 32.0f;
            for (uint32_t y = 0; y < GLOW_SIZE; y++)
                for (uint32_t x = 0; x < GLOW_SIZE; x++) {
                    float dx = ((float)x + 0.5f) / GLOW_SIZE * texels - texels * 0.5f;
                    float dy = ((float)y + 0.5f) / GLOW_SIZE * texels - texels * 0.5f;
                    float r = sqrtf(dx * dx + dy * dy), l = 0.0f;
                    for (int k = 0; k + 1 < nr; k++)
                        if (r >= R[k] && r < R[k + 1]) { l = L[k] + (L[k + 1] - L[k]) * (r - R[k]) / (R[k + 1] - R[k]); break; }
                    int v = (int)(l + 0.5f);
                    gp[bk][y * GLOW_SIZE + x] = v > 0 ? (uint16_t)(0x8000u | (uint32_t)v | ((uint32_t)v << 5) | ((uint32_t)v << 10)) : 0u;
                }
        }
        s_glow_bank_ok = psx_mod_define_texture_bank(GLOW_BANK, GLOW_SIZE, GLOW_SIZE, gp[0]) &&
                         psx_mod_define_texture_bank(GLOW_BANK2, GLOW_SIZE, GLOW_SIZE, gp[1]);
        fprintf(stdout, "tsr native scene: texture banks flat %d glow %d\n", s_flat_bank_ok, s_glow_bank_ok);
        fflush(stdout);
    }
}

/*
 * Game objects (karts, pick-ups) are still drawn by the game, sorted into the
 * OT by their average depth only; a big wall polygon sorts by its own
 * average, so objects just behind it could be drawn after it and show
 * through (the PS1 painter's limit; the boxes of the attic). Give the
 * framework each OT entry's depth so it tests the game's primitives against
 * the native scenery's depth buffer. The depth table is read as halfwords
 * (the object renderers index it at half steps, the scenery at even ones):
 * offset = table[u] for u = 2 * (z / 4 + 80 + bias), so an entry covers
 * depths 2 * (u - 160) for its u range; it is tested at the middle of the
 * range, 8 units farther (objects resting just behind a wall). Blended
 * polygons (shadows lying on the floor, sparks) are tested nearer by half
 * their screen size in depth.
 */
static float    s_otd_q[8192];
static uint32_t s_otd_ztab, s_otd_lim, s_otd_n;
static int      s_otd_off = -1;

static void set_ot_depth(Ctx *c) {
    int off = s_debug_mode_addr && rd8(s_debug_mode_addr + 4u) == 1u;
    uint32_t lim = s_far_cache_lim;
    static uint8_t s_otd_rel = 0xFF;
    uint8_t relb = s_debug_mode_addr ? (uint8_t)(rd8(s_debug_mode_addr + 5u) ^ (rd8(s_debug_mode_addr + 6u) * 31u)) : 0;
    if (off != s_otd_off || s_otd_ztab != c->ztab || s_otd_lim != lim || relb != s_otd_rel) {
        s_otd_rel = relb;
        s_otd_off = off; s_otd_ztab = c->ztab; s_otd_lim = lim; s_otd_n = 0;
        static float zmin[8192], zmax[8192];
        for (uint32_t e = 0; e < 8192u; e++) { zmin[e] = 1e30f; zmax[e] = -1e30f; }
        for (uint32_t u = 0; u < 2u * lim && !off; u++) {   /* halfword index */
            uint32_t e = rd16(c->ztab + u * 2u) / 4u;
            if (e >= 8192u) continue;
            float z = 2.0f * ((float)u - 160.0f);
            if (z < zmin[e]) zmin[e] = z;
            if (z > zmax[e]) zmax[e] = z;
            if (e + 1u > s_otd_n) s_otd_n = e + 1u;
        }
        float last = 1.0f;
        for (uint32_t e = 0; e < s_otd_n; e++) {
            if (zmin[e] < 1e29f) {
                float z = 0.5f * (zmin[e] + zmax[e]);   /* middle of the entry's range */
                last = z <= NEAR_Z ? 1.0f : NEAR_Z / z;
            }
            s_otd_q[e] = last;   /* unused entries: as the previous one */
        }
        psx_mod_set_ot_depth_table(s_otd_q, s_otd_n);
        uint8_t rel = s_debug_mode_addr ? rd8(s_debug_mode_addr + 5u) : 0;   /* debug: margin % */
        int8_t push = s_debug_mode_addr ? (int8_t)rd8(s_debug_mode_addr + 6u) : 0;   /* debug: farther by 8*n */
        psx_mod_set_ot_depth_params(NEAR_Z, c->h, rel == 255u ? -1.0f : rel / 100.0f, -8.0f - 8.0f * push);
    }
    psx_mod_add_ot_depth_base(c->ot);
}

/* Start a view's native drawing if nothing native has been drawn into this
 * OT yet this frame (the depth-clear marker and the OT depth table). The
 * scenery normally does it; in menus and other scenes without native
 * scenery the models or sprites are the first native packets. */
static void view_begin(Ctx *c) {
    for (int i = 0; i < s_nviews; i++) if (s_views[i] == c->ot) return;
    if (s_nviews >= 4) return;
    s_views[s_nviews++] = c->ot;
    emit_view_start_marker(c);
    set_ot_depth(c);
}

/* ---- karts, characters and other models ------------------------------------ */

/*
 * The game draws its models (karts, drivers, pick-ups) with four hand-written
 * renderers, all on the same packet-like format:
 *   0x8002207c  skinned (a matrix per vertex)      0x8002274c  rigid (GTE RT)
 *   0x80024568  skinned + depth-cue tint (DCPL)    0x80023f58  rigid + tint
 * a0 points at a list of polygons ending with a word whose bits 0x38000000
 * are clear. A quad is 48 bytes (word 0 & 0x38000000 == 0x38000000), a
 * triangle 36: per vertex 12 bytes
 *   +0  GPU command/colour word (vertex 0: 0x3C/0x34.. command, textured if
 *       bit 26, semi-transparent if bit 25); the low two bits of R, G and B
 *       hold the vertex's matrix index (0..63) in the skinned renderers
 *   +4  x, y int16    +8  z int16, u8 u, u8 v
 * The top byte of the last vertex's colour word is the texmap (0x1F) and
 * blend mode (0x60) into the 0x800DB070 table; palette = u0/64 + (v0/64)*4.
 * Skinned matrices: scratchpad 0x1F800020 + index*32 (GTE rotation, 5 words,
 * then translation, 3 words), already camera space. Depth slot: halfword
 * table index 2*otz (no item bias).
 * The hook draws the model natively and points a0 at an empty list, so the
 * game's renderer returns at once.
 */
#define MODEL_SKINNED      0x8002207Cu
#define MODEL_RIGID        0x8002274Cu
#define MODEL_RIGID_TINT   0x80023F58u
#define MODEL_SKINNED_TINT 0x80024568u
static uint32_t s_empty_model;      /* mod memory: zero words */
static uint32_t s_model_calls, s_model_tris;

static uint32_t s_last_prim_cursor;
static int frame_buffer_begin(void) {
    int buf = rd32(BUFFER_VAR) == BUFFER0 ? 0 : 1;
    uint32_t cur = rd32(0x800A9F4Cu);   /* the game's primitive cursor */
    if (buf != s_last_buf) {
        s_last_buf = buf;
        s_frame++;
        s_arena_used[buf] = 0;
        s_nviews = 0;
    } else if (cur < s_last_prim_cursor) {
        /* the same buffer built again from its start (the game threw the
         * frame away: end-of-race camera cuts) with a cleared OT: every view
         * needs its depth clear again */
        s_nviews = 0;
    }
    s_last_prim_cursor = cur;
    return buf;
}

static void model_draw_impl(struct CPUState *cpu, uint32_t address) {
    if (!s_enabled || !s_arena_host[0] || !s_empty_model) return;
    uint8_t mode = s_debug_mode_addr ? rd8(s_debug_mode_addr) : 0;
    if ((mode & 0x0Fu) == 1u) return;
    if (s_debug_mode_addr && rd8(s_debug_mode_addr + 7u) == 1u) return;   /* debug: models to the game */
    uint32_t list = cpu->gpr[4];
    if ((list & 0xFF800000u) != 0x80000000u) return;
    ensure_banks();
    int skinned = address == MODEL_SKINNED || address == MODEL_SKINNED_TINT;
    int tint = address == MODEL_SKINNED_TINT || address == MODEL_RIGID_TINT;
    int buf = frame_buffer_begin();

    static Ctx c;
    g_tsr_ctx = &c;
    memset(c.m, 0, sizeof c.m);
    c.h   = (float)(cpu->gte_ctrl[26] & 0xFFFFu);
    c.ofx = (float)(int32_t)cpu->gte_ctrl[24] / 65536.0f;
    c.ofy = (float)(int32_t)cpu->gte_ctrl[25] / 65536.0f;
    if (c.h <= 0.0f) return;
    c.ot = rd32(0x800A9B10u);
    c.ztab = rd32(ZTAB_PTR);
    c.zfar = rd32(ZFAR_VAR);
    if (c.zfar < 2u || c.zfar > 0x4000u) c.zfar = 0x4000u;
    for (int i = 0; i < 16; i++) c.zbias[i] = rd32(ZBIAS_TABLE + (uint32_t)i * 4u) + 80u;
    c.out = s_arena_host[buf] + s_arena_used[buf];
    c.out_addr = s_arena[buf] + s_arena_used[buf];
    c.out_used = 0;
    c.out_cap = ARENA_BYTES - s_arena_used[buf];
    c.rect_on = 0;
    c.bank = 0;
    find_marker_slot(&c);
    view_begin(&c);
    for (int t = 0; t < 32; t++) {
        s_tex_tpage[t] = rd16(TEX_TABLE + (uint32_t)t * 16u);
        s_tex_clut[t]  = rd16(TEX_TABLE + (uint32_t)t * 16u + 2u);
    }

    /* matrices: the GTE's current one (rigid) or the 64 scratchpad ones */
    float mats[64][12];
    int nm = skinned ? 64 : 1;
    for (int k = 0; k < nm; k++) {
        uint32_t w[8];
        if (skinned) for (int i = 0; i < 8; i++) w[i] = rd32(0x1F800020u + (uint32_t)k * 32u + (uint32_t)i * 4u);
        else for (int i = 0; i < 8; i++) w[i] = cpu->gte_ctrl[i];
        float *M = mats[k];
        M[0] = (int16_t)(w[0] & 0xFFFFu) / 4096.0f; M[1] = (int16_t)(w[0] >> 16) / 4096.0f;
        M[2] = (int16_t)(w[1] & 0xFFFFu) / 4096.0f; M[3] = (int16_t)(w[1] >> 16) / 4096.0f;
        M[4] = (int16_t)(w[2] & 0xFFFFu) / 4096.0f; M[5] = (int16_t)(w[2] >> 16) / 4096.0f;
        M[6] = (int16_t)(w[3] & 0xFFFFu) / 4096.0f; M[7] = (int16_t)(w[3] >> 16) / 4096.0f;
        M[8] = (int16_t)(w[4] & 0xFFFFu) / 4096.0f;
        M[9] = (float)(int32_t)w[5]; M[10] = (float)(int32_t)w[6]; M[11] = (float)(int32_t)w[7];
    }
    /* depth-cue tint (a1..a3, the DCPL far colour weights): approximate as
     * a colour scale */
    /* tint renderers: DCPL with IR1..3 = a1..a3 (4.12) and IR0 = 0, i.e.
     * each vertex colour channel scaled by a1/a2/a3 (fades, flashes) */
    float tr = 1.0f, tg = 1.0f, tb = 1.0f;
    if (tint) {
        tr = (int16_t)(cpu->gpr[5] & 0xFFFFu) / 4096.0f;
        tg = (int16_t)(cpu->gpr[6] & 0xFFFFu) / 4096.0f;
        tb = (int16_t)(cpu->gpr[7] & 0xFFFFu) / 4096.0f;
        if (tr < 0.0f) tr = 0.0f;
        if (tg < 0.0f) tg = 0.0f;
        if (tb < 0.0f) tb = 0.0f;
    }

    s_qmul = 1.0f;
    if (s_debug_mode_addr && rd8(s_debug_mode_addr + 22u)) s_qmul = rd8(s_debug_mode_addr + 22u) / 16.0f;
    /* Model camera space is in half the scenery's units: the kart draw
     * (0x80042864..) builds the translation as ((pos - camera) >> 2) * view
     * matrix >> 15, i.e. high-precision position / 32, where the scenery uses
     * position / 64. Scale model vertices into scenery units (projection is
     * unchanged) so native depth and near clip agree with the scenery; the
     * OT slot keeps the game's own AVSZ on the unscaled depth (ZSF3/ZSF4). */
    const float munit = 0.5f;
    s_zsf3 = (float)(int16_t)(cpu->gte_ctrl[29] & 0xFFFFu) / 4096.0f / munit;
    s_zsf4 = (float)(int16_t)(cpu->gte_ctrl[30] & 0xFFFFu) / 4096.0f / munit;
    if (s_zsf3 <= 0.0f || s_zsf4 <= 0.0f) s_zsf3 = s_zsf4 = 0.0f;
    if (s_debug_mode_addr && rd8(s_debug_mode_addr + 7u) == 4u && address == MODEL_RIGID) {
        NVert t[3];
        memset(t, 0, sizeof t);
        t[0].x = -100; t[0].y = -100; t[0].z = 600;
        t[1].x = 100;  t[1].y = -100; t[1].z = 600;
        t[2].x = 0;    t[2].y = 100;  t[2].z = 600;
        for (int k = 0; k < 3; k++) { t[k].r = 255; t[k].g = 0; t[k].b = 255; }
        c.bank = FLAT_BANK_OPAQUE;
        s_cull = 0;
        NVert g2[3];
        memcpy(g2, t, sizeof g2);
        for (int k = 0; k < 3; k++) { g2[k].z = 900; g2[k].x *= 1.5f; g2[k].x += 40.0f; g2[k].y *= 1.5f; g2[k].r = 0; g2[k].g = 255; g2[k].b = 0; }
        emit_tri(&c, &g2[0], &g2[1], &g2[2], 0x34u, 0, 0x100u, c.ot + 8u, 0);   /* far green: drawn second */
        emit_tri(&c, &t[0], &t[1], &t[2], 0x34u, 0, 0x100u, c.ot + 8u, 0);      /* near magenta: drawn first */
        c.bank = 0;
    }
    /* Model faces wind the other way round from the scenery's (the game
     * keeps a face when its screen NCLIP xor this word is positive... as
     * measured: back_facing must be inverted); a negative word flips it. */
    s_dbg_mflags = s_debug_mode_addr ? rd8(s_debug_mode_addr + 23u) : 0u;
    s_cull_flip = ((s_dbg_mflags >> 2) & 1) ^ (int)(rd32(0x800A9DACu) >> 31);
    s_ztab_half = 1;
    uint32_t p = list;
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t w0 = rd32(p);
        uint32_t kind = w0 & 0x38000000u;
        if (kind == 0u) break;
        int n = kind == 0x38000000u ? 4 : 3;
        NVert v[4];
        uint8_t uvs[4][2];
        for (int k = 0; k < n; k++) {
            uint32_t code = rd32(p + (uint32_t)k * 12u);
            uint32_t xy = rd32(p + (uint32_t)k * 12u + 4u), zw = rd32(p + (uint32_t)k * 12u + 8u);
            float x = (int16_t)(xy & 0xFFFFu), y = (int16_t)(xy >> 16), z = (int16_t)(zw & 0xFFFFu);
            int bi = skinned ? (int)((code & 3u) | ((code >> 6) & 0xCu) | ((code >> 12) & 0x30u)) : 0;
            const float *M = mats[bi];
            NVert *o = &v[k];
            if (s_dbg_mflags & 1u) z = -z;
            if (s_dbg_mflags & 2u) {
                o->x = (M[0] * x + M[3] * y + M[6] * z + M[9]) * munit;
                o->y = (M[1] * x + M[4] * y + M[7] * z + M[10]) * munit;
                o->z = (M[2] * x + M[5] * y + M[8] * z + M[11]) * munit;
            } else {
            o->x = (M[0] * x + M[1] * y + M[2] * z + M[9]) * munit;
            o->y = (M[3] * x + M[4] * y + M[5] * z + M[10]) * munit;
            o->z = (M[6] * x + M[7] * y + M[8] * z + M[11]) * munit;
            }
            o->r = (float)(code & 0xFCu) * tr; o->g = (float)((code >> 8) & 0xFCu) * tg; o->b = (float)((code >> 16) & 0xFCu) * tb;
            uvs[k][0] = (uint8_t)(zw >> 16); uvs[k][1] = (uint8_t)(zw >> 24);
            o->u = uvs[k][0]; o->v = uvs[k][1];
        }
        uint8_t tmb = rd8(p + (n == 4 ? 39u : 27u));
        int textured = (w0 & 0x04000000u) != 0u;
        int semi = (w0 & 0x02000000u) != 0u;
        uint8_t flags = (uint8_t)((textured ? 0u : 0x10u) | (semi ? (tmb & 0x60u) : 0x60u));
        /* double-sided: the last vertex's command word is negative */
        if (rd32(p + (uint32_t)(n - 1) * 12u) & 0x80000000u) flags |= 2u;
        s_semi_override = semi;
        s_pal_override = (uvs[0][0] >> 6) + (uvs[0][1] >> 6) * 4;
        if (s_debug_mode_addr && rd8(s_debug_mode_addr + 7u) == 5u) {   /* debug: colour by renderer */
            float cr = address == MODEL_RIGID ? 255.0f : address == MODEL_RIGID_TINT ? 255.0f : 0.0f;
            float cg = address == MODEL_SKINNED ? 255.0f : address == MODEL_RIGID_TINT ? 255.0f : 0.0f;
            float cb = address == MODEL_SKINNED_TINT ? 255.0f : 0.0f;
            flags = (uint8_t)((flags & 0x02u) | 0x70u); s_semi_override = 0;
            for (int k = 0; k < n; k++) { float sh = 0.5f + 0.5f * (float)((guard * 7 + k) % 5) / 4.0f; v[k].r = cr * sh; v[k].g = cg * sh; v[k].b = cb * sh; }
        }
        if (s_debug_mode_addr && rd8(s_debug_mode_addr + 7u) == 3u) {   /* debug: loud flat */
            flags = 0x70u; s_semi_override = 0;
            for (int k = 0; k < n; k++) { v[k].r = 255.0f; v[k].g = 0.0f; v[k].b = 255.0f; }
            s_zsf3 = s_zsf4 = 1e-6f;   /* nearest slot */
            s_dbg_nosuffix = 1;
        }
        /* The list's quad vertices are already cyclic (the renderer stores
         * them to the GPU packet as 0 1 3 2); emit_face/emit_poly split on
         * the 1-3 diagonal like the GPU does. */
        emit_face(&c, v, n, flags, (uint8_t)(tmb & 0x1Fu), (const uint8_t (*)[2])uvs, c.ztab);
        p += n == 4 ? 48u : 36u;
    }
    s_semi_override = -1;
    s_pal_override = -1;
    s_zsf3 = s_zsf4 = 0.0f;
    s_dbg_nosuffix = 0;
    s_qmul = 1.0f;
    s_cull_flip = 0;
    s_ztab_half = 0;
    s_arena_used[buf] += c.out_used;
    s_model_calls++;
    s_model_tris += c.out_used / PACKET_BYTES;
    if ((s_model_calls % 2000u) == 1u) {
        fprintf(stdout, "tsr native scene: model %08X list %08X -> %u packets (calls %u)\n",
                address, list, c.out_used / PACKET_BYTES, s_model_calls);
        fflush(stdout);
    }
    if ((mode & 0x0Fu) != 2u) cpu->gpr[4] = s_empty_model;   /* the game's renderer finds an empty list */
}

/* ---- hook ----------------------------------------------------------------- */

/*
 * The game calls the section draw four times per view: its two section lists
 * (0x800CD250 and the second list 0x800D5890), each in pass 0 and pass 1.
 * The lists hold the sections of the areas (rooms) visible from the camera
 * this frame, built by 0x80011568 from the area table at 0x800B4D70 (52 bytes
 * per area: +0 visible, +4/+6/+8/+10 screen rect x0, y0, x1, y1). When the
 * area mode word 0x800A9D48 is 1 (races), pass 0 draws the sections of the
 * camera's own area (0x800A9CAC[0x800A9EC4]) and pass 1 those of the other
 * areas, each only through its area's screen rect: the object renderers
 * reject every face wholly outside it. A section whose per-view record word
 * (+24 + view * 4) is zero is skipped. The native path follows the same
 * rules call by call, so it draws what the game would, and takes the
 * sections it draws out of the game's list.
 */
#define AREA_TABLE    0x800B4D70u
#define AREA_MODE     0x800A9D48u
#define CUR_AREA_TBL  0x800A9CACu
#define CUR_AREA_IDX  0x800A9EC4u
#define VIEW_INDEX    0x800A9C38u

static void section_draw_impl(struct CPUState *cpu, uint32_t address) {
    (void)address;
    /* Scenery everywhere it is seen: races, the track preview, the attract
     * demo and the menus' 3D scenes (character select by the window). The
     * title and the first menu are 2D screens (the cloud sky) with the 3D
     * camera parked high above the house (y about -10900; probably the
     * language/menu scene of an earlier design): the game shows nothing of
     * that scene, but drawn natively a subtractive band of it darkened the
     * title logo. Only there: track previews fly as high (car lot, cinema)
     * and their stage number differs after the attract demo. */
    if (!s_enabled || !s_arena_host[0]) return;
    if (!g_tsr_racing && tsr_ui_sky_recent() &&
        ((int32_t)rd32(VIEW + 68u) >> 6) < -4000) return;
    /* Debug mode byte (mod memory, see activation log): 0 native only,
     * 1 game only, 2 both overlaid. */
    if (s_debug_mode_addr && rd8(s_debug_mode_addr + 2u)) freeze_restore();
    uint8_t mode = s_debug_mode_addr ? rd8(s_debug_mode_addr) : 0;
    s_no_sprites = (mode & 0x10u) != 0;
    s_no_depth = (mode & 0x20u) != 0;
    s_no_split = (mode & 0x40u) != 0;
    s_no_expand = (mode & 0x80u) != 0;
    s_split_max = s_debug_mode_addr && rd8(s_debug_mode_addr + 24u) ? rd8(s_debug_mode_addr + 24u) : 5;
    s_under_rule = s_debug_mode_addr ? rd8(s_debug_mode_addr + 25u) : 0u;
    s_split_rule = s_debug_mode_addr ? rd8(s_debug_mode_addr + 26u) : 0u;
    s_early_opaque = !(s_debug_mode_addr && rd8(s_debug_mode_addr + 42u) == 1u);
    s_far_reject = s_debug_mode_addr && rd8(s_debug_mode_addr + 43u) == 1u;
    { uint8_t nz = s_debug_mode_addr ? rd8(s_debug_mode_addr + 3u) : 0; s_near = nz ? nz * 4.0f : NEAR_Z; }
    /* next byte: 1 = colour by section index, 2 = by face index */
    uint8_t dbg1 = s_debug_mode_addr ? rd8(s_debug_mode_addr + 1u) : 0;
    s_false_colour = dbg1 & 0x0Fu;
    s_no_cull = (dbg1 & 0x80u) != 0;
    mode &= 0x0Fu;
    if (mode == 1) { s_last_buf = -1; return; }   /* next native frame starts clean */
    uint32_t list = cpu->gpr[4], count = cpu->gpr[5], ot = cpu->gpr[6], pass = cpu->gpr[7];
    ensure_banks();
    if (!load_level()) return;
    /* The texture table can change after the parse (track photos in the
     * level select are loaded when chosen): re-read it every draw. */
    for (int t = 0; t < 32; t++) {
        s_tex_tpage[t] = rd16(TEX_TABLE + (uint32_t)t * 16u);
        s_tex_clut[t]  = rd16(TEX_TABLE + (uint32_t)t * 16u + 2u);
    }
    /* The underlay closes hairline cracks between track pieces; on the menu
     * characters (scenery sections there) it grew a fringe of skin texels. */
    s_underlay_on = g_tsr_racing || rd8(GAME_STAGE) == 38u;
    /* A new game frame starts building when the double buffer flips; each
     * view (one per player) has its own OT. */
    int buf = frame_buffer_begin();
    int first = 1;
    for (int i = 0; i < s_nviews; i++) if (s_views[i] == ot) first = 0;
    if (first && s_nviews < 4) s_views[s_nviews++] = ot;
    uint32_t frame = s_frame;

    static Ctx c;
    g_tsr_ctx = &c;
    /* The scenery is transformed with the view's second matrix copy (view+96,
     * loaded into the GTE light-matrix slot by 0x80010584 at the start of the
     * section draw); its first row carries the widescreen X scale. */
    for (int i = 0; i < 9; i++) c.m[i] = (int16_t)rd16(VIEW + 96u + (uint32_t)i * 2u) / 4096.0f;
    /* The section draw starts by recomputing the camera position it uses
     * (view+80..88) from the high-precision one at view+64..72 >> 6; this
     * hook runs before that, so take the same value directly. */
    for (int i = 0; i < 3; i++) c.cam[i] = (float)((int32_t)rd32(VIEW + 64u + (uint32_t)i * 4u) >> 6);
    c.h   = (float)(cpu->gte_ctrl[26] & 0xFFFFu);
    c.ofx = (float)(int32_t)cpu->gte_ctrl[24] / 65536.0f;
    c.ofy = (float)(int32_t)cpu->gte_ctrl[25] / 65536.0f;
    if (c.h <= 0.0f) return;
    c.ot = ot;
    c.ztab = rd32(ZTAB_PTR);
    c.zfar = rd32(ZFAR_VAR);
    if (c.zfar < 2u || c.zfar > 0x4000u) c.zfar = 0x4000u;
    for (int i = 0; i < 16; i++) c.zbias[i] = rd32(ZBIAS_TABLE + (uint32_t)i * 4u) + 80u;
    c.out = s_arena_host[buf] + s_arena_used[buf];
    c.out_addr = s_arena[buf] + s_arena_used[buf];
    c.out_used = 0;
    c.out_cap = ARENA_BYTES - s_arena_used[buf];
    c.rect_on = 0;
    find_marker_slot(&c);
    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);
    if (first) {
        emit_view_start_marker(&c);
        set_ot_depth(&c);
    }

    int by_area = rd32(AREA_MODE) == 1u;
    int cur_area = (int16_t)rd16(CUR_AREA_TBL + 2u * rd8(CUR_AREA_IDX));
    int view = (int8_t)rd8(VIEW_INDEX);
    uint32_t kept = 0, drawn = 0;
    for (uint32_t i = 0; i < count && i < 4096u; i++) {
        uint32_t p = rd32(list + i * 4u);
        uint32_t idx = (p - s_sec_base) / 20u;
        int item = (p >= s_sec_base && idx < s_sec_count && (p - s_sec_base) % 20u == 0u) ? s_sec_item[idx] : -1;
        int to_game = item < 0 || (s_no_sprites && s_items[item].kind);
        if (to_game || mode != 0) {
            /* left to the game (or drawn by both, debug mode 2) */
            if (kept != i) psx_mod_write_word(list + kept * 4u, p);
            kept++;
            if (to_game) continue;
        }
        int area = rd8(p + 15u);
        if (!(dbg1 & 0x40u) && by_area && (pass == 0u ? area != cur_area : pass == 1u ? area == cur_area : 1)) continue;
        uint32_t rec = rd32(p + 16u);
        if (!(dbg1 & 0x20u) && view >= 0 && view < 2 && rd32(rec + 24u + (uint32_t)view * 4u) == 0u) continue;
        c.rect_on = by_area && !(dbg1 & 0x10u);
        if (by_area) {
            uint32_t a = AREA_TABLE + (uint32_t)area * 52u;
            c.rx0 = (float)(int16_t)rd16(a + 4u);  c.ry0 = (float)(int16_t)rd16(a + 6u);
            c.rx1 = (float)(int16_t)rd16(a + 8u);  c.ry1 = (float)(int16_t)rd16(a + 10u);
            /* The portal walk (0x8004754c -> 0x800456e8) clips the rects to
             * the view bounds, x 0..512 in the game (the current area gets
             * exactly 0..512), -768..1280 with the widescreen patch of its
             * bounds: an edge there is the view edge, open into the
             * widescreen margins. */
            if (c.rx0 <= -767.0f || c.rx0 == 0.0f) c.rx0 = -1e30f;
            if (c.rx1 >= 1279.0f || c.rx1 == 512.0f) c.rx1 = 1e30f;
        }
        emit_item(&c, &s_items[item]);
        drawn++;
    }
    cpu->gpr[5] = kept;
    timespec_get(&t1, TIME_UTC);
    s_emit_us += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000LL + (t1.tv_nsec - t0.tv_nsec) / 1000);
    s_emit_n++;
    s_arena_used[buf] += c.out_used;
    if ((++s_stat_frames % 400u) == 1u) {
        fprintf(stdout, "tsr native scene: frame %u ot %08X pass %u list %08X: %u sections, %u drawn natively, %u left -> %u tris, arena %u KB (area mode %d, cur %d)\n",
                frame, ot, pass, list, count, drawn, kept, c.out_used / PACKET_BYTES, s_arena_used[buf] / 1024u, by_area, cur_area);
        fprintf(stdout, "tsr native scene: emit avg %.3f ms/call\n", s_emit_n ? (double)s_emit_us / 1000.0 / (double)s_emit_n : 0.0);
        s_emit_us = 0; s_emit_n = 0;
        fflush(stdout);
    }
}

/*
 * Debug camera freeze (debug byte +2: bit n freezes view n): at the entry of
 * the per-view section list builders (0x80011568 with areas, 0x80010000
 * without) the view's state below is captured once and written back on every
 * later frame (and again at each section draw), so the game's scenery and the
 * native one can be compared from exactly the same camera while the race goes
 * on.
 */
/* Frozen per view: the camera block (view+32..+120), the area table the
 * portal walk filled for that camera, and the current-area words. */
static const struct { uint32_t addr, len; } s_freeze_regions[3] = {
    { VIEW + 32u, 88u }, { AREA_TABLE, 52u * 16u }, { CUR_AREA_TBL, 8u } };
#define FREEZE_BYTES (88u + 52u * 16u + 8u)
static uint8_t s_freeze_buf[2][FREEZE_BYTES];
static int     s_frozen[2];

static void freeze_copy(int view, int save) {
    uint32_t o = 0;
    for (int r = 0; r < 3; r++)
        for (uint32_t i = 0; i < s_freeze_regions[r].len; i++, o++) {
            if (save) s_freeze_buf[view][o] = rd8(s_freeze_regions[r].addr + i);
            else psx_mod_write_byte(s_freeze_regions[r].addr + i, s_freeze_buf[view][o]);
        }
}

static void freeze_restore(void) {
    int view = rd8(VIEW_INDEX) & 1;
    if (!s_frozen[view] || !(rd8(s_debug_mode_addr + 2u) & (1u << view))) return;
    freeze_copy(view, 0);
}

void tsr_native_scene_builder_entry(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!s_debug_mode_addr) return;
    uint8_t fz = rd8(s_debug_mode_addr + 2u);
    int view = rd8(VIEW_INDEX) & 1;
    if (!(fz & (1u << view))) { s_frozen[view] = 0; return; }
    if (!s_frozen[view]) { freeze_copy(view, 1); s_frozen[view] = 1; }
    freeze_restore();
}

/* Debug code patch request (debug block +8 address, +12/+16 two words,
 * +20 sequence): applied at VBlank, between frames, so a running function is
 * never seen half-written. Used to stub functions while investigating. */
/* ---- 3D sprite list (wheels, antennae, shadows, sparks) -------------------
 *
 * Kart wheels and antenna tips, shadows and effects are queued during the
 * frame into a list of 28-byte entries that 0x80012040 (a0 list, a1 count)
 * turns into POLY_FT4 billboards:
 *   +0  x, y, z int16, relative to the camera, rotated by the camera matrix
 *       kept at scratchpad 0x1F8003D4 (MVMVA, light-colour matrix slot)
 *   +6  int16 OT bias (added to the AVSZ4 depth index)
 *   +8  half width (lo), half height (hi)
 *   +12 corner/flip flags (low 4 bits), rotation angle (hi; sin table
 *       0x8009F264, cos at +2048)
 *   +16 tpage (lo), clut (hi)    +20 u0 v0, width, height bytes
 *   +24 GPU command + colour (0x2C opaque / 0x2E blended)
 * The corners are offsets in a screen-aligned plane through the centre,
 * scaled by the widescreen X factor. The hook draws the entries natively
 * (depth tested like the models) and hands the game a count of zero.
 */
static void sprite_list_impl(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_enabled || !s_arena_host[0]) return;
    uint8_t mode = s_debug_mode_addr ? rd8(s_debug_mode_addr) : 0;
    if ((mode & 0x0Fu) == 1u) return;
    if (s_debug_mode_addr && rd8(s_debug_mode_addr + 40u) == 1u) return;   /* debug: sprites to the game */
    uint32_t list = cpu->gpr[4];
    int count = (int32_t)cpu->gpr[5];
    if (count <= 0 || (list & 0xFF800000u) != 0x80000000u) return;
    ensure_banks();
    int buf = frame_buffer_begin();

    static Ctx c;
    g_tsr_ctx = &c;
    memset(c.m, 0, sizeof c.m);
    c.h   = (float)(cpu->gte_ctrl[26] & 0xFFFFu);
    c.ofx = (float)(int32_t)cpu->gte_ctrl[24] / 65536.0f;
    c.ofy = (float)(int32_t)cpu->gte_ctrl[25] / 65536.0f;
    if (c.h <= 0.0f) return;
    c.ot = rd32(0x800A9B10u);
    c.ztab = rd32(ZTAB_PTR);
    c.zfar = rd32(ZFAR_VAR);
    if (c.zfar < 2u || c.zfar > 0x4000u) c.zfar = 0x4000u;
    for (int i = 0; i < 16; i++) c.zbias[i] = rd32(ZBIAS_TABLE + (uint32_t)i * 4u) + 80u;
    c.out = s_arena_host[buf] + s_arena_used[buf];
    c.out_addr = s_arena[buf] + s_arena_used[buf];
    c.out_used = 0;
    c.out_cap = ARENA_BYTES - s_arena_used[buf];
    c.rect_on = 0;
    c.bank = 0;
    find_marker_slot(&c);
    view_begin(&c);

    /* camera matrix (light-colour slot) */
    float M[9];
    for (int i = 0; i < 9; i++) {
        uint32_t w = rd32(0x1F8003D4u + (uint32_t)(i / 2) * 4u);
        M[i] = (int16_t)((i & 1) ? (w >> 16) : (w & 0xFFFFu)) / 4096.0f;
    }
    int s2 = (int16_t)rd16(0x800A9806u);
    float zsf4 = (float)(int16_t)(cpu->gte_ctrl[30] & 0xFFFFu) / 4096.0f;
    if (zsf4 <= 0.0f) zsf4 = 0.0625f;
    /* sprite camera units -> scenery units (debug +41: value / 16) */
    float sunit = 0.5f;
    if (s_debug_mode_addr && rd8(s_debug_mode_addr + 41u)) sunit = rd8(s_debug_mode_addr + 41u) / 16.0f;
    s_ztab_half = 1;
    s_cull = 0;

    for (int i = 0; i < count && i < 4096; i++) {
        uint32_t e = list + (uint32_t)i * 28u;
        float vx = (int16_t)rd16(e), vy = (int16_t)rd16(e + 2u), vz = (int16_t)rd16(e + 4u);
        int bias = (int16_t)rd16(e + 6u);
        uint32_t t3 = rd32(e + 8u), w12 = rd32(e + 12u);
        uint32_t angle = w12 >> 16, flags = w12 & 0xFu;
        const uint32_t at = 0xFFFF0000u;
        uint32_t a0, a1, t4;
        /* corner offsets, packed x | y << 16 exactly as the game builds them */
        if (flags == 0u) {
            t4 = t3 ^ at;
            a0 = t3 ^ 0xFFFFu;
            a1 = t4 ^ 0xFFFFu;
        } else {
            a1 = t3 & at;
            if (flags & 2u)      { t4 = (t3 + t3) & 0xFFFFu; a0 = 0u; }
            else if (flags & 8u) { a0 = ((t3 + t3) & 0xFFFFu) ^ 0xFFFFu; t4 = 0u; }
            else                 { t4 = t3 & 0xFFFFu; a0 = t4 ^ 0xFFFFu; }
            t3 = a1 << 1;
            if (flags & 1u) {
                a1 = a0; a0 = a0 + t3; t3 = t3 + t4;
            } else if (flags & 4u) {
                t3 ^= at;
                uint32_t v1 = t4;
                a1 = a0 + t3; t4 = t4 + t3; t3 = v1;
            } else {
                t3 = a1;
                uint32_t v1 = a1 ^ at;
                uint32_t t6 = v1 + a0;
                a0 = t3 + a0; t3 = a1 + t4; t4 = v1 + t4; a1 = t6;
            }
        }
        /* billboard matrix (rows 1-2; row 3 is 0 0 4096) */
        float r11, r12, r21, r22;
        if (angle == 0u) {
            r11 = 6553.0f; r12 = 0.0f; r21 = 0.0f; r22 = (float)(int16_t)s2;
        } else {
            int v1 = (int16_t)rd16(0x8009F264u + angle * 2u);
            int t5 = (int16_t)rd16(0x8009F264u + angle * 2u + 2048u);
            v1 >>= 2; t5 >>= 2;
            if (s2 - 4096 != 0) { v1 = (int16_t)((v1 * s2) >> 12); t5 = (int16_t)((t5 * s2) >> 12); }
            int t2 = (v1 >> 1) + v1, a = (t5 >> 1) + t5;
            r11 = (float)(int16_t)a; r12 = (float)(int16_t)(~t2 & 0xFFFF);
            r21 = (float)(int16_t)v1; r22 = (float)(int16_t)t5;
        }
        float cx = M[0] * vx + M[1] * vy + M[2] * vz;
        float cy = M[3] * vx + M[4] * vy + M[5] * vz;
        float cz = M[6] * vx + M[7] * vy + M[8] * vz;
        /* GPU quad order p0 p1 p2 p3 = corners a1 t4 a0 t3 */
        const uint32_t cw[4] = { a1, t4, a0, t3 };
        NVert v[4];
        float zsum = 0.0f;
        int behind = 0;
        uint32_t uvw = rd32(e + 20u);
        uint32_t uv0 = uvw & 0xFFFFu, W = (uvw >> 16) & 0xFFu, H = uvw >> 24;
        const uint32_t uvs[4] = { uv0, uv0 + W, uv0 + (H << 8), uv0 + W + (H << 8) };
        uint32_t code = rd32(e + 24u);
        for (int k = 0; k < 4; k++) {
            float ox = (int16_t)(cw[k] & 0xFFFFu), oy = (int16_t)(cw[k] >> 16);
            v[k].x = (cx + (r11 * ox + r12 * oy) / 4096.0f) * sunit;
            v[k].y = (cy + (r21 * ox + r22 * oy) / 4096.0f) * sunit;
            v[k].z = cz * sunit;
            if (v[k].z < s_near) behind = 1;
            zsum += cz;
            v[k].r = (float)(code & 0xFFu); v[k].g = (float)((code >> 8) & 0xFFu); v[k].b = (float)((code >> 16) & 0xFFu);
            v[k].u = (float)(uvs[k] & 0xFFu); v[k].v = (float)((uvs[k] >> 8) & 0xFFu);
        }
        /* A billboard stands for a round thing (wheel, antenna tip, spark)
         * centred on its position: drawn flat at the centre depth its lower
         * half sinks into the floor the native depth knows about. Move it
         * towards the camera by its half size along the view rays (the
         * screen image is unchanged), like the front of a sphere. */
        {
            float hw = (float)(int16_t)(rd32(e + 8u) & 0xFFFFu), hh = (float)(int16_t)(rd32(e + 8u) >> 16);
            /* the same for every billboard (about a wheel's radius): stacked
             * billboards (a level-select photo, its frame, its paper and
             * icon) keep equal depths, so the game's list order still
             * decides which one shows */
            (void)hw; (void)hh;
            float r = 40.0f * sunit;
            float z0 = cz * sunit, z1 = z0 - r;
            if (z1 < s_near) z1 = s_near;
            if (z0 > z1) {
                float f = z1 / z0;
                for (int k = 0; k < 4; k++) { v[k].x *= f; v[k].y *= f; v[k].z *= f; }
            }
        }
        if (behind) continue;
        int otz = (int)(zsum * zsf4);
        if (otz < 40) continue;
        int idx = otz + bias;
        if (idx < 0) idx = 0;
        if ((uint32_t)idx >= c.zfar) continue;
        uint32_t ot_entry = c.ot + rd16(c.ztab + (uint32_t)idx * 2u);
        if (ot_entry == c.marker_entry && ot_entry >= c.ot + 4u) ot_entry -= 4u;
        uint32_t w16 = rd32(e + 16u);
        uint16_t tpage = (uint16_t)(w16 & 0xFFFFu), clut = (uint16_t)(w16 >> 16);
        uint32_t cmd = (code & 0x02000000u) ? 0x36u : 0x34u;
        emit_tri(&c, &v[0], &v[1], &v[2], cmd, clut, tpage, ot_entry, 0);
        emit_tri(&c, &v[1], &v[3], &v[2], cmd, clut, tpage, ot_entry, 0);
    }
    s_ztab_half = 0;
    s_arena_used[buf] += c.out_used;
    cpu->gpr[5] = 0;   /* the game's renderer finds nothing to draw */
}

/* debug: host time spent in the hooks, microseconds per VBlank (+32 scenery,
 * +36 models) */
static double s_us_scene, s_us_model;
static double now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

void tsr_native_model_draw(struct CPUState *cpu, uint32_t address) {
    double t0 = now_us();
    model_draw_impl(cpu, address);
    s_us_model += now_us() - t0;
}

void tsr_native_sprite_list_draw(struct CPUState *cpu, uint32_t address) {
    double t0 = now_us();
    sprite_list_impl(cpu, address);
    s_us_model += now_us() - t0;
}

void tsr_native_scene_section_draw(struct CPUState *cpu, uint32_t address) {
    double t0 = now_us();
    section_draw_impl(cpu, address);
    s_us_scene += now_us() - t0;
}

/* debug block byte (0 when the block is not allocated) */
unsigned tsr_native_scene_debug_byte(unsigned off) {
    return s_debug_mode_addr ? rd8(s_debug_mode_addr + off) : 0u;
}

void tsr_native_scene_debug_vblank(void) {
    if (!s_debug_mode_addr) return;
    psx_mod_write_word(s_debug_mode_addr + 28u, s_vb_tris);
    psx_mod_write_word(s_debug_mode_addr + 32u, (uint32_t)s_us_scene);
    psx_mod_write_word(s_debug_mode_addr + 36u, (uint32_t)s_us_model);
    s_vb_tris = 0;
    s_us_scene = s_us_model = 0.0;
    uint8_t seq = rd8(s_debug_mode_addr + 20u);
    if (seq == rd8(s_debug_mode_addr + 21u)) return;
    uint32_t a = rd32(s_debug_mode_addr + 8u);
    if ((a & 0xFF800003u) == 0x80000000u) {
        psx_mod_write_code_word(a, rd32(s_debug_mode_addr + 12u));
        psx_mod_write_code_word(a + 4u, rd32(s_debug_mode_addr + 16u));
    }
    psx_mod_write_byte(s_debug_mode_addr + 21u, seq);
}

/* Called from the plugin's activation callback. */
void tsr_native_scene_activate(int enabled, int draw_distance) {
    s_enabled = enabled;
    s_draw_distance = (float)draw_distance;
    s_level_sig = 0; s_nitems = 0;
    s_last_buf = -1;
    if (enabled && !s_empty_model) s_empty_model = psx_mod_memory_alloc(64u, 4u);
    if (enabled && !s_debug_mode_addr) {
        s_debug_mode_addr = psx_mod_memory_alloc(64u, 4u);
        fprintf(stdout, "tsr native scene: debug mode byte at %08X\n", s_debug_mode_addr);
        fflush(stdout);
    }
    if (enabled && !s_arena[0]) {
        for (int i = 0; i < 2; i++) {
            s_arena[i] = psx_mod_alloc_texture_packet_memory(ARENA_BYTES, 64u);
            s_arena_host[i] = s_arena[i] ? psx_mod_gpu_dma_host_ptr(s_arena[i], ARENA_BYTES) : NULL;
        }
        if (!s_arena_host[0] || !s_arena_host[1]) {
            fprintf(stdout, "tsr native scene: packet arena unavailable, disabled\n");
            s_enabled = 0;
        }
    }
}
