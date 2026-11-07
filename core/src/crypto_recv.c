/* Receiving side of E2EE: Olm to-device (room keys), Megolm room events,
 * session storage, pending events.  Also dispatches plain room events. */
#include "amx_internal.h"
#include <time.h>

/* ------------------------------------------------------------- sessions */

OlmSession *amx__new_olm_session(void) { return olm_session(amx__malloc(olm_session_size())); }

OlmInboundGroupSession *amx__new_group_session(void)
{
    return olm_inbound_group_session(amx__malloc(olm_inbound_group_session_size()));
}

void amx__add_olm_session(AmxClient *c, const char *sender_key, OlmSession *s)
{
    c->olm = realloc(c->olm, sizeof *c->olm * (c->n_olm + 1));
    if (!c->olm) { fprintf(AMX_ERR, "amatrix: out of memory\n"); exit(20); }
    c->olm[c->n_olm].sender_key = amx__strdup(sender_key);
    c->olm[c->n_olm].s = s;
    c->n_olm++;
}

void amx__add_group_session(AmxClient *c, const char *room, const char *sid, const char *sender_key,
                            const char *ed, OlmInboundGroupSession *s)
{
    GroupSess *g;
    c->grp = realloc(c->grp, sizeof *c->grp * (c->n_grp + 1));
    if (!c->grp) { fprintf(AMX_ERR, "amatrix: out of memory\n"); exit(20); }
    g = &c->grp[c->n_grp++];
    g->room_id = amx__strdup(room);
    g->session_id = amx__strdup(sid);
    g->sender_key = amx__strdup(sender_key);
    g->ed25519 = amx__strdup(ed ? ed : "-");
    g->s = s;
}

/* A session we got again (backup, forwarded): kept if we had none, or if it
 * reaches further back than ours (ours may start at a later message index,
 * e.g. shared with us after some messages were sent).  1 = taken (s now
 * belongs to the client), 0 = not better (s freed). */
int amx__put_group_session(AmxClient *c, const char *room, const char *sid, const char *sender_key,
                           const char *ed, OlmInboundGroupSession *s)
{
    GroupSess *g = amx__find_group(c, room, sid);
    if (g) {
        uint32_t have = olm_inbound_group_session_first_known_index(g->s),
                 got = olm_inbound_group_session_first_known_index(s);
        if (got >= have) { olm_clear_inbound_group_session(s); free(s); return 0; }
        LOG(c, "Megolm: session %.12s... now known from index %lu (was %lu)", sid,
            (unsigned long)got, (unsigned long)have);
        olm_clear_inbound_group_session(g->s);
        free(g->s);
        g->s = s;
        return 1;
    }
    amx__add_group_session(c, room, sid, sender_key, ed, s);
    return 1;
}

GroupSess *amx__find_group(AmxClient *c, const char *room_id, const char *session_id)
{
    int i;
    for (i = 0; i < c->n_grp; i++)
        if (!strcmp(c->grp[i].room_id, room_id) && !strcmp(c->grp[i].session_id, session_id))
            return &c->grp[i];
    return NULL;
}

int amx__save_olm_sessions(AmxClient *c)
{
    size_t cap = 1, len = 0;
    char *out;
    int i, r;
    for (i = 0; i < c->n_olm; i++)
        cap += strlen(c->olm[i].sender_key) + olm_pickle_session_length(c->olm[i].s) + 2;
    out = amx__malloc(cap);
    out[0] = 0;
    for (i = 0; i < c->n_olm; i++) {
        size_t n = olm_pickle_session_length(c->olm[i].s);
        len += sprintf(out + len, "%s ", c->olm[i].sender_key);
        olm_pickle_session(c->olm[i].s, PK, out + len, n);
        len += n;
        out[len++] = '\n';
        out[len] = 0;
    }
    r = amx__write_store(c, "olm_sessions", out);
    free(out);
    return r;
}

