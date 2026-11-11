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
    else if (!strcmp(type, "m.room.create")) {
        const char *t = amx__jstr(ct, "type");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "ct");
        cJSON_AddStringToObject(r, "ct", t ? t : "");
    } else if (!strcmp(type, "m.room.tombstone")) {
        set_str(r, "repl", amx__jstr(ct, "replacement_room"));
    } else if (!strcmp(type, "m.room.pinned_events")) {
        const cJSON *pins = cJSON_GetObjectItemCaseSensitive(ct, "pinned");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "pins");
        if (cJSON_IsArray(pins)) cJSON_AddItemToObject(r, "pins", cJSON_Duplicate(pins, 1));
    } else if (!strcmp(type, "m.space.child")) {
        cJSON *ch = cJSON_GetObjectItemCaseSensitive(r, "children");
        const cJSON *via = cJSON_GetObjectItemCaseSensitive(ct, "via");
        if (!ch) ch = cJSON_AddObjectToObject(r, "children");
        cJSON_DeleteItemFromObjectCaseSensitive(ch, sk);
        if (cJSON_IsArray(via) && cJSON_GetArraySize(via) > 0) cJSON_AddTrueToObject(ch, sk);
    }
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
    {   /* room tags (favourite, low priority) */
        const cJSON *ev;
        cJSON_ArrayForEach(ev, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(jr, "account_data"), "events"))
            if (amx__jstr(ev, "type") && !strcmp(amx__jstr(ev, "type"), "m.tag")) {
                const cJSON *tags = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ev, "content"), "tags");
                cJSON_DeleteItemFromObjectCaseSensitive(r, "tags");
                cJSON_AddItemToObject(r, "tags", tags ? cJSON_Duplicate(tags, 1) : cJSON_CreateObject());
            }
    }
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

/* the newest message of a room, for the room list (live events only) */
void amx__room_note_message(AmxClient *c, const AmxEvent *ev)
{
    cJSON *r, *l;
    char body[124];
    const char *b;
    size_t n;
    if (!ev->room_id || (ev->type != AMX_EV_MESSAGE && ev->type != AMX_EV_UNDECRYPTABLE)) return;
    if (!(r = cJSON_GetObjectItemCaseSensitive(c->rooms, ev->room_id))) return;
    l = cJSON_GetObjectItemCaseSensitive(r, "last");
    if (l && get_num(l, "ts") > ev->timestamp) return;
    /* from the history only the newest message itself, now decrypted (its key
     * came from the backup or another device), or a first preview */
    if (ev->history && (ev->type == AMX_EV_UNDECRYPTABLE ? l != NULL :
        l && !(ev->event_id && amx__jstr(l, "id") && !strcmp(amx__jstr(l, "id"), ev->event_id))))
        return;
    b = ev->type == AMX_EV_MESSAGE ? (ev->body ? ev->body : "") : "(verschl\xc3\xbcsselt)";
    if (ev->type == AMX_EV_MESSAGE && ev->msgtype && strcmp(ev->msgtype, "m.text") && strcmp(ev->msgtype, "m.notice")
        && strcmp(ev->msgtype, "m.emote"))
        b = !strcmp(ev->msgtype, "m.image") ? "[Bild]" : !strcmp(ev->msgtype, "m.video") ? "[Video]" :
            !strcmp(ev->msgtype, "m.audio") ? "[Audio]" : "[Datei]";
    n = strcspn(b, "\r\n");
    if (n > 100) {                 /* cut at a UTF-8 character boundary */
        n = 100;
        while (n && ((unsigned char)b[n] & 0xc0) == 0x80) n--;
    }
    memcpy(body, b, n);
    body[n] = 0;
    if (n < strlen(b)) strcat(body, "...");
    cJSON_DeleteItemFromObjectCaseSensitive(r, "last");
    l = cJSON_AddObjectToObject(r, "last");
    cJSON_AddStringToObject(l, "s", ev->sender_name ? ev->sender_name : (ev->sender ? ev->sender : ""));
    cJSON_AddStringToObject(l, "b", body);
    cJSON_AddNumberToObject(l, "ts", ev->timestamp);
    if (ev->own) cJSON_AddTrueToObject(l, "own");
    if (ev->event_id) cJSON_AddStringToObject(l, "id", ev->event_id);
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
    if (x->favourite != y->favourite) return x->favourite ? -1 : 1;
    if (x->low_priority != y->low_priority) return x->low_priority ? 1 : -1;
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
        i->inviter_name = i->inviter ? dupn(display(r, i->inviter)) : NULL;
        i->membership = !strcmp(m, "invite") ? AMX_ROOM_INVITED : AMX_ROOM_JOINED;
        du = direct_user(c, r->string);
        if (!du && i->membership == AMX_ROOM_INVITED && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "idirect")))
            du = i->inviter;
        i->dm_user = dupn(du);
        i->avatar_url = dupn(room_avatar(c, r, du));
        {
            const cJSON *l = cJSON_GetObjectItemCaseSensitive(r, "last");
            i->last_sender = dupn(amx__jstr(l, "s"));
            i->last_body = dupn(amx__jstr(l, "b"));
            i->last_own = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(l, "own"));
        }
        i->is_direct = du != NULL;
        i->encrypted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "enc"));
        i->joined_members = get_int(r, "jc");
        i->invited_members = get_int(r, "ic");
        i->unread = get_int(r, "unread");
        i->highlights = get_int(r, "hl");
        i->last_activity = get_num(r, "ts");
        {
            const cJSON *tags = cJSON_GetObjectItemCaseSensitive(r, "tags"), *sp;
            size_t len = 0;
            char *par = NULL;
            i->favourite = cJSON_GetObjectItemCaseSensitive(tags, "m.favourite") != NULL;
            i->low_priority = cJSON_GetObjectItemCaseSensitive(tags, "m.lowpriority") != NULL;
            i->is_space = amx__jstr(r, "ct") && !strcmp(amx__jstr(r, "ct"), "m.space");
            cJSON_ArrayForEach(sp, c->rooms)              /* spaces that list this room */
                if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(sp, "children"), r->string)) {
                    size_t k = strlen(sp->string);
                    char *t = realloc(par, len + k + 2);
                    if (!t) break;
                    par = t;
                    if (len) par[len++] = '\n';
                    memcpy(par + len, sp->string, k + 1);
                    len += k;
                }
            i->parents = par;
            i->notify = amx__room_notify(c, r->string);
            i->replacement = amx__jstr(r, "repl") && *amx__jstr(r, "repl") ? strdup(amx__jstr(r, "repl")) : NULL;
            {
                const cJSON *pins = cJSON_GetObjectItemCaseSensitive(r, "pins"), *x;
                size_t pl = 1;
                char *ps;
                cJSON_ArrayForEach(x, pins) if (cJSON_IsString(x)) pl += strlen(x->valuestring) + 1;
                if (pl > 1 && (ps = malloc(pl)) != NULL) {
                    ps[0] = 0;
                    cJSON_ArrayForEach(x, pins)
                        if (cJSON_IsString(x)) { if (ps[0]) strcat(ps, "\n"); strcat(ps, x->valuestring); }
                    i->pinned = ps;
                }
            }
        }
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
        free((char *)list[i].last_sender); free((char *)list[i].last_body);
        free((char *)list[i].inviter_name);
        free((char *)list[i].parents);
        free((char *)list[i].pinned);
        free((char *)list[i].replacement);
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

