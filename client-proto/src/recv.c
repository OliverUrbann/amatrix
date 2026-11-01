/* amatrix-recv: receive path of a Matrix E2EE client, in plain C.
 *
 *   login (m.login.password) -> upload signed device keys, one-time keys and
 *   a fallback key -> /sync loop: accept invites, decrypt Olm to-device
 *   messages (m.room_key), decrypt Megolm room messages and print them.
 *
 * Everything that has to survive a restart (login, Olm account, Olm and
 * Megolm sessions, sync token) lives as small text files in a store
 * directory, so the device stays the same across runs.
 *
 * Dependencies: libolm, cJSON, and an http.h implementation (libcurl on the
 * host; bsdsocket + AmiSSL on the Amiga, later).
 *
 * usage: amatrix-recv [options] HOMESERVER_URL USER PASSWORD
 *   -s DIR     store directory (default: ./amatrix-store)
 *   -n COUNT   exit after COUNT decrypted messages (default: run forever)
 *   -t SECS    exit after SECS seconds (default: run forever)
 *   -v         verbose
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "olm/olm.h"
#include "cJSON.h"
#include "canonjson.h"
#include "http.h"
#include "platform.h"

/* Prototype: fixed key for pickles at rest. A real client derives this from
 * a passphrase or keeps it in a protected location. */
static const char PICKLE_KEY[] = "amatrix-prototype-pickle-key";
#define PK PICKLE_KEY, (sizeof PICKLE_KEY - 1)

static const char *OLM_ALG = "m.olm.v1.curve25519-aes-sha2";
static const char *MEGOLM_ALG = "m.megolm.v1.aes-sha2";

static int verbose;
#define LOG(...) do { if (verbose) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ------------------------------------------------------------------ state */

typedef struct {
    char *sender_key;
    OlmSession *s;
} OlmSess;

typedef struct {
    char *room_id, *session_id, *sender_key, *ed25519;
    OlmInboundGroupSession *s;
} GroupSess;

static struct {
    char *hs, *store;
    char *user_id, *device_id, *token;
    OlmAccount *acct;
    char *curve, *ed;            /* our identity keys */
    char *since;
    OlmSess *olm; int n_olm;
    GroupSess *grp; int n_grp;
    cJSON *pending;              /* undecryptable room events, retried later */
    int decrypted;
} G;

/* ---------------------------------------------------------------- helpers */

static void die(const char *msg) { fprintf(stderr, "error: %s\n", msg); exit(1); }

static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }

static char *xstrdup(const char *s) { char *r = xmalloc(strlen(s) + 1); strcpy(r, s); return r; }

static char *xstrndup(const char *s, size_t n) { char *r = xmalloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }

static char *path_in_store(const char *name)
{
    char *p = xmalloc(strlen(G.store) + strlen(name) + 2);
    sprintf(p, "%s/%s", G.store, name);
    return p;
}

static char *read_store(const char *name)
{
    char *p = path_in_store(name), *r = plat_read_file(p);
    free(p);
    return r;
}

static void write_store(const char *name, const char *data)
{
    char *p = path_in_store(name);
    if (plat_write_file(p, data) != 0) { fprintf(stderr, "cannot write %s\n", p); exit(1); }
    free(p);
}

