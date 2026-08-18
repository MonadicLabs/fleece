#define _DEFAULT_SOURCE  // usleep etc.
#define _POSIX_C_SOURCE 200809L

#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HEADER_CAP 8192
#define BODY_CAP 65536
#define FILE_CAP (4u * 1024u * 1024u)
#define HANDLER_CAP 16
#define SCRATCH_CAP 65536

typedef struct {
    char path[128];
    FleeceHttpHandler handler;
    void* user_data;
} HttpHandler;

struct FleeceHttpServer {
    int listen_fd;
    char* webroot;
    HttpHandler handlers[HANDLER_CAP];
    uint32_t handler_count;
    volatile int stop;
    char scratch[SCRATCH_CAP];
};

typedef struct {
    int fd;
    FleeceHttpServer* srv;
} ConnJob;

static const char* mime_for(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

FleeceHttpServer* fleece_http_server_create(const char* webroot, uint16_t port) {
    FleeceHttpServer* srv = (FleeceHttpServer*)calloc(1, sizeof(FleeceHttpServer));
    if (!srv) return NULL;

    srv->webroot = strdup(webroot ? webroot : ".");
    if (!srv->webroot) {
        free(srv);
        return NULL;
    }
    srv->listen_fd = -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("http: socket");
        fleece_http_server_destroy(srv);
        return NULL;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "http: bind port %u: %s\n", (unsigned)port, strerror(errno));
        close(fd);
        fleece_http_server_destroy(srv);
        return NULL;
    }
    if (listen(fd, 16) < 0) {
        perror("http: listen");
        close(fd);
        fleece_http_server_destroy(srv);
        return NULL;
    }

    srv->listen_fd = fd;
    return srv;
}

void fleece_http_server_destroy(FleeceHttpServer* srv) {
    if (!srv) return;
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    free(srv->webroot);
    free(srv);
}

int fleece_http_server_register(FleeceHttpServer* srv, const char* path,
                                FleeceHttpHandler handler, void* user_data) {
    if (!srv || !path || !handler) return -1;
    if (srv->handler_count >= HANDLER_CAP) return -1;
    HttpHandler* h = &srv->handlers[srv->handler_count++];
    snprintf(h->path, sizeof(h->path), "%s", path);
    h->handler = handler;
    h->user_data = user_data;
    return 0;
}

void fleece_http_server_stop(FleeceHttpServer* srv) {
    if (!srv) return;
    srv->stop = 1;
    // Interrupt the blocking accept() in run().
    if (srv->listen_fd >= 0) shutdown(srv->listen_fd, SHUT_RDWR);
}

// --- Request handling ------------------------------------------------------

static void http_send(int fd, int status, const char* reason, const char* mime,
                      const void* body, size_t body_size) {
    char head[512];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n\r\n",
                      status, reason, mime ? mime : "application/octet-stream", body_size);
    if (hn < 0) return;
    (void)send(fd, head, (size_t)hn, 0);
    if (body && body_size > 0) {
        size_t sent = 0;
        while (sent < body_size) {
            ssize_t n = send(fd, (const char*)body + sent, body_size - sent, 0);
            if (n <= 0) break;
            sent += (size_t)n;
        }
    }
}

static void http_send_simple(int fd, int status, const char* reason, const char* mime, const char* body) {
    http_send(fd, status, reason, mime, body, body ? strlen(body) : 0);
}

// Fill *resp for a static file under webroot; returns true if found.
static bool serve_static(FleeceHttpServer* srv, const char* path, char** file_buf, size_t* file_size, FleeceHttpResponse* resp) {
    *file_buf = NULL;
    *file_size = 0;

    if (!path[0] || path[0] != '/') return false;
    // Reject traversal and empty segments - only ever serve inside webroot.
    if (strstr(path, "..") != NULL) {
        resp->status = 403;
        resp->mime = "text/plain";
        resp->body = "forbidden";
        return true;
    }

    char full[4096];
    if (snprintf(full, sizeof(full), "%s%s", srv->webroot, path) >= (int)sizeof(full)) return false;

    const char* serve = full;
    if (strcmp(path, "/") == 0) {
        serve = "/index.html";
        snprintf(full, sizeof(full), "%s%s", srv->webroot, serve);
    }

    FILE* f = fopen(full, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > FILE_CAP) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return false; }
    buf[rd] = '\0';

    resp->status = 200;
    resp->mime = mime_for(serve);
    resp->body = buf;
    *file_buf = buf;
    *file_size = rd;
    return true;
}

