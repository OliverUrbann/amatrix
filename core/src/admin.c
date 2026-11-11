/* Members, room settings and moderation: the member list with power levels,
 * name/topic/picture/join rule/history visibility/encryption, kick, ban,
 * unban and power levels. */
#include "amx_internal.h"

static char *room_path(const char *room_id, const char *rest)
{
    char *esc = http_escape(room_id), *p = amx__malloc(strlen(esc) + strlen(rest) + 16);
    sprintf(p, "/rooms/%s%s", esc, rest);
    free(esc);
    return p;
}

/* GET a state event's content (NULL if not set or on error; *missing = 1
 * for "not set") */
static cJSON *get_state(AmxClient *c, const char *room_id, const char *type, int *missing)
{
    char *esc = http_escape(type), *rest = amx__malloc(strlen(esc) + 16), *p;
    cJSON *r;
    long st;
    sprintf(rest, "/state/%s/", esc);
    free(esc);
    p = room_path(room_id, rest);
    free(rest);
    r = amx__api(c, "GET", p, NULL, 60, &st);
    free(p);
    if (missing) *missing = st == 404;
    if (st != 200) { cJSON_Delete(r); return NULL; }
    return r;
}

static int put_state(AmxClient *c, const char *room_id, const char *type, cJSON *content)
{
    char *esc = http_escape(type), *rest = amx__malloc(strlen(esc) + 16), *p;
    cJSON *r;
    sprintf(rest, "/state/%s/", esc);
    free(esc);
    p = room_path(room_id, rest);
    free(rest);
    r = amx__api_ok(c, "PUT", p, content);
    cJSON_Delete(content);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    LOG(c, "set %s in %s", type, room_id);
    return 0;
}

static int num(const cJSON *o, const char *k, int def)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(x) ? x->valueint : def;
}

/* the level needed for a state event type */
static int state_level(const cJSON *pl, const char *type)
{
    return num(cJSON_GetObjectItemCaseSensitive(pl, "events"), type, num(pl, "state_default", pl ? 50 : 0));
}

/* the room's creators: room version 12+ gives them unlimited power (they are
 * not in the power levels), older versions take "creator" for the default
 * power levels only */
typedef struct { const char *sender; const cJSON *content; int v12; } Create;

static void get_create(const cJSON *state, Create *cr)
{
    const cJSON *ev;
    memset(cr, 0, sizeof *cr);
    cJSON_ArrayForEach(ev, state) {
        const char *t = amx__jstr(ev, "type");
        if (t && !strcmp(t, "m.room.create")) {
            const char *v;
            cr->sender = amx__jstr(ev, "sender");
            cr->content = cJSON_GetObjectItemCaseSensitive(ev, "content");
            v = amx__jstr(cr->content, "room_version");
            cr->v12 = v && atoi(v) >= 12;
            return;
        }
    }
}

static int is_creator(const Create *cr, const char *user)
{
    const cJSON *x;
    if (!user || !cr->sender) return 0;
    if (!strcmp(cr->sender, user)) return 1;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(cr->content, "additional_creators"))
        if (cJSON_IsString(x) && !strcmp(x->valuestring, user)) return 1;
    return 0;
}

static int user_level(const cJSON *pl, const char *user, const Create *cr)
{
    if (cr && cr->v12 && is_creator(cr, user)) return AMX_POWER_CREATOR;
    if (!pl) return cr && is_creator(cr, user) ? 100 : 0;
    return num(cJSON_GetObjectItemCaseSensitive(pl, "users"), user, num(pl, "users_default", 0));
}

/* the whole room state (array of events); NULL on error */
static cJSON *room_state(AmxClient *c, const char *room_id)
{
    char *p = room_path(room_id, "/state");
    cJSON *r = amx__api_ok(c, "GET", p, NULL);
    free(p);
    return r;
}

static const cJSON *state_content(const cJSON *state, const char *type)
{
    const cJSON *ev;
    cJSON_ArrayForEach(ev, state) {
        const char *t = amx__jstr(ev, "type"), *sk = amx__jstr(ev, "state_key");
        if (t && !strcmp(t, type) && sk && !*sk) return cJSON_GetObjectItemCaseSensitive(ev, "content");
    }
    return NULL;
}

