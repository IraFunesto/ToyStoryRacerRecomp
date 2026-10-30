/* Title config for psxrecomp/host/psxrecomp_codegen_host.
 * Wired only when the build opts into -DPSX_SETUP_WIZARD=ON.
 *
 * One install, three discs: the European Es, It, Nl disc's project is the
 * install root, the American and the European Fr, De discs' projects are
 * versions/USA and versions/FrDe (sources only, like the root). Generate &
 * rebuild takes whichever of the three discs is selected: the disc is told
 * apart by its size (the generate step then checks its digests) and that
 * disc's project is generated and built, so any one disc is enough and every
 * further disc adds its version. The setup executable at the root starts the
 * version played last (tsr_version.txt, written here and by
 * plugins/tsr_versions.c), else any built one. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "codegen_setup.h"

#include "psxrecomp_codegen_host.h"
#include "plugins/tsr_settings_sync.h"

#define PATH_CAP 1100

/* The boot file names and serials are split: tools/make_disc_project.py
 * renames this project's own in the versions' copies. */
#define VERSION_CONFIG(marker)                                                  \
    {                                                                           \
        .display_name = "ToyStoryRacer",                                        \
        .project_root_env = "TOYSTORYRACERRECOMP_PROJECT_ROOT",                 \
        .build_dir_env = "TOYSTORYRACERRECOMP_BUILD_DIR",                       \
        .force_setup_env = "TOYSTORYRACERRECOMP_FORCE_SETUP",                   \
        .psxrecomp_cli_relpath = "psxrecomp/psxrecomp_cli.py",                  \
        .seed_cfg_relpath = "game.toml",                                        \
        .game_toml_relpath = "game.toml",                                       \
        .gen_marker_relpath = marker,                                           \
        .build_dir_name = "build-release",                                      \
        .cmake_target = "psx-runtime",                                          \
        .exe_basename = "ToyStoryRacer_Recompiled",                             \
        .prepare_note =                                                         \
            "Uses your legal disc with the local psxrecomp SDK to generate "    \
            "BIOS + game C, then cmake --build. The product lives under "       \
            "build-release/; reopening this setup exe forwards there.",         \
        .prepare_note_windows =                                                 \
            "Uses your legal disc (the European Es, It, Nl, the USA or the "    \
            "European Fr, De disc: each one you set up adds its version) "      \
            "with the local psxrecomp SDK to generate BIOS + game C, then "     \
            "quits and rebuilds via a helper. Afterward, this setup exe "       \
            "forwards to build-release/ (bios, mods, settings).",               \
        .prepare_note_no_cmake =                                                \
            "Uses your legal disc with the local psxrecomp SDK to generate "    \
            "BIOS + game C. Rebuild into build-release/, then relaunch this "   \
            "setup exe (it forwards to the product build).",                    \
    }

typedef struct {
    const char* dir;      /* from the install root */
    const char* serial;   /* game.toml [game] id */
    PsxrecompCodegenHostConfig cfg;
} Version;

static const Version kVersions[] = {
    { "", "SLES-" "03398", VERSION_CONFIG("generated/SLES_033" ".98_dispatch.c") },
    { "versions/USA", "SLUS-" "01214", VERSION_CONFIG("generated/SLUS_012" ".14_dispatch.c") },
    { "versions/FrDe", "SLES-" "03397", VERSION_CONFIG("generated/SLES_033" ".97_dispatch.c") },
};
#define NVERSIONS (int)(sizeof kVersions / sizeof kVersions[0])

static int s_found;
static int s_self;                    /* this project's version */
static int s_prev = -1;               /* the version played before this setup */
static char s_root[PATH_CAP];         /* the install root, "" outside an install */
static RecompLauncherCGameInfo* s_gi;
static int (*s_prepare)(const char*, char*, size_t, char*, size_t,
                        RecompLauncherCPrepareProgressFn, void*);

static int is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR);
}

static int is_file(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR);
}

static void version_root(int v, char* out) {
    snprintf(out, PATH_CAP, "%s%s%s", s_root, kVersions[v].dir[0] ? "/" : "", kVersions[v].dir);
}

