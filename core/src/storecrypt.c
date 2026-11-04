/* storecrypt.c - optional passphrase for the store (the drawer with login,
 * keys and sessions).
 *
 * Without a passphrase files are written as before.  With one, every file
 * the core writes through amx__write_store() is "AMXE1 " + base64(iv |
 * ciphertext | tag): AES-256-GCM, the file name as additional data (files
 * cannot be swapped).  The key comes from PBKDF2-HMAC-SHA256 over the
 * passphrase; salt, rounds and an encrypted check value are in store.json
 * (that file itself is plain).  Unencrypted files are still read, so an
 * old store keeps working and can be converted.
 */
#include "amx_internal.h"

#ifdef AMX_AMISSL
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#endif
#include <openssl/evp.h>
#include <openssl/crypto.h>

#define MAGIC "AMXE1 "
#define ROUNDS 60000                  /* ~1 s on a Vampire, a few on a 68030 */
#define CHECK "amatrix store key check"

/* the files of a store (avatars "av-*" are not secret and stay as they are) */
static const char *const store_files[] = {
    "login.json", "account", "keys_uploaded", "olm_sessions", "megolm_sessions", "megolm_out.json",
    "since", "rooms.json", "direct.json", "backup.json", NULL };

static char *path_of(AmxClient *c, const char *name)
{
    char *p = amx__malloc(strlen(c->store) + strlen(name) + 2);
    sprintf(p, "%s/%s", c->store, name);
    return p;
}

static char *b64(const unsigned char *d, size_t n)
{
    char *o = amx__malloc(4 * ((n + 2) / 3) + 1);
    EVP_EncodeBlock((unsigned char *)o, d, (int)n);
    return o;
}

/* base64 -> bytes; *n = length; NULL on error */
static unsigned char *unb64(const char *s, size_t *n)
{
    size_t l = strcspn(s, "\r\n");
    unsigned char *o = amx__malloc(l / 4 * 3 + 3);
    int r;
    if (l % 4) { free(o); return NULL; }
    r = EVP_DecodeBlock(o, (const unsigned char *)s, (int)l);
    if (r < 0) { free(o); return NULL; }
    while (l && s[l - 1] == '=') { r--; l--; }      /* DecodeBlock counts padding */
    *n = (size_t)r;
    return o;
}

static char *seal(const unsigned char key[32], const char *name, const char *text)
{
    size_t n = strlen(text);
    unsigned char *buf = amx__malloc(12 + n + 16);
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    int l = 0, l2 = 0, ok;
    char *enc, *out;
    if (plat_random(buf, 12) || !x) { free(buf); EVP_CIPHER_CTX_free(x); return NULL; }
    ok = EVP_EncryptInit_ex(x, EVP_aes_256_gcm(), NULL, key, buf) == 1 &&
         EVP_EncryptUpdate(x, NULL, &l, (const unsigned char *)name, (int)strlen(name)) == 1 &&
         EVP_EncryptUpdate(x, buf + 12, &l, (const unsigned char *)text, (int)n) == 1 &&
         EVP_EncryptFinal_ex(x, buf + 12 + l, &l2) == 1 &&
         EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_GET_TAG, 16, buf + 12 + n) == 1;
    EVP_CIPHER_CTX_free(x);
    if (!ok) { free(buf); return NULL; }
    enc = b64(buf, 12 + n + 16);
    free(buf);
    out = amx__malloc(strlen(MAGIC) + strlen(enc) + 2);
    sprintf(out, "%s%s\n", MAGIC, enc);
    free(enc);
    return out;
}

/* decrypted text or NULL (wrong key, damaged, swapped) */
static char *open_sealed(const unsigned char key[32], const char *name, const char *file)
{
    size_t n;
    unsigned char *raw = unb64(file + strlen(MAGIC), &n);
    EVP_CIPHER_CTX *x;
    char *out;
    int l = 0, l2 = 0, ok;
    if (!raw || n < 28) { free(raw); return NULL; }
    out = amx__malloc(n - 28 + 1);
    x = EVP_CIPHER_CTX_new();
    ok = x && EVP_DecryptInit_ex(x, EVP_aes_256_gcm(), NULL, key, raw) == 1 &&
         EVP_DecryptUpdate(x, NULL, &l, (const unsigned char *)name, (int)strlen(name)) == 1 &&
         EVP_DecryptUpdate(x, (unsigned char *)out, &l, raw + 12, (int)(n - 28)) == 1 &&
         EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_TAG, 16, raw + n - 16) == 1 &&
         EVP_DecryptFinal_ex(x, (unsigned char *)out + l, &l2) == 1;
    EVP_CIPHER_CTX_free(x);
    free(raw);
    if (!ok) { OPENSSL_cleanse(out, n - 28); free(out); return NULL; }
    out[n - 28] = 0;
    return out;
}

/* ------------------------------------------------- used by client.c */

char *amx__store_decode(AmxClient *c, const char *name, char *file)
{
    char *t;
    if (!file || strncmp(file, MAGIC, strlen(MAGIC))) return file;      /* plain */
    if (!c->skey_set) { free(file); return NULL; }                       /* locked */
    t = open_sealed(c->skey, name, file);
    free(file);
    if (!t) LOG(c, "store: %s cannot be decrypted", name);
    return t;
}

