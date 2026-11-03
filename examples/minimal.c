/* minimal.c - the smallest useful Amatrix client.
 *
 *   minimal SERVER USER PASSWORD            list my rooms and the public rooms
 *   minimal SERVER USER PASSWORD ROOM       open ROOM (ID or alias): show the last
 *                                           messages, send one, then print new
 *                                           messages until someone writes "/quit"
 *
 * e.g.  minimal https://matrix.org test geheim "#testkanal:matrix.org"
 *
 * Encryption happens by itself: if the room is encrypted, the core encrypts
 * and decrypts; the program only ever sees plain text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amatrix.h"

#ifdef __amigaos__
unsigned long __stack = 128 * 1024;     /* the crypto needs more than the 4 KB default */
#endif

static int quit;

/* Called by amx_sync() / amx_load_history() for everything that happens. */
static void on_event(AmxClient *c, const AmxEvent *ev, void *userdata)
{
    if (ev->type == AMX_EV_MESSAGE) {
        char *text = amx_to_local(ev->body);          /* UTF-8 -> Amiga charset */
        printf("%s%s: %s\n", ev->history ? "  (earlier) " : "", ev->sender, text);
        free(text);
        if (!ev->history && !ev->own && !strcmp(ev->body, "/quit")) quit = 1;
    }
}

static void print_local(const char *fmt, const char *utf8)
{
    char *t = amx_to_local(utf8 ? utf8 : "");
    printf(fmt, t);
    free(t);
}

static void list_rooms(AmxClient *c)
{
    AmxRoomInfo *rooms;
    AmxPublicRoom *pub;
    int i, n;

    n = amx_rooms(c, &rooms);
    printf("My rooms (%d):\n", n);
    for (i = 0; i < n; i++) {
        print_local("  %-30s", rooms[i].name);
        printf(" %s%s%s unread %d  %s\n",
               rooms[i].membership == AMX_ROOM_INVITED ? "[invitation] " : "",
               rooms[i].is_direct ? "[direct] " : "", rooms[i].encrypted ? "[encrypted]" : "",
               rooms[i].unread, rooms[i].room_id);
    }
    amx_free_rooms(rooms, n);

    n = amx_public_rooms(c, NULL, NULL, 20, &pub);
    printf("Public rooms on this server (%d):\n", n < 0 ? 0 : n);
    for (i = 0; i < n; i++) {
        print_local("  %-30s", pub[i].name ? pub[i].name : pub[i].room_id);
        printf(" %d members  %s\n", pub[i].members, pub[i].alias ? pub[i].alias : pub[i].room_id);
    }
    amx_free_public_rooms(pub, n);
}

int main(int argc, char **argv)
{
    AmxClient *c;
    char *room = NULL;

    if (argc != 4 && argc != 5) {
        printf("usage: minimal SERVER USER PASSWORD [ROOM]\n");
        return 10;
    }

    /* 1. network (bsdsocket + AmiSSL on the Amiga), once per program */
    if (amx_init()) { printf("no network\n"); return 20; }

    /* 2. a client for one account; its keys, sessions and room list live in
     *    the drawer "minimal-store" */
    c = amx_client_new(argv[1], "minimal-store");
    amx_set_event_handler(c, on_event, NULL);

    /* 3. log in (the second time it reuses the saved login and device) */
    if (amx_login(c, argv[2], argv[3])) {
        printf("login failed: %s\n", amx_error(c));
        goto out;
    }

    /* 4. one sync: brings the room list up to date */
    amx_sync(c, 0);

    if (argc == 4) {                     /* no room given: show where we can go */
        list_rooms(c);
        goto out;
    }

    /* 5. open the room: join (no-op if already in it), show the last 10 messages */
    if (amx_join(c, argv[4], &room)) {
        printf("join failed: %s\n", amx_error(c));
        goto out;
    }
    printf("--- %s ---\n", room);
    amx_load_history(c, room, 10, 1, NULL);

    /* 6. send */
    if (amx_send_text(c, room, "Hallo vom Amiga!", NULL))
        printf("send failed: %s\n", amx_error(c));

    /* 7. wait for messages: each amx_sync() waits up to 30 s for news */
    while (!quit) {
        if (amx_sync(c, 30000))
            printf("sync failed: %s\n", amx_error(c));
    }

out:
    free(room);
    amx_client_free(c);
    amx_cleanup();
    return 0;
}