static int by_power(const void *a, const void *b)
{
    const AmxMember *x = a, *y = b;
    if (x->membership[0] != y->membership[0]) return x->membership[0] == 'j' ? -1 : 1;  /* joined first */
    if (x->power != y->power) return y->power - x->power;
    return strcmp(x->name, y->name);
}

int amx_room_members(AmxClient *c, const char *room_id, AmxMember **out)
{
    cJSON *r = room_state(c, room_id);
    const cJSON *pl, *ev;
    Create cr;
    AmxMember *l;
    int n = 0;
    *out = NULL;
    if (!r) return -1;
    pl = state_content(r, "m.room.power_levels");
    get_create(r, &cr);
    l = amx__malloc(sizeof *l * (cJSON_GetArraySize(r) + 1));
    cJSON_ArrayForEach(ev, r) {
        if (!amx__jstr(ev, "type") || strcmp(amx__jstr(ev, "type"), "m.room.member")) continue;
        {
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content");
        const char *u = amx__jstr(ev, "state_key"), *ms = amx__jstr(ct, "membership");
        const char *dn = amx__jstr(ct, "displayname"), *av = amx__jstr(ct, "avatar_url");
        if (!u || !ms || (strcmp(ms, "join") && strcmp(ms, "invite") && strcmp(ms, "ban") && strcmp(ms, "knock"))) continue;
        l[n].user_id = amx__strdup(u);
        if (dn && *dn) l[n].name = amx__strdup(dn);
        else {
            const char *s = u[0] == '@' ? u + 1 : u;
            size_t k = strcspn(s, ":");
            l[n].name = amx__malloc(k + 1);
            memcpy(l[n].name, s, k);
            l[n].name[k] = 0;
        }
        l[n].avatar_url = av ? amx__strdup(av) : NULL;
        l[n].membership = amx__strdup(ms);
        l[n].power = user_level(pl, u, &cr);
        n++;
        }
    }
    qsort(l, n, sizeof *l, by_power);
    cJSON_Delete(r);
    *out = l;
    return n;
}

void amx_free_members(AmxMember *l, int n)
{
    int i;
    if (!l) return;
    for (i = 0; i < n; i++) { free(l[i].user_id); free(l[i].name); free(l[i].avatar_url); free(l[i].membership); }
    free(l);
}

int amx_room_settings(AmxClient *c, const char *room_id, AmxRoomSettings *s)
{
    cJSON *st = room_state(c, room_id);
    const cJSON *x, *pl;
    Create cr;
    memset(s, 0, sizeof *s);
    if (!st) return -1;
    get_create(st, &cr);
    pl = state_content(st, "m.room.power_levels");
    if ((x = state_content(st, "m.room.name")) && amx__jstr(x, "name") && *amx__jstr(x, "name"))
        s->name = amx__strdup(amx__jstr(x, "name"));
    if ((x = state_content(st, "m.room.topic")) && amx__jstr(x, "topic") && *amx__jstr(x, "topic"))
        s->topic = amx__strdup(amx__jstr(x, "topic"));
    if ((x = state_content(st, "m.room.avatar")) && amx__jstr(x, "url") && *amx__jstr(x, "url"))
        s->avatar_url = amx__strdup(amx__jstr(x, "url"));
    x = state_content(st, "m.room.join_rules");
    s->join_rule = amx__strdup(amx__jstr(x, "join_rule") ? amx__jstr(x, "join_rule") : "invite");
    x = state_content(st, "m.room.history_visibility");
    s->history = amx__strdup(amx__jstr(x, "history_visibility") ? amx__jstr(x, "history_visibility") : "shared");
    if ((x = state_content(st, "m.room.encryption")) && amx__jstr(x, "algorithm")) {
        s->encrypted = 1;
        amx__note_room_encryption(c, room_id, 1);
    }
    s->my_power = user_level(pl, c->user_id, &cr);
    s->level_name = state_level(pl, "m.room.name");
    s->level_topic = state_level(pl, "m.room.topic");
    s->level_avatar = state_level(pl, "m.room.avatar");
    s->level_join_rules = state_level(pl, "m.room.join_rules");
    s->level_history = state_level(pl, "m.room.history_visibility");
    s->level_encryption = state_level(pl, "m.room.encryption");
    s->level_power = state_level(pl, "m.room.power_levels");
    s->level_invite = num(pl, "invite", 0);
    s->level_kick = num(pl, "kick", 50);
    s->level_ban = num(pl, "ban", 50);
    s->level_redact = num(pl, "redact", 50);
    cJSON_Delete(st);
    return 0;
}