/* Rooms we have not seen the create event of (joined before we tracked it):
 * fetch it, a few per sync round; a space also gets its children. */
void amx__rooms_upkeep(AmxClient *c)
{
    cJSON *r;
    int asked = 0;
    cJSON_ArrayForEach(r, c->rooms) {
        const char *m = amx__jstr(r, "m");
        char *esc, *path;
        cJSON *res;
        long st;
        if (asked >= 4) break;
        if (!m || strcmp(m, "join") || cJSON_GetObjectItemCaseSensitive(r, "ct")) continue;
        asked++;
        esc = http_escape(r->string);
        path = amx__malloc(strlen(esc) + 40);
        sprintf(path, "/rooms/%s/state/m.room.create/", esc);
        res = amx__api(c, "GET", path, NULL, 60, &st);
        cJSON_DeleteItemFromObjectCaseSensitive(r, "ct");
        cJSON_AddStringToObject(r, "ct", st == 200 && amx__jstr(res, "type") ? amx__jstr(res, "type") : "");
        cJSON_Delete(res);
        if (!strcmp(amx__jstr(r, "ct"), "m.space")) {           /* its children */
            sprintf(path, "/rooms/%s/state", esc);
            res = amx__api(c, "GET", path, NULL, 60, &st);
            if (st == 200) {
                const cJSON *ev;
                cJSON_ArrayForEach(ev, res) apply_state(c, r, r->string, ev);
            }
            cJSON_Delete(res);
        }
        free(path);
        free(esc);
    }
    if (asked) amx__rooms_save(c);
}

