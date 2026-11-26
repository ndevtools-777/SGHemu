/* The WAP gateway worker is the only thread that owns a curl handle. */
#include "wap_gateway.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_LIMIT (1024u * 1024u)

typedef struct {
    uint8_t *bytes;
    size_t length;
} HttpBody;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static int curl_ready;

static void init_curl(void)
{
    curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void wap_http_cancel(WapHttpContext *context)
{
    __atomic_store_n(&context->canceled, 1, __ATOMIC_RELAXED);
}

static int canceled(const WapHttpContext *context)
{
    return context && __atomic_load_n(&context->canceled, __ATOMIC_RELAXED);
}

static int progress(void *user, curl_off_t down, curl_off_t done, curl_off_t up, curl_off_t sent)
{
    (void)down; (void)done; (void)up; (void)sent;
    return canceled(user);
}

static size_t receive(char *bytes, size_t size, size_t count, void *user)
{
    HttpBody *body = user;
    if (count && size > (HTTP_LIMIT - body->length) / count) return 0;
    size *= count;
    memcpy(body->bytes + body->length, bytes, size);
    body->length += size;
    return size;
}

int wap_http_fetch(void *user, const char *url, uint8_t **body, size_t *length, char mime[96], unsigned *status)
{
    CURL *request;
    HttpBody incoming = {0};
    long code = 0;
    char *type = NULL;
    int ok = 0;

    *body = NULL; *length = 0; *status = 502;
    if (canceled(user) || pthread_once(&curl_once, init_curl) || !curl_ready) return 0;
    request = curl_easy_init();
    if (!request) return 0;
    incoming.bytes = malloc(HTTP_LIMIT + 1);
    if (!incoming.bytes) goto done;

    curl_easy_setopt(request, CURLOPT_URL, url);
    curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(request, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request, CURLOPT_MAXREDIRS, 5L);
    /* The limitation applies to the final URL too, not just the first GET. */
#if LIBCURL_VERSION_NUM >= 0x075500
    if (curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "http,https") ||
        curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "http,https")) goto done;
#else
    if (curl_easy_setopt(request, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS)) ||
        curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS))) goto done;
#endif
    curl_easy_setopt(request, CURLOPT_USERAGENT, "SGHemu built-in WAP gateway");
    curl_easy_setopt(request, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(request, CURLOPT_WRITEDATA, &incoming);
    curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(request, CURLOPT_XFERINFODATA, user);
    curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
    if (curl_easy_perform(request) != CURLE_OK || canceled(user)) goto done;
    if (curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK || !code) goto done;
    if (curl_easy_getinfo(request, CURLINFO_CONTENT_TYPE, &type) != CURLE_OK || !type)
        type = "application/octet-stream";
    {
        size_t bytes = strcspn(type, ";");
        if (bytes >= 96) goto done;
        memcpy(mime, type, bytes);
        mime[bytes] = 0;
    }
    incoming.bytes[incoming.length] = 0;
    *body = incoming.bytes;
    *length = incoming.length;
    *status = (unsigned)code;
    ok = 1;
done:
    if (!ok) free(incoming.bytes);
    curl_easy_cleanup(request);
    return ok;
}
