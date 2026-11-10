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
        if (!u || !ms || (strcmp(ms, "join") && strcmp(ms, "invite") && strcmp(ms, "ban"))) continue;
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