static void *random_bytes(size_t n)
{
    void *b = xmalloc(n);
    if (n && plat_random(b, n) != 0) die("no random source");
    return b;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* --------------------------------------------------------------- HTTP/API */

/* Calls the client-server API. path is relative to /_matrix/client/v3.
 * body may be NULL. Returns parsed JSON (caller deletes) or NULL; *status
 * receives the HTTP status. */
static cJSON *api(const char *method, const char *path, const cJSON *body,
                  long timeout_s, long *status)
{
    HttpResponse r;
    char *url = xmalloc(strlen(G.hs) + strlen(path) + 32);
    char *b = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON *res = NULL;

    sprintf(url, "%s/_matrix/client/v3%s", G.hs, path);
    LOG("> %s %s", method, path);
    if (http_request(method, url, G.token, b, timeout_s, &r) != 0) {
        fprintf(stderr, "network error on %s %s\n", method, path);
        *status = 0;
    } else {
        *status = r.status;
        if (r.body) res = cJSON_Parse(r.body);
        if (r.status != 200)
            fprintf(stderr, "%s %s -> HTTP %ld: %s\n", method, path, r.status,
                    r.body ? r.body : "");
        http_response_free(&r);
    }
    free(url);
    free(b);
    return res;
}

static cJSON *api_ok(const char *method, const char *path, const cJSON *body)
{
    long st;
    cJSON *r = api(method, path, body, 60, &st);
    if (st != 200) { cJSON_Delete(r); return NULL; }
    return r;
}

/* ------------------------------------------------------------- persistence */

static char *pickle_account(void)
{
    size_t n = olm_pickle_account_length(G.acct);
    char *p = xmalloc(n + 1);
    if (olm_pickle_account(G.acct, PK, p, n) == olm_error()) die(olm_account_last_error(G.acct));
    p[n] = 0;
    return p;
}

static void save_account(void)
{
    char *p = pickle_account();
    write_store("account", p);
    free(p);
}

static void save_olm_sessions(void)
{
    size_t cap = 1, len = 0;
    char *out;
    int i;
    for (i = 0; i < G.n_olm; i++)
        cap += strlen(G.olm[i].sender_key) + olm_pickle_session_length(G.olm[i].s) + 2;
    out = xmalloc(cap);
    out[0] = 0;
    for (i = 0; i < G.n_olm; i++) {
        size_t n = olm_pickle_session_length(G.olm[i].s);
        len += sprintf(out + len, "%s ", G.olm[i].sender_key);
        if (olm_pickle_session(G.olm[i].s, PK, out + len, n) == olm_error())
            die(olm_session_last_error(G.olm[i].s));
        len += n;
        out[len++] = '\n';
        out[len] = 0;
    }
    write_store("olm_sessions", out);
    free(out);
}

static void save_group_sessions(void)
{
    size_t cap = 1, len = 0;
    char *out;
    int i;
    for (i = 0; i < G.n_grp; i++) {
        GroupSess *g = &G.grp[i];
        cap += strlen(g->room_id) + strlen(g->session_id) + strlen(g->sender_key)
             + strlen(g->ed25519) + olm_pickle_inbound_group_session_length(g->s) + 6;
    }
    out = xmalloc(cap);
    out[0] = 0;
    for (i = 0; i < G.n_grp; i++) {
        GroupSess *g = &G.grp[i];
        size_t n = olm_pickle_inbound_group_session_length(g->s);
        len += sprintf(out + len, "%s %s %s %s ", g->room_id, g->session_id, g->sender_key, g->ed25519);
        if (olm_pickle_inbound_group_session(g->s, PK, out + len, n) == olm_error())
            die(olm_inbound_group_session_last_error(g->s));
        len += n;
        out[len++] = '\n';
        out[len] = 0;
    }
    write_store("megolm_sessions", out);
    free(out);
}

static OlmSession *new_olm_session(void) { return olm_session(xmalloc(olm_session_size())); }
static OlmInboundGroupSession *new_group_session(void)
{
    return olm_inbound_group_session(xmalloc(olm_inbound_group_session_size()));
}

static void add_olm_session(const char *sender_key, OlmSession *s)
{
    G.olm = realloc(G.olm, sizeof *G.olm * (G.n_olm + 1));
    if (!G.olm) die("out of memory");
    G.olm[G.n_olm].sender_key = xstrdup(sender_key);
    G.olm[G.n_olm].s = s;
    G.n_olm++;
}

static GroupSess *find_group(const char *room_id, const char *session_id)
{
    int i;
    for (i = 0; i < G.n_grp; i++)
        if (!strcmp(G.grp[i].room_id, room_id) && !strcmp(G.grp[i].session_id, session_id))
            return &G.grp[i];
    return NULL;
}

static void add_group_session(const char *room_id, const char *session_id,
                              const char *sender_key, const char *ed25519,
                              OlmInboundGroupSession *s)
{
    G.grp = realloc(G.grp, sizeof *G.grp * (G.n_grp + 1));
    if (!G.grp) die("out of memory");
    G.grp[G.n_grp].room_id = xstrdup(room_id);
    G.grp[G.n_grp].session_id = xstrdup(session_id);
    G.grp[G.n_grp].sender_key = xstrdup(sender_key);
    G.grp[G.n_grp].ed25519 = xstrdup(ed25519);
    G.grp[G.n_grp].s = s;
    G.n_grp++;
}

/* Split text into lines and space-separated fields (modifies text). */
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

static void load_sessions(void)
{
    char *data, *line, *next;
    if ((data = read_store("olm_sessions"))) {
        for (line = data; line && *line; line = next) {
            char *f[2];
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (split_fields(line, f, 2) == 2) {
                OlmSession *s = new_olm_session();
                if (olm_unpickle_session(s, PK, f[1], strlen(f[1])) == olm_error())
                    die("cannot unpickle Olm session");
                add_olm_session(f[0], s);
            }
        }
        free(data);
    }
    if ((data = read_store("megolm_sessions"))) {
        for (line = data; line && *line; line = next) {
            char *f[5];
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (split_fields(line, f, 5) == 5) {
                OlmInboundGroupSession *s = new_group_session();
                if (olm_unpickle_inbound_group_session(s, PK, f[4], strlen(f[4])) == olm_error())
                    die("cannot unpickle Megolm session");
                add_group_session(f[0], f[1], f[2], f[3], s);
            }
        }
        free(data);
    }
    G.since = read_store("since");
    LOG("loaded %d Olm and %d Megolm sessions", G.n_olm, G.n_grp);
}

/* ------------------------------------------------------- login & account */

static void login(const char *user, const char *password)
{
    char *saved = read_store("login.json");
    cJSON *j, *req, *id;
    if (saved) {
        j = cJSON_Parse(saved);
        free(saved);
        if (j && jstr(j, "user_id") && jstr(j, "device_id") && jstr(j, "access_token")) {
            G.user_id = xstrdup(jstr(j, "user_id"));
            G.device_id = xstrdup(jstr(j, "device_id"));
            G.token = xstrdup(jstr(j, "access_token"));
            cJSON_Delete(j);
            printf("using saved login %s, device %s\n", G.user_id, G.device_id);
            return;
        }
        cJSON_Delete(j);
    }
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "m.login.password");
    id = cJSON_AddObjectToObject(req, "identifier");
    cJSON_AddStringToObject(id, "type", "m.id.user");
    cJSON_AddStringToObject(id, "user", user);
    cJSON_AddStringToObject(req, "password", password);
    cJSON_AddStringToObject(req, "initial_device_display_name", "Amatrix (Amiga)");
    j = api_ok("POST", "/login", req);
    cJSON_Delete(req);
    if (!j || !jstr(j, "user_id") || !jstr(j, "device_id") || !jstr(j, "access_token"))
        die("login failed");
    G.user_id = xstrdup(jstr(j, "user_id"));
    G.device_id = xstrdup(jstr(j, "device_id"));
    G.token = xstrdup(jstr(j, "access_token"));
    {
        cJSON *s = cJSON_CreateObject();
        char *txt;
        cJSON_AddStringToObject(s, "user_id", G.user_id);
        cJSON_AddStringToObject(s, "device_id", G.device_id);
        cJSON_AddStringToObject(s, "access_token", G.token);
        txt = cJSON_Print(s);
        write_store("login.json", txt);
        free(txt);
        cJSON_Delete(s);
    }
    cJSON_Delete(j);
    /* a fresh device needs fresh keys */
    write_store("keys_uploaded", "0");
    printf("logged in as %s, new device %s\n", G.user_id, G.device_id);
}