static char* read_text(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* s = (char*)malloc((size_t)n + 1u);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    fclose(f);
    if (s) s[n] = '\0';
    return s;
}

static int read_choice(void) {
    char path[PATH_CAP + 32];
    snprintf(path, sizeof path, "%s/tsr_version.txt", s_root);
    char* choice = read_text(path);
    int found = -1;
    if (choice) {
        char* nl = strpbrk(choice, "\r\n");
        if (nl) *nl = '\0';
        for (int v = 0; v < NVERSIONS && found < 0; ++v)
            if (!strcmp(choice, kVersions[v].dir[0] ? kVersions[v].dir : ".")) found = v;
        free(choice);
    }
    return found;
}

static void write_choice(int v) {
    char path[PATH_CAP + 32];
    snprintf(path, sizeof path, "%s/tsr_version.txt", s_root);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fputs(kVersions[v].dir[0] ? kVersions[v].dir : ".", f);
    fclose(f);
}

/* This project: the executable's folder or one above it holding game.toml,
 * its version by the [game] id there. The install: this project, or the
 * folder this one is versions/<name> of, when it holds versions/. */
static void find_install(void) {
    if (s_found) return;
    s_found = 1;
#if defined(_WIN32)
    char dir[PATH_CAP], probe[PATH_CAP + 32];
    if (!GetModuleFileNameA(NULL, dir, sizeof dir)) return;
    for (char* p = dir; *p; ++p) if (*p == '\\') *p = '/';
    /* a game lies in its project's build folder (which has a game.toml copy
     * too), the setup executable in the project itself */
    char exe_dir[PATH_CAP];
    char* slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(exe_dir, sizeof exe_dir, "%s", dir);
    slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    snprintf(probe, sizeof probe, "%s/game.toml", dir);
    char* toml = slash ? read_text(probe) : NULL;
    if (!toml) {
        snprintf(dir, sizeof dir, "%s", exe_dir);
        snprintf(probe, sizeof probe, "%s/game.toml", dir);
        toml = read_text(probe);
    }
    if (!toml) return;
    for (int v = 0; v < NVERSIONS; ++v) {
        snprintf(probe, sizeof probe, "id = \"%s\"", kVersions[v].serial);
        if (strstr(toml, probe)) { s_self = v; break; }
    }
    free(toml);
    size_t n = strlen(dir), m = strlen(kVersions[s_self].dir);
    if (m == 0) snprintf(s_root, sizeof s_root, "%s", dir);
    else if (n > m && dir[n - m - 1] == '/' && _stricmp(dir + n - m, kVersions[s_self].dir) == 0)
        snprintf(s_root, sizeof s_root, "%.*s", (int)(n - m - 1), dir);
    snprintf(probe, sizeof probe, "%s/versions", s_root);
    if (!s_root[0] || !is_dir(probe)) s_root[0] = '\0';   /* a project on its own */
#endif
}

/* The size of the disc image's data track (a .cue's first FILE, or the
 * file itself), 0 when unknown (a CHD: left to the current project). */
static long long image_size(const char* path) {
#if defined(_WIN32)
    char bin[PATH_CAP];
    size_t n = strlen(path);
    snprintf(bin, sizeof bin, "%s", path);
    if (n > 4 && _stricmp(path + n - 4, ".cue") == 0) {
        char* cue = read_text(path);
        if (!cue) return 0;
        const char* f = strstr(cue, "FILE \"");
        const char* e = f ? strchr(f + 6, '"') : NULL;
        if (!e) { free(cue); return 0; }
        const char* slash = strrchr(path, '/');
        const char* back = strrchr(path, '\\');
        if (!slash || (back && back > slash)) slash = back;
        snprintf(bin, sizeof bin, "%.*s%.*s", slash ? (int)(slash - path + 1) : 0, path,
                 (int)(e - f - 6), f + 6);
        free(cue);
    } else if (n <= 4 || _stricmp(path + n - 4, ".bin") != 0) {
        return 0;
    }
    struct _stat64 st;
    return _stat64(bin, &st) == 0 ? (long long)st.st_size : 0;
#else
    (void)path;
    return 0;
#endif
}

