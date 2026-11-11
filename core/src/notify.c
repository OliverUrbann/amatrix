/* Notification settings on the server (push rules), shared with all our
 * devices: per room "all messages", "default", "mentions and keywords
 * only", "off"; keywords that count like a mention.  The rules come with
 * the sync (account data m.push_rules) and are kept in pushrules.json. */
#include "amx_internal.h"

#define PUSH_FILE "pushrules.json"

static void push_load(AmxClient *c)
{
    char *t;
    if (c->push) return;
    t = amx__read_store(c, PUSH_FILE);
    c->push = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!c->push) c->push = cJSON_CreateObject();
}

void amx__set_push_rules(AmxClient *c, const cJSON *content)
{
    if (!content) return;
    cJSON_Delete(c->push);
    c->push = cJSON_Duplicate(content, 1);
    amx__save_json(c, PUSH_FILE, c->push);
}

static const cJSON *rule(AmxClient *c, const char *kind, const char *id)
{
    const cJSON *x;
    push_load(c);
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c->push, "global"), kind)) {
        const char *rid = amx__jstr(x, "rule_id");
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(x, "enabled");
        if (rid && !strcmp(rid, id) && !cJSON_IsFalse(en)) return x;
    }
    return NULL;
}

static int notifies(const cJSON *r)
{
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(r, "actions"))
        if (cJSON_IsString(a) && !strcmp(a->valuestring, "notify")) return 1;
    return 0;
}

int amx__room_notify(AmxClient *c, const char *room_id)
{
    const cJSON *r;
    if ((r = rule(c, "override", room_id)) != NULL && !notifies(r)) return AMX_NOTIFY_OFF;
    if ((r = rule(c, "room", room_id)) != NULL) return notifies(r) ? AMX_NOTIFY_ALL : AMX_NOTIFY_MENTIONS;
    return AMX_NOTIFY_DEFAULT;
}

/* our own keywords: content rules that are not the server's defaults */
char *amx_keywords(AmxClient *c)
{
    const cJSON *x;
    size_t n = 1;
    char *out;
    push_load(c);
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c->push, "global"), "content"))
        if (amx__jstr(x, "rule_id") && amx__jstr(x, "rule_id")[0] != '.' && amx__jstr(x, "pattern"))
            n += strlen(amx__jstr(x, "pattern")) + 1;
    out = amx__malloc(n);
    out[0] = 0;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c->push, "global"), "content"))
        if (amx__jstr(x, "rule_id") && amx__jstr(x, "rule_id")[0] != '.' && amx__jstr(x, "pattern")
            && !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(x, "enabled"))) {
            if (out[0]) strcat(out, "\n");
            strcat(out, amx__jstr(x, "pattern"));
        }
    return out;
}

/* does a text contain one of our keywords (case-insensitive)? */
int amx__keyword_hit(AmxClient *c, const char *text)
{
    const cJSON *x;
    if (!text || !*text) return 0;
    push_load(c);
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c->push, "global"), "content")) {
        const char *rid = amx__jstr(x, "rule_id"), *pat = amx__jstr(x, "pattern"), *p;
        size_t k;
        if (!rid || rid[0] == '.' || !pat || !*pat || cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(x, "enabled"))) continue;
        k = strlen(pat);
        for (p = text; *p; p++) {
            size_t i;
            for (i = 0; i < k && p[i]; i++) {
                char a = p[i], b = pat[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
            }
            if (i == k) return 1;
        }
    }
    return 0;
}

static int put_rule(AmxClient *c, const char *kind, const char *id, cJSON *body)
{
    char *e = http_escape(id), *path = amx__malloc(strlen(e) + 48);
    cJSON *r;
    long st;
    sprintf(path, "/pushrules/global/%s/%s", kind, e);
    free(e);
    r = amx__api(c, body ? "PUT" : "DELETE", path, body, 60, &st);
    cJSON_Delete(body);
    cJSON_Delete(r);
    free(path);
    if (!body && st == 404) { c->err[0] = 0; return 0; }   /* nothing to delete */
    return st == 200 ? 0 : -1;
}

static int refresh(AmxClient *c)
{
    long st;
    cJSON *r = amx__api(c, "GET", "/pushrules/", NULL, 60, &st);
    if (st == 200 && r) amx__set_push_rules(c, r);
    cJSON_Delete(r);
    return st == 200 ? 0 : -1;
}

int amx_set_room_notify(AmxClient *c, const char *room_id, int level)
{
    cJSON *b, *a, *cond;
    if (put_rule(c, "override", room_id, NULL) || put_rule(c, "room", room_id, NULL)) return -1;
    if (level == AMX_NOTIFY_ALL) {
        b = cJSON_CreateObject();
        a = cJSON_AddArrayToObject(b, "actions");
        cJSON_AddItemToArray(a, cJSON_CreateString("notify"));
        cond = cJSON_CreateObject();
        cJSON_AddStringToObject(cond, "set_tweak", "sound");
        cJSON_AddStringToObject(cond, "value", "default");
        cJSON_AddItemToArray(a, cond);
        if (put_rule(c, "room", room_id, b)) return -1;
    } else if (level == AMX_NOTIFY_MENTIONS) {
        b = cJSON_CreateObject();
        cJSON_AddArrayToObject(b, "actions");
        if (put_rule(c, "room", room_id, b)) return -1;
    } else if (level == AMX_NOTIFY_OFF) {
        b = cJSON_CreateObject();
        cJSON_AddArrayToObject(b, "actions");
        cond = cJSON_CreateObject();
        cJSON_AddStringToObject(cond, "kind", "event_match");
        cJSON_AddStringToObject(cond, "key", "room_id");
        cJSON_AddStringToObject(cond, "pattern", room_id);
        cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "conditions"), cond);
        if (put_rule(c, "override", room_id, b)) return -1;
    }
    LOG(c, "notifications for %s: level %d", room_id, level);
    return refresh(c);
}

int amx_set_keyword(AmxClient *c, const char *word, int on)
{
    cJSON *b = NULL, *a, *tw;
    if (!word || !*word || strchr(word, '/')) return amx__fail(c, "not a keyword");
    if (on) {
        b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "pattern", word);
        a = cJSON_AddArrayToObject(b, "actions");
        cJSON_AddItemToArray(a, cJSON_CreateString("notify"));
        tw = cJSON_CreateObject();
        cJSON_AddStringToObject(tw, "set_tweak", "sound");
        cJSON_AddStringToObject(tw, "value", "default");
        cJSON_AddItemToArray(a, tw);
        tw = cJSON_CreateObject();
        cJSON_AddStringToObject(tw, "set_tweak", "highlight");
        cJSON_AddItemToArray(a, tw);
    }
    if (put_rule(c, "content", word, b)) return -1;
    return refresh(c);
}
