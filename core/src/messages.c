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
        if (amx__encrypt_room_event(c, room_id, type, content, &body)) return -1;
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
