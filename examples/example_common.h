// Small shared helper for fleece's examples: loads a companion .js file from
// disk instead of embedding the script as an escaped C string literal.
//
// Embedding a whole script as `"line one\n" "line two\n" ...` works, but is
// genuinely painful to author and maintain: no syntax highlighting, every
// embedded '"' needs escaping, and a JS syntax error only surfaces when the
// binary actually runs (fleece_embedded's dump_exception prints it to
// stderr), not while editing. Loading a real .js file fixes all of that for
// free - it's just plain, lintable, highlightable JavaScript - at the cost
// of needing to find that file at runtime, which is what this header does.
//
// header-only + `static`: each example is still exactly one .c file compiled
// to exactly one executable (see the CMakeLists.txt examples/ glob loop), so
// this avoids adding a second translation unit / CMake target just for a
// ~20-line helper. Not meant to grow into a general-purpose example library -
// see e.g. the UDP transport code, deliberately duplicated between
// example2_search_and_deliver.c and example3_embodied_swarm.c for the same
// "each example stays a single, standalone, copy-pasteable reference" reason.

#ifndef FLEECE_EXAMPLE_COMMON_H
#define FLEECE_EXAMPLE_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>  // dirname

// Reads an entire file into a NUL-terminated, malloc'd buffer. Caller frees.
// Returns NULL (without partial allocation left behind) on any failure.
static char* fleece_example_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return NULL; }

    buf[n] = '\0';
    return buf;
}

// Locates and loads this example's companion .js file, trying a few
// candidate paths relative to the executable's own location - mirrors the
// same candidate-path search examples/run_swarm.sh already uses to find the
// compiled binary. Works whether invoked as `./build/exampleN` from the repo
// root (the documented usage) or directly from within examples/. Caller
// frees the returned buffer. Returns NULL if the script couldn't be found
// anywhere.
static char* fleece_example_load_script(const char* argv0, const char* script_name) {
    char argv0_copy[4096];
    snprintf(argv0_copy, sizeof(argv0_copy), "%s", argv0);
    char* dir = dirname(argv0_copy);  // dirname() may return "." if argv0 has no '/'; that's fine below

    char candidate[4200];
    char* content;

    // Typical case: executable in build/, script in examples/ (sibling of build/).
    snprintf(candidate, sizeof(candidate), "%s/../examples/%s", dir, script_name);
    content = fleece_example_read_file(candidate);
    if (content) return content;

    // Executable and script colocated (e.g. after some form of install step).
    snprintf(candidate, sizeof(candidate), "%s/%s", dir, script_name);
    content = fleece_example_read_file(candidate);
    if (content) return content;

    // Invoked from within examples/ itself, or script path given relative to CWD.
    content = fleece_example_read_file(script_name);
    if (content) return content;

    return NULL;
}

#endif // FLEECE_EXAMPLE_COMMON_H
