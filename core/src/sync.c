/* /sync: one round, dispatching to-device, invites, timelines, key upkeep. */
#include "amx_internal.h"

/* m.typing and m.receipt of a joined room */
static void handle_ephemeral(AmxClient *c, const char *room_id, const cJSON *events)
{
    const cJSON *ev, *x, *rt, *u;
    cJSON_ArrayForEach(ev, events) {
        const char *type = amx__jstr(ev, "type");
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content");
        AmxEvent e;
        memset(&e, 0, sizeof e);
        e.room_id = room_id;
        if (type && !strcmp(type, "m.typing")) {
            size_t n = 1;
            char *ids, *names;
            cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(ct, "user_ids"))
                if (cJSON_IsString(x)) n += strlen(x->valuestring) + 1;
            ids = amx__malloc(n);
            names = amx__malloc(n * 2 + 1);
            ids[0] = names[0] = 0;
            cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(ct, "user_ids"))
                if (cJSON_IsString(x) && (!c->user_id || strcmp(x->valuestring, c->user_id))) {
                    const char *nm = amx__member_name(c, room_id, x->valuestring);
                    char lp[128];
                    if (!nm) {                              /* the localpart */
                        const char *s = x->valuestring[0] == '@' ? x->valuestring + 1 : x->valuestring;
                        size_t k = strcspn(s, ":");
                        if (k >= sizeof lp) k = sizeof lp - 1;
                        memcpy(lp, s, k);
                        lp[k] = 0;
                        nm = lp;
                    }
                    if (ids[0]) { strcat(ids, "\n"); }
                    strcat(ids, x->valuestring);
                    if (strlen(names) + strlen(nm) + 2 < n * 2 + 1) {
                        if (names[0]) strcat(names, "\n");
                        strcat(names, nm);
                    }
                }
            e.type = AMX_EV_TYPING;
            e.state_key = ids;
            e.body = names;
            amx__emit(c, &e);
            free(ids);
            free(names);
        } else if (type && !strcmp(type, "m.receipt")) {
            cJSON_ArrayForEach(x, ct) {                    /* event ID -> receipt types */
                cJSON_ArrayForEach(rt, x) {
                    if (strcmp(rt->string, "m.read") && strcmp(rt->string, "m.read.private")) continue;
                    cJSON_ArrayForEach(u, rt) {               /* user -> {ts, thread_id} */
                        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(u, "ts");
                        const char *thr = amx__jstr(u, "thread_id");
                        if (c->user_id && !strcmp(u->string, c->user_id)) continue;
                        if (thr && strcmp(thr, "main")) continue;   /* a thread's own receipt */
                        e.type = AMX_EV_RECEIPT;
                        e.event_id = x->string;
                        e.sender = u->string;
                        e.timestamp = cJSON_IsNumber(ts) ? ts->valuedouble : 0;
                        amx__emit(c, &e);
                    }
                }
            }
        }
    }
}

static void handle_state(AmxClient *c, const char *room_id, const cJSON *events)
{
    const cJSON *ev;
    cJSON_ArrayForEach(ev, events) {
        const char *type = amx__jstr(ev, "type");
        if (type && !strcmp(type, "m.room.encryption")) amx__note_room_encryption(c, room_id, 1);
    }
}

