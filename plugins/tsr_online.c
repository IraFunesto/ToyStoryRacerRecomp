#include <stdarg.h>
#include "psx_modrb.h"
extern void psx_dispatch_call(struct CPUState* cpu, uint32_t addr, uint32_t return_addr);
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "mod_memory.h"
#include "tsr_online.h"
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

/* Toy Story Racer - two players online (input lockstep).
 *
 * The game's own two-player mode runs on both PCs, from the same state:
 * when the other player joins, the host measures the network delay, sends
 * its save state, then every frame each PC sends the buttons of its own
 * player (host = port 1, the one who joins = port 2) and the game advances
 * only with both players' buttons of that frame, taken a few frames late so
 * the network has time to bring them. The PlayStation side computes exactly
 * the same on both PCs from the same state and buttons; a checksum of the
 * main RAM is compared every few seconds to catch a split.
 *
 * Started by the game's menu (tsr_online_host / tsr_online_join) or, for
 * tests, by the environment: TSR_ONLINE=host|join, TSR_ONLINE_ADDR (join),
 * TSR_ONLINE_PORT (default 27450), TSR_ONLINE_DELAY (frames; default:
 * measured from the ping). A host waiting for a player announces itself on
 * the local networks (UDP port + 1), so the join window can list it. */

#define STATE_SLOT     11            /* the save state slot used for the start */
#define MIN_DELAY      2
#define MAX_DELAY      16
#define RING           64            /* frames of buttons kept */
#define CHECK_EVERY    120           /* frames between RAM checksums */
#define PINGS          8             /* round trips measured before the start */
#define FRAME_MS       17            /* a frame, rounded up (60 Hz; 50 Hz is slower) */
#define CONNECT_MS     15000         /* join: give up when the host does not answer */
#define MAGIC          0x52535354u   /* "TSSR" */
#define BEACON_MAGIC   0x42535354u   /* "TSSB" */
#define VERSION        10u

enum { ROLE_NONE, ROLE_HOST, ROLE_JOIN };
enum { ST_OFF, ST_WAIT, ST_PING, ST_SAVE, ST_SEND, ST_RELOAD, ST_LOAD, ST_PLAY, ST_DEAD };
enum { MSG_HELLO = 1, MSG_STATE = 2, MSG_PAD = 3, MSG_CHECK = 4,
       MSG_PING = 5, MSG_PONG = 6, MSG_DELAY = 7, MSG_SETTINGS = 8, MSG_MARGIN = 9,
       MSG_SESSION = 10 };

/* Up to four players: the host and up to three who join, each connected to
 * the host only (a star). Every PC sends its own player's buttons to the
 * host; the host passes each player's on to the others. Player 1 is the
 * host, 2 the first who joined (the game's two players: ports 1 and 2), 3
 * and 4 the next ones, who drive the game's karts 3 and 4 (below). */
#define MAX_PLAYERS    4
#define MAX_PEERS      (MAX_PLAYERS - 1)

typedef struct {
    uint32_t frame, slot;
    PSXModPad pad;
} PadMsg;

typedef struct {   /* the room announcement */
    uint32_t magic, version, port, game;
    char name[32];
} Beacon;

static int s_role, s_state, s_delay = 3, s_delay_fixed;
static int s_view = -1;             /* split screen: the view shown, -1 both */
static int s_failed;                /* the last session ended badly */
static char s_peer_addr[64] = "127.0.0.1";
static int s_port = TSR_ONLINE_PORT;
static char s_message[256] = "";
static uint32_t s_game;             /* this game's code, to refuse another version */
static int s_slot;                  /* this PC's player: 0 the host, 1.. in the order they joined */
static int s_slots = 2;             /* players in the session */
static int s_start_req;             /* host: start with the players here (the menu's Start) */
static int s_join_loading;          /* join: the host's state is loading */
#if defined(_WIN32)
typedef struct {                    /* another PC (the host's: those who joined; theirs: the host) */
    SOCKET sock;
    uint32_t ip;                    /* its address (network order) */
    uint8_t* rx;                    /* receive buffer */
    size_t rx_len, rx_cap;
    int slot;                       /* its player */
    int ready;                      /* hello and settings checked */
    struct sockaddr_in udp;         /* where its UDP comes from (host: learned) */
    int udp_ok;
    uint32_t peer_ms, peer_ms_at;   /* its last send time, and when it came */
    int bonus;                      /* lockstep: frames more it asks for */
    float ahead;                    /* rollback: frames it runs past its confirmed ones */
    int pongs;
    DWORD rtt_max;
    uint32_t rbc[8], rbc_f[8];      /* rollback: its checksums */
    uint32_t check, check_frame;    /* lockstep: its checksum */
    int check_ok;
} Peer;
static Peer s_peer[MAX_PEERS];
static int s_npeers;
static SOCKET s_listen = INVALID_SOCKET;
static SOCKET s_pending = INVALID_SOCKET;   /* join: the connect in progress */
static SOCKET s_beacon = INVALID_SOCKET;
static DWORD s_connect_start, s_next_try, s_next_beacon;
static DWORD s_ping_sent[PINGS];
static int s_pings_sent;
static CRITICAL_SECTION s_req_lock;
#endif
/* requests from other threads (the join window) */
static volatile int s_req;          /* 0 none, 1 host, 2 join, 3 cancel */
static char s_req_addr[64];
static int s_req_port;

static uint32_t s_frame;             /* lockstep frame */
static PSXModPad s_pad[MAX_PLAYERS][RING];    /* every player's buttons, this PC's too */
static uint32_t s_have[MAX_PLAYERS][RING];    /* frame number + 1 stored at each slot */
static uint32_t s_at[MAX_PLAYERS][RING];      /* test (TSR_ONLINE_LAG ms): when they count as arrived */
static uint32_t s_newest[MAX_PLAYERS];        /* newest frame + 1 stored, per player */
/* Rollback play (TSR_ONLINE_RB on every PC): this PC plays each frame at
 * once, with the other players' buttons when they are here and a guess (the
 * last ones known) when they are not; when the real ones arrive and differ,
 * the machine goes back to that frame (psx_modrb) and replays up to now. */
#define RB_WINDOW      10   /* frames this PC may run past the others' last confirmed buttons */
#define RB_LOCAL_DELAY 1    /* frames this PC's own buttons are late (so most arrive in time) */
static int s_rb;                       /* rollback play is on */
static uint32_t s_rb_confirmed;        /* the frames below were played with the real buttons */
static PSXModPad s_used[MAX_PLAYERS][RING];   /* the buttons each frame was played with */
static uint32_t s_used_tag[RING];      /* frame + 1 */
static PSXModPad s_last[MAX_PLAYERS];  /* the newest real buttons known (the guess) */
static uint32_t s_last_frame[MAX_PLAYERS];
static uint32_t s_rb_rollbacks, s_rb_deepest, s_rb_stalls, s_rb_guessed, s_rb_slowed;
static float s_rb_ahead_avg;           /* frames past the confirmed ones, here */
static uint32_t s_rbc_mine[8], s_rbc_mine_f[8], s_rbc_sent;
static uint32_t s_check_frame, s_check_mine;
static int s_check_mine_ok;
static uint32_t s_crc, s_crc_next, s_crc_frame;   /* the check in progress (ram_checksum_part) */
/* The buttons also go by UDP, each packet with one player's last UDP_FRAMES
 * frames: a lost packet costs nothing (the next one brings its frames again)
 * and TCP's resend after a loss -- a stall of a fifth of a second or more on
 * a home connection -- no longer holds the game. The TCP copy stays, for
 * networks that let no UDP through. */
#define UDP_FRAMES   16
#define UDP_MAGIC    0x55535354u   /* "TSSU" */
typedef struct {
    uint32_t magic, slot;             /* whose buttons */
    uint32_t newest, have;            /* frames newest-UDP_FRAMES+1 .. newest; bit i: pad[i] is known */
    uint32_t sent_ms, echo_ms, hold_ms;   /* round trip: the receiver's last time, held here */
    uint32_t ask_lead;                /* frames more the sender asks for: it had to wait */
    PSXModPad pad[UDP_FRAMES];
} UdpPads;
#if defined(_WIN32)
static SOCKET s_udp = INVALID_SOCKET;
#endif
static uint32_t s_last_sent;       /* the newest frame whose buttons were sent */
static int s_send_delay;           /* frames ahead this PC sends its buttons (adapts) */
static uint32_t s_delay_drop_at;   /* not lowered before this time */
static float s_rtt_est;            /* round trip, ms (decaying maximum) */
static int s_loss_bonus;           /* frames more this PC asks for after its waits, fading */
static uint32_t s_loss_bonus_drop_at;
/* the connection's log (tsr_online.log): waits, round trip, delay */
static uint32_t s_stat_start, s_stat_waits, s_stat_wait_ms, s_stat_wait_max, s_stat_udp, s_stat_frames;
/* this PC's own time per frame (from the end of one lockstep step to the
 * start of the next: the game, the drawing, the pacing), in ms */
static double s_work_sum, s_work_max, s_prev_exit;
static uint32_t s_work_n, s_work_over18, s_work_over21;

static void xp_session(int slots, int slot);   /* players 3-4 (below) */

static int env_on(const char* name) {   /* set and not empty */
    const char* v = getenv(name);
    return v && v[0];
}

/* rollback play chosen (mod option; TSR_ONLINE_RB=1/0 overrides, for tests) */
static int rb_wanted(void) {
    const char* e = getenv("TSR_ONLINE_RB");
    char v[16];
    if (e && e[0]) return e[0] != '0';
    return psx_mod_option_value("toystoryracer.widescreen", "online", "netcode", v, sizeof v) && strcmp(v, "rollback") == 0;
}

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_message, sizeof s_message, fmt, ap);
    va_end(ap);
    fprintf(stdout, "TSR ONLINE: %s\n", s_message);
    fflush(stdout);
    FILE* f = fopen("tsr_online.log", "a");
    if (f) {
#if defined(_WIN32)
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02u:%02u:%02u %s\n", t.wHour, t.wMinute, t.wSecond, s_message);
#else
        fprintf(f, "%s\n", s_message);
#endif
        fclose(f);
    }
}

/* ---- state for the menus ---------------------------------------------- */

int tsr_online_status(void) {
    switch (s_state) {
    case ST_WAIT: return s_role == ROLE_HOST ? TSR_ONLINE_WAITING : TSR_ONLINE_CONNECTING;
    case ST_PING: case ST_SAVE: case ST_SEND: case ST_RELOAD: case ST_LOAD: return TSR_ONLINE_STARTING;
    case ST_PLAY: return TSR_ONLINE_PLAYING;
    case ST_DEAD: return s_failed ? TSR_ONLINE_FAILED : TSR_ONLINE_OFF;
    default: return TSR_ONLINE_OFF;
    }
}

const char* tsr_online_message(void) { return s_message; }
int tsr_online_is_host(void) { return s_role == ROLE_HOST; }

#if defined(_WIN32)
static int net_ready(void) {
    static int s_ok = -1;
    if (s_ok < 0) {
        WSADATA wsa;
        s_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    return s_ok;
}

static void request(int what, const char* addr, int port) {
    EnterCriticalSection(&s_req_lock);
    if (addr) snprintf(s_req_addr, sizeof s_req_addr, "%s", addr);
    s_req_port = port > 0 && port < 65536 ? port : TSR_ONLINE_PORT;
    s_req = what;
    LeaveCriticalSection(&s_req_lock);
}
#endif

void tsr_online_host(int port) {
#if defined(_WIN32)
    request(1, NULL, port);
#else
    (void)port;
#endif
}

void tsr_online_join(const char* addr, int port) {
#if defined(_WIN32)
    request(2, addr, port);
#else
    (void)addr; (void)port;
#endif
}

void tsr_online_cancel(void) {
#if defined(_WIN32)
    request(3, NULL, 0);
#endif
}

/* the disc this build is made for (the disc projects get their own id):
 * both PCs need the same one */
static uint32_t game_code_hash(void) {
    const char* disc = "SLES-03398";
    uint32_t h = 2166136261u;
    for (const char* c = disc; *c; c++) { h ^= (uint8_t)*c; h *= 16777619u; }
    return h;
}

#if defined(_WIN32)
static void close_sock(SOCKET* s) {
    if (*s != INVALID_SOCKET) closesocket(*s);
    *s = INVALID_SOCKET;
}

static void peer_close(Peer* p) {
    close_sock(&p->sock);
    p->rx_len = 0;
    p->udp_ok = 0;
    p->ready = 0;
}

/* end the session; failed: tell the menu something went wrong */
static void close_all(int failed, const char* why) {
    const int was_on = s_state != ST_OFF && s_state != ST_DEAD;
    for (int i = 0; i < MAX_PEERS; ++i) peer_close(&s_peer[i]);
    s_npeers = 0;
    close_sock(&s_listen);
    close_sock(&s_pending);
    close_sock(&s_beacon);
    close_sock(&s_udp);
    psx_modrb_disable();
    psx_mod_set_widescreen_x_margin(-1);   /* this PC's own again */
    s_failed = failed;
    s_state = ST_DEAD;
    s_view = -1;
    s_start_req = 0;
    s_join_loading = 0;
    s_slot = 0;
    s_slots = 2;
    xp_session(0, 0);
    {   /* this PC alone again */
        extern int g_psx_mod_lockstep, g_cdrom_savestate_boost_off;
        g_psx_mod_lockstep = 0;
        g_cdrom_savestate_boost_off = 0;
    }
    if (was_on || failed) say("%s (frame %u)", why, s_frame);
}

static int send_all(Peer* p, const void* data, size_t len) {
    const char* c = (const char*)data;
    while (len) {
        int n = send(p->sock, c, (int)(len > 65536 ? 65536 : len), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(0); continue; }
            return 0;
        }
        c += n; len -= (size_t)n;
    }
    return 1;
}

static int send_msg(Peer* p, uint32_t type, const void* body, uint32_t len) {
    uint32_t head[2] = { type, len };
    return send_all(p, head, sizeof head) && (!len || send_all(p, body, len));
}

/* to every other PC; 0 when a connection broke */
static int send_msg_all(uint32_t type, const void* body, uint32_t len) {
    int ok = 1;
    for (int i = 0; i < s_npeers; ++i) ok &= send_msg(&s_peer[i], type, body, len);
    return ok;
}

/* Read what has arrived; 0 when the connection is gone. */
static int pump(Peer* p) {
    for (;;) {
        if (p->rx_len + 65536 > p->rx_cap) {
            size_t cap = p->rx_cap ? p->rx_cap * 2 : 1u << 20;
            while (p->rx_len + 65536 > cap) cap *= 2;
            uint8_t* n = (uint8_t*)realloc(p->rx, cap);
            if (!n) return 0;
            p->rx = n; p->rx_cap = cap;
        }
        int n = recv(p->sock, (char*)p->rx + p->rx_len, 65536, 0);
        if (n == 0) return 0;
        if (n == SOCKET_ERROR) return WSAGetLastError() == WSAEWOULDBLOCK;
        p->rx_len += (size_t)n;
    }
}

/* the next whole message, if one has arrived (consumed by message_done) */
static int message(Peer* p, uint32_t* type, uint8_t** body, uint32_t* len) {
    if (p->rx_len < 8) return 0;
    uint32_t t, l;
    memcpy(&t, p->rx, 4); memcpy(&l, p->rx + 4, 4);
    if (p->rx_len < 8u + l) return 0;
    *type = t; *body = p->rx + 8; *len = l;
    return 1;
}

static void message_done(Peer* p, uint32_t len) {
    memmove(p->rx, p->rx + 8 + len, p->rx_len - 8 - len);
    p->rx_len -= 8 + len;
}

static void nonblocking(SOCKET s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

static Peer* peer_of_slot(int slot) {
    for (int i = 0; i < s_npeers; ++i) if (s_peer[i].slot == slot) return &s_peer[i];
    return NULL;
}

/* ---- the room announcement ---------------------------------------------- */

static void beacon_send_to(uint32_t addr_be, const Beacon* b) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((u_short)(TSR_ONLINE_PORT + 1));
    to.sin_addr.s_addr = addr_be;
    sendto(s_beacon, (const char*)b, sizeof *b, 0, (struct sockaddr*)&to, sizeof to);
}

/* every second while waiting: to every network this PC is on (a VPN such as
 * Radmin is its own network; the plain broadcast goes out on one only) */
static void beacon_tick(void) {
    if ((int)(GetTickCount() - s_next_beacon) < 0) return;   /* not yet */
    s_next_beacon = GetTickCount() + 1000;
    if (s_beacon == INVALID_SOCKET) {
        s_beacon = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_beacon == INVALID_SOCKET) return;
        BOOL on = TRUE;
        setsockopt(s_beacon, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
    }
    if (!s_game) s_game = game_code_hash();
    Beacon b;
    memset(&b, 0, sizeof b);
    b.magic = BEACON_MAGIC; b.version = VERSION; b.port = (uint32_t)s_port; b.game = s_game;
    DWORD n = sizeof b.name;
    if (!GetComputerNameA(b.name, &n)) snprintf(b.name, sizeof b.name, "Toy Story Racer");
    beacon_send_to(htonl(INADDR_BROADCAST), &b);
    beacon_send_to(htonl(INADDR_LOOPBACK), &b);   /* a second copy on this PC */

    ULONG size = 16384;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    IP_ADAPTER_ADDRESSES* list = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!list) return;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, NULL, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        free(list);
        list = (IP_ADAPTER_ADDRESSES*)malloc(size);
        if (!list) return;
        r = GetAdaptersAddresses(AF_INET, flags, NULL, list, &size);
    }
    if (r != NO_ERROR) { free(list); return; }
    for (IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            const struct sockaddr_in* in = (const struct sockaddr_in*)u->Address.lpSockaddr;
            if (in->sin_family != AF_INET) continue;
            const unsigned bits = u->OnLinkPrefixLength;
            if (bits == 0 || bits >= 31) continue;
            const uint32_t mask = 0xFFFFFFFFu << (32 - bits);
            beacon_send_to(htonl(ntohl(in->sin_addr.s_addr) | ~mask), &b);
        }
    }
    free(list);
}

