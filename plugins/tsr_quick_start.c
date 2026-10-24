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
 * in the manifest. */

#define FN_PLAY_FMV 0x80061F44u

static int s_skip_logos;

static void play_fmv_entry(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (s_skip_logos) psx_mod_finish_function();
}

static void quick_start_activate(void) {
    char logos[16];
    s_skip_logos = !psx_mod_option_value("toystoryracer.widescreen", "quick-start", "logos",
                                         logos, sizeof logos) ||
                   strcmp(logos, "show") != 0;
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_quick_start_plugin) {
    (void)psx_mod_register_activation_plugin("toystoryracer.quickstart", quick_start_activate);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.quickstart", FN_PLAY_FMV,
                                                 play_fmv_entry);
}
