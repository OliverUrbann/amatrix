/* recovery.c - the security key (secret storage, SSSS) and the server-side
 * key backup.
 *
 * amx_recovery_unlock(): the recovery key ("EsTc ...") or the passphrase
 * opens secret storage (account data, m.secret_storage.v1.aes-hmac-sha2).
 * From it we take
 *   - m.cross_signing.self_signing: signs our own device -> verified in Element
 *     (the key is used once and not stored),
 *   - m.megolm_backup.v1: the private key of the key backup, kept in the store
 *     (backup.json) so that amx_backup_room() can fetch room keys later.
 *
 * amx_backup_room(): imports the backed-up Megolm sessions of one room
 * (m.megolm_backup.v1.curve25519-aes-sha2, decrypted with libolm's PkDecryption).
 * amx_load_history() calls it once per room and run, so opening a room shows
 * its old messages.
 *
 * Crypto: HKDF/HMAC-SHA256, AES-256-CTR and PBKDF2-SHA512 from OpenSSL
 * (AmiSSL on the Amiga - only in the process that opened it, i.e. the
 * network process), Curve25519/Ed25519 from libolm.
 */
#include "amx_internal.h"
#include <time.h>

#ifdef AMX_AMISSL
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#endif
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <olm/pk.h>

#define SSSS_ALG "m.secret_storage.v1.aes-hmac-sha2"
#define BACKUP_ALG "m.megolm_backup.v1.curve25519-aes-sha2"

/* ------------------------------------------------------------- encodings */

static int b64val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

/* base64 (padded or not) -> bytes; length or -1 */
static long b64dec(const char *s, unsigned char *out, size_t max)
{
    unsigned long acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *s && *s != '='; s++) {
        int v = b64val((unsigned char)*s);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    return (long)n;
}