static void load_or_create_account(void)
{
    char *p = read_store("account");
    G.acct = olm_account(xmalloc(olm_account_size()));
    if (p) {
        if (olm_unpickle_account(G.acct, PK, p, strlen(p)) == olm_error())
            die(olm_account_last_error(G.acct));
        free(p);
    } else {
        size_t n = olm_create_account_random_length(G.acct);
        void *r = random_bytes(n);
        if (olm_create_account(G.acct, r, n) == olm_error()) die(olm_account_last_error(G.acct));
        free(r);
        save_account();
        write_store("keys_uploaded", "0");
    }
    {
        size_t n = olm_account_identity_keys_length(G.acct);
        char *buf = xmalloc(n + 1);
        cJSON *k;
        olm_account_identity_keys(G.acct, buf, n);
        buf[n] = 0;
        k = cJSON_Parse(buf);
        G.curve = xstrdup(jstr(k, "curve25519"));
        G.ed = xstrdup(jstr(k, "ed25519"));
        cJSON_Delete(k);
        free(buf);
    }
    printf("device keys: curve25519 %s\n             ed25519    %s\n", G.curve, G.ed);
}

/* Adds signatures.<user>.ed25519:<device> to obj (signing its canonical JSON). */
static void sign_json(cJSON *obj)
{
    char *canon = canonical_json_for_signing(obj);
    size_t n = olm_account_signature_length(G.acct);
    char *sig = xmalloc(n + 1), keyid[128];
    cJSON *sigs, *mine;
    if (!canon) die("canonical JSON failed");
    if (olm_account_sign(G.acct, canon, strlen(canon), sig, n) == olm_error())
        die(olm_account_last_error(G.acct));
    sig[n] = 0;
    sigs = cJSON_GetObjectItemCaseSensitive(obj, "signatures");
    if (!sigs) sigs = cJSON_AddObjectToObject(obj, "signatures");
    mine = cJSON_GetObjectItemCaseSensitive(sigs, G.user_id);
    if (!mine) mine = cJSON_AddObjectToObject(sigs, G.user_id);
    snprintf(keyid, sizeof keyid, "ed25519:%s", G.device_id);
    cJSON_AddStringToObject(mine, keyid, sig);
    free(sig);
    free(canon);
}

