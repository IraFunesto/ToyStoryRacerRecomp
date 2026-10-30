#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "tsr_settings_sync.h"
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#endif

/* Toy Story Racer - one game, three discs.
 *
 * The install holds a project per disc: the European Es, It, Nl disc
 * (SLES-03398) at the root, the American disc (SLUS-01214) in versions/USA
 * and the European Fr, De disc (SLES-03397) in versions/FrDe. The setup
 * builds the ones whose disc the player has (codegen_setup.c); each is a
 * complete game with this plugin.
 *
 *   Version (PAL 50 Hz / NTSC 60 Hz): Play starts the built game of that
 *   standard -- the American program for NTSC, a European one for PAL --
 *   with this game's launcher settings, mod options and memory cards, and
 *   this one quits. The quick start's language works in every program.
 *
 *   Voices: before a game mounts its extracted disc (tsr_data), the
 *   character voice files the other installed discs have and it lacks
 *   (AUDIO/x_CHAR1-4.XA, tools/add_disc_voices.py does the same by hand)
 *   are copied into it and listed in its layout, so every language's voices
 *   play in every version (plugins/tsr_disc_files.c picks them).
 *
 * Nothing of a disc the player does not have is ever used: every file comes
 * from the player's own discs. */

#define PATH_CAP 1024

typedef struct {
    const char* dir;      /* from the install root */
    const char* serial;
    int ntsc;
} Version;

static const Version VERSIONS[] = {
    { "",               "SLES-" "03398", 0 },   /* split: make_disc_project.py renames the serial */
    { "versions/FrDe",  "SLES-" "03397", 0 },
    { "versions/USA",   "SLUS-" "01214", 1 },
};
#define NVERSIONS (int)(sizeof VERSIONS / sizeof VERSIONS[0])

static char s_root[PATH_CAP];      /* the install root, "" when not an install */
static int s_self = -1;            /* the running game's version */

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void join(char* out, const char* a, const char* b) {
    snprintf(out, PATH_CAP, "%s%s%s", a, (a[0] && b[0]) ? "/" : "", b);
}

static void version_root(int v, char* out) { join(out, s_root, VERSIONS[v].dir); }

static char* read_text(const char* path, long* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* s = (char*)malloc((size_t)n + 1u);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    fclose(f);
    if (s) { s[n] = '\0'; if (len) *len = n; }
    return s;
}

static int write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

/* The game's project: the folder above the executable's (build-release/,
 * build-debug/...) holding game.toml; the install root is that folder or,
 * for a project under versions/, the one two levels up. */
static void find_install(void) {
#if defined(_WIN32)
    char exe[PATH_CAP], game[PATH_CAP], probe[PATH_CAP];
    if (s_self >= 0 || !GetModuleFileNameA(NULL, exe, sizeof exe)) return;
    for (char* p = exe; *p; ++p) if (*p == '\\') *p = '/';
    /* a game lies in its project's build folder (which has a game.toml copy
     * too), the setup executable in the project itself */
    char* slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = '\0';
    char exe_dir[PATH_CAP];
    snprintf(exe_dir, sizeof exe_dir, "%s", exe);
    slash = strrchr(exe, '/');
    if (slash) *slash = '\0';
    snprintf(game, sizeof game, "%s/game.toml", exe);
    char* toml = slash ? read_text(game, NULL) : NULL;
    if (!toml) {
        snprintf(exe, sizeof exe, "%s", exe_dir);
        snprintf(game, sizeof game, "%s/game.toml", exe);
        toml = read_text(game, NULL);
    }
    if (!toml) return;
    int self = -1;
    for (int v = 0; v < NVERSIONS && self < 0; ++v) {
        snprintf(probe, sizeof probe, "id = \"%s\"", VERSIONS[v].serial);
        if (strstr(toml, probe)) self = v;
    }
    free(toml);
    if (self < 0) return;
    /* the project's place in the install: the root, or versions/<name> */
    size_t n = strlen(exe), m = strlen(VERSIONS[self].dir);
    if (m == 0) snprintf(s_root, sizeof s_root, "%s", exe);
    else if (n > m && exe[n - m - 1] == '/' && _stricmp(exe + n - m, VERSIONS[self].dir) == 0)
        snprintf(s_root, sizeof s_root, "%.*s", (int)(n - m - 1), exe);
    else return;   /* a project on its own: no other versions */
    s_self = self;
#endif
}

/* ---- the voices -------------------------------------------------------------- */

static int voice_path(const char* p) {
    return strncmp(p, "AUDIO/", 6) == 0 && p[6] >= 'A' && p[6] <= 'Z' && strncmp(p + 7, "_CHAR", 5) == 0 &&
           p[12] >= '1' && p[12] <= '4' && strcmp(p + 13, ".XA") == 0;
}

/* layout line "raw <lba> <count> <path>" of a voice file: its path and count */
static int voice_line(const char* line, char* path, unsigned* count) {
    unsigned lba;
    char p[64];
    if (sscanf(line, "raw %u %u %63s", &lba, count, p) != 3 || !voice_path(p)) return 0;
    strcpy(path, p);
    return 1;
}