/* Bitcoin base58 -> bytes (big number); length or -1 */
static long b58dec(const char *s, unsigned char *out, size_t max)
{
    static const char alpha[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    unsigned char buf[64];
    size_t len = 0, i;
    int zeros = 0;
    memset(buf, 0, sizeof buf);
    for (; *s == '1'; s++) zeros++;
    for (; *s; s++) {
        const char *p = strchr(alpha, *s);
        unsigned int carry;
        if (!p || !*s) return -1;
        carry = (unsigned int)(p - alpha);
        for (i = 0; i < len || carry; i++) {     /* buf (little endian) * 58 + carry */
            if (i >= sizeof buf) return -1;
            carry += 58u * buf[i];
            buf[i] = (unsigned char)carry;
            carry >>= 8;
            if (i >= len) len = i + 1;
        }
    }
    if (len + (size_t)zeros > max) return -1;
    for (i = 0; i < (size_t)zeros; i++) out[i] = 0;
    for (i = 0; i < len; i++) out[zeros + i] = buf[len - 1 - i];
    return (long)(zeros + len);
}

/* ------------------------------------------------------------- primitives */

static void hmac256(const unsigned char *key, size_t kl, const unsigned char *d, size_t dl, unsigned char out[32])
{
    unsigned int ol = 32;
    HMAC(EVP_sha256(), key, (int)kl, d, dl, out, &ol);
}

/* HKDF-SHA256 with a zero salt of 32 bytes, 64 bytes out (AES key, HMAC key) */
static void hkdf64(const unsigned char key[32], const char *info, unsigned char out[64])
{
    unsigned char salt[32], prk[32], t[32 + 200 + 1];
    size_t il = strlen(info);
    memset(salt, 0, sizeof salt);
    hmac256(salt, 32, key, 32, prk);
    if (il > 200) il = 200;
    memcpy(t, info, il);
    t[il] = 1;
    hmac256(prk, 32, t, il + 1, out);                 /* T(1) */
    memcpy(t, out, 32);
    memcpy(t + 32, info, il);
    t[32 + il] = 2;
    hmac256(prk, 32, t, 32 + il + 1, out + 32);       /* T(2) */
    OPENSSL_cleanse(prk, sizeof prk);
}

static int aes_ctr(const unsigned char key[32], const unsigned char iv[16], const unsigned char *in,
                   size_t n, unsigned char *out)
{
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    int l1 = 0, l2 = 0, ok;
    if (!x) return -1;
    ok = EVP_EncryptInit_ex(x, EVP_aes_256_ctr(), NULL, key, iv) == 1 &&
         EVP_EncryptUpdate(x, out, &l1, in, (int)n) == 1 &&
         EVP_EncryptFinal_ex(x, out + l1, &l2) == 1;
    EVP_CIPHER_CTX_free(x);
    return ok ? 0 : -1;
}

/* constant-time compare of a computed MAC with a base64 one */
static int mac_ok(const unsigned char mac[32], const char *b64)
{
    unsigned char want[40];
    if (!b64 || b64dec(b64, want, sizeof want) != 32) return 0;
    return CRYPTO_memcmp(mac, want, 32) == 0;
}

/* ------------------------------------------------------- secret storage */

static cJSON *account_data(AmxClient *c, const char *type)
{
    char *u = http_escape(c->user_id), *t = http_escape(type), *path;
    cJSON *r;
    path = amx__malloc(strlen(u) + strlen(t) + 40);
    sprintf(path, "/user/%s/account_data/%s", u, t);
    r = amx__api_ok(c, "GET", path, NULL);
    free(path); free(u); free(t);
    return r;
}

/* the 32-byte storage key from a recovery key; 0 or -1 */
static int from_recovery_key(const char *in, unsigned char key[32])
{
    char clean[128];
    unsigned char raw[64], parity = 0;
    size_t n = 0;
    long len;
    int i;
    for (; *in && n < sizeof clean - 1; in++)
        if (*in != ' ' && *in != '-' && *in != '\t') clean[n++] = *in;
    clean[n] = 0;
    len = b58dec(clean, raw, sizeof raw);
    if (len != 35 || raw[0] != 0x8b || raw[1] != 0x01) return -1;
    for (i = 0; i < 35; i++) parity ^= raw[i];
    if (parity) return -1;
    memcpy(key, raw + 2, 32);
    OPENSSL_cleanse(raw, sizeof raw);
    return 0;
}

/* does key open the storage key described by info (iv + mac of 32 zero bytes)? */
static int key_matches(const unsigned char key[32], const cJSON *info)
{
    unsigned char k[64], iv[20], zero[32], ct[32], mac[32];
    if (b64dec(amx__jstr(info, "iv") ? amx__jstr(info, "iv") : "", iv, sizeof iv) != 16) return 0;
    memset(zero, 0, sizeof zero);
    hkdf64(key, "", k);
    if (aes_ctr(k, iv, zero, 32, ct)) return 0;
    hmac256(k + 32, 32, ct, 32, mac);
    OPENSSL_cleanse(k, sizeof k);
    return mac_ok(mac, amx__jstr(info, "mac"));
}

/* one secret from account data, decrypted; caller frees (cleanse!) or NULL */
static char *get_secret(AmxClient *c, const char *name, const char *key_id, const unsigned char key[32])
{
    cJSON *ad = account_data(c, name);
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(ad, "encrypted"), key_id);
    unsigned char k[64], iv[20], mac[32], *ct = NULL, *pt = NULL;
    long n = 0;
    char *out = NULL;
    if (!e) { cJSON_Delete(ad); return NULL; }
    if (b64dec(amx__jstr(e, "iv") ? amx__jstr(e, "iv") : "", iv, sizeof iv) != 16) goto done;
    {
        const char *s = amx__jstr(e, "ciphertext");
        size_t max = s ? strlen(s) : 0;
        ct = amx__malloc(max + 1);
        pt = amx__malloc(max + 1);
        if (!s || (n = b64dec(s, ct, max)) < 0) goto done;
    }
    hkdf64(key, name, k);
    hmac256(k + 32, 32, ct, (size_t)n, mac);
    if (!mac_ok(mac, amx__jstr(e, "mac"))) { LOG(c, "secret %s: MAC does not match", name); goto done; }
    if (aes_ctr(k, iv, ct, (size_t)n, pt)) goto done;
    pt[n] = 0;
    out = amx__strdup((char *)pt);
done:
    OPENSSL_cleanse(k, sizeof k);
    if (pt) { OPENSSL_cleanse(pt, n > 0 ? (size_t)n : 0); free(pt); }
    free(ct);
    cJSON_Delete(ad);
    return out;
}

