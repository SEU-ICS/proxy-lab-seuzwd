/* proxy.c
 *
 * 完整实现：多线程 + LRU 缓存（MAX_CACHE_SIZE = 1MiB, MAX_OBJECT_SIZE = 100KiB）
 *
 * 依赖 handout 提供的 csapp.h / csapp.c (Open_listenfd, Open_clientfd, Rio_*)
 */

#include "csapp.h"
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>

#define MAX_CACHE_SIZE 1049000    /* 1 MiB approx */
#define MAX_OBJECT_SIZE 102400    /* 100 KiB */
#define CACHE_BLOCK_COUNT 100     /* upper cap on number of blocks (for safety) */

static const char *user_agent_hdr = 
"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
"Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct cache_block {
    char *uri;                  /* NULL if unused */
    char *buf;                  /* pointer to stored object */
    int size;                   /* object size in bytes */
    struct cache_block *prev;   /* LRU doubly-linked list */
    struct cache_block *next;
} cache_block;

/* Cache head is most-recently-used, tail is least-recently-used */
static cache_block *cache_head = NULL;
static cache_block *cache_tail = NULL;
static int cache_used = 0;      /* total bytes used in cache */
static pthread_rwlock_t cache_rwlock; /* allow multiple readers concurrently */

void cache_init();
void cache_deinit();
int cache_lookup_and_write(const char *uri, int clientfd);
void cache_store(const char *uri, char *buf, int size);

/* proxy helpers */
void *thread(void *vargp);
void doit(int connfd);
void parse_uri(const char *uri, char *hostname, char *path, int *port);
void build_requesthdrs(rio_t *client_rio, char *hostname, char *path, int port, char *out_hdr);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* Ignore SIGPIPE so write to closed socket doesn't kill process */
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

/* process a single request */
void doit(int connfd) {
    int clientfd;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;
    rio_t rio;

    /* Read request line */
    Rio_readinitb(&rio, connfd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) return;

    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        clienterror(connfd, buf, "400", "Bad Request", "Can't parse request line");
        return;
    }

    if (strcasecmp(method, "GET")) {
        clienterror(connfd, method, "501", "Not Implemented", "Proxy only implements GET");
        return;
    }

    /* If requested resource is in cache, serve from cache */
    if (cache_lookup_and_write(uri, connfd) == 0) {
        /* cache hit and served */
        return;
    }

    /* parse URI */
    parse_uri(uri, hostname, path, &port);

    /* build request headers to send to server */
    char requesthdrs[MAXLINE * 8];
    build_requesthdrs(&rio, hostname, path, port, requesthdrs);

    /* connect to server */
    clientfd = Open_clientfd(hostname, port);
    if (clientfd < 0) {
        clienterror(connfd, hostname, "502", "Bad Gateway", "Can't connect to end server");
        return;
    }

    rio_t server_rio;
    Rio_readinitb(&server_rio, clientfd);

    /* send request to server */
    if (Rio_writen(clientfd, requesthdrs, strlen(requesthdrs)) < 0) {
        Close(clientfd);
        return;
    }

    /* read response from server and forward to client, and possibly cache */
    char buf2[MAXLINE];
    int n;
    int total_size = 0;
    char *object_buf = Malloc(MAX_OBJECT_SIZE); /* temp buffer to assemble cached object */
    int can_cache = 1;

    while ((n = Rio_readnb(&server_rio, buf2, MAXLINE)) > 0) {
        /* write to client */
        if (Rio_writen(connfd, buf2, n) < 0) {
            /* client closed, stop */
            break;
        }
        /* attempt to accumulate for cache */
        if (can_cache) {
            if (total_size + n > MAX_OBJECT_SIZE) {
                can_cache = 0; /* too big to cache */
            } else {
                memcpy(object_buf + total_size, buf2, n);
                total_size += n;
            }
        }
    }

    /* if object fits and we got full response, store in cache */
    if (can_cache && total_size > 0) {
        /* copy uri and buf into cache_store (it will duplicate memory) */
        cache_store(uri, object_buf, total_size);
    }
    Free(object_buf);
    Close(clientfd);
}

