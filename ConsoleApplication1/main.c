/*
 * main.c - process entry point.
 *
 * All real work lives in cli.c. This file exists so that the kernel can be
 * built as a library (no main) simply by leaving main.c out of the file list,
 * or by building with -DWASM.
 */
#include "common.h"

#ifdef CONSOLE

#include "cli.h"

int main(int argc, char **argv)
{
    return cli_main(argc, argv);
}

#else

/*
 * ISO C forbids an empty translation unit, so keep a dummy declaration around
 * for the non-console (wasm) build where this file contributes nothing.
 */
typedef int main_unused_translation_unit_t;

#endif /* CONSOLE */
