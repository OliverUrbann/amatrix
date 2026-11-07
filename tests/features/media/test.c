/* Feature: attachments.  The peer sends an encrypted PNG without a preview,
 * an encrypted JPEG with an encrypted preview, an encrypted text file and an
 * unencrypted picture (server preview).  We get each with a media
 * description, make previews that fit 128x96 (aspect kept, left red / right
 * blue resp. green) and save the file. */
#include "mxtest.h"

static char *room;

static int have_room(void *arg)
{
    AmxClient *c = arg;
    AmxRoomInfo *l;
    int n = amx_rooms(c, &l), i;
    for (i = 0; i < n && !room; i++)
        if (l[i].membership == AMX_ROOM_INVITED) room = mxt_dup(l[i].room_id);
    amx_free_rooms(l, n);
    return room != NULL;
}

static const char *media_of(const char *body)
{
    int i;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].body && !strcmp(mxt_ev[i].body, body))
            return mxt_ev[i].media;
    return NULL;
}

static int red(unsigned long p) { return ((p >> 16) & 255) > 150 && (p & 255) < 100; }
static int blue(unsigned long p) { return (p & 255) > 150 && ((p >> 16) & 255) < 100; }
static int green(unsigned long p) { return ((p >> 8) & 255) > 150 && ((p >> 16) & 255) < 100; }

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    MxtMsg m;
    unsigned long *px = NULL;
    int w = 0, h = 0;
    const char *md;
    char path[512], buf[64];
    FILE *f;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(mxt_sync_until(c, have_room, c, 240), "invited");
    if (!room) return mxt_done();
    MXT_OK(amx_join(c, room, NULL) == 0, "joined");
    m.room = room; m.sender = a.peer; m.body = "fertig";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 240), "all attachments arrived");

    md = media_of("rot-blau.png");
    MXT_OK(md != NULL, "encrypted picture has a media description");
    MXT_OK(md && amx_media_thumbnail(c, md, 0, 128, 96, &px, &w, &h) == 0, "preview from the picture itself (%s)",
           amx_error(c));
    MXT_OK(w == 128 && h == 96, "fitted into 128x96 (%dx%d)", w, h);
    MXT_OK(px && red(px[0]) && blue(px[w - 1]), "colours and orientation right (%08lx %08lx)",
           px ? px[0] : 0UL, px ? px[w - 1] : 0UL);
    free(px); px = NULL;

    md = media_of("gross.jpg");
    MXT_OK(md && amx_media_thumbnail(c, md, 0, 128, 96, &px, &w, &h) == 0, "preview from the encrypted preview");
    MXT_OK(w == 120 && h == 90, "small preview not enlarged (%dx%d: the preview, not the big picture)", w, h);
    MXT_OK(px && green(px[0]) && blue(px[w - 1]), "preview decrypted correctly");
    free(px); px = NULL;

    md = media_of("offen.png");
    MXT_OK(md && amx_media_thumbnail(c, md, 0, 128, 96, &px, &w, &h) == 0, "unencrypted: server preview (%s)",
           amx_error(c));
    MXT_OK(w == 128 && h == 96 && px && red(px[0]) && blue(px[w - 1]), "server preview fitted (%dx%d)", w, h);
    free(px); px = NULL;

    md = media_of("hallo.txt");
    snprintf(path, sizeof path, "%s/hallo.txt", a.store);
    MXT_OK(md && amx_media_save(c, md, 0, path) == 0, "file saved (%s)", amx_error(c));
    buf[0] = 0;
    if ((f = fopen(path, "rb")) != NULL) { buf[fread(buf, 1, sizeof buf - 1, f)] = 0; fclose(f); }
    MXT_OK(!strcmp(buf, "Hallo Datei aus dem Test\n"), "decrypted content right (%s)", buf);

    md = media_of("Zwei Bilder");
    MXT_OK(md && amx_media_count(md) == 2, "a gallery: two pictures in one message");
    MXT_OK(md && amx_media_thumbnail(c, md, 1, 128, 96, &px, &w, &h) == 0 && px && green(px[0]),
           "second picture of the gallery (%dx%d)", w, h);
    {
        char b[64];
        MXT_OK(md && amx_media_info(md, 1, "name", b, sizeof b) && !strcmp(b, "zwei.png"), "its name");
    }
    free(px); px = NULL;
    md = media_of("Aufkleber");
    MXT_OK(md && amx_media_count(md) == 1 && amx_media_thumbnail(c, md, 0, 128, 96, &px, &w, &h) == 0,
           "a sticker (m.sticker) is a picture too");
    free(px); px = NULL;

    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
