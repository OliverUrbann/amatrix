/* libcurl implementation of http.h (Linux/macOS host build). */
#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CURL *g_curl;

typedef struct { char *buf; size_t len; } Buf;

static size_t on_data(char *p, size_t sz, size_t n, void *ud)
{
    Buf *b = ud;
    size_t add = sz * n;
    char *nb = realloc(b->buf, b->len + add + 1);
    if (!nb) return 0;
    memcpy(nb + b->len, p, add);
    b->buf = nb;
    b->len += add;
    b->buf[b->len] = 0;
    return add;
}

int http_init(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) return -1;
    g_curl = curl_easy_init();
    return g_curl ? 0 : -1;
}

void http_cleanup(void)
{
    if (g_curl) curl_easy_cleanup(g_curl);
    g_curl = NULL;
    curl_global_cleanup();
}

int http_request(const char *method, const char *url, const char *bearer,
                 const char *body, long timeout_s, HttpResponse *out)
{
    return http_request_bin(method, url, bearer, NULL, body, body ? strlen(body) : 0, timeout_s, out);
}

int http_request_bin(const char *method, const char *url, const char *bearer,
                     const char *content_type, const void *body, size_t len,
                     long timeout_s, HttpResponse *out)
{
    struct curl_slist *hdr = NULL;
    char auth[1024], ct[160];
    Buf b = { NULL, 0 };
    CURLcode rc;

    memset(out, 0, sizeof *out);
    curl_easy_reset(g_curl);
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(g_curl, CURLOPT_USERAGENT, "Amatrix/0.1");
    snprintf(ct, sizeof ct, "Content-Type: %s", content_type ? content_type : "application/json");
    hdr = curl_slist_append(hdr, ct);
    if (bearer) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
        hdr = curl_slist_append(hdr, auth);
    }
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, hdr);
    if (body) {
        curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(g_curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
    }
    rc = curl_easy_perform(g_curl);
    curl_slist_free_all(hdr);
    if (rc != CURLE_OK) {
        free(b.buf);
        return -1;
    }
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &out->status);
    out->body = b.buf;
    out->body_len = b.len;
    return 0;
}

void http_response_free(HttpResponse *r)
{
    free(r->body);
    r->body = NULL;
}

char *http_escape(const char *s)
{
    char *e = curl_easy_escape(g_curl, s, 0);
    char *r = e ? strdup(e) : NULL;
    curl_free(e);
    return r;
}

const char *http_last_error(void) { return "libcurl error"; }