int amx_sync(AmxClient *c, long timeout_ms)
{
    /* {"room":{"timeline":{"limit":20},"state":{"lazy_load_members":true}}} */
    static const char filter[] =
        "%7B%22room%22%3A%7B%22timeline%22%3A%7B%22limit%22%3A20%7D%2C%22state%22%3A%7B"
        "%22lazy_load_members%22%3Atrue%7D%7D%7D";
    char *path;
    long st;
    cJSON *r, *x, *room;
    int otk = -1, need_fb = 1;

    if (!c->token) return amx__fail(c, "not logged in");
    path = amx__malloc(sizeof filter + (c->since ? strlen(c->since) * 3 : 0) + 80);
    if (c->since) {
        char *esc = http_escape(c->since);
        sprintf(path, "/sync?timeout=%ld&filter=%s&since=%s", timeout_ms, filter, esc);
        free(esc);
    } else {
        sprintf(path, "/sync?timeout=0&filter=%s", filter);
    }
    http_interruptible(c->interruptible);
    r = amx__api(c, "GET", path, NULL, timeout_ms / 1000 + 30, &st);
    http_interruptible(0);
    free(path);
    if (st == -1) return 1;             /* woken up by the async layer */
    if (st != 200 || !r) { cJSON_Delete(r); return -1; }

    /* to-device first: the room keys for this batch's messages are here */
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(r, "to_device"), "events"))
        amx__handle_to_device(c, x);
    amx__retry_pending(c);

    /* device lists changed: fetch again before the next send */
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(r, "device_lists"), "changed"))
        if (cJSON_IsString(x)) amx__forget_devices(c, x->valuestring);

    /* account data: m.direct (which rooms are direct chats) */
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(r, "account_data"), "events"))
    {
        if (amx__jstr(x, "type") && !strcmp(amx__jstr(x, "type"), "m.direct"))
            amx__set_direct(c, cJSON_GetObjectItemCaseSensitive(x, "content"));
        else if (amx__jstr(x, "type") && !strcmp(amx__jstr(x, "type"), "m.push_rules"))
            amx__set_push_rules(c, cJSON_GetObjectItemCaseSensitive(x, "content"));
    }

    cJSON_ArrayForEach(room, cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetObjectItemCaseSensitive(r, "rooms"), "leave"))
    {
        amx__room_forget(c, room->string);
        amx__hcache_forget(c, room->string);
    }

    cJSON_ArrayForEach(room, cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetObjectItemCaseSensitive(r, "rooms"), "invite")) {
        AmxEvent e;
        const cJSON *sev;
        amx__room_sync_invite(c, room->string, room);
        memset(&e, 0, sizeof e);
        e.type = AMX_EV_INVITE;
        e.room_id = room->string;
        cJSON_ArrayForEach(sev, cJSON_GetObjectItemCaseSensitive(
                                    cJSON_GetObjectItemCaseSensitive(room, "invite_state"), "events")) {
            const char *t = amx__jstr(sev, "type"), *sk = amx__jstr(sev, "state_key");
            if (t && !strcmp(t, "m.room.member") && sk && !strcmp(sk, c->user_id))
                e.sender = amx__jstr(sev, "sender");
        }
        amx__emit(c, &e);
    }

    cJSON_ArrayForEach(room, cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetObjectItemCaseSensitive(r, "rooms"), "join")) {
        handle_state(c, room->string, cJSON_GetObjectItemCaseSensitive(
                                          cJSON_GetObjectItemCaseSensitive(room, "state"), "events"));
        amx__room_sync_join(c, room->string, room);
        cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(
                                  cJSON_GetObjectItemCaseSensitive(room, "timeline"), "events"))
            amx__handle_room_event(c, room->string, x, 0);
        handle_ephemeral(c, room->string, cJSON_GetObjectItemCaseSensitive(
                                              cJSON_GetObjectItemCaseSensitive(room, "ephemeral"), "events"));
    }
    amx__rooms_save(c);
    amx__rooms_upkeep(c);
    amx__hcache_save(c);
    amx__fetch_missing_keys(c);           /* keys still missing: backup, our other devices */

    x = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(r, "device_one_time_keys_count"), "signed_curve25519");
    otk = cJSON_IsNumber(x) ? x->valueint : 0;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "device_unused_fallback_key_types"))
        if (cJSON_IsString(x) && !strcmp(x->valuestring, "signed_curve25519")) need_fb = 0;
    amx__upload_keys(c, otk, need_fb);

    if (amx__jstr(r, "next_batch")) {
        free(c->since);
        c->since = amx__strdup(amx__jstr(r, "next_batch"));
        amx__write_store(c, "since", c->since);
    }
    cJSON_Delete(r);
    return 0;
}

/* ------------------------------------------------------------ history */

int amx_load_history(AmxClient *c, const char *room_id, int limit, int from_newest, int *at_start)
{
    const char *from = from_newest ? NULL : amx__jstr(c->hist, room_id);
    char *esc = http_escape(room_id), *fesc = from ? http_escape(from) : NULL, *path;
    cJSON *r, *x;
    int n = 0;
    long st;

    if (at_start) *at_start = 0;
    if (!c->hist) c->hist = cJSON_CreateObject();
    if (!from_newest && !from && cJSON_GetObjectItemCaseSensitive(c->hist, room_id)) {
        /* we already reached the beginning */
        free(esc);
        if (at_start) *at_start = 1;
        return 0;
    }
    path = amx__malloc(strlen(esc) + (fesc ? strlen(fesc) : 0) + 80);
    sprintf(path, "/rooms/%s/messages?dir=b&limit=%d%s%s", esc, limit > 0 ? limit : 20,
            fesc ? "&from=" : "", fesc ? fesc : "");
    free(esc);
    free(fesc);
    r = amx__api(c, "GET", path, NULL, 60, &st);
    free(path);
    if (st != 200 || !r) { cJSON_Delete(r); return -1; }
    LOG(c, "history %s: %d events", room_id, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "chunk")));
    /* keys we lack for this page: only those sessions from the key backup */
    if (amx_backup_available(c)) amx__backup_for_events(c, room_id, cJSON_GetObjectItemCaseSensitive(r, "chunk"), 1);
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "chunk")) {
        LOG2(c, "  history event %s %s", amx__jstr(x, "type") ? amx__jstr(x, "type") : "?",
             amx__jstr(x, "event_id") ? amx__jstr(x, "event_id") : "?");
        amx__handle_room_event(c, room_id, x, 1);
        n++;
    }
    amx__fetch_missing_keys(c);           /* still missing: ask our other devices */
    LOG(c, "history %s: done", room_id);
    cJSON_DeleteItemFromObjectCaseSensitive(c->hist, room_id);
    if (amx__jstr(r, "end") && n > 0) {
        cJSON_AddStringToObject(c->hist, room_id, amx__jstr(r, "end"));
    } else {
        cJSON_AddNullToObject(c->hist, room_id);      /* beginning reached */
        if (at_start) *at_start = 1;
    }
    cJSON_Delete(r);
    amx__hcache_save(c);
    return n;
}