int amx__save_group_sessions(AmxClient *c)
{
    size_t cap = 1, len = 0;
    char *out;
    int i, r;
    for (i = 0; i < c->n_grp; i++) {
        GroupSess *g = &c->grp[i];
        cap += strlen(g->room_id) + strlen(g->session_id) + strlen(g->sender_key) + strlen(g->ed25519)
             + olm_pickle_inbound_group_session_length(g->s) + 6;
    }
    out = amx__malloc(cap);
    out[0] = 0;
    for (i = 0; i < c->n_grp; i++) {
        GroupSess *g = &c->grp[i];
        size_t n = olm_pickle_inbound_group_session_length(g->s);
        len += sprintf(out + len, "%s %s %s %s ", g->room_id, g->session_id, g->sender_key, g->ed25519);
        olm_pickle_inbound_group_session(g->s, PK, out + len, n);
        len += n;
        out[len++] = '\n';
        out[len] = 0;
    }
    r = amx__write_store(c, "megolm_sessions", out);
    free(out);
    return r;
}

static int split_fields(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max && *p) {
        f[n++] = p;
        p = strchr(p, ' ');
        if (!p) break;
        *p++ = 0;
    }
    return n;
}

int amx__load_sessions(AmxClient *c)
{
    char *data, *line, *next;
    if ((data = amx__read_store(c, "olm_sessions"))) {
        for (line = data; line && *line; line = next) {
            char *f[2];
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (split_fields(line, f, 2) == 2) {
                OlmSession *s = amx__new_olm_session();
                if (olm_unpickle_session(s, PK, f[1], strlen(f[1])) == olm_error()) {
                    free(s); free(data);
                    return amx__fail(c, "cannot unpickle Olm session");
                }
                amx__add_olm_session(c, f[0], s);
            }
        }
        free(data);
    }
    if ((data = amx__read_store(c, "megolm_sessions"))) {
        for (line = data; line && *line; line = next) {
            char *f[5];
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (split_fields(line, f, 5) == 5) {
                OlmInboundGroupSession *s = amx__new_group_session();
                if (olm_unpickle_inbound_group_session(s, PK, f[4], strlen(f[4])) == olm_error()) {
                    free(s); free(data);
                    return amx__fail(c, "cannot unpickle Megolm session");
                }
                amx__add_group_session(c, f[0], f[1], f[2], f[3], s);
            }
        }
        free(data);
    }
    LOG(c, "loaded %d Olm and %d Megolm sessions", c->n_olm, c->n_grp);
    return 0;
}

/* ------------------------------------------------------- Olm (to-device) */

/* Decrypts one Olm message from sender_key; malloc'd plaintext or NULL. */
static char *olm_decrypt_from(AmxClient *c, const char *sender_key, int type, const char *body)
{
    size_t blen = strlen(body), max, n;
    char *tmp, *plain;
    int i;
    OlmSession *s;

    for (i = c->n_olm - 1; i >= 0; i--) {
        if (strcmp(c->olm[i].sender_key, sender_key)) continue;
        s = c->olm[i].s;
        if (type == 0) {
            tmp = amx__strndup(body, blen);
            n = olm_matches_inbound_session_from(s, sender_key, strlen(sender_key), tmp, blen);
            free(tmp);
            if (n != 1) continue;
        }
        tmp = amx__strndup(body, blen);
        max = olm_decrypt_max_plaintext_length(s, type, tmp, blen);
        free(tmp);
        if (max == olm_error()) continue;
        plain = amx__malloc(max + 1);
        tmp = amx__strndup(body, blen);
        n = olm_decrypt(s, type, tmp, blen, plain, max);
        free(tmp);
        if (n == olm_error()) { free(plain); continue; }
        plain[n] = 0;
        amx__save_olm_sessions(c);
        LOG(c, "Olm: decrypted with existing session %d", i);
        return plain;
    }
    if (type != 0) {
        amx__fail(c, "Olm: no session for message from %s", sender_key);
        return NULL;
    }
    /* pre-key message: new inbound session using one of our one-time keys */
    s = amx__new_olm_session();
    tmp = amx__strndup(body, blen);
    n = olm_create_inbound_session_from(s, c->acct, sender_key, strlen(sender_key), tmp, blen);
    free(tmp);
    if (n == olm_error()) {
        amx__fail(c, "Olm: cannot create inbound session: %s", olm_session_last_error(s));
        free(s);
        return NULL;
    }
    olm_remove_one_time_keys(c->acct, s);
    tmp = amx__strndup(body, blen);
    max = olm_decrypt_max_plaintext_length(s, 0, tmp, blen);
    free(tmp);
    plain = amx__malloc(max + 1);
    tmp = amx__strndup(body, blen);
    n = olm_decrypt(s, 0, tmp, blen, plain, max);
    free(tmp);
    if (n == olm_error()) {
        amx__fail(c, "Olm: decrypt failed: %s", olm_session_last_error(s));
        free(plain); free(s);
        return NULL;
    }
    plain[n] = 0;
    amx__add_olm_session(c, sender_key, s);
    amx__save_account(c);
    amx__save_olm_sessions(c);
    LOG(c, "Olm: new inbound session from %s", sender_key);
    return plain;
}