void amx_free_room_settings(AmxRoomSettings *s)
{
    free(s->name); free(s->topic); free(s->avatar_url); free(s->join_rule); free(s->history);
    memset(s, 0, sizeof *s);
}

int amx_set_room_setting(AmxClient *c, const char *room_id, const char *what, const char *value)
{
    cJSON *ct = cJSON_CreateObject();
    if (!value) value = "";
    if (!strcmp(what, "name")) {
        cJSON_AddStringToObject(ct, "name", value);
        return put_state(c, room_id, "m.room.name", ct);
    }
    if (!strcmp(what, "topic")) {
        cJSON_AddStringToObject(ct, "topic", value);
        return put_state(c, room_id, "m.room.topic", ct);
    }
    if (!strcmp(what, "join_rule")) {
        if (strcmp(value, "public") && strcmp(value, "invite") && strcmp(value, "knock")) {
            cJSON_Delete(ct);
            return amx__fail(c, "unknown join rule %s", value);
        }
        cJSON_AddStringToObject(ct, "join_rule", value);
        return put_state(c, room_id, "m.room.join_rules", ct);
    }
    if (!strcmp(what, "history")) {
        if (strcmp(value, "world_readable") && strcmp(value, "shared") && strcmp(value, "invited")
            && strcmp(value, "joined")) {
            cJSON_Delete(ct);
            return amx__fail(c, "unknown history visibility %s", value);
        }
        cJSON_AddStringToObject(ct, "history_visibility", value);
        return put_state(c, room_id, "m.room.history_visibility", ct);
    }
    if (!strcmp(what, "encryption")) {                    /* cannot be switched off again */
        int r;
        cJSON_AddStringToObject(ct, "algorithm", "m.megolm.v1.aes-sha2");
        r = put_state(c, room_id, "m.room.encryption", ct);
        if (!r) amx__note_room_encryption(c, room_id, 1);
        return r;
    }
    if (!strcmp(what, "avatar")) {                        /* value: a picture file */
        cJSON *fc = amx__file_content(c, value, NULL, 0);
        const char *url = amx__jstr(fc, "url");
        cJSON *info = cJSON_GetObjectItemCaseSensitive(fc, "info");
        if (!url) { cJSON_Delete(fc); cJSON_Delete(ct); return -1; }
        cJSON_AddStringToObject(ct, "url", url);
        if (info) cJSON_AddItemToObject(ct, "info", cJSON_Duplicate(info, 1));
        cJSON_Delete(fc);
        return put_state(c, room_id, "m.room.avatar", ct);
    }
    cJSON_Delete(ct);
    return amx__fail(c, "unknown room setting %s", what);
}

static int moderate(AmxClient *c, const char *room_id, const char *what, const char *user, const char *reason)
{
    char rest[16], *p;
    cJSON *b = cJSON_CreateObject(), *r;
    sprintf(rest, "/%s", what);
    p = room_path(room_id, rest);
    cJSON_AddStringToObject(b, "user_id", user);
    if (reason && *reason) cJSON_AddStringToObject(b, "reason", reason);
    r = amx__api_ok(c, "POST", p, b);
    cJSON_Delete(b);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    LOG(c, "%s %s in %s", what, user, room_id);
    return 0;
}

int amx_kick(AmxClient *c, const char *room_id, const char *user, const char *reason)
{ return moderate(c, room_id, "kick", user, reason); }
int amx_ban(AmxClient *c, const char *room_id, const char *user, const char *reason)
{ return moderate(c, room_id, "ban", user, reason); }
int amx_unban(AmxClient *c, const char *room_id, const char *user)
{ return moderate(c, room_id, "unban", user, NULL); }

