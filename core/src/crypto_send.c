/* Sending side of E2EE: device lists, outbound Olm sessions, outbound Megolm
 * sessions, room-key sharing, encrypting room events. */
#include "amx_internal.h"

int amx__load_send_state(AmxClient *c)
{
    char *t = amx__read_store(c, "megolm_out.json");
    c->outbound = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!c->outbound) c->outbound = cJSON_CreateObject();
    c->devices = cJSON_CreateObject();
    return 0;
}

void amx__forget_devices(AmxClient *c, const char *user)
{
    cJSON_DeleteItemFromObjectCaseSensitive(c->devices, user);
}

/* Fetches (self-signature verified) device lists for users not cached yet. */
static void query_devices(AmxClient *c, const cJSON *users)
{
    cJSON *req = cJSON_CreateObject(), *dk = cJSON_AddObjectToObject(req, "device_keys"), *res, *ud;
    const cJSON *u;
    int n = 0;
    cJSON_ArrayForEach(u, users) {
        if (cJSON_GetObjectItemCaseSensitive(c->devices, u->valuestring)) continue;
        cJSON_AddItemToObject(dk, u->valuestring, cJSON_CreateArray());
        n++;
    }
    if (!n) { cJSON_Delete(req); return; }
    res = amx__api_ok(c, "POST", "/keys/query", req);
    cJSON_Delete(req);
    if (!res) return;
    cJSON_ArrayForEach(ud, cJSON_GetObjectItemCaseSensitive(res, "device_keys")) {
        cJSON *list = cJSON_CreateArray(), *d;
        cJSON_ArrayForEach(d, ud) {
            const char *uid = amx__jstr(d, "user_id"), *did = amx__jstr(d, "device_id"), *curve, *ed;
            const cJSON *keys = cJSON_GetObjectItemCaseSensitive(d, "keys");
            char kid[200];
            cJSON *e;
            if (!uid || !did || strcmp(uid, ud->string) || strcmp(did, d->string)) continue;
            snprintf(kid, sizeof kid, "curve25519:%s", did);
            curve = amx__jstr(keys, kid);
            snprintf(kid, sizeof kid, "ed25519:%s", did);
            ed = amx__jstr(keys, kid);
            if (!curve || !ed) continue;
            if (!amx__verify_signed(c, d, uid, kid, ed)) {
                LOG(c, "device %s of %s: bad self-signature, ignored", did, uid);
                continue;
            }
            if (!strcmp(uid, c->user_id) && !strcmp(did, c->device_id)) continue;   /* us */
            e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "user", uid);
            cJSON_AddStringToObject(e, "device_id", did);
            cJSON_AddStringToObject(e, "curve", curve);
            cJSON_AddStringToObject(e, "ed", ed);
            cJSON_AddItemToArray(list, e);
        }
        LOG(c, "devices of %s: %d", ud->string, cJSON_GetArraySize(list));
        cJSON_DeleteItemFromObjectCaseSensitive(c->devices, ud->string);
        cJSON_AddItemToObject(c->devices, ud->string, list);
    }
    cJSON_Delete(res);
}

static OlmSession *session_for(AmxClient *c, const char *curve)
{
    int i;
    for (i = c->n_olm - 1; i >= 0; i--)          /* newest first */
        if (!strcmp(c->olm[i].sender_key, curve)) return c->olm[i].s;
    return NULL;
}