int amx_set_room_tag(AmxClient *c, const char *room_id, const char *tag, int on)
{
    char *eu = http_escape(c->user_id ? c->user_id : ""), *er = http_escape(room_id), *et = http_escape(tag), *path;
    cJSON *b = cJSON_CreateObject(), *res;
    long st;
    path = amx__malloc(strlen(eu) + strlen(er) + strlen(et) + 40);
    sprintf(path, "/user/%s/rooms/%s/tags/%s", eu, er, et);
    free(eu); free(er); free(et);
    if (on) cJSON_AddNumberToObject(b, "order", 0.5);
    res = amx__api(c, on ? "PUT" : "DELETE", path, b, 60, &st);
    cJSON_Delete(b);
    cJSON_Delete(res);
    free(path);
    if (st != 200) return -1;
    {   /* at once in the list, not only after the next sync */
        cJSON *r = cJSON_GetObjectItemCaseSensitive(c->rooms, room_id), *tags;
        if (r) {
            tags = cJSON_GetObjectItemCaseSensitive(r, "tags");
            if (!tags) tags = cJSON_AddObjectToObject(r, "tags");
            cJSON_DeleteItemFromObjectCaseSensitive(tags, tag);
            if (on) cJSON_AddObjectToObject(tags, tag);
            amx__rooms_save(c);
        }
    }
    LOG(c, "tag %s %s on %s", tag, on ? "set" : "removed", room_id);
    return 0;
}

int amx_pin_event(AmxClient *c, const char *room_id, const char *event_id, int on)
{
    char *er = http_escape(room_id), *path = amx__malloc(strlen(er) + 48);
    cJSON *cur, *pins, *x, *res;
    long st;
    sprintf(path, "/rooms/%s/state/m.room.pinned_events/", er);
    free(er);
    cur = amx__api(c, "GET", path, NULL, 60, &st);
    if (st != 200 || !cJSON_IsObject(cur)) { cJSON_Delete(cur); cur = cJSON_CreateObject(); c->err[0] = 0; }
    pins = cJSON_GetObjectItemCaseSensitive(cur, "pinned");
    if (!cJSON_IsArray(pins)) { cJSON_DeleteItemFromObjectCaseSensitive(cur, "pinned"); pins = cJSON_AddArrayToObject(cur, "pinned"); }
    for (x = pins->child; x; ) {
        cJSON *next = x->next;
        if (cJSON_IsString(x) && !strcmp(x->valuestring, event_id)) cJSON_Delete(cJSON_DetachItemViaPointer(pins, x));
        x = next;
    }
    if (on) cJSON_AddItemToArray(pins, cJSON_CreateString(event_id));
    res = amx__api_ok(c, "PUT", path, cur);
    if (res) {                                   /* in the room list at once */
        cJSON *r = cJSON_GetObjectItemCaseSensitive(c->rooms, room_id);
        if (r) {
            cJSON_DeleteItemFromObjectCaseSensitive(r, "pins");
            cJSON_AddItemToObject(r, "pins", cJSON_Duplicate(pins, 1));
            amx__rooms_save(c);
        }
    }
    cJSON_Delete(cur);
    free(path);
    if (!res) return -1;
    cJSON_Delete(res);
    return 0;
}

int amx_fetch_event(AmxClient *c, const char *room_id, const char *event_id)
{
    char *er = http_escape(room_id), *ee = http_escape(event_id), *path = amx__malloc(strlen(er) + strlen(ee) + 24);
    cJSON *ev;
    long st;
    sprintf(path, "/rooms/%s/event/%s", er, ee);
    free(er); free(ee);
    ev = amx__api(c, "GET", path, NULL, 60, &st);
    free(path);
    if (st != 200 || !ev) { cJSON_Delete(ev); return -1; }
    if (amx_backup_available(c)) {
        cJSON *one = cJSON_CreateArray();
        cJSON_AddItemReferenceToArray(one, ev);
        amx__backup_for_events(c, room_id, one, 1);
        cJSON_Delete(one);
    }
    c->lookup_mode = 1;
    amx__handle_room_event(c, room_id, ev, 1);
    c->lookup_mode = 0;
    cJSON_Delete(ev);
    return 0;
}

int amx_upgrade_room(AmxClient *c, const char *room_id, const char *version, char **new_room)
{
    char *e = http_escape(room_id), *p = amx__malloc(strlen(e) + 24);
    cJSON *b = cJSON_CreateObject(), *r;
    *new_room = NULL;
    sprintf(p, "/rooms/%s/upgrade", e);
    free(e);
    if (!version) {                              /* the server's default version */
        cJSON *cap = amx__api_ok(c, "GET", "/capabilities", NULL);
        const char *v = amx__jstr(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cap, "capabilities"),
                                  "m.room_versions"), "default");
        cJSON_AddStringToObject(b, "new_version", v ? v : "10");
        cJSON_Delete(cap);
    } else cJSON_AddStringToObject(b, "new_version", version);
    r = amx__api_ok(c, "POST", p, b);
    cJSON_Delete(b);
    free(p);
    if (!r) return -1;
    if (amx__jstr(r, "replacement_room")) *new_room = amx__strdup(amx__jstr(r, "replacement_room"));
    cJSON_Delete(r);
    return 0;
}
