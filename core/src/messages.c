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
    cJSON_AddStringToObject(b, c->private_receipts ? "m.read.private" : "m.read", event_id);
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

int amx_send_file(AmxClient *c, const char *room_id, const char *path, const char *name,
                  const char *thread_root, char **event_id_out)
{
    int enc = amx__room_encrypted(c, room_id), r;
    cJSON *ct;
    if (event_id_out) *event_id_out = NULL;
    if (enc < 0) return -1;
    if (!(ct = amx__file_content(c, path, name, enc))) return -1;
    if (thread_root) {
        cJSON *rel = cJSON_AddObjectToObject(ct, "m.relates_to");
        cJSON_AddStringToObject(rel, "rel_type", "m.thread");
        cJSON_AddStringToObject(rel, "event_id", thread_root);
        cJSON_AddTrueToObject(rel, "is_falling_back");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(rel, "m.in_reply_to"), "event_id", thread_root);
    }
    r = send_event(c, room_id, "m.room.message", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}

void amx_set_private_receipts(AmxClient *c, int on) { c->private_receipts = on != 0; }

int amx_send_typing(AmxClient *c, const char *room_id, long ms)
{
    cJSON *b = cJSON_CreateObject(), *r;
    char *er = http_escape(room_id), *eu = http_escape(c->user_id ? c->user_id : ""), *path;
    path = amx__malloc(strlen(er) + strlen(eu) + 32);
    sprintf(path, "/rooms/%s/typing/%s", er, eu);
    if (ms > 0) {
        cJSON_AddTrueToObject(b, "typing");
        cJSON_AddNumberToObject(b, "timeout", (double)ms);
    } else cJSON_AddFalseToObject(b, "typing");
    r = amx__api_ok(c, "PUT", path, b);
    cJSON_Delete(b);
    free(path); free(er); free(eu);
    if (!r) return -1;
    cJSON_Delete(r);
    return 0;
}

int amx_send_poll(AmxClient *c, const char *room_id, const char *question, const char *const *answers, int n,
                  int max, int disclosed, char **event_id_out)
{
    cJSON *ct = cJSON_CreateObject(), *p, *arr;
    char *fb, id[16];
    size_t len = strlen(question ? question : "") + 4;
    int i, r;
    if (event_id_out) *event_id_out = NULL;
    if (!question || !*question || n < 2) { cJSON_Delete(ct); return amx__fail(c, "a poll needs a question and two answers"); }
    for (i = 0; i < n; i++) len += strlen(answers[i]) + 8;
    fb = amx__malloc(len);                       /* the text for clients without polls */
    strcpy(fb, question);
    for (i = 0; i < n; i++) sprintf(fb + strlen(fb), "\n%d. %s", i + 1, answers[i]);
    p = cJSON_AddObjectToObject(ct, "org.matrix.msc3381.poll.start");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "question"), "org.matrix.msc1767.text", question);
    cJSON_AddStringToObject(p, "kind", disclosed ? "org.matrix.msc3381.poll.disclosed" : "org.matrix.msc3381.poll.undisclosed");
    cJSON_AddNumberToObject(p, "max_selections", max > 0 && max <= n ? max : 1);
    arr = cJSON_AddArrayToObject(p, "answers");
    for (i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        sprintf(id, "a%d", i + 1);
        cJSON_AddStringToObject(a, "id", id);
        cJSON_AddStringToObject(a, "org.matrix.msc1767.text", answers[i]);
        cJSON_AddItemToArray(arr, a);
    }
    cJSON_AddStringToObject(ct, "org.matrix.msc1767.text", fb);
    cJSON_AddStringToObject(ct, "body", fb);
    free(fb);
    r = send_event(c, room_id, "org.matrix.msc3381.poll.start", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}

static cJSON *reference(const char *event_id)
{
    cJSON *ct = cJSON_CreateObject(), *rel = cJSON_AddObjectToObject(ct, "m.relates_to");
    cJSON_AddStringToObject(rel, "rel_type", "m.reference");
    cJSON_AddStringToObject(rel, "event_id", event_id);
    return ct;
}

int amx_poll_vote(AmxClient *c, const char *room_id, const char *poll_id, const char *const *answer_ids, int n)
{
    cJSON *ct = reference(poll_id), *arr;
    int i, r;
    arr = cJSON_AddArrayToObject(cJSON_AddObjectToObject(ct, "org.matrix.msc3381.poll.response"), "answers");
    for (i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(answer_ids[i]));
    r = send_event(c, room_id, "org.matrix.msc3381.poll.response", ct, NULL);
    cJSON_Delete(ct);
    return r;
}

int amx_poll_end(AmxClient *c, const char *room_id, const char *poll_id)
{
    cJSON *ct = reference(poll_id);
    int r;
    cJSON_AddObjectToObject(ct, "org.matrix.msc3381.poll.end");
    cJSON_AddStringToObject(ct, "org.matrix.msc1767.text", "Die Umfrage ist beendet.");
    cJSON_AddStringToObject(ct, "body", "Die Umfrage ist beendet.");
    r = send_event(c, room_id, "org.matrix.msc3381.poll.end", ct, NULL);
    cJSON_Delete(ct);
    return r;
}

