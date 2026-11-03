/* Client lifecycle, store, HTTP/API helpers, login. */
#include "amx_internal.h"
#include <stdarg.h>
#include <time.h>

/* ------------------------------------------------------------ helpers */

void amx__log(AmxClient *c, int level, const char *fmt, ...)
{
    va_list ap;
    if (!c || c->verbose < level) return;
    va_start(ap, fmt);
    vfprintf(AMX_ERR, fmt, ap);
    va_end(ap);
    fputc('\n', AMX_ERR);
    fflush(AMX_ERR);
}

int amx__fail(AmxClient *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    LOG(c, "error: %s", c->err);
    return -1;
}

void *amx__malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(AMX_ERR, "amatrix: out of memory\n"); exit(20); }
    return p;
}

char *amx__strdup(const char *s) { return amx__strndup(s, strlen(s)); }

char *amx__strndup(const char *s, size_t n)
{
    char *r = amx__malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

const char *amx__jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static char *store_path(AmxClient *c, const char *name)
{
    char *p = amx__malloc(strlen(c->store) + strlen(name) + 2);
    sprintf(p, "%s/%s", c->store, name);
    return p;
}

char *amx__read_store(AmxClient *c, const char *name)
{
    char *p = store_path(c, name), *r = plat_read_file(p);
    free(p);
    return r;
}

int amx__write_store(AmxClient *c, const char *name, const char *data)
{
    char *p = store_path(c, name);
    int r = plat_write_file(p, data);
    if (r) amx__fail(c, "cannot write %s", p);
    free(p);
    return r;
}

int amx__save_json(AmxClient *c, const char *name, const cJSON *j)
{
    char *t = cJSON_PrintUnformatted(j);
    int r = amx__write_store(c, name, t);
    free(t);
    return r;
}

void *amx__random(size_t n)
{
    void *b = amx__malloc(n);
    if (n && plat_random(b, n) != 0) { fprintf(AMX_ERR, "amatrix: no random source\n"); exit(20); }
    return b;
}

char *amx__txn(AmxClient *c)
{
    char buf[48];
    sprintf(buf, "amx%lu.%lu", (unsigned long)time(NULL), ++c->txn);
    return amx__strdup(buf);
}

cJSON *amx__api(AmxClient *c, const char *method, const char *path, const cJSON *body,
                long timeout_s, long *status)
{
    HttpResponse r;
    char *url = amx__malloc(strlen(c->hs) + strlen(path) + 32);
    char *b = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON *res = NULL;

    if (!strncmp(path, "/_matrix", 8)) sprintf(url, "%s%s", c->hs, path);
    else sprintf(url, "%s/_matrix/client/v3%s", c->hs, path);
    LOG2(c, "> %s %s", method, path);
    if (http_request(method, url, c->token, b, timeout_s, &r) != 0) {
        amx__fail(c, "network error on %s %s: %s", method, path, http_last_error());
        *status = 0;
    } else {
        *status = r.status;
        if (r.body) res = cJSON_Parse(r.body);
        if (r.status != 200) {
            const char *code = amx__jstr(res, "errcode"), *msg = amx__jstr(res, "error");
            amx__fail(c, "%s %s: HTTP %ld %s %s", method, path, r.status,
                      code ? code : "", msg ? msg : "");
        }
        http_response_free(&r);
    }
    free(url);
    free(b);
    return res;
}

cJSON *amx__api_ok(AmxClient *c, const char *method, const char *path, const cJSON *body)
{
    long st;
    cJSON *r = amx__api(c, method, path, body, 60, &st);
    if (st != 200) { cJSON_Delete(r); return NULL; }
    return r;
}

void amx__emit(AmxClient *c, AmxEvent *ev)
{
    if (c->fn) c->fn(c, ev, c->ud);
}

/* ---------------------------------------------------------- lifecycle */

int amx_init(void) { return http_init(); }
void amx_cleanup(void) { http_cleanup(); }

AmxClient *amx_client_new(const char *homeserver, const char *store_dir)
{
    AmxClient *c = calloc(1, sizeof *c);
    size_t n;
    if (!c) return NULL;
    c->hs = amx__strdup(homeserver);
    n = strlen(c->hs);
    while (n && c->hs[n - 1] == '/') c->hs[--n] = 0;
    c->store = amx__strdup(store_dir);
    c->pending = cJSON_CreateArray();
    c->nokey = cJSON_CreateObject();
    c->room_enc = cJSON_CreateObject();
    c->util = olm_utility(amx__malloc(olm_utility_size()));
    if (plat_mkdir(c->store) != 0) {
        fprintf(AMX_ERR, "amatrix: cannot create store directory %s\n", c->store);
        amx_client_free(c);
        return NULL;
    }
    return c;
}

void amx_client_free(AmxClient *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < c->n_olm; i++) { olm_clear_session(c->olm[i].s); free(c->olm[i].s); free(c->olm[i].sender_key); }
    for (i = 0; i < c->n_grp; i++) {
        GroupSess *g = &c->grp[i];
        olm_clear_inbound_group_session(g->s); free(g->s);
        free(g->room_id); free(g->session_id); free(g->sender_key); free(g->ed25519);
    }
    free(c->olm); free(c->grp);
    if (c->acct) { olm_clear_account(c->acct); free(c->acct); }
    olm_clear_utility(c->util); free(c->util);
    cJSON_Delete(c->pending); cJSON_Delete(c->devices); cJSON_Delete(c->outbound);
    cJSON_Delete(c->nokey); cJSON_Delete(c->room_enc); cJSON_Delete(c->seen);
    cJSON_Delete(c->rooms); cJSON_Delete(c->direct); cJSON_Delete(c->hist);
    free(c->hs); free(c->store); free(c->user_id); free(c->device_id); free(c->token);
    free(c->curve); free(c->ed); free(c->since);
    free(c);
}

