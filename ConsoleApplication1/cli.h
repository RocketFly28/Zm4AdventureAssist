/*
 * cli.h - command line front end for the survival-adventure DP kernel.
 *
 * This module is the only place in the native build that touches stdin and
 * stdout. It is written strictly against the public API in Adventure.h, so
 * the kernel itself stays free of I/O and can be linked into any host
 * (WebAssembly, a static library, a test harness, ...).
 *
 * All user visible text is plain ASCII on purpose: the source tree mixes GBK
 * and UTF-8 encoded files, and non-ASCII console output would be garbled on
 * most non-Windows terminals.
 */
#ifndef CLI_H
#define CLI_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse argv, run the requested command, and return the process exit code.
 *
 *   0  success
 *   1  runtime failure (out of memory, invalid state, ...)
 *   2  bad usage (unknown command or malformed option)
 *
 * When no command is given the interactive REPL is started.
 */
int cli_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
