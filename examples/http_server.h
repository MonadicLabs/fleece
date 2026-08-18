// Tiny self-contained HTTP server helper for fleece examples.
//
// NOT part of the fleece library - it lives under examples/ so example
// programs can serve a local web UI (a dashboard, a debugger, a plan editor)
// without pulling in a heavyweight web framework. It deliberately offers the
// smallest useful surface:
//
//   - serve static files from a webroot (GET /, /style.css, /app.js, ...)
//   - let the example register JSON handlers (e.g. GET /state, POST /cmd)
//   - one thread per connection, so slow browsers don't stall the sim
//
// Handlers receive a thread-local scratch buffer to build their response into;
// the server frees nothing the handler mallocs, so handlers should either use
// the scratch buffer or take ownership of the response lifetime themselves.
//
// Usage:
//   FleeceHttpServer* srv = fleece_http_server_create("examples/webui", 8080);
//   fleece_http_server_register(srv, "/state", on_state, ud);
//   fleece_http_server_register(srv, "/cmd",   on_cmd,   ud);
//   // ... on SIGINT:
//   fleece_http_server_stop(srv);
//   int rc = fleece_http_server_run(srv);   // blocks until stopped
//   fleece_http_server_destroy(srv);
//
// Paths are resolved only inside webroot (no ../ traversal), and requests for
// a missing static file fall through to registered handlers before 404ing.

#ifndef FLEECE_EXAMPLE_HTTP_SERVER_H
#define FLEECE_EXAMPLE_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceHttpServer FleeceHttpServer;

// Response filled in by a handler. `mime` defaults to application/octet-stream
// if left NULL; `body` must be NUL-terminated (handlers may write the scratch
// buffer they were given).
typedef struct {
    int status;          // 200, 400, 404, ...
    const char* mime;    // e.g. "application/json" (NULL => octet-stream)
    const char* body;    // NUL-terminated response body
} FleeceHttpResponse;

// A registered handler. method is "GET"/"POST"/...; path is the URL path
// without query string; body is the POST body (NULL if none / not body-sized).
// Implementations should write their response into resp using `scratch`
// (scratch_cap bytes, thread-local per connection).
typedef void (*FleeceHttpHandler)(const char* method, const char* path,
                                  const char* body, uint32_t body_size,
                                  char* scratch, size_t scratch_cap,
                                  FleeceHttpResponse* resp, void* user_data);

// Create a server bound to 127.0.0.1:port serving files from webroot. Returns
// NULL if the listener could not be created (port busy, bad webroot path).
FleeceHttpServer* fleece_http_server_create(const char* webroot, uint16_t port);

// Destroy the server (must not be called while run() is active).
void fleece_http_server_destroy(FleeceHttpServer* srv);

// Register a handler for a path prefix. Handlers are consulted only when the
// static webroot lookup misses. Returns 0 on success.
int fleece_http_server_register(FleeceHttpServer* srv, const char* path,
                                FleeceHttpHandler handler, void* user_data);

// Stop the server: run() returns (accept loop is interrupted). Safe to call
// from a signal handler.
void fleece_http_server_stop(FleeceHttpServer* srv);

// Run the accept loop until stopped. Returns 0 on clean stop, nonzero on error.
int fleece_http_server_run(FleeceHttpServer* srv);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_EXAMPLE_HTTP_SERVER_H
