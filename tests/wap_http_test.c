#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "net_host.h"
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#endif
#include "wap_gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            ++failures;                                                                                      \
            printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x);                                            \
        }                                                                                                    \
    } while (0)
#ifdef _WIN32
static unsigned __stdcall serve(void *user)
#else
static void *serve(void *user)
#endif
{
    NetSocket listener = *(const NetSocket *)user, client = NET_INVALID;
    char request[4096];
    unsigned tries;
    const char reply[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 15\r\nConnection: close\r\n\r\ngateway-http-ok";
    for (tries = 0; tries < 400; ++tries) {
        client = accept(listener, NULL, NULL);
        if (client != NET_INVALID)
            break;
        net_sleep(5);
    }
    if (client == NET_INVALID)
        return 0;
    if (net_nonblocking(client)) {
        int got = 0;
        for (tries = 0; tries < 200; ++tries) {
            got = (int)recv(client, request, sizeof(request), 0);
            if (got >= 0 || !net_would_block())
                break;
            net_sleep(5);
        }
        if (got > 0) {
            size_t sent = 0;
            for (tries = 0; sent < sizeof(reply) - 1 && tries < 200; ++tries) {
                int n = net_send(client, reply + sent, (int)(sizeof(reply) - 1 - sent));
                if (n > 0)
                    sent += (unsigned)n;
                else if (!net_would_block())
                    break;
                else
                    net_sleep(5);
            }
        }
    }
    net_socket_close(client);
    return 0;
}

int main(void)
{
    NetSocket listener;
    struct sockaddr_in address = {0};
    NetAddrLen size = sizeof(address);
    WapHttpContext ctx = {0};
    char url[100], mime[96];
    uint8_t *body = NULL;
    size_t length = 0;
    unsigned status = 0;
    CHECK(net_start());
    listener = net_socket(SOCK_STREAM, IPPROTO_TCP);
    CHECK(listener != NET_INVALID);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(listener, (struct sockaddr *)&address, sizeof(address)));
    CHECK(!listen(listener, 1));
    CHECK(!getsockname(listener, (struct sockaddr *)&address, &size));
#ifdef _WIN32
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, serve, &listener, 0, NULL);
    CHECK(thread != NULL);
    if (!thread)
        return 1;
#else
    pthread_t thread;
    int error = pthread_create(&thread, NULL, serve, &listener);
    CHECK(!error);
    if (error)
        return 1;
#endif
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)ntohs(address.sin_port));
    CHECK(wap_http_fetch(&ctx, url, &body, &length, mime, &status));
    CHECK(status == 200 && length == 15 && !strcmp(mime, "text/plain"));
    CHECK(body && !memcmp(body, "gateway-http-ok", 15));
    free(body);
    body = NULL;
#ifdef _WIN32
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    CHECK(!pthread_join(thread, NULL));
#endif
    CHECK(!wap_http_fetch(&ctx, "file:///not-http", &body, &length, mime, &status) && !body);
    wap_http_cancel(&ctx);
    CHECK(!wap_http_fetch(&ctx, url, &body, &length, mime, &status) && !body);
    net_socket_close(listener);
    net_stop();
    printf("WAP HTTP worker: %u checks, %u failures.\n", checks, failures);
    return failures ? 1 : 0;
}