int amx_set_power(AmxClient *c, const char *room_id, const char *user, int level)
{
    int missing = 0;
    cJSON *pl = get_state(c, room_id, "m.room.power_levels", &missing), *users;
    if (!pl) {
        if (!missing) return -1;
        pl = cJSON_CreateObject();
    }
    users = cJSON_GetObjectItemCaseSensitive(pl, "users");
    if (!cJSON_IsObject(users)) {
        cJSON_DeleteItemFromObjectCaseSensitive(pl, "users");
        users = cJSON_AddObjectToObject(pl, "users");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(users, user);
    if (level != num(pl, "users_default", 0)) cJSON_AddNumberToObject(users, user, level);
    return put_state(c, room_id, "m.room.power_levels", pl);
}

/* ---------------------------------------------------------- own profile */

int amx_get_profile(AmxClient *c, const char *user, char **name, char **avatar_url)
{
    char *esc = http_escape(user ? user : c->user_id ? c->user_id : ""), *p = amx__malloc(strlen(esc) + 16);
    cJSON *r;
    long st;
    *name = *avatar_url = NULL;
    sprintf(p, "/profile/%s", esc);
    free(esc);
    r = amx__api(c, "GET", p, NULL, 60, &st);
    free(p);
    if (st == 404) { cJSON_Delete(r); return 0; }        /* no profile yet */
    if (st != 200) { cJSON_Delete(r); return -1; }
    if (amx__jstr(r, "displayname")) *name = amx__strdup(amx__jstr(r, "displayname"));
    if (amx__jstr(r, "avatar_url")) *avatar_url = amx__strdup(amx__jstr(r, "avatar_url"));
    cJSON_Delete(r);
    return 0;
}

static int put_profile(AmxClient *c, const char *field, const char *value)
{
    char *esc = http_escape(c->user_id ? c->user_id : ""), *p = amx__malloc(strlen(esc) + 40);
    cJSON *b = cJSON_CreateObject(), *r;
    sprintf(p, "/profile/%s/%s", esc, field);
    free(esc);
    cJSON_AddStringToObject(b, field, value);
    r = amx__api_ok(c, "PUT", p, b);
    cJSON_Delete(b);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    LOG(c, "profile: %s set", field);
    return 0;
}

int amx_set_displayname(AmxClient *c, const char *name)
{
    if (!c->token) return amx__fail(c, "not logged in");
    return put_profile(c, "displayname", name ? name : "");
}

int amx_set_avatar(AmxClient *c, const char *path)
{
    cJSON *fc;
    int r;
    if (!c->token) return amx__fail(c, "not logged in");
    if (!(fc = amx__file_content(c, path, NULL, 0))) return -1;
    if (!amx__jstr(fc, "url") || strcmp(amx__jstr(fc, "msgtype"), "m.image")) {
        cJSON_Delete(fc);
        return amx__fail(c, "not a picture (PNG, JPEG or GIF)");
    }
    r = put_profile(c, "avatar_url", amx__jstr(fc, "url"));
    cJSON_Delete(fc);
    return r;
}

/* ------------------------------------------------------------- sessions */

static int by_seen(const void *a, const void *b)
{
    const AmxDevice *x = a, *y = b;
    if (x->current != y->current) return y->current - x->current;
    return x->last_seen_ts < y->last_seen_ts ? 1 : x->last_seen_ts > y->last_seen_ts ? -1 : 0;
}

int amx_devices(AmxClient *c, AmxDevice **out)
{
    cJSON *r = amx__api_ok(c, "GET", "/devices", NULL), *req, *keys = NULL;
    const cJSON *d, *ssk = NULL, *mine = NULL;
    AmxDevice *l;
    long st;
    int n = 0;
    *out = NULL;
    if (!r) return -1;
    req = cJSON_CreateObject();                  /* which are cross-signed (verified) */
    cJSON_AddArrayToObject(cJSON_AddObjectToObject(req, "device_keys"), c->user_id);
    keys = amx__api(c, "POST", "/keys/query", req, 60, &st);
    cJSON_Delete(req);
    if (st == 200 && keys) {
        ssk = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetObjectItemCaseSensitive(keys, "self_signing_keys"), c->user_id), "keys");
        mine = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(keys, "device_keys"), c->user_id);
    }
    l = amx__malloc(sizeof *l * (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "devices")) + 1));
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(r, "devices")) {
        const char *id = amx__jstr(d, "device_id"), *nm = amx__jstr(d, "display_name"), *ip = amx__jstr(d, "last_seen_ip");
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(d, "last_seen_ts"), *dk;
        if (!id) continue;
        memset(&l[n], 0, sizeof l[n]);
        l[n].device_id = amx__strdup(id);
        l[n].name = nm ? amx__strdup(nm) : NULL;
        l[n].last_seen_ip = ip ? amx__strdup(ip) : NULL;
        l[n].last_seen_ts = cJSON_IsNumber(ts) ? ts->valuedouble : 0;
        l[n].current = c->device_id && !strcmp(id, c->device_id);
        l[n].verified = -1;                       /* no device keys: not an E2EE device */
        dk = cJSON_GetObjectItemCaseSensitive(mine, id);
        if (dk) {
            l[n].verified = 0;
            if (ssk && ssk->child && cJSON_IsString(ssk->child) && strlen(ssk->child->valuestring) < 80) {
                char kid[96];
                cJSON *x = cJSON_Duplicate(dk, 1);
                cJSON_DeleteItemFromObjectCaseSensitive(x, "unsigned");
                sprintf(kid, "ed25519:%s", ssk->child->valuestring);
                l[n].verified = amx__verify_signed(c, x, c->user_id, kid, ssk->child->valuestring) == 1;
                cJSON_Delete(x);
            }
        }
        n++;
    }
    qsort(l, n, sizeof *l, by_seen);
    cJSON_Delete(keys);
    cJSON_Delete(r);
    *out = l;
    return n;
}