/* Opens Olm sessions to all devices in targets that have none yet. */
static void ensure_olm_sessions(AmxClient *c, const cJSON *targets)
{
    cJSON *req = cJSON_CreateObject(), *otk = cJSON_AddObjectToObject(req, "one_time_keys"), *res, *uo;
    const cJSON *t;
    int n = 0, made = 0;
    cJSON_ArrayForEach(t, targets) {
        cJSON *uobj;
        if (session_for(c, amx__jstr(t, "curve"))) continue;
        if (cJSON_GetObjectItemCaseSensitive(c->nokey, amx__jstr(t, "curve"))) continue;
        uobj = cJSON_GetObjectItemCaseSensitive(otk, amx__jstr(t, "user"));
        if (!uobj) uobj = cJSON_AddObjectToObject(otk, amx__jstr(t, "user"));
        cJSON_AddStringToObject(uobj, amx__jstr(t, "device_id"), "signed_curve25519");
        n++;
    }
    if (!n) { cJSON_Delete(req); return; }
    res = amx__api_ok(c, "POST", "/keys/claim", req);
    cJSON_Delete(req);
    if (!res) return;
    uo = cJSON_GetObjectItemCaseSensitive(res, "one_time_keys");
    cJSON_ArrayForEach(t, targets) {
        const cJSON *keys = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(uo, amx__jstr(t, "user")), amx__jstr(t, "device_id")), *k;
        char kid[200];
        const char *ck = amx__jstr(t, "curve");
        if (session_for(c, ck) || cJSON_GetObjectItemCaseSensitive(c->nokey, ck)) continue;
        if (!keys || !keys->child) {
            LOG(c, "no one-time key for %s %s, skipped", amx__jstr(t, "user"), amx__jstr(t, "device_id"));
            cJSON_AddTrueToObject(c->nokey, ck);
            continue;
        }
        k = keys->child;
        snprintf(kid, sizeof kid, "ed25519:%s", amx__jstr(t, "device_id"));
        if (!amx__jstr(k, "key") || !amx__verify_signed(c, k, amx__jstr(t, "user"), kid, amx__jstr(t, "ed"))) {
            LOG(c, "one-time key of %s %s: bad signature, skipped", amx__jstr(t, "user"), amx__jstr(t, "device_id"));
            continue;
        }
        {
            OlmSession *s = amx__new_olm_session();
            size_t rn = olm_create_outbound_session_random_length(s);
            void *r = amx__random(rn);
            const char *okey = amx__jstr(k, "key");
            if (olm_create_outbound_session(s, c->acct, ck, strlen(ck), okey, strlen(okey), r, rn) == olm_error()) {
                LOG(c, "Olm: outbound session failed: %s", olm_session_last_error(s));
                free(r); free(s);
                continue;
            }
            free(r);
            amx__add_olm_session(c, ck, s);
            made++;
            LOG(c, "Olm: new outbound session to %s %s", amx__jstr(t, "user"), amx__jstr(t, "device_id"));
        }
    }
    cJSON_Delete(res);
    if (made) amx__save_olm_sessions(c);
}

static char *olm_encrypt_str(AmxClient *c, OlmSession *s, const char *plain, int *type)
{
    size_t rn = olm_encrypt_random_length(s), cl = olm_encrypt_message_length(s, strlen(plain));
    void *r = amx__random(rn);
    char *ct = amx__malloc(cl + 1);
    *type = (int)olm_encrypt_message_type(s);
    if (olm_encrypt(s, plain, strlen(plain), r, rn, ct, cl) == olm_error()) {
        LOG(c, "Olm: encrypt failed: %s", olm_session_last_error(s));
        free(r); free(ct);
        return NULL;
    }
    free(r);
    ct[cl] = 0;
    return ct;
}

static OlmOutboundGroupSession *unpickle_out(AmxClient *c, const cJSON *entry)
{
    OlmOutboundGroupSession *o = olm_outbound_group_session(amx__malloc(olm_outbound_group_session_size()));
    char *p = amx__strdup(amx__jstr(entry, "pickle"));
    if (olm_unpickle_outbound_group_session(o, PK, p, strlen(p)) == olm_error()) {
        amx__fail(c, "unpickle outbound session: %s", olm_outbound_group_session_last_error(o));
        free(p); free(o);
        return NULL;
    }
    free(p);
    return o;
}

static void repickle_out(cJSON *entry, OlmOutboundGroupSession *o)
{
    size_t n = olm_pickle_outbound_group_session_length(o);
    char *p = amx__malloc(n + 1);
    olm_pickle_outbound_group_session(o, PK, p, n);
    p[n] = 0;
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "pickle");
    cJSON_AddStringToObject(entry, "pickle", p);
    free(p);
}

/* New outbound Megolm session for room; our own inbound copy is added so we
 * can read our own messages when they come back in /sync. */
static cJSON *new_outbound(AmxClient *c, const char *room_id)
{
    OlmOutboundGroupSession *o = olm_outbound_group_session(amx__malloc(olm_outbound_group_session_size()));
    OlmInboundGroupSession *in;
    size_t rn = olm_init_outbound_group_session_random_length(o), n;
    void *r = amx__random(rn);
    uint8_t *key;
    char sid[128];
    cJSON *entry = cJSON_CreateObject();
    olm_init_outbound_group_session(o, r, rn);
    free(r);
    n = olm_outbound_group_session_id(o, (uint8_t *)sid, sizeof sid - 1);
    sid[n] = 0;
    n = olm_outbound_group_session_key_length(o);
    key = amx__malloc(n + 1);
    olm_outbound_group_session_key(o, key, n);
    in = amx__new_group_session();
    olm_init_inbound_group_session(in, key, n);
    free(key);
    amx__add_group_session(c, room_id, sid, c->curve, c->ed, in);
    amx__save_group_sessions(c);
    repickle_out(entry, o);
    cJSON_AddObjectToObject(entry, "shared");
    cJSON_AddNumberToObject(entry, "count", 0);
    cJSON_AddStringToObject(entry, "sid", sid);
    LOG(c, "Megolm: new outbound session %.16s... for %s (own copy %s)", sid, room_id,
        amx__find_group(c, room_id, sid) ? "stored" : "MISSING");
    olm_clear_outbound_group_session(o);
    free(o);
    cJSON_DeleteItemFromObjectCaseSensitive(c->outbound, room_id);
    cJSON_AddItemToObject(c->outbound, room_id, entry);
    LOG(c, "Megolm: new outbound session %.12s... for %s", sid, room_id);
    return entry;
}