static void handle_room_key(AmxClient *c, const cJSON *payload, const char *sender,
                            const char *sender_key, const char *sender_ed)
{
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(payload, "content");
    const char *alg = amx__jstr(ct, "algorithm"), *room = amx__jstr(ct, "room_id"),
               *sid = amx__jstr(ct, "session_id"), *key = amx__jstr(ct, "session_key");
    OlmInboundGroupSession *s;
    char *k, idbuf[128];
    size_t n;
    AmxEvent ev;

    if (!alg || strcmp(alg, MEGOLM_ALG) || !room || !sid || !key) {
        LOG(c, "m.room_key: unsupported or incomplete");
        return;
    }
    if (amx__find_group(c, room, sid)) { LOG(c, "m.room_key: already known"); return; }
    s = amx__new_group_session();
    k = amx__strdup(key);
    if (olm_init_inbound_group_session(s, (uint8_t *)k, strlen(k)) == olm_error()) {
        LOG(c, "m.room_key: %s", olm_inbound_group_session_last_error(s));
        free(k); free(s);
        return;
    }
    free(k);
    n = olm_inbound_group_session_id(s, (uint8_t *)idbuf, sizeof idbuf - 1);
    idbuf[n == olm_error() ? 0 : n] = 0;
    if (strcmp(idbuf, sid)) { LOG(c, "m.room_key: session id mismatch"); free(s); return; }
    amx__add_group_session(c, room, sid, sender_key, sender_ed, s);
    amx__save_group_sessions(c);
    LOG(c, "got room key for %s (session %.12s...)", room, sid);
    memset(&ev, 0, sizeof ev);
    ev.type = AMX_EV_ROOM_KEY;
    ev.room_id = room;
    ev.sender = sender;
    ev.encrypted = 1;
    amx__emit(c, &ev);
}

/* m.forwarded_room_key: a room key one of OUR other devices sends on our
 * request (Element does this for its own verified devices).  Accepted only
 * from our own user and only for sessions we asked for. */
static void handle_forwarded_key(AmxClient *c, const cJSON *payload, const char *sender)
{
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(payload, "content");
    const char *alg = amx__jstr(ct, "algorithm"), *room = amx__jstr(ct, "room_id"),
               *sid = amx__jstr(ct, "session_id"), *key = amx__jstr(ct, "session_key"),
               *orig = amx__jstr(ct, "sender_key");
    OlmInboundGroupSession *s;
    char *k, idbuf[128], *rk;
    size_t n;
    int asked;
    AmxEvent ev;

    if (!sender || !c->user_id || strcmp(sender, c->user_id)) {
        LOG(c, "forwarded room key from another user, ignored");
        return;
    }
    if (!alg || strcmp(alg, MEGOLM_ALG) || !room || !sid || !key || !orig) {
        LOG(c, "forwarded room key: unsupported or incomplete");
        return;
    }
    rk = amx__malloc(strlen(room) + strlen(sid) + 2);
    sprintf(rk, "%s|%s", room, sid);
    asked = cJSON_GetObjectItemCaseSensitive(c->kreq, rk) != NULL;
    free(rk);
    if (!asked) { LOG(c, "forwarded room key we did not ask for, ignored"); return; }
    s = amx__new_group_session();
    k = amx__strdup(key);
    if (olm_import_inbound_group_session(s, (uint8_t *)k, strlen(k)) == olm_error()) {
        LOG(c, "forwarded room key: %s", olm_inbound_group_session_last_error(s));
        free(k); free(s);
        return;
    }
    free(k);
    n = olm_inbound_group_session_id(s, (uint8_t *)idbuf, sizeof idbuf - 1);
    idbuf[n == olm_error() ? 0 : n] = 0;
    if (strcmp(idbuf, sid)) { LOG(c, "forwarded room key: session id mismatch"); free(s); return; }
    if (!amx__put_group_session(c, room, sid, orig, amx__jstr(ct, "sender_claimed_ed25519_key"), s)) return;
    amx__save_group_sessions(c);
    LOG(c, "got forwarded room key for %s (session %.12s...)", room, sid);
    memset(&ev, 0, sizeof ev);
    ev.type = AMX_EV_ROOM_KEY;
    ev.room_id = room;
    ev.sender = sender;
    ev.encrypted = 1;
    amx__emit(c, &ev);
}

