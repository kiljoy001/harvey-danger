#include "../../../amd64/include/u.h"

typedef signed char schar;

#undef getcallerpc
#define getcallerpc(...) ((usize)__builtin_return_address(0))