int amx_load_thread(AmxClient *c, const char *room_id, const char *root, int limit, int from_newest, int *at_start)
{
    char *key = amx__malloc(strlen(room_id) + strlen(root) + 4), *esc, *resc, *fesc, *path;
    const char *from;
    cJSON *r, *x;
    int n = 0;
    long st;

    sprintf(key, "t:%s|%s", room_id, root);
    if (at_start) *at_start = 0;
    if (!c->hist) c->hist = cJSON_CreateObject();
    from = from_newest ? NULL : amx__jstr(c->hist, key);
    if (!from_newest && !from && cJSON_GetObjectItemCaseSensitive(c->hist, key)) {
        free(key);
        if (at_start) *at_start = 1;
        return 0;
    }
    esc = http_escape(room_id);
    resc = http_escape(root);
    fesc = from ? http_escape(from) : NULL;
    path = amx__malloc(strlen(esc) + strlen(resc) + (fesc ? strlen(fesc) : 0) + 120);
    sprintf(path, "/_matrix/client/v1/rooms/%s/relations/%s/m.thread?dir=b&limit=%d%s%s", esc, resc,
            limit > 0 ? limit : 20, fesc ? "&from=" : "", fesc ? fesc : "");
    free(fesc);
    r = amx__api(c, "GET", path, NULL, 60, &st);
    free(path);
    if (st != 200 || !r) { cJSON_Delete(r); free(esc); free(resc); free(key); return -1; }
    if (amx_backup_available(c)) amx__backup_for_events(c, room_id, cJSON_GetObjectItemCaseSensitive(r, "chunk"), 1);
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "chunk")) {
        amx__handle_room_event(c, room_id, x, 1);
        n++;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(c->hist, key);
    if (amx__jstr(r, "next_batch")) cJSON_AddStringToObject(c->hist, key, amx__jstr(r, "next_batch"));
    else {                                                  /* all there: the root on top */
        cJSON *ev;
        cJSON_AddNullToObject(c->hist, key);
        if (at_start) *at_start = 1;
        path = amx__malloc(strlen(esc) + strlen(resc) + 40);
        sprintf(path, "/rooms/%s/event/%s", esc, resc);
        ev = amx__api(c, "GET", path, NULL, 60, &st);
        free(path);
        if (st == 200 && ev) {
            if (amx_backup_available(c)) {
                cJSON *one = cJSON_CreateArray();
                cJSON_AddItemReferenceToArray(one, ev);
                amx__backup_for_events(c, room_id, one, 1);
                cJSON_Delete(one);
            }
            amx__handle_room_event(c, room_id, ev, 1);
            n++;
        }
        cJSON_Delete(ev);
    }
    amx__fetch_missing_keys(c);
    cJSON_Delete(r);
    free(esc); free(resc); free(key);
    amx__hcache_save(c);
    return n;
}

static int load_rel(AmxClient *c, const char *room_id, const char *event_id, const char *rel);

int amx_load_relations(AmxClient *c, const char *room_id, const char *event_id)
{
    return load_rel(c, room_id, event_id, "m.reference");
}

int amx_load_edits(AmxClient *c, const char *room_id, const char *event_id)
{
    int n;
    c->lookup_mode = 1;
    n = load_rel(c, room_id, event_id, "m.replace");
    c->lookup_mode = 0;
    return n;
}

static int load_rel(AmxClient *c, const char *room_id, const char *event_id, const char *rel)
{
    char *esc = http_escape(room_id), *eesc = http_escape(event_id), *fesc = NULL, *path, *from = NULL;
    int n = 0, pages = 0;
    for (;;) {
        cJSON *r, *x;
        long st;
        path = amx__malloc(strlen(esc) + strlen(eesc) + (fesc ? strlen(fesc) : 0) + 120);
        sprintf(path, "/_matrix/client/v1/rooms/%s/relations/%s/%s?limit=100%s%s", esc, eesc, rel,
                fesc ? "&from=" : "", fesc ? fesc : "");
        free(fesc);
        fesc = NULL;
        r = amx__api(c, "GET", path, NULL, 60, &st);
        free(path);
        if (st != 200 || !r) { cJSON_Delete(r); if (!pages) n = -1; break; }
        if (amx_backup_available(c)) amx__backup_for_events(c, room_id, cJSON_GetObjectItemCaseSensitive(r, "chunk"), 1);
        cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "chunk")) {
            amx__handle_room_event(c, room_id, x, 1);
            n++;
        }
        from = (char *)amx__jstr(r, "next_batch");
        if (from && ++pages < 10) fesc = http_escape(from);
        cJSON_Delete(r);
        if (!fesc) break;
    }
    free(esc);
    free(eesc);
    return n;
}
