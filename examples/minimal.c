/* minimal.c - the smallest useful Amatrix client.
 *
 *   minimal SERVER USER PASSWORD ROOM
 *   e.g.  minimal https://matrix.org test geheim "#testkanal:matrix.org"
 *
 * Logs in, joins ROOM (ID or alias), sends one message, then prints every
 * message it receives until someone writes "/quit".
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

/* Called by amx_sync() for everything that happens. */
static void on_event(AmxClient *c, const AmxEvent *ev, void *userdata)
{
    if (ev->type == AMX_EV_MESSAGE && !ev->own) {
        char *text = amx_to_local(ev->body);          /* UTF-8 -> Amiga charset */
        printf("%s: %s\n", ev->sender, text);
        free(text);
        if (!strcmp(ev->body, "/quit")) quit = 1;
    }
}

int main(int argc, char **argv)
{
    AmxClient *c;
    char *room = NULL;

    if (argc != 5) {
        printf("usage: minimal SERVER USER PASSWORD ROOM\n");
        return 10;
    }

    /* 1. network (bsdsocket + AmiSSL on the Amiga), once per program */
    if (amx_init()) { printf("no network\n"); return 20; }

    /* 2. a client for one account; its keys and sessions live in the drawer "minimal-store" */
    c = amx_client_new(argv[1], "minimal-store");
    amx_set_event_handler(c, on_event, NULL);

    /* 3. log in (the second time it reuses the saved login and device) */
    if (amx_login(c, argv[2], argv[3])) {
        printf("login failed: %s\n", amx_error(c));
        goto out;
    }

    /* 4. join the room */
    if (amx_join(c, argv[4], &room)) {
        printf("join failed: %s\n", amx_error(c));
        goto out;
    }
    printf("joined %s\n", room);

    /* 5. one first sync: establishes where "now" is (and shows recent messages) */
    amx_sync(c, 0);

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