/* ---- starting and ending ------------------------------------------------ */

static void start_host(int port) {
    close_all(0, "starting a new session");
    s_role = ROLE_HOST;
    s_port = port;
    s_frame = 0;
    if (!net_ready()) { close_all(1, "the network is not available"); return; }
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)s_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_listen, (struct sockaddr*)&a, sizeof a) != 0 || listen(s_listen, MAX_PEERS) != 0) {
        close_all(1, "the port is busy (another game is hosting?)");
        return;
    }
    u_long on = 1;
    ioctlsocket(s_listen, FIONBIO, &on);
    s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp != INVALID_SOCKET) {
        if (bind(s_udp, (struct sockaddr*)&a, sizeof a) != 0) close_sock(&s_udp);
        else ioctlsocket(s_udp, FIONBIO, &on);
    }
    s_next_beacon = GetTickCount();
    s_state = ST_WAIT;
    say("waiting for players on port %d", s_port);
}

static void start_join(const char* addr, int port) {
    close_all(0, "starting a new session");
    s_role = ROLE_JOIN;
    s_port = port;
    s_frame = 0;
    snprintf(s_peer_addr, sizeof s_peer_addr, "%s", addr);
    if (!net_ready()) { close_all(1, "the network is not available"); return; }
    s_connect_start = s_next_try = GetTickCount();
    s_state = ST_WAIT;
    say("looking for the host at %s:%d", s_peer_addr, s_port);
}

static void take_request(void) {
    if (!s_req) return;
    EnterCriticalSection(&s_req_lock);
    const int what = s_req;
    char addr[64];
    snprintf(addr, sizeof addr, "%s", s_req_addr);
    const int port = s_req_port;
    s_req = 0;
    LeaveCriticalSection(&s_req_lock);
    if (what == 1) start_host(port);
    else if (what == 2) start_join(addr, port);
    else close_all(0, "left the online game");
}

/* The mod options that change how the game runs: both PCs need the same,
 * or their games part (the others -- picture, HUD, sound -- may differ). */
extern int g_tsr_widescreen_on, g_tsr_files_on, g_tsr_memcard_on;
static char s_mismatch[32];   /* the option the other PC has otherwise */
static void game_settings(char* out, size_t cap) {
    static const struct { const char* label; const char* id; } opts[] = {
        { "FRAME RATE", "frame_rate" }, { "CPU OVERCLOCK", "cpu_overclock" },
        { "TRACK DETAIL", "track_detail" }, { "POLYGON POOL", "polygon_pool" },
        { "MUSIC CHANGE", "music_change" },
    };
    extern char tsr_files_voice(void);
    const char voice = g_tsr_files_on ? tsr_files_voice() : 0;
    size_t n = (size_t)snprintf(out, cap, "WIDESCREEN=%d;INSTANT LOADING=%d;INSTANT MEMORY CARD=%d;ROLLBACK=%d;VOICES=%c;",
                                g_tsr_widescreen_on, g_tsr_files_on, g_tsr_memcard_on, rb_wanted(), voice ? voice : '-');
    for (size_t i = 0; i < sizeof opts / sizeof opts[0] && n < cap; ++i) {
        char v[32] = "-";
        if (!psx_mod_option_value("toystoryracer.widescreen", "widescreen", opts[i].id, v, sizeof v)) snprintf(v, sizeof v, "-");
        n += (size_t)snprintf(out + n, cap - n, "%s=%s;", opts[i].label, v);
    }
}

/* a connection is up: say hello, with this PC's settings */
static void peer_open(Peer* p, SOCKET sock) {
    memset(&p->udp, 0, sizeof p->udp);
    p->sock = sock;
    nonblocking(sock);
    p->rx_len = 0;
    p->ready = 0;
    p->udp_ok = 0;
    p->ip = 0;
    {
        struct sockaddr_in a;
        int len = sizeof a;
        if (getpeername(sock, (struct sockaddr*)&a, &len) == 0) { p->ip = a.sin_addr.s_addr; p->udp = a; }
    }
    s_game = game_code_hash();
    uint32_t hello[3] = { MAGIC, VERSION, s_game };
    send_msg(p, MSG_HELLO, hello, sizeof hello);
    char settings[256];
    game_settings(settings, sizeof settings);
    send_msg(p, MSG_SETTINGS, settings, (uint32_t)strlen(settings) + 1u);
    {   /* this PC's widescreen margin: all use the largest (see MSG_MARGIN) */
        int32_t m = psx_mod_widescreen_x_margin();
        send_msg(p, MSG_MARGIN, &m, 4);
    }
}

/* the session's per-game values, before the first frame */
static void session_reset(void) {
    {   /* The same timing on every PC: the runtime's host enhancements that
         * make PCs part are off, as under the framework's own netplay (idle
         * skip, the sound's catch-up drop, turbo loads, FMV skip -- with idle
         * skip on, the PCs' clocks drifted a few cycles apart, the disc's
         * reads and from them the voices and the sound chip followed, and the
         * games parted); and the disc's post-load boost (applied at each host
         * present). TSR_ONLINE_HOSTENH / TSR_ONLINE_CDBOOST: tests, left on. */
        extern int g_psx_mod_lockstep, g_cdrom_savestate_boost_off;
        g_psx_mod_lockstep = !env_on("TSR_ONLINE_HOSTENH");
        g_cdrom_savestate_boost_off = !env_on("TSR_ONLINE_CDBOOST");
    }
    s_mismatch[0] = 0;
    s_pings_sent = 0;
    s_crc_next = 0;   /* no check left over from an earlier session */
    s_rb = rb_wanted();
    s_rb_confirmed = 0;
    s_last_sent = 0;
    s_rtt_est = 0.0f;
    s_loss_bonus = 0;
    s_prev_exit = 0.0;
    s_work_sum = s_work_max = 0.0;
    s_work_n = s_work_over18 = s_work_over21 = 0;
    s_stat_start = GetTickCount();
    s_stat_waits = s_stat_wait_ms = s_stat_wait_max = s_stat_udp = s_stat_frames = 0;
    s_check_mine_ok = 0;
    for (int i = 0; i < MAX_PEERS; ++i) {
        Peer* p = &s_peer[i];
        p->peer_ms = p->peer_ms_at = 0;
        p->bonus = 0; p->ahead = 0.0f;
        p->pongs = 0; p->rtt_max = 0;
        p->check_ok = 0;
        memset(p->rbc_f, 0, sizeof p->rbc_f);
    }
}

/* the players this session has: this PC's view and players 3-4 */
static void session_players(int slots, int slot) {
    s_slots = slots;
    s_slot = slot;
    s_view = slot < 2 ? slot : 1;   /* split screen: each sees their own player (3-4: below) */
    xp_session(slots, slot);
}

/* host: the players here are the session's; tell each its player */
static void session_begin(void) {
    close_sock(&s_listen);
    close_sock(&s_beacon);
    session_reset();
    int32_t margin = psx_mod_widescreen_x_margin();   /* the largest of all (MSG_MARGIN) */
    for (int i = 0; i < s_npeers; ++i) {
        uint32_t body[2] = { (uint32_t)(i + 1), (uint32_t)(s_npeers + 1) };
        s_peer[i].slot = i + 1;
        send_msg(&s_peer[i], MSG_SESSION, body, sizeof body);
        send_msg(&s_peer[i], MSG_MARGIN, &margin, 4);
    }
    session_players(s_npeers + 1, 0);
    s_state = ST_PING;
    say("%d players: starting", s_slots);
}

static void peer_drop(int i, const char* why) {
    say("a player was not let in: %s", why);
    peer_close(&s_peer[i]);
    Peer t = s_peer[i];   /* keeps its buffer */
    for (int k = i; k + 1 < s_npeers; ++k) s_peer[k] = s_peer[k + 1];
    s_peer[s_npeers - 1] = t;
    s_npeers--;
}

static const char* handle_message(Peer* p, uint32_t type, const uint8_t* body, uint32_t len);
static int main_menu_shown(void);

/* host: take the players who come, until the start */
static void host_wait(void) {
    beacon_tick();
    if (s_npeers < MAX_PEERS) {
        SOCKET c = accept(s_listen, NULL, NULL);
        if (c != INVALID_SOCKET) {
            peer_open(&s_peer[s_npeers], c);
            s_npeers++;
            say("a player joined (%d here)", s_npeers + 1);
        }
    } else {
        SOCKET c = accept(s_listen, NULL, NULL);   /* full: no more */
        if (c != INVALID_SOCKET) closesocket(c);
    }
    for (int i = 0; i < s_npeers; ++i) {
        Peer* p = &s_peer[i];
        const int alive = pump(p);
        const char* bad = NULL;
        uint32_t type, len;
        uint8_t* body;
        while (!bad && message(p, &type, &body, &len)) {
            bad = handle_message(p, type, body, len);
            message_done(p, len);
        }
        if (bad || !alive) { peer_drop(i, bad ? bad : "left"); --i; continue; }
    }
    /* the start: the host's Start, or (tests) TSR_ONLINE_PLAYERS players here */
    int ready = 0;
    for (int i = 0; i < s_npeers; ++i) ready += s_peer[i].ready;
    if (!s_npeers || ready != s_npeers) return;
    const char* want = getenv("TSR_ONLINE_PLAYERS");
    const int wanted = want && want[0] ? atoi(want) : 0;
    if (wanted >= 2 && env_on("TSR_ONLINE_START_MENU") && !main_menu_shown()) return;   /* test: from the main menu */
    if (s_start_req || (wanted >= 2 && s_npeers + 1 >= wanted) || (!wanted && s_npeers + 1 >= MAX_PLAYERS)) {
        s_start_req = 0;
        session_begin();
    }
}

/* join: connect to the host (never blocking the game: the host may be far
 * away or not there at all) */
static void try_connect(void) {
    if (s_role == ROLE_HOST) { host_wait(); return; }
    if (GetTickCount() - s_connect_start > CONNECT_MS) {
        close_all(1, "no host answered at that address");
        return;
    }
    if (s_pending == INVALID_SOCKET) {
        if ((int)(GetTickCount() - s_next_try) < 0) return;   /* not yet */
        s_next_try = GetTickCount() + 1000;
        struct addrinfo hints, *res = NULL;
        char port[16];
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port, sizeof port, "%d", s_port);
        if (getaddrinfo(s_peer_addr, port, &hints, &res) != 0 || !res) {
            close_all(1, "that address is not valid");
            return;
        }
        s_pending = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        u_long on = 1;
        ioctlsocket(s_pending, FIONBIO, &on);
        connect(s_pending, res->ai_addr, (int)res->ai_addrlen);   /* in progress */
        freeaddrinfo(res);
    }
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s_pending, &wr); FD_SET(s_pending, &ex);
    struct timeval now = { 0, 0 };
    if (select(0, NULL, &wr, &ex, &now) <= 0) return;   /* still trying */
    if (FD_ISSET(s_pending, &ex)) {   /* refused: try again in a moment */
        close_sock(&s_pending);
        return;
    }
    Peer* h = &s_peer[0];
    s_npeers = 1;
    peer_open(h, s_pending);
    s_pending = INVALID_SOCKET;
    h->slot = 0;
    h->udp.sin_port = htons((u_short)s_port);   /* the host's game port */
    h->udp_ok = h->ip != 0;
    if (s_udp == INVALID_SOCKET) s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp != INVALID_SOCKET) { u_long nb = 1; ioctlsocket(s_udp, FIONBIO, &nb); }
    session_reset();
    s_join_loading = 0;
    s_state = ST_LOAD;   /* the host's start: its player numbers, the delay, the state */
    say("connected to %s, waiting for the host to start", s_peer_addr);
}

/* CRC-32 of the PlayStation's main RAM below the frame packet buffers (each
 * PC draws its own player's view: the packets differ, the game does not read
 * them back) */
#define CHECK_END 0x80193518u
/* The checksum is spread over CHECK_PARTS frames (no hitch on the frame of
 * the check): each PC adds the same part on the same lockstep frame. */
#define CHECK_PARTS 16u

static void ram_checksum_part(void) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint32_t part = ((CHECK_END - 0x80010000u) / CHECK_PARTS + 3u) & ~3u;
    const uint32_t end = s_crc_next + part < CHECK_END ? s_crc_next + part : CHECK_END;
    uint32_t crc = s_crc;
    for (uint32_t a = s_crc_next; a < end; a += 4) {
        if (a == 0x800AC89Cu || a == 0x800A9DB0u || (a >= 0x800DE140u && a < 0x800DE540u))
            continue;   /* timer readings (the file loads' times, a count or two apart
                         * on the two PCs), not used by the race */
        if ((a >= 0x800A7E20u && a < 0x800A7F60u))
            continue;   /* the CD library's read state (0x8008C39C, 0x8008DC0C): a step
                         * apart while the disc streams, as the stream's buffers */
        if ((a >= 0x800D9000u && a < 0x800DB000u) || a == 0x800A6ED8u || a == 0x800A6EDCu)
            continue;   /* the music stream's buffers: after the state's load they alternate
                         * one step apart on the two PCs (the disc keeps streaming) */
        uint32_t w = psx_mod_read_word(a);
        for (int k = 0; k < 4; k++) crc = table[(crc ^ (w >> (8 * k))) & 0xFFu] ^ (crc >> 8);
    }
    s_crc = crc;
    s_crc_next = end < CHECK_END ? end : 0u;
}

/* a player's buttons for a frame, as they arrive (more than once: UDP and
 * TCP); the host passes them on to the other players */
static void remote_store(int slot, uint32_t frame, const PSXModPad* pad) {
    static int s_lag = -1;
    if (s_lag < 0) { const char* v = getenv("TSR_ONLINE_LAG"); s_lag = v && v[0] ? atoi(v) : 0; }
    if (slot < 0 || slot >= s_slots || slot == s_slot) return;
    if (s_have[slot][frame % RING] == frame + 1) return;   /* the first copy counts */
    s_pad[slot][frame % RING] = *pad;
    s_have[slot][frame % RING] = frame + 1;
    s_at[slot][frame % RING] = GetTickCount() + (uint32_t)s_lag;
    if (frame + 1 > s_newest[slot]) s_newest[slot] = frame + 1;
    if (s_role == ROLE_HOST && s_slots > 2) {
        PadMsg m = { frame, (uint32_t)slot, *pad };
        for (int i = 0; i < s_npeers; ++i)
            if (s_peer[i].slot != slot) send_msg(&s_peer[i], MSG_PAD, &m, sizeof m);
    }
}

/* this PC's own buttons for a frame (fixed once sent) */
static void local_store(uint32_t frame, const PSXModPad* pad) {
    s_pad[s_slot][frame % RING] = *pad;
    s_have[s_slot][frame % RING] = frame + 1;
    s_at[s_slot][frame % RING] = 0;
    if (frame + 1 > s_newest[s_slot]) s_newest[s_slot] = frame + 1;
}

static int slot_ready(int slot, uint32_t frame) {
    return s_have[slot][frame % RING] == frame + 1 &&
           (slot == s_slot || (int32_t)(GetTickCount() - s_at[slot][frame % RING]) >= 0);
}

/* every other player's buttons for the frame are here */
static int remote_ready(uint32_t frame) {
    for (int s = 0; s < s_slots; ++s)
        if (s != s_slot && !slot_ready(s, frame)) return 0;
    return 1;
}

/* The checked memory of the last two checks, kept so that a mismatch can be
 * written out (online_desync_<role>_<frame>.bin, once a session) and the two
 * PCs' files compared: by the time the other PC's checksum arrives the game
 * has moved on. */
#define RB_KEEP_WORDS ((0x80200000u - 0x80010000u) / 4u)
static uint32_t* s_rb_keep[2];
static uint32_t s_rb_keep_f[2];
static int s_rb_dumped;

static void rb_check_compare(Peer* p, uint32_t frame) {
    const uint32_t i = (frame / CHECK_EVERY) % 8u;
    if (s_rbc_mine_f[i] != frame || p->rbc_f[i] != frame || frame > s_rbc_sent) return;
    if (s_rbc_mine[i] != p->rbc[i]) {
        fprintf(stdout, "TSR ONLINE: DESYNC with player %d at frame %u (mine %08X, theirs %08X)\n", p->slot + 1, frame, s_rbc_mine[i], p->rbc[i]);
        fflush(stdout);
        const uint32_t k = (frame / CHECK_EVERY) & 1u;
        if (!s_rb_dumped && s_rb_keep[k] && s_rb_keep_f[k] == frame) {
            char path[96];
            snprintf(path, sizeof path, "online_desync_p%d_%u.bin", s_slot + 1, frame);
            FILE* df = fopen(path, "wb");
            if (df) { fwrite(s_rb_keep[k], 4, RB_KEEP_WORDS, df); fclose(df); s_rb_dumped = 1; }
            say("the games differ at frame %u: memory written to %s", frame, path);
        }
    } else if ((frame / CHECK_EVERY) % 10 == 0) {
        say("frame %u in step with player %d", frame, p->slot + 1);
    }
    p->rbc_f[i] = 0;
}

