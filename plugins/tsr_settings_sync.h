#ifndef TSR_SETTINGS_SYNC_H
#define TSR_SETTINGS_SYNC_H

/* Toy Story Racer - one game's launcher settings and mod options given to
 * another version's game (plugins/tsr_versions.c when Play switches version,
 * codegen_setup.c when the setup builds a new one). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#define TSS_PATH_CAP 1024

static char* tss_read(const char* path, long* len) {
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

static int tss_write(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

/* text without the [section] blocks named in drop ("disc", "bios", ...) */
static char* tss_drop_sections(const char* text, const char* const* drop, int ndrop, char* kept, size_t kept_cap) {
    size_t cap = strlen(text) + 1u;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t o = 0, k = 0;
    int dropping = 0;
    for (const char* line = text; *line;) {
        const char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) + 1u : strlen(line);
        if (line[0] == '[') {
            dropping = 0;
            for (int i = 0; i < ndrop; ++i) {
                size_t m = strlen(drop[i]);
                if (strncmp(line + 1, drop[i], m) == 0 && line[1 + m] == ']') dropping = 1;
            }
        }
        if (!dropping) { memcpy(out + o, line, n); o += n; }
        else if (kept && k + n < kept_cap) { memcpy(kept + k, line, n); k += n; }
        line += n;
    }
    out[o] = '\0';
    if (kept) kept[k] = '\0';
    return out;
}

/* path from the settings of the game in dir made absolute */
static void tss_absolute_from(const char* dir, const char* value, char* out) {
    if (value[0] == '/' || (value[0] && value[1] == ':')) snprintf(out, TSS_PATH_CAP, "%s", value);
    else snprintf(out, TSS_PATH_CAP, "%s/%s", dir, value);
}

/* The launcher settings and mod options of this game for the game in
 * to_dir: everything but its own disc and BIOS, memory cards at the same
 * (absolute) files. */
static void tss_sync_settings(const char* from_dir, const char* to_dir) {
    char from[TSS_PATH_CAP], to[TSS_PATH_CAP];
    snprintf(from, sizeof from, "%s/settings.toml", from_dir);
    snprintf(to, sizeof to, "%s/settings.toml", to_dir);
    char* mine = tss_read(from, NULL);
    if (mine) {
        static const char* const own[] = { "disc", "bios" };
        static const char* const card[] = { "memcard" };
        char* theirs = tss_read(to, NULL);
        char kept[2048] = "", cards[1024] = "";
        char* base = tss_drop_sections(mine, own, 2, NULL, 0);
        if (theirs) { char* t = tss_drop_sections(theirs, own, 2, kept, sizeof kept); free(t); }
        char* body = base ? tss_drop_sections(base, card, 1, cards, sizeof cards) : NULL;
        if (body) {
            /* [memcard] with absolute paths */
            char memcard[2048] = "";
            size_t mlen = 0;
            for (char* line = strtok(cards, "\n"); line; line = strtok(NULL, "\n")) {
                char key[32], value[TSS_PATH_CAP], abs_path[TSS_PATH_CAP];
                char* q1 = strchr(line, '"');
                char* q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                if (q1 && q2 && sscanf(line, " %31[a-z0-9_]", key) == 1 &&
                    (!strcmp(key, "dir") || !strcmp(key, "card1") || !strcmp(key, "card2"))) {
                    snprintf(value, sizeof value, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
                    tss_absolute_from(from_dir, value, abs_path);
                    mlen += (size_t)snprintf(memcard + mlen, sizeof memcard - mlen, "%s = \"%s\"\n", key, abs_path);
                } else {
                    mlen += (size_t)snprintf(memcard + mlen, sizeof memcard - mlen, "%s\n", line);
                }
                if (mlen >= sizeof memcard) break;
            }
            size_t cap = strlen(body) + strlen(kept) + strlen(memcard) + 8u;
            char* out = (char*)malloc(cap);
            if (out) {
                snprintf(out, cap, "%s\n%s\n%s", body, memcard, kept);
                tss_write(to, out);
                free(out);
            }
        }
        free(body); free(base); free(theirs); free(mine);
    }
    snprintf(from, sizeof from, "%s/mods/state.toml", from_dir);
    snprintf(to, sizeof to, "%s/mods/state.toml", to_dir);
    char* state = tss_read(from, NULL);
    if (state) {
        char mods[TSS_PATH_CAP];
        snprintf(mods, sizeof mods, "%s/mods", to_dir);
#if defined(_WIN32)
        _mkdir(mods);
#endif
        tss_write(to, state);
        free(state);
    }
}

#endif