/* Shares the outbound session with every device in targets that lacks it. */
static int share_session(AmxClient *c, const char *room_id, cJSON *entry,
                         OlmOutboundGroupSession *o, const cJSON *targets)
{
    cJSON *shared = cJSON_GetObjectItemCaseSensitive(entry, "shared"), *need = cJSON_CreateArray();
    cJSON *msgs, *req, *res;
    const cJSON *d;
    char sid[128], *txn, *path;
    uint8_t *key;
    size_t n;
    int n_share = 0;

    cJSON_ArrayForEach(d, targets)
        if (!cJSON_GetObjectItemCaseSensitive(shared, amx__jstr(d, "curve")))
            cJSON_AddItemToArray(need, cJSON_Duplicate(d, 1));
    if (!cJSON_GetArraySize(need)) { cJSON_Delete(need); return 0; }
    ensure_olm_sessions(c, need);

    n = olm_outbound_group_session_id(o, (uint8_t *)sid, sizeof sid - 1);
    sid[n] = 0;
    n = olm_outbound_group_session_key_length(o);
    key = amx__malloc(n + 1);
    olm_outbound_group_session_key(o, key, n);
    key[n] = 0;
    msgs = cJSON_CreateObject();
    cJSON_ArrayForEach(d, need) {
        OlmSession *s = session_for(c, amx__jstr(d, "curve"));
        cJSON *pl, *ct, *cph, *one, *uo;
        char *pt, *body;
        int type;
        if (!s) continue;
        pl = cJSON_CreateObject();
        cJSON_AddStringToObject(pl, "type", "m.room_key");
        ct = cJSON_AddObjectToObject(pl, "content");
        cJSON_AddStringToObject(ct, "algorithm", MEGOLM_ALG);
        cJSON_AddStringToObject(ct, "room_id", room_id);
        cJSON_AddStringToObject(ct, "session_id", sid);
        cJSON_AddStringToObject(ct, "session_key", (char *)key);
        cJSON_AddStringToObject(pl, "sender", c->user_id);
        cJSON_AddStringToObject(pl, "sender_device", c->device_id);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(pl, "keys"), "ed25519", c->ed);
        cJSON_AddStringToObject(pl, "recipient", amx__jstr(d, "user"));
        cJSON_AddStringToObject(cJSON_AddObjectToObject(pl, "recipient_keys"), "ed25519", amx__jstr(d, "ed"));
        pt = cJSON_PrintUnformatted(pl);
        cJSON_Delete(pl);
        body = olm_encrypt_str(c, s, pt, &type);
        free(pt);
        if (!body) continue;
        ct = cJSON_CreateObject();
        cJSON_AddStringToObject(ct, "algorithm", OLM_ALG);
        cJSON_AddStringToObject(ct, "sender_key", c->curve);
        cph = cJSON_AddObjectToObject(ct, "ciphertext");
        one = cJSON_AddObjectToObject(cph, amx__jstr(d, "curve"));
        cJSON_AddNumberToObject(one, "type", type);
        cJSON_AddStringToObject(one, "body", body);
        free(body);
        uo = cJSON_GetObjectItemCaseSensitive(msgs, amx__jstr(d, "user"));
        if (!uo) uo = cJSON_AddObjectToObject(msgs, amx__jstr(d, "user"));
        cJSON_AddItemToObject(uo, amx__jstr(d, "device_id"), ct);
        n_share++;
    }
    free(key);
    amx__save_olm_sessions(c);
    if (!n_share) { cJSON_Delete(msgs); cJSON_Delete(need); return 0; }

    txn = amx__txn(c);
    path = amx__malloc(strlen(txn) + 48);
    sprintf(path, "/sendToDevice/m.room.encrypted/%s", txn);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "messages", msgs);
    res = amx__api_ok(c, "PUT", path, req);
    cJSON_Delete(req);
    free(path); free(txn);
    if (!res) { cJSON_Delete(need); return -1; }
    cJSON_Delete(res);
    /* only now: mark as shared */
    cJSON_ArrayForEach(d, need)
        if (session_for(c, amx__jstr(d, "curve")) && !cJSON_GetObjectItemCaseSensitive(shared, amx__jstr(d, "curve")))
            cJSON_AddTrueToObject(shared, amx__jstr(d, "curve"));
    cJSON_Delete(need);
    LOG(c, "room key shared with %d device(s)", n_share);
    return 0;
}

