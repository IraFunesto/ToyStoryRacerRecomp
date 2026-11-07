#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
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
#define VERSION        4u

enum { ROLE_NONE, ROLE_HOST, ROLE_JOIN };
enum { ST_OFF, ST_WAIT, ST_PING, ST_SAVE, ST_SEND, ST_RELOAD, ST_LOAD, ST_PLAY, ST_DEAD };
enum { MSG_HELLO = 1, MSG_STATE = 2, MSG_PAD = 3, MSG_CHECK = 4,
       MSG_PING = 5, MSG_PONG = 6, MSG_DELAY = 7, MSG_SETTINGS = 8 };

typedef struct {
    uint32_t frame;
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
#if defined(_WIN32)
static SOCKET s_listen = INVALID_SOCKET, s_sock = INVALID_SOCKET;
static SOCKET s_pending = INVALID_SOCKET;   /* join: the connect in progress */
static SOCKET s_beacon = INVALID_SOCKET;
static DWORD s_connect_start, s_next_try, s_next_beacon;
static DWORD s_ping_sent[PINGS];
static int s_pings_sent, s_pings_back;
static DWORD s_rtt_max;
static CRITICAL_SECTION s_req_lock;
#endif
/* requests from other threads (the join window) */
static volatile int s_req;          /* 0 none, 1 host, 2 join, 3 cancel */
static char s_req_addr[64];
static int s_req_port;

static uint32_t s_frame;             /* lockstep frame */
static PSXModPad s_local[RING], s_remote[RING];
static uint32_t s_remote_have[RING]; /* frame number + 1 stored at each slot */
static uint32_t s_check_frame, s_check_mine, s_check_theirs;
static int s_check_mine_ok, s_check_theirs_ok;
static uint32_t s_crc, s_crc_next, s_crc_frame;   /* the check in progress (ram_checksum_part) */
/* The buttons also go by UDP, each packet with the last UDP_FRAMES frames: a
 * lost packet costs nothing (the next one brings its frames again) and TCP's
 * resend after a loss -- a stall of a fifth of a second or more on a home
 * connection -- no longer holds the game. The TCP copy stays, for networks
 * that let no UDP through. */
#define UDP_FRAMES   16
#define UDP_MAGIC    0x55535354u   /* "TSSU" */
typedef struct {
    uint32_t magic, newest;           /* frames newest-UDP_FRAMES+1 .. newest */
    uint32_t sent_ms, echo_ms, hold_ms;   /* round trip: the peer's last time, held here */
    uint32_t ask_lead;                /* frames more the sender asks for: it had to wait */
    PSXModPad pad[UDP_FRAMES];
} UdpPads;
#if defined(_WIN32)
static SOCKET s_udp = INVALID_SOCKET;
static struct sockaddr_in s_udp_peer;   /* host: learned from the first packet */
static int s_udp_peer_ok;
static uint32_t s_peer_ms, s_peer_ms_at;   /* the peer's last send time, and when it came */
#endif
static uint32_t s_last_sent;       /* the newest frame whose buttons were sent */
static int s_send_delay;           /* frames ahead this PC sends its buttons (adapts) */
static uint32_t s_delay_drop_at;   /* not lowered before this time */
static float s_rtt_est;            /* round trip, ms (decaying maximum) */
static int s_loss_bonus;           /* frames more this PC asks for after its waits, fading */
static int s_peer_bonus;           /* frames more the other PC asks for */
static uint32_t s_loss_bonus_drop_at;
/* the connection's log (tsr_online.log): waits, round trip, delay */
static uint32_t s_stat_start, s_stat_waits, s_stat_wait_ms, s_stat_wait_max, s_stat_udp, s_stat_frames;
/* this PC's own time per frame (from the end of one lockstep step to the
 * start of the next: the game, the drawing, the pacing), in ms */
static double s_work_sum, s_work_max, s_prev_exit;
static uint32_t s_work_n, s_work_over18, s_work_over21;
static uint8_t* s_rx;                /* receive buffer */
static size_t s_rx_len, s_rx_cap;

static int env_on(const char* name) {   /* set and not empty */
    const char* v = getenv(name);
    return v && v[0];
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

/* end the session; failed: tell the menu something went wrong */
static void close_all(int failed, const char* why) {
    const int was_on = s_state != ST_OFF && s_state != ST_DEAD;
    close_sock(&s_sock);
    close_sock(&s_listen);
    close_sock(&s_pending);
    close_sock(&s_beacon);
    close_sock(&s_udp);
    s_udp_peer_ok = 0;
    s_rx_len = 0;
    s_failed = failed;
    s_state = ST_DEAD;
    s_view = -1;
    if (was_on || failed) say("%s (frame %u)", why, s_frame);
}

static int send_all(const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len) {
        int n = send(s_sock, p, (int)(len > 65536 ? 65536 : len), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(0); continue; }
            return 0;
        }
        p += n; len -= (size_t)n;
    }
    return 1;
}