static cJSON *device_keys_object(void)
{
    cJSON *dk = cJSON_CreateObject(), *alg, *keys;
    char kid[128];
    cJSON_AddStringToObject(dk, "user_id", G.user_id);
    cJSON_AddStringToObject(dk, "device_id", G.device_id);
    alg = cJSON_AddArrayToObject(dk, "algorithms");
    cJSON_AddItemToArray(alg, cJSON_CreateString(OLM_ALG));
    cJSON_AddItemToArray(alg, cJSON_CreateString(MEGOLM_ALG));
    keys = cJSON_AddObjectToObject(dk, "keys");
    snprintf(kid, sizeof kid, "curve25519:%s", G.device_id);
    cJSON_AddStringToObject(keys, kid, G.curve);
    snprintf(kid, sizeof kid, "ed25519:%s", G.device_id);
    cJSON_AddStringToObject(keys, kid, G.ed);
    sign_json(dk);
    return dk;
}

/* Turns libolm's {"curve25519":{"id":"key",...}} into signed
 * {"signed_curve25519:id":{"key":...,"signatures":...}} entries in dst. */
static int add_signed_keys(cJSON *dst, const char *olm_json, int fallback)
{
    cJSON *src = cJSON_Parse(olm_json), *c, *k;
    int n = 0;
    cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(src, "curve25519")) {
        char kid[128];
        c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "key", k->valuestring);
        if (fallback) cJSON_AddTrueToObject(c, "fallback");
        sign_json(c);
        snprintf(kid, sizeof kid, "signed_curve25519:%s", k->string);
        cJSON_AddItemToObject(dst, kid, c);
        n++;
    }
    cJSON_Delete(src);
    return n;
}