static void add_text(cJSON *o, const AmxOutMsg *m, const char *prefix)
{
    char *b = amx__malloc(strlen(m->body ? m->body : "") + 4);
    sprintf(b, "%s%s", prefix, m->body ? m->body : "");
    cJSON_AddStringToObject(o, "msgtype", m->msgtype ? m->msgtype : "m.text");
    cJSON_AddStringToObject(o, "body", b);
    free(b);
    if (m->html) {
        b = amx__malloc(strlen(m->html) + 4);
        sprintf(b, "%s%s", prefix, m->html);
        cJSON_AddStringToObject(o, "format", "org.matrix.custom.html");
        cJSON_AddStringToObject(o, "formatted_body", b);
        free(b);
    }
}

static void add_mentions(cJSON *o, const char *list)
{
    cJSON *ids = cJSON_AddArrayToObject(cJSON_AddObjectToObject(o, "m.mentions"), "user_ids");
    const char *p = list;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t k = nl ? (size_t)(nl - p) : strlen(p);
        if (k) {
            char *u = amx__strndup(p, k);
            cJSON_AddItemToArray(ids, cJSON_CreateString(u));
            free(u);
        }
        p = nl ? nl + 1 : NULL;
    }
}

int amx_send_message(AmxClient *c, const char *room_id, const AmxOutMsg *m, char **event_id_out)
{
    cJSON *ct = cJSON_CreateObject(), *rel;
    int r;
    if (m->replaces) {                                   /* an edit */
        cJSON *nc;
        add_text(ct, m, "* ");
        nc = cJSON_AddObjectToObject(ct, "m.new_content");
        add_text(nc, m, "");
        add_mentions(nc, m->mentions);
        rel = cJSON_AddObjectToObject(ct, "m.relates_to");
        cJSON_AddStringToObject(rel, "rel_type", "m.replace");
        cJSON_AddStringToObject(rel, "event_id", m->replaces);
    } else {
        add_text(ct, m, "");
        if (m->reply_to || m->thread_root) {
            rel = cJSON_AddObjectToObject(ct, "m.relates_to");
            if (m->thread_root) {
                cJSON_AddStringToObject(rel, "rel_type", "m.thread");
                cJSON_AddStringToObject(rel, "event_id", m->thread_root);
                if (!m->reply_to) cJSON_AddTrueToObject(rel, "is_falling_back");
            }
            cJSON_AddStringToObject(cJSON_AddObjectToObject(rel, "m.in_reply_to"), "event_id",
                                    m->reply_to ? m->reply_to : m->thread_root);
        }
    }
    add_mentions(ct, m->mentions);
    r = send_event(c, room_id, "m.room.message", ct, event_id_out);
    cJSON_Delete(ct);
    return r;
}

int amx_set_room_nick(AmxClient *c, const char *room_id, const char *name)
{
    char *er = http_escape(room_id), *eu = http_escape(c->user_id ? c->user_id : ""), *path;
    cJSON *cur, *r;
    long st;
    path = amx__malloc(strlen(er) + strlen(eu) + 48);
    sprintf(path, "/rooms/%s/state/m.room.member/%s", er, eu);
    free(er); free(eu);
    cur = amx__api(c, "GET", path, NULL, 60, &st);
    if (st != 200 || !cur) { cJSON_Delete(cur); free(path); return -1; }
    cJSON_DeleteItemFromObjectCaseSensitive(cur, "displayname");
    if (name && *name) cJSON_AddStringToObject(cur, "displayname", name);
    r = amx__api_ok(c, "PUT", path, cur);
    cJSON_Delete(cur);
    free(path);
    if (!r) return -1;
    cJSON_Delete(r);
    return 0;
}

int amx_forward(AmxClient *c, const char *to_room, const char *from_room, const char *event_id, char **event_id_out)
{
    char *er = http_escape(from_room), *ee = http_escape(event_id), *path = amx__malloc(strlen(er) + strlen(ee) + 24);
    char *type = NULL;
    cJSON *ev, *ct;
    long st;
    int r;
    if (event_id_out) *event_id_out = NULL;
    sprintf(path, "/rooms/%s/event/%s", er, ee);
    free(er); free(ee);
    ev = amx__api(c, "GET", path, NULL, 60, &st);
    free(path);
    if (st != 200 || !ev) { cJSON_Delete(ev); return -1; }
    ct = amx__event_content(c, from_room, ev, &type);
    cJSON_Delete(ev);
    if (!ct) { free(type); return -1; }
    if (!cJSON_GetArraySize(ct)) { cJSON_Delete(ct); free(type); return amx__fail(c, "this message was deleted"); }
    cJSON_DeleteItemFromObjectCaseSensitive(ct, "m.relates_to");      /* no reply / thread / edit there */
    cJSON_DeleteItemFromObjectCaseSensitive(ct, "m.new_content");
    r = send_event(c, to_room, type, ct, event_id_out);
    cJSON_Delete(ct);
    free(type);
    return r;
}