/* ---------------------- URI parsing --------------------------
 * parse_uri:
 *  input: uri like http://host:port/path or http://host/path or http://host
 *  output: hostname, path (starting with '/'), and port number (default 80)
 * ------------------------------------------------------------ */
void parse_uri(const char *uri, char *hostname, char *path, int *port) {
    *port = 80;
    const char *hostbegin;
    const char *pathbegin;
    const char *portptr;
    char hostbuf[MAXLINE];
    memset(hostbuf, 0, sizeof(hostbuf));

    if (strncasecmp(uri, "http://", 7) == 0) {
        hostbegin = uri + 7;
    } else {
        hostbegin = uri;
    }

    pathbegin = strchr(hostbegin, '/');
    if (pathbegin) {
        strncpy(hostbuf, hostbegin, pathbegin - hostbegin);
        snprintf(path, MAXLINE, "%s", pathbegin);
    } else {
        snprintf(hostbuf, MAXLINE, "%s", hostbegin);
        snprintf(path, MAXLINE, "/");
    }

    /* check for port */
    portptr = strchr(hostbuf, ':');
    if (portptr) {
        char hostonly[MAXLINE];
        strncpy(hostonly, hostbuf, portptr - hostbuf);
        hostonly[portptr - hostbuf] = '\0';
        strcpy(hostname, hostonly);
        *port = atoi(portptr + 1);
        if (*port == 0) *port = 80;
    } else {
        strcpy(hostname, hostbuf);
    }
}

/* ---------------- build request headers to server -----------------
 * Uses: client's rio to forward any extra headers (except Connection/Proxy-Connection/User-Agent)
 * Always include Host, User-Agent, Connection: close, Proxy-Connection: close
 * ------------------------------------------------------------------ */
void build_requesthdrs(rio_t *client_rio, char *hostname, char *path, int port, char *out_hdr) {
    char buf[MAXLINE], host_hdr[MAXLINE];
    sprintf(out_hdr, "GET %s HTTP/1.0\r\n", path);
    /* Host header */
    if (strchr(hostname, ':') == NULL) {
        /* hostname already without port; if port != 80, append port */
        if (port != 80)
            sprintf(host_hdr, "Host: %s:%d\r\n", hostname, port);
        else
            sprintf(host_hdr, "Host: %s\r\n", hostname);
    } else {
        sprintf(host_hdr, "Host: %s\r\n", hostname);
    }
    strcat(out_hdr, host_hdr);

    /* User-Agent */
    strcat(out_hdr, user_agent_hdr);

    /* Connection headers */
    strcat(out_hdr, "Connection: close\r\n");
    strcat(out_hdr, "Proxy-Connection: close\r\n");

    /* Copy other headers from client except those we replaced */
    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) break; /* end of headers */
        /* ignore existing Host, User-Agent, Connection, Proxy-Connection */
        if (strncasecmp(buf, "Host:", 5) == 0) continue;
        if (strncasecmp(buf, "User-Agent:", 11) == 0) continue;
        if (strncasecmp(buf, "Connection:", 11) == 0) continue;
        if (strncasecmp(buf, "Proxy-Connection:", 17) == 0) continue;
        strcat(out_hdr, buf);
    }
    strcat(out_hdr, "\r\n"); /* end of headers */
}

/* -------------------- error helper ------------------ */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body + strlen(body), "<body bgcolor=\"ffffff\">\r\n");
    sprintf(body + strlen(body), "%s: %s\r\n", errnum, shortmsg);
    sprintf(body + strlen(body), "<p>%s: %s\r\n", longmsg, cause);
    sprintf(body + strlen(body), "<hr><em>Simple Proxy</em>\r\n");
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %lu\r\n\r\n", strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/* ----------------------- CACHE IMPLEMENTATION -----------------------
 * Simple doubly-linked LRU list of cache_block structures.
 * Protected by a single pthread_rwlock_t allowing multiple concurrent readers,
 * but writers (insert/evict/move) take write lock.
 * ------------------------------------------------------------------- */

void cache_init() {
    pthread_rwlock_init(&cache_rwlock, NULL);
    cache_head = cache_tail = NULL;
    cache_used = 0;
}