/* Upload device keys (once per device), one-time keys up to half of what
 * libolm can hold, and a fallback key if the server has none. */
static void upload_keys(int otk_on_server, int need_fallback)
{
    cJSON *req = cJSON_CreateObject(), *res;
    char *flag = read_store("keys_uploaded");
    int want = (int)olm_account_max_number_of_one_time_keys(G.acct) / 2;
    int changed = 0, n_otk = 0, n_fb = 0;

    if (!flag || flag[0] != '1') {
        cJSON_AddItemToObject(req, "device_keys", device_keys_object());
        changed = 1;
    }
    free(flag);

    if (otk_on_server < want) {
        size_t gen = want - otk_on_server;
        size_t rn = olm_account_generate_one_time_keys_random_length(G.acct, gen);
        void *r = random_bytes(rn);
        size_t len;
        char *buf;
        olm_account_generate_one_time_keys(G.acct, gen, r, rn);
        free(r);
        len = olm_account_one_time_keys_length(G.acct);
        buf = xmalloc(len + 1);
        olm_account_one_time_keys(G.acct, buf, len);
        buf[len] = 0;
        n_otk = add_signed_keys(cJSON_AddObjectToObject(req, "one_time_keys"), buf, 0);
        free(buf);
        changed = 1;
    }
    if (need_fallback) {
        size_t rn = olm_account_generate_fallback_key_random_length(G.acct), len;
        void *r = random_bytes(rn);
        char *buf;
        olm_account_generate_fallback_key(G.acct, r, rn);
        free(r);
        len = olm_account_unpublished_fallback_key_length(G.acct);
        buf = xmalloc(len + 1);
        olm_account_unpublished_fallback_key(G.acct, buf, len);
        buf[len] = 0;
        n_fb = add_signed_keys(cJSON_AddObjectToObject(req, "fallback_keys"), buf, 1);
        free(buf);
        changed = 1;
    }
    if (!changed) { cJSON_Delete(req); return; }

    res = api_ok("POST", "/keys/upload", req);
    cJSON_Delete(req);
    if (!res) die("key upload failed");
    olm_account_mark_keys_as_published(G.acct);
    save_account();
    write_store("keys_uploaded", "1");
    printf("uploaded keys: %d one-time, %d fallback\n", n_otk, n_fb);
    cJSON_Delete(res);
}

/* ----------------------------------------------------------- decryption */

/* Decrypts one Olm message from sender_key. Returns malloc'd plaintext or NULL. */
static char *olm_decrypt_from(const char *sender_key, int type, const char *body)
{
    size_t blen = strlen(body), max, n;
    char *tmp, *plain;
    int i;
    OlmSession *s;

    for (i = 0; i < G.n_olm; i++) {
        if (strcmp(G.olm[i].sender_key, sender_key)) continue;
        s = G.olm[i].s;
        if (type == 0) {
            tmp = xstrndup(body, blen);
            n = olm_matches_inbound_session_from(s, sender_key, strlen(sender_key), tmp, blen);
            free(tmp);
            if (n != 1) continue;
        }
        tmp = xstrndup(body, blen);
        max = olm_decrypt_max_plaintext_length(s, type, tmp, blen);
        free(tmp);
        if (max == olm_error()) continue;
        plain = xmalloc(max + 1);
        tmp = xstrndup(body, blen);
        n = olm_decrypt(s, type, tmp, blen, plain, max);
        free(tmp);
        if (n == olm_error()) { free(plain); continue; }
        plain[n] = 0;
        save_olm_sessions();
        LOG("Olm: decrypted with existing session %d", i);
        return plain;
    }
    if (type != 0) {
        fprintf(stderr, "Olm: no session for message from %s\n", sender_key);
        return NULL;
    }
    /* pre-key message: new inbound session using one of our one-time keys */
    s = new_olm_session();
    tmp = xstrndup(body, blen);
    n = olm_create_inbound_session_from(s, G.acct, sender_key, strlen(sender_key), tmp, blen);
    free(tmp);
    if (n == olm_error()) {
        fprintf(stderr, "Olm: cannot create inbound session: %s\n", olm_session_last_error(s));
        free(s);
        return NULL;
    }
    olm_remove_one_time_keys(G.acct, s);
    tmp = xstrndup(body, blen);
    max = olm_decrypt_max_plaintext_length(s, 0, tmp, blen);
    free(tmp);
    plain = xmalloc(max + 1);
    tmp = xstrndup(body, blen);
    n = olm_decrypt(s, 0, tmp, blen, plain, max);
    free(tmp);
    if (n == olm_error()) {
        fprintf(stderr, "Olm: decrypt failed: %s\n", olm_session_last_error(s));
        free(plain);
        free(s);
        return NULL;
    }
    plain[n] = 0;
    add_olm_session(sender_key, s);
    save_account();
    save_olm_sessions();
    LOG("Olm: new inbound session from %s", sender_key);
    return plain;
}

