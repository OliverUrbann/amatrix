/* media.c - avatars: server thumbnails, decoded with stb_image (PNG, JPEG),
 * centre-cropped, box-scaled to size x size ARGB and cached in the store as
 * hex text ("av-<hash>-<size>"). */
#include "amx_internal.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
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