void amx__handle_to_device(AmxClient *c, const cJSON *ev)
{
    const char *type = amx__jstr(ev, "type"), *sender = amx__jstr(ev, "sender");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content"), *mine, *t;
    const char *sk, *body, *ptype;
    cJSON *payload;
    char *plain;

    LOG(c, "to-device %s from %s", type ? type : "?", sender ? sender : "?");
    if (!type || strcmp(type, "m.room.encrypted") || !sender) return;
    if (!amx__jstr(ct, "algorithm") || strcmp(amx__jstr(ct, "algorithm"), OLM_ALG)) return;
    sk = amx__jstr(ct, "sender_key");
    mine = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ct, "ciphertext"), c->curve);
    body = amx__jstr(mine, "body");
    t = cJSON_GetObjectItemCaseSensitive(mine, "type");
    if (!sk || !body || !cJSON_IsNumber(t)) { LOG(c, "to-device: not for this device"); return; }

    plain = olm_decrypt_from(c, sk, t->valueint, body);
    if (!plain) return;
    payload = cJSON_Parse(plain);
    free(plain);
    if (!payload) return;
    /* the checks the spec requires before trusting an Olm payload */
    if (!amx__jstr(payload, "sender") || strcmp(amx__jstr(payload, "sender"), sender) ||
        !amx__jstr(payload, "recipient") || strcmp(amx__jstr(payload, "recipient"), c->user_id) ||
        !amx__jstr(cJSON_GetObjectItemCaseSensitive(payload, "recipient_keys"), "ed25519") ||
        strcmp(amx__jstr(cJSON_GetObjectItemCaseSensitive(payload, "recipient_keys"), "ed25519"), c->ed)) {
        LOG(c, "Olm payload failed sender/recipient checks, dropped");
        cJSON_Delete(payload);
        return;
    }
    ptype = amx__jstr(payload, "type");
    LOG(c, "Olm payload from %s: %s", sender, ptype ? ptype : "?");
    if (ptype && !strcmp(ptype, "m.room_key"))
        handle_room_key(c, payload, sender, sk,
                        amx__jstr(cJSON_GetObjectItemCaseSensitive(payload, "keys"), "ed25519"));
    else if (ptype && !strcmp(ptype, "m.forwarded_room_key"))
        handle_forwarded_key(c, payload, sender);
    cJSON_Delete(payload);
}

/* ---------------------------------------------------------- room events */

static void stamp(AmxEvent *e, const cJSON *ev, int history)
{
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(ev, "origin_server_ts");
    e->history = history;
    e->timestamp = cJSON_IsNumber(ts) ? ts->valuedouble : 0;
}