static void handle_room_key(const cJSON *payload, const char *sender_key, const char *sender_ed)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(payload, "content");
    const char *alg = jstr(c, "algorithm"), *room = jstr(c, "room_id"),
               *sid = jstr(c, "session_id"), *key = jstr(c, "session_key");
    OlmInboundGroupSession *s;
    char *k, idbuf[128];
    size_t n;

    if (!alg || strcmp(alg, MEGOLM_ALG) || !room || !sid || !key) {
        fprintf(stderr, "m.room_key: unsupported or incomplete\n");
        return;
    }
    if (find_group(room, sid)) { LOG("m.room_key: already known"); return; }
    s = new_group_session();
    k = xstrdup(key);
    if (olm_init_inbound_group_session(s, (uint8_t *)k, strlen(k)) == olm_error()) {
        fprintf(stderr, "m.room_key: %s\n", olm_inbound_group_session_last_error(s));
        free(k); free(s);
        return;
    }
    free(k);
    n = olm_inbound_group_session_id(s, (uint8_t *)idbuf, sizeof idbuf - 1);
    idbuf[n == olm_error() ? 0 : n] = 0;
    if (strcmp(idbuf, sid)) {
        fprintf(stderr, "m.room_key: session id mismatch\n");
        free(s);
        return;
    }
    add_group_session(room, sid, sender_key, sender_ed ? sender_ed : "-", s);
    save_group_sessions();
    printf("got room key for %s (session %.12s...)\n", room, sid);
}

static void handle_to_device(const cJSON *ev)
{
    const char *type = jstr(ev, "type"), *sender = jstr(ev, "sender");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(ev, "content"), *mine;
    const char *sk, *body, *ptype;
    cJSON *payload;
    char *plain;
    const cJSON *t;

    if (!type || strcmp(type, "m.room.encrypted")) { LOG("to-device %s ignored", type ? type : "?"); return; }
    if (!jstr(c, "algorithm") || strcmp(jstr(c, "algorithm"), OLM_ALG)) return;
    sk = jstr(c, "sender_key");
    mine = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c, "ciphertext"), G.curve);
    body = jstr(mine, "body");
    t = cJSON_GetObjectItemCaseSensitive(mine, "type");
    if (!sk || !body || !cJSON_IsNumber(t)) { LOG("to-device: not for this device"); return; }

    plain = olm_decrypt_from(sk, t->valueint, body);
    if (!plain) return;
    payload = cJSON_Parse(plain);
    free(plain);
    if (!payload) { fprintf(stderr, "Olm payload is not JSON\n"); return; }

    /* the checks the spec requires before trusting an Olm payload */
    if (!jstr(payload, "sender") || strcmp(jstr(payload, "sender"), sender) ||
        !jstr(payload, "recipient") || strcmp(jstr(payload, "recipient"), G.user_id) ||
        !jstr(cJSON_GetObjectItemCaseSensitive(payload, "recipient_keys"), "ed25519") ||
        strcmp(jstr(cJSON_GetObjectItemCaseSensitive(payload, "recipient_keys"), "ed25519"), G.ed)) {
        fprintf(stderr, "Olm payload failed sender/recipient checks, dropped\n");
        cJSON_Delete(payload);
        return;
    }
    ptype = jstr(payload, "type");
    LOG("Olm payload from %s: %s", sender, ptype ? ptype : "?");
    if (ptype && !strcmp(ptype, "m.room_key"))
        handle_room_key(payload, sk, jstr(cJSON_GetObjectItemCaseSensitive(payload, "keys"), "ed25519"));
    cJSON_Delete(payload);
}

