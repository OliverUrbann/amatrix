/* Feature: send pictures and files.
 * We create an encrypted and an unencrypted room with the peer and send:
 * a PNG (m.image with its size), the same PNG under a name without an
 * extension (recognised by its content, as usual on the Amiga), a 200 KB
 * file (m.file) - encrypted - and the PNG unencrypted.  The peer downloads
 * everything, decrypts, compares the bytes and answers "dateien ok". */
#include "mxtest.h"

static const unsigned char PNG[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x03\x00\x00\x00\x02\x08\x02"
    "\x00\x00\x00\x12\x16\xf1\x4d\x00\x00\x00\x14\x49\x44\x41\x54\x78\x5e\x63\x3c\x21\x22\xc2\x00\x06\x4c\x10"
    "\x8a\x81\x81\x01\x00\x11\xd0\x00\xf4\x62\xea\x4f\x10\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";
#define BIG 200000L

static char *path_in(const char *dir, const char *name)
{
    char *p = malloc(strlen(dir) + strlen(name) + 2);
    size_t n = strlen(dir);
    sprintf(p, "%s%s%s", dir, n && dir[n - 1] != '/' && dir[n - 1] != ':' ? "/" : "", name);
    return p;
}

static int write_file(const char *path, const unsigned char *d, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(d, 1, n, f) != (size_t)n) { fclose(f); return -1; }
    return fclose(f);
}

static AmxClient *cl;
static int send_one(const char *room, const char *path, const char *what)
{
    char *ev = NULL;
    int ok = amx_send_file(cl, room, path, NULL, NULL, &ev) == 0 && ev;
    MXT_OK(ok, "send %s (%s)", what, ev ? ev : amx_error(cl));
    free(ev);
    return ok;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxRoomOptions o;
    const char *invite[2];
    char *enc = NULL, *plain = NULL, *p_png, *p_bild, *p_big;
    unsigned char *big;
    MxtMember join;
    MxtMsg done;
    long i;
    int own;

    mxt_args(argc, argv, &a);
    cl = mxt_client(&a);
    p_png = path_in(a.store, "rot.png");
    p_bild = path_in(a.store, "Bild");
    p_big = path_in(a.store, "gross.bin");
    big = malloc(BIG);
    for (i = 0; i < BIG; i++) big[i] = (unsigned char)((i * 7 + i / 251) & 255);
    MXT_OK(write_file(p_png, PNG, sizeof PNG - 1) == 0 && write_file(p_bild, PNG, sizeof PNG - 1) == 0
           && write_file(p_big, big, BIG) == 0, "test files written");

    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Dateien verschluesselt";
    o.encrypted = 1;
    MXT_OK(amx_create_room(cl, &o, &enc) == 0, "create encrypted room");
    o.name = "Dateien offen";
    o.encrypted = 0;
    MXT_OK(amx_create_room(cl, &o, &plain) == 0, "create unencrypted room");
    if (!enc || !plain) return mxt_done();
    join.room = enc; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(cl, mxt_have_membership, &join, 180), "peer joined the encrypted room");
    join.room = plain;
    MXT_OK(mxt_sync_until(cl, mxt_have_membership, &join, 180), "peer joined the unencrypted room");

    send_one(enc, p_png, "encrypted picture");
    send_one(enc, p_bild, "encrypted picture without extension");
    send_one(enc, p_big, "encrypted 200 KB file");
    send_one(plain, p_png, "unencrypted picture");
    {
        char *ev = NULL;
        MXT_OK(amx_send_file(cl, enc, "does/not/exist", NULL, NULL, &ev) != 0 && !ev,
               "missing file: error (%s)", amx_error(cl));
    }

    done.room = enc; done.sender = a.peer; done.body = "dateien ok";
    MXT_OK(mxt_sync_until(cl, mxt_have_message, &done, 240), "peer got all files intact");

    /* our own attachments come back and can be shown */
    for (own = 0, i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].own && mxt_ev[i].media) own++;
    MXT_OK(own >= 4, "own attachments come back as media (%d)", own);
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].own && mxt_ev[i].media && mxt_ev[i].encrypted) {
            unsigned long *px = NULL;
            int w = 0, h = 0;
            char t[32];
            if (!amx_media_info(mxt_ev[i].media, 0, "type", t, sizeof t) || strcmp(t, "m.image")) continue;
            MXT_OK(amx_media_thumbnail(cl, mxt_ev[i].media, 0, 64, 64, &px, &w, &h) == 0 && w == 3 && h == 2,
                   "own encrypted picture decrypts (%dx%d)", w, h);
            free(px);
            break;
        }

    free(enc); free(plain); free(big);
    amx_client_free(cl);
    amx_cleanup();
    return mxt_done();
}