/* the version whose game.toml lists the disc's size */
static int disc_version(const char* path) {
    const long long size = image_size(path);
    if (!size) return -1;
    char want[32];
    snprintf(want, sizeof want, "%lld", size);
    const size_t wl = strlen(want);
    for (int v = 0; v < NVERSIONS; ++v) {
        char toml_path[PATH_CAP + 16], root[PATH_CAP];
        version_root(v, root);
        snprintf(toml_path, sizeof toml_path, "%s/game.toml", root);
        char* toml = read_text(toml_path);
        if (!toml) continue;
        const char* sizes = strstr(toml, "known_sizes");
        const char* end = sizes ? strchr(sizes, ']') : NULL;
        int hit = 0;
        for (const char* p = sizes; p && p < end && !hit; ++p)
            hit = strncmp(p, want, wl) == 0 && (p[wl] < '0' || p[wl] > '9') && (p[-1] < '0' || p[-1] > '9');
        free(toml);
        if (hit) return v;
    }
    return -1;
}

/* version v's project uses the install's framework checkouts */
static int link_framework(int v) {
#if defined(_WIN32)
    static const char* const dirs[] = { "psxrecomp", "recomp-ui" };
    char root[PATH_CAP];
    version_root(v, root);
    for (int i = 0; i < 2; ++i) {
        char link[PATH_CAP + 16], target[PATH_CAP + 16], cmd[3 * PATH_CAP];
        snprintf(link, sizeof link, "%s/%s", root, dirs[i]);
        snprintf(target, sizeof target, "%s/%s", s_root, dirs[i]);
        if (is_dir(link) || !is_dir(target)) continue;
        for (char* p = link; *p; ++p) if (*p == '/') *p = '\\';
        for (char* p = target; *p; ++p) if (*p == '/') *p = '\\';
        snprintf(cmd, sizeof cmd, "cmd.exe /C mklink /J \"%s\" \"%s\"", link, target);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        memset(&pi, 0, sizeof pi);
        si.cb = sizeof si;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            return 0;
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (!is_dir(link)) return 0;
    }
#else
    (void)v;
#endif
    return 1;
}

static void use_version(int v) {
#if defined(_WIN32)
    char root[PATH_CAP];
    version_root(v, root);
    _putenv_s(kVersions[v].cfg.project_root_env, root);
#else
    (void)v;
#endif
}

static int route_prepare(const char* source_path, char* out_path, size_t out_cap,
                         char* err_msg, size_t err_cap,
                         RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx);

static void apply_version(RecompLauncherCGameInfo* gi, int v) {
    psxrecomp_codegen_host_apply(gi, &kVersions[v].cfg);
    if (gi->prepare_with_progress && gi->prepare_with_progress != route_prepare) {
        s_prepare = gi->prepare_with_progress;
        gi->prepare_with_progress = route_prepare;
    }
}

/* Generate & rebuild: the selected disc's project */
static int route_prepare(const char* source_path, char* out_path, size_t out_cap,
                         char* err_msg, size_t err_cap,
                         RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx) {
    const int v = s_root[0] && source_path ? disc_version(source_path) : -1;
#if defined(_WIN32)
    /* opened on purpose (plugins/tsr_versions.c): the game built now must
     * not open the setup again */
    _putenv_s(kVersions[0].cfg.force_setup_env, "");
#endif
    if (v >= 0 && v != s_self) {
        if (!link_framework(v)) {
            snprintf(err_msg, err_cap, "Could not link the framework into %s.", kVersions[v].dir);
            return 0;
        }
        fprintf(stderr, "tsr setup: %s disc -> project %s\n", kVersions[v].serial,
                kVersions[v].dir[0] ? kVersions[v].dir : ".");
        use_version(v);
        apply_version(s_gi, v);
        s_self = v;
    }
    if (v >= 0) {
        if (s_prev < 0) s_prev = read_choice();
        write_choice(v);
    }
    return s_prepare ? s_prepare(source_path, out_path, out_cap, err_msg, err_cap, on_progress, progress_ctx) : 0;
}

