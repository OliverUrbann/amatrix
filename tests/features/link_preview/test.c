/* Feature: link previews.
 * The peer serves a web page with Open Graph data (title, description,
 * picture) and posts its link; we ask our server for the preview.  A page
 * that does not exist gives none. */
#include "mxtest.h"

static const char *ROOM, *PEER;
static int have_link(void *u)
{
    int i;
    (void)u;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && !strcmp(mxt_ev[i].room_id, ROOM) && mxt_ev[i].sender
            && !strcmp(mxt_ev[i].sender, PEER) && mxt_ev[i].body && strstr(mxt_ev[i].body, "http://"))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL, url[200], bad[220], *t = NULL, *d = NULL, *im = NULL, *p;
    MxtMember join;
    int i;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Links";
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create room");
    if (!room) return mxt_done();
    ROOM = room; PEER = a.peer;
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");
    MXT_OK(mxt_sync_until(c, have_link, NULL, 120), "a message with a link");
    url[0] = 0;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].body && (p = strstr(mxt_ev[i].body, "http://")) != NULL) {
            snprintf(url, sizeof url, "%.*s", (int)strcspn(p, " \n"), p);
            break;
        }
    MXT_OK(amx_url_preview(c, url, &t, &d, &im) == 0 && t && !strcmp(t, "Amiga-Seite") && d
           && !strcmp(d, "Alles \xc3\xbc" "ber den Amiga") && im && !strncmp(im, "mxc://", 6),
           "preview: title, description, picture (%s / %s / %s; %s)", t ? t : "-", d ? d : "-", im ? im : "-",
           amx_error(c));
    free(t); free(d); free(im);
    snprintf(bad, sizeof bad, "%s", url);
    if ((p = strstr(bad, "seite.html")) != NULL) memcpy(p, "keine", 5);     /* .../keine.html: 404 */
    MXT_OK(amx_url_preview(c, bad, &t, &d, &im) != 0 && !t, "a missing page: no preview (%s)", amx_error(c));
    amx_send_text(c, room, "vorschau ok", NULL);
    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
