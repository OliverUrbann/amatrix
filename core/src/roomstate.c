/* Room list: what we know about every joined/invited room from /sync,
 * persisted in the store (rooms.json, direct.json) because /sync only sends
 * changes after the first round.  Also m.direct handling and the room
 * display name (spec: name, alias, heroes). */
#include "amx_internal.h"

#define MAX_MEMBERS 300     /* display names kept per room (lazy loading keeps it small anyway) */

static cJSON *room(AmxClient *c, const char *room_id)
{
    cJSON *r = cJSON_GetObjectItemCaseSensitive(c->rooms, room_id);
    if (!r) {
        r = cJSON_AddObjectToObject(c->rooms, room_id);
        cJSON_AddObjectToObject(r, "members");
    }
    return r;
}

static void set_str(cJSON *o, const char *k, const char *v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    if (v && *v) cJSON_AddStringToObject(o, k, v);
}

static void set_num(cJSON *o, const char *k, double v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddNumberToObject(o, k, v);
}

static double get_num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int get_int(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

int amx__rooms_load(AmxClient *c)
{
    char *t = amx__read_store(c, "rooms.json");
    c->rooms = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!c->rooms) c->rooms = cJSON_CreateObject();
    t = amx__read_store(c, "direct.json");
    c->direct = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!c->direct) c->direct = cJSON_CreateObject();
    return 0;
}

int amx__rooms_save(AmxClient *c)
{
    return amx__save_json(c, "rooms.json", c->rooms) || amx__save_json(c, "direct.json", c->direct);
}

/* One state event (from state, timeline or invite_state). */
static void apply_state(AmxClient *c, cJSON *r, const char *room_id, const cJSON *ev)
{
    const char *type = amx__jstr(ev, "type"), *sk = amx__jstr(ev, "state_key");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(ev, "content");
    if (!type || !sk) return;
    if (!strcmp(type, "m.room.name")) set_str(r, "name", amx__jstr(ct, "name"));
    else if (!strcmp(type, "m.room.topic")) set_str(r, "topic", amx__jstr(ct, "topic"));
    else if (!strcmp(type, "m.room.canonical_alias")) set_str(r, "alias", amx__jstr(ct, "alias"));
    else if (!strcmp(type, "m.room.avatar")) set_str(r, "avatar", amx__jstr(ct, "url"));
    else if (!strcmp(type, "m.room.encryption")) {
        cJSON_DeleteItemFromObjectCaseSensitive(r, "enc");
        cJSON_AddTrueToObject(r, "enc");
        amx__note_room_encryption(c, room_id, 1);
    } else if (!strcmp(type, "m.room.member")) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "members");
        const char *ms = amx__jstr(ct, "membership"), *dn = amx__jstr(ct, "displayname");
        cJSON *av = cJSON_GetObjectItemCaseSensitive(r, "mav");
        const char *au = amx__jstr(ct, "avatar_url");
        if (!av) av = cJSON_AddObjectToObject(r, "mav");
        cJSON_DeleteItemFromObjectCaseSensitive(m, sk);
        cJSON_DeleteItemFromObjectCaseSensitive(av, sk);
        if (ms && (!strcmp(ms, "join") || !strcmp(ms, "invite")) && cJSON_GetArraySize(m) < MAX_MEMBERS) {
            cJSON_AddStringToObject(m, sk, dn && *dn ? dn : sk);
            if (au && !strncmp(au, "mxc://", 6)) cJSON_AddStringToObject(av, sk, au);
        }
        if (c->user_id && !strcmp(sk, c->user_id) && ms && !strcmp(ms, "invite")) {
            set_str(r, "inviter", amx__jstr(ev, "sender"));
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ct, "is_direct"))) {
                cJSON_DeleteItemFromObjectCaseSensitive(r, "idirect");
                cJSON_AddTrueToObject(r, "idirect");
            }
        }
    }
}

static void apply_events(AmxClient *c, cJSON *r, const char *room_id, const cJSON *events, int track_ts)
{
    const cJSON *ev;
    cJSON_ArrayForEach(ev, events) {
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(ev, "origin_server_ts");
        apply_state(c, r, room_id, ev);
        if (track_ts && cJSON_IsNumber(ts) && ts->valuedouble > get_num(r, "ts"))
            set_num(r, "ts", ts->valuedouble);
    }
}