static void check_compare(Peer* p) {
    if (!s_check_mine_ok || !p->check_ok || p->check_frame != s_check_frame) return;
    p->check_ok = 0;
    if (s_check_mine == p->check) return;
    fprintf(stdout, "TSR ONLINE: DESYNC with player %d at frame %u (mine %08X, theirs %08X)\n",
            p->slot + 1, s_check_frame, s_check_mine, p->check);
    fflush(stdout);
}

/* one arrived message other than the state; NULL, or why the other PC
 * cannot play with this one */
static const char* handle_message(Peer* p, uint32_t type, const uint8_t* body, uint32_t len) {
    if (type == MSG_PAD && len == sizeof(PadMsg)) {
        PadMsg m;
        memcpy(&m, body, sizeof m);
        if (m.frame >= (s_rb ? s_rb_confirmed : s_frame) && m.frame < s_frame + RING - RB_WINDOW - 2 && !env_on("TSR_ONLINE_NOTCPPADS") &&
            (s_role == ROLE_JOIN || (int)m.slot == p->slot))   /* the host takes each player's from that player */
            remote_store((int)m.slot, m.frame, &m.pad);
    } else if (type == MSG_CHECK && len == 8) {
        uint32_t f, c;
        memcpy(&f, body, 4); memcpy(&c, body + 4, 4);
        if (s_rb) {
            p->rbc[(f / CHECK_EVERY) % 8u] = c;
            p->rbc_f[(f / CHECK_EVERY) % 8u] = f;
            rb_check_compare(p, f);
            return NULL;
        }
        p->check_frame = f; p->check = c; p->check_ok = 1;
        check_compare(p);
    } else if (type == MSG_HELLO && len == 12) {
        uint32_t h[3];
        memcpy(h, body, sizeof h);
        if (h[0] != MAGIC || h[1] != VERSION) return "the other player has a different version of the mod";
        if (h[2] != s_game) return "the other player has another version of the game (all need the same disc)";
    } else if (type == MSG_SETTINGS && len > 0 && len < 256) {
        char mine[256], theirs[256];
        game_settings(mine, sizeof mine);
        memcpy(theirs, body, len);
        theirs[len - 1] = 0;
        if (strcmp(mine, theirs) != 0) {
            /* the first entry that differs, for the message */
            const char* a = mine; const char* b = theirs;
            while (*a && *b) {
                const char* ea = strchr(a, ';'); const char* eb = strchr(b, ';');
                if (!ea || !eb) break;
                if ((ea - a) != (eb - b) || strncmp(a, b, (size_t)(ea - a)) != 0) {
                    const char* eq = strchr(a, '=');
                    snprintf(s_mismatch, sizeof s_mismatch, "%.*s", eq && eq < ea ? (int)(eq - a) : 0, a);
                    break;
                }
                a = ea + 1; b = eb + 1;
            }
            if (!s_mismatch[0]) snprintf(s_mismatch, sizeof s_mismatch, "MOD");
            fprintf(stdout, "TSR ONLINE: settings here %s, there %s\n", mine, theirs);
            return "the PCs have different mod options (they must match to play together)";
        }
        p->ready = 1;
    } else if (type == MSG_MARGIN && len == 4) {
        /* The widescreen margin widens the game's own on-screen tests (which
         * objects it moves, animates or marks visible): all the PCs must use
         * the same one, whatever their screens' shapes. The largest: the
         * widest screen still sees everything; the narrower ones draw a
         * little past their sides. The host keeps the largest of all and
         * sends it again at the start. */
        int32_t theirs, mine;
        memcpy(&theirs, body, 4);
        mine = psx_mod_widescreen_x_margin();
        if (theirs < 0 || theirs > 4096) theirs = 0;
        if (!env_on("TSR_ONLINE_OWNMARGIN"))   /* test: each PC its own margin, as before */
            psx_mod_set_widescreen_x_margin(theirs > mine ? theirs : mine);
        say("widescreen margin %d here, %d there: %d used", (int)mine, (int)theirs, (int)(theirs > mine ? theirs : mine));
    } else if (type == MSG_SESSION && len == 8 && s_role == ROLE_JOIN) {
        uint32_t b[2];
        memcpy(b, body, sizeof b);
        if (b[1] < 2u || b[1] > MAX_PLAYERS || b[0] < 1u || b[0] >= b[1]) return "the host sent a bad start";
        session_players((int)b[1], (int)b[0]);
        say("the host started: %d players, this PC is player %d", s_slots, s_slot + 1);
    } else if (type == MSG_PING && len == 4) {
        send_msg(p, MSG_PONG, body, 4);
    } else if (type == MSG_PONG && len == 4) {
        uint32_t i;
        memcpy(&i, body, 4);
        if (i < PINGS) {
            DWORD rtt = GetTickCount() - s_ping_sent[i];
            if (rtt > p->rtt_max) p->rtt_max = rtt;
            p->pongs++;
        }
    } else if (type == MSG_DELAY && len == 4) {
        uint32_t d;
        memcpy(&d, body, 4);
        if (d <= MAX_DELAY) s_delay = (int)d;
    }
    return NULL;
}

/* everything arrived from one PC, up to the state (join: left for ST_LOAD) */
static int handle_messages(Peer* p) {
    uint32_t type, len;
    uint8_t* body;
    while (message(p, &type, &body, &len)) {
        if (type == MSG_STATE) return 1;
        const char* bad = handle_message(p, type, body, len);
        if (bad) { close_all(1, bad); return 0; }
        message_done(p, len);
    }
    return 1;
}

static void udp_pump(void);

/* everything arrived from every PC; 0 when the session ended */
static int net_poll(void) {
    for (int i = 0; i < s_npeers; ++i) {
        Peer* p = &s_peer[i];
        const int alive = pump(p);
        if (!handle_messages(p)) return 0;
        if (!alive) {
            close_all(1, s_role == ROLE_HOST ? "a player left" : "the host left");
            return 0;
        }
    }
    udp_pump();
    return 1;
}
#endif

/* ---- the online menu ------------------------------------------------------
 * On the main menu (the prompt 1 PLAYER / 2 PLAYERS / OPTIONS / SAVE) a line
 * "SELECT ONLINE" is added under the prompt. Select opens the online prompt
 * in its place: Square HOST, Circle JOIN, Cross BACK. Host waits for a player
 * (Cross cancels); Join opens a window to pick a room found on the local
 * networks or type an address and port. Everything is drawn by the game's own
 * text drawer (0x80033A84), with the game's buttons and fonts; while the
 * online menu is open the game does not see this PC's buttons. */
#define LANGUAGE      0x800A9820u   /* byte: 0 EN 1 FR 2 DE 3 IT 4 ES 5 NL */
#define ROW_1PLAYER   0x8009DF00u   /* "1 PLAYER" in the six languages */
#define TEXT_FN       0x80033A84u
#define RA_LBL_BOTTOM 0x80036218u   /* the prompt's parts: labels and glyphs */
#define RA_GLY_BOTTOM 0x800362C8u
#define RA_LBL_TOP    0x8003635Cu
#define RA_GLY_TOP    0x8003640Cu
#define RA_LBL_RIGHT  0x800364A4u
#define RA_GLY_RIGHT  0x80036554u
#define RA_LBL_LEFT   0x800365ECu
#define RA_GLY_LEFT   0x800366A0u
#define BTN_SELECT    0x0001u
#define BTN_START     0x0008u
#define BTN_TRIANGLE  0x1000u
#define BTN_CIRCLE    0x2000u
#define BTN_CROSS     0x4000u
#define BTN_SQUARE    0x8000u

enum { UI_IDLE, UI_MENU, UI_HOSTING, UI_JOINING, UI_NOTICE };
enum { T_SELECT, T_HOST, T_JOIN, T_BACK, T_WAITING, T_CANCEL, T_CONNECTING, T_FAILED, T_LOST, T_SEARCH, T_SETTINGS, T_PLAY, T_WAITHOST, T_COUNT };
static const char* const s_texts[6][T_COUNT] = {
    { "SELECT  ONLINE", "HOST", "JOIN", "BACK", "WAITING FOR PLAYERS...", "CANCEL", "CONNECTING...",
      "CONNECTION FAILED", "CONNECTION LOST", "CHOOSE A ROOM...", "DIFFERENT MOD OPTION:", "PLAY", "WAITING FOR THE HOST TO START..." },
    { "SELECT  EN LIGNE", "HEBERGER", "REJOINDRE", "RETOUR", "EN ATTENTE D'UN JOUEUR...", "ANNULER",
      "CONNEXION...", "ECHEC DE LA CONNEXION", "CONNEXION PERDUE", "CHOISIS UNE PARTIE...", "OPTION DE MOD DIFFERENTE :", "JOUER", "EN ATTENTE DE L'HOTE..." },
    { "SELECT  ONLINE", "HOSTEN", "BEITRETEN", "ZURUECK", "WARTE AUF SPIELER...", "ABBRECHEN",
      "VERBINDE...", "VERBINDUNG FEHLGESCHLAGEN", "VERBINDUNG VERLOREN", "WAEHLE EIN SPIEL...", "ANDERE MOD-OPTION:", "SPIELEN", "WARTE AUF DEN HOST..." },
    { "SELECT  ONLINE", "HOST", "JOIN", "INDIETRO", "IN ATTESA DI ALTRI GIOCATORI...", "ANNULLA",
      "CONNESSIONE...", "CONNESSIONE FALLITA", "CONNESSIONE PERSA", "SCEGLI UNA STANZA...", "OPZIONE MOD DIVERSA:", "GIOCA", "IN ATTESA CHE L'HOST INIZI..." },
    { "SELECT  EN LINEA", "CREAR", "UNIRSE", "ATRAS", "ESPERANDO JUGADORES...", "CANCELAR",
      "CONECTANDO...", "CONEXION FALLIDA", "CONEXION PERDIDA", "ELIGE UNA SALA...", "OPCION DE MOD DISTINTA:", "JUGAR", "ESPERANDO AL ANFITRION..." },
    { "SELECT  ONLINE", "HOSTEN", "DEELNEMEN", "TERUG", "WACHTEN OP SPELERS...", "ANNULEREN",
      "VERBINDEN...", "VERBINDING MISLUKT", "VERBINDING VERBROKEN", "KIES EEN SPEL...", "ANDERE MOD-OPTIE:", "SPELEN", "WACHTEN OP DE HOST..." },
};

static int s_ui = UI_IDLE;
static uint32_t s_ui_frame;          /* pad hook calls */
static uint32_t s_menu_seen = 0x80000000u;   /* s_ui_frame when the main menu prompt was last drawn */
static uint32_t s_notice_until;
static int s_notice_text;
static uint16_t s_prev_buttons = 0xFFFFu;
static int s_last_status;
static int s_in_text;                /* our own call of the drawer */
static volatile int s_window_open;   /* the join window */

static int ui_lang(void) {
    int l = psx_mod_read_byte(LANGUAGE);
    return l >= 0 && l < 6 ? l : 0;
}
static const char* ui_text(int t) { return s_texts[ui_lang()][t]; }
static int main_menu_shown(void) { return s_ui_frame - s_menu_seen < 8u; }

/* join: connected, waiting for the host to start */
static int waiting_host(void) { return s_role == ROLE_JOIN && s_state == ST_LOAD && !s_join_loading; }
static int players_here(void) {
#if defined(_WIN32)
    return s_role == ROLE_HOST && s_state == ST_WAIT ? s_npeers + 1 : 0;
#else
    return 0;
#endif
}

static void notice(int text) {
    s_ui = UI_NOTICE;
    s_notice_text = text;
    s_notice_until = s_ui_frame + 240;
}

#if defined(_WIN32)
static DWORD WINAPI join_window_thread(LPVOID arg);
#endif

static void open_join_window(void) {
#if defined(_WIN32)
    if (s_window_open) return;
    s_window_open = 1;
    HANDLE t = CreateThread(NULL, 0, join_window_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else s_window_open = 0;
#endif
}

/* Buttons the online menu presses for this PC's player in the game (to the
 * character select and back), frames left. */
static uint16_t s_inject;
static int s_inject_n;
static int s_in_charsel;   /* the online menu took the game to the character select */
static void press(uint16_t button) { s_inject = button; s_inject_n = 8; }

/* Back from the character select the online menu went to (Triangle: back). */
static void back_to_menu(void) {
    if (s_in_charsel) press(BTN_TRIANGLE);
    s_in_charsel = 0;
    s_ui = UI_IDLE;
}

/* The online menu's buttons; returns 1 when this PC's buttons are the menu's
 * (the game sees none, only what the menu presses itself). */
static int ui_pads(PSXModPad pads[2]) {
    s_ui_frame++;
    const uint16_t now = pads[0].buttons;
    const uint16_t pressed = (uint16_t)(s_prev_buttons & ~now);   /* active low: 1 -> 0 */
    s_prev_buttons = now;
    const int status = tsr_online_status();
    if (status != s_last_status) {
        if (s_last_status == TSR_ONLINE_PLAYING && status == TSR_ONLINE_FAILED) notice(T_LOST);
        else if (status == TSR_ONLINE_FAILED) notice(s_mismatch[0] ? T_SETTINGS : T_FAILED);
        else if (status == TSR_ONLINE_STARTING || status == TSR_ONLINE_PLAYING) { s_ui = UI_IDLE; s_in_charsel = 0; }
        s_last_status = status;
    }
    int block = 1;
    if (waiting_host() && (pressed & BTN_CROSS)) { tsr_online_cancel(); back_to_menu(); return 1; }
    switch (s_ui) {
    case UI_IDLE:
        block = 0;
        if ((pressed & BTN_SELECT) && main_menu_shown() &&
            status != TSR_ONLINE_PLAYING && status != TSR_ONLINE_STARTING) {
            s_ui = UI_MENU;
            block = 1;
        }
        break;
    case UI_MENU:
        /* Host and Join both go to the two-player character select first:
         * there the game waits (on the main menu it starts its demo after a
         * minute), and the host's state the other PC receives starts there. */
        if (pressed & BTN_SQUARE) {
            tsr_online_host(TSR_ONLINE_PORT);
            s_ui = UI_HOSTING; press(BTN_CIRCLE); s_in_charsel = 1;
        } else if (pressed & BTN_CIRCLE) {
            open_join_window();
            s_ui = UI_JOINING; press(BTN_CIRCLE); s_in_charsel = 1;
        } else if (pressed & (BTN_CROSS | BTN_TRIANGLE | BTN_SELECT)) {
            s_ui = UI_IDLE;
        }
        break;
    case UI_HOSTING:
        if (pressed & BTN_CROSS) { tsr_online_cancel(); back_to_menu(); }
        else if ((pressed & BTN_START) && players_here() >= 2) s_start_req = 1;   /* play with the players here */
        break;
    case UI_JOINING:
        /* the window is up, or the join it started is connecting */
        if (!s_window_open && status != TSR_ONLINE_CONNECTING && status != TSR_ONLINE_STARTING) back_to_menu();
        else if (!s_window_open && (pressed & BTN_CROSS)) { tsr_online_cancel(); back_to_menu(); }
        break;
    case UI_NOTICE:
        if ((int32_t)(s_ui_frame - s_notice_until) >= 0 || (pressed & (BTN_CROSS | BTN_CIRCLE))) back_to_menu();
        break;
    }
    if (block) {
        pads[0].buttons = 0xFFFFu;
        pads[0].lx = pads[0].ly = pads[0].rx = pads[0].ry = 0x80u;
        pads[1].buttons = 0xFFFFu;
    }
    if (s_inject_n > 0) {
        pads[0].buttons = (uint16_t)~s_inject;
        s_inject_n--;
        block = 1;
    }
    return block;
}

/* A text of ours for the drawer, in the game's own text buffer: the
 * collectibles counter and the level select build their texts there right
 * before drawing them, so it is free between those calls. (The mod memory
 * was not usable: a drawer reading its text there stopped the function
 * hooks.) */
#define TEXT_BUFFER 0x800DAF98u
static uint32_t guest_text(const char* s) {
    uint32_t i = 0;
    for (; s[i] && i < 47; i++) psx_mod_write_byte(TEXT_BUFFER + i, (uint8_t)s[i]);
    psx_mod_write_byte(TEXT_BUFFER + i, 0);
    return TEXT_BUFFER;
}

/* Draw one more text with the game's drawer, with the arguments of the
 * drawer call being made now (a3 and the four stack words) at x, y. */
/* Our own lines (the hint, the waiting text): the arguments of a menu
 * label's drawer call are kept, and the lines are drawn with them at the
 * start of the next frame (0x800723A0), from a stack frame of our own --
 * never from inside the drawer's own call. */
#define FRAME_START_FN 0x800723A0u
typedef struct { char text[48]; int16_t x, y; } ExtraLine;
static ExtraLine s_lines[2];
static int s_nlines;
static uint32_t s_line_a3, s_line_args[4];
static uint32_t s_lines_frame;   /* s_ui_frame they were set in */

static void queue_line(struct CPUState* cpu, const char* text, int x, int y) {
    if (s_nlines >= 2 || (s_state != ST_OFF && s_state != ST_WAIT && s_state != ST_DEAD && !waiting_host())) return;
    ExtraLine* l = &s_lines[s_nlines++];
    snprintf(l->text, sizeof l->text, "%s", text);
    l->x = (int16_t)x; l->y = (int16_t)y;
    s_line_a3 = cpu->gpr[7];
    for (int i = 0; i < 4; i++) s_line_args[i] = psx_mod_read_word(cpu->gpr[29] + 16u + 4u * (uint32_t)i);
    s_lines_frame = s_ui_frame;
}

static void draw_lines(struct CPUState* cpu) {
    const int n = s_nlines;
    s_nlines = 0;
    if (!n || s_ui_frame - s_lines_frame > 4u) return;   /* the screen changed */
    /* never once the PCs share the game: both must run exactly the same */
    if (s_state != ST_OFF && s_state != ST_WAIT && s_state != ST_DEAD && !waiting_host()) return;
    uint32_t gpr[32], hi = cpu->hi, lo = cpu->lo;
    memcpy(gpr, cpu->gpr, sizeof gpr);
    for (int k = 0; k < n; k++) {
        const uint32_t str = guest_text(s_lines[k].text);
        if (!str) break;
        const uint32_t sp = gpr[29] - 64u;   /* a frame of ours under the caller's */
        for (int i = 0; i < 4; i++) psx_mod_write_word(sp + 16u + 4u * (uint32_t)i, s_line_args[i]);
        cpu->gpr[29] = sp;
        cpu->gpr[4] = str;
        cpu->gpr[5] = (uint32_t)(int32_t)s_lines[k].x;
        cpu->gpr[6] = (uint32_t)(int32_t)s_lines[k].y;
        cpu->gpr[7] = s_line_a3;
        cpu->gpr[31] = gpr[31];
        cpu->pc = 0;
        s_in_text = 1;
        psx_dispatch_call(cpu, TEXT_FN, gpr[31]);
        s_in_text = 0;
        if (g_psx_call_bail || cpu->pc != 0) return;   /* the guest went elsewhere: let it */
        memcpy(cpu->gpr, gpr, sizeof gpr);
        cpu->hi = hi; cpu->lo = lo;
    }
}

void tsr_online_humans_update(void);   /* players 3-4 (below) */
static void online_frame_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->pc != 0) return;
    tsr_online_humans_update();
    draw_lines(cpu);
}

