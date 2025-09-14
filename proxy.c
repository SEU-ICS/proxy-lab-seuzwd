/*
 * proxy.c - 完整的多线程 + LRU 缓存代理
 * 功能：
 *   - 支持 HTTP/1.0 GET 请求
 *   - 每个客户端连接用一个线程处理
 *   - 正确转发请求和响应
 *   - 支持缓存，LRU 淘汰策略
 */

#include "csapp.h"
#include <pthread.h>
#include <string.h>

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_CNT 10

/* 缓存块结构 */
typedef struct {
    char uri[MAXLINE];
    char obj[MAX_OBJECT_SIZE];
    int size;
    unsigned long timestamp;
} cache_block;

cache_block cache[CACHE_CNT];
pthread_mutex_t cache_lock;
unsigned long global_time = 0;

/* 固定的 UA 字符串 */
static const char *user_agent_hdr =
"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
"Gecko/20120305 Firefox/10.0.3\r\n";

void *thread(void *vargp);
void doit(int fd);
void parse_uri(const char *uri, char *hostname, char *path, int *port);
void build_requesthdrs(rio_t *client_rio, char *hostname, char *path, int port, char *out_hdr);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

/* 缓存函数 */
int cache_read(const char *uri, int fd);
void cache_write(const char *uri, char *buf, int size);

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&cache_lock, NULL);

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }
}

/* 线程函数 */
void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

/* 处理一个请求 */
void doit(int fd) {
    int serverfd;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;
    rio_t rio, server_rio;

    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE)) return;

    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented", "Proxy only implements GET");
        return;
    }

    /* 检查缓存 */
    if (cache_read(uri, fd)) return;

    parse_uri(uri, hostname, path, &port);

    /* 构建请求头 */
    char requesthdrs[MAXLINE * 10];
    build_requesthdrs(&rio, hostname, path, port, requesthdrs);

    /* 连接服务器 */
    serverfd = Open_clientfd(hostname, port);
    if (serverfd < 0) {
        clienterror(fd, hostname, "502", "Bad Gateway", "Cannot connect to end server");
        return;
    }

    Rio_readinitb(&server_rio, serverfd);
    Rio_writen(serverfd, requesthdrs, strlen(requesthdrs));

    /* 转发响应 + 写缓存 */
    size_t n, total_size = 0;
    char obj_buf[MAX_OBJECT_SIZE];
    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
        if (total_size + n < MAX_OBJECT_SIZE) {
            memcpy(obj_buf + total_size, buf, n);
            total_size += n;
        }
    }
    if (total_size > 0) cache_write(uri, obj_buf, total_size);

    Close(serverfd);
}

/* 解析 URI */
void parse_uri(const char *uri, char *hostname, char *path, int *port) {
    *port = 80;
    const char *hostbegin;
    const char *pathbegin;
    char hostbuf[MAXLINE];

    if (strncasecmp(uri, "http://", 7) == 0)
        hostbegin = uri + 7;
    else
        hostbegin = uri;

    pathbegin = strchr(hostbegin, '/');
    if (pathbegin) {
        strcpy(path, pathbegin);
        strncpy(hostbuf, hostbegin, pathbegin - hostbegin);
        hostbuf[pathbegin - hostbegin] = '\0';
    } else {
        strcpy(path, "/");
        strcpy(hostbuf, hostbegin);
    }

    char *portptr = strchr(hostbuf, ':');
    if (portptr) {
        *portptr = '\0';
        strcpy(hostname, hostbuf);
        *port = atoi(portptr + 1);
    } else {
        strcpy(hostname, hostbuf);
    }
}

/* 构造请求头 */
void build_requesthdrs(rio_t *client_rio, char *hostname, char *path, int port, char *out_hdr) {
    char buf[MAXLINE], host_hdr[MAXLINE];

    sprintf(out_hdr, "GET %s HTTP/1.0\r\n", path);

    /* Host */
    if (port != 80)
        sprintf(host_hdr, "Host: %s:%d\r\n", hostname, port);
    else
        sprintf(host_hdr, "Host: %s\r\n", hostname);
    strcat(out_hdr, host_hdr);

    /* 固定要求的头部 */
    strcat(out_hdr, user_agent_hdr);
    strcat(out_hdr, "Connection: close\r\n");
    strcat(out_hdr, "Proxy-Connection: close\r\n");

    /* 转发其他客户端头部（排除我们已加的） */
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

/* 错误响应 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body + strlen(body), "<body>%s: %s<br>%s<br></body></html>", errnum, shortmsg, longmsg);

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/* ========== 缓存实现 ========== */

/* 读缓存 */
int cache_read(const char *uri, int fd) {
    pthread_mutex_lock(&cache_lock);
    global_time++;
    for (int i = 0; i < CACHE_CNT; i++) {
        if (cache[i].size > 0 && !strcmp(uri, cache[i].uri)) {
            Rio_writen(fd, cache[i].obj, cache[i].size);
            cache[i].timestamp = global_time;
            pthread_mutex_unlock(&cache_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return 0;
}

/* 写缓存 */
void cache_write(const char *uri, char *buf, int size) {
    if (size > MAX_OBJECT_SIZE) return;

    pthread_mutex_lock(&cache_lock);
    global_time++;

    /* 找空位 */
    int victim = -1;
    for (int i = 0; i < CACHE_CNT; i++) {
        if (cache[i].size == 0) {
            victim = i;
            break;
        }
    }
    /* LRU 淘汰 */
    if (victim == -1) {
        unsigned long min_time = cache[0].timestamp;
        victim = 0;
        for (int i = 1; i < CACHE_CNT; i++) {
            if (cache[i].timestamp < min_time) {
                min_time = cache[i].timestamp;
                victim = i;
            }
        }
    }

    strcpy(cache[victim].uri, uri);
    memcpy(cache[victim].obj, buf, size);
    cache[victim].size = size;
    cache[victim].timestamp = global_time;

    pthread_mutex_unlock(&cache_lock);
}