void amx__room_sync_join(AmxClient *c, const char *room_id, const cJSON *jr)
{
    cJSON *r = room(c, room_id);
    const cJSON *sum = cJSON_GetObjectItemCaseSensitive(jr, "summary"), *un, *h;
    set_str(r, "m", "join");
    cJSON_DeleteItemFromObjectCaseSensitive(r, "inviter");
    cJSON_DeleteItemFromObjectCaseSensitive(r, "idirect");
    apply_events(c, r, room_id, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(jr, "state"), "events"), 0);
    apply_events(c, r, room_id, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(jr, "timeline"), "events"), 1);
    if ((h = cJSON_GetObjectItemCaseSensitive(sum, "m.heroes"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(r, "heroes");
        cJSON_AddItemToObject(r, "heroes", cJSON_Duplicate(h, 1));
    }
    if (cJSON_GetObjectItemCaseSensitive(sum, "m.joined_member_count"))
        set_num(r, "jc", get_int(sum, "m.joined_member_count"));
    if (cJSON_GetObjectItemCaseSensitive(sum, "m.invited_member_count"))
        set_num(r, "ic", get_int(sum, "m.invited_member_count"));
    if ((un = cJSON_GetObjectItemCaseSensitive(jr, "unread_notifications"))) {
        set_num(r, "unread", get_int(un, "notification_count"));
        set_num(r, "hl", get_int(un, "highlight_count"));
    }
}

void amx__room_sync_invite(AmxClient *c, const char *room_id, const cJSON *ir)
{
    cJSON *r = room(c, room_id);
    set_str(r, "m", "invite");
    apply_events(c, r, room_id, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ir, "invite_state"), "events"), 1);
}

void amx__room_forget(AmxClient *c, const char *room_id)
{
    cJSON_DeleteItemFromObjectCaseSensitive(c->rooms, room_id);
}

void amx__set_direct(AmxClient *c, const cJSON *content)
{
    cJSON_Delete(c->direct);
    c->direct = cJSON_Duplicate(content, 1);
}

/* display name of user in room as far as we know it, or NULL */
const char *amx__member_name(AmxClient *c, const char *room_id, const char *user)
{
    const cJSON *r = room_id ? cJSON_GetObjectItemCaseSensitive(c->rooms, room_id) : NULL;
    const char *n = amx__jstr(cJSON_GetObjectItemCaseSensitive(r, "members"), user);
    return n && strcmp(n, user) ? n : NULL;
}

const char *amx__room_inviter_if_direct(AmxClient *c, const char *room_id)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(c->rooms, room_id);
    if (!r || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "idirect"))) return NULL;
    return amx__jstr(r, "inviter");
}

/* Adds room to m.direct under user (account data, shared with our other devices). */
int amx__add_direct(AmxClient *c, const char *user, const char *room_id)
{
    cJSON *d = cJSON_Duplicate(c->direct, 1), *list, *x, *res;
    char *esc, *path;
    list = cJSON_GetObjectItemCaseSensitive(d, user);
    if (!list) list = cJSON_AddArrayToObject(d, user);
    cJSON_ArrayForEach(x, list)
        if (cJSON_IsString(x) && !strcmp(x->valuestring, room_id)) { cJSON_Delete(d); return 0; }
    cJSON_AddItemToArray(list, cJSON_CreateString(room_id));
    esc = http_escape(c->user_id);
    path = amx__malloc(strlen(esc) + 48);
    sprintf(path, "/user/%s/account_data/m.direct", esc);
    res = amx__api_ok(c, "PUT", path, d);
    free(path);
    free(esc);
    if (!res) { cJSON_Delete(d); return -1; }
    cJSON_Delete(res);
    cJSON_Delete(c->direct);
    c->direct = d;
    amx__rooms_save(c);
    return 0;
}

/* ------------------------------------------------------------ the list */

static const char *direct_user(AmxClient *c, const char *room_id)
{
    const cJSON *u, *x;
    cJSON_ArrayForEach(u, c->direct)
        cJSON_ArrayForEach(x, u)
            if (cJSON_IsString(x) && !strcmp(x->valuestring, room_id)) return u->string;
    return NULL;
}

static const char *display(const cJSON *r, const char *user)
{
    const char *dn = amx__jstr(cJSON_GetObjectItemCaseSensitive(r, "members"), user);
    return dn ? dn : user;
}

/* Spec 13.2.2 "calculating the display name", without localised words. */
static char *room_name(AmxClient *c, const char *room_id, const cJSON *r)
{
    const char *n = amx__jstr(r, "name");
    const cJSON *heroes = cJSON_GetObjectItemCaseSensitive(r, "heroes"), *h, *m;
    char buf[400];
    size_t len = 0;
    int shown = 0, total;
    if (n) return amx__strdup(n);
    if ((n = amx__jstr(r, "alias"))) return amx__strdup(n);
    buf[0] = 0;
    if (cJSON_GetArraySize(heroes)) {
        cJSON_ArrayForEach(h, heroes) {
            if (!cJSON_IsString(h) || (c->user_id && !strcmp(h->valuestring, c->user_id))) continue;
            if (shown < 5) len += snprintf(buf + len, sizeof buf - len, "%s%s", shown ? ", " : "", display(r, h->valuestring));
            shown++;
        }
    } else {
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(r, "members")) {
            if (c->user_id && !strcmp(m->string, c->user_id)) continue;
            if (shown < 5) len += snprintf(buf + len, sizeof buf - len, "%s%s", shown ? ", " : "", m->valuestring);
            shown++;
        }
    }
    total = get_int(r, "jc") + get_int(r, "ic") - 1;   /* without us */
    if (shown && total > (shown < 5 ? shown : 5) && len < sizeof buf - 16)
        snprintf(buf + len, sizeof buf - len, " (+%d)", total - (shown < 5 ? shown : 5));
    if (buf[0]) return amx__strdup(buf);
    if ((n = amx__jstr(r, "inviter"))) return amx__strdup(display(r, n));
    return amx__strdup(room_id);
}

