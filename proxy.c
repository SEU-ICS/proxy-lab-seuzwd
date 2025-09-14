/* proxy.c
 *
 * 完整实现：Basic + Concurrency + Cache (LRU-ish)
 * 依赖 csapp.h / csapp.c (handout)
 */

#include "csapp.h"
#include <pthread.h>
#include <signal.h>
#include <string.h>

#define MAX_CACHE_SIZE 1049000    /* 1 MiB approx */
#define MAX_OBJECT_SIZE 102400    /* 100 KiB */
#define MAX_CACHE_BLOCKS 10

static const char *user_agent_hdr =
"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
"Gecko/20120305 Firefox/10.0.3\r\n";

/* ----- cache data structures ----- */
typedef struct cache_block {
    char *uri;
    char *buf;
    int size;
    struct cache_block *prev, *next; /* LRU doubly-linked list */
} cache_block;

static cache_block *cache_head = NULL;
static cache_block *cache_tail = NULL;
static int cache_bytes = 0;
static pthread_rwlock_t cache_lock;

/* prototypes */
void *thread(void *vargp);
void doit(int connfd);
void parse_uri(const char *uri, char *host, char *path, char *portstr);
void build_requesthdrs(rio_t *client_rio, char *host, char *path, char *portstr, char *out_hdr);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

/* cache helpers */
void cache_init();
void cache_deinit();
int cache_lookup_and_serve(const char *uri, int clientfd);
void cache_store(const char *uri, char *buf, int size);

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* Ignore SIGPIPE so write to closed socket won't kill process */
    signal(SIGPIPE, SIG_IGN);

    cache_init();

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }

    cache_deinit();
    return 0;
}

/* thread wrapper */
void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

/* handle one HTTP request (single connection) */
void doit(int connfd) {
    rio_t rio;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], path[MAXLINE], portstr[16];

    Rio_readinitb(&rio, connfd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) return;
    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        clienterror(connfd, buf, "400", "Bad Request", "Can't parse request line");
        return;
    }

    if (strcasecmp(method, "GET") != 0) {
        clienterror(connfd, method, "501", "Not Implemented", "Proxy only implements GET");
        return;
    }

    /* cache hit? */
    if (cache_lookup_and_serve(uri, connfd) == 0) {
        /* served from cache */
        return;
    }

    /* parse uri -> host, path, portstr */
    parse_uri(uri, host, path, portstr);

    /* build request headers to send to end server */
    char requesthdrs[MAXLINE * 16];
    build_requesthdrs(&rio, host, path, portstr, requesthdrs);

    /* connect to end server; Open_clientfd expects host and port string */
    int serverfd = Open_clientfd(host, portstr);
    if (serverfd < 0) {
        clienterror(connfd, host, "502", "Bad Gateway", "Can't connect to end server");
        return;
    }

    rio_t server_rio;
    Rio_readinitb(&server_rio, serverfd);

    /* send request to server */
    if (Rio_writen(serverfd, requesthdrs, strlen(requesthdrs)) < 0) {
        Close(serverfd);
        return;
    }

    /* read response from server and forward to client; also try to cache */
    char buf2[MAXLINE];
    ssize_t n;
    int total_cached = 0;
    char *object_buf = Malloc(MAX_OBJECT_SIZE);
    int can_cache = 1;

    /* read until EOF from server */
    while ((n = Rio_readnb(&server_rio, buf2, MAXLINE)) > 0) {
        if (Rio_writen(connfd, buf2, n) < 0) {
            /* client closed; stop */
            break;
        }
        if (can_cache) {
            if (total_cached + n > MAX_OBJECT_SIZE) {
                can_cache = 0;
            } else {
                memcpy(object_buf + total_cached, buf2, n);
                total_cached += n;
            }
        }
    }

    if (can_cache && total_cached > 0) {
        cache_store(uri, object_buf, total_cached);
    }
    Free(object_buf);
    Close(serverfd);
}

