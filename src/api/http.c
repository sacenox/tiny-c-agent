#include "http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "sse.h"

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static void set_error(char **err, const char *fmt, ...) {
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    free(*err);
    *err = xstrdup(detail);
}

typedef struct {
    cage_sse sse;
    char *raw;
    size_t raw_len;
    size_t raw_cap;
    long status;
    const volatile bool *cancel;
    cage_http_data_fn on_data;
    void *user;
} http_state;

static void raw_add(http_state *st, const char *p, size_t n) {
    if (st->raw_len + n + 1 > st->raw_cap) {
        size_t cap = st->raw_cap ? st->raw_cap : 256;
        while (cap < st->raw_len + n + 1) cap *= 2;
        st->raw = realloc(st->raw, cap);
        if (!st->raw) abort();
        st->raw_cap = cap;
    }
    memcpy(st->raw + st->raw_len, p, n);
    st->raw_len += n;
    st->raw[st->raw_len] = '\0';
}

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    http_state *st = ud;
    size_t n = size * nmemb;
    if (st->cancel && *st->cancel) return 0;
    if (st->status >= 400) raw_add(st, ptr, n);
    else cage_sse_feed(&st->sse, ptr, n, st->on_data, st->user);
    return n;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *ud) {
    http_state *st = ud;
    size_t n = size * nmemb;
    if (n >= 5 && memcmp(ptr, "HTTP/", 5) == 0) {
        const char *sp = memchr(ptr, ' ', n);
        if (sp) st->status = atoi(sp + 1);
    }
    return n;
}

static int on_xfer(void *ud, curl_off_t dlt, curl_off_t dln, curl_off_t ult, curl_off_t uln) {
    (void)dlt;
    (void)dln;
    (void)ult;
    (void)uln;
    http_state *st = ud;
    return (st->cancel && *st->cancel) ? 1 : 0;
}

// "provider: HTTP <status>: <best message from the body>".
static char *http_error(long status, const char *body, size_t len) {
    const char *msg = NULL;
    yyjson_doc *doc = body ? yyjson_read(body, len, 0) : NULL;
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *err = yyjson_obj_get(root, "error");
        if (yyjson_is_obj(err)) {
            yyjson_val *m = yyjson_obj_get(err, "message");
            if (yyjson_is_str(m)) msg = yyjson_get_str(m);
        } else if (yyjson_is_str(err)) {
            msg = yyjson_get_str(err);
        } else {
            yyjson_val *m = yyjson_obj_get(root, "message");
            if (yyjson_is_str(m)) msg = yyjson_get_str(m);
        }
    }
    char detail[512];
    snprintf(detail, sizeof detail, "provider: HTTP %ld: %s", status,
             msg ? msg : (body ? body : ""));
    if (doc) yyjson_doc_free(doc);
    return xstrdup(detail);
}

int cage_http_stream(cage_http_request *req) {
    http_state st = {0};
    st.cancel = req->cancel;
    st.on_data = req->on_data;
    st.user = req->user;
    cage_sse_init(&st.sse);

    CURL *curl = curl_easy_init();
    if (!curl) {
        cage_sse_free(&st.sse);
        req->error = xstrdup("provider: cannot init curl");
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(req->body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &st);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_xfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    req->status = st.status;
    req->aborted = (req->cancel && *req->cancel) || res == CURLE_ABORTED_BY_CALLBACK;
    int rc = 0;
    if (req->aborted) {
        // The caller turns this into an "aborted" stop reason.
    } else if (res != CURLE_OK) {
        set_error(&req->error, "provider: %s", curl_easy_strerror(res));
        rc = -1;
    } else if (st.status >= 400) {
        req->error = http_error(st.status, st.raw, st.raw_len);
    }

    cage_sse_free(&st.sse);
    free(st.raw);
    return rc;
}