// Serve a single request on an accepted connection. Returns false if the
// connection should be closed.
static void handle_conn(FleeceHttpServer* srv, int fd) {
    char buf[HEADER_CAP + BODY_CAP];
    size_t off = 0;

    // Read until headers are complete (HTTP request sent in one go by browsers
    // for our payload sizes, but loop defensively).
    char* header_end = NULL;
    while (off < sizeof(buf)) {
        ssize_t n = recv(fd, buf + off, sizeof(buf) - off - 1, 0);
        if (n <= 0) break;
        off += (size_t)n;
        buf[off] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }
    if (!header_end) {
        http_send_simple(fd, 400, "Bad Request", "text/plain", "bad request");
        return;
    }

    char method[16] = {0};
    char path[2048] = {0};
    if (sscanf(buf, "%15s %2047s", method, path) != 2) {
        http_send_simple(fd, 400, "Bad Request", "text/plain", "bad request");
        return;
    }

    // Strip query string.
    char* q = strchr(path, '?');
    if (q) *q = '\0';

    // Capture body after headers, if declared.
    const char* body = header_end + 4;
    uint32_t body_size = 0;
    const char* cl = strstr(buf, "Content-Length:");
    if (cl) {
        long declared = strtol(cl + 15, NULL, 10);
        if (declared > 0) {
            size_t avail = off - (size_t)(body - buf);
            if ((unsigned long)declared <= avail) {
                body_size = (uint32_t)declared;
            } else if (declared <= BODY_CAP) {
                // Need more bytes for the body - read the remainder.
                size_t need = (size_t)declared - avail;
                while (need > 0 && off < sizeof(buf)) {
                    ssize_t n = recv(fd, buf + off, sizeof(buf) - off - 1, 0);
                    if (n <= 0) break;
                    off += (size_t)n;
                    size_t got = off - (size_t)(body - buf);
                    need = (size_t)declared > got ? (size_t)declared - got : 0;
                }
                if (off - (size_t)(body - buf) >= (size_t)declared) {
                    body_size = (uint32_t)declared;
                }
            }
        }
    }

    char* file_buf = NULL;
    size_t file_size = 0;
    FleeceHttpResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = 404;
    resp.body = "not found";

    if (!serve_static(srv, path, &file_buf, &file_size, &resp)) {
        // Fall through to registered handlers.
        bool handled = false;
        for (uint32_t i = 0; i < srv->handler_count; i++) {
            HttpHandler* h = &srv->handlers[i];
            if (strncmp(path, h->path, strlen(h->path)) == 0) {
                h->handler(method, path, body, body_size, srv->scratch, sizeof(srv->scratch), &resp, h->user_data);
                handled = true;
                break;
            }
        }
        if (!handled) {
            resp.status = 404;
            resp.mime = "text/plain";
            resp.body = "not found";
        }
    }

    http_send(fd, resp.status, resp.status == 200 ? "OK" : resp.status == 400 ? "Bad Request" : resp.status == 403 ? "Forbidden" : "Not Found",
              resp.mime, resp.body, strlen(resp.body));

    free(file_buf);
}

static void* conn_thread(void* ud) {
    ConnJob* job = (ConnJob*)ud;
    handle_conn(job->srv, job->fd);
    close(job->fd);
    free(job);
    return NULL;
}

int fleece_http_server_run(FleeceHttpServer* srv) {
    if (!srv || srv->listen_fd < 0) return -1;

    srv->stop = 0;
    while (!srv->stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(srv->listen_fd, (struct sockaddr*)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR || errno == EBADF) {
                if (srv->stop) break;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("http: accept");
            usleep(100000);
            continue;
        }

        ConnJob* job = (ConnJob*)malloc(sizeof(ConnJob));
        if (!job) { close(fd); continue; }
        job->fd = fd;
        job->srv = srv;

        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread, job) != 0) {
            close(fd);
            free(job);
            continue;
        }
        pthread_detach(tid);
    }

    return 0;
}
