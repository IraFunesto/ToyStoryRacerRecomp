#ifndef TSR_ONLINE_H
#define TSR_ONLINE_H

/* Toy Story Racer - two players online (tsr_online.c) */

#define TSR_ONLINE_PORT 27450   /* TCP, the game; UDP port + 1: the room search */

enum {
    TSR_ONLINE_OFF,        /* not online */
    TSR_ONLINE_WAITING,    /* host: waiting for the other player */
    TSR_ONLINE_CONNECTING, /* join: looking for the host */
    TSR_ONLINE_STARTING,   /* connected: measuring the delay, sending the state */
    TSR_ONLINE_PLAYING,    /* both PCs running the same game */
    TSR_ONLINE_FAILED      /* see tsr_online_message() */
};

/* Requests: safe from any thread, carried out on the game's next frame. */
void tsr_online_host(int port);
void tsr_online_join(const char* addr, int port);
void tsr_online_cancel(void);

int tsr_online_status(void);
const char* tsr_online_message(void);   /* last event, English, for the log/UI */
int tsr_online_is_host(void);

/* split screen while online: the view this PC shows (0 top, 1 bottom), -1 both */
int tsr_online_local_view(void);

/* Room search on the local networks (LAN, Radmin, Hamachi...): the hosts
 * waiting for a player announce themselves every second. Call from one
 * thread only (the join window). */
typedef struct {
    char addr[64];
    int port;
    char name[32];
} TsrOnlineRoom;

int tsr_online_search_start(void);
int tsr_online_search_poll(TsrOnlineRoom* rooms, int max);   /* rooms seen lately */
void tsr_online_search_stop(void);

#endif