/* The text drawer's entry, before it runs. Returns 1 when the element is
 * not to be drawn (the drawer is then skipped). */
static int online_text_entry(struct CPUState* cpu) {
    if (s_in_text) return 0;
    if (cpu->pc != 0) return 0;   /* the drawer resuming inside itself, not a new call */
    const uint32_t ra = cpu->gpr[31];
    if (ra < RA_LBL_BOTTOM || ra > RA_GLY_LEFT) return 0;
    if (ra == RA_LBL_BOTTOM && cpu->gpr[4] == psx_mod_read_word(ROW_1PLAYER + 4u * (uint32_t)ui_lang()))
        s_menu_seen = s_ui_frame;
    if (ra == RA_LBL_BOTTOM) { void xp_text_args_take(struct CPUState* cpu); xp_text_args_take(cpu); }   /* players 3-4's lines */
    const int status = tsr_online_status();
    const int online = status == TSR_ONLINE_PLAYING || status == TSR_ONLINE_STARTING;
    const int x = (int)(int16_t)cpu->gpr[5], y = (int)(int16_t)cpu->gpr[6];
    if (waiting_host()) {   /* joined: until the host starts */
        static uint32_t s_drawn;
        if (ra == RA_LBL_BOTTOM && s_drawn != s_ui_frame) {
            s_drawn = s_ui_frame;
            char cancel[48];
            snprintf(cancel, sizeof cancel, "x %s", ui_text(T_CANCEL));
            queue_line(cpu, ui_text(T_WAITHOST), 256, 16);
            queue_line(cpu, cancel, 256, 32);
        }
        return 0;
    }
    if (s_ui == UI_HOSTING || s_ui == UI_JOINING || s_ui == UI_NOTICE) {
        /* waiting, connecting, a notice: two lines at the bottom of the
         * screen (on the character select or still the main menu), once a
         * frame, under the first prompt drawn */
        static uint32_t s_drawn;
        if (ra == RA_LBL_BOTTOM && s_drawn != s_ui_frame) {
            s_drawn = s_ui_frame;
            const int text = s_ui == UI_HOSTING ? T_WAITING : s_ui == UI_NOTICE ? s_notice_text
                           : s_window_open ? T_SEARCH : T_CONNECTING;
            char cancel[48];
            snprintf(cancel, sizeof cancel, "x %s", ui_text(s_ui == UI_NOTICE ? T_BACK : T_CANCEL));
            char line[64];
            const int here = players_here();
            if (text == T_SETTINGS) snprintf(line, sizeof line, "%s %s", ui_text(text), s_mismatch);
            else if (text == T_WAITING && here >= 2) snprintf(line, sizeof line, "%s %d/4", ui_text(text), here);
            else snprintf(line, sizeof line, "%s", ui_text(text));
            if (text == T_WAITING && here >= 2)   /* players here: Start plays */
                snprintf(cancel, sizeof cancel, "START %s   x %s", ui_text(T_PLAY), ui_text(T_CANCEL));
            queue_line(cpu, line, 256, 16);
            queue_line(cpu, cancel, 256, 32);
        }
        return 0;
    }
    if (!main_menu_shown()) return 0;
    switch (s_ui) {
    case UI_IDLE:
        /* the main menu as it is, one line more under it */
        if (ra == RA_LBL_BOTTOM && !online) queue_line(cpu, ui_text(T_SELECT), x, y + 16);
        return 0;
    case UI_MENU:   /* the online prompt in place of the main menu's */
        switch (ra) {
        case RA_LBL_LEFT: cpu->gpr[4] = guest_text(ui_text(T_HOST)); return 0;
        case RA_LBL_RIGHT: cpu->gpr[4] = guest_text(ui_text(T_JOIN)); return 0;
        case RA_LBL_BOTTOM: cpu->gpr[4] = guest_text(ui_text(T_BACK)); return 0;
        case RA_LBL_TOP: cpu->gpr[4] = guest_text("ONLINE"); return 0;
        case RA_GLY_TOP: return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

#if defined(_WIN32)
/* ---- the join window ------------------------------------------------------ */
enum { ID_LIST = 101, ID_ADDR, ID_PORT, ID_JOIN, ID_CANCEL, ID_TIMER };
static HWND s_list, s_addr_edit, s_port_edit;
static TsrOnlineRoom s_shown[16];
static int s_shown_n = -1;

static void list_refresh(void) {
    TsrOnlineRoom rooms[16];
    memset(rooms, 0, sizeof rooms);
    const int n = tsr_online_search_poll(rooms, 16);
    if (n == s_shown_n && !memcmp(rooms, s_shown, sizeof(TsrOnlineRoom) * (size_t)n)) return;
    const int sel = (int)SendMessageA(s_list, LB_GETCURSEL, 0, 0);
    SendMessageA(s_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) {
        char line[128];
        snprintf(line, sizeof line, "%s   (%s:%d)", rooms[i].name, rooms[i].addr, rooms[i].port);
        SendMessageA(s_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) SendMessageA(s_list, LB_SETCURSEL, (WPARAM)sel, 0);
    memcpy(s_shown, rooms, sizeof rooms);
    s_shown_n = n;
}

static LRESULT CALLBACK join_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        list_refresh();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == ID_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            const int i = (int)SendMessageA(s_list, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < s_shown_n) {
                char port[16];
                snprintf(port, sizeof port, "%d", s_shown[i].port);
                SetWindowTextA(s_addr_edit, s_shown[i].addr);
                SetWindowTextA(s_port_edit, port);
            }
            return 0;
        }
        if (id == ID_JOIN || id == IDOK || (id == ID_LIST && HIWORD(wp) == LBN_DBLCLK)) {
            if (id == ID_LIST) SendMessageA(w, WM_COMMAND, MAKEWPARAM(ID_LIST, LBN_SELCHANGE), 0);
            char addr[64], port[16];
            GetWindowTextA(s_addr_edit, addr, sizeof addr);
            GetWindowTextA(s_port_edit, port, sizeof port);
            char* a = addr;
            while (*a == ' ') a++;
            if (!*a) { SetFocus(s_addr_edit); return 0; }
            tsr_online_join(a, atoi(port) > 0 ? atoi(port) : TSR_ONLINE_PORT);
            DestroyWindow(w);
            return 0;
        }
        if (id == ID_CANCEL || id == IDCANCEL) { DestroyWindow(w); return 0; }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        KillTimer(w, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static DWORD WINAPI join_window_thread(LPVOID arg) {
    (void)arg;
    const int it = ui_lang() == 3;
    const char* title = it ? "Toy Story Racer - Unisciti a una partita online" : "Toy Story Racer - Join an online game";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = join_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "TsrOnlineJoin";
    RegisterClassA(&wc);   /* fails harmlessly the second time */
    RECT r = { 0, 0, 460, 330 };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowExA(WS_EX_TOPMOST, "TsrOnlineJoin", title, style,
                             (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                             ww, wh, NULL, NULL, wc.hInstance, NULL);
    if (!w) { s_window_open = 0; return 0; }
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND c;
#define CTL(cls, text, st, x, y, cw, ch, id) \
    (c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | (st), x, y, cw, ch, w, (HMENU)(INT_PTR)(id), wc.hInstance, NULL), \
     SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE), c)
    CTL("STATIC", it ? "Stanze trovate sulle reti di questo PC (LAN, Radmin, Hamachi...):"
                     : "Rooms found on this PC's networks (LAN, Radmin, Hamachi...):", 0, 12, 10, 436, 18, 0);
    s_list = CTL("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 12, 30, 436, 150, ID_LIST);
    CTL("STATIC", it ? "Oppure scrivi l'indirizzo IP dell'host e la porta:"
                     : "Or type the host's IP address and port:", 0, 12, 192, 436, 18, 0);
    CTL("STATIC", it ? "Indirizzo IP:" : "IP address:", 0, 12, 216, 90, 18, 0);
    s_addr_edit = CTL("EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 104, 213, 200, 22, ID_ADDR);
    CTL("STATIC", it ? "Porta:" : "Port:", 0, 316, 216, 40, 18, 0);
    char port[16];
    snprintf(port, sizeof port, "%d", TSR_ONLINE_PORT);
    s_port_edit = CTL("EDIT", port, WS_BORDER | ES_NUMBER | WS_TABSTOP, 358, 213, 90, 22, ID_PORT);
    CTL("STATIC", it ? "Chi fa da host deve aprire questa porta (TCP e UDP) sul router; con Radmin non serve."
                     : "The host must open this port (TCP and UDP) on the router; not needed with Radmin.", 0, 12, 244, 436, 30, 0);
    CTL("BUTTON", it ? "Entra" : "Join", BS_DEFPUSHBUTTON | WS_TABSTOP, 238, 284, 100, 30, ID_JOIN);
    CTL("BUTTON", it ? "Annulla" : "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 348, 284, 100, 30, ID_CANCEL);
#undef CTL
    s_shown_n = -1;
    tsr_online_search_start();
    list_refresh();
    SetTimer(w, ID_TIMER, 500, NULL);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(s_list);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    tsr_online_search_stop();
    s_window_open = 0;
    return 0;
}
#endif

#if defined(_WIN32)
/* one player's last UDP_FRAMES frames of buttons, to one PC */
static void udp_send_slot(Peer* to, int slot) {
    const uint32_t newest1 = slot == s_slot ? s_last_sent + 1u : s_newest[slot];
    if (!newest1) return;
    UdpPads u;
    memset(&u, 0, sizeof u);
    u.magic = UDP_MAGIC;
    u.slot = (uint32_t)slot;
    u.newest = newest1 - 1u;
    u.sent_ms = GetTickCount();
    u.echo_ms = to->peer_ms;
    u.hold_ms = to->peer_ms ? GetTickCount() - to->peer_ms_at : 0;
    /* rollback play: how far this PC runs past the confirmed frames (x16),
     * so the one further ahead can let the others catch up */
    u.ask_lead = s_rb ? (0x80000000u | (uint32_t)(s_rb_ahead_avg * 16.0f)) : (uint32_t)s_loss_bonus;
    for (int i = 0; i < UDP_FRAMES; ++i) {
        const uint32_t fr = u.newest - (uint32_t)(UDP_FRAMES - 1 - i);
        if ((int32_t)fr < 0 || s_have[slot][fr % RING] != fr + 1) continue;
        u.pad[i] = s_pad[slot][fr % RING];
        u.have |= 1u << i;
    }
    sendto(s_udp, (const char*)&u, sizeof u, 0, (const struct sockaddr*)&to->udp, sizeof to->udp);
}

/* this PC's buttons to the host; the host: every player's to every other */
static void udp_send(void) {
    if (s_udp == INVALID_SOCKET) return;
    for (int i = 0; i < s_npeers; ++i) {
        Peer* p = &s_peer[i];
        if (!p->udp_ok) continue;
        if (s_role == ROLE_JOIN) { udp_send_slot(p, s_slot); continue; }
        for (int s = 0; s < s_slots; ++s)
            if (s != p->slot) udp_send_slot(p, s);
    }
}

static void udp_pump(void) {
    if (s_udp == INVALID_SOCKET) return;
    for (;;) {
        UdpPads u;
        struct sockaddr_in from;
        int fl = sizeof from;
        const int n = recvfrom(s_udp, (char*)&u, sizeof u, 0, (struct sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) return;
        if (n != (int)sizeof u || u.magic != UDP_MAGIC || u.slot >= MAX_PLAYERS) continue;
        {   /* test: a bad network (TSR_ONLINE_UDPDROP = percent of packets lost) */
            static int s_drop = -1;
            if (s_drop < 0) { const char* v = getenv("TSR_ONLINE_UDPDROP"); s_drop = v && v[0] ? atoi(v) : 0; }
            if (s_drop > 0 && rand() % 100 < s_drop) continue;
        }
        Peer* p;
        if (s_role == ROLE_HOST) {
            /* each player sends its own buttons; the first packet names its
             * address (through its router), which must be its TCP one */
            p = peer_of_slot((int)u.slot);
            if (!p || from.sin_addr.s_addr != p->ip) continue;
            p->udp = from;
            p->udp_ok = 1;
        } else {
            if (!s_npeers) continue;
            p = &s_peer[0];
            if (from.sin_addr.s_addr != p->ip) continue;
        }
        s_stat_udp++;
        if (u.ask_lead & 0x80000000u) p->ahead = (float)(u.ask_lead & 0xFFFFu) / 16.0f;
        else if (u.ask_lead <= 8u) p->bonus = (int)u.ask_lead;
        if (u.echo_ms) {   /* a round trip: now - our time - the time it waited there */
            const uint32_t rtt = GetTickCount() - u.echo_ms - u.hold_ms;
            if (rtt < 5000u && (float)rtt > s_rtt_est) s_rtt_est = (float)rtt;
        }
        if (u.sent_ms - p->peer_ms < 0x80000000u || !p->peer_ms) { p->peer_ms = u.sent_ms; p->peer_ms_at = GetTickCount(); }
        for (int i = 0; i < UDP_FRAMES; ++i) {
            if (!(u.have & (1u << i))) continue;
            const uint32_t fr = u.newest - (uint32_t)(UDP_FRAMES - 1 - i);
            if ((int32_t)fr < 0 || fr < (s_rb ? s_rb_confirmed : s_frame) || fr >= s_frame + RING - RB_WINDOW - 2) continue;
            remote_store((int)u.slot, fr, &u.pad[i]);
        }
    }
}

/* how many frames ahead this PC sends its buttons: half a round trip, plus
 * margin; up at once when the network gets slower, down slowly */
static void adapt_delay(void) {
    if (s_delay_fixed) { s_send_delay = s_delay; return; }
    s_rtt_est *= 0.995f;   /* the worst recent round trip, fading in ~10 s */
    if (s_loss_bonus > 0 && (int32_t)(GetTickCount() - s_loss_bonus_drop_at) >= 0) {
        s_loss_bonus--;
        s_loss_bonus_drop_at = GetTickCount() + 5000;
    }
    int peer_bonus = 0;
    for (int i = 0; i < s_npeers; ++i) if (s_peer[i].bonus > peer_bonus) peer_bonus = s_peer[i].bonus;
    int want = (int)((s_rtt_est * 0.5f + 10.0f) / (float)FRAME_MS + 0.999f) + 2 + peer_bonus;
    if (s_slots > 2) want += 1;   /* through the host: one hop more */
    if (want < MIN_DELAY) want = MIN_DELAY;
    if (want > MAX_DELAY) want = MAX_DELAY;
    if (want > s_send_delay) {
        s_send_delay = want;
        s_delay_drop_at = GetTickCount() + 3000;
    } else if (want < s_send_delay && (int32_t)(GetTickCount() - s_delay_drop_at) >= 0) {
        s_send_delay--;
        s_delay_drop_at = GetTickCount() + 3000;
    }
}

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void stats_tick(void) {
    s_stat_frames++;
    if (GetTickCount() - s_stat_start < 10000u) return;
    if (s_work_n)
        say("  this PC per frame (without waiting): average %.1f ms, worst %.1f ms, %u frames over 18 ms (60 Hz), %u over 21 ms (50 Hz)",
            s_work_sum / s_work_n, s_work_max, s_work_over18, s_work_over21);
    int peer_bonus = 0;
    for (int i = 0; i < s_npeers; ++i) if (s_peer[i].bonus > peer_bonus) peer_bonus = s_peer[i].bonus;
    say("last 10 s: %u frames, waited %u times (%u ms, longest %u ms), round trip %.0f ms, buttons %d frames late (others ask %d more), %u UDP packets",
        s_stat_frames, s_stat_waits, s_stat_wait_ms, s_stat_wait_max, s_rtt_est, s_send_delay, peer_bonus, s_stat_udp);
    s_stat_start = GetTickCount();
    s_stat_waits = s_stat_wait_ms = s_stat_wait_max = s_stat_udp = s_stat_frames = 0;
    s_work_sum = s_work_max = 0.0;
    s_work_n = s_work_over18 = s_work_over21 = 0;
}

/* wait for every other player's buttons of a frame (resending ours now and
 * then: another PC may be waiting for them, and a lost packet must not leave
 * both waiting); 0 when the session ended */
static int wait_remote(uint32_t frame) {
    const DWORD t0 = GetTickCount();
    int resent = 0;
    while (!remote_ready(frame)) {
        if (!net_poll()) return 0;
        if (remote_ready(frame)) break;
        if (GetTickCount() - t0 > 10000) {
            int who = 0;
            for (int s = 0; s < s_slots; ++s) if (s != s_slot && !slot_ready(s, frame)) { who = s + 1; break; }
            char why[64];
            snprintf(why, sizeof why, "player %d stopped answering", who);
            close_all(1, why);
            return 0;
        }
        if (GetTickCount() - t0 >= (DWORD)(2 + 8 * resent)) { resent++; udp_send(); }
        Sleep(0);
    }
    return 1;
}
#endif

static void neutral(PSXModPad* p) {
    p->buttons = 0xFFFFu;
    p->lx = p->ly = p->rx = p->ry = 0x80u;
    p->analog = 0; p->connected = 1;
}

/* the machine went back: tick T holds the frames below T, frame T is next */
static void rb_loaded(uint32_t tick) {
    extern void tsr_native_scene_on_rollback(void);
    s_frame = tick;
    tsr_native_scene_on_rollback();
}

/* Everything the console's pad reply carries: the analog flag too (a guess
 * kept digital where the real pad was analog gives the game another pad type
 * and its pad buffers part, with no rollback to correct them). */
static int pad_same(const PSXModPad* a, const PSXModPad* b) {
    return a->buttons == b->buttons && a->lx == b->lx && a->ly == b->ly && a->rx == b->rx && a->ry == b->ry &&
           a->analog == b->analog;
}

static uint32_t rb_crc(void) {
    uint32_t h = 2166136261u;
    for (uint32_t a = 0x80010000u; a < CHECK_END; a += 4) {
        if (a == 0x800AC89Cu || a == 0x800A9DB0u || (a >= 0x800DE140u && a < 0x800DE540u)) continue;
        if ((a >= 0x800A7E20u && a < 0x800A7F60u)) continue;
        if ((a >= 0x800D9000u && a < 0x800DB000u) || a == 0x800A6ED8u || a == 0x800A6EDCu) continue;
        h = (h ^ psx_mod_read_word(a)) * 16777619u;
    }
    return h;
}

/* the game's buttons of a frame: ports 1 and 2 for players 1 and 2, players
 * 3-4's kept for their karts (xp_buttons) */
#define XP_PADS 0x800AA0C4u   /* players 3-4's buttons this frame (game's form, 2 halfwords) */
static void frame_pads(PSXModPad pads[2], const PSXModPad* all) {
    pads[0] = all[0]; pads[0].connected = 1;
    pads[1] = all[1]; pads[1].connected = 1;
    /* players 3-4's go into game RAM the game does not use (beside the
     * human count 0x800AA0C0, checked unused), where the karts' driver hook
     * reads them: so a rollback's snapshots hold them as they hold the
     * ports' buttons, and a replayed frame drives with the same */
    for (int i = 0; i < 2; ++i) {
        const uint16_t b = 2 + i < s_slots ? (uint16_t)~all[2 + i].buttons : 0u;
        if (psx_mod_read_half(XP_PADS + 2u * (uint32_t)i) != b) psx_mod_write_half(XP_PADS + 2u * (uint32_t)i, b);
    }
}

#if defined(_WIN32)
/* Confirm the frames whose real buttons are all here and equal the ones
 * played; returns the first frame played with other buttons, or 0xFFFFFFFF. */
static uint32_t rb_confirm(void) {
    while (s_rb_confirmed < s_frame) {
        const uint32_t g = s_rb_confirmed;
        if (!remote_ready(g)) break;
        for (int s = 0; s < s_slots; ++s) {
            if (s == s_slot) continue;
            if (s_used_tag[g % RING] == g + 1 && !pad_same(&s_used[s][g % RING], &s_pad[s][g % RING])) return g;
        }
        for (int s = 0; s < s_slots; ++s)
            if (s != s_slot && g >= s_last_frame[s]) { s_last[s] = s_pad[s][g % RING]; s_last_frame[s] = g; }
        s_rb_confirmed++;
    }
    return 0xFFFFFFFFu;
}

static void rb_step(PSXModPad pads[2], PSXModPad mine) {
    if (s_frame == 0 && !psx_modrb_enabled()) {
        psx_modrb_enable(rb_loaded);
        s_rb_confirmed = 0;
        memset(s_used_tag, 0, sizeof s_used_tag);
        for (int s = 0; s < MAX_PLAYERS; ++s) { neutral(&s_last[s]); s_last_frame[s] = 0; }
        s_rb_rollbacks = s_rb_deepest = s_rb_stalls = s_rb_guessed = s_rb_slowed = 0;
        s_rb_ahead_avg = 0.0f;
        s_rb_dumped = 0;
        memset(s_rbc_mine_f, 0, sizeof s_rbc_mine_f);
        for (int i = 0; i < MAX_PEERS; ++i) { memset(s_peer[i].rbc_f, 0, sizeof s_peer[i].rbc_f); s_peer[i].ahead = 0.0f; }
        s_rbc_sent = 0;
        for (int s = 0; s < MAX_PLAYERS; ++s)
            for (int i = 0; i < s_delay; i++) s_at[s][i % RING] = GetTickCount();
        say("rollback play, %d players (own buttons %d frame late, window %d frames)", s_slots, RB_LOCAL_DELAY, RB_WINDOW);
    }
    const int resim = psx_modrb_resimulating();
    if (!net_poll()) return;
    if (!resim) {   /* this player's buttons, fixed once sent */
        const uint32_t ahead = s_frame + (uint32_t)RB_LOCAL_DELAY;
        while ((int32_t)(ahead - s_last_sent) > 0) {
            s_last_sent++;
            local_store(s_last_sent, &mine);
            PadMsg m = { s_last_sent, (uint32_t)s_slot, mine };
            if (!send_msg_all(MSG_PAD, &m, sizeof m)) { close_all(1, "the connection broke"); return; }
        }
        udp_send();
    }
    uint32_t bad = rb_confirm();
    /* too far ahead of what the others confirmed: wait, as lockstep does */
    if (!resim && bad == 0xFFFFFFFFu && s_frame - s_rb_confirmed >= (uint32_t)RB_WINDOW) {
        s_rb_stalls++;
        if (!wait_remote(s_rb_confirmed)) return;
        bad = rb_confirm();
    }
    if (!resim) {
        /* All the PCs see about the same network delay, so the one that is
         * further past its confirmed frames is simply ahead in time: it gives
         * the slowest a little of each frame to catch up (otherwise it would
         * run into the window and stop in jolts). */
        s_rb_ahead_avg = s_rb_ahead_avg * 0.9f + (float)(s_frame - s_rb_confirmed) * 0.1f;
        float peer_ahead = 1e9f;
        for (int i = 0; i < s_npeers; ++i) if (s_peer[i].ahead < peer_ahead) peer_ahead = s_peer[i].ahead;
        if (!s_npeers) peer_ahead = s_rb_ahead_avg;
        const float lead = (s_rb_ahead_avg - peer_ahead) * 0.5f;   /* frames */
        if (lead >= 0.75f && s_frame > 120u) {
            extern void psx_mod_pacer_shift_ms(double ms);
            psx_mod_pacer_shift_ms(lead > 4.0f ? 2.0 : (double)lead * 0.5);   /* the frame cadence, half a ms per frame of lead */
            s_rb_slowed++;
        }
    }
    if (!resim && bad != 0xFFFFFFFFu) {
        /* frame `bad` was played with a wrong guess: back to the tick that
         * holds the frames below it, at the end of this boundary */
        /* (a tick without a snapshot: from the newest one before it; the
         * frames in between are replayed with the buttons already confirmed) */
        const uint32_t from = psx_modrb_snapshot_at_or_before(bad);
        if (!from || s_frame - from >= (uint32_t)RING - 4u || !psx_modrb_rewind(from)) {
            PsxModRbStats st;
            psx_modrb_get_stats(&st);
            say("rollback refused: frame %u wrong, now %u, tick %u, snapshots %u (failed %u), loads %u (failed %u), held %d",
                bad, s_frame, psx_modrb_tick(), (unsigned)st.saves, (unsigned)st.save_failures, (unsigned)st.loads,
                (unsigned)st.load_failures, psx_modrb_can_rewind(bad));
            close_all(1, "the game could not go back far enough");
            return;
        }
        s_rb_rollbacks++;
        if (s_frame - from > s_rb_deepest) s_rb_deepest = s_frame - from;
    }
    /* this frame's buttons */
    const uint32_t f = s_frame;
    /* A guess must be one the machine can take back: with no snapshot at or
     * before this frame yet (the first frames of the session), wait for the
     * real buttons as lockstep does. */
    if (!resim && !remote_ready(f) && !psx_modrb_snapshot_at_or_before(f)) {
        if (!wait_remote(f)) return;
    }
    PSXModPad all[MAX_PLAYERS];
    int guessed = 0;
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        if (s >= s_slots) { neutral(&all[s]); continue; }
        if (s == s_slot) { all[s] = s_pad[s][f % RING]; continue; }
        if (slot_ready(s, f)) {
            all[s] = s_pad[s][f % RING];
            if (f >= s_last_frame[s]) { s_last[s] = all[s]; s_last_frame[s] = f; }
        } else {
            all[s] = s_last[s];
            guessed = 1;
        }
        s_used[s][f % RING] = all[s];
    }
    if (guessed && !resim) s_rb_guessed++;
    s_used_tag[f % RING] = f + 1;
    frame_pads(pads, all);

    /* the checksum of the game before frame f, sent once every frame below f
     * is confirmed (a replay through f computes it again) */
    if (f && f % CHECK_EVERY == 0) {
        if (env_on("TSR_ONLINE_DUMP")) {   /* test: the RAM of each check (a replay writes it again) */
            char path[96];
            snprintf(path, sizeof path, "online_ram_p%d_%u.bin", s_slot + 1, f);
            FILE* df = fopen(path, "wb");
            if (df) {
                for (uint32_t a = 0x80000000u; a < 0x80200000u; a += 4) { uint32_t w = psx_mod_read_word(a); fwrite(&w, 4, 1, df); }
                fclose(df);
            }
        }
        {
            const uint32_t k = (f / CHECK_EVERY) & 1u;
            if (!s_rb_keep[k]) s_rb_keep[k] = (uint32_t*)malloc(RB_KEEP_WORDS * 4u);
            if (s_rb_keep[k]) {
                for (uint32_t w = 0; w < RB_KEEP_WORDS; ++w) s_rb_keep[k][w] = psx_mod_read_word(0x80010000u + w * 4u);
                s_rb_keep_f[k] = f;
            }
        }
        s_rbc_mine[(f / CHECK_EVERY) % 8u] = rb_crc();
        s_rbc_mine_f[(f / CHECK_EVERY) % 8u] = f;
    }
    {
        const uint32_t next = s_rbc_sent + CHECK_EVERY;
        const uint32_t i = (next / CHECK_EVERY) % 8u;
        if (!resim && s_rbc_mine_f[i] == next && s_rb_confirmed >= next) {
            uint32_t body[2] = { next, s_rbc_mine[i] };
            send_msg_all(MSG_CHECK, body, sizeof body);
            s_rbc_sent = next;
            for (int k = 0; k < s_npeers; ++k) rb_check_compare(&s_peer[k], next);
        }
    }
    if (!resim && f && f % 600u == 0u) {
        float peer_ahead = 0.0f;
        for (int i = 0; i < s_npeers; ++i) if (s_peer[i].ahead > peer_ahead) peer_ahead = s_peer[i].ahead;
        say("rollback: %u rollbacks so far (deepest %u frames), %u frames guessed, %u waits, %u frames slowed; %.1f frames ahead of the confirmed ones (others up to %.1f)",
            s_rb_rollbacks, s_rb_deepest, s_rb_guessed, s_rb_stalls, s_rb_slowed, s_rb_ahead_avg, peer_ahead);
    }
    s_frame++;
}
#endif

/* TEST (TSR_RB_TEST="N,K"): in the scripted race, every N boundaries rewind K
 * ticks and replay them with the same pads; the game-relevant RAM at every
 * replayed boundary must equal what the live run had there. */
static uint32_t rb_test_crc(void) {
    uint32_t h = 2166136261u;
    for (uint32_t a = 0x80010000u; a < CHECK_END; a += 4) {
        if (a == 0x800AC89Cu || a == 0x800A9DB0u || (a >= 0x800DE140u && a < 0x800DE540u)) continue;
        if ((a >= 0x800A7E20u && a < 0x800A7F60u)) continue;
        if ((a >= 0x800D9000u && a < 0x800DB000u) || a == 0x800A6ED8u || a == 0x800A6EDCu) continue;
        h = (h ^ psx_mod_read_word(a)) * 16777619u;
    }
    return h;
}
static void rb_test(unsigned since_race) {
    static int n = -1, k;
    static uint32_t crc[64], crc_tick[64];
    static unsigned rewinds, compared, mismatched, refused, live_since;
    if (n < 0) {
        const char* e = getenv("TSR_RB_TEST");
        n = 0;
        if (e && e[0]) { n = atoi(e); const char* c = strchr(e, ','); k = c ? atoi(c + 1) : 4; }
    }
    if (!n || (since_race < 700u && !env_on("TSR_RB_TEST_MENU"))) return;
    if (!psx_modrb_enabled()) { extern void tsr_native_scene_on_rollback(void); psx_modrb_enable((void (*)(uint32_t))(void*)tsr_native_scene_on_rollback); return; }
    const uint32_t done = psx_modrb_tick();          /* the RAM now: the end of this tick */
    const uint32_t c = rb_test_crc();
    /* the live RAM of the last 16 ticks, to name what a replay changed */
    #define RBT_WORDS ((CHECK_END - 0x80010000u) / 4u)
    static uint32_t* s_ram[16];
    static uint32_t s_ram_tick[16];
    uint32_t* cur = NULL;
    if (!psx_modrb_resimulating()) {
        uint32_t k = done % 16u;
        if (!s_ram[k]) s_ram[k] = (uint32_t*)malloc(RBT_WORDS * 4u);
        if (s_ram[k]) {
            for (uint32_t w = 0; w < RBT_WORDS; ++w) s_ram[k][w] = psx_mod_read_word(0x80010000u + w * 4u);
            s_ram_tick[k] = done;
        }
    } else if (s_ram[done % 16u] && s_ram_tick[done % 16u] == done) {
        cur = s_ram[done % 16u];
    }
    if (psx_modrb_resimulating() && crc_tick[done % 64u] == done) {
        compared++;
        if (crc[done % 64u] != c) {
            mismatched++;
            if (mismatched <= 8u) {
                fprintf(stdout, "TSR RB TEST: MISMATCH at tick %u (live %08X, replay %08X)\n", done, crc[done % 64u], c);
                if (cur) {
                    unsigned shown = 0;
                    for (uint32_t w = 0; w < RBT_WORDS && shown < 16u; ++w) {
                        const uint32_t a = 0x80010000u + w * 4u, v = psx_mod_read_word(a);
                        if (v != cur[w]) { fprintf(stdout, "  %08X: live %08X replay %08X\n", a, cur[w], v); shown++; }
                    }
                }
                fflush(stdout);
            }
        }
    }
    crc[done % 64u] = c;
    crc_tick[done % 64u] = done;
    if (!psx_modrb_resimulating() && ++live_since >= (unsigned)n && done > (uint32_t)k + 2u) {
        live_since = 0;
        if (psx_modrb_rewind(done - (uint32_t)k)) rewinds++; else refused++;
    }
    if (rewinds && rewinds % 20u == 0u && live_since == 1u) {
        PsxModRbStats st;
        psx_modrb_get_stats(&st);
        fprintf(stdout, "TSR RB TEST: %u rewinds (%u refused), %u boundaries compared, %u mismatched; snapshots %llu avg %.2f ms, loads %llu avg %.2f ms, %llu frames replayed\n",
                rewinds, refused, compared, mismatched, (unsigned long long)st.saves, st.saves ? st.save_ms / (double)st.saves : 0.0,
                (unsigned long long)st.loads, st.loads ? st.load_ms / (double)st.loads : 0.0, (unsigned long long)st.resim_frames);
        fflush(stdout);
    }
}

static int s_xp_fake4;   /* test (TSR_XP_FAKE4): this PC alone plays as a four-player session's player 1 */
#define XP_PADS_ADDR 0x800AA0C4u
static void online_pads(PSXModPad pads[2]) {
#if defined(_WIN32)
    {   /* test (TSR_AUTORACE): from the main menu into a two-player race by
         * itself, the same buttons on the same frames every run -- Circle on
         * the main menu (two players), then Cross from each pad in turn
         * through the character and track select; in the race pad 1 holds
         * Cross (accelerate). No save state involved. */
        static int s_auto = -1, s_stage;
        static unsigned s_calls, s_menu_calls, s_race_at;
        if (s_auto < 0) s_auto = env_on("TSR_AUTORACE");
        if (s_auto) {
            s_calls++;
            { extern int g_snap_bench_req; if (s_stage == 3) g_snap_bench_req = 1; }   /* test: PSX_SNAP_BENCH */
            if (s_stage < 3 && s_calls > 900u && env_on("TSR_RB_TEST_MENU")) rb_test(700u);
            if (s_stage == 0) {
                /* up to the main menu: Start, then Triangle, in turn (the
                 * title and the screen after it), none once the menu shows */
                if (main_menu_shown()) s_menu_calls++;
                else {
                    const unsigned ph = s_calls % 120u;
                    s_menu_calls = 0;
                    if (s_calls > 200u && ph < 6u) pads[0].buttons = (uint16_t)~BTN_START;
                    if (s_calls > 200u && ph >= 60u && ph < 66u) pads[0].buttons = (uint16_t)~BTN_TRIANGLE;
                }
                if (s_menu_calls >= 120u) { s_stage = 1; s_menu_calls = 0; }
            } else if (s_stage == 1) {
                pads[0].buttons = (uint16_t)~BTN_CIRCLE;
                if (++s_menu_calls >= 8u) { s_stage = 2; s_menu_calls = 0; }
            } else if (s_stage == 2) {
                const unsigned ph = s_menu_calls++ % 60u;
                if (ph >= 30u && ph < 36u) pads[0].buttons = (uint16_t)~BTN_CROSS;
                if (ph >= 45u && ph < 51u) pads[1].buttons = (uint16_t)~BTN_CROSS;
                if (psx_mod_read_byte(0x800A9D34u)) {   /* EU: the two-player race is on */
                    s_stage = 3; s_race_at = s_calls;
                    {   /* measure 1000 frames, from 1200 frames on (the kart holding Cross by then) */
                        extern void psx_runtime_perf_bench_arm(uint32_t after, uint32_t frames);
                        psx_runtime_perf_bench_arm(1200u, 1000u);
                    }
                    fprintf(stdout, "TSR TEST: race on at pad call %u\n", s_calls);
                    fflush(stdout);
                }
            } else {
                if (s_calls - s_race_at > 500u) pads[0].buttons = (uint16_t)~BTN_CROSS;
                rb_test(s_calls - s_race_at);
                {   /* test (TSR_DUMPAT="N,file"): the game's memory N calls into the race */
                    const char* e = getenv("TSR_DUMPAT");
                    if (e && e[0] && (s_calls - s_race_at) % (unsigned)atoi(e) == 0u) {
                        const char* fn = strchr(e, ',');
                        char path[300];
                        if (fn) snprintf(path, sizeof path, "%s_%05u.bin", fn + 1, s_calls - s_race_at);
                        FILE* df = fn ? fopen(path, "wb") : NULL;
                        if (df) {
                            for (uint32_t a = 0x80000000u; a < 0x80200000u; a += 4) { uint32_t w = psx_mod_read_word(a); fwrite(&w, 4, 1, df); }
                            fclose(df);
                        }
                    }
                }
            }
        }
    }
    {   /* test (TSR_RB_TEST): a replayed frame gets the buttons the frame it
         * replaces had (the scripted buttons follow a host counter); players
         * 3-4 of a pretend session (TSR_XP_FAKE4) hold Cross */
        static PSXModPad s_rec[64][2];
        static uint32_t s_rec_tick[64];
        if (getenv("TSR_RB_TEST") && psx_modrb_enabled()) {
            const uint32_t t = psx_modrb_tick();
            if (!psx_modrb_resimulating()) { s_rec[t % 64u][0] = pads[0]; s_rec[t % 64u][1] = pads[1]; s_rec_tick[t % 64u] = t + 1u; }
            else if (s_rec_tick[t % 64u] == t + 1u) { pads[0] = s_rec[t % 64u][0]; pads[1] = s_rec[t % 64u][1]; }
        }
        if (s_xp_fake4) {
            if (psx_mod_read_half(XP_PADS_ADDR) != 0x4000u) psx_mod_write_half(XP_PADS_ADDR, 0x4000u);
            if (psx_mod_read_half(XP_PADS_ADDR + 2u) != 0x4000u) psx_mod_write_half(XP_PADS_ADDR + 2u, 0x4000u);
        }
    }
    take_request();
    if (s_state != ST_PLAY) ui_pads(pads);   /* the online menu may have the buttons */
    /* this PC's player: the pad on port 1 */
    PSXModPad mine = pads[0];
    mine.connected = 1;
    {   /* test (TSR_ONLINE_AUTOPAD, on a PC that joined): its player presses
         * Cross now and then (the character select); players 3-4 hold it and
         * steer left and right by turns */
        static int s_autopad = -1;
        static uint32_t s_n;
        if (s_autopad < 0) s_autopad = env_on("TSR_ONLINE_AUTOPAD");
        if (s_autopad && s_state == ST_PLAY) {
            s_n++;
            if (s_slot >= 2) {
                uint16_t b = BTN_CROSS;
                if ((s_n / 90u + (uint32_t)s_slot) & 1u) b |= s_slot == 2 ? 0x0080u : 0x0020u;   /* left / right */
                mine.buttons = (uint16_t)~b;
            } else if (s_n % 60u < 6u) {
                mine.buttons = (uint16_t)~BTN_CROSS;
            }
        }
    }
    switch (s_state) {
    case ST_WAIT:
        try_connect();
        return;
    case ST_PING: {   /* host: how far away are the other players? */
        if (!net_poll()) return;   /* what arrived first (another PC's reason, when it leaves) */
        if (s_pings_sent < PINGS) {
            uint32_t i = (uint32_t)s_pings_sent;
            s_ping_sent[i] = GetTickCount();
            send_msg_all(MSG_PING, &i, 4);
            s_pings_sent++;
            return;
        }
        DWORD rtt_max = 0;
        for (int i = 0; i < s_npeers; ++i) {
            if (s_peer[i].pongs < PINGS) {
                if (GetTickCount() - s_ping_sent[0] > 10000) close_all(1, "a player does not answer");
                return;
            }
            if (s_peer[i].rtt_max > rtt_max) rtt_max = s_peer[i].rtt_max;
        }
        if (!s_delay_fixed) {
            int d = (int)((rtt_max / 2 + FRAME_MS - 1) / FRAME_MS) + 2 + (s_slots > 2 ? 1 : 0);
            s_delay = d < MIN_DELAY ? MIN_DELAY : d > MAX_DELAY ? MAX_DELAY : d;
        }
        uint32_t d = (uint32_t)s_delay;
        send_msg_all(MSG_DELAY, &d, 4);
        say("ping %u ms: buttons %u frames late", (unsigned)rtt_max, d);
        s_state = ST_SAVE;
        return;
    }
    case ST_SAVE:   /* host: the state the game continues from on every PC */
        if (!psx_mod_state_save(STATE_SLOT)) { close_all(1, "could not save the state"); return; }
        s_state = ST_SEND;
        pads[0] = mine; neutral(&pads[1]);   /* this frame belongs to no one */
        return;
    case ST_SEND: {
        if (psx_mod_state_pending()) { neutral(&pads[1]); return; }
        uint8_t* data = NULL;
        size_t size = 0;
        if (!psx_mod_state_read(STATE_SLOT, &data, &size) || !send_msg_all(MSG_STATE, data, (uint32_t)size)) {
            free(data); close_all(1, "could not send the state"); return;
        }
        /* The host loads the state it sent, as the other PCs do: what a
         * state does not hold (the disc's and the sound chip's progress
         * inside their current work) then restarts the same on every PC. */
        if (!psx_mod_state_load(data, size)) { free(data); close_all(1, "could not reload the state"); return; }
        free(data);
        say("state sent (%u bytes)", (unsigned)size);
        s_state = ST_RELOAD;
        return;
    }
    case ST_RELOAD:   /* host: its own load of the state sent */
        if (psx_mod_state_pending()) return;
        say("state reloaded, playing (buttons %d frames late)", s_delay);
        s_state = ST_PLAY;
        s_frame = 0;
        break;   /* the first frame after the load: lockstep frame 0, as on the other PCs */
    case ST_LOAD: {   /* join: wait for the host's start and state, load it */
        if (s_join_loading) {
            if (psx_mod_state_pending()) return;
            s_join_loading = 0;
            s_state = ST_PLAY;
            s_frame = 0;
            say("state loaded, playing (buttons %d frames late)", s_delay);
            break;   /* the first frame after the load: lockstep frame 0 */
        }
        if (!net_poll()) return;   /* hello, settings, the start, pings, the delay */
        Peer* h = &s_peer[0];
        uint32_t type, len;
        uint8_t* body;
        if (!message(h, &type, &body, &len) || type != MSG_STATE) return;
        if (!psx_mod_state_load(body, len)) { close_all(1, "could not load the state"); return; }
        s_join_loading = 1;
        message_done(h, len);
        return;
    }
    case ST_PLAY:
        break;
    default:
        return;
    }

    /* lockstep: send this player's buttons for frame + delay, play frame
     * with every player's buttons of frame (neutral before the delay) */
    if (s_frame == 0 && !(s_rb && psx_modrb_enabled())) {
        if (s_rb) s_delay = RB_LOCAL_DELAY;
        memset(s_have, 0, sizeof s_have);
        memset(s_newest, 0, sizeof s_newest);
        for (int s = 0; s < MAX_PLAYERS; ++s)
            for (int i = 0; i < s_delay; i++) {   /* the first frames: nobody presses */
                neutral(&s_pad[s][i % RING]);
                s_have[s][i % RING] = (uint32_t)i + 1;
                s_at[s][i % RING] = 0;
                s_newest[s] = (uint32_t)s_delay;
            }
        s_last_sent = (uint32_t)s_delay - 1u;
        s_send_delay = s_delay;
        s_delay_drop_at = GetTickCount() + 3000;
    }
    if (s_rb) { rb_step(pads, mine); return; }
    /* this player's buttons for the frames up to frame + delay not sent yet
     * (the delay grew: the frames in between get the same buttons; it
     * shrank: nothing new until frame + delay passes what was sent). Each
     * frame's buttons are fixed once sent, so every PC plays the same. */
    {
        const double t = now_ms();
        if (s_prev_exit > 0.0 && s_frame > 1) {
            const double w = t - s_prev_exit;
            s_work_sum += w; s_work_n++;
            if (w > s_work_max) s_work_max = w;
            if (w > 18.0) s_work_over18++;
            if (w > 21.0) s_work_over21++;
        }
    }
    udp_pump();
    adapt_delay();
    const uint32_t ahead = s_frame + (uint32_t)s_send_delay;
    while ((int32_t)(ahead - s_last_sent) > 0) {
        s_last_sent++;
        local_store(s_last_sent, &mine);
        PadMsg m = { s_last_sent, (uint32_t)s_slot, mine };
        if (!send_msg_all(MSG_PAD, &m, sizeof m)) { close_all(1, "the connection broke"); return; }
    }
    udp_send();
    const DWORD wait_start = GetTickCount();
    if (!wait_remote(s_frame)) return;
    {
        const uint32_t w = GetTickCount() - wait_start;
        if (w >= 2) {
            /* waited: another PC's packets were lost or late -- ask (in ours)
             * for a frame further ahead for a while */
            if (s_loss_bonus < 4) s_loss_bonus++;
            s_loss_bonus_drop_at = GetTickCount() + 5000;
            s_stat_waits++;
            s_stat_wait_ms += w;
            if (w > s_stat_wait_max) s_stat_wait_max = w;
        }
        stats_tick();
        s_prev_exit = now_ms();
    }
    {
        PSXModPad all[MAX_PLAYERS];
        for (int s = 0; s < MAX_PLAYERS; ++s) {
            if (s < s_slots) all[s] = s_pad[s][s_frame % RING];
            else neutral(&all[s]);
        }
        frame_pads(pads, all);
    }

    if (s_frame && s_frame % CHECK_EVERY == 0) {   /* a check starts */
        s_crc = 0xFFFFFFFFu;
        s_crc_next = 0x80010000u;   /* above the BIOS kernel */
        s_crc_frame = s_frame;
        if (env_on("TSR_ONLINE_DUMP")) {   /* test: the RAM of each check, to compare */
            char path[96];
            snprintf(path, sizeof path, "online_ram_p%d_%u.bin", s_slot + 1, s_frame);
            FILE* f = fopen(path, "wb");
            if (f) {
                for (uint32_t a = 0x80000000u; a < 0x80200000u; a += 4) {
                    uint32_t w = psx_mod_read_word(a);
                    fwrite(&w, 4, 1, f);
                }
                fclose(f);
            }
        }
    }
    if (s_crc_next) {
        ram_checksum_part();
        if (!s_crc_next) {   /* the last part: compare */
            s_check_frame = s_crc_frame;
            s_check_mine = ~s_crc;
            s_check_mine_ok = 1;
            uint32_t body[2] = { s_crc_frame, s_check_mine };
            send_msg_all(MSG_CHECK, body, sizeof body);
            for (int k = 0; k < s_npeers; ++k) check_compare(&s_peer[k]);
            if ((s_crc_frame / CHECK_EVERY) % 10 == 0) say("frame %u in step", s_crc_frame);
        }
    }
    s_frame++;
#else
    (void)pads;
#endif
}

/* ---- the room search (the join window's thread) -------------------------- */

#if defined(_WIN32)
static SOCKET s_search = INVALID_SOCKET;
static struct { TsrOnlineRoom room; DWORD seen; } s_rooms[16];
static int s_room_count;
static uint32_t s_search_game;
#endif

int tsr_online_search_start(void) {
#if defined(_WIN32)
    if (s_search != INVALID_SOCKET) return 1;
    if (!net_ready()) return 0;
    s_search = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_search == INVALID_SOCKET) return 0;
    BOOL on = TRUE;
    setsockopt(s_search, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(TSR_ONLINE_PORT + 1);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_search, (struct sockaddr*)&a, sizeof a) != 0) { close_sock(&s_search); return 0; }
    u_long nb = 1;
    ioctlsocket(s_search, FIONBIO, &nb);
    s_room_count = 0;
    s_search_game = game_code_hash();
    return 1;
#else
    return 0;
#endif
}

int tsr_online_search_poll(TsrOnlineRoom* rooms, int max) {
#if defined(_WIN32)
    if (s_search == INVALID_SOCKET) return 0;
    for (;;) {
        Beacon b;
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(s_search, (char*)&b, sizeof b, 0, (struct sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) break;
        if (n != (int)sizeof b || b.magic != BEACON_MAGIC || b.version != VERSION || b.game != s_search_game) continue;
        char addr[64];
        inet_ntop(AF_INET, &from.sin_addr, addr, sizeof addr);
        b.name[sizeof b.name - 1] = 0;
        /* one room per host: the same beacon reaches this PC on several networks */
        int i;
        for (i = 0; i < s_room_count; i++)
            if (!strcmp(s_rooms[i].room.name, b.name) && s_rooms[i].room.port == (int)b.port) break;
        if (i == s_room_count) {
            if (s_room_count == 16) continue;
            s_room_count++;
            snprintf(s_rooms[i].room.addr, sizeof s_rooms[i].room.addr, "%s", addr);
        }
        s_rooms[i].room.port = (int)b.port;
        snprintf(s_rooms[i].room.name, sizeof s_rooms[i].room.name, "%s", b.name);
        s_rooms[i].seen = GetTickCount();
    }
    /* a host announces itself every second: gone after a few silent ones */
    for (int i = 0; i < s_room_count;)
        if (GetTickCount() - s_rooms[i].seen > 3500) s_rooms[i] = s_rooms[--s_room_count];
        else i++;
    int n = 0;
    for (int i = 0; i < s_room_count && n < max; i++) rooms[n++] = s_rooms[i].room;
    return n;
#else
    (void)rooms; (void)max;
    return 0;
#endif
}

void tsr_online_search_stop(void) {
#if defined(_WIN32)
    close_sock(&s_search);
    s_room_count = 0;
#endif
}

/* ---- this PC's player's view ---------------------------------------------
 * The game draws both players' views of the split screen, each into its own
 * half of the screen (its drawing area: rows 0-127 the host's player, rows
 * 128-255 the other); this PC shows only its own player's. Nothing of the
 * game changes: the other view is computed as usual and only not drawn, so
 * both PCs keep running the same game. */
#define SPLIT_SCREEN  0x800A9D34u   /* byte, nonzero in two-player races */

static int xp_view_kart(void);
static int s_xp_drawing;
int tsr_online_local_view(void) {
    if (xp_view_kart() >= 0) return 1;   /* players 3-4: drawn as the bottom half (below) */
    return s_view >= 0 && psx_mod_read_byte(SPLIT_SCREEN) ? s_view : -1;
}

/* The two views' rectangles: pointers to x0, y0, x1, y1, centre x, centre y
 * (halfwords); 0-127 and 128-255 in PAL, 0-119 and 120-239 in NTSC. */
#define VIEW_TABLE    0x800A97DCu

void tsr_online_list_hook(void) {
    if (env_on("TSR_MUTEALL")) psx_mod_set_list_mute(1);   /* test: the game's drawing skipped */
    { extern int psx_modrb_resimulating(void); if (psx_modrb_resimulating()) psx_mod_set_list_mute(1); }   /* replayed frames are not drawn */
    int view = tsr_online_local_view();
    if (xp_view_kart() >= 0 && !s_xp_drawing) {   /* players 3-4: only the view drawn for them */
        psx_mod_set_list_mute(1);
        return;
    }
    if (view < 0) {
        psx_mod_set_hidden_area(-1, -1);
        psx_mod_set_area_remap(-1, -1, -1, -1, 0);
        psx_mod_set_hidden_tile(0, 0, 0, 0);
        return;
    }
    const uint32_t mine = psx_mod_read_word(VIEW_TABLE + 4u * (uint32_t)view);
    const uint32_t other = psx_mod_read_word(VIEW_TABLE + 4u * (uint32_t)(view ^ 1));
    if ((mine >> 24) != 0x80u || (other >> 24) != 0x80u) return;
    const int top = (int16_t)psx_mod_read_half(mine + 2u), bottom = (int16_t)psx_mod_read_half(mine + 6u);
    const int cy = (int16_t)psx_mod_read_half(mine + 10u);
    const int o_top = (int16_t)psx_mod_read_half(other + 2u), o_bottom = (int16_t)psx_mod_read_half(other + 6u);
    const int screen_bottom = bottom > o_bottom ? bottom : o_bottom;
    const int boundary = view == 0 ? o_top : top;   /* the bottom view's first row */
    /* the split screen's divider: no split shown */
    psx_mod_set_hidden_tile(0, boundary - 2, 512, 4);
    /* the other view is not drawn; this one fills the screen, its centre
     * moved to the screen's */
    psx_mod_set_hidden_area(o_top, o_bottom);
    psx_mod_set_area_remap(top, bottom, 0, screen_bottom, (screen_bottom + 1) / 2 - cy);
}

static void online_text_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (online_text_entry(cpu)) psx_mod_finish_function();
}

/* ---- players 3 and 4 --------------------------------------------------------
 *
 * A two-player race has four karts: 0 and 1 driven by the players, 2 and 3 by
 * the game. The race loop (0x80073484) gives every kart its buttons and
 * applies them (0x800673E4): a kart with byte +0x317 bit 0 set gets them from
 * the game's driver 0x800861CC(kart), the others from pad table 0x800A9D80
 * (two entries). Players 3 and 4 drive karts 2 and 3 by answering for that
 * driver: the hook returns their buttons instead of the game's choice.
 */
#define XP_KARTS        0x800C1478u
#define XP_KART_STRIDE  812u
#define XP_KART_COUNT   0x800A9E90u
#define XP_AI_FN        0x800861CCu
static int s_xp_players = -1;      /* extra players (0..2); -1 = read TSR_EXTRA_PLAYERS */
static uint32_t s_xp_frames;

/* The game's button word for extra player i this frame (game bits are the
 * pad's, set when pressed). Test: hold Cross, steer left/right by turns. */
static int s_xp_online;            /* an online session has players 3-4: theirs from the network */
static uint16_t xp_buttons(int i) {
    if (s_xp_online) return psx_mod_read_half(0x800AA0C4u + 2u * (uint32_t)i);   /* this frame's (frame_pads) */
    uint16_t b = 0x4000u;   /* test: Cross (accelerate), steer left/right by turns */
    if ((s_xp_frames / 90u + (uint32_t)i) & 1u) b |= i ? 0x0020u : 0x0080u;   /* right / left */
    return b;
}

static int s_xp_view = -2;            /* kart this PC shows (2, 3), -1 none; -2 = read TSR_XP_VIEW */

/* An online session starts (slots players, this PC is player slot + 1) or
 * ends (0): players 3-4 are the session's, else the test settings. */
static void xp_session(int slots, int slot) {
    if (slots > 2) {
        s_xp_online = 1;
        s_xp_players = slots - 2;
        s_xp_view = slot >= 2 ? slot : -1;
    } else {
        s_xp_online = 0;
        s_xp_players = -1;   /* read the environment again */
        s_xp_view = -2;
    }
}

static int xp_players(void) {
    if (s_xp_players < 0) {
        const char* e = getenv("TSR_EXTRA_PLAYERS");
        s_xp_players = e && e[0] ? atoi(e) : 0;
        if (s_xp_players > 2) s_xp_players = 2;
    }
    return s_xp_players;
}

/* The game tells its players' karts from its own by kart index (+0x90) <
 * player count (0x800A97E4). Six of those tests (game.toml "humans-*"
 * patches: start boost, antenna, HUD rope, a kart flag at the start) read
 * this byte instead, kept every frame = player count, plus players 3-4 in a
 * two-player race. Game RAM the game itself never uses (checked through
 * boot, menus and a race), so it is saved with the rest for rollback. */
#define XP_HUMANS 0x800AA0C0u
void tsr_online_humans_update(void) {
    uint8_t n = psx_mod_read_byte(0x800A97E4u);
    if (psx_mod_read_byte(SPLIT_SCREEN) && n == 2u && xp_players() > 0) n = (uint8_t)(2 + xp_players());
    if (psx_mod_read_byte(XP_HUMANS) != n) psx_mod_write_byte(XP_HUMANS, n);
}

static void xp_ai_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (xp_players() <= 0) return;
    const uint32_t kart = cpu->gpr[4];
    if (kart < XP_KARTS || (kart - XP_KARTS) % XP_KART_STRIDE) return;
    const uint32_t k = (kart - XP_KARTS) / XP_KART_STRIDE;
    if (k < 2u || k >= 2u + (uint32_t)s_xp_players || k >= psx_mod_read_byte(XP_KART_COUNT)) return;
    if (k == 2u) s_xp_frames++;
    cpu->gpr[2] = xp_buttons((int)k - 2);   /* v0: this kart's buttons */    psx_mod_finish_function();
}

/* ---- players 3 and 4: their view ------------------------------------------
 *
 * The game keeps two cameras (0x800C2F20, 0x88 bytes each, updated by
 * 0x8006B394(camera, kart)) and draws two views (0x80072BE4). A PC whose
 * player drives kart 2 or 3 draws that kart's view itself, in a sandbox
 * (psx_mod_sandbox_call): right after the game sent its frame to the GPU
 * (DrawOTag 0x8008A958 in the flip 0x8005D920, then the pad decode 0x8005E638
 * returning to 0x8005D9F0) it moves its own copy of a camera behind the kart,
 * redraws view 0 from it and sends that; then every byte of the game is put
 * back. The game's own lists are not shown on that PC. The camera copy is
 * this PC's alone: it only decides what this PC shows.
 */
#define XP_FLIP_NEXT_FN 0x8005E638u   /* called by the flip right after DrawOTag */
#define XP_FLIP_RA      0x8005D9F0u
#define XP_CAMS         0x800C2F20u
#define XP_CAM_SIZE     0x88u
#define XP_CAM_FN       0x8006B394u
#define XP_CUR_CAM      0x800A9EC8u   /* word: the camera being drawn */
#define XP_CUR_AREA     0x800A9EC4u   /* byte: the current-area slot being drawn */
#define XP_AREA_TBL     0x800A9CACu   /* halfword per slot (2) */
#define XP_VIEW_INDEX   0x800A9C38u
#define XP_VIEW_SETUP   0x8004AA50u
#define XP_VIEW_ENV0    0x80063540u
#define XP_VIEW_ENV1    0x800635C8u
#define XP_DRAWENV_FN   0x800636D8u
#define XP_DRAWENVS     0x800A97D4u   /* word per view */
#define XP_BG_FN        0x800615CCu
#define XP_RENDER_FN    0x8004820Cu
#define XP_CLEAR_OT_FN  0x8008A850u
#define XP_FULL_ENV_FN  0x80063650u   /* the whole screen's environment, near the OT's end */
#define XP_HUD_FN       0x8003B8B4u
#define XP_BUF          0x800A9E34u   /* the frame's buffer */
#define XP_PRIM_PTR     0x800A9F4Cu   /* gp+0x10FC: next primitive */
static uint8_t s_xp_cam[XP_CAM_SIZE]; /* this PC's camera behind it */
static uint16_t s_xp_area;            /* the camera's current area */
static int s_xp_cam_ok;
static uint32_t s_xp_refused;

static int xp_view_kart(void) {
    extern int g_tsr_racing;   /* the race bar drawn lately (widescreen plugin) */
    if (s_xp_view == -2) { const char* e = getenv("TSR_XP_VIEW"); s_xp_view = e && e[0] ? atoi(e) : -1; }
    /* a two-player race's level (the front end is level 18-19: its split
     * flag stays set after a two-player race), from its start: the grid's
     * presentation and the countdown too (the widescreen plugin's racing
     * flag waits for the HUD: player 2's view was shown until the start) */
    if (s_xp_view < 2 || !psx_mod_read_byte(SPLIT_SCREEN) || psx_mod_read_word(0x800AA00Cu) - 18u < 2u) return -1;
    if ((uint32_t)s_xp_view >= psx_mod_read_byte(XP_KART_COUNT)) return -1;
    return s_xp_view;
}

static void xp_call(struct CPUState* cpu, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    cpu->gpr[4] = a0; cpu->gpr[5] = a1; cpu->gpr[6] = a2;
    cpu->gpr[31] = XP_FLIP_RA;
    cpu->pc = 0;
    psx_dispatch_call(cpu, fn, XP_FLIP_RA);
}

/* In the sandbox: the camera moved behind kart k, the frame's drawing
 * restarted and view 0 (the bottom half) drawn from that camera, then sent. */
static int xp_view_pass(struct CPUState* cpu, void* user) {
    const uint32_t k = (uint32_t)(uintptr_t)user;
    const uint32_t cam = XP_CAMS + XP_CAM_SIZE;   /* camera 1, which view 0 draws */
    cpu->gpr[29] -= 0x400u;                       /* below the interrupted code's frame */
    if (!s_xp_cam_ok) {                           /* start from the game's camera there */
        for (uint32_t i = 0; i < XP_CAM_SIZE; ++i) s_xp_cam[i] = psx_mod_read_byte(cam + i);
        s_xp_area = psx_mod_read_half(XP_AREA_TBL + 2u);
        s_xp_cam_ok = 1;
    }
    for (uint32_t i = 0; i < XP_CAM_SIZE; ++i) psx_mod_write_byte(cam + i, s_xp_cam[i]);
    xp_call(cpu, XP_CAM_FN, cam, XP_KARTS + k * XP_KART_STRIDE, 0);
    for (uint32_t i = 0; i < XP_CAM_SIZE; ++i) s_xp_cam[i] = psx_mod_read_byte(cam + i);
    const uint32_t buf = psx_mod_read_word(XP_BUF);
    /* the frame's drawing restarted, as 0x800491DC does (without its GPU sync) */
    xp_call(cpu, XP_CLEAR_OT_FN, buf + 0x2B4u, 0x824u, 0);
    psx_mod_write_word(XP_PRIM_PTR, buf + 0x2504u);
    psx_mod_write_word(0x800A9DF8u, buf + 0x118C4u);              /* primitives' end */
    psx_mod_write_word(0x800A9DCCu, buf == 0x80193518u ? 0x801A57D0u : 0x801D768Cu);
    psx_mod_write_word(0x800A9DE4u, buf == 0x80193518u ? 0x801D768Cu : 0x801A57D0u);
    psx_mod_write_half(0x800A9BB4u, 0xFFFFu);
    psx_mod_write_word(0x800A9B10u, buf + 0x2C4u);
    { extern void tsr_native_scene_new_frame(void); tsr_native_scene_new_frame(); }
    /* as the game draws view 0 (0x80072C68..0x80072CC4); slot 1 of the
     * current-area table follows this camera (the portal walk 0x8004754C
     * moves it from area to area) */
    psx_mod_write_byte(XP_VIEW_INDEX, 0);
    psx_mod_write_byte(XP_CUR_AREA, 1);
    psx_mod_write_half(XP_AREA_TBL + 2u, s_xp_area);
    psx_mod_write_word(XP_CUR_CAM, cam);
    xp_call(cpu, XP_VIEW_SETUP, cam, 0, 0);   /* a0: the camera */
    psx_mod_write_byte(XP_VIEW_INDEX, 0);
    xp_call(cpu, XP_VIEW_ENV0, 0, 0, 0);      /* the top half's environment, at the OT head */
    xp_call(cpu, XP_VIEW_ENV1, 0, 0, 0);      /* the bottom half's (view 0), halfway down */
    xp_call(cpu, XP_DRAWENV_FN, psx_mod_read_word(XP_DRAWENVS), 0, 0);
    xp_call(cpu, XP_BG_FN, 0, 0, 0xFFu);
    xp_call(cpu, XP_RENDER_FN, 0, 0, 0);
    s_xp_area = psx_mod_read_half(XP_AREA_TBL + 2u);
    /* the HUD, as the game draws it after the views (0x80072D20): the bottom
     * half's player's part (kart 1) shows kart k, whose data is swapped into
     * kart 1's place for it (all of it put back when the sandbox ends) */
    {
        const uint32_t a = XP_KARTS + XP_KART_STRIDE, b = XP_KARTS + k * XP_KART_STRIDE;
        for (uint32_t i = 0; i < XP_KART_STRIDE; i += 4) {
            const uint32_t t = psx_mod_read_word(a + i);
            psx_mod_write_word(a + i, psx_mod_read_word(b + i));
            psx_mod_write_word(b + i, t);
        }
    }
    xp_call(cpu, XP_FULL_ENV_FN, 0, 0, 0);
    xp_call(cpu, XP_HUD_FN, 0, 0, 0);
    /* sent straight to the GPU's DMA: libgpu's DrawOTag would queue it
     * behind the game's frame, a queue that only moves on an interrupt */
    s_xp_drawing = 1;
    psx_mod_write_word(0x1F801814u, 0x04000002u);           /* GP1(04): DMA CPU -> GP0 */
    psx_mod_write_word(0x1F8010A0u, (buf + 0x2340u) & 0xFFFFFFu);
    psx_mod_write_word(0x1F8010A4u, 0);
    psx_mod_write_word(0x1F8010A8u, 0x01000401u);           /* linked list, start */
    s_xp_drawing = 0;
    return 1;
}

/* ---- players 3 and 4: their characters ------------------------------------
 *
 * On the two-player character select (its cursors move in 0x8006E63C, every
 * frame; 0x8006F9BC then writes players 1-2's characters to 0x800A97F0, from
 * the cursors through the select's order 0x800A592C) players 3-4 pick theirs with
 * Left/Right (Triangle: back to the game's own pick). The picks live in game
 * RAM the game does not use, changed only from inside the game's own select
 * function, so every PC makes the same ones (and a rollback replays them).
 * Karts 3-4 get their players' picks when the race inits them (below). The picks
 * are shown under the select by every PC, drawn in a sandbox after the
 * game's frame (the game's text drawer, its state put back). */
#define XP_PICK       0x800AA0C8u   /* byte per player 3-4: character + 1, 0 = the game's own */
#define XP_PREV       0x800AA0BCu   /* halfword per player 3-4: the buttons the select saw last */
#define XP_KART_INIT  0x800667E8u
#define XP_CHARSEL    0x8006E63Cu   /* a player's cursor on the select (also the pause menu: level test) */
#define XP_P_CHARS    0x800A97F0u   /* players 1-2's characters */
#define XP_NAMES      0x8009D1B0u   /* six name pointers (languages) per character */
#define XP_NCHARS     12
#define XP_CHAR_SKIP  10            /* never drawn by the game's own pick either */
static uint32_t s_xp_flips;                        /* frames shown (the flip hook) */
static uint32_t s_xp_charsel_seen = 0x80000000u;   /* s_xp_flips of the last select frame */
static uint32_t s_xp_text_a3, s_xp_text_args[4];  /* a menu text's drawer arguments */
static int s_xp_text_ok;

static int xp_char_taken(int c, int i) {   /* by players 1-2 or the other extra player */
    if (c == (int)psx_mod_read_byte(XP_P_CHARS) || c == (int)psx_mod_read_byte(XP_P_CHARS + 1u)) return 1;
    for (int j = 0; j < xp_players(); ++j)
        if (j != i && psx_mod_read_byte(XP_PICK + (uint32_t)j) == (uint8_t)(c + 1)) return 1;
    return 0;
}

static void xp_charsel_hook(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!s_xp_online) return;
    /* the front end (level 18-19: the menus' house), not a race (it runs there too) */
    if (psx_mod_read_word(0x800AA00Cu) - 18u >= 2u) return;
    s_xp_charsel_seen = s_xp_flips;
    for (int i = 0; i < xp_players(); ++i) {
        const uint16_t b = psx_mod_read_half(XP_PADS + 2u * (uint32_t)i);
        const uint16_t prev = psx_mod_read_half(XP_PREV + 2u * (uint32_t)i);
        const uint16_t press = (uint16_t)(b & ~prev);
        if (b != prev) psx_mod_write_half(XP_PREV + 2u * (uint32_t)i, b);
        int pick = (int)psx_mod_read_byte(XP_PICK + (uint32_t)i) - 1;
        if (press & BTN_TRIANGLE) pick = -1;
        const int dir = (press & 0x0020u) ? 1 : (press & 0x0080u) ? -1 : 0;   /* Right / Left */
        if (dir) {
            int c = pick < 0 ? (dir > 0 ? -1 : XP_NCHARS) : pick;
            for (int n = 0; n < XP_NCHARS; ++n) {
                c = (c + dir + XP_NCHARS) % XP_NCHARS;
                if (c != XP_CHAR_SKIP && !xp_char_taken(c, i)) { pick = c; break; }
            }
        }
        if ((uint8_t)(pick + 1) != psx_mod_read_byte(XP_PICK + (uint32_t)i))
            psx_mod_write_byte(XP_PICK + (uint32_t)i, (uint8_t)(pick + 1));
    }
}

/* Each kart's character is its init's argument (0x800667E8(kart, character,
 * index, flags); the race's setup 0x8008329C draws the game's own karts' at
 * random right there, and the init loads the model): karts 3-4 get their
 * players' picks; a kart of the game's own that drew a picked character
 * gets a free one instead. */
static void xp_kart_init_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_xp_online || cpu->pc != 0) return;
    const uint32_t kart = cpu->gpr[4], k = cpu->gpr[6] & 0xFFFFu;
    if (k < 2u || k > 3u || kart != XP_KARTS + k * XP_KART_STRIDE) return;
    const int was = (int)(int8_t)cpu->gpr[5];
    int c = was;
    const int i = (int)k - 2;
    if (i < xp_players()) {   /* a player's kart: the pick, if still free */
        const int pick = (int)psx_mod_read_byte(XP_PICK + (uint32_t)i) - 1;
        if (pick >= 0 && pick < XP_NCHARS && !xp_char_taken(pick, i)) c = pick;
    } else {                  /* the game's own: not a character a player picked */
        int clash = 0;
        for (int j = 0; j < xp_players(); ++j)
            if ((int)psx_mod_read_byte(XP_PICK + (uint32_t)j) - 1 == c) clash = 1;
        if (clash)
            for (int n = 0; n < XP_NCHARS; ++n) {
                int used = n == XP_CHAR_SKIP || xp_char_taken(n, -1);
                for (uint32_t q = 0; q < k && !used; ++q)   /* the karts before it */
                    used = (int)(int8_t)psx_mod_read_byte(XP_KARTS + q * XP_KART_STRIDE + 0x91u) == n;
                if (!used) { c = n; break; }
            }
    }
    if (!psx_modrb_resimulating()) say("players 3-4: kart %u gets character %d (the game drew %d)", k + 1u, c, was);
    cpu->gpr[5] = (uint32_t)c;
}