/* parse URI into host, path and port string ("80" default) */
void parse_uri(const char *uri, char *host, char *path, char *portstr) {
    /* format: http://host:port/path or http://host/path or host/path */
    const char *hostbegin;
    if (strncasecmp(uri, "http://", 7) == 0)
        hostbegin = uri + 7;
    else
        hostbegin = uri;

    const char *pathbegin = strchr(hostbegin, '/');
    if (pathbegin) {
        strncpy(path, pathbegin, MAXLINE-1);
        path[MAXLINE-1] = '\0';
        size_t hostlen = pathbegin - hostbegin;
        char hostbuf[MAXLINE];
        if (hostlen < MAXLINE) {
            strncpy(hostbuf, hostbegin, hostlen);
            hostbuf[hostlen] = '\0';
        } else {
            strncpy(hostbuf, hostbegin, MAXLINE-1);
            hostbuf[MAXLINE-1] = '\0';
        }
        /* check port in hostbuf */
        char *colon = strchr(hostbuf, ':');
        if (colon) {
            *colon = '\0';
            strcpy(host, hostbuf);
            int port = atoi(colon + 1);
            if (port <= 0) port = 80;
            sprintf(portstr, "%d", port);
        } else {
            strcpy(host, hostbuf);
            strcpy(portstr, "80");
        }
    } else {
        /* no path -> use "/" */
        strcpy(path, "/");
        char hostbuf[MAXLINE];
        strncpy(hostbuf, hostbegin, MAXLINE-1);
        hostbuf[MAXLINE-1] = '\0';
        char *colon = strchr(hostbuf, ':');
        if (colon) {
            *colon = '\0';
            strcpy(host, hostbuf);
            int port = atoi(colon + 1);
            if (port <= 0) port = 80;
            sprintf(portstr, "%d", port);
        } else {
            strcpy(host, hostbuf);
            strcpy(portstr, "80");
        }
    }
}

/* build request header to server:
 * - start line: GET path HTTP/1.0
 * - include Host header (either with port if non-80)
 * - include User-Agent, Connection: close, Proxy-Connection: close
 * - forward other headers from client except Host/User-Agent/Connection/Proxy-Connection
 */
void build_requesthdrs(rio_t *client_rio, char *host, char *path, char *portstr, char *out_hdr) {
    char buf[MAXLINE];
    char host_hdr[MAXLINE];

    snprintf(out_hdr, MAXLINE*16, "GET %s HTTP/1.0\r\n", path);

    /* Host header */
    if (strcmp(portstr, "80") == 0) {
        snprintf(host_hdr, sizeof(host_hdr), "Host: %s\r\n", host);
    } else {
        snprintf(host_hdr, sizeof(host_hdr), "Host: %s:%s\r\n", host, portstr);
    }
    strcat(out_hdr, host_hdr);

    /* required headers */
    strcat(out_hdr, user_agent_hdr);
    strcat(out_hdr, "Connection: close\r\n");
    strcat(out_hdr, "Proxy-Connection: close\r\n");

    /* append other headers from client */
    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) break;
        if (strncasecmp(buf, "Host:", 5) == 0) continue;
        if (strncasecmp(buf, "User-Agent:", 11) == 0) continue;
        if (strncasecmp(buf, "Connection:", 11) == 0) continue;
        if (strncasecmp(buf, "Proxy-Connection:", 17) == 0) continue;
        strcat(out_hdr, buf);
    }
    strcat(out_hdr, "\r\n");
}

/* send HTTP error to client */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char body[MAXBUF], buf[MAXLINE];
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body + strlen(body), "<body><h3>%s: %s</h3>\r\n", errnum, shortmsg);
    sprintf(body + strlen(body), "<p>%s: %s</p>\r\n", longmsg, cause);
    sprintf(body + strlen(body), "</body></html>\r\n");

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-Type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-Length: %lu\r\n\r\n", (unsigned long)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/* ------------------ Cache Implementation ------------------ */