static void emit_message(AmxClient *c, const char *room_id, const cJSON *ev, const char *type,
                         const cJSON *content, int encrypted, int history)
{
    AmxEvent e;
    const char *sender = amx__jstr(ev, "sender");
    if (!strcmp(type, "m.reaction")) {
        const cJSON *rel = cJSON_GetObjectItemCaseSensitive(content, "m.relates_to");
        if (!rel) rel = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ev, "content"),
                                                         "m.relates_to");
        if (!amx__jstr(rel, "rel_type") || strcmp(amx__jstr(rel, "rel_type"), "m.annotation")
            || !amx__jstr(rel, "event_id") || !amx__jstr(rel, "key"))
            return;                                     /* redacted or not an annotation */
        memset(&e, 0, sizeof e);
        e.type = AMX_EV_REACTION;
        e.relates_to = amx__jstr(rel, "event_id");
        e.body = amx__jstr(rel, "key");
    } else if (!strcmp(type, "m.room.redaction")) {
        memset(&e, 0, sizeof e);
        e.type = AMX_EV_REDACTION;
        e.relates_to = amx__jstr(ev, "redacts") ? amx__jstr(ev, "redacts") : amx__jstr(content, "redacts");
        if (!e.relates_to) return;
    } else if (strcmp(type, "m.room.message")) {
        return;                                         /* other types: later features */
    } else {
        memset(&e, 0, sizeof e);
        e.type = AMX_EV_MESSAGE;
        e.msgtype = amx__jstr(content, "msgtype");
        e.body = amx__jstr(content, "body") ? amx__jstr(content, "body") : "";
    }
    e.room_id = room_id;
    e.event_id = amx__jstr(ev, "event_id");
    e.sender = sender;
    e.encrypted = encrypted;
    e.own = sender && c->user_id && !strcmp(sender, c->user_id);
    stamp(&e, ev, history);
    amx__emit(c, &e);
}

static void emit_undecryptable(AmxClient *c, const char *room_id, const cJSON *ev, const char *reason, int history)
{
    AmxEvent e;
    memset(&e, 0, sizeof e);
    e.type = AMX_EV_UNDECRYPTABLE;
    e.room_id = room_id;
    e.event_id = amx__jstr(ev, "event_id");
    e.sender = amx__jstr(ev, "sender");
    e.reason = reason;
    e.encrypted = 1;
    e.own = e.sender && c->user_id && !strcmp(e.sender, c->user_id);
    {
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content");
        const char *sid = amx__jstr(ct, "session_id"), *dev = amx__jstr(ct, "device_id");
        const char *osid = amx__jstr(cJSON_GetObjectItemCaseSensitive(c->outbound, room_id), "sid");
        LOG(c, "undecryptable %s in %s from %s device %s session %.16s...: %s%s%s",
            e.event_id ? e.event_id : "-", room_id, e.sender ? e.sender : "-", dev ? dev : "-",
            sid ? sid : "-", reason, dev && c->device_id && !strcmp(dev, c->device_id) ? " (THIS device)" : "",
            osid && sid && !strcmp(osid, sid) ? " (our current outbound session)" : "");
    }
    stamp(&e, ev, history);
    amx__emit(c, &e);
}

/* Replay protection: one event per (session, message index). */
static int is_replay(AmxClient *c, const char *sid, uint32_t idx, const char *event_id)
{
    char key[160];
    const char *prev;
    if (!event_id) return 0;
    if (!c->seen) c->seen = cJSON_CreateObject();
    snprintf(key, sizeof key, "%s|%lu", sid, (unsigned long)idx);
    prev = amx__jstr(c->seen, key);
    if (prev) return strcmp(prev, event_id) != 0;
    cJSON_AddStringToObject(c->seen, key, event_id);
    return 0;
}