static int copy_file(const char* from, const char* to, long expect) {
    FILE* in = fopen(from, "rb");
    if (!in) return 0;
    FILE* out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[65536];
    long total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { total = -1; break; }
        total += (long)n;
    }
    fclose(in);
    if (fclose(out) != 0 || total != expect) { remove(to); return 0; }
    return 1;
}

static void voices_prepare(const char* folder) {
    find_install();
    if (s_self < 0) return;
    char layout_path[PATH_CAP];
    join(layout_path, folder, "disc.psxdisc");
    for (int c = 0; c < PATH_CAP && layout_path[c]; ++c) if (layout_path[c] == '\\') layout_path[c] = '/';
    char* layout = read_text(layout_path, NULL);
    if (!layout) return;
    const char* sectors_at = strstr(layout, "\nsectors ");
    if (!sectors_at) { free(layout); return; }
    unsigned end = (unsigned)strtoul(sectors_at + 9, NULL, 10);
    char added[4096] = "";
    size_t added_len = 0;
    for (int v = 0; v < NVERSIONS; ++v) {
        if (v == s_self) continue;
        char other_root[PATH_CAP], other_data[PATH_CAP], other_layout_path[PATH_CAP];
        version_root(v, other_root);
        join(other_data, other_root, "tsr_data");
        join(other_layout_path, other_data, "disc.psxdisc");
        char* other = read_text(other_layout_path, NULL);
        if (!other) continue;
        for (char* line = strtok(other, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            char path[64], have[80];
            unsigned count;
            if (!voice_line(line, path, &count)) continue;
            snprintf(have, sizeof have, " %s", path);   /* already listed? */
            if (strstr(layout, have) || strstr(added, have)) continue;
            char from[PATH_CAP], to[PATH_CAP], dir[PATH_CAP];
            join(from, other_data, path);
            join(dir, folder, "AUDIO");
#if defined(_WIN32)
            _mkdir(dir);
#endif
            join(to, folder, path);
            if (!copy_file(from, to, (long)count * 2352L)) continue;
            int n = snprintf(added + added_len, sizeof added - added_len, "raw %u %u %s\n", end, count, path);
            if (n <= 0 || (size_t)n >= sizeof added - added_len) break;
            added_len += (size_t)n;
            end += count;
            fprintf(stdout, "TSR VERSIONS: voices %s added from %s\n", path, VERSIONS[v].serial);
        }
        free(other);
    }
    if (added_len) {
        /* the new sector count, then the files after the disc's last sector */
        size_t head = (size_t)(sectors_at - layout) + 1u;
        const char* rest = strchr(sectors_at + 1, '\n');
        size_t cap = strlen(layout) + added_len + 128u;
        char* out = (char*)malloc(cap);
        if (out) {
            int n = snprintf(out, cap, "%.*ssectors %u%s", (int)head, layout, end, rest ? rest : "\n");
            size_t len = (size_t)n;
            if (len && out[len - 1] != '\n') out[len++] = '\n';
            snprintf(out + len, cap - len, "# another disc's character voices (tsr_versions.c)\n%s", added);
            if (!write_text(layout_path, out))
                fprintf(stdout, "TSR VERSIONS: could not write %s\n", layout_path);
            free(out);
        }
    }
    free(layout);
}

/* ---- the version switch ------------------------------------------------------ */

/* the built game of version v (its launcher-published name, else the default) */
static int version_exe(int v, char* out) {
    char root[PATH_CAP], marker[PATH_CAP], name[256] = "ToyStoryRacer_Recompiled";
    version_root(v, root);
    for (int b = 0; b < 2; ++b) {
        const char* build = b ? "build" : "build-release";
        snprintf(marker, sizeof marker, "%s/%s/psxrecomp_exe_name-psx-runtime.txt", root, build);
        char* published = read_text(marker, NULL);
        if (published) {
            char* nl = strpbrk(published, "\r\n");
            if (nl) *nl = '\0';
            if (published[0]) snprintf(name, sizeof name, "%s", published);
            free(published);
        }
        snprintf(out, PATH_CAP, "%s/%s/%s.exe", root, build, name);
        if (file_exists(out)) return 1;
    }
    return 0;
}

#if defined(_WIN32)
static int start(const char* cmd, const char* dir) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    char line[PATH_CAP * 2];
    snprintf(line, sizeof line, "%s", cmd);
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        fprintf(stdout, "TSR VERSIONS: could not start %s (error %lu)\n", cmd, (unsigned long)GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}
#endif

/* The install's setup (the executable at its root) with the disc wizard
 * open, to set up another disc; this game quits. */
static void open_setup(const char* why) {
#if defined(_WIN32)
    char exe[PATH_CAP], cmd[PATH_CAP + 32];
    snprintf(exe, sizeof exe, "%s/ToyStoryRacer_Recompiled.exe", s_root);
    if (!file_exists(exe)) return;
    fprintf(stdout, "TSR VERSIONS: %s: opening the disc setup\n", why);
    fflush(stdout);
    SetEnvironmentVariableA("TOYSTORYRACERRECOMP_PROJECT_ROOT", s_root);
    SetEnvironmentVariableA("TOYSTORYRACERRECOMP_FORCE_SETUP", "1");
    snprintf(cmd, sizeof cmd, "\"%s\" --launcher", exe);
    if (start(cmd, s_root)) ExitProcess(0);
#else
    (void)why;
#endif
}

/* this game's mod options: value of option id set back to "no" */
static void reset_option(const char* id) {
#if defined(_WIN32)
    char path[PATH_CAP], from[64], to[64];
    if (!GetModuleFileNameA(NULL, path, sizeof path)) return;
    for (char* p = path; *p; ++p) if (*p == '\\') *p = '/';
    snprintf(strrchr(path, '/'), 32, "/mods/state.toml");
    char* state = read_text(path, NULL);
    if (!state) return;
    snprintf(from, sizeof from, "%s = \"yes\"", id);
    snprintf(to, sizeof to, "%s = \"no\" ", id);   /* same length */
    char* at = strstr(state, from);
    if (at) {
        memcpy(at, to, strlen(to));
        write_text(path, state);
    }
    free(state);
#else
    (void)id;
#endif
}

static void versions_activate(void) {
    char want[16], setup[8];
    find_install();
    if (s_self < 0) return;
    if (psx_mod_option_value("toystoryracer.widescreen", "version", "setup", setup, sizeof setup) &&
        !strcmp(setup, "yes")) {
        reset_option("setup");
        open_setup("set up another disc");
    }
    if (!psx_mod_option_value("toystoryracer.widescreen", "version", "standard", want, sizeof want))
        return;
    const int ntsc = strcmp(want, "ntsc") == 0;
    if (VERSIONS[s_self].ntsc == ntsc) return;
    char exe[PATH_CAP], self_exe[PATH_CAP];
    int target = -1;
    for (int v = 0; v < NVERSIONS && target < 0; ++v)
        if (VERSIONS[v].ntsc == ntsc && version_exe(v, exe)) target = v;
    if (target < 0) {
        fprintf(stdout, "TSR VERSIONS: no %s version built (its disc is not set up)\n", ntsc ? "NTSC" : "PAL");
        open_setup(ntsc ? "NTSC needs the USA disc" : "PAL needs a European disc");
        return;
    }
#if defined(_WIN32)
    if (!GetModuleFileNameA(NULL, self_exe, sizeof self_exe)) return;
    for (char* p = self_exe; *p; ++p) if (*p == '\\') *p = '/';
    char from_dir[PATH_CAP], to_dir[PATH_CAP], to_root[PATH_CAP], cmd[PATH_CAP * 2];
    snprintf(from_dir, sizeof from_dir, "%s", self_exe);
    *strrchr(from_dir, '/') = '\0';
    snprintf(to_dir, sizeof to_dir, "%s", exe);
    *strrchr(to_dir, '/') = '\0';
    version_root(target, to_root);
    tss_sync_settings(from_dir, to_dir);
    char choice[PATH_CAP];
    join(choice, s_root, "tsr_version.txt");
    write_text(choice, VERSIONS[target].dir[0] ? VERSIONS[target].dir : ".");
    snprintf(cmd, sizeof cmd, "\"%s\" --no-launcher", exe);
    /* the setup's project choice (codegen_setup.c), for that game's own */
    SetEnvironmentVariableA("TOYSTORYRACERRECOMP_PROJECT_ROOT", to_root);
    fprintf(stdout, "TSR VERSIONS: %s -> %s (%s)\n", VERSIONS[s_self].serial, VERSIONS[target].serial, exe);
    fflush(stdout);
    if (start(cmd, to_root)) ExitProcess(0);
#else
    (void)self_exe;
#endif
}

#if !defined(PSX_HAS_GAME_DISPATCH)
/* The setup (no game linked yet) takes any of the install's discs: Generate
 * & rebuild builds the selected disc's project (codegen_setup.c). The games
 * themselves keep accepting only their own disc. */
static int setup_accepts_disc(const char* serial) {
    find_install();
    if (s_self < 0) return 0;
    for (int v = 0; v < NVERSIONS; ++v) {
        char root[PATH_CAP], game[PATH_CAP];
        if (strcmp(serial, VERSIONS[v].serial) != 0) continue;
        version_root(v, root);
        join(game, root, "game.toml");
        return file_exists(game);
    }
    return 0;
}
#endif

PSX_MOD_CONSTRUCTOR(psx_register_tsr_versions_plugin) {
#if !defined(PSX_HAS_GAME_DISPATCH)
    psx_mod_set_disc_serial_hook(setup_accepts_disc);
#endif
    psx_mod_set_disc_folder_hook(voices_prepare);
    (void)psx_mod_register_activation_plugin("toystoryracer.versions", versions_activate);
}