void amx_free_devices(AmxDevice *l, int n)
{
    int i;
    if (!l) return;
    for (i = 0; i < n; i++) { free(l[i].device_id); free(l[i].name); free(l[i].last_seen_ip); }
    free(l);
}

int amx_rename_device(AmxClient *c, const char *device_id, const char *name)
{
    char *esc = http_escape(device_id), *p = amx__malloc(strlen(esc) + 16);
    cJSON *b = cJSON_CreateObject(), *r;
    sprintf(p, "/devices/%s", esc);
    free(esc);
    cJSON_AddStringToObject(b, "display_name", name ? name : "");
    r = amx__api_ok(c, "PUT", p, b);
    cJSON_Delete(b);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    return 0;
}

/* signing out another session needs our password (user-interactive auth) */
int amx_delete_device(AmxClient *c, const char *device_id, const char *password)
{
    char *esc = http_escape(device_id), *p = amx__malloc(strlen(esc) + 16);
    cJSON *b = cJSON_CreateObject(), *r, *auth, *idf;
    long st;
    const char *session;
    sprintf(p, "/devices/%s", esc);
    free(esc);
    if (c->device_id && !strcmp(device_id, c->device_id)) {
        free(p);
        cJSON_Delete(b);
        return amx__fail(c, "this is the session in use - log out instead");
    }
    r = amx__api(c, "DELETE", p, b, 60, &st);           /* first: which auth the server wants */
    cJSON_Delete(b);
    if (st == 200) { cJSON_Delete(r); free(p); LOG(c, "signed out %s", device_id); return 0; }
    session = amx__jstr(r, "session");
    if (st != 401 || !session) { cJSON_Delete(r); free(p); return st == 401 ? amx__fail(c, "the server wants another way to confirm") : -1; }
    c->err[0] = 0;
    b = cJSON_CreateObject();
    auth = cJSON_AddObjectToObject(b, "auth");
    cJSON_AddStringToObject(auth, "type", "m.login.password");
    cJSON_AddStringToObject(auth, "session", session);
    idf = cJSON_AddObjectToObject(auth, "identifier");
    cJSON_AddStringToObject(idf, "type", "m.id.user");
    cJSON_AddStringToObject(idf, "user", c->user_id);
    cJSON_AddStringToObject(auth, "password", password ? password : "");
    cJSON_Delete(r);
    r = amx__api(c, "DELETE", p, b, 60, &st);
    {   /* the password out of memory */
        cJSON *pw = cJSON_GetObjectItemCaseSensitive(auth, "password");
        if (pw && pw->valuestring) memset(pw->valuestring, 0, strlen(pw->valuestring));
    }
    cJSON_Delete(b);
    cJSON_Delete(r);
    free(p);
    if (st == 401) return amx__fail(c, "wrong password");
    if (st != 200) return -1;
    LOG(c, "signed out %s", device_id);
    return 0;
}

