/* media.c - avatars: server thumbnails, decoded with stb_image (PNG, JPEG),
 * centre-cropped, box-scaled to size x size ARGB and cached in the store as
 * hex text ("av-<hash>-<size>").
 *
 * Attachments (m.image, m.file, ...): previews fitted into a box, whole files
 * saved to disk; encrypted attachments (AES-256-CTR, SHA-256 checked) are
 * decrypted here. */
#include "amx_internal.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../../third_party/stb/stb_image.h"

static unsigned long fnv(const char *s)
{
    unsigned long h = 2166136261UL;
    for (; *s; s++) h = ((h ^ (unsigned char)*s) * 16777619UL) & 0xffffffffUL;
    return h;
}

static unsigned long *from_cache(AmxClient *c, const char *name, int size)
{
    char *t = amx__read_store(c, name);
    unsigned long *px;
    int i, n = size * size;
    if (!t) return NULL;
    if ((int)strlen(t) < n * 8) { free(t); return NULL; }
    px = amx__malloc(sizeof *px * n);
    for (i = 0; i < n; i++) {
        char h[9];
        memcpy(h, t + i * 8, 8);
        h[8] = 0;
        px[i] = strtoul(h, NULL, 16);
    }
    free(t);
    return px;
}

static void to_cache(AmxClient *c, const char *name, const unsigned long *px, int size)
{
    int i, n = size * size;
    char *t = amx__malloc(n * 8 + 1);
    for (i = 0; i < n; i++) sprintf(t + i * 8, "%08lx", px[i] & 0xffffffffUL);
    amx__write_store(c, name, t);
    free(t);
}

