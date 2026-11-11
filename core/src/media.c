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
    if (cJSON_GetObjectItemCaseSensitive(content, "org.matrix.msc3245.voice")
        || cJSON_GetObjectItemCaseSensitive(content, "m.voice"))
        cJSON_AddTrueToObject(m, "voice");                       /* a voice message */
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
        if ((x = cJSON_GetObjectItemCaseSensitive(info, "duration")) && cJSON_IsNumber(x))
            cJSON_AddNumberToObject(m, "duration", x->valuedouble);       /* ms */
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
    else if (x && cJSON_IsBool(x)) { snprintf(buf, size, "%d", cJSON_IsTrue(x) ? 1 : 0); r = buf; }
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

/* ---------------------------------------------------------------- sending */

static char *b64enc(const unsigned char *d, size_t n, int url)
{
    const char *tab = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                          : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = amx__malloc(n * 4 / 3 + 4), *q = o;
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned long v = (unsigned long)d[i] << 16 | (unsigned long)d[i + 1] << 8 | d[i + 2];
        *q++ = tab[v >> 18 & 63]; *q++ = tab[v >> 12 & 63]; *q++ = tab[v >> 6 & 63]; *q++ = tab[v & 63];
    }
    if (n - i == 1) {
        unsigned long v = (unsigned long)d[i] << 16;
        *q++ = tab[v >> 18 & 63]; *q++ = tab[v >> 12 & 63];
    } else if (n - i == 2) {
        unsigned long v = (unsigned long)d[i] << 16 | (unsigned long)d[i + 1] << 8;
        *q++ = tab[v >> 18 & 63]; *q++ = tab[v >> 12 & 63]; *q++ = tab[v >> 6 & 63];
    }
    *q = 0;
    return o;
}

static int ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    size_t i;
    if (!dot || strlen(dot + 1) != strlen(ext)) return 0;
    for (i = 0; ext[i]; i++) {
        char ch = dot[1 + i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        if (ch != ext[i]) return 0;
    }
    return 1;
}

/* the MIME type of a file, by its name (Amiga files often have none: then
 * by content for pictures) */