/* signs our own device with the self-signing key (seed, base64); 0 or -1 */
static int sign_own_device(AmxClient *c, const char *seed_b64)
{
    unsigned char seed[40];
    char pub[64], *canon, *sig, *keyid;
    OlmPkSigning *s;
    cJSON *req, *res, *dev, *up, *sigs;
    const cJSON *ssk;
    const char *want = NULL;
    size_t sl;
    long st;
    int r = -1;

    if (b64dec(seed_b64, seed, sizeof seed) != 32) return amx__fail(c, "self-signing key: bad format");
    /* our current device keys and the published self-signing key */
    req = cJSON_CreateObject();
    cJSON_AddArrayToObject(cJSON_AddObjectToObject(req, "device_keys"), c->user_id);
    res = amx__api(c, "POST", "/keys/query", req, 60, &st);
    cJSON_Delete(req);
    if (st != 200 || !res) { cJSON_Delete(res); OPENSSL_cleanse(seed, sizeof seed); return -1; }
    ssk = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(res, "self_signing_keys"), c->user_id), "keys");
    if (ssk && ssk->child && cJSON_IsString(ssk->child)) want = ssk->child->valuestring;
    dev = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(res, "device_keys"), c->user_id), c->device_id);

    s = olm_pk_signing(amx__malloc(olm_pk_signing_size()));
    if (olm_pk_signing_key_from_seed(s, pub, olm_pk_signing_public_key_length(), seed, 32) == olm_error()) {
        amx__fail(c, "self-signing key: %s", olm_pk_signing_last_error(s));
        goto out;
    }
    pub[olm_pk_signing_public_key_length()] = 0;
    if (!want || strcmp(want, pub)) { amx__fail(c, "self-signing key does not match the published one"); goto out; }
    if (!dev) { amx__fail(c, "own device keys not found on the server"); goto out; }

    up = cJSON_Duplicate(dev, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(up, "unsigned");
    canon = canonical_json_for_signing(up);
    sl = olm_pk_signature_length();
    sig = amx__malloc(sl + 1);
    if (olm_pk_sign(s, (uint8_t *)canon, strlen(canon), (uint8_t *)sig, sl) == olm_error()) {
        free(canon); free(sig); cJSON_Delete(up);
        amx__fail(c, "signing failed: %s", olm_pk_signing_last_error(s));
        goto out;
    }
    sig[sl] = 0;
    free(canon);
    cJSON_DeleteItemFromObjectCaseSensitive(up, "signatures");
    sigs = cJSON_AddObjectToObject(cJSON_AddObjectToObject(up, "signatures"), c->user_id);
    keyid = amx__malloc(strlen(pub) + 10);
    sprintf(keyid, "ed25519:%s", pub);
    cJSON_AddStringToObject(sigs, keyid, sig);
    free(keyid);
    free(sig);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(cJSON_AddObjectToObject(req, c->user_id), c->device_id, up);
    cJSON_Delete(res);
    res = amx__api(c, "POST", "/keys/signatures/upload", req, 60, &st);
    cJSON_Delete(req);
    if (st == 200 && res && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(res, "failures")) == 0) {
        LOG(c, "own device signed with the self-signing key");
        r = 0;
    } else if (st == 200) amx__fail(c, "signature rejected by the server");
out:
    OPENSSL_cleanse(seed, sizeof seed);
    olm_clear_pk_signing(s);
    free(s);
    cJSON_Delete(res);
    return r;
}