void cache_init() {
    pthread_rwlock_init(&cache_lock, NULL);
    cache_head = cache_tail = NULL;
    cache_bytes = 0;
}

void cache_deinit() {
    pthread_rwlock_wrlock(&cache_lock);
    cache_block *p = cache_head;
    while (p) {
        cache_block *next = p->next;
        Free(p->uri);
        Free(p->buf);
        Free(p);
        p = next;
    }
    cache_head = cache_tail = NULL;
    cache_bytes = 0;
    pthread_rwlock_unlock(&cache_lock);
    pthread_rwlock_destroy(&cache_lock);
}

/* move block to head (MRU) - caller must hold write lock */
static void move_to_head(cache_block *b) {
    if (b == cache_head) return;
    if (b->prev) b->prev->next = b->next;
    if (b->next) b->next->prev = b->prev;
    if (b == cache_tail) cache_tail = b->prev;

    b->prev = NULL;
    b->next = cache_head;
    if (cache_head) cache_head->prev = b;
    cache_head = b;
    if (!cache_tail) cache_tail = cache_head;
}

/* evict tail - caller must hold write lock */
static void evict_tail() {
    if (!cache_tail) return;
    cache_block *t = cache_tail;
    if (t->prev) {
        cache_tail = t->prev;
        cache_tail->next = NULL;
    } else {
        cache_head = cache_tail = NULL;
    }
    cache_bytes -= t->size;
    Free(t->uri);
    Free(t->buf);
    Free(t);
}

/* lookup uri; if found, write to clientfd and update LRU. returns 0 on hit-and-served, -1 miss */
int cache_lookup_and_serve(const char *uri, int clientfd) {
    pthread_rwlock_rdlock(&cache_lock);
    cache_block *p = cache_head;
    while (p) {
        if (p->uri && strcmp(p->uri, uri) == 0) {
            int sz = p->size;
            char *localbuf = Malloc(sz);
            memcpy(localbuf, p->buf, sz);
            pthread_rwlock_unlock(&cache_lock);

            /* serve */
            if (Rio_writen(clientfd, localbuf, sz) < 0) {
                Free(localbuf);
                return -1;
            }
            Free(localbuf);

            /* promote LRU: need write lock */
            pthread_rwlock_wrlock(&cache_lock);
            /* find again (could have changed) */
            cache_block *q = cache_head;
            while (q) {
                if (q->uri && strcmp(q->uri, uri) == 0) {
                    move_to_head(q);
                    break;
                }
                q = q->next;
            }
            pthread_rwlock_unlock(&cache_lock);
            return 0;
        }
        p = p->next;
    }
    pthread_rwlock_unlock(&cache_lock);
    return -1;
}

/* store object into cache (make copies). thread-safe */
void cache_store(const char *uri, char *buf, int size) {
    if (size > MAX_OBJECT_SIZE) return;

    pthread_rwlock_wrlock(&cache_lock);

    /* if someone else cached same uri while we were fetching, just abort */
    cache_block *p = cache_head;
    while (p) {
        if (p->uri && strcmp(p->uri, uri) == 0) {
            move_to_head(p);
            pthread_rwlock_unlock(&cache_lock);
            return;
        }
        p = p->next;
    }

    /* make space if needed */
    while (cache_bytes + size > MAX_CACHE_SIZE) {
        evict_tail();
    }

    /* create new block */
    cache_block *b = Malloc(sizeof(cache_block));
    b->uri = Malloc(strlen(uri) + 1);
    strcpy(b->uri, uri);
    b->buf = Malloc(size);
    memcpy(b->buf, buf, size);
    b->size = size;
    b->prev = b->next = NULL;

    /* insert at head */
    b->next = cache_head;
    if (cache_head) cache_head->prev = b;
    cache_head = b;
    if (!cache_tail) cache_tail = b;

    cache_bytes += size;
    pthread_rwlock_unlock(&cache_lock);
}