/* the drawer's arguments of a menu text (font, colour...), for our lines */
void xp_text_args_take(struct CPUState* cpu) {
    s_xp_text_a3 = cpu->gpr[7];
    for (int i = 0; i < 4; i++) s_xp_text_args[i] = psx_mod_read_word(cpu->gpr[29] + 16u + 4u * (uint32_t)i);
    s_xp_text_ok = 1;
}

static void xp_text(struct CPUState* cpu, const char* s, int x, int y) {
    const uint32_t str = guest_text(s);
    const uint32_t sp = cpu->gpr[29];
    for (int i = 0; i < 4; i++) psx_mod_write_word(sp + 16u + 4u * (uint32_t)i, s_xp_text_args[i]);
    cpu->gpr[7] = s_xp_text_a3;
    xp_call(cpu, TEXT_FN, str, (uint32_t)x, (uint32_t)y);
}

static void xp_char_name(int c, char* out, size_t cap) {
    out[0] = 0;
    if (c < 0 || c >= XP_NCHARS) return;
    const uint32_t p = psx_mod_read_word(XP_NAMES + 24u * (uint32_t)c + 4u * (uint32_t)ui_lang());
    size_t n = 0;
    for (; n + 1 < cap; ++n) {
        const uint8_t ch = psx_mod_read_byte(p + (uint32_t)n);
        if (!ch) break;
        out[n] = (char)ch;
    }
    out[n] = 0;
}

