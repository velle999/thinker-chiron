
#include "msvcrt_compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>

/*
libmsvcrt-os.a's snprintf alias calls __ms_vsnprintf, but this toolchain's
libmingwex was built for UCRT and no longer provides it -- it is defined in
none of libmingwex.a, libmsvcrt-os.a, libmsvcrt.a or libucrt.a. Supply it.

Declared with its plain C name so the compiler applies the i386 underscore
prefix itself: the archive member wants ___ms_vsnprintf, and ld reports that
as "__ms_vsnprintf" with one underscore consumed. Pinning the symbol by hand
via an asm label gets this wrong by exactly one underscore.

Forwarding to vsnprintf is faithful: the alias exists to give snprintf MS-style
semantics, which is what msvcrt's own implementation already does.
*/
extern "C" int __ms_vsnprintf(char* buf, size_t count, const char* fmt, va_list ap) {
    return vsnprintf(buf, count, fmt, ap);
}

/*
Definitions for the two C11 functions msvcrt.dll does not export. See
msvcrt_compat.h for why they are needed at all.

Nothing in Thinker calls either one; they exist so that any reference from
libstdc++ resolves. The behaviour matches what the C standard asks for closely
enough for a game mod: at_quick_exit registers nothing and reports failure, and
quick_exit terminates without running atexit handlers or static destructors,
which is precisely what _exit does.
*/

extern "C" int at_quick_exit(void (*func)(void)) {
    (void)func;
    return -1; // registration failed; callers must cope
}

extern "C" void quick_exit(int status) {
    _exit(status);
}
