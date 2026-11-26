/* Firmware DS socket boundary. No Java bytecode/HTTP/installer replacement. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "net_host.h"
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <stdatomic.h>
#endif
#include "java_net_guest.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define STREAMS 8
#define TX_BYTES 65536u
typedef struct {
#ifdef _WIN32
    volatile LONG refs, done;
#else
    atomic_int refs, done;
#endif
    int success;
    char host[256], address[16];
} DnsJob;
typedef struct {
    NetSocket socket;unsigned handle,connecting,read_wait,event;
    unsigned tx_head,tx_count;uint8_t tx[TX_BYTES];
} Stream;
struct JavaNetRuntime {Stream stream[STREAMS];DnsJob *dns;unsigned dns_notified;};
static void dns_release(DnsJob *job)
{
    if (!job) return;
#ifdef _WIN32
    if (!InterlockedDecrement(&job->refs)) free(job);
#else
    if (atomic_fetch_sub(&job->refs, 1) == 1) free(job);
#endif
}
static int dns_done(DnsJob *job)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&job->done, 0, 0) != 0;
#else
    return atomic_load_explicit(&job->done, memory_order_acquire);
#endif
}
#ifdef _WIN32
static unsigned __stdcall resolve(void *context)
#else
static void *resolve(void *context)
#endif
{
    DnsJob *job=context;struct addrinfo hints={0},*result=NULL,*p;
    /* Whitespace and controls are not host names. Some NSS backends retry
       even queries for these strings, delaying the guest's failure callback. */
    const unsigned char *ch=(const unsigned char *)job->host;
    while(*ch>32&&*ch!=127)++ch;
    if(!*ch&&net_start()){
        hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
        if(!getaddrinfo(job->host,NULL,&hints,&result)){
            for(p=result;p;p=p->ai_next)if(p->ai_family==AF_INET&&p->ai_addrlen>=sizeof(struct sockaddr_in)){
                const uint8_t *ip=(const uint8_t *)&((struct sockaddr_in *)p->ai_addr)->sin_addr;
                snprintf(job->address,sizeof(job->address),"%u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]);job->success=1;break;
            }
            freeaddrinfo(result);
        }net_stop();
    }
#ifdef _WIN32
    InterlockedExchange(&job->done, 1);
#else
    atomic_store_explicit(&job->done, 1, memory_order_release);
#endif
    dns_release(job);return 0;
}
static int start_resolver(DnsJob *job)
{
#ifdef _WIN32
    uintptr_t thread;
    job->refs = 2;
    thread = _beginthreadex(NULL, 0, resolve, job, 0, NULL);
    if (!thread) return 0;
    CloseHandle((HANDLE)thread);
    return 1;
#else
    pthread_attr_t attributes;
    pthread_t thread;
    int ok;
    atomic_init(&job->refs, 2);
    atomic_init(&job->done, 0);
    if (pthread_attr_init(&attributes)) return 0;
    ok = !pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) &&
         !pthread_create(&thread, &attributes, resolve, job);
    pthread_attr_destroy(&attributes);
    return ok;