/* In the sandbox: players 3-4's picks, drawn over the select's frame. */
static int xp_charsel_pass(struct CPUState* cpu, void* user) {
    (void)user;
    static const char* const label[6] = { "PLAYER", "JOUEUR", "SPIELER", "GIOCATORE", "JUGADOR", "SPELER" };
    static const char* const random_[6] = { "RANDOM", "AU HASARD", "ZUFALL", "CASUALE", "AL AZAR", "WILLEKEURIG" };
    cpu->gpr[29] -= 0x400u;
    const uint32_t buf = psx_mod_read_word(XP_BUF);
    xp_call(cpu, XP_CLEAR_OT_FN, buf + 0x2B4u, 0x824u, 0);
    psx_mod_write_word(XP_PRIM_PTR, buf + 0x2504u);
    psx_mod_write_word(0x800A9DF8u, buf + 0x118C4u);
    xp_call(cpu, XP_FULL_ENV_FN, 0, 0, 0);
    for (int i = 0; i < xp_players(); ++i) {
        char name[40], line[64];
        const int pick = (int)psx_mod_read_byte(XP_PICK + (uint32_t)i) - 1;
        if (pick >= 0) xp_char_name(pick, name, sizeof name);
        else snprintf(name, sizeof name, "%s", random_[ui_lang()]);
        snprintf(line, sizeof line, "%s %d   %s", label[ui_lang()], 3 + i, name);   /* (the font has no < >) */
        xp_text(cpu, line, 256, 196 + 16 * i);
    }
    s_xp_drawing = 1;
    psx_mod_write_word(0x1F801814u, 0x04000002u);           /* GP1(04): DMA CPU -> GP0 */
    psx_mod_write_word(0x1F8010A0u, (buf + 0x2340u) & 0xFFFFFFu);
    psx_mod_write_word(0x1F8010A4u, 0);
    psx_mod_write_word(0x1F8010A8u, 0x01000401u);           /* linked list, start */
    s_xp_drawing = 0;
    return 1;
}