/* 1 = handled (decrypted or never decryptable), 0 = key missing (retry later) */
static int try_megolm(AmxClient *c, const char *room_id, const cJSON *ev, int report, int history)
{
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content");
    const char *sid = amx__jstr(ct, "session_id"), *cipher = amx__jstr(ct, "ciphertext");
    GroupSess *g;
    uint8_t *tmp, *plain;
    size_t max, n, len;
    uint32_t idx;
    cJSON *p;

    if (!amx__jstr(ct, "algorithm") || strcmp(amx__jstr(ct, "algorithm"), MEGOLM_ALG) || !sid || !cipher) {
        if (report) emit_undecryptable(c, room_id, ev, "unsupported algorithm", history);
        return 1;
    }
    g = amx__find_group(c, room_id, sid);
    if (!g) {
        if (report) emit_undecryptable(c, room_id, ev, "no key", history);
        return 0;
    }
    len = strlen(cipher);
    tmp = (uint8_t *)amx__strndup(cipher, len);
    max = olm_group_decrypt_max_plaintext_length(g->s, tmp, len);
    free(tmp);
    if (max == olm_error()) {
        emit_undecryptable(c, room_id, ev, olm_inbound_group_session_last_error(g->s), history);
        return 1;
    }
    plain = amx__malloc(max + 1);
    tmp = (uint8_t *)amx__strndup(cipher, len);
    n = olm_group_decrypt(g->s, tmp, len, plain, max, &idx);
    free(tmp);
    if (n == olm_error()) {
        /* UNKNOWN_MESSAGE_INDEX: sent before the point our copy of the key
         * starts at; a fuller copy may come (backup, our other devices) */
        int later = !strcmp(olm_inbound_group_session_last_error(g->s), "UNKNOWN_MESSAGE_INDEX");
        if (report || !later) emit_undecryptable(c, room_id, ev, olm_inbound_group_session_last_error(g->s), history);
        free(plain);
        return later ? 0 : 1;
    }
    plain[n] = 0;
    if (is_replay(c, sid, idx, amx__jstr(ev, "event_id"))) {
        LOG(c, "Megolm: replayed message index %lu in %s, dropped", (unsigned long)idx, room_id);
        emit_undecryptable(c, room_id, ev, "replayed message index", history);
        free(plain);
        return 1;
    }
    p = cJSON_Parse((char *)plain);
    free(plain);
    if (!p || !amx__jstr(p, "room_id") || strcmp(amx__jstr(p, "room_id"), room_id) || !amx__jstr(p, "type")) {
        emit_undecryptable(c, room_id, ev, "payload room_id mismatch", history);
        cJSON_Delete(p);
        return 1;
    }
    emit_message(c, room_id, ev, amx__jstr(p, "type"), cJSON_GetObjectItemCaseSensitive(p, "content"), 1, history);
    cJSON_Delete(p);
    return 1;
}

void amx__handle_room_event(AmxClient *c, const char *room_id, const cJSON *ev, int history)
{
    const char *type = amx__jstr(ev, "type");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content");
    if (!type) return;
    if (!strcmp(type, "m.room.encrypted")) {
        if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ev, "unsigned"), "redacted_because")
            && !amx__jstr(ct, "ciphertext"))
            return;                       /* removed: nothing left to show or decrypt */
        if (!try_megolm(c, room_id, ev, 1, history)) {
            /* the key may still come (to-device): try again after each sync,
             * also for history; once per event */
            const char *id = amx__jstr(ev, "event_id");
            const cJSON *x;
            int dup = 0;
            cJSON_ArrayForEach(x, c->pending)
                if (id && amx__jstr(x, "event_id") && !strcmp(amx__jstr(x, "event_id"), id)) dup = 1;
            if (!dup) {
                cJSON *e;
                while (cJSON_GetArraySize(c->pending) >= 300)      /* keep the oldest out */
                    cJSON_DeleteItemFromArray(c->pending, 0);
                e = cJSON_Duplicate(ev, 1);
                cJSON_AddStringToObject(e, "_room_id", room_id);
                if (history) cJSON_AddTrueToObject(e, "_history");
                cJSON_AddItemToArray(c->pending, e);
            }
        }
    } else if (!strcmp(type, "m.room.encryption")) {
        amx__note_room_encryption(c, room_id, 1);
    } else if (!strcmp(type, "m.room.member")) {
        AmxEvent e;
        memset(&e, 0, sizeof e);
        e.type = AMX_EV_MEMBERSHIP;
        e.room_id = room_id;
        e.event_id = amx__jstr(ev, "event_id");
        e.sender = amx__jstr(ev, "sender");
        e.state_key = amx__jstr(ev, "state_key");
        e.body = amx__jstr(ct, "membership");
        stamp(&e, ev, history);
        amx__emit(c, &e);
    } else {
        emit_message(c, room_id, ev, type, ct, 0, history);
    }
}

void amx__retry_pending(AmxClient *c)
{
    cJSON *e = c->pending->child, *next;
    while (e) {
        next = e->next;
        if (try_megolm(c, amx__jstr(e, "_room_id"), e, 0,
                       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "_history"))))
            cJSON_Delete(cJSON_DetachItemViaPointer(c->pending, e));
        e = next;
    }
}

