/* Olm account: create/load, sign, verify, upload device/one-time/fallback keys. */
#include "amx_internal.h"

int amx__save_account(AmxClient *c)
{
    size_t n = olm_pickle_account_length(c->acct);
    char *p = amx__malloc(n + 1);
    int r;
    if (olm_pickle_account(c->acct, PK, p, n) == olm_error()) {
        free(p);
        return amx__fail(c, "pickle account: %s", olm_account_last_error(c->acct));
    }
    p[n] = 0;
    r = amx__write_store(c, "account", p);
    free(p);
    return r;
}

int amx__load_or_create_account(AmxClient *c)
{
    char *p = amx__read_store(c, "account");
    size_t n;
    char *buf;
    cJSON *k;
    c->acct = olm_account(amx__malloc(olm_account_size()));
    if (p) {
        size_t r = olm_unpickle_account(c->acct, PK, p, strlen(p));
        free(p);
        if (r == olm_error()) return amx__fail(c, "unpickle account: %s", olm_account_last_error(c->acct));
    } else {
        void *rnd;
        n = olm_create_account_random_length(c->acct);
        rnd = amx__random(n);
        if (olm_create_account(c->acct, rnd, n) == olm_error()) {
            free(rnd);
            return amx__fail(c, "create account: %s", olm_account_last_error(c->acct));
        }
        free(rnd);
        if (amx__save_account(c)) return -1;
        amx__write_store(c, "keys_uploaded", "0");
    }
    n = olm_account_identity_keys_length(c->acct);
    buf = amx__malloc(n + 1);
    olm_account_identity_keys(c->acct, buf, n);
    buf[n] = 0;
    k = cJSON_Parse(buf);
    free(buf);
    c->curve = amx__strdup(amx__jstr(k, "curve25519"));
    c->ed = amx__strdup(amx__jstr(k, "ed25519"));
    cJSON_Delete(k);
    LOG(c, "device keys: curve25519 %s, ed25519 %s", c->curve, c->ed);
    return 0;
}

/* Adds signatures.<user>.ed25519:<device> to obj (signing its canonical JSON). */
void amx__sign_json(AmxClient *c, cJSON *obj)
{
    char *canon = canonical_json_for_signing(obj);
    size_t n = olm_account_signature_length(c->acct);
    char *sig = amx__malloc(n + 1), keyid[160];
    cJSON *sigs, *mine;
    olm_account_sign(c->acct, canon, strlen(canon), sig, n);
    sig[n] = 0;
    sigs = cJSON_GetObjectItemCaseSensitive(obj, "signatures");
    if (!sigs) sigs = cJSON_AddObjectToObject(obj, "signatures");
    mine = cJSON_GetObjectItemCaseSensitive(sigs, c->user_id);
    if (!mine) mine = cJSON_AddObjectToObject(sigs, c->user_id);
    snprintf(keyid, sizeof keyid, "ed25519:%s", c->device_id);
    cJSON_AddStringToObject(mine, keyid, sig);
    free(sig);
    free(canon);
}

/* Checks signatures.<user>.<keyid> on obj against the ed25519 key ed. */
int amx__verify_signed(AmxClient *c, const cJSON *obj, const char *user, const char *keyid, const char *ed)
{
    const cJSON *sig = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(obj, "signatures"), user), keyid);
    char *canon, *sc;
    size_t r;
    if (!cJSON_IsString(sig) || !ed) return 0;
    canon = canonical_json_for_signing(obj);
    if (!canon) return 0;
    sc = amx__strdup(sig->valuestring);               /* libolm may scribble on it */
    r = olm_ed25519_verify(c->util, ed, strlen(ed), canon, strlen(canon), sc, strlen(sc));
    free(canon);
    free(sc);
    return r != olm_error();
}

