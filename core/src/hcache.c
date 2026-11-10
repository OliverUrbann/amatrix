/* The local history cache: the newest messages of each room (decrypted,
 * edits and deletions applied) in the store, so a chat window can show them
 * at once - before, and without, the network.  In an encrypted store
 * (passphrase) the file is encrypted like everything else. */
#include "amx_internal.h"

#define HC_FILE "history_cache.json"
#define HC_PER_ROOM 40                 /* messages kept per room */
#define HC_ROOMS 60                    /* rooms kept (the most recently active) */

static void hc_load(AmxClient *c)
{
    char *t;
    if (c->hc) return;
    t = amx__read_store(c, HC_FILE);
    c->hc = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!cJSON_IsObject(c->hc)) { cJSON_Delete(c->hc); c->hc = cJSON_CreateObject(); }
}

static cJSON *find_id(cJSON *arr, const char *id)
{
    cJSON *x;
    cJSON_ArrayForEach(x, arr) {
        const char *i = amx__jstr(x, "id");
        if (i && !strcmp(i, id)) return x;
    }
    return NULL;
}

static double ts_of(const cJSON *x)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(x, "ts");
    return cJSON_IsNumber(t) ? t->valuedouble : 0;
}

static void set_str(cJSON *o, const char *k, const char *v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    if (v) cJSON_AddStringToObject(o, k, v);
}

void amx__hcache_note(AmxClient *c, const AmxEvent *e)
{
    cJSON *arr, *x;
    if (c->hc_off || c->hc_emitting || !e->room_id) return;
    if (e->type != AMX_EV_MESSAGE && e->type != AMX_EV_EDIT && e->type != AMX_EV_REDACTION) return;
    hc_load(c);
    arr = cJSON_GetObjectItemCaseSensitive(c->hc, e->room_id);
    if (e->type == AMX_EV_MESSAGE) {
        int i, n, at;
        if (!e->event_id) return;
        if (!arr) arr = cJSON_AddArrayToObject(c->hc, e->room_id);
        if ((x = find_id(arr, e->event_id)) == NULL) {
            n = cJSON_GetArraySize(arr);
            /* oldest first; a message older than all kept ones of a full room is not needed */
            if (n >= HC_PER_ROOM && e->timestamp <= ts_of(cJSON_GetArrayItem(arr, 0))) return;
            x = cJSON_CreateObject();
            for (at = n, i = 0; i < n; i++)
                if (ts_of(cJSON_GetArrayItem(arr, i)) > e->timestamp) { at = i; break; }
            if (at == n) cJSON_AddItemToArray(arr, x);
            else cJSON_InsertItemInArray(arr, at, x);
            while (cJSON_GetArraySize(arr) > HC_PER_ROOM) cJSON_DeleteItemFromArray(arr, 0);
        } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "edited"))) {
            return;                       /* keep the edited text over the original */
        }
        set_str(x, "id", e->event_id);
        set_str(x, "sender", e->sender);
        set_str(x, "name", e->sender_name);
        set_str(x, "msgtype", e->msgtype);
        set_str(x, "body", e->body ? e->body : "");
        set_str(x, "formatted", e->formatted);
        set_str(x, "reply_to", e->reply_to);
        set_str(x, "thread_root", e->thread_root);
        set_str(x, "media", e->media);
        cJSON_DeleteItemFromObjectCaseSensitive(x, "ts");
        cJSON_AddNumberToObject(x, "ts", e->timestamp);
        cJSON_DeleteItemFromObjectCaseSensitive(x, "f");
        cJSON_AddNumberToObject(x, "f", (e->own ? 1 : 0) | (e->encrypted ? 2 : 0) | (e->redacted ? 4 : 0)
                                        | (e->mention ? 8 : 0));
        cJSON_DeleteItemFromObjectCaseSensitive(x, "tc");
        if (e->thread_count) cJSON_AddNumberToObject(x, "tc", e->thread_count);
    } else if (arr && e->relates_to && (x = find_id(arr, e->relates_to)) != NULL) {
        if (e->type == AMX_EV_EDIT) {
            set_str(x, "body", e->body ? e->body : "");
            set_str(x, "formatted", e->formatted);
            if (e->msgtype) set_str(x, "msgtype", e->msgtype);
            cJSON_DeleteItemFromObjectCaseSensitive(x, "edited");
            cJSON_AddTrueToObject(x, "edited");
        } else {
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(x, "f");
            int fl = cJSON_IsNumber(f) ? f->valueint : 0;
            set_str(x, "body", "");
            set_str(x, "formatted", NULL);
            set_str(x, "media", NULL);
            cJSON_DeleteItemFromObjectCaseSensitive(x, "f");
            cJSON_AddNumberToObject(x, "f", fl | 4);
        }
    } else return;
    c->hc_dirty = 1;
}