/* ---- players 3 and 4: what they hear ---------------------------------------
 *
 * The sound chip is part of the game: every PC must play the same sounds.
 * So players 3-4 get what the split screen gives players 1-2: a sound's
 * volume is set by the nearest player. The game's positional sound
 * 0x80073BC0(sound, a1, scale, x, [y, z, key]) takes the nearest of the two
 * cameras (volume 8192 - 32 * sqrt(d), d the squared distance / 1024 per
 * axis, nothing past d 0xFFFF), then a looped sound (0x80063FAC, keyed: eight
 * slots, each dropped when not refreshed) or a one-shot (0x80063D10). With
 * players 3-4 it is done here the same way, karts 3-4 counting as listeners
 * too. Their engine sound (game.toml humans-80067460: the players' engine
 * code now runs for them) and their other looped kart sounds get keys of
 * their own (the game keys them by kart index + 0/2/4, which would clash);
 * their held-item hum is left out (slots). The characters' voices: below.
 * A few other sound kinds keep their own listener loops: the two cameras. */
#define XP_SOUND_FN   0x80073BC0u
#define XP_SQRT_FN    0x80088C98u
#define XP_LOOP_FN    0x80063FACu
#define XP_ONCE_FN    0x80063D10u
#define XP_CAM_POS    0x800C2F64u   /* camera + 0x44: x, y, z */