/* the room's picture; for rooms without one the other person's (direct chat,
 * or a room with a single other hero) */
static const char *room_avatar(AmxClient *c, const cJSON *r, const char *dm_user)
{
    const char *a = amx__jstr(r, "avatar"), *other = dm_user;
    const cJSON *h;
    int n = 0;
    if (a) return a;
    if (!other) {
        cJSON_ArrayForEach(h, cJSON_GetObjectItemCaseSensitive(r, "heroes"))
            if (cJSON_IsString(h) && !(c->user_id && !strcmp(h->valuestring, c->user_id))) { other = h->valuestring; n++; }
        if (n != 1) other = NULL;
    }
    if (!other) other = amx__jstr(r, "inviter");
    return other ? amx__jstr(cJSON_GetObjectItemCaseSensitive(r, "mav"), other) : NULL;
}

static int cmp_rooms(const void *a, const void *b)
{
    const AmxRoomInfo *x = a, *y = b;
    if (x->membership != y->membership) return x->membership == AMX_ROOM_INVITED ? -1 : 1;
    if (x->last_activity != y->last_activity) return x->last_activity > y->last_activity ? -1 : 1;
    return strcmp(x->name, y->name);
}

static char *dupn(const char *s) { return s ? amx__strdup(s) : NULL; }

int amx_rooms(AmxClient *c, AmxRoomInfo **list)
{
    const cJSON *r;
    int n = 0;
    AmxRoomInfo *l = amx__malloc(sizeof *l * (cJSON_GetArraySize(c->rooms) + 1));
    cJSON_ArrayForEach(r, c->rooms) {
        AmxRoomInfo *i = &l[n];
        const char *m = amx__jstr(r, "m"), *du;
        if (!m) continue;
        memset(i, 0, sizeof *i);
        i->room_id = amx__strdup(r->string);
        i->name = room_name(c, r->string, r);
        i->topic = dupn(amx__jstr(r, "topic"));
        i->alias = dupn(amx__jstr(r, "alias"));
        i->inviter = dupn(amx__jstr(r, "inviter"));
        i->membership = !strcmp(m, "invite") ? AMX_ROOM_INVITED : AMX_ROOM_JOINED;
        du = direct_user(c, r->string);
        if (!du && i->membership == AMX_ROOM_INVITED && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "idirect")))
            du = i->inviter;
        i->dm_user = dupn(du);
        i->avatar_url = dupn(room_avatar(c, r, du));
        i->is_direct = du != NULL;
        i->encrypted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "enc"));
        i->joined_members = get_int(r, "jc");
        i->invited_members = get_int(r, "ic");
        i->unread = get_int(r, "unread");
        i->highlights = get_int(r, "hl");
        i->last_activity = get_num(r, "ts");
        n++;
    }
    qsort(l, n, sizeof *l, cmp_rooms);
    *list = l;
    return n;
}

void amx_free_rooms(AmxRoomInfo *list, int n)
{
    int i;
    if (!list) return;
    for (i = 0; i < n; i++) {
        free((char *)list[i].room_id); free((char *)list[i].name); free((char *)list[i].topic);
        free((char *)list[i].alias); free((char *)list[i].dm_user); free((char *)list[i].inviter);
        free((char *)list[i].avatar_url);
    }
    free(list);
}

/* -------------------------------------------------------- directory */

int amx_public_rooms(AmxClient *c, const char *server, const char *search, int limit,
                     AmxPublicRoom **list)
{
    cJSON *req = cJSON_CreateObject(), *res, *x;
    char *path, *esc = server ? http_escape(server) : NULL;
    int n = 0;
    AmxPublicRoom *l;
    *list = NULL;
    if (limit > 0) cJSON_AddNumberToObject(req, "limit", limit);
    if (search && *search) cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "filter"), "generic_search_term", search);
    path = amx__malloc((esc ? strlen(esc) : 0) + 40);
    if (esc) sprintf(path, "/publicRooms?server=%s", esc); else strcpy(path, "/publicRooms");
    res = amx__api_ok(c, "POST", path, req);
    cJSON_Delete(req);
    free(path);
    free(esc);
    if (!res) return -1;
    l = amx__malloc(sizeof *l * (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(res, "chunk")) + 1));
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(res, "chunk")) {
        if (!amx__jstr(x, "room_id")) continue;
        l[n].room_id = amx__strdup(amx__jstr(x, "room_id"));
        l[n].name = dupn(amx__jstr(x, "name"));
        l[n].topic = dupn(amx__jstr(x, "topic"));
        l[n].alias = dupn(amx__jstr(x, "canonical_alias"));
        l[n].members = get_int(x, "num_joined_members");
        n++;
    }
    cJSON_Delete(res);
    *list = l;
    return n;
}

void amx_free_public_rooms(AmxPublicRoom *list, int n)
{
    int i;
    if (!list) return;
    for (i = 0; i < n; i++) {
        free((char *)list[i].room_id); free((char *)list[i].name);
        free((char *)list[i].topic); free((char *)list[i].alias);
    }
    free(list);
}