/* Returns 1 if decrypted (or not decryptable ever), 0 if the key is missing. */
static int try_room_event(const char *room_id, const cJSON *ev)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(ev, "content");
    const char *type = jstr(ev, "type"), *sender = jstr(ev, "sender");
    const char *sid, *ct;
    GroupSess *g;
    uint8_t *tmp, *plain;
    size_t max, n, len;
    uint32_t idx;
    cJSON *p;

    if (!type) return 1;
    if (strcmp(type, "m.room.encrypted")) {
        if (!strcmp(type, "m.room.message"))
            printf("[%s] %s (unencrypted): %s\n", room_id, sender,
                   jstr(c, "body") ? jstr(c, "body") : "");
        return 1;
    }
    if (!jstr(c, "algorithm") || strcmp(jstr(c, "algorithm"), MEGOLM_ALG)) return 1;
    sid = jstr(c, "session_id");
    ct = jstr(c, "ciphertext");
    if (!sid || !ct) return 1;
    g = find_group(room_id, sid);
    if (!g) return 0;

    len = strlen(ct);
    tmp = (uint8_t *)xstrndup(ct, len);
    max = olm_group_decrypt_max_plaintext_length(g->s, tmp, len);
    free(tmp);
    if (max == olm_error()) {
        fprintf(stderr, "Megolm: %s\n", olm_inbound_group_session_last_error(g->s));
        return 1;
    }
    plain = xmalloc(max + 1);
    tmp = (uint8_t *)xstrndup(ct, len);
    n = olm_group_decrypt(g->s, tmp, len, plain, max, &idx);
    free(tmp);
    if (n == olm_error()) {
        fprintf(stderr, "Megolm: %s\n", olm_inbound_group_session_last_error(g->s));
        free(plain);
        return 1;
    }
    plain[n] = 0;
    /* TODO: replay protection (remember event_id per (session, index)) */
    p = cJSON_Parse((char *)plain);
    free(plain);
    if (!p || !jstr(p, "room_id") || strcmp(jstr(p, "room_id"), room_id)) {
        fprintf(stderr, "Megolm: payload room_id mismatch, dropped\n");
        cJSON_Delete(p);
        return 1;
    }
    {
        const cJSON *pc = cJSON_GetObjectItemCaseSensitive(p, "content");
        const char *ptype = jstr(p, "type");
        if (ptype && !strcmp(ptype, "m.room.message"))
            printf("[%s] %s: %s\n", room_id, sender, jstr(pc, "body") ? jstr(pc, "body") : "");
        else
            printf("[%s] %s: (%s)\n", room_id, sender, ptype ? ptype : "?");
        fflush(stdout);
    }
    cJSON_Delete(p);
    G.decrypted++;
    return 1;
}

static void handle_room_event(const char *room_id, const cJSON *ev)
{
    if (!try_room_event(room_id, ev)) {
        cJSON *e = cJSON_Duplicate(ev, 1);
        cJSON_AddStringToObject(e, "_room_id", room_id);
        cJSON_AddItemToArray(G.pending, e);
        LOG("Megolm: key not (yet) known, event queued");
    }
}

static void retry_pending(void)
{
    cJSON *e = G.pending->child, *next;
    while (e) {
        next = e->next;
        if (try_room_event(jstr(e, "_room_id"), e))
            cJSON_Delete(cJSON_DetachItemViaPointer(G.pending, e));
        e = next;
    }
}