static cJSON *device_keys_object(AmxClient *c)
{
    cJSON *dk = cJSON_CreateObject(), *alg, *keys;
    char kid[160];
    cJSON_AddStringToObject(dk, "user_id", c->user_id);
    cJSON_AddStringToObject(dk, "device_id", c->device_id);
    alg = cJSON_AddArrayToObject(dk, "algorithms");
    cJSON_AddItemToArray(alg, cJSON_CreateString(OLM_ALG));
    cJSON_AddItemToArray(alg, cJSON_CreateString(MEGOLM_ALG));
    keys = cJSON_AddObjectToObject(dk, "keys");
    snprintf(kid, sizeof kid, "curve25519:%s", c->device_id);
    cJSON_AddStringToObject(keys, kid, c->curve);
    snprintf(kid, sizeof kid, "ed25519:%s", c->device_id);
    cJSON_AddStringToObject(keys, kid, c->ed);
    amx__sign_json(c, dk);
    return dk;
}

/* libolm's {"curve25519":{"id":"key",...}} -> signed "signed_curve25519:id" entries */
static int add_signed_keys(AmxClient *c, cJSON *dst, const char *olm_json, int fallback)
{
    cJSON *src = cJSON_Parse(olm_json), *k;
    int n = 0;
    cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(src, "curve25519")) {
        char kid[160];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "key", k->valuestring);
        if (fallback) cJSON_AddTrueToObject(e, "fallback");
        amx__sign_json(c, e);
        snprintf(kid, sizeof kid, "signed_curve25519:%s", k->string);
        cJSON_AddItemToObject(dst, kid, e);
        n++;
    }
    cJSON_Delete(src);
    return n;
}

/* Device keys once per device, one-time keys up to half of libolm's maximum,
 * a fallback key if the server has none.  otk_on_server < 0: unknown. */
int amx__upload_keys(AmxClient *c, int otk_on_server, int need_fallback)
{
    cJSON *req = cJSON_CreateObject(), *res;
    char *flag = amx__read_store(c, "keys_uploaded");
    int want = (int)olm_account_max_number_of_one_time_keys(c->acct) / 2;
    int changed = 0, n_otk = 0, n_fb = 0;

    if (!flag || flag[0] != '1') {
        cJSON_AddItemToObject(req, "device_keys", device_keys_object(c));
        changed = 1;
    }
    free(flag);
    if (otk_on_server >= 0 && otk_on_server < want) {
        size_t gen = want - otk_on_server;
        size_t rn = olm_account_generate_one_time_keys_random_length(c->acct, gen), len;
        void *r = amx__random(rn);
        char *buf;
        olm_account_generate_one_time_keys(c->acct, gen, r, rn);
        free(r);
        len = olm_account_one_time_keys_length(c->acct);
        buf = amx__malloc(len + 1);
        olm_account_one_time_keys(c->acct, buf, len);
        buf[len] = 0;
        n_otk = add_signed_keys(c, cJSON_AddObjectToObject(req, "one_time_keys"), buf, 0);
        free(buf);
        changed = 1;
    }
    if (need_fallback) {
        size_t rn = olm_account_generate_fallback_key_random_length(c->acct), len;
        void *r = amx__random(rn);
        char *buf;
        olm_account_generate_fallback_key(c->acct, r, rn);
        free(r);
        len = olm_account_unpublished_fallback_key_length(c->acct);
        buf = amx__malloc(len + 1);
        olm_account_unpublished_fallback_key(c->acct, buf, len);
        buf[len] = 0;
        n_fb = add_signed_keys(c, cJSON_AddObjectToObject(req, "fallback_keys"), buf, 1);
        free(buf);
        changed = 1;
    }
    if (!changed) { cJSON_Delete(req); return 0; }
    res = amx__api_ok(c, "POST", "/keys/upload", req);
    cJSON_Delete(req);
    if (!res) return -1;
    cJSON_Delete(res);
    olm_account_mark_keys_as_published(c->acct);
    if (amx__save_account(c)) return -1;
    amx__write_store(c, "keys_uploaded", "1");
    LOG(c, "uploaded keys: %d one-time, %d fallback", n_otk, n_fb);
    return 0;
}