static int send_msg(uint32_t type, const void* body, uint32_t len) {
    uint32_t head[2] = { type, len };
    return send_all(head, sizeof head) && (!len || send_all(body, len));
}

/* Read what has arrived; 0 when the connection is gone. */
static int pump(void) {
    for (;;) {
        if (s_rx_len + 65536 > s_rx_cap) {
            size_t cap = s_rx_cap ? s_rx_cap * 2 : 1u << 20;
            while (s_rx_len + 65536 > cap) cap *= 2;
            uint8_t* n = (uint8_t*)realloc(s_rx, cap);
            if (!n) return 0;
            s_rx = n; s_rx_cap = cap;
        }
        int n = recv(s_sock, (char*)s_rx + s_rx_len, 65536, 0);
        if (n == 0) return 0;
        if (n == SOCKET_ERROR) return WSAGetLastError() == WSAEWOULDBLOCK;
        s_rx_len += (size_t)n;
    }
}

/* the next whole message, if one has arrived (consumed by message_done) */
static int message(uint32_t* type, uint8_t** body, uint32_t* len) {
    if (s_rx_len < 8) return 0;
    uint32_t t, l;
    memcpy(&t, s_rx, 4); memcpy(&l, s_rx + 4, 4);
    if (s_rx_len < 8u + l) return 0;
    *type = t; *body = s_rx + 8; *len = l;
    return 1;
}

static void message_done(uint32_t len) {
    memmove(s_rx, s_rx + 8 + len, s_rx_len - 8 - len);
    s_rx_len -= 8 + len;
}

static void nonblocking(SOCKET s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
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
    if (bind(s_listen, (struct sockaddr*)&a, sizeof a) != 0 || listen(s_listen, 1) != 0) {
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
    say("waiting for the other player on port %d", s_port);
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
    size_t n = (size_t)snprintf(out, cap, "WIDESCREEN=%d;INSTANT LOADING=%d;INSTANT MEMORY CARD=%d;",
                                g_tsr_widescreen_on, g_tsr_files_on, g_tsr_memcard_on);
    for (size_t i = 0; i < sizeof opts / sizeof opts[0] && n < cap; ++i) {
        char v[32] = "-";
        if (!psx_mod_option_value("toystoryracer.widescreen", "widescreen", opts[i].id, v, sizeof v)) snprintf(v, sizeof v, "-");
        n += (size_t)snprintf(out + n, cap - n, "%s=%s;", opts[i].label, v);
    }
}

/* the connection is up: say hello, then the start */
static void connected(void) {
    nonblocking(s_sock);
    close_sock(&s_listen);
    close_sock(&s_beacon);
    s_rx_len = 0;
    s_game = game_code_hash();
    uint32_t hello[3] = { MAGIC, VERSION, s_game };
    send_msg(MSG_HELLO, hello, sizeof hello);
    char settings[256];
    game_settings(settings, sizeof settings);
    send_msg(MSG_SETTINGS, settings, (uint32_t)strlen(settings) + 1u);
    s_mismatch[0] = 0;
    s_pings_sent = s_pings_back = 0;
    s_rtt_max = 0;
    s_crc_next = 0;   /* no check left over from an earlier session */
    s_last_sent = 0;
    s_rtt_est = 0.0f;
    s_loss_bonus = s_peer_bonus = 0;
    s_prev_exit = 0.0;
    s_work_sum = s_work_max = 0.0;
    s_work_n = s_work_over18 = s_work_over21 = 0;
    s_peer_ms = s_peer_ms_at = 0;
    s_stat_start = GetTickCount();
    s_stat_waits = s_stat_wait_ms = s_stat_wait_max = s_stat_udp = s_stat_frames = 0;
    if (s_role == ROLE_JOIN) {   /* the host's address and game port */
        int len = sizeof s_udp_peer;
        s_udp_peer_ok = getpeername(s_sock, (struct sockaddr*)&s_udp_peer, &len) == 0;
        s_udp_peer.sin_port = htons((u_short)s_port);
        if (s_udp == INVALID_SOCKET) s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp != INVALID_SOCKET) { u_long nb = 1; ioctlsocket(s_udp, FIONBIO, &nb); }
    }
    s_check_mine_ok = s_check_theirs_ok = 0;
    s_state = s_role == ROLE_HOST ? ST_PING : ST_LOAD;
    s_view = s_role == ROLE_HOST ? 0 : 1;   /* split screen: each sees their own player */
    say("connected to %s", s_role == ROLE_HOST ? "the player who joined" : s_peer_addr);
}