#endif
}
static int open_runtime(JavaNetwork *n)
{
    unsigned i;
    if(n->runtime)return 1;
    if(!net_start())return 0;
    n->runtime=calloc(1,sizeof(*n->runtime));if(!n->runtime){net_stop();return 0;}
    for(i=0;i<STREAMS;++i)n->runtime->stream[i].socket=NET_INVALID;
    return 1;
}
void java_net_close(JavaNetwork *n)
{
    JavaNetRuntime *r=n->runtime;unsigned i;if(!r)return;
    for(i=0;i<STREAMS;++i)if(r->stream[i].socket!=NET_INVALID)net_socket_close(r->stream[i].socket);
    /* A slow resolver owns its own job/ref/WSA lifetime, not this runtime or
       guest RAM. Reset/close never waits for DNS or accepts a stale result. */
    dns_release(r->dns);free(r);n->runtime=NULL;n->next_poll=0;net_stop();
}
static Stream *stream(JavaNetwork *n,unsigned handle)
{
    unsigned i;if(n->runtime)for(i=0;i<STREAMS;++i){Stream *s=&n->runtime->stream[i];
        if(s->socket!=NET_INVALID&&s->handle==handle)return s;}
    return NULL;
}
static int notify(Cellular *cell,uint32_t address,unsigned event,unsigned handle)
{
    uint8_t args[8];unsigned i;if(cell->count==CELL_EVENTS)return 0;
    for(i=0;i<4;++i){args[i]=(uint8_t)(event>>(8*i));args[i+4]=(uint8_t)(handle>>(8*i));}
    return cellular_queue(cell,CELL_CALLBACK,address,args,sizeof(args));
}
static void flush(Stream *s)
{
    if(s->connecting||!s->tx_count)return;
    {unsigned count=TX_BYTES-s->tx_head;int sent;
        if(count>s->tx_count)count=s->tx_count;
        sent=net_send(s->socket,(const char *)s->tx+s->tx_head,(int)count);
        if(sent>0){s->tx_head=(s->tx_head+(unsigned)sent)%TX_BYTES;s->tx_count-=(unsigned)sent;}
        else if(sent<0&&!net_would_block())s->event=0x223;
    }
}
void java_net_advance(JavaNetwork *n,Cellular *cell,Arm7 *cpu,int enabled)
{
    JavaNetRuntime *r=n->runtime;unsigned i;
    if(!r)return;
    if(!enabled){java_net_close(n);return;}
    if(cpu->cycles<n->next_poll)return;
    n->next_poll=cpu->cycles+65536;
    if(r->dns&&!r->dns_notified&&dns_done(r->dns)){
        if(notify(cell,n->entry[JN_NOTIFY],0x220,0))r->dns_notified=1;
    }
    for(i=0;i<STREAMS;++i){Stream *s=&r->stream[i];
        if(s->socket==NET_INVALID)continue;
        if(s->connecting&&net_ready(s->socket,1)){
            int error=0;NetAddrLen len=sizeof(error);
            if(getsockopt(s->socket,SOL_SOCKET,SO_ERROR,(char *)&error,&len))error=net_error();
            s->connecting=0;s->event=error?0x223:0x222;
        }
        flush(s);
        if(s->read_wait&&!s->event&&!s->connecting&&net_ready(s->socket,0))s->event=0x1fa;
        if(s->event&&notify(cell,n->entry[JN_NOTIFY],s->event,s->handle)){
            if(s->event==0x1fa)s->read_wait=0;
            s->event=0;
        }
    }
}
int java_net_hle(JavaNetwork *n,Cellular *cell,Arm7 *c,int enabled)
{
    unsigned op = java_net_operation(n, c);
    unsigned a = c->r[0], b = c->r[1], length = c->r[2];
    uint32_t err = c->r[3];
    Stream *s;

    (void)cell;
    if (op == JN_COUNT)
        return 0;
    if (op == JN_SHUTDOWN) {
        java_net_close(n);
        return returned(c, 0);
    }
    if (!enabled || !open_runtime(n))
        return java_net_unavailable(c, op);
    if(op==JN_OPEN)return returned(c,0);
    if(op==JN_LOOKUP){JavaNetRuntime *r=n->runtime;unsigned i;char name[256]={0};int terminated=0;
        if(!memory(c->bus,b,16,1)||!memory(c->bus,err,2,1))return error_return(c,err,0x13,UINT32_MAX);
        for(i=0;i<sizeof(name);++i){uint32_t ch;if(!bus_peek(c->bus,a+i,1,&ch))break;
            name[i]=(char)ch;if(!ch){terminated=1;break;}}
        if(!i||!terminated)return error_return(c,err,0x13,UINT32_MAX);
        if(length){DnsJob *job;
            /* The guest API has only one result slot. Keep the active lookup
               until it completes instead of spawning a worker for each retry. */
            if(r->dns&&!dns_done(r->dns))return error_return(c,err,0x19,UINT32_MAX);
            dns_release(r->dns);r->dns=NULL;r->dns_notified=0;
            job=calloc(1,sizeof(*job));if(!job)return error_return(c,err,0x7c,UINT32_MAX);
            memcpy(job->host,name,i+1);
            if(!start_resolver(job)){free(job);return error_return(c,err,0x7c,UINT32_MAX);}
            r->dns=job;++n->lookups;n->next_poll=0;
            n->error[0]=0;if(c->bus->log)fprintf(c->bus->log,"Java DNS request host=%s\n",name);
        }else if(r->dns&&!strcmp(r->dns->host,name)&&dns_done(r->dns)){
            if(r->dns->success){for(i=0;i<16;++i)bus_write(c->bus,b+i,1,(uint8_t)r->dns->address[i]);
                if(c->bus->log)fprintf(c->bus->log,"Java DNS reply host=%s address=%s\n",name,r->dns->address);
                return error_return(c,err,0,0);}
            snprintf(n->error,sizeof(n->error),"Java DNS could not resolve %.100s",name);
        }
        return error_return(c,err,0x19,UINT32_MAX);
    }
    if(op==JN_SOCKET){unsigned i;JavaNetRuntime *r=n->runtime;
        if(a||b||length!=6||!memory(c->bus,err,2,1))return error_return(c,err,0x6a,UINT32_MAX);
        for(i=0;i<STREAMS;++i)if(r->stream[i].socket==NET_INVALID)break;
        if(i==STREAMS)return error_return(c,err,0x7d,UINT32_MAX);
        s=&r->stream[i];memset(s,0,sizeof(*s));
        s->socket=net_socket(SOCK_STREAM,IPPROTO_TCP);
        if(s->socket==NET_INVALID)return error_return(c,err,0x7d,UINT32_MAX);
        s->handle=0x100+(++n->serial%0x7e00);return error_return(c,err,0,s->handle);
    }
    s=stream(n,a);if(!s){uint32_t pointer=op==JN_CLOSE||op==JN_AVAILABLE?b:
            op==JN_CONNECT||op==JN_BIND||op==JN_READ||op==JN_WRITE?err:0;
        return error_return(c,pointer,0x13,UINT32_MAX);}
    if(op==JN_CLOSE){net_socket_close(s->socket);s->socket=NET_INVALID;return error_return(c,b,0,0);}
    if(op==JN_CONNECT||op==JN_BIND){struct sockaddr_in address={0};int result;
        if(length!=16||!memory(c->bus,b,16,0))return error_return(c,err,0x46,UINT32_MAX);
        address.sin_family=AF_INET;address.sin_port=(uint16_t)bus_read(c->bus,b+2,2);
        address.sin_addr.s_addr=bus_read(c->bus,b+4,4);
        if(op==JN_BIND){result=bind(s->socket,(struct sockaddr *)&address,sizeof(address));
            return error_return(c,err,result?0x47:0,result?UINT32_MAX:0);}
        result=connect(s->socket,(struct sockaddr *)&address,sizeof(address));
        if(result<0&&!net_connect_pending())return error_return(c,err,0x47,UINT32_MAX);
        s->connecting=result<0;s->event=s->connecting?0:0x222;
        n->next_poll=0;++n->connects;return error_return(c,err,0,0);
    }
    if(op==JN_AVAILABLE){unsigned available=0;
        if(!memory(c->bus,b,2,1)||!net_readable_bytes(s->socket,&available))return returned(c,UINT32_MAX);
        bus_write(c->bus,b,2,0);return returned(c,available>32767?32767:available);
    }
    if(op==JN_LOCAL_IP||op==JN_REMOTE_IP||op==JN_LOCAL_PORT||op==JN_REMOTE_PORT){
        struct sockaddr_in address={0};NetAddrLen bytes=sizeof(address);int result;
        unsigned width=op==JN_LOCAL_IP||op==JN_REMOTE_IP?4:2;
        if(!memory(c->bus,b,width,1))return returned(c,UINT32_MAX);
        result=op==JN_LOCAL_IP||op==JN_LOCAL_PORT?getsockname(s->socket,(struct sockaddr *)&address,&bytes):
            getpeername(s->socket,(struct sockaddr *)&address,&bytes);
        if(result)return returned(c,UINT32_MAX);
        bus_write(c->bus,b,width,width==4?address.sin_addr.s_addr:address.sin_port);return returned(c,0);
    }
    if(op==JN_READ||op==JN_WRITE){unsigned i;
        if(!length||length>32767||!memory(c->bus,b,length,op==JN_READ)||!memory(c->bus,err,2,1))
            return error_return(c,err,0x13,UINT32_MAX);
        if(op==JN_WRITE){if(length>TX_BYTES-s->tx_count)return error_return(c,err,0x19,UINT32_MAX);
            for(i=0;i<length;++i)s->tx[(s->tx_head+s->tx_count+i)%TX_BYTES]=(uint8_t)bus_read(c->bus,b+i,1);
            s->tx_count+=length;n->sent+=length;flush(s);n->next_poll=0;return error_return(c,err,0,length);
        }else {uint8_t bytes[32767];int got=(int)recv(s->socket,(char *)bytes,(int)length,0);
            if(got>0){for(i=0;i<(unsigned)got;++i)bus_write(c->bus,b+i,1,bytes[i]);n->received+=(unsigned)got;return error_return(c,err,0,(uint32_t)got);}
            if(got==0)return error_return(c,err,0x4d,UINT32_MAX);
            if(net_would_block()){s->read_wait=1;n->next_poll=0;return error_return(c,err,0x14,0);}
            return error_return(c,err,0x47,UINT32_MAX);
        }
    }
    return 0;
}