static const char *guess_mime(const char *name, const unsigned char *d, size_t n)
{
    static const char *const tab[][2] = {
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "png", "image/png" }, { "gif", "image/gif" },
        { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "iff", "image/x-ilbm" }, { "ilbm", "image/x-ilbm" },
        { "svg", "image/svg+xml" }, { "txt", "text/plain" }, { "guide", "text/plain" }, { "readme", "text/plain" },
        { "html", "text/html" }, { "htm", "text/html" }, { "pdf", "application/pdf" },
        { "lha", "application/x-lha" }, { "lzh", "application/x-lha" }, { "lzx", "application/x-lzx" },
        { "zip", "application/zip" }, { "gz", "application/gzip" }, { "tar", "application/x-tar" },
        { "adf", "application/x-amiga-disk-format" }, { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" },
        { "wav", "audio/wav" }, { "8svx", "audio/x-8svx" }, { "mod", "audio/x-mod" },
        { "mp4", "video/mp4" }, { "webm", "video/webm" }, { "avi", "video/x-msvideo" }, { "mov", "video/quicktime" },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (ext_is(name, tab[i][0])) return tab[i][1];
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return "image/jpeg";
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return "image/gif";
    if (n >= 12 && !memcmp(d, "FORM", 4) && !memcmp(d + 8, "ILBM", 4)) return "image/x-ilbm";
    return "application/octet-stream";
}

/* upload data to the media repository; the mxc:// URI (malloc'd) or NULL */
static char *upload(AmxClient *c, const void *data, size_t len, const char *name, const char *mime)
{
    char *esc = http_escape(name), *url = amx__malloc(strlen(c->hs) + strlen(esc) + 64), *mxc = NULL;
    HttpResponse r;
    sprintf(url, "%s/_matrix/media/v3/upload?filename=%s", c->hs, esc);
    free(esc);
    LOG2(c, "> POST /_matrix/media/v3/upload (%lu bytes, %s)", (unsigned long)len, mime);
    if (http_request_bin("POST", url, c->token, mime, data, len, 900, &r) != 0) {
        free(url);
        amx__fail(c, "network error: %s (upload)", http_last_error());
        return NULL;
    }
    free(url);
    if (r.status == 200) {
        cJSON *j = r.body ? cJSON_Parse(r.body) : NULL;
        const char *u = amx__jstr(j, "content_uri");
        if (u && !strncmp(u, "mxc://", 6)) mxc = amx__strdup(u);
        else amx__fail(c, "upload: no content_uri in the answer");
        cJSON_Delete(j);
    } else {
        cJSON *j = r.body ? cJSON_Parse(r.body) : NULL;
        const char *code = amx__jstr(j, "errcode"), *msg = amx__jstr(j, "error");
        if (r.status == 413) amx__fail(c, "the file is too large for the server");
        else amx__fail(c, "upload: HTTP %ld %s %s", r.status, code ? code : "", msg ? msg : "");
        cJSON_Delete(j);
    }
    http_response_free(&r);
    return mxc;
}

#define MAX_SEND (100L * 1024 * 1024)

/* the content of an m.image/m.file/... message for the file at path, the file
 * uploaded (AES-256-CTR encrypted first when enc); NULL on error */
cJSON *amx__file_content(AmxClient *c, const char *path, const char *name, int enc)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d = NULL;
    long n;
    const char *mime, *msgtype;
    char *mxc;
    cJSON *ct, *info;
    int iw = 0, ih = 0, ich = 0;

    if (!f) { amx__fail(c, "cannot open %s", path); return NULL; }
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        amx__fail(c, "cannot read %s", path);
        return NULL;
    }
    if (n > MAX_SEND) { fclose(f); amx__fail(c, "%s is too large (over 100 MB)", path); return NULL; }
    d = amx__malloc(n ? n : 1);
    if (n && fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); amx__fail(c, "cannot read %s", path); return NULL; }
    fclose(f);
    if (!name || !*name) {
        const char *s1 = strrchr(path, '/'), *s2 = strrchr(path, ':');
        name = s1 > s2 ? s1 + 1 : s2 ? s2 + 1 : path;
    }
    mime = guess_mime(name, d, n);
    msgtype = !strncmp(mime, "image/", 6) ? "m.image" : !strncmp(mime, "video/", 6) ? "m.video"
            : !strncmp(mime, "audio/", 6) ? "m.audio" : "m.file";
    if (!strcmp(msgtype, "m.image") && !stbi_info_from_memory(d, (int)n, &iw, &ih, &ich))
        iw = ih = 0;
    /* pictures other clients cannot show (IFF, BMP, ...) are sent as files */
    if (!strcmp(msgtype, "m.image") && strcmp(mime, "image/png") && strcmp(mime, "image/jpeg")
        && strcmp(mime, "image/gif") && strcmp(mime, "image/webp"))
        msgtype = "m.file";

    ct = cJSON_CreateObject();
    cJSON_AddStringToObject(ct, "msgtype", msgtype);
    cJSON_AddStringToObject(ct, "body", name);
    cJSON_AddStringToObject(ct, "filename", name);
    info = cJSON_AddObjectToObject(ct, "info");
    cJSON_AddStringToObject(info, "mimetype", mime);
    cJSON_AddNumberToObject(info, "size", (double)n);
    if (iw > 0 && ih > 0) {
        cJSON_AddNumberToObject(info, "w", iw);
        cJSON_AddNumberToObject(info, "h", ih);
    }
    if (enc) {
        unsigned char *key = amx__random(32), *rnd = amx__random(8), iv[16], hash[32];
        unsigned char *ct_data = amx__malloc(n + 16);
        EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
        int l1 = 0, l2 = 0, ok;
        cJSON *file, *jk, *ops;
        char *s;
        memset(iv, 0, sizeof iv);
        memcpy(iv, rnd, 8);                   /* the low 64 bits are the counter */
        free(rnd);
        ok = x && EVP_EncryptInit_ex(x, EVP_aes_256_ctr(), NULL, key, iv) == 1
             && EVP_EncryptUpdate(x, ct_data, &l1, d, (int)n) == 1
             && EVP_EncryptFinal_ex(x, ct_data + l1, &l2) == 1;
        if (x) EVP_CIPHER_CTX_free(x);
        free(d);
        if (!ok) {
            OPENSSL_cleanse(key, 32);
            free(key); free(ct_data); cJSON_Delete(ct);
            amx__fail(c, "encryption of the attachment failed");
            return NULL;
        }
        SHA256(ct_data, (size_t)n, hash);
        mxc = upload(c, ct_data, (size_t)n, name, "application/octet-stream");
        free(ct_data);
        if (!mxc) { OPENSSL_cleanse(key, 32); free(key); cJSON_Delete(ct); return NULL; }
        file = cJSON_AddObjectToObject(ct, "file");
        cJSON_AddStringToObject(file, "v", "v2");
        cJSON_AddStringToObject(file, "url", mxc);
        jk = cJSON_AddObjectToObject(file, "key");
        cJSON_AddStringToObject(jk, "kty", "oct");
        ops = cJSON_AddArrayToObject(jk, "key_ops");
        cJSON_AddItemToArray(ops, cJSON_CreateString("encrypt"));
        cJSON_AddItemToArray(ops, cJSON_CreateString("decrypt"));
        cJSON_AddStringToObject(jk, "alg", "A256CTR");
        s = b64enc(key, 32, 1);
        cJSON_AddStringToObject(jk, "k", s);
        OPENSSL_cleanse(s, strlen(s));
        free(s);
        cJSON_AddTrueToObject(jk, "ext");
        OPENSSL_cleanse(key, 32);
        free(key);
        s = b64enc(iv, 16, 0); cJSON_AddStringToObject(file, "iv", s); free(s);
        s = b64enc(hash, 32, 0);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(file, "hashes"), "sha256", s);
        free(s);
    } else {
        mxc = upload(c, d, (size_t)n, name, mime);
        free(d);
        if (!mxc) { cJSON_Delete(ct); return NULL; }
        cJSON_AddStringToObject(ct, "url", mxc);
    }
    LOG(c, "uploaded %s (%ld bytes, %s) as %s", name, n, mime, mxc);
    free(mxc);
    return ct;
}

