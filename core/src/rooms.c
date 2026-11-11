/* Rooms: create, join, leave, invite, list; encryption state. */
#include "amx_internal.h"

void amx__note_room_encryption(AmxClient *c, const char *room_id, int enc)
{
    cJSON_DeleteItemFromObjectCaseSensitive(c->room_enc, room_id);
    cJSON_AddBoolToObject(c->room_enc, room_id, enc);
}

int amx__room_encrypted(AmxClient *c, const char *room_id)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(c->room_enc, room_id);
    char *esc, *path;
    long st;
    cJSON *r;
    if (v) return cJSON_IsTrue(v);
    esc = http_escape(room_id);
    path = amx__malloc(strlen(esc) + 64);
    sprintf(path, "/rooms/%s/state/m.room.encryption/", esc);
    r = amx__api(c, "GET", path, NULL, 60, &st);
    free(path);
    free(esc);
    cJSON_Delete(r);
    if (st == 200) { amx__note_room_encryption(c, room_id, 1); return 1; }
    if (st == 404) { c->err[0] = 0; amx__note_room_encryption(c, room_id, 0); return 0; }
    return -1;
}

int amx_create_room(AmxClient *c, const AmxRoomOptions *o, char **room_id_out)
{
    cJSON *req = cJSON_CreateObject(), *r;
    *room_id_out = NULL;
    cJSON_AddStringToObject(req, "preset", o->is_public ? "public_chat" : "private_chat");
    if (o->is_space) {
        cJSON *pl = cJSON_AddObjectToObject(req, "power_level_content_override");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "creation_content"), "type", "m.space");
        cJSON_AddNumberToObject(pl, "events_default", 100);   /* a space has no chat */
        cJSON_AddNumberToObject(pl, "invite", 0);
    }
    if (o->is_public) cJSON_AddStringToObject(req, "visibility", "public");
    if (o->name) cJSON_AddStringToObject(req, "name", o->name);
    if (o->topic) cJSON_AddStringToObject(req, "topic", o->topic);
    if (o->is_direct) cJSON_AddTrueToObject(req, "is_direct");
    if (o->invite) {
        cJSON *a = cJSON_AddArrayToObject(req, "invite");
        const char **u;
        for (u = o->invite; *u; u++) cJSON_AddItemToArray(a, cJSON_CreateString(*u));
    }
    if (o->encrypted) {
        cJSON *is = cJSON_AddArrayToObject(req, "initial_state"), *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "m.room.encryption");
        cJSON_AddStringToObject(e, "state_key", "");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "content"), "algorithm", MEGOLM_ALG);
        cJSON_AddItemToArray(is, e);
    }
    r = amx__api_ok(c, "POST", "/createRoom", req);
    cJSON_Delete(req);
    if (!r || !amx__jstr(r, "room_id")) { cJSON_Delete(r); return -1; }
    *room_id_out = amx__strdup(amx__jstr(r, "room_id"));
    amx__note_room_encryption(c, *room_id_out, o->encrypted ? 1 : 0);
    cJSON_Delete(r);
    if (o->is_direct && o->invite && o->invite[0] && amx__add_direct(c, o->invite[0], *room_id_out))
        LOG(c, "could not mark %s as direct chat: %s", *room_id_out, c->err);
    LOG(c, "created room %s", *room_id_out);
    return 0;
}

int amx_join(AmxClient *c, const char *room_id_or_alias, char **room_id_out)
{
    char *esc = http_escape(room_id_or_alias), *path = amx__malloc(strlen(esc) + 16);
    cJSON *body = cJSON_CreateObject(), *r;
    if (room_id_out) *room_id_out = NULL;
    sprintf(path, "/join/%s", esc);
    r = amx__api_ok(c, "POST", path, body);
    cJSON_Delete(body);
    free(path);
    free(esc);
    if (!r || !amx__jstr(r, "room_id")) { cJSON_Delete(r); return -1; }
    if (room_id_out) *room_id_out = amx__strdup(amx__jstr(r, "room_id"));
    LOG(c, "joined %s", amx__jstr(r, "room_id"));
    {
        /* accepting a direct-chat invite: mark it as direct for us too */
        const char *inviter = amx__room_inviter_if_direct(c, amx__jstr(r, "room_id"));
        if (inviter) {
            char *who = amx__strdup(inviter);
            if (amx__add_direct(c, who, amx__jstr(r, "room_id")))
                LOG(c, "could not mark room as direct chat: %s", c->err);
            free(who);
        }
    }
    cJSON_Delete(r);
    return 0;
}

static int room_post(AmxClient *c, const char *room_id, const char *what, cJSON *body)
{
    char *esc = http_escape(room_id), *path = amx__malloc(strlen(esc) + strlen(what) + 16);
    cJSON *r;
    sprintf(path, "/rooms/%s/%s", esc, what);
    r = amx__api_ok(c, "POST", path, body);
    cJSON_Delete(body);
    free(path);
    free(esc);
    if (!r) return -1;
    cJSON_Delete(r);
    return 0;
}

int amx_leave(AmxClient *c, const char *room_id)
{
    int r = room_post(c, room_id, "leave", cJSON_CreateObject());
    if (!r) {
        amx__room_forget(c, room_id);
        amx__rooms_save(c);
        amx__hcache_forget(c, room_id);
        amx__hcache_save(c);
        cJSON_DeleteItemFromObjectCaseSensitive(c->outbound, room_id);   /* no reuse after re-join */
        amx__save_json(c, "megolm_out.json", c->outbound);
    }
    return r;
}

int amx_invite(AmxClient *c, const char *room_id, const char *user_id)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "user_id", user_id);
    return room_post(c, room_id, "invite", b);
}

char **amx_joined_rooms(AmxClient *c)
{
    cJSON *r = amx__api_ok(c, "GET", "/joined_rooms", NULL), *x;
    char **list;
    int n = 0;
    if (!r) return NULL;
    list = amx__malloc(sizeof *list * (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "joined_rooms")) + 1));
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "joined_rooms"))
        if (cJSON_IsString(x)) list[n++] = amx__strdup(x->valuestring);
    list[n] = NULL;
    cJSON_Delete(r);
    return list;
}

void amx_free_strings(char **list)
{
    char **p;
    if (!list) return;
    for (p = list; *p; p++) free(*p);
    free(list);
}