static uint32_t xp_guest_call(struct CPUState* cpu, uint32_t fn, uint32_t ra,
                              uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    cpu->gpr[4] = a0; cpu->gpr[5] = a1; cpu->gpr[6] = a2; cpu->gpr[7] = a3;
    cpu->gpr[31] = ra;
    cpu->pc = 0;
    psx_dispatch_call(cpu, fn, ra);
    return cpu->gpr[2];
}

static void xp_sound_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_xp_online || cpu->pc != 0 || !psx_mod_read_byte(SPLIT_SCREEN)) return;
    const int extra = xp_players();
    if (extra <= 0) return;
    const uint32_t sp = cpu->gpr[29], ra = cpu->gpr[31];
    const int32_t x = (int32_t)cpu->gpr[7];
    const int32_t y = (int32_t)psx_mod_read_word(sp + 16u), z = (int32_t)psx_mod_read_word(sp + 20u);
    const int16_t sound = (int16_t)cpu->gpr[4], a1 = (int16_t)cpu->gpr[5], scale = (int16_t)cpu->gpr[6];
    int key = (int16_t)psx_mod_read_word(sp + 24u);
    {   /* karts 3-4's looped kart sounds: keys of their own */
        int k = -1, base = 0;
        if (ra == 0x80067538u) { k = key; base = 0x7F10; }            /* engine: key = kart */
        else if (ra == 0x800675D8u) { k = key - 2; base = 0x7F20; }   /* key = kart + 2 */
        else if (ra == 0x80068980u) { k = key - 4; base = 0x7F30; }   /* key = kart + 4 */
        if (k >= 2 && k < 4) key = base + k;
        /* the eight looped slots: four engines and players 1-2's held-item
         * sound already take six; karts 3-4's held-item sound is left out
         * so the other looped sounds still find a slot */
        if (base == 0x7F20 && k >= 2 && k < 4) { psx_mod_finish_function(); return; }
    }
    /* the nearest listener: the players' cameras, then karts 3-4 */
    int32_t best = 0x7FFFFFFF;
    const int cams = (int)(int8_t)psx_mod_read_byte(0x800A97E4u);
    for (int i = 0; i < cams + extra; ++i) {
        uint32_t p;
        if (i < cams) p = XP_CAM_POS + 0x88u * (uint32_t)i;
        else p = XP_KARTS + (uint32_t)(2 + i - cams) * XP_KART_STRIDE + 32u;
        const int32_t dx = (int32_t)(x - (int32_t)psx_mod_read_word(p)) >> 10;
        const int32_t dy = (int32_t)(y - (int32_t)psx_mod_read_word(p + 4u)) >> 10;
        const int32_t dz = (int32_t)(z - (int32_t)psx_mod_read_word(p + 8u)) >> 10;
        const int32_t d = (int32_t)((uint32_t)(dx * dx) + (uint32_t)(dy * dy) + (uint32_t)(dz * dz));
        if (!(0xFFFF < d) && d < best) best = d;
    }
    uint32_t gpr[32];
    memcpy(gpr, cpu->gpr, sizeof gpr);
    const uint32_t hi = cpu->hi, lo = cpu->lo;
    if (best != 0x7FFFFFFF) {
        cpu->gpr[29] = sp - 64u;   /* a frame of ours under the caller's */
        const int32_t root = (int32_t)xp_guest_call(cpu, XP_SQRT_FN, ra, (uint32_t)best, 0, 0, 0);
        if (g_psx_call_bail || cpu->pc != 0) return;   /* the guest went elsewhere: let it */
        const int32_t vol = 8192 - (int32_t)((uint32_t)root << 5);
        if (vol > 0) {
            const int32_t m = (int32_t)scale * vol;
            const int32_t v = (m >= 0 ? m : m + 8191) >> 13;
            if (v > 0) {
                cpu->gpr[29] = sp - 64u;
                if (key != -1) {
                    psx_mod_write_word(sp - 64u + 16u, 0);
                    (void)xp_guest_call(cpu, XP_LOOP_FN, ra, (uint32_t)(int32_t)(int16_t)key, (uint32_t)(int32_t)sound,
                                        (uint32_t)(int32_t)a1, (uint32_t)v);
                } else {
                    (void)xp_guest_call(cpu, XP_ONCE_FN, ra, (uint32_t)(int32_t)sound, (uint32_t)(int32_t)a1, (uint32_t)v, 0);
                }
                if (g_psx_call_bail || cpu->pc != 0) return;
            }
        }
    }
    memcpy(cpu->gpr, gpr, sizeof gpr);
    cpu->hi = hi; cpu->lo = lo;
    psx_mod_finish_function();   /* done: back to the caller */
}

/* The characters' voices (0x80073D78, every kart in turn): the nearest
 * camera to the kart sets the volume (as above, scale 160), then the one-shot
 * 0x80063D10(sound, 5376, volume) -- not called when no camera is within
 * range. With players 3-4, karts 3-4 count as listeners too: a voice played
 * gets the nearer one's volume. (Also playing the voices left out for the
 * cameras, near karts 3-4 only, made the PCs' games part now and then: the
 * sound library's voice bookkeeping 0x800E24C8.. / 0x800D8918 came out
 * differently. Found later: the runtime's idle skip drifting the PCs'
 * clocks (now off in a session, see session_reset); left out for now.) */
#define XP_VOICE_PLAY   0x80074034u   /* ra of the voice's one-shot */

/* the squared distance (game units / 1024 per axis) to the nearest listener */
static int32_t xp_nearest(int32_t x, int32_t y, int32_t z, int with_cams) {
    int32_t best = 0x7FFFFFFF;
    const int cams = with_cams ? (int)(int8_t)psx_mod_read_byte(0x800A97E4u) : 0;
    const int extra = xp_players();
    for (int i = 0; i < cams + extra; ++i) {
        uint32_t p;
        if (i < cams) p = XP_CAM_POS + 0x88u * (uint32_t)i;
        else p = XP_KARTS + (uint32_t)(2 + i - cams) * XP_KART_STRIDE + 32u;
        const int32_t dx = (int32_t)(x - (int32_t)psx_mod_read_word(p)) >> 10;
        const int32_t dy = (int32_t)(y - (int32_t)psx_mod_read_word(p + 4u)) >> 10;
        const int32_t dz = (int32_t)(z - (int32_t)psx_mod_read_word(p + 8u)) >> 10;
        const int32_t d = (int32_t)((uint32_t)(dx * dx) + (uint32_t)(dy * dy) + (uint32_t)(dz * dz));
        if (!(0xFFFF < d) && d < best) best = d;
    }
    return best;
}

/* the voice's volume from the nearest listener (karts 3-4 included); 0: none
 * near. Calls the game's square root (registers kept by the caller). */
static int32_t xp_voice_volume(struct CPUState* cpu, uint32_t kart, uint32_t ra) {
    const int32_t best = xp_nearest((int32_t)psx_mod_read_word(kart + 32u), (int32_t)psx_mod_read_word(kart + 36u),
                                    (int32_t)psx_mod_read_word(kart + 40u), 1);
    if (best == 0x7FFFFFFF) return 0;
    const uint32_t sp = cpu->gpr[29];
    cpu->gpr[29] = sp - 64u;
    const int32_t root = (int32_t)xp_guest_call(cpu, XP_SQRT_FN, ra, (uint32_t)best, 0, 0, 0);
    cpu->gpr[29] = sp;
    if (g_psx_call_bail || cpu->pc != 0) return -1;
    const int32_t vol = 8192 - (int32_t)((uint32_t)root << 5);
    if (vol <= 0) return 0;
    return (vol * 160) >> 13;
}

static void xp_voice_play_hook(struct CPUState* cpu, uint32_t address) {   /* 0x80063D10 */
    (void)address;
    if (!s_xp_online || cpu->pc != 0 || cpu->gpr[31] != XP_VOICE_PLAY) return;
    if (!psx_mod_read_byte(SPLIT_SCREEN) || xp_players() <= 0) return;
    uint32_t gpr[32];
    memcpy(gpr, cpu->gpr, sizeof gpr);
    const uint32_t hi = cpu->hi, lo = cpu->lo;
    const int32_t v = xp_voice_volume(cpu, gpr[17] - 698u, gpr[31]);   /* s1: the kart + 698 */
    if (v < 0) return;   /* the guest went elsewhere */
    memcpy(cpu->gpr, gpr, sizeof gpr);
    cpu->hi = hi; cpu->lo = lo;
    if (v > (int32_t)cpu->gpr[6]) { cpu->gpr[6] = (uint32_t)v; cpu->gpr[7] = (uint32_t)v; }
}

static void xp_flip_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[31] != XP_FLIP_RA) return;
    s_xp_flips++;
    const int k = xp_view_kart();
    if (k < 0) {
        s_xp_cam_ok = 0;
        /* the character select: players 3-4's picks under it */
        if (s_xp_online && s_xp_text_ok && s_xp_flips - s_xp_charsel_seen < 4u && !psx_modrb_resimulating())
            (void)psx_mod_sandbox_call(cpu, xp_charsel_pass, NULL);
        return;
    }
    if (psx_modrb_resimulating()) return;
    if (!psx_mod_sandbox_call(cpu, xp_view_pass, (void*)(uintptr_t)k) && s_xp_refused++ < 4u) {
        fprintf(stdout, "TSR ONLINE: player view of kart %d not drawn (sandbox refused)\n", k);
        fflush(stdout);
    }
}

/* The "Online play" feature is on: the buttons, the menu and the view. */
static int s_active;
static void online_activate(void) {
    if (s_active) return;
    s_active = 1;
    if (env_on("TSR_XP_FAKE4")) { s_xp_fake4 = 1; xp_session(4, 0); }   /* test */
    fprintf(stdout, "TSR ONLINE PLAY ACTIVATED\n");
    fflush(stdout);
#if defined(_WIN32)
    psx_mod_set_pad_hook(online_pads);
#endif
    psx_mod_add_linked_list_hook(tsr_online_list_hook);
    /* tests: started by the environment */
    const char* v;
    const char* role = getenv("TSR_ONLINE");
    if (!role || !role[0]) return;
    int port = TSR_ONLINE_PORT;
    if ((v = getenv("TSR_ONLINE_PORT")) && atoi(v) > 0) port = atoi(v);
    if (!strcmp(role, "host")) tsr_online_host(port);
    else if (!strcmp(role, "join"))
        tsr_online_join((v = getenv("TSR_ONLINE_ADDR")) && v[0] ? v : "127.0.0.1", port);
}

static void online_setup(void) {
#if defined(_WIN32)
    InitializeCriticalSection(&s_req_lock);
#endif
    const char* view = getenv("TSR_ONLINE_VIEW");   /* test: one view without the network */
    if (view && (view[0] == '0' || view[0] == '1')) s_view = view[0] - '0';
    const char* v;
    if ((v = getenv("TSR_ONLINE_DELAY")) && v[0] && atoi(v) >= 0 && atoi(v) <= MAX_DELAY) {
        s_delay = atoi(v);
        s_delay_fixed = 1;
    }
    (void)psx_mod_register_activation_plugin("toystoryracer.online", online_activate);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", TEXT_FN, online_text_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", FRAME_START_FN, online_frame_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", XP_AI_FN, xp_ai_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", XP_FLIP_NEXT_FN, xp_flip_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", XP_CHARSEL, xp_charsel_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", XP_KART_INIT, xp_kart_init_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", XP_SOUND_FN, xp_sound_hook);
    (void)psx_mod_register_function_entry_plugin("toystoryracer.online", XP_ONCE_FN, xp_voice_play_hook);
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_online_plugin) {
    online_setup();
}
