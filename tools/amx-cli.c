/* amx-cli: small command-line client on top of the Amatrix core, used for
 * manual tests on a real Amiga (and as an example of the API).
 *
 *   amx-cli [options] HOMESERVER_URL USER PASSWORD
 *     -s DIR     store directory (default amatrix-store)
 *     -n COUNT   exit after COUNT messages from others
 *     -t SECS    exit after SECS seconds
 *     -v / -vv   verbose / with HTTP requests
 *     -e         echo: answer every message from others ("Amiga hat empfangen: ...")
 *     -g TEXT    greeting: sent once to each room joined via invite
 *
 * Joins every room it is invited to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "amatrix.h"

#ifdef __amigaos__
unsigned long __stack = 200 * 1024;
#endif

static struct {
    int echo, received, sent;
    char *greeting;            /* UTF-8 */
    char *join[16];            /* rooms to join after this sync round */
    int n_join;
    char *reply_room[16], *reply_text[16];
    int n_reply;
} S;

static void on_event(AmxClient *c, const AmxEvent *ev, void *ud)
{
    char *shown;
    (void)ud;
    switch (ev->type) {
    case AMX_EV_INVITE:
        printf("invited to %s by %s\n", ev->room_id, ev->sender ? ev->sender : "?");
        if (S.n_join < 16) S.join[S.n_join++] = strdup(ev->room_id);
        break;
    case AMX_EV_ROOM_KEY:
        printf("got room key for %s from %s\n", ev->room_id, ev->sender);
        break;
    case AMX_EV_UNDECRYPTABLE:
        printf("[%s] %s: <cannot decrypt: %s>\n", ev->room_id, ev->sender ? ev->sender : "?", ev->reason);
        break;
    case AMX_EV_MESSAGE:
        shown = amx_to_local(ev->body);
        printf("[%s] %s: %s%s\n", ev->room_id, ev->sender, shown ? shown : "", ev->own ? "   (own)" : "");
        free(shown);
        if (!ev->own) {
            S.received++;
            if (S.echo && S.n_reply < 16) {
                char *t = malloc(strlen(ev->body) + 32);
                sprintf(t, "Amiga hat empfangen: %s", ev->body);
                S.reply_room[S.n_reply] = strdup(ev->room_id);
                S.reply_text[S.n_reply++] = t;
            }
        }
        break;
    default:
        break;
    }
    fflush(stdout);
    (void)c;
}

static void send_one(AmxClient *c, const char *room, const char *text)
{
    char *shown;
    if (amx_send_text(c, room, text, NULL)) {
        printf("send failed: %s\n", amx_error(c));
        return;
    }
    shown = amx_to_local(text);
    printf("sent to %s: %s\n", room, shown ? shown : "");
    free(shown);
    S.sent++;
}

int main(int argc, char **argv)
{
    const char *store = "amatrix-store";
    int i, max = -1, secs = -1, verbose = 0;
    time_t start = time(NULL);
    AmxClient *c;

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) store = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-vv")) verbose = 2;
        else if (!strcmp(argv[i], "-e")) S.echo = 1;
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) S.greeting = amx_from_local(argv[++i]);
        else break;
    }
    if (argc - i != 3) {
        fprintf(stderr, "usage: amx-cli [-s store] [-n count] [-t secs] [-v|-vv] [-e] [-g text] SERVER USER PASSWORD\n");
        return 10;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (amx_init()) { printf("network init failed\n"); return 20; }
    c = amx_client_new(argv[i], store);
    if (!c) { amx_cleanup(); return 20; }
    amx_set_verbose(c, verbose);
    amx_set_event_handler(c, on_event, NULL);
    if (amx_login(c, argv[i + 1], argv[i + 2])) {
        printf("login failed: %s\n", amx_error(c));
        amx_client_free(c);
        amx_cleanup();
        return 10;
    }
    printf("logged in as %s, device %s\n", amx_user_id(c), amx_device_id(c));

    for (;;) {
        long wait = 30000;
        int k;
        if (secs >= 0) {
            long rem = (long)(secs - (time(NULL) - start));
            if (rem <= 0) break;
            if (rem * 1000 < wait) wait = rem * 1000;
        }
        if (amx_sync(c, wait)) { printf("sync failed: %s\n", amx_error(c)); continue; }
        for (k = 0; k < S.n_join; k++) {
            char *rid = NULL;
            if (amx_join(c, S.join[k], &rid) == 0) {
                printf("joined %s\n", rid);
                if (S.greeting) send_one(c, rid, S.greeting);
            } else {
                printf("join failed: %s\n", amx_error(c));
            }
            free(rid);
            free(S.join[k]);
        }
        S.n_join = 0;
        for (k = 0; k < S.n_reply; k++) {
            send_one(c, S.reply_room[k], S.reply_text[k]);
            free(S.reply_room[k]);
            free(S.reply_text[k]);
        }
        S.n_reply = 0;
        if (max >= 0 && S.received >= max) break;
    }
    printf("done, %d message(s) received, %d sent\n", S.received, S.sent);
    amx_client_free(c);
    amx_cleanup();
    free(S.greeting);
    return 0;
}
