#ifndef EXFATRIM_SHELL_H
#define EXFATRIM_SHELL_H

#include <stddef.h>

/* Returns argv[0..argc-1] as one line a POSIX shell parses back to the same
 * argv: arguments made only of A-Za-z0-9_./:=@%+,- print bare, any other in
 * single quotes with each ' written as '\''. The caller frees the result;
 * NULL on allocation failure. */
char* shell_join(int argc, char* const* argv);

#endif