/* host: accept the other player; join: connect to the host (never blocking
 * the game: the host may be far away or not there at all) */
static void try_connect(void) {
    if (s_role == ROLE_HOST) {
        beacon_tick();
        SOCKET c = accept(s_listen, NULL, NULL);
        if (c == INVALID_SOCKET) return;
        s_sock = c;
        connected();
        return;
    }
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
    s_sock = s_pending;
    s_pending = INVALID_SOCKET;
    connected();
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
        if (a == 0x800A7F30u || a == 0x800A7F50u || a == 0x800A7F54u)
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

static void check_compare(void) {
    if (!s_check_mine_ok || !s_check_theirs_ok) return;
    s_check_mine_ok = s_check_theirs_ok = 0;
    if (s_check_mine == s_check_theirs) return;
    fprintf(stdout, "TSR ONLINE: DESYNC at frame %u (mine %08X, theirs %08X)\n",
            s_check_frame, s_check_mine, s_check_theirs);
    fflush(stdout);
}

/* one arrived message other than the state; 0 when the session must end */
static int handle_message(uint32_t type, const uint8_t* body, uint32_t len) {
    if (type == MSG_PAD && len == sizeof(PadMsg)) {
        PadMsg m;
        memcpy(&m, body, sizeof m);
        if (m.frame >= s_frame && m.frame < s_frame + RING - 1 && !env_on("TSR_ONLINE_NOTCPPADS")) {
            s_remote[m.frame % RING] = m.pad;
            s_remote_have[m.frame % RING] = m.frame + 1;
        }
    } else if (type == MSG_CHECK && len == 8) {
        uint32_t f, c;
        memcpy(&f, body, 4); memcpy(&c, body + 4, 4);
        if (f == s_check_frame || !s_check_mine_ok) { s_check_frame = f; s_check_theirs = c; s_check_theirs_ok = 1; }
        check_compare();
    } else if (type == MSG_HELLO && len == 12) {
        uint32_t h[3];
        memcpy(h, body, sizeof h);
        if (h[0] != MAGIC || h[1] != VERSION) {
            close_all(1, "the other player has a different version of the mod");
            return 0;
        }
        if (h[2] != s_game) {
            close_all(1, "the other player has another version of the game (both need the same disc)");
            return 0;
        }
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
            close_all(1, "the two PCs have different mod options (they must match to play together)");
            return 0;
        }
    } else if (type == MSG_PING && len == 4) {
        send_msg(MSG_PONG, body, 4);
    } else if (type == MSG_PONG && len == 4) {
        uint32_t i;
        memcpy(&i, body, 4);
        if (i < PINGS) {
            DWORD rtt = GetTickCount() - s_ping_sent[i];
            if (rtt > s_rtt_max) s_rtt_max = rtt;
            s_pings_back++;
        }
    } else if (type == MSG_DELAY && len == 4) {
        uint32_t d;
        memcpy(&d, body, 4);
        if (d <= MAX_DELAY) s_delay = (int)d;
    }
    return 1;
}