/* the key backup: version on the server and its public key */
/* 1 if the server has a signature of our self-signing key on this device's
 * keys (i.e. we are verified, also after a restart), 0 if not, -1 on error. */
int amx_device_verified(AmxClient *c)
{
    cJSON *req, *res;
    const cJSON *ssk, *dev;
    char kid[96];
    long st;
    int r = 0;

    if (!c->user_id || !c->device_id) return -1;
    req = cJSON_CreateObject();
    cJSON_AddArrayToObject(cJSON_AddObjectToObject(req, "device_keys"), c->user_id);
    res = amx__api(c, "POST", "/keys/query", req, 60, &st);
    cJSON_Delete(req);
    if (st != 200 || !res) { cJSON_Delete(res); return -1; }
    ssk = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(res, "self_signing_keys"), c->user_id), "keys");
    dev = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(res, "device_keys"), c->user_id), c->device_id);
    if (ssk && ssk->child && cJSON_IsString(ssk->child) && dev
        && strlen(ssk->child->valuestring) < sizeof kid - 10) {
        cJSON *d = cJSON_Duplicate(dev, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(d, "unsigned");
        sprintf(kid, "ed25519:%s", ssk->child->valuestring);
        r = amx__verify_signed(c, d, c->user_id, kid, ssk->child->valuestring);
        cJSON_Delete(d);
    }
    cJSON_Delete(res);
    return r;
}

static cJSON *backup_version(AmxClient *c)
{
    long st;
    cJSON *r = amx__api(c, "GET", "/room_keys/version", NULL, 60, &st);
    if (st != 200 || !r) { cJSON_Delete(r); return NULL; }
    if (!amx__jstr(r, "algorithm") || strcmp(amx__jstr(r, "algorithm"), BACKUP_ALG)) {
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

/* public key of a backup private key (base64) or NULL */
static char *backup_public(const unsigned char priv[32])
{
    OlmPkDecryption *d = olm_pk_decryption(amx__malloc(olm_pk_decryption_size()));
    char *pub = amx__malloc(olm_pk_key_length() + 1);
    if (olm_pk_key_from_private(d, pub, olm_pk_key_length(), priv, 32) == olm_error()) {
        free(pub);
        pub = NULL;
    } else pub[olm_pk_key_length()] = 0;
    olm_clear_pk_decryption(d);
    free(d);
    return pub;
}

int amx_recovery_unlock(AmxClient *c, const char *input, int *verified, int *backup)
{
    cJSON *def, *info;
    const char *key_id;
    char *secret, *keyname;
    unsigned char key[32];
    int have = 0;

    if (verified) *verified = 0;
    if (backup) *backup = 0;
    if (!c->token) return amx__fail(c, "not logged in");
    def = account_data(c, "m.secret_storage.default_key");
    key_id = amx__jstr(def, "key");
    if (!key_id) { cJSON_Delete(def); return amx__fail(c, "no secret storage on the server (set up recovery in Element first)"); }
    keyname = amx__malloc(strlen(key_id) + 32);
    sprintf(keyname, "m.secret_storage.key.%s", key_id);
    info = account_data(c, keyname);
    free(keyname);
    if (!info || !amx__jstr(info, "algorithm") || strcmp(amx__jstr(info, "algorithm"), SSSS_ALG)) {
        cJSON_Delete(def); cJSON_Delete(info);
        return amx__fail(c, "secret storage: unsupported algorithm");
    }

    /* recovery key, else passphrase */
    if (from_recovery_key(input, key) == 0 && key_matches(key, info)) have = 1;
    if (!have) {
        const cJSON *pp = cJSON_GetObjectItemCaseSensitive(info, "passphrase");
        const char *salt = amx__jstr(pp, "salt"), *alg = amx__jstr(pp, "algorithm");
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(pp, "iterations");
        if (pp && salt && alg && !strcmp(alg, "m.pbkdf2") && cJSON_IsNumber(it)) {
            LOG(c, "deriving the key from the passphrase (%d rounds) ...", it->valueint);
            if (PKCS5_PBKDF2_HMAC(input, (int)strlen(input), (const unsigned char *)salt, (int)strlen(salt),
                                  it->valueint, EVP_sha512(), 32, key) == 1 && key_matches(key, info))
                have = 1;
        }
    }
    if (!have) {
        OPENSSL_cleanse(key, sizeof key);
        cJSON_Delete(def); cJSON_Delete(info);
        return amx__fail(c, "wrong security key or passphrase");
    }
    LOG(c, "secret storage unlocked (key %s)", key_id);

    /* the key backup */
    if ((secret = get_secret(c, "m.megolm_backup.v1", key_id, key)) != NULL) {
        unsigned char priv[40];
        cJSON *ver = backup_version(c);
        char *pub = NULL;
        if (b64dec(secret, priv, sizeof priv) == 32 && ver && (pub = backup_public(priv)) &&
            amx__jstr(cJSON_GetObjectItemCaseSensitive(ver, "auth_data"), "public_key") &&
            !strcmp(pub, amx__jstr(cJSON_GetObjectItemCaseSensitive(ver, "auth_data"), "public_key"))) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "version", amx__jstr(ver, "version") ? amx__jstr(ver, "version") : "");
            cJSON_AddStringToObject(s, "key", secret);
            amx__save_json(c, "backup.json", s);
            cJSON_Delete(s);
            cJSON_Delete(c->backup);
            c->backup = NULL;
            if (backup) *backup = 1;
            LOG(c, "key backup %s available", amx__jstr(ver, "version"));
        } else LOG(c, "key backup: no matching backup version on the server");
        OPENSSL_cleanse(priv, sizeof priv);
        OPENSSL_cleanse(secret, strlen(secret));
        free(secret);
        free(pub);
        cJSON_Delete(ver);
    }

    /* verify ourselves */
    if ((secret = get_secret(c, "m.cross_signing.self_signing", key_id, key)) != NULL) {
        if (sign_own_device(c, secret) == 0 && verified) *verified = 1;
        OPENSSL_cleanse(secret, strlen(secret));
        free(secret);
    } else LOG(c, "no self-signing key in secret storage (cross-signing not set up?)");

    OPENSSL_cleanse(key, sizeof key);
    cJSON_Delete(def);
    cJSON_Delete(info);
    return 0;
}

