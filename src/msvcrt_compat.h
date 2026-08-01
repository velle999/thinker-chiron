#pragma once

/*
Building against msvcrt with a libstdc++ that was configured for UCRT.

Arch's mingw-w64 defaults to UCRT, so its C++ headers assume the C11
quick_exit/at_quick_exit pair exists and do `using ::at_quick_exit;` in
<cstdlib>. The real msvcrt.dll predates C11 and exports neither, so those
headers fail to compile the moment -mcrtdll=msvcrt-os is in effect.

Declaring them here (force-included ahead of every translation unit via
-include) satisfies the headers. msvcrt_compat.cpp then provides the
definitions, so nothing is left dangling if libstdc++ actually references them.

The alternative is a second toolchain built against msvcrt, which is the
cleaner fix if one is ever available. See CMakeLists.txt for why msvcrt is
mandatory here at all.
*/

#ifdef __cplusplus
extern "C" {
#endif

int at_quick_exit(void (*func)(void));
void quick_exit(int status) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif
