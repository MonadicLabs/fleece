// Defines fleece's four pluggable allocator hooks (declared extern in
// include/fleece_alloc.h) for the standalone Python module build, which links
// only src/planner/fleece_planner.c + src/state/fleece_cbor.c - not
// src/embedded/fleece_embedded.c, which is where the full firmware build gets
// these defaults from. Same default-to-libc values either way.
#include <stdlib.h>

void* (*fleece_malloc_fn)(size_t size) = malloc;
void (*fleece_free_fn)(void* ptr) = free;
void* (*fleece_calloc_fn)(size_t count, size_t size) = calloc;
void* (*fleece_realloc_fn)(void* ptr, size_t size) = realloc;
