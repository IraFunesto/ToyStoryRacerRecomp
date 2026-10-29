#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"

/* Toy Story Racer - quick start: the logo videos.
 *
 * The boot logo loop (0x80072618) plays the Activision, Disney Interactive
 * and Traveller's Tales videos through 0x80061F44(index), its only caller.
 * With Logo videos = Skip that call returns at once, as on the game's own
 * skip path (X or START on a video already seen): the loop runs on to the
 * language choice.
 *
 * This is a function-entry hook (game.toml mod_function_entry_funcs), not a
 * patch of the loop's branch: patched code leaves the compiled function for
 * the interpreter, and the function holding that loop is also the game's main
 * loop, which then ran at half speed. The language presets stay data patches
 * in the manifest.
 *
 * The American build's texts: its program carries the same six-language
 * tables as the European one but always reads the English entry. With a
 * language chosen, each table's first pointer is set to that language's at
 * the first logo call (the program is loaded, no menu drawn yet); a table
 * already changed, or not holding its English pointer, is left alone. */

#define FN_PLAY_FMV 0x80061F44u

#include "tsr_lang_tables.h"

static int s_skip_logos;
static int s_text_lang;   /* 0 English, 1 French, 2 German, 3 Italian, 4 Spanish, 5 Dutch */

static void texts_apply(void) {
    if (!s_text_lang) return;
    for (int i = 0; i < TSR_LANG_TABLE_COUNT; i++) {
        uint32_t t = TSR_LANG_TABLES[i].addr;
        if (psx_mod_read_word(t) == TSR_LANG_TABLES[i].en)
            psx_mod_write_word(t, psx_mod_read_word(t + 4u * (uint32_t)s_text_lang));
    }
}

static void play_fmv_entry(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    texts_apply();
    if (s_skip_logos) psx_mod_finish_function();
}

static void quick_start_activate(void) {
    char logos[16], lang[16];
    s_skip_logos = !psx_mod_option_value("toystoryracer.widescreen", "quick-start", "logos",
                                         logos, sizeof logos) ||
                   strcmp(logos, "show") != 0;
    s_text_lang = 0;
    if (TSR_LANG_TABLE_COUNT > 0 &&
        psx_mod_option_value("toystoryracer.widescreen", "quick-start", "language", lang, sizeof lang)) {
        static const char *const ids[6] = { "en", "fr", "de", "it", "es", "nl" };
        for (int i = 0; i < 6; i++) if (strcmp(lang, ids[i]) == 0) s_text_lang = i;
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_quick_start_plugin) {
    (void)psx_mod_register_activation_plugin("toystoryracer.quickstart", quick_start_activate);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.quickstart", FN_PLAY_FMV,
                                                 play_fmv_entry);
}