/* ------------------------------------------------------------ link previews */

int amx_url_preview(AmxClient *c, const char *url, char **title, char **description, char **image)
{
    static const char *const paths[] = { "/_matrix/client/v1/media/preview_url?url=", "/_matrix/media/v3/preview_url?url=" };
    char *esc = http_escape(url), *full = amx__malloc(strlen(c->hs) + strlen(esc) + 80);
    HttpResponse r;
    cJSON *j = NULL;
    int i, got = -1;
    *title = *description = *image = NULL;
    if (!c->token) { free(esc); free(full); return amx__fail(c, "not logged in"); }
    for (i = 0; i < 2; i++) {
        sprintf(full, "%s%s%s", c->hs, paths[i], esc);
        LOG2(c, "> GET %s... (link preview)", paths[i]);
        if (http_request("GET", full, c->token, NULL, 60, &r) != 0) {
            amx__fail(c, "network error: %s (link preview)", http_last_error());
            break;
        }
        if (r.status == 200) { j = r.body ? cJSON_Parse(r.body) : NULL; got = j ? 0 : -1; http_response_free(&r); break; }
        if (i == 0 && (r.status == 404 || r.status == 400 || r.status == 405)) { http_response_free(&r); continue; }
        amx__fail(c, "link preview: HTTP %ld", r.status);
        http_response_free(&r);
        break;
    }
    free(esc);
    free(full);
    if (got) { cJSON_Delete(j); return -1; }
    {
        const char *t = amx__jstr(j, "og:title"), *d = amx__jstr(j, "og:description");
        const char *im = amx__jstr(j, "og:image"), *site = amx__jstr(j, "og:site_name");
        if (!t) t = site;
        if (t && *t) *title = amx__strdup(t);
        if (d && *d) *description = amx__strdup(d);
        if (im && !strncmp(im, "mxc://", 6)) *image = amx__strdup(im);
    }
    cJSON_Delete(j);
    if (!*title && !*description) return amx__fail(c, "no preview for this page");
    return 0;
}