/* ------------------------------------------------------- ignored users */

static char *ignore_path(AmxClient *c)
{
    char *eu = http_escape(c->user_id ? c->user_id : ""), *p = amx__malloc(strlen(eu) + 64);
    sprintf(p, "/user/%s/account_data/m.ignored_user_list", eu);
    free(eu);
    return p;
}

char *amx_ignored_users(AmxClient *c)
{
    char *p = ignore_path(c), *out;
    cJSON *r, *x;
    long st;
    size_t n = 1;
    r = amx__api(c, "GET", p, NULL, 60, &st);
    free(p);
    if (st == 404) { cJSON_Delete(r); c->err[0] = 0; return amx__strdup(""); }
    if (st != 200) { cJSON_Delete(r); return NULL; }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "ignored_users")) n += strlen(x->string) + 1;
    out = amx__malloc(n);
    out[0] = 0;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "ignored_users")) {
        if (out[0]) strcat(out, "\n");
        strcat(out, x->string);
    }
    cJSON_Delete(r);
    return out;
}

int amx_ignore_user(AmxClient *c, const char *user, int on)
{
    char *p = ignore_path(c);
    cJSON *r, *list, *res;
    long st;
    if (!user || user[0] != '@' || !strchr(user, ':')) { free(p); return amx__fail(c, "not a user ID: %s", user ? user : ""); }
    r = amx__api(c, "GET", p, NULL, 60, &st);
    if (st != 200 || !cJSON_IsObject(r)) { cJSON_Delete(r); r = cJSON_CreateObject(); c->err[0] = 0; }
    list = cJSON_GetObjectItemCaseSensitive(r, "ignored_users");
    if (!cJSON_IsObject(list)) {
        cJSON_DeleteItemFromObjectCaseSensitive(r, "ignored_users");
        list = cJSON_AddObjectToObject(r, "ignored_users");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(list, user);
    if (on) cJSON_AddObjectToObject(list, user);
    res = amx__api_ok(c, "PUT", p, r);
    cJSON_Delete(r);
    free(p);
    if (!res) return -1;
    cJSON_Delete(res);
    LOG(c, "%s %s", on ? "ignoring" : "no longer ignoring", user);
    return 0;
}

/* ------------------------------------------------------------- account */

/* a request that the server wants confirmed with our password (UIA) */
static int uia_password(AmxClient *c, const char *method, const char *path, cJSON *body, const char *password)
{
    cJSON *r, *auth, *idf, *pw;
    long st;
    const char *session;
    r = amx__api(c, method, path, body, 60, &st);
    if (st == 200) { cJSON_Delete(r); return 0; }
    session = amx__jstr(r, "session");
    if (st != 401 || !session) { cJSON_Delete(r); return -1; }
    c->err[0] = 0;
    auth = cJSON_AddObjectToObject(body, "auth");
    cJSON_AddStringToObject(auth, "type", "m.login.password");
    cJSON_AddStringToObject(auth, "session", session);
    idf = cJSON_AddObjectToObject(auth, "identifier");
    cJSON_AddStringToObject(idf, "type", "m.id.user");
    cJSON_AddStringToObject(idf, "user", c->user_id);
    cJSON_AddStringToObject(auth, "password", password ? password : "");
    cJSON_Delete(r);
    r = amx__api(c, method, path, body, 60, &st);
    pw = cJSON_GetObjectItemCaseSensitive(auth, "password");
    if (pw && pw->valuestring) memset(pw->valuestring, 0, strlen(pw->valuestring));
    cJSON_Delete(r);
    if (st == 401) return amx__fail(c, "wrong password");
    return st == 200 ? 0 : -1;
}

int amx_change_password(AmxClient *c, const char *old_password, const char *new_password)
{
    cJSON *b;
    int r;
    if (!new_password || strlen(new_password) < 8) return amx__fail(c, "the new password is too short (8 characters or more)");
    b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "new_password", new_password);
    cJSON_AddFalseToObject(b, "logout_devices");
    r = uia_password(c, "POST", "/account/password", b, old_password);
    {
        cJSON *np = cJSON_GetObjectItemCaseSensitive(b, "new_password");
        if (np && np->valuestring) memset(np->valuestring, 0, strlen(np->valuestring));
    }
    cJSON_Delete(b);
    if (!r) LOG(c, "password changed");
    return r;
}