/* the newest message time of a room's cache */
static double newest(const cJSON *arr)
{
    return ts_of(cJSON_GetArrayItem(arr, cJSON_GetArraySize(arr) - 1));
}

void amx__hcache_save(AmxClient *c)
{
    if (!c->hc || !c->hc_dirty || c->hc_off) return;
    while (cJSON_GetArraySize(c->hc) > HC_ROOMS) {          /* the least recently active room goes */
        cJSON *x, *old = NULL;
        cJSON_ArrayForEach(x, c->hc) if (!old || newest(x) < newest(old)) old = x;
        cJSON_Delete(cJSON_DetachItemViaPointer(c->hc, old));
    }
    amx__save_json(c, HC_FILE, c->hc);
    c->hc_dirty = 0;
}

void amx__hcache_forget(AmxClient *c, const char *room_id)
{
    if (!c->hc && c->hc_off) return;
    hc_load(c);
    if (cJSON_GetObjectItemCaseSensitive(c->hc, room_id)) {
        cJSON_DeleteItemFromObjectCaseSensitive(c->hc, room_id);
        c->hc_dirty = 1;
    }
}

int amx_cached_history(AmxClient *c, const char *room_id)
{
    cJSON *arr, *x;
    int n = 0, i;
    if (c->hc_off) return 0;
    hc_load(c);
    arr = cJSON_GetObjectItemCaseSensitive(c->hc, room_id);
    c->hc_emitting = 1;
    for (i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {     /* newest first, like the history */
        AmxEvent e;
        const cJSON *f, *tc;
        int fl;
        x = cJSON_GetArrayItem(arr, i);
        memset(&e, 0, sizeof e);
        f = cJSON_GetObjectItemCaseSensitive(x, "f");
        tc = cJSON_GetObjectItemCaseSensitive(x, "tc");
        fl = cJSON_IsNumber(f) ? f->valueint : 0;
        e.type = AMX_EV_MESSAGE;
        e.room_id = room_id;
        e.event_id = amx__jstr(x, "id");
        e.sender = amx__jstr(x, "sender");
        e.sender_name = amx__jstr(x, "name");
        e.msgtype = amx__jstr(x, "msgtype");
        e.body = amx__jstr(x, "body");
        e.formatted = amx__jstr(x, "formatted");
        e.reply_to = amx__jstr(x, "reply_to");
        e.thread_root = amx__jstr(x, "thread_root");
        e.media = amx__jstr(x, "media");
        e.timestamp = ts_of(x);
        e.own = fl & 1;
        e.encrypted = (fl & 2) != 0;
        e.redacted = (fl & 4) != 0;
        e.mention = (fl & 8) != 0;
        e.thread_count = cJSON_IsNumber(tc) ? tc->valueint : 0;
        e.history = 1;
        e.cached = 1;
        if (!e.event_id) continue;
        amx__emit(c, &e);
        n++;
    }
    c->hc_emitting = 0;
    return n;
}

void amx_set_history_cache(AmxClient *c, int on)
{
    c->hc_off = !on;
    if (!on) {
        char *p = amx__malloc(strlen(c->store) + sizeof HC_FILE + 2);
        sprintf(p, "%s/%s", c->store, HC_FILE);
        remove(p);
        free(p);
        cJSON_Delete(c->hc);
        c->hc = NULL;
        c->hc_dirty = 0;
    }
}
