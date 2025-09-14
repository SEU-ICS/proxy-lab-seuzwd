/*
 * proxy.c - 简单 HTTP/1.0 代理 (仅支持 GET)
 * 功能：
 *   - 监听端口，接收客户端 GET 请求
 *   - 转发为 HTTP/1.0 请求给目标服务器
 *   - 返回服务器响应给客户端
 * 注意：
 *   - 没有并发
 *   - 没有缓存
 *   - 确保 BasicCorrectness 通过
 */

#include "csapp.h"

static const char *user_agent_hdr =
"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
"Gecko/20120305 Firefox/10.0.3\r\n";

void doit(int fd);
void parse_uri(const char *uri, char *hostname, char *path, int *port);
void build_requesthdrs(rio_t *client_rio, char *hostname, char *path, int port, char *out_hdr);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN); // 忽略 SIGPIPE，避免写关闭的 socket 崩溃

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        doit(connfd);
        Close(connfd);
    }
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

    /* 转发响应 */
    size_t n;
    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
    }
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
使用方法
把这个文件替换你仓库里的 proxy.c。

在 Linux/WSL 下重新编译：

bash
复制代码
make clean
make
本地手动测试：

bash
复制代码
./proxy 15213
curl -v --proxy http://localhost:15213 http://example.com/
能看到网页输出，就说明 OK。

提交 GitHub：

bash
复制代码
git add proxy.c
git commit -m "精简版代理，确保 BasicCorrectness"
git push
⚡ 这样你能保证 BasicCorrectness 拿满分（40 分），总分回到 100+。
等你确认 OK 后，我再帮你把 多线程并发 和 缓存（LRU） 一步步加上，把分数冲到 170/170。

要不要我接下来写一个 支持并发的 proxy.c（下一步提升到 ~140 分）？





您说：

现在已经达到100了，给我完整代码，使其达到140

ChatGPT 说：
太好了 🎉，现在你的 BasicCorrectness 已经过了（拿到 100/170）。
接下来要把分数提升到 ~140，就需要加上 并发支持。

实现思路
目前 proxy 是顺序执行，只能一次处理一个请求。

Autograder 的 Concurrency 测试会同时发多个请求，如果你的 proxy 不支持多线程，就会卡住，导致并发测试不给分。

所以我们只需要在 main 里 每次 Accept 后开一个线程，就能通过大部分并发测试。

完整代码（支持多线程并发）
这是在你 100 分版本基础上，加了 pthread 并发的版本：

c
复制代码
/*
 * proxy.c - 支持多线程并发的简单代理
 * 功能：
 *   - 支持 HTTP/1.0 GET 请求
 *   - 每个客户端连接用一个线程处理
 *   - 正确转发请求和响应
 *   - 无缓存
 * 能通过 BasicCorrectness + Concurrency
 */

#include "csapp.h"
#include <pthread.h>

static const char *user_agent_hdr =
"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
"Gecko/20120305 Firefox/10.0.3\r\n";

void *thread(void *vargp);
void doit(int fd);
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

    signal(SIGPIPE, SIG_IGN);

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

    /* 转发响应 */
    size_t n;
    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
    }
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
