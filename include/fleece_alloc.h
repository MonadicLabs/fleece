// Fleece pluggable allocator
//
// Every allocation the fleece library itself makes - state-manager field
// values, the GOAP planner's A* frontier/visited/heap, JS-context scratch
// buffers, the embedded/runtime/comms/platform structs - goes through the
// four function pointers below, which default to the standard C
// malloc/calloc/realloc/free.
//
// Call fleece_embedded_set_allocator() (or set the pointers directly) before
// creating any fleece object to redirect all of these to a static pool, arena,
// or any allocator you supply. On memory-constrained targets this is what lets
// the library run without a system heap at all.
//
// The QuickJS engine's runtime allocator is driven by the same four functions:
// fleece_embedded_create() auto-wraps them into a JSMallocFunctions for
// JS_NewRuntime2(), so one call covers the whole stack. If you need the JS GC
// heap in its own dedicated arena (different lifetime/size profile from
// fleece's transient C allocations), pass an explicit JSMallocFunctions to
// fleece_embedded_create_with_allocator() instead.

#ifndef FLEECE_ALLOC_H
#define FLEECE_ALLOC_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pluggable allocator functions for the fleece library. All four default to
// the libc implementations; assign them before creating any fleece object.
extern void* (*fleece_malloc_fn)(size_t size);
extern void (*fleece_free_fn)(void* ptr);
extern void* (*fleece_calloc_fn)(size_t count, size_t size);
extern void* (*fleece_realloc_fn)(void* ptr, size_t size);

#ifdef __cplusplus
}
#endif

// Internal: resolve the library's allocation calls through the pluggable
// functions so a single fleece_embedded_set_allocator() call redirects the
// whole library. Named *_fn to avoid colliding with the fleece_* macros.
#define fleece_malloc(s)      fleece_malloc_fn(s)
#define fleece_free(p)        fleece_free_fn(p)
#define fleece_calloc(n, s)   fleece_calloc_fn((n), (s))
#define fleece_realloc(p, s)  fleece_realloc_fn((p), (s))

#endif // FLEECE_ALLOC_H