int amx_deactivate_account(AmxClient *c, const char *password, int erase)
{
    cJSON *b = cJSON_CreateObject();
    int r;
    cJSON_AddBoolToObject(b, "erase", erase != 0);
    r = uia_password(c, "POST", "/account/deactivate", b, password);
    cJSON_Delete(b);
    if (!r) LOG(c, "account deactivated");
    return r;
}

int amx_knock(AmxClient *c, const char *room, const char *reason)
{
    char *e = http_escape(room), *p = amx__malloc(strlen(e) + 16);
    cJSON *b = cJSON_CreateObject(), *r;
    sprintf(p, "/knock/%s", e);
    free(e);
    if (reason && *reason) cJSON_AddStringToObject(b, "reason", reason);
    r = amx__api_ok(c, "POST", p, b);
    cJSON_Delete(b);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    LOG(c, "knocked on %s", room);
    return 0;
}

/* ------------------------------------------------- before joining; spaces */

static void summary_from(AmxRoomSummary *s, const cJSON *j)
{
    const char *v;
    const cJSON *x;
    memset(s, 0, sizeof *s);
    if ((v = amx__jstr(j, "room_id"))) s->room_id = amx__strdup(v);
    if ((v = amx__jstr(j, "name")) && *v) s->name = amx__strdup(v);
    if ((v = amx__jstr(j, "topic")) && *v) s->topic = amx__strdup(v);
    if ((v = amx__jstr(j, "canonical_alias")) && *v) s->alias = amx__strdup(v);
    if ((v = amx__jstr(j, "avatar_url")) && *v) s->avatar_url = amx__strdup(v);
    s->join_rule = amx__strdup((v = amx__jstr(j, "join_rule")) ? v : "public");
    x = cJSON_GetObjectItemCaseSensitive(j, "num_joined_members");
    s->members = cJSON_IsNumber(x) ? x->valueint : 0;
    s->is_space = (v = amx__jstr(j, "room_type")) && !strcmp(v, "m.space");
    s->world_readable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "world_readable"));
    v = amx__jstr(j, "membership");
    s->joined = v && !strcmp(v, "join");
}

int amx_room_summary(AmxClient *c, const char *room, AmxRoomSummary *s)
{
    static const char *const paths[] = { "/_matrix/client/v1/room_summary/", "/_matrix/client/unstable/im.nheko.summary/rooms/",
                                         "/_matrix/client/unstable/im.nheko.summary/summary/" };
    char *esc = http_escape(room), *full = amx__malloc(strlen(c->hs) + strlen(esc) + 80);
    HttpResponse r;
    int i, ok = -1;
    memset(s, 0, sizeof *s);
    for (i = 0; i < 3 && ok < 0; i++) {
        sprintf(full, "%s%s%s", c->hs, paths[i], esc);
        if (i == 1) strcat(full, "/summary");
        LOG2(c, "> GET %s (room summary)", full + strlen(c->hs));
        if (http_request("GET", full, c->token, NULL, 60, &r) != 0) {
            amx__fail(c, "network error: %s (room summary)", http_last_error());
            break;
        }
        if (r.status == 200) {
            cJSON *j = r.body ? cJSON_Parse(r.body) : NULL;
            if (j) { summary_from(s, j); ok = 0; }
            cJSON_Delete(j);
        } else if (r.status == 403 || (r.status == 404 && r.body && strstr(r.body, "M_NOT_FOUND") && !strstr(r.body, "Unrecognized"))) {
            amx__fail(c, "the room is private or unknown");
            http_response_free(&r);
            break;
        }
        LOG2(c, "< %ld", r.status);
        http_response_free(&r);
    }
    free(esc);
    free(full);
    if (ok < 0 && !c->err[0]) amx__fail(c, "the server has no room preview");
    return ok;
}

void amx_free_room_summary(AmxRoomSummary *s)
{
    free(s->room_id); free(s->name); free(s->topic); free(s->alias); free(s->join_rule); free(s->avatar_url);
    memset(s, 0, sizeof *s);
}

