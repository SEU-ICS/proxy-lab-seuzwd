#include "csapp.h"
#include <pthread.h>
#include <string.h>

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

typedef struct {
    char uri[MAXLINE];
    char obj[MAX_OBJECT_SIZE];
    int size;
    int valid;
} cache_block;

cache_block cache[10];
pthread_rwlock_t lock;

void *thread(void *vargp);
void doit(int fd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
    int listenfd, *connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    pthread_rwlock_init(&lock, NULL);

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Malloc(sizeof(int));
        *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        pthread_create(&tid, NULL, thread, connfd);
    }
}

void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

void doit(int fd) {
    int clientfd, port;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    rio_t rio, server_rio;

    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE)) return;
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented", "Proxy does not implement this method");
        return;
    }

    parse_uri(uri, hostname, path, &port);

    char request[MAXLINE];
    sprintf(request, "GET %s HTTP/1.0\r\n", path);
    sprintf(request + strlen(request), "Host: %s\r\n", hostname);
    sprintf(request + strlen(request), "User-Agent: Mozilla/5.0\r\n");
    sprintf(request + strlen(request), "Connection: close\r\n");
    sprintf(request + strlen(request), "Proxy-Connection: close\r\n\r\n");

    clientfd = Open_clientfd(hostname, port);
    Rio_readinitb(&server_rio, clientfd);
    Rio_writen(clientfd, request, strlen(request));

    size_t n;
    char objbuf[MAX_OBJECT_SIZE];
    int total_size = 0;

    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
        if (total_size + n < MAX_OBJECT_SIZE) {
            memcpy(objbuf + total_size, buf, n);
            total_size += n;
        }
    }

    // 缓存（简单策略）
    if (total_size < MAX_OBJECT_SIZE) {
        pthread_rwlock_wrlock(&lock);
        strcpy(cache[0].uri, uri);
        memcpy(cache[0].obj, objbuf, total_size);
        cache[0].size = total_size;
        cache[0].valid = 1;
        pthread_rwlock_unlock(&lock);
    }

    Close(clientfd);
}

void parse_uri(char *uri, char *hostname, char *path, int *port) {
    char *hostbegin, *hostend, *pathbegin;
    *port = 80;

    hostbegin = strstr(uri, "//") ? strstr(uri, "//") + 2 : uri;
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin) {
        strcpy(path, pathbegin);
        *pathbegin = '\0';
    } else {
        strcpy(path, "/");
    }

    hostend = strchr(hostbegin, ':');
    if (hostend) {
        *hostend = '\0';
        sscanf(hostend + 1, "%d", port);
    }
    strcpy(hostname, hostbegin);
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body + strlen(body), "<body>%s: %s<br>%s<br></body></html>", errnum, shortmsg, longmsg);
    sprintf(buf, "HTTP/1.0 %s %s\r\nContent-type: text/html\r\nContent-length: %d\r\n\r\n",
            errnum, shortmsg, (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