void cache_deinit() {
    pthread_rwlock_wrlock(&cache_rwlock);
    cache_block *p = cache_head;
    while (p) {
        cache_block *next = p->next;
        Free(p->uri);
        Free(p->buf);
        Free(p);
        p = next;
    }
    cache_head = cache_tail = NULL;
    cache_used = 0;
    pthread_rwlock_unlock(&cache_rwlock);
    pthread_rwlock_destroy(&cache_rwlock);
}

/* move block to head (MRU) -- caller must hold write lock */
static void move_to_head(cache_block *b) {
    if (b == cache_head) return;
    /* unlink */
    if (b->prev) b->prev->next = b->next;
    if (b->next) b->next->prev = b->prev;
    if (b == cache_tail) cache_tail = b->prev;

    /* insert at head */
    b->prev = NULL;
    b->next = cache_head;
    if (cache_head) cache_head->prev = b;
    cache_head = b;
    if (!cache_tail) cache_tail = cache_head;
}

/* remove tail (LRU) -- caller must hold write lock */
static void evict_tail() {
    if (!cache_tail) return;
    cache_block *t = cache_tail;
    if (t->prev) {
        cache_tail = t->prev;
        cache_tail->next = NULL;
    } else {
        cache_head = cache_tail = NULL;
    }
    cache_used -= t->size;
    Free(t->uri);
    Free(t->buf);
    Free(t);
}

/* lookup: if found, write to clientfd and update LRU.
 * returns 0 on hit-and-served, -1 on miss or error.
 */
int cache_lookup_and_write(const char *uri, int clientfd) {
    int served = -1;
    pthread_rwlock_rdlock(&cache_rwlock);
    cache_block *p = cache_head;
    while (p) {
        if (p->uri && strcmp(p->uri, uri) == 0) {
            /* found -> need to serve. We want to move this to head (MRU).
             * Moving requires write lock. We'll copy the data pointer & size under read lock,
             * then upgrade to write lock to move list (simple approach: release read lock, take write lock).
             */
            int objsize = p->size;
            char *localbuf = Malloc(objsize);
            memcpy(localbuf, p->buf, objsize);
            pthread_rwlock_unlock(&cache_rwlock);

            /* write to client */
            if (Rio_writen(clientfd, localbuf, objsize) >= 0) {
                served = 0;
            }
            Free(localbuf);

            /* now promote to write lock to update LRU position */
            pthread_rwlock_wrlock(&cache_rwlock);
            /* find again (could be changed) */
            cache_block *q = cache_head;
            while (q) {
                if (q->uri && strcmp(q->uri, uri) == 0) {
                    move_to_head(q);
                    break;
                }
                q = q->next;
            }
            pthread_rwlock_unlock(&cache_rwlock);
            return served;
        }
        p = p->next;
    }
    pthread_rwlock_unlock(&cache_rwlock);
    return -1; /* miss */
}

/* store: insert new object; evict as necessary. Makes copies of uri and buf */
void cache_store(const char *uri, char *buf, int size) {
    if (size > MAX_OBJECT_SIZE) return; /* cannot cache too-large objects */

    /* allocate new block */
    cache_block *b = Malloc(sizeof(cache_block));
    b->uri = Malloc(strlen(uri) + 1);
    strcpy(b->uri, uri);
    b->buf = Malloc(size);
    memcpy(b->buf, buf, size);
    b->size = size;
    b->prev = b->next = NULL;

    pthread_rwlock_wrlock(&cache_rwlock);

    /* Check if someone else cached the same URI already -> drop ours */
    cache_block *p = cache_head;
    while (p) {
        if (p->uri && strcmp(p->uri, uri) == 0) {
            /* someone else cached it; free new and update LRU */
            Free(b->uri);
            Free(b->buf);
            Free(b);
            /* move existing to head */
            move_to_head(p);
            pthread_rwlock_unlock(&cache_rwlock);
            return;
        }
        p = p->next;
    }

    /* make space if needed */
    while (cache_used + size > MAX_CACHE_SIZE) {
        if (!cache_tail) break;
        evict_tail();
    }

    /* insert at head */
    b->next = cache_head;
    if (cache_head) cache_head->prev = b;
    cache_head = b;
    if (!cache_tail) cache_tail = b;

    cache_used += size;
    pthread_rwlock_unlock(&cache_rwlock);
}
