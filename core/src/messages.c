/* Sending messages. */
#include "amx_internal.h"

static int send_event(AmxClient *c, const char *room_id, const char *type, const cJSON *content,
                      char **event_id_out)
{
    char *esc, *txn, *path;
    cJSON *body = NULL, *r;
    const char *send_type = type;
    int enc = amx__room_encrypted(c, room_id);

    if (event_id_out) *event_id_out = NULL;
    if (enc < 0) return -1;
    if (enc) {
        const cJSON *rel = cJSON_GetObjectItemCaseSensitive(content, "m.relates_to");
        if (amx__encrypt_room_event(c, room_id, type, content, &body)) return -1;
        if (rel) cJSON_AddItemToObject(body, "m.relates_to", cJSON_Duplicate(rel, 1));
        send_type = "m.room.encrypted";
    }
    esc = http_escape(room_id);
    txn = amx__txn(c);
    path = amx__malloc(strlen(esc) + strlen(send_type) + strlen(txn) + 32);
    sprintf(path, "/rooms/%s/send/%s/%s", esc, send_type, txn);
    r = amx__api_ok(c, "PUT", path, body ? body : content);
    cJSON_Delete(body);
    free(path); free(txn); free(esc);
    if (!r) return -1;
    if (event_id_out && amx__jstr(r, "event_id")) *event_id_out = amx__strdup(amx__jstr(r, "event_id"));
    LOG(c, "sent %s to %s", type, room_id);
    cJSON_Delete(r);
    return 0;
}

int amx_send_text(AmxClient *c, const char *room_id, const char *body, char **event_id_out)
{
    cJSON *ct = cJSON_CreateObject();
    int r;
    cJSON_AddStringToObject(ct, "msgtype", "m.text");
    cJSON_AddStringToObject(ct, "body", body);
    r = send_event(c, room_id, "m.room.message", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}

int amx_mark_read(AmxClient *c, const char *room_id, const char *event_id)
{
    cJSON *b = cJSON_CreateObject(), *r;
    char *er = http_escape(room_id), *path = amx__malloc(strlen(er) + 40);
    sprintf(path, "/rooms/%s/read_markers", er);
    cJSON_AddStringToObject(b, "m.fully_read", event_id);
    cJSON_AddStringToObject(b, "m.read", event_id);
    r = amx__api_ok(c, "POST", path, b);
    cJSON_Delete(b);
    free(path);
    free(er);
    if (!r) return -1;
    cJSON_Delete(r);
    return 0;
}

int amx_send_reaction(AmxClient *c, const char *room_id, const char *event_id, const char *key,
                      char **event_id_out)
{
    cJSON *ct = cJSON_CreateObject(), *rel = cJSON_AddObjectToObject(ct, "m.relates_to");
    int r;
    cJSON_AddStringToObject(rel, "rel_type", "m.annotation");
    cJSON_AddStringToObject(rel, "event_id", event_id);
    cJSON_AddStringToObject(rel, "key", key);
    r = send_event(c, room_id, "m.reaction", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}

int amx_redact(AmxClient *c, const char *room_id, const char *event_id, const char *reason)
{
    cJSON *b = cJSON_CreateObject(), *r;
    char *er = http_escape(room_id), *ee = http_escape(event_id), *txn = amx__txn(c), *path;
    path = amx__malloc(strlen(er) + strlen(ee) + strlen(txn) + 40);
    sprintf(path, "/rooms/%s/redact/%s/%s", er, ee, txn);
    if (reason) cJSON_AddStringToObject(b, "reason", reason);
    r = amx__api_ok(c, "PUT", path, b);
    cJSON_Delete(b);
    free(path); free(er); free(ee); free(txn);
    if (!r) return -1;
    cJSON_Delete(r);
    LOG(c, "redacted %s in %s", event_id, room_id);
    return 0;
}

int amx_send_reply(AmxClient *c, const char *room_id, const char *reply_to, const char *thread_root,
                   const char *body, char **event_id_out)
{
    cJSON *ct = cJSON_CreateObject(), *rel = cJSON_AddObjectToObject(ct, "m.relates_to");
    int r;
    cJSON_AddStringToObject(ct, "msgtype", "m.text");
    cJSON_AddStringToObject(ct, "body", body);
    if (thread_root) {
        cJSON_AddStringToObject(rel, "rel_type", "m.thread");
        cJSON_AddStringToObject(rel, "event_id", thread_root);
        if (!reply_to) cJSON_AddTrueToObject(rel, "is_falling_back");
    }
    if (reply_to || thread_root)
        cJSON_AddStringToObject(cJSON_AddObjectToObject(rel, "m.in_reply_to"), "event_id",
                                reply_to ? reply_to : thread_root);
    r = send_event(c, room_id, "m.room.message", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}

int amx_send_edit(AmxClient *c, const char *room_id, const char *event_id, const char *body,
                  char **event_id_out)
{
    cJSON *ct = cJSON_CreateObject(), *nc, *rel;
    char *star = amx__malloc(strlen(body) + 3);
    int r;
    sprintf(star, "* %s", body);                         /* for clients without edits */
    cJSON_AddStringToObject(ct, "msgtype", "m.text");
    cJSON_AddStringToObject(ct, "body", star);
    free(star);
    nc = cJSON_AddObjectToObject(ct, "m.new_content");
    cJSON_AddStringToObject(nc, "msgtype", "m.text");
    cJSON_AddStringToObject(nc, "body", body);
    rel = cJSON_AddObjectToObject(ct, "m.relates_to");
    cJSON_AddStringToObject(rel, "rel_type", "m.replace");
    cJSON_AddStringToObject(rel, "event_id", event_id);
    r = send_event(c, room_id, "m.room.message", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}