/* ---------------------------------------------------------- key backup */

static int load_backup(AmxClient *c)
{
    char *t;
    if (c->backup) return 0;
    if (!(t = amx__read_store(c, "backup.json"))) return -1;
    c->backup = cJSON_Parse(t);
    OPENSSL_cleanse(t, strlen(t));
    free(t);
    return c->backup ? 0 : -1;
}

int amx_backup_available(AmxClient *c) { return load_backup(c) == 0; }

/* one backed-up session: decrypt and import; 1 imported, 0 known/skipped, -1 bad */
static int import_one(AmxClient *c, OlmPkDecryption *d, const char *room, const char *sid, const cJSON *data)
{
    const char *eph = amx__jstr(data, "ephemeral"), *mac = amx__jstr(data, "mac"),
               *ct = amx__jstr(data, "ciphertext");
    char *ctb, *plain, *k, idbuf[128];
    size_t max, n;
    cJSON *p;
    OlmInboundGroupSession *s;
    const char *alg, *key, *sender;

    if (!eph || !mac || !ct) return -1;
    ctb = amx__strdup(ct);
    max = olm_pk_max_plaintext_length(d, strlen(ctb));
    plain = amx__malloc(max + 1);
    n = olm_pk_decrypt(d, eph, strlen(eph), mac, strlen(mac), ctb, strlen(ctb), plain, max);
    free(ctb);
    if (n == olm_error()) {
        LOG(c, "backup: session %.12s...: %s", sid, olm_pk_decryption_last_error(d));
        free(plain);
        return -1;
    }
    plain[n] = 0;
    p = cJSON_Parse(plain);
    OPENSSL_cleanse(plain, n);
    free(plain);
    alg = amx__jstr(p, "algorithm");
    key = amx__jstr(p, "session_key");
    sender = amx__jstr(p, "sender_key");
    if (!p || !alg || strcmp(alg, MEGOLM_ALG) || !key || !sender) { cJSON_Delete(p); return -1; }
    s = amx__new_group_session();
    k = amx__strdup(key);
    if (olm_import_inbound_group_session(s, (uint8_t *)k, strlen(k)) == olm_error()) {
        LOG(c, "backup: import %.12s...: %s", sid, olm_inbound_group_session_last_error(s));
        OPENSSL_cleanse(k, strlen(k));
        free(k); free(s); cJSON_Delete(p);
        return -1;
    }
    OPENSSL_cleanse(k, strlen(k));
    free(k);
    n = olm_inbound_group_session_id(s, (uint8_t *)idbuf, sizeof idbuf - 1);
    idbuf[n == olm_error() ? 0 : n] = 0;
    if (strcmp(idbuf, sid)) { LOG(c, "backup: session id mismatch"); free(s); cJSON_Delete(p); return -1; }
    n = amx__put_group_session(c, room, sid, sender,
                               amx__jstr(cJSON_GetObjectItemCaseSensitive(p, "sender_claimed_keys"), "ed25519"), s);
    cJSON_Delete(p);
    return (int)n;
}