void amx_set_event_handler(AmxClient *c, AmxEventFn fn, void *userdata) { c->fn = fn; c->ud = userdata; }
void amx_set_verbose(AmxClient *c, int level) { c->verbose = level; }
const char *amx_error(AmxClient *c) { return c->err; }
const char *amx_user_id(AmxClient *c) { return c->user_id; }
const char *amx_device_id(AmxClient *c) { return c->device_id; }

/* -------------------------------------------------------------- login */

static int load_saved_login(AmxClient *c)
{
    char *saved = amx__read_store(c, "login.json");
    cJSON *j;
    int ok = 0;
    if (!saved) return 0;
    j = cJSON_Parse(saved);
    free(saved);
    if (j && amx__jstr(j, "user_id") && amx__jstr(j, "device_id") && amx__jstr(j, "access_token")) {
        c->user_id = amx__strdup(amx__jstr(j, "user_id"));
        c->device_id = amx__strdup(amx__jstr(j, "device_id"));
        c->token = amx__strdup(amx__jstr(j, "access_token"));
        ok = 1;
    }
    cJSON_Delete(j);
    return ok;
}

int amx_login(AmxClient *c, const char *user, const char *password)
{
    if (load_saved_login(c)) {
        LOG(c, "using saved login %s, device %s", c->user_id, c->device_id);
    } else {
        cJSON *req = cJSON_CreateObject(), *id, *j, *s;
        cJSON_AddStringToObject(req, "type", "m.login.password");
        id = cJSON_AddObjectToObject(req, "identifier");
        cJSON_AddStringToObject(id, "type", "m.id.user");
        cJSON_AddStringToObject(id, "user", user);
        cJSON_AddStringToObject(req, "password", password);
        cJSON_AddStringToObject(req, "initial_device_display_name", "Amatrix (Amiga)");
        j = amx__api_ok(c, "POST", "/login", req);
        cJSON_Delete(req);
        if (!j || !amx__jstr(j, "user_id") || !amx__jstr(j, "device_id") || !amx__jstr(j, "access_token")) {
            cJSON_Delete(j);
            return -1;
        }
        c->user_id = amx__strdup(amx__jstr(j, "user_id"));
        c->device_id = amx__strdup(amx__jstr(j, "device_id"));
        c->token = amx__strdup(amx__jstr(j, "access_token"));
        cJSON_Delete(j);
        s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "user_id", c->user_id);
        cJSON_AddStringToObject(s, "device_id", c->device_id);
        cJSON_AddStringToObject(s, "access_token", c->token);
        if (amx__save_json(c, "login.json", s)) { cJSON_Delete(s); return -1; }
        cJSON_Delete(s);
        amx__write_store(c, "keys_uploaded", "0");
        LOG(c, "logged in as %s, new device %s", c->user_id, c->device_id);
    }
    if (amx__load_or_create_account(c)) return -1;
    if (amx__load_sessions(c)) return -1;
    if (amx__load_send_state(c)) return -1;
    if (amx__rooms_load(c)) return -1;
    c->since = amx__read_store(c, "since");
    {
        /* publish device keys right away, so others can encrypt for us
         * before our first /sync */
        char *flag = amx__read_store(c, "keys_uploaded");
        int fresh = !flag || flag[0] != '1';
        free(flag);
        if (fresh && amx__upload_keys(c, 0, 1)) return -1;
    }
    return 0;
}

int amx_logout(AmxClient *c)
{
    static const char *files[] = { "login.json", "account", "keys_uploaded", "olm_sessions",
                                   "megolm_sessions", "megolm_out.json", "since", "rooms.json",
                                   "direct.json", NULL };
    cJSON *body = cJSON_CreateObject(), *r = amx__api_ok(c, "POST", "/logout", body);
    int i;
    cJSON_Delete(body);
    if (!r) return -1;
    cJSON_Delete(r);
    for (i = 0; files[i]; i++) {
        char *p = amx__malloc(strlen(c->store) + strlen(files[i]) + 2);
        sprintf(p, "%s/%s", c->store, files[i]);
        remove(p);
        free(p);
    }
    free(c->token);
    c->token = NULL;
    return 0;
}

char *amx_to_local(const char *utf8) { return plat_display(utf8); }
char *amx_from_local(const char *local) { return plat_from_local(local); }