/* Sends m.room_key_request for one session to all our other devices. */
static int request_key(AmxClient *c, const char *room, const char *sid, const char *sender_key)
{
    cJSON *req, *ct, *body, *res;
    char *txn, *path, rid[40];
    sprintf(rid, "amxkr%lu", ++c->txn);
    ct = cJSON_CreateObject();
    cJSON_AddStringToObject(ct, "action", "request");
    cJSON_AddStringToObject(ct, "request_id", rid);
    cJSON_AddStringToObject(ct, "requesting_device_id", c->device_id);
    body = cJSON_AddObjectToObject(ct, "body");
    cJSON_AddStringToObject(body, "algorithm", MEGOLM_ALG);
    cJSON_AddStringToObject(body, "room_id", room);
    cJSON_AddStringToObject(body, "session_id", sid);
    if (sender_key) cJSON_AddStringToObject(body, "sender_key", sender_key);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(req, "messages"), c->user_id), "*", ct);
    txn = amx__txn(c);
    path = amx__malloc(strlen(txn) + 48);
    sprintf(path, "/sendToDevice/m.room_key_request/%s", txn);
    res = amx__api_ok(c, "PUT", path, req);
    cJSON_Delete(req);
    free(path); free(txn);
    if (!res) return -1;
    cJSON_Delete(res);
    LOG(c, "asked our other devices for room key %s (session %.12s...)", room, sid);
    return 0;
}

/* Our other devices may be offline or not trust us yet: ask again after 1, 5
 * and 30 minutes (each request is answered or dropped by the other side). */
static int kreq_due(AmxClient *c, const char *key)
{
    static const long delay[] = { 60, 300, 1800 };
    cJSON *e = cJSON_GetObjectItemCaseSensitive(c->kreq, key);
    long now = (long)time(NULL);
    int n;
    if (!e) {
        e = cJSON_AddObjectToObject(c->kreq, key);
        cJSON_AddNumberToObject(e, "n", 0);
        cJSON_AddNumberToObject(e, "t", 0);
    }
    n = cJSON_GetObjectItemCaseSensitive(e, "n")->valueint;
    if (n > 3 || now < (long)cJSON_GetObjectItemCaseSensitive(e, "t")->valuedouble) return 0;
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "n"), n + 1);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "t"), (double)(now + (n < 3 ? delay[n] : 0)));
    return 1;
}

void amx__fetch_missing_keys(AmxClient *c)
{
    cJSON *copy, *e, *byroom, *r;
    int sent = 0;
    if (!c->pending || !cJSON_GetArraySize(c->pending) || !c->device_id) return;
    copy = cJSON_Duplicate(c->pending, 1);

    /* the key backup first (no other device needed) */
    if (amx_backup_available(c)) {
        byroom = cJSON_CreateObject();
        cJSON_ArrayForEach(e, copy) {
            const char *room = amx__jstr(e, "_room_id");
            cJSON *arr;
            if (!room) continue;
            if (!(arr = cJSON_GetObjectItemCaseSensitive(byroom, room))) arr = cJSON_AddArrayToObject(byroom, room);
            cJSON_AddItemReferenceToArray(arr, e);
        }
        cJSON_ArrayForEach(r, byroom) amx__backup_for_events(c, r->string, r, 0);
        cJSON_Delete(byroom);
    }

    /* then our other devices (Element answers its own verified devices) */
    if (!c->kreq) c->kreq = cJSON_CreateObject();
    cJSON_ArrayForEach(e, copy) {
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(e, "content");
        const char *room = amx__jstr(e, "_room_id"), *sid = amx__jstr(ct, "session_id");
        char *k;
        if (!room || !sid || sent >= 20) continue;
        k = amx__malloc(strlen(room) + strlen(sid) + 2);
        sprintf(k, "%s|%s", room, sid);
        if (kreq_due(c, k) && request_key(c, room, sid, amx__jstr(ct, "sender_key")) == 0) sent++;
        free(k);
    }
    cJSON_Delete(copy);
}