void psx_game_codegen_setup_apply(RecompLauncherCGameInfo* gi) {
    find_install();
#if defined(PSX_HAS_GAME_DISPATCH) && defined(_WIN32)
    /* a built game: a setup opened to add another disc is not its own */
    _putenv_s(kVersions[0].cfg.force_setup_env, "");
#endif
    s_gi = gi;
    apply_version(gi, s_self);
}

/* A version set up after another starts with that one's launcher settings
 * and mod options (its own disc and BIOS kept), on its own standard. */
static void adopt_settings(void) {
    if (!s_root[0] || s_prev < 0 || s_prev == s_self) return;
    char from[PATH_CAP + 32], to[PATH_CAP + 32], root[PATH_CAP], state_path[PATH_CAP + 48];
    version_root(s_prev, root);
    snprintf(from, sizeof from, "%s/build-release", root);
    version_root(s_self, root);
    snprintf(to, sizeof to, "%s/build-release", root);
    snprintf(state_path, sizeof state_path, "%s/settings.toml", from);
    if (!is_file(state_path)) return;
#if defined(_WIN32)
    _mkdir(to);
#endif
    tss_sync_settings(from, to);
    snprintf(state_path, sizeof state_path, "%s/mods/state.toml", to);
    char* state = read_text(state_path);
    if (!state) return;
    const int ntsc = strcmp(kVersions[s_self].serial, "SLUS-" "01214") == 0;
    char* at = strstr(state, ntsc ? "standard = \"pal\"" : "standard = \"ntsc\"");
    if (at) {
        /* same length: "pal" + a space for "ntsc" and back */
        const char* repl = ntsc ? "standard = \"ntsc\"" : "standard = \"pal\" ";
        size_t len = strlen(ntsc ? "standard = \"pal\"" : "standard = \"ntsc\"");
        size_t total = strlen(state) + 2;
        char* out = (char*)malloc(total);
        if (out) {
            snprintf(out, total, "%.*s%s%s", (int)(at - state), state, repl, at + len);
            tss_write(state_path, out);
            free(out);
        }
    }
    free(state);
    fprintf(stderr, "tsr setup: %s starts with the settings of %s\n", kVersions[s_self].serial,
            kVersions[s_prev].serial);
}

void psx_game_codegen_relaunch_or_exit(const char* disc_path) {
    adopt_settings();
    psxrecomp_codegen_host_relaunch_or_exit(disc_path);
}

/* The setup executable: the version chosen last, else any built one. */
void psx_game_codegen_forward_if_built(int argc, char** argv) {
    find_install();
#if !defined(PSX_HAS_GAME_DISPATCH)
    if (s_root[0]) {
        int order[NVERSIONS], n = 0;
        char path[PATH_CAP + 32];
        snprintf(path, sizeof path, "%s/tsr_version.txt", s_root);
        char* choice = read_text(path);
        if (choice) {
            char* nl = strpbrk(choice, "\r\n");
            if (nl) *nl = '\0';
            for (int v = 0; v < NVERSIONS && !n; ++v)
                if (!strcmp(choice, kVersions[v].dir[0] ? kVersions[v].dir : ".")) order[n++] = v;
            free(choice);
        }
        for (int v = 0; v < NVERSIONS; ++v)
            if (!n || order[0] != v) order[n++] = v;
        for (int i = 0; i < n; ++i) {
            char root[PATH_CAP], marker[PATH_CAP + 64];
            version_root(order[i], root);
            snprintf(marker, sizeof marker, "%s/%s", root, kVersions[order[i]].cfg.gen_marker_relpath);
            if (!is_file(marker)) continue;
            use_version(order[i]);
            /* starts that version's game and exits when it is built */
            psxrecomp_codegen_host_forward_if_built(&kVersions[order[i]].cfg, argc, argv);
        }
        use_version(s_self);
    }
#endif
    psxrecomp_codegen_host_forward_if_built(&kVersions[s_self].cfg, argc, argv);
}