/* RGBA w x h -> centre square -> size x size ARGB (box filter; nearest when enlarging) */
static unsigned long *scale(const unsigned char *rgba, int w, int h, int size)
{
    unsigned long *px = amx__malloc(sizeof *px * size * size);
    int sq = w < h ? w : h, ox = (w - sq) / 2, oy = (h - sq) / 2, x, y;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            int x0 = ox + x * sq / size, x1 = ox + (x + 1) * sq / size;
            int y0 = oy + y * sq / size, y1 = oy + (y + 1) * sq / size, i, j;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (j = y0; j < y1; j++)
                for (i = x0; i < x1; i++) {
                    const unsigned char *p = rgba + ((size_t)j * w + i) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            px[y * size + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    return px;
}

static int fetch(AmxClient *c, const char *server, const char *id, int px, HttpResponse *r)
{
    static const char *const paths[] = { "/_matrix/client/v1/media/thumbnail/",
                                         "/_matrix/media/v3/thumbnail/" };
    char *url = amx__malloc(strlen(c->hs) + strlen(server) + strlen(id) + 120);
    int i;
    for (i = 0; i < 2; i++) {
        sprintf(url, "%s%s%s/%s?width=%d&height=%d&method=crop", c->hs, paths[i], server, id, px, px);
        LOG2(c, "> GET %s%s/%s (thumbnail)", paths[i], server, id);
        if (http_request("GET", url, c->token, NULL, 60, r) != 0) {
            free(url);
            return amx__fail(c, "network error loading %s/%s: %s", server, id, http_last_error());
        }
        if (r->status == 200) { free(url); return 0; }
        /* old servers: no authenticated media (v1) yet */
        if (i == 0 && (r->status == 404 || r->status == 400 || r->status == 405)) {
            http_response_free(r);
            continue;
        }
        amx__fail(c, "thumbnail %s/%s: HTTP %ld", server, id, r->status);
        http_response_free(r);
        break;
    }
    free(url);
    return -1;
}

int amx_avatar(AmxClient *c, const char *mxc, int size, unsigned long **argb)
{
    char server[256], id[256], cache[48];
    const char *p, *slash;
    HttpResponse r;
    unsigned char *rgba;
    int w = 0, h = 0, ch = 0;

    *argb = NULL;
    if (!mxc || strncmp(mxc, "mxc://", 6) || size < 1 || size > 256) return amx__fail(c, "bad avatar URL");
    p = mxc + 6;
    slash = strchr(p, '/');
    if (!slash || slash == p || (size_t)(slash - p) >= sizeof server || strlen(slash + 1) >= sizeof id
        || !slash[1] || strpbrk(slash + 1, "/?#"))
        return amx__fail(c, "bad avatar URL %s", mxc);
    memcpy(server, p, slash - p);
    server[slash - p] = 0;
    strcpy(id, slash + 1);
    sprintf(cache, "av-%08lx-%d", fnv(mxc), size);
    if ((*argb = from_cache(c, cache, size)) != NULL) return 0;
    if (!c->token) return amx__fail(c, "not logged in");

    if (fetch(c, server, id, size < 32 ? 32 : size, &r)) return -1;
    rgba = stbi_load_from_memory((const unsigned char *)r.body, (int)r.body_len, &w, &h, &ch, 4);
    http_response_free(&r);
    if (!rgba || w < 1 || h < 1) {
        if (rgba) stbi_image_free(rgba);
        return amx__fail(c, "avatar %s: not a PNG/JPEG picture", mxc);
    }
    *argb = scale(rgba, w, h, size);
    stbi_image_free(rgba);
    to_cache(c, cache, *argb, size);
    return 0;
}

/* ------------------------------------------------------------ attachments */

/* one attachment of a content (or a gallery item): mxc / file, type, info */
static cJSON *media_item(const cJSON *content, const char *msgtype)
{
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(content, "info"), *f, *x;
    cJSON *m = cJSON_CreateObject();
    if (amx__jstr(content, "url")) cJSON_AddStringToObject(m, "mxc", amx__jstr(content, "url"));
    if ((f = cJSON_GetObjectItemCaseSensitive(content, "file")) && cJSON_IsObject(f))
        cJSON_AddItemToObject(m, "file", cJSON_Duplicate(f, 1));
    if (!cJSON_GetObjectItemCaseSensitive(m, "mxc") && !cJSON_GetObjectItemCaseSensitive(m, "file")) {
        cJSON_Delete(m);
        return NULL;
    }
    cJSON_AddStringToObject(m, "type", msgtype);
    if (amx__jstr(content, "body")) cJSON_AddStringToObject(m, "name", amx__jstr(content, "body"));
    if (amx__jstr(content, "filename")) cJSON_ReplaceItemInObject(m, "name", cJSON_CreateString(amx__jstr(content, "filename")));
    if (info) {
        if (amx__jstr(info, "mimetype")) cJSON_AddStringToObject(m, "mime", amx__jstr(info, "mimetype"));
        if ((x = cJSON_GetObjectItemCaseSensitive(info, "size")) && cJSON_IsNumber(x))
            cJSON_AddNumberToObject(m, "size", x->valuedouble);
        if ((x = cJSON_GetObjectItemCaseSensitive(info, "w")) && cJSON_IsNumber(x))
            cJSON_AddNumberToObject(m, "w", x->valuedouble);
        if ((x = cJSON_GetObjectItemCaseSensitive(info, "h")) && cJSON_IsNumber(x))
            cJSON_AddNumberToObject(m, "h", x->valuedouble);
        if (amx__jstr(info, "thumbnail_url")) cJSON_AddStringToObject(m, "tmxc", amx__jstr(info, "thumbnail_url"));
        if ((f = cJSON_GetObjectItemCaseSensitive(info, "thumbnail_file")) && cJSON_IsObject(f))
            cJSON_AddItemToObject(m, "tfile", cJSON_Duplicate(f, 1));
    }
    return m;
}

static int is_media_type(const char *t)
{
    return t && (!strcmp(t, "m.image") || !strcmp(t, "m.file") || !strcmp(t, "m.video") || !strcmp(t, "m.audio")
                 || !strcmp(t, "m.sticker"));
}

/* the attachments of a message content, as a compact JSON array (several for
 * a gallery, MSC4274), NULL if none */
char *amx__media_json(const cJSON *content, const char *msgtype)
{
    cJSON *list = cJSON_CreateArray(), *it;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(content, "itemtypes"), *x;
    char *out = NULL;
    if (!items) items = cJSON_GetObjectItemCaseSensitive(content, "items");
    if (is_media_type(msgtype)) {
        if ((it = media_item(content, msgtype)) != NULL) cJSON_AddItemToArray(list, it);
    } else if (cJSON_IsArray(items)) {                                   /* a gallery */
        cJSON_ArrayForEach(x, items) {
            const char *t = amx__jstr(x, "itemtype") ? amx__jstr(x, "itemtype") : amx__jstr(x, "msgtype");
            if (is_media_type(t) && (it = media_item(x, t)) != NULL) cJSON_AddItemToArray(list, it);
        }
    }
    if (cJSON_GetArraySize(list) > 0) out = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    return out;
}

int amx_media_count(const char *media)
{
    cJSON *m = media ? cJSON_Parse(media) : NULL;
    int n = cJSON_IsArray(m) ? cJSON_GetArraySize(m) : 0;
    cJSON_Delete(m);
    return n;
}

const char *amx_media_info(const char *media, int index, const char *field, char *buf, size_t size)
{
    cJSON *m = media ? cJSON_Parse(media) : NULL;
    const cJSON *it = cJSON_GetArrayItem(m, index), *x = cJSON_GetObjectItemCaseSensitive(it, field);
    const char *r = NULL;
    if (x && cJSON_IsString(x)) { snprintf(buf, size, "%s", x->valuestring); r = buf; }
    else if (x && cJSON_IsNumber(x)) { snprintf(buf, size, "%.0f", x->valuedouble); r = buf; }
    cJSON_Delete(m);
    return r;
}

/* item index of a media list, parsed (caller deletes *root) */
static cJSON *media_at(const char *media, int index, cJSON **root)
{
    *root = media ? cJSON_Parse(media) : NULL;
    return cJSON_IsArray(*root) ? cJSON_GetArrayItem(*root, index) : NULL;
}

static int split_mxc(AmxClient *c, const char *mxc, char server[256], char id[256])
{
    const char *p, *slash;
    if (!mxc || strncmp(mxc, "mxc://", 6)) return amx__fail(c, "bad media URL");
    p = mxc + 6;
    slash = strchr(p, '/');
    if (!slash || slash == p || (size_t)(slash - p) >= 256 || strlen(slash + 1) >= 256 || !slash[1]
        || strpbrk(slash + 1, "/?#"))
        return amx__fail(c, "bad media URL %s", mxc);
    memcpy(server, p, slash - p);
    server[slash - p] = 0;
    strcpy(id, slash + 1);
    return 0;
}

/* GET a whole file (authenticated media v1, v3 for old servers) */
static int download(AmxClient *c, const char *mxc, HttpResponse *r)
{
    static const char *const paths[] = { "/_matrix/client/v1/media/download/", "/_matrix/media/v3/download/" };
    char server[256], id[256], *url;
    int i;
    if (split_mxc(c, mxc, server, id)) return -1;
    url = amx__malloc(strlen(c->hs) + strlen(server) + strlen(id) + 80);
    for (i = 0; i < 2; i++) {
        sprintf(url, "%s%s%s/%s", c->hs, paths[i], server, id);
        LOG2(c, "> GET %s%s/%s", paths[i], server, id);
        if (http_request("GET", url, c->token, NULL, 300, r) != 0) {
            free(url);
            return amx__fail(c, "network error loading %s: %s", mxc, http_last_error());
        }
        if (r->status == 200) { free(url); return 0; }
        if (i == 0 && (r->status == 404 || r->status == 400 || r->status == 405)) {
            http_response_free(r);
            continue;
        }
        amx__fail(c, "%s: HTTP %ld", mxc, r->status);
        http_response_free(r);
        break;
    }
    free(url);
    return -1;
}

/* unpadded base64 / base64url */
static int b64any(const char *s, unsigned char *out, int max)
{
    unsigned long v = 0;
    int bits = 0, n = 0;
    for (; s && *s && *s != '='; s++) {
        int d;
        char ch = *s;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+' || ch == '-') d = 62;
        else if (ch == '/' || ch == '_') d = 63;
        else return -1;
        v = (v << 6) | (unsigned long)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (unsigned char)(v >> bits);
        }
    }
    return n;
}

/* an encrypted attachment (EncryptedFile: url, key.k, iv, hashes.sha256):
 * download, check, decrypt in place */
static int download_encrypted(AmxClient *c, const cJSON *file, HttpResponse *r)
{
    unsigned char key[40], iv[20], want[40], got[32];
    const char *k = amx__jstr(cJSON_GetObjectItemCaseSensitive(file, "key"), "k");
    const char *h = amx__jstr(cJSON_GetObjectItemCaseSensitive(file, "hashes"), "sha256");
    EVP_CIPHER_CTX *x;
    int l1 = 0, l2 = 0, ok;
    unsigned char *pt;
    if (!k || b64any(k, key, sizeof key) != 32 || b64any(amx__jstr(file, "iv"), iv, sizeof iv) != 16
        || !h || b64any(h, want, sizeof want) != 32)
        return amx__fail(c, "encrypted attachment: bad key description");
    if (download(c, amx__jstr(file, "url"), r)) return -1;
    SHA256((const unsigned char *)r->body, r->body_len, got);
    if (CRYPTO_memcmp(got, want, 32)) {
        http_response_free(r);
        return amx__fail(c, "encrypted attachment: wrong checksum");
    }
    pt = amx__malloc(r->body_len + 16);
    x = EVP_CIPHER_CTX_new();
    ok = x && EVP_DecryptInit_ex(x, EVP_aes_256_ctr(), NULL, key, iv) == 1
         && EVP_DecryptUpdate(x, pt, &l1, (const unsigned char *)r->body, (int)r->body_len) == 1
         && EVP_DecryptFinal_ex(x, pt + l1, &l2) == 1;
    if (x) EVP_CIPHER_CTX_free(x);
    OPENSSL_cleanse(key, sizeof key);
    if (!ok) { free(pt); http_response_free(r); return amx__fail(c, "encrypted attachment: decryption failed"); }
    memcpy(r->body, pt, r->body_len);
    free(pt);
    return 0;
}

/* RGBA w x h -> fitted into bw x bh (aspect kept, not enlarged), ARGB */
static unsigned long *fit(const unsigned char *rgba, int w, int h, int bw, int bh, int *ow, int *oh)
{
    unsigned long *px;
    int tw = w, th = h, x, y;
    if (tw > bw) { th = (int)((long)th * bw / tw); tw = bw; }
    if (th > bh) { tw = (int)((long)tw * bh / th); th = bh; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    px = amx__malloc(sizeof *px * tw * th);
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            int x0 = (int)((long)x * w / tw), x1 = (int)((long)(x + 1) * w / tw);
            int y0 = (int)((long)y * h / th), y1 = (int)((long)(y + 1) * h / th), i, j;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (j = y0; j < y1; j++)
                for (i = x0; i < x1; i++) {
                    const unsigned char *p = rgba + ((size_t)j * w + i) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            px[y * tw + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    *ow = tw;
    *oh = th;
    return px;
}

static int server_thumbnail(AmxClient *c, const char *mxc, int bw, int bh, HttpResponse *r)
{
    static const char *const paths[] = { "/_matrix/client/v1/media/thumbnail/", "/_matrix/media/v3/thumbnail/" };
    char server[256], id[256], *url;
    int i;
    if (split_mxc(c, mxc, server, id)) return -1;
    url = amx__malloc(strlen(c->hs) + strlen(server) + strlen(id) + 120);
    for (i = 0; i < 2; i++) {
        sprintf(url, "%s%s%s/%s?width=%d&height=%d&method=scale", c->hs, paths[i], server, id, bw, bh);
        LOG2(c, "> GET %s%s/%s (preview)", paths[i], server, id);
        if (http_request("GET", url, c->token, NULL, 120, r) != 0) {
            free(url);
            return amx__fail(c, "network error loading %s: %s", mxc, http_last_error());
        }
        if (r->status == 200) { free(url); return 0; }
        if (i == 0 && (r->status == 404 || r->status == 400 || r->status == 405)) {
            http_response_free(r);
            continue;
        }
        amx__fail(c, "preview %s: HTTP %ld", mxc, r->status);
        http_response_free(r);
        break;
    }
    free(url);
    return -1;
}

#define MAX_FULL_PREVIEW (6L * 1024 * 1024)   /* decode a whole picture only up to this size */

int amx_media_thumbnail(AmxClient *c, const char *media, int index, int max_w, int max_h,
                        unsigned long **argb, int *w, int *h)
{
    cJSON *root, *m;
    const cJSON *tfile, *file, *sz;
    const char *mime;
    HttpResponse r;
    unsigned char *rgba;
    int iw = 0, ih = 0, ch = 0, got = -1;

    *argb = NULL;
    *w = *h = 0;
    if (!c->token) return amx__fail(c, "not logged in");
    if (max_w < 8 || max_h < 8 || max_w > 1024 || max_h > 1024) return amx__fail(c, "bad preview size");
    if (!(m = media_at(media, index, &root))) { cJSON_Delete(root); return amx__fail(c, "bad media description"); }
    tfile = cJSON_GetObjectItemCaseSensitive(m, "tfile");
    file = cJSON_GetObjectItemCaseSensitive(m, "file");
    mime = amx__jstr(m, "mime");
    sz = cJSON_GetObjectItemCaseSensitive(m, "size");

    if (tfile) got = download_encrypted(c, tfile, &r);                    /* encrypted preview */
    else if (amx__jstr(m, "tmxc") && !file) got = download(c, amx__jstr(m, "tmxc"), &r);
    else if (amx__jstr(m, "mxc")) got = server_thumbnail(c, amx__jstr(m, "mxc"), max_w, max_h, &r);
    if (got && (!mime || !strncmp(mime, "image/", 6))                    /* no preview: the picture */
        && (!sz || sz->valuedouble <= MAX_FULL_PREVIEW)) {
        if (file) got = download_encrypted(c, file, &r);
        else if (amx__jstr(m, "mxc")) got = download(c, amx__jstr(m, "mxc"), &r);
    }
    cJSON_Delete(root);
    if (got) return -1;
    rgba = stbi_load_from_memory((const unsigned char *)r.body, (int)r.body_len, &iw, &ih, &ch, 4);
    http_response_free(&r);
    if (!rgba || iw < 1 || ih < 1) {
        if (rgba) stbi_image_free(rgba);
        return amx__fail(c, "preview: not a PNG/JPEG/GIF picture");
    }
    *argb = fit(rgba, iw, ih, max_w, max_h, w, h);
    stbi_image_free(rgba);
    return 0;
}

int amx_media_save(AmxClient *c, const char *media, int index, const char *path)
{
    cJSON *root, *m;
    const cJSON *file;
    HttpResponse r;
    int got;
    FILE *f;
    if (!c->token) return amx__fail(c, "not logged in");
    if (!(m = media_at(media, index, &root))) { cJSON_Delete(root); return amx__fail(c, "bad media description"); }
    file = cJSON_GetObjectItemCaseSensitive(m, "file");
    got = file ? download_encrypted(c, file, &r)
        : amx__jstr(m, "mxc") ? download(c, amx__jstr(m, "mxc"), &r) : amx__fail(c, "no file");
    cJSON_Delete(root);
    if (got) return -1;
    f = fopen(path, "wb");
    if (!f) { http_response_free(&r); return amx__fail(c, "cannot write %s", path); }
    got = fwrite(r.body, 1, r.body_len, f) == r.body_len ? 0 : -1;
    if (fclose(f)) got = -1;
    http_response_free(&r);
    if (got) return amx__fail(c, "cannot write %s", path);
    LOG(c, "saved attachment to %s", path);
    return 0;
}