/* everything arrived, up to the state (join: left for ST_LOAD) */
static int handle_messages(void) {
    uint32_t type, len;
    uint8_t* body;
    while (message(&type, &body, &len)) {
        if (type == MSG_STATE) return 1;
        if (!handle_message(type, body, len)) return 0;
        message_done(len);
    }
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
#define BTN_TRIANGLE  0x1000u
#define BTN_CIRCLE    0x2000u
#define BTN_CROSS     0x4000u
#define BTN_SQUARE    0x8000u

enum { UI_IDLE, UI_MENU, UI_HOSTING, UI_JOINING, UI_NOTICE };
enum { T_SELECT, T_HOST, T_JOIN, T_BACK, T_WAITING, T_CANCEL, T_CONNECTING, T_FAILED, T_LOST, T_SEARCH, T_SETTINGS, T_COUNT };
static const char* const s_texts[6][T_COUNT] = {
    { "SELECT  ONLINE", "HOST", "JOIN", "BACK", "WAITING FOR A PLAYER...", "CANCEL", "CONNECTING...",
      "CONNECTION FAILED", "CONNECTION LOST", "CHOOSE A ROOM...", "DIFFERENT MOD OPTION:" },
    { "SELECT  EN LIGNE", "HEBERGER", "REJOINDRE", "RETOUR", "EN ATTENTE D'UN JOUEUR...", "ANNULER",
      "CONNEXION...", "ECHEC DE LA CONNEXION", "CONNEXION PERDUE", "CHOISIS UNE PARTIE...", "OPTION DE MOD DIFFERENTE :" },
    { "SELECT  ONLINE", "HOSTEN", "BEITRETEN", "ZURUECK", "WARTE AUF SPIELER...", "ABBRECHEN",
      "VERBINDE...", "VERBINDUNG FEHLGESCHLAGEN", "VERBINDUNG VERLOREN", "WAEHLE EIN SPIEL...", "ANDERE MOD-OPTION:" },
    { "SELECT  ONLINE", "HOST", "JOIN", "INDIETRO", "IN ATTESA DI ALTRI GIOCATORI...", "ANNULLA",
      "CONNESSIONE...", "CONNESSIONE FALLITA", "CONNESSIONE PERSA", "SCEGLI UNA STANZA...", "OPZIONE MOD DIVERSA:" },
    { "SELECT  EN LINEA", "CREAR", "UNIRSE", "ATRAS", "ESPERANDO JUGADORES...", "CANCELAR",
      "CONECTANDO...", "CONEXION FALLIDA", "CONEXION PERDIDA", "ELIGE UNA SALA...", "OPCION DE MOD DISTINTA:" },
    { "SELECT  ONLINE", "HOSTEN", "DEELNEMEN", "TERUG", "WACHTEN OP SPELERS...", "ANNULEREN",
      "VERBINDEN...", "VERBINDING MISLUKT", "VERBINDING VERBROKEN", "KIES EEN SPEL...", "ANDERE MOD-OPTIE:" },
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
    if (s_nlines >= 2 || (s_state != ST_OFF && s_state != ST_WAIT && s_state != ST_DEAD)) return;
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
    if (s_state != ST_OFF && s_state != ST_WAIT && s_state != ST_DEAD) return;
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

static void online_frame_hook(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->pc != 0) return;
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
    const int status = tsr_online_status();
    const int online = status == TSR_ONLINE_PLAYING || status == TSR_ONLINE_STARTING;
    const int x = (int)(int16_t)cpu->gpr[5], y = (int)(int16_t)cpu->gpr[6];
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
            if (text == T_SETTINGS) snprintf(line, sizeof line, "%s %s", ui_text(text), s_mismatch);
            else snprintf(line, sizeof line, "%s", ui_text(text));
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
static void udp_send(void) {
    if (s_udp == INVALID_SOCKET || !s_udp_peer_ok) return;
    UdpPads u;
    memset(&u, 0, sizeof u);
    u.magic = UDP_MAGIC;
    u.newest = s_last_sent;
    u.sent_ms = GetTickCount();
    u.echo_ms = s_peer_ms;
    u.hold_ms = s_peer_ms ? GetTickCount() - s_peer_ms_at : 0;
    u.ask_lead = (uint32_t)s_loss_bonus;
    for (int i = 0; i < UDP_FRAMES; ++i) {
        const uint32_t fr = s_last_sent - (uint32_t)(UDP_FRAMES - 1 - i);
        if ((int32_t)fr >= 0) u.pad[i] = s_local[fr % RING];
    }
    sendto(s_udp, (const char*)&u, sizeof u, 0, (const struct sockaddr*)&s_udp_peer, sizeof s_udp_peer);
}

static void udp_pump(void) {
    if (s_udp == INVALID_SOCKET) return;
    for (;;) {
        UdpPads u;
        struct sockaddr_in from;
        int fl = sizeof from;
        const int n = recvfrom(s_udp, (char*)&u, sizeof u, 0, (struct sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) return;
        if (n != (int)sizeof u || u.magic != UDP_MAGIC) continue;
        {   /* test: a bad network (TSR_ONLINE_UDPDROP = percent of packets lost) */
            static int s_drop = -1;
            if (s_drop < 0) { const char* v = getenv("TSR_ONLINE_UDPDROP"); s_drop = v && v[0] ? atoi(v) : 0; }
            if (s_drop > 0 && rand() % 100 < s_drop) continue;
        }
        if (s_role == ROLE_HOST) {
            /* the first packet names the other player's address (through
             * their router); later ones must come from it */
            if (!s_udp_peer_ok) { s_udp_peer = from; s_udp_peer_ok = 1; }
            else if (from.sin_addr.s_addr != s_udp_peer.sin_addr.s_addr) continue;
            else s_udp_peer.sin_port = from.sin_port;
        }
        s_stat_udp++;
        if (u.ask_lead <= 8u) s_peer_bonus = (int)u.ask_lead;
        if (u.echo_ms) {   /* a round trip: now - our time - the time it waited there */
            const uint32_t rtt = GetTickCount() - u.echo_ms - u.hold_ms;
            if (rtt < 5000u && (float)rtt > s_rtt_est) s_rtt_est = (float)rtt;
        }
        if (u.sent_ms - s_peer_ms < 0x80000000u || !s_peer_ms) { s_peer_ms = u.sent_ms; s_peer_ms_at = GetTickCount(); }
        for (int i = 0; i < UDP_FRAMES; ++i) {
            const uint32_t fr = u.newest - (uint32_t)(UDP_FRAMES - 1 - i);
            if ((int32_t)fr < 0 || fr < s_frame || fr >= s_frame + RING - 1) continue;
            s_remote[fr % RING] = u.pad[i];
            s_remote_have[fr % RING] = fr + 1;
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
    int want = (int)((s_rtt_est * 0.5f + 10.0f) / (float)FRAME_MS + 0.999f) + 2 + s_peer_bonus;
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
    say("last 10 s: %u frames, waited %u times (%u ms, longest %u ms), round trip %.0f ms, buttons %d frames late (other PC asks %d more), %u UDP packets",
        s_stat_frames, s_stat_waits, s_stat_wait_ms, s_stat_wait_max, s_rtt_est, s_send_delay, s_peer_bonus, s_stat_udp);
    s_stat_start = GetTickCount();
    s_stat_waits = s_stat_wait_ms = s_stat_wait_max = s_stat_udp = s_stat_frames = 0;
    s_work_sum = s_work_max = 0.0;
    s_work_n = s_work_over18 = s_work_over21 = 0;
}
#endif

static void neutral(PSXModPad* p) {
    p->buttons = 0xFFFFu;
    p->lx = p->ly = p->rx = p->ry = 0x80u;
    p->analog = 0; p->connected = 1;
}

static void online_pads(PSXModPad pads[2]) {
#if defined(_WIN32)
    {   /* tests: start from a save state file (TSR_BOOTSTATE), hold buttons (TSR_HOLD, hex) */
        static unsigned s_boot_frames;
        static int s_boot_done;
        const char* bs = getenv("TSR_BOOTSTATE");
        if (bs && bs[0] && !s_boot_done && ++s_boot_frames == 400u) {
            s_boot_done = 1;
            FILE* f = fopen(bs, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                void* d = n > 0 ? malloc((size_t)n) : NULL;
                if (d && fread(d, 1, (size_t)n, f) == (size_t)n)
                    fprintf(stdout, "TSR TEST: state %s %s\n", bs, psx_mod_state_load(d, (size_t)n) ? "loading" : "refused");
                free(d);
                fclose(f);
            }
        }
        const char* hold = getenv("TSR_HOLD");
        if (hold && hold[0] && s_boot_done && s_boot_frames++ > 600u) pads[0].buttons = (uint16_t)strtoul(hold, NULL, 16);
    }
    take_request();
    if (s_state != ST_PLAY) ui_pads(pads);   /* the online menu may have the buttons */
    /* this PC's player: the pad on port 1 */
    PSXModPad mine = pads[0];
    mine.connected = 1;
    switch (s_state) {
    case ST_WAIT:
        try_connect();
        return;
    case ST_PING: {   /* host: how far away is the other player? */
        {   /* what arrived first (the other PC's reason, when it leaves) */
            const int alive = pump();
            if (!handle_messages()) return;
            if (!alive) { close_all(1, "the other player left"); return; }
        }
        if (s_pings_sent < PINGS) {
            uint32_t i = (uint32_t)s_pings_sent;
            s_ping_sent[i] = GetTickCount();
            send_msg(MSG_PING, &i, 4);
            s_pings_sent++;
            return;
        }
        if (s_pings_back < PINGS) {
            if (GetTickCount() - s_ping_sent[0] > 10000) close_all(1, "the other player does not answer");
            return;
        }
        if (!s_delay_fixed) {
            int d = (int)((s_rtt_max / 2 + FRAME_MS - 1) / FRAME_MS) + 2;
            s_delay = d < MIN_DELAY ? MIN_DELAY : d > MAX_DELAY ? MAX_DELAY : d;
        }
        uint32_t d = (uint32_t)s_delay;
        send_msg(MSG_DELAY, &d, 4);
        say("ping %u ms: buttons %u frames late", (unsigned)s_rtt_max, d);
        s_state = ST_SAVE;
        return;
    }
    case ST_SAVE:   /* host: the state the game continues from on both PCs */
        if (!psx_mod_state_save(STATE_SLOT)) { close_all(1, "could not save the state"); return; }
        s_state = ST_SEND;
        pads[0] = mine; neutral(&pads[1]);   /* this frame belongs to no one */
        return;
    case ST_SEND: {
        if (psx_mod_state_pending()) { neutral(&pads[1]); return; }
        uint8_t* data = NULL;
        size_t size = 0;
        if (!psx_mod_state_read(STATE_SLOT, &data, &size) || !send_msg(MSG_STATE, data, (uint32_t)size)) {
            free(data); close_all(1, "could not send the state"); return;
        }
        /* The host loads the state it sent, as the other PC does: what a
         * state does not hold (the disc's and the sound chip's progress
         * inside their current work) then restarts the same on both PCs. */
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
        break;   /* the first frame after the load: lockstep frame 0, as on the other PC */
    case ST_LOAD: {   /* join: wait for the host's state, load it */
        static int s_loading;
        if (s_loading) {
            if (psx_mod_state_pending()) return;
            s_loading = 0;
            s_state = ST_PLAY;
            s_frame = 0;
            say("state loaded, playing (buttons %d frames late)", s_delay);
            break;   /* the first frame after the load: lockstep frame 0 */
        }
        {
            const int alive = pump();
            if (!handle_messages()) return;   /* hello, settings, pings, the delay */
            if (!alive) { close_all(1, "the host left"); return; }
        }
        uint32_t type, len;
        uint8_t* body;
        if (!message(&type, &body, &len) || type != MSG_STATE) return;
        if (!psx_mod_state_load(body, len)) { close_all(1, "could not load the state"); return; }
        s_loading = 1;
        message_done(len);
        return;
    }
    case ST_PLAY:
        break;
    default:
        return;
    }

    /* lockstep: send this player's buttons for frame + delay, play frame
     * with both players' buttons of frame (neutral before the delay) */
    if (s_frame == 0) {
        memset(s_remote_have, 0, sizeof s_remote_have);
        for (int i = 0; i < s_delay; i++) {   /* the first frames: nobody presses */
            neutral(&s_local[i % RING]);
            neutral(&s_remote[i % RING]);
            s_remote_have[i % RING] = (uint32_t)i + 1;
        }
        s_last_sent = (uint32_t)s_delay - 1u;
        s_send_delay = s_delay;
        s_delay_drop_at = GetTickCount() + 3000;
    }
    /* this player's buttons for the frames up to frame + delay not sent yet
     * (the delay grew: the frames in between get the same buttons; it
     * shrank: nothing new until frame + delay passes what was sent). Each
     * frame's buttons are fixed once sent, so both PCs play the same. */
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
        s_local[s_last_sent % RING] = mine;
        PadMsg m = { s_last_sent, mine };
        if (!send_msg(MSG_PAD, &m, sizeof m)) { close_all(1, "the connection broke"); return; }
    }
    udp_send();
    const DWORD wait_start = GetTickCount();
    int waited = 0;
    for (;;) {
        if (!pump()) { close_all(1, "the other player left"); return; }
        if (!handle_messages()) return;
        udp_pump();
        if (s_remote_have[s_frame % RING] == s_frame + 1) break;
        if (GetTickCount() - wait_start > 10000) { close_all(1, "the other player stopped answering"); return; }
        /* waiting: ours again every few ms (the other PC may be waiting
         * for ours, and a lost packet must not leave both waiting) */
        if (GetTickCount() - wait_start >= (DWORD)(2 + 8 * waited)) { waited++; udp_send(); }
        Sleep(0);
    }
    {
        const uint32_t w = GetTickCount() - wait_start;
        if (w >= 2) {
            /* waited: the other PC's packets were lost or late -- ask it
             * (in ours) to send a frame further ahead for a while */
            if (s_loss_bonus < 4) s_loss_bonus++;
            s_loss_bonus_drop_at = GetTickCount() + 5000;
            s_stat_waits++;
            s_stat_wait_ms += w;
            if (w > s_stat_wait_max) s_stat_wait_max = w;
        }
        stats_tick();
        s_prev_exit = now_ms();
    }
    PSXModPad host_pad = s_role == ROLE_HOST ? s_local[s_frame % RING] : s_remote[s_frame % RING];
    PSXModPad join_pad = s_role == ROLE_HOST ? s_remote[s_frame % RING] : s_local[s_frame % RING];
    pads[0] = host_pad; pads[0].connected = 1;
    pads[1] = join_pad; pads[1].connected = 1;

    if (s_frame && s_frame % CHECK_EVERY == 0) {   /* a check starts */
        s_crc = 0xFFFFFFFFu;
        s_crc_next = 0x80010000u;   /* above the BIOS kernel */
        s_crc_frame = s_frame;
        if (env_on("TSR_ONLINE_DUMP")) {   /* test: the RAM of each check, to compare */
            char path[96];
            snprintf(path, sizeof path, "online_ram_%s_%u.bin", s_role == ROLE_HOST ? "host" : "join", s_frame);
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
            send_msg(MSG_CHECK, body, sizeof body);
            check_compare();
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

int tsr_online_local_view(void) {
    return s_view >= 0 && psx_mod_read_byte(SPLIT_SCREEN) ? s_view : -1;
}

/* The two views' rectangles: pointers to x0, y0, x1, y1, centre x, centre y
 * (halfwords); 0-127 and 128-255 in PAL, 0-119 and 120-239 in NTSC. */
#define VIEW_TABLE    0x800A97DCu

void tsr_online_list_hook(void) {
    if (env_on("TSR_MUTEALL")) psx_mod_set_list_mute(1);   /* test: the game's drawing skipped */
    const int view = tsr_online_local_view();
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

/* The "Online play" feature is on: the buttons, the menu and the view. */
static int s_active;
static void online_activate(void) {
    if (s_active) return;
    s_active = 1;
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
}

PSX_MOD_CONSTRUCTOR(psx_register_tsr_online_plugin) {
    online_setup();
}