/* ------------------------------------------------------------------ sync */

static void join_room(const char *room_id)
{
    char *esc = http_escape(room_id), *path = xmalloc(strlen(esc) + 16);
    cJSON *body = cJSON_CreateObject(), *r;
    sprintf(path, "/join/%s", esc);
    r = api_ok("POST", path, body);
    if (r) printf("joined %s\n", room_id);
    cJSON_Delete(r);
    cJSON_Delete(body);
    free(path);
    free(esc);
}

static int sync_once(long timeout_ms)
{
    static const char filter[] =
        "%7B%22room%22%3A%7B%22timeline%22%3A%7B%22limit%22%3A20%7D%7D%7D"; /* {"room":{"timeline":{"limit":20}}} */
    char path[1024];
    long st;
    cJSON *r, *x, *room;
    int otk = 0, need_fb = 1;

    if (G.since)
        snprintf(path, sizeof path, "/sync?timeout=%ld&filter=%s&since=%s", timeout_ms, filter, G.since);
    else
        snprintf(path, sizeof path, "/sync?timeout=0&filter=%s", filter);
    r = api("GET", path, NULL, timeout_ms / 1000 + 30, &st);
    if (st != 200 || !r) { cJSON_Delete(r); return -1; }

    /* to-device first: the room keys for this batch's messages are here */
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetObjectItemCaseSensitive(r, "to_device"), "events"))
        handle_to_device(x);
    retry_pending();

    cJSON_ArrayForEach(room, cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetObjectItemCaseSensitive(r, "rooms"), "invite"))
        join_room(room->string);

    cJSON_ArrayForEach(room, cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetObjectItemCaseSensitive(r, "rooms"), "join"))
        cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(
                                  cJSON_GetObjectItemCaseSensitive(room, "timeline"), "events"))
            handle_room_event(room->string, x);

    x = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(r, "device_one_time_keys_count"), "signed_curve25519");
    if (cJSON_IsNumber(x)) otk = x->valueint;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "device_unused_fallback_key_types"))
        if (cJSON_IsString(x) && !strcmp(x->valuestring, "signed_curve25519")) need_fb = 0;
    upload_keys(otk, need_fb);

    if (jstr(r, "next_batch")) {
        free(G.since);
        G.since = xstrdup(jstr(r, "next_batch"));
        write_store("since", G.since);
    }
    cJSON_Delete(r);
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    int i, max_msgs = -1, secs = -1;
    time_t start = time(NULL);
    const char *user, *pass;

    G.store = "amatrix-store";
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) G.store = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_msgs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else break;
    }
    if (argc - i != 3) {
        fprintf(stderr, "usage: %s [-s storedir] [-n count] [-t seconds] [-v] HOMESERVER_URL USER PASSWORD\n", argv[0]);
        return 2;
    }
    G.hs = xstrdup(argv[i]);
    if (G.hs[strlen(G.hs) - 1] == '/') G.hs[strlen(G.hs) - 1] = 0;
    user = argv[i + 1];
    pass = argv[i + 2];

    if (plat_mkdir(G.store) != 0) die("cannot create store directory");
    if (http_init() != 0) die("HTTP init failed");
    G.pending = cJSON_CreateArray();

    login(user, pass);
    load_or_create_account();
    load_sessions();

    for (;;) {
        long left = 30000;
        if (secs >= 0) {
            long rem = (long)(secs - (time(NULL) - start));
            if (rem <= 0) break;
            if (rem * 1000 < left) left = rem * 1000;
        }
        if (sync_once(left) != 0) plat_sleep(5);
        if (max_msgs >= 0 && G.decrypted >= max_msgs) break;
    }
    if (cJSON_GetArraySize(G.pending))
        printf("%d message(s) could not be decrypted (no key)\n", cJSON_GetArraySize(G.pending));
    printf("done, %d message(s) decrypted\n", G.decrypted);
    http_cleanup();
    return 0;
}
