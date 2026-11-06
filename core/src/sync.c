/* /sync: one round, dispatching to-device, invites, timelines, key upkeep. */
#include "amx_internal.h"

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
        if (amx__jstr(x, "type") && !strcmp(amx__jstr(x, "type"), "m.direct"))
            amx__set_direct(c, cJSON_GetObjectItemCaseSensitive(x, "content"));

    cJSON_ArrayForEach(room, cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetObjectItemCaseSensitive(r, "rooms"), "leave"))
        amx__room_forget(c, room->string);

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
    }
    amx__rooms_save(c);
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
    if (amx_backup_available(c)) amx__backup_for_events(c, room_id, cJSON_GetObjectItemCaseSensitive(r, "chunk"));
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
    return n;
}