/* a PkDecryption for the backup key, or NULL (error set) */
static OlmPkDecryption *backup_decryption(AmxClient *c)
{
    unsigned char priv[40];
    OlmPkDecryption *d;
    char *pub;
    if (load_backup(c)) return NULL;
    if (!amx__jstr(c->backup, "version") ||
        b64dec(amx__jstr(c->backup, "key") ? amx__jstr(c->backup, "key") : "", priv, sizeof priv) != 32) {
        amx__fail(c, "backup.json damaged");
        return NULL;
    }
    d = olm_pk_decryption(amx__malloc(olm_pk_decryption_size()));
    pub = amx__malloc(olm_pk_key_length() + 1);
    if (olm_pk_key_from_private(d, pub, olm_pk_key_length(), priv, 32) == olm_error()) {
        olm_clear_pk_decryption(d); free(d); d = NULL;
        amx__fail(c, "backup key unusable");
    }
    OPENSSL_cleanse(priv, sizeof priv);
    free(pub);
    return d;
}

/* The sessions the encrypted events in `events` need and we do not have:
 * fetched one by one from the backup (a room can have thousands of backed-up
 * keys; a page of history needs a few).  Each session is asked for once per
 * run.  Number imported. */
/* The backup may get a key later than we see the message (the other device
 * uploads it after sending): try a session again after 20 s, 1, 5 and 30 min. */
static int backup_due(AmxClient *c, const char *key)
{
    static const long delay[] = { 20, 60, 300, 1800 };
    cJSON *e;
    long now = (long)time(NULL);
    int n;
    if (!c->bk_done) c->bk_done = cJSON_CreateObject();
    e = cJSON_GetObjectItemCaseSensitive(c->bk_done, key);
    if (!e) {
        e = cJSON_AddObjectToObject(c->bk_done, key);
        cJSON_AddNumberToObject(e, "n", 0);
        cJSON_AddNumberToObject(e, "t", 0);
    }
    n = cJSON_GetObjectItemCaseSensitive(e, "n")->valueint;
    if (n > 4 || now < (long)cJSON_GetObjectItemCaseSensitive(e, "t")->valuedouble) return 0;
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "n"), n + 1);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "t"), (double)(now + (n < 4 ? delay[n] : 0)));
    return 1;
}

