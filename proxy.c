/* proxy.c
 *
 * 完整实现：Basic + Concurrency + Cache (LRU-ish)
 * 依赖 csapp.h / csapp.c (handout)
 */

/*
 * proxy.c - CS:APP Proxy Lab
 * C89 style: all variable declarations at beginning of block
 */
//
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* cache node struct */
typedef struct cache_block {
    char url[MAXLINE];
    char data[MAX_OBJECT_SIZE];
    int size;
    struct cache_block *prev;
    struct cache_block *next;
} cache_block;

typedef struct {
    cache_block *head;
    cache_block *tail;
    int total_size;
    sem_t mutex;
} cache_list;

cache_list cache;

/* Function prototypes */
void *thread(void *vargp);
void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_requesthdrs(rio_t *client_rio, char *req_hdrs, char *hostname);
int cache_find(char *url, int connfd);
void cache_insert(char *url, char *buf, int size);
void cache_init();

/* ---------------- Main ---------------- */
int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    cache_init();

    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }
}

/* ---------------- Thread ---------------- */
void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

/* ---------------- doit ---------------- */
void doit(int connfd) {
    int clientfd;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;
    rio_t rio, server_rio;
    char req_hdrs[MAXLINE], response_buf[MAX_OBJECT_SIZE];
    int n, total_size;

    Rio_readinitb(&rio, connfd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;

    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET")) {
        printf("Proxy only supports GET\n");
        return;
    }

    if (cache_find(uri, connfd))
        return;

    parse_uri(uri, hostname, path, &port);
    build_requesthdrs(&rio, req_hdrs, hostname);

    clientfd = Open_clientfd(hostname, port);
    if (clientfd < 0) return;

    Rio_readinitb(&server_rio, clientfd);
    Rio_writen(clientfd, req_hdrs, strlen(req_hdrs));

    total_size = 0;
    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(connfd, buf, n);
        if (total_size + n < MAX_OBJECT_SIZE) {
            memcpy(response_buf + total_size, buf, n);
        }
        total_size += n;
    }

    if (total_size < MAX_OBJECT_SIZE)
        cache_insert(uri, response_buf, total_size);

    Close(clientfd);
}

/* ---------------- parse_uri ---------------- */
void parse_uri(char *uri, char *hostname, char *path, int *port) {
    char *hostbegin;
    char *portpos;
    char *pathpos;

    *port = 80;
    hostbegin = strstr(uri, "//");
    if (hostbegin != NULL)
        hostbegin += 2;
    else
        hostbegin = uri;

    portpos = strchr(hostbegin, ':');
    if (portpos != NULL) {
        *port = atoi(portpos + 1);
        *portpos = '\0';
    }

    pathpos = strchr(hostbegin, '/');
    if (pathpos != NULL) {
        strcpy(path, pathpos);
        *pathpos = '\0';
    } else {
        strcpy(path, "/");
    }

    strcpy(hostname, hostbegin);
}

/* ---------------- build_requesthdrs ---------------- */
void build_requesthdrs(rio_t *client_rio, char *req_hdrs, char *hostname) {
    char buf[MAXLINE];
    int has_host = 0;

    sprintf(req_hdrs, "GET %s HTTP/1.0\r\n", "/");
    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) break;
        if (!strncasecmp(buf, "Host:", 5)) has_host = 1;
        if (strncasecmp(buf, "Connection:", 11)
            && strncasecmp(buf, "Proxy-Connection:", 17)
            && strncasecmp(buf, "User-Agent:", 11)) {
            strcat(req_hdrs, buf);
        }
    }
    if (!has_host)
        sprintf(req_hdrs + strlen(req_hdrs), "Host: %s\r\n", hostname);

    strcat(req_hdrs, "Connection: close\r\n");
    strcat(req_hdrs, "Proxy-Connection: close\r\n");
    strcat(req_hdrs, "User-Agent: Mozilla/5.0\r\n\r\n");
}

/* ---------------- Cache Implementation ---------------- */
void cache_init() {
    cache.head = NULL;
    cache.tail = NULL;
    cache.total_size = 0;
    Sem_init(&cache.mutex, 0, 1);
}

int cache_find(char *url, int connfd) {
    cache_block *p;
    P(&cache.mutex);
    p = cache.head;
    while (p) {
        if (strcmp(url, p->url) == 0) {
            Rio_writen(connfd, p->data, p->size);
            V(&cache.mutex);
            return 1;
        }
        p = p->next;
    }
    V(&cache.mutex);
    return 0;
}

void cache_insert(char *url, char *buf, int size) {
    cache_block *new_block;

    if (size > MAX_OBJECT_SIZE) return;

    P(&cache.mutex);
    while (cache.total_size + size > MAX_CACHE_SIZE) {
        cache_block *victim = cache.tail;
        if (victim == NULL) break;
        if (victim->prev)
            victim->prev->next = NULL;
        cache.tail = victim->prev;
        cache.total_size -= victim->size;
        Free(victim);
    }

    new_block = Malloc(sizeof(cache_block));
    strcpy(new_block->url, url);
    memcpy(new_block->data, buf, size);
    new_block->size = size;
    new_block->prev = NULL;
    new_block->next = cache.head;

    if (cache.head)
        cache.head->prev = new_block;
    cache.head = new_block;
    if (cache.tail == NULL)
        cache.tail = new_block;

    cache.total_size += size;
    V(&cache.mutex);
}