/* what to write: a new buffer (encrypted) or NULL = write data as it is */
char *amx__store_encode(AmxClient *c, const char *name, const char *data)
{
    if (!c->skey_set || !strncmp(name, "av-", 3)) return NULL;
    return seal(c->skey, name, data);
}

/* ------------------------------------------------------- public API */

int amx_store_encrypted(AmxClient *c)
{
    char *p = path_of(c, "store.json"), *t = plat_read_file(p);
    free(p);
    free(t);
    return t != NULL;
}

int amx_store_locked(AmxClient *c) { return amx_store_encrypted(c) && !c->skey_set; }

static int derive(const char *pass, const unsigned char *salt, size_t sl, int rounds, unsigned char key[32])
{
    return PKCS5_PBKDF2_HMAC(pass, (int)strlen(pass), salt, (int)sl, rounds, EVP_sha256(), 32, key) == 1 ? 0 : -1;
}

int amx_store_unlock(AmxClient *c, const char *pass)
{
    char *p = path_of(c, "store.json"), *t = plat_read_file(p), *chk;
    cJSON *j;
    unsigned char *salt, key[32];
    size_t sl;
    const cJSON *it;
    free(p);
    if (!t) return amx__fail(c, "the store has no passphrase");
    j = cJSON_Parse(t);
    free(t);
    it = cJSON_GetObjectItemCaseSensitive(j, "rounds");
    if (!j || !amx__jstr(j, "salt") || !amx__jstr(j, "check") || !cJSON_IsNumber(it) ||
        !(salt = unb64(amx__jstr(j, "salt"), &sl))) {
        cJSON_Delete(j);
        return amx__fail(c, "store.json damaged");
    }
    LOG(c, "store: deriving the key (%d rounds) ...", it->valueint);
    if (derive(pass, salt, sl, it->valueint, key)) { free(salt); cJSON_Delete(j); return amx__fail(c, "PBKDF2 failed"); }
    free(salt);
    chk = open_sealed(key, "store.json", amx__jstr(j, "check"));
    cJSON_Delete(j);
    if (!chk || strcmp(chk, CHECK)) {
        free(chk);
        OPENSSL_cleanse(key, sizeof key);
        return amx__fail(c, "wrong passphrase for the store");
    }
    free(chk);
    memcpy(c->skey, key, 32);
    c->skey_set = 1;
    OPENSSL_cleanse(key, sizeof key);
    LOG(c, "store unlocked");
    return 0;
}

int amx_store_set_passphrase(AmxClient *c, const char *pass)
{
    char *content[sizeof store_files / sizeof store_files[0]];
    unsigned char salt[16];
    int i, r = 0;

    if (amx_store_locked(c)) return amx__fail(c, "store is locked: unlock it first");
    plat_mkdir(c->store);
    /* read everything with the old key (or plain) */
    for (i = 0; store_files[i]; i++) content[i] = amx__read_store(c, store_files[i]);

    if (pass && *pass) {
        cJSON *j = cJSON_CreateObject();
        char *s, *chk, *p;
        unsigned char key[32];
        if (plat_random(salt, sizeof salt) || derive(pass, salt, sizeof salt, ROUNDS, key)) {
            for (i = 0; store_files[i]; i++) free(content[i]);
            cJSON_Delete(j);
            return amx__fail(c, "no random source / PBKDF2 failed");
        }
        memcpy(c->skey, key, 32);
        c->skey_set = 1;
        OPENSSL_cleanse(key, sizeof key);
        s = b64(salt, sizeof salt);
        chk = seal(c->skey, "store.json", CHECK);
        chk[strcspn(chk, "\n")] = 0;
        cJSON_AddNumberToObject(j, "v", 1);
        cJSON_AddStringToObject(j, "kdf", "pbkdf2-hmac-sha256");
        cJSON_AddStringToObject(j, "salt", s);
        cJSON_AddNumberToObject(j, "rounds", ROUNDS);
        cJSON_AddStringToObject(j, "check", chk);
        free(s); free(chk);
        p = cJSON_Print(j);
        cJSON_Delete(j);
        {
            char *path = path_of(c, "store.json");
            r = plat_write_file(path, p);
            free(path);
        }
        free(p);
    } else {
        char *path = path_of(c, "store.json");
        OPENSSL_cleanse(c->skey, sizeof c->skey);
        c->skey_set = 0;
        remove(path);
        free(path);
    }
    /* write everything again with the new key (or plain) */
    for (i = 0; store_files[i]; i++) {
        if (content[i]) {
            if (amx__write_store(c, store_files[i], content[i])) r = -1;
            OPENSSL_cleanse(content[i], strlen(content[i]));
            free(content[i]);
        }
    }
    LOG(c, "store %s", pass && *pass ? "encrypted with the passphrase" : "no longer encrypted");
    return r ? amx__fail(c, "could not rewrite the store") : 0;
}