int amx__backup_for_events(AmxClient *c, const char *room_id, const cJSON *events, int only_missing)
{
    OlmPkDecryption *d = NULL;
    const cJSON *e;
    char *resc = NULL, *vesc = NULL;
    int n = 0, asked = 0;

    cJSON_ArrayForEach(e, events) {
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(e, "content");
        const char *sid = amx__jstr(ct, "session_id"), *typ = amx__jstr(e, "type");
        char *key, *sesc, *path;
        cJSON *r;
        long st;
        if (!typ || strcmp(typ, "m.room.encrypted") || !sid || (only_missing && amx__find_group(c, room_id, sid)))
            continue;
        key = amx__malloc(strlen(room_id) + strlen(sid) + 2);
        sprintf(key, "%s|%s", room_id, sid);
        if (!backup_due(c, key)) { free(key); continue; }
        free(key);
        if (!d) {
            if (!(d = backup_decryption(c))) return -1;
            resc = http_escape(room_id);
            vesc = http_escape(amx__jstr(c->backup, "version"));
        }
        sesc = http_escape(sid);
        path = amx__malloc(strlen(resc) + strlen(sesc) + strlen(vesc) + 40);
        sprintf(path, "/room_keys/keys/%s/%s?version=%s", resc, sesc, vesc);
        r = amx__api(c, "GET", path, NULL, 60, &st);
        free(path);
        free(sesc);
        asked++;
        if (st == 200 && r && import_one(c, d, room_id, sid, cJSON_GetObjectItemCaseSensitive(r, "session_data")) == 1)
            n++;
        cJSON_Delete(r);
    }
    if (d) { olm_clear_pk_decryption(d); free(d); }
    free(resc);
    free(vesc);
    if (n) {
        amx__save_group_sessions(c);
        amx__retry_pending(c);
    }
    if (asked) LOG(c, "backup: %d of %d missing room keys found for %s", n, asked, room_id);
    return n;
}

int amx_backup_room(AmxClient *c, const char *room_id)
{
    unsigned char priv[40];
    char *esc, *vesc, *path, *pub;
    const char *ver;
    OlmPkDecryption *d;
    cJSON *r;
    const cJSON *x;
    long st;
    int n = 0;

    if (load_backup(c)) return 0;                       /* no backup key: nothing to do */
    ver = amx__jstr(c->backup, "version");
    if (!ver || b64dec(amx__jstr(c->backup, "key") ? amx__jstr(c->backup, "key") : "", priv, sizeof priv) != 32)
        return amx__fail(c, "backup.json damaged");
    d = olm_pk_decryption(amx__malloc(olm_pk_decryption_size()));
    pub = amx__malloc(olm_pk_key_length() + 1);
    if (olm_pk_key_from_private(d, pub, olm_pk_key_length(), priv, 32) == olm_error()) {
        OPENSSL_cleanse(priv, sizeof priv);
        free(pub); olm_clear_pk_decryption(d); free(d);
        return amx__fail(c, "backup key unusable");
    }
    OPENSSL_cleanse(priv, sizeof priv);
    free(pub);
    esc = http_escape(room_id);
    vesc = http_escape(ver);
    path = amx__malloc(strlen(esc) + strlen(vesc) + 40);
    sprintf(path, "/room_keys/keys/%s?version=%s", esc, vesc);
    r = amx__api(c, "GET", path, NULL, 120, &st);
    free(path); free(esc); free(vesc);
    if (st == 404) { cJSON_Delete(r); olm_clear_pk_decryption(d); free(d); return 0; }   /* nothing for this room */
    if (st != 200 || !r) { cJSON_Delete(r); olm_clear_pk_decryption(d); free(d); return -1; }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(r, "sessions"))
        if (import_one(c, d, room_id, x->string, cJSON_GetObjectItemCaseSensitive(x, "session_data")) == 1) n++;
    cJSON_Delete(r);
    olm_clear_pk_decryption(d);
    free(d);
    if (n) {
        amx__save_group_sessions(c);
        LOG(c, "backup: %d room keys for %s imported", n, room_id);
        amx__retry_pending(c);                         /* placeholders become messages */
    }
    return n;
}