int amx_space_rooms(AmxClient *c, const char *space_id, int depth, AmxRoomSummary **out)
{
    char *esc = http_escape(space_id), *p = amx__malloc(strlen(esc) + 80);
    cJSON *r;
    const cJSON *x;
    AmxRoomSummary *l;
    int n = 0;
    *out = NULL;
    sprintf(p, "/_matrix/client/v1/rooms/%s/hierarchy?limit=100&max_depth=%d", esc, depth < 1 ? 1 : depth);
    free(esc);
    {   /* a v1 path: not under /_matrix/client/v3 */
        char *full = amx__malloc(strlen(c->hs) + strlen(p) + 4);
        HttpResponse h;
        sprintf(full, "%s%s", c->hs, p);
        free(p);
        if (http_request("GET", full, c->token, NULL, 60, &h) != 0) {
            free(full);
            return amx__fail(c, "network error: %s (space)", http_last_error());
        }
        free(full);
        if (h.status != 200) {
            long st = h.status;
            http_response_free(&h);
            return amx__fail(c, "space: HTTP %ld", st);
        }
        r = h.body ? cJSON_Parse(h.body) : NULL;
        http_response_free(&h);
    }
    if (!r) return amx__fail(c, "space: bad answer");
    l = amx__malloc(sizeof *l * (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "rooms")) + 1));
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "rooms")) {
        const cJSON *m;
        summary_from(&l[n], x);
        m = cJSON_GetObjectItemCaseSensitive(c->rooms, l[n].room_id ? l[n].room_id : "");
        l[n].joined = m && amx__jstr(m, "m") && !strcmp(amx__jstr(m, "m"), "join");
        n++;
    }
    cJSON_Delete(r);
    *out = l;
    return n;
}

void amx_free_space_rooms(AmxRoomSummary *l, int n)
{
    int i;
    if (!l) return;
    for (i = 0; i < n; i++) amx_free_room_summary(&l[i]);
    free(l);
}

static int put_state_key(AmxClient *c, const char *room_id, const char *type, const char *key, cJSON *content)
{
    char *er = http_escape(room_id), *et = http_escape(type), *ek = http_escape(key), *p;
    cJSON *r;
    p = amx__malloc(strlen(er) + strlen(et) + strlen(ek) + 32);
    sprintf(p, "/rooms/%s/state/%s/%s", er, et, ek);
    free(er); free(et); free(ek);
    r = amx__api_ok(c, "PUT", p, content);
    cJSON_Delete(content);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    return 0;
}

int amx_space_add(AmxClient *c, const char *space_id, const char *room_id, int on)
{
    const char *srv = c->user_id ? strchr(c->user_id, ':') : NULL;
    cJSON *ct = cJSON_CreateObject();
    char err[512];
    if (on) cJSON_AddItemToArray(cJSON_AddArrayToObject(ct, "via"), cJSON_CreateString(srv ? srv + 1 : "localhost"));
    if (put_state_key(c, space_id, "m.space.child", room_id, ct)) return -1;
    snprintf(err, sizeof err, "%s", c->err);
    ct = cJSON_CreateObject();                 /* the room's side; optional (needs power there) */
    if (on) cJSON_AddItemToArray(cJSON_AddArrayToObject(ct, "via"), cJSON_CreateString(srv ? srv + 1 : "localhost"));
    if (put_state_key(c, room_id, "m.space.parent", space_id, ct)) snprintf(c->err, sizeof c->err, "%s", err);
    LOG(c, "space %s: %s %s", space_id, on ? "added" : "removed", room_id);
    return 0;
}

int amx_report_event(AmxClient *c, const char *room_id, const char *event_id, const char *reason)
{
    char *er = http_escape(room_id), *ee = http_escape(event_id), *p = amx__malloc(strlen(er) + strlen(ee) + 32);
    cJSON *b = cJSON_CreateObject(), *r;
    sprintf(p, "/rooms/%s/report/%s", er, ee);
    free(er); free(ee);
    cJSON_AddStringToObject(b, "reason", reason ? reason : "");
    r = amx__api_ok(c, "POST", p, b);
    cJSON_Delete(b);
    free(p);
    if (!r) return -1;
    cJSON_Delete(r);
    LOG(c, "reported %s", event_id);
    return 0;
}