int amx__encrypt_room_event(AmxClient *c, const char *room_id, const char *type,
                            const cJSON *content, cJSON **encrypted_out)
{
    char *esc = http_escape(room_id), *path, *plain;
    cJSON *members, *users, *targets, *entry, *x;
    const cJSON *u, *d;
    OlmOutboundGroupSession *o;
    int rotate = 0;
    size_t n;

    *encrypted_out = NULL;
    /* 1. who is in the room, and their devices */
    path = amx__malloc(strlen(esc) + 40);
    sprintf(path, "/rooms/%s/joined_members", esc);
    members = amx__api_ok(c, "GET", path, NULL);
    free(path);
    free(esc);
    if (!members) return -1;
    users = cJSON_CreateArray();
    cJSON_ArrayForEach(u, cJSON_GetObjectItemCaseSensitive(members, "joined"))
        cJSON_AddItemToArray(users, cJSON_CreateString(u->string));
    cJSON_Delete(members);
    query_devices(c, users);
    targets = cJSON_CreateArray();
    cJSON_ArrayForEach(u, users)
        cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(c->devices, u->valuestring))
            cJSON_AddItemToArray(targets, cJSON_Duplicate(d, 1));
    cJSON_Delete(users);

    /* 2. outbound session: rotate after 100 messages or if a device left */
    entry = cJSON_GetObjectItemCaseSensitive(c->outbound, room_id);
    if (entry) {
        const cJSON *sh;
        if (cJSON_GetObjectItemCaseSensitive(entry, "count")->valueint >= 100) rotate = 1;
        cJSON_ArrayForEach(sh, cJSON_GetObjectItemCaseSensitive(entry, "shared")) {
            int found = 0;
            cJSON_ArrayForEach(d, targets) if (!strcmp(amx__jstr(d, "curve"), sh->string)) found = 1;
            if (!found) { rotate = 1; LOG(c, "Megolm: a device left %s, rotating", room_id); break; }
        }
    }
    if (!entry || rotate) entry = new_outbound(c, room_id);
    o = unpickle_out(c, entry);
    if (!o) { cJSON_Delete(targets); return -1; }

    /* 3. share the key with devices that do not have it */
    if (share_session(c, room_id, entry, o, targets)) {
        olm_clear_outbound_group_session(o); free(o);
        cJSON_Delete(targets);
        return -1;
    }
    cJSON_Delete(targets);

    /* 4. encrypt */
    x = cJSON_CreateObject();
    cJSON_AddStringToObject(x, "type", type);
    cJSON_AddItemToObject(x, "content", cJSON_Duplicate(content, 1));
    cJSON_AddStringToObject(x, "room_id", room_id);
    plain = cJSON_PrintUnformatted(x);
    cJSON_Delete(x);
    {
        size_t cl = olm_group_encrypt_message_length(o, strlen(plain));
        uint8_t *ct = amx__malloc(cl + 1);
        char sid[128];
        olm_group_encrypt(o, (uint8_t *)plain, strlen(plain), ct, cl);
        ct[cl] = 0;
        n = olm_outbound_group_session_id(o, (uint8_t *)sid, sizeof sid - 1);
        sid[n] = 0;
        x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "algorithm", MEGOLM_ALG);
        cJSON_AddStringToObject(x, "sender_key", c->curve);
        cJSON_AddStringToObject(x, "ciphertext", (char *)ct);
        cJSON_AddStringToObject(x, "session_id", sid);
        cJSON_AddStringToObject(x, "device_id", c->device_id);
        LOG(c, "Megolm: encrypted for %s with session %.16s... index %lu (own copy %s)", room_id, sid,
            (unsigned long)olm_outbound_group_session_message_index(o) - 1,
            amx__find_group(c, room_id, sid) ? "present" : "MISSING");
        free(ct);
    }
    free(plain);
    /* the ratchet has advanced: store it before the request goes out */
    repickle_out(entry, o);
    cJSON_ReplaceItemInObjectCaseSensitive(entry, "count",
        cJSON_CreateNumber(cJSON_GetObjectItemCaseSensitive(entry, "count")->valueint + 1));
    amx__save_json(c, "megolm_out.json", c->outbound);
    olm_clear_outbound_group_session(o);
    free(o);
    *encrypted_out = x;
    return 0;
}
