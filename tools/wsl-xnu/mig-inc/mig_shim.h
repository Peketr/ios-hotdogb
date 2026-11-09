#include <sys/types.h>
#include <stdint.h>
#define __kernel_ptr_semantics
#define __private_extern__ extern
#define __BEGIN_DECLS
#define __END_DECLS
#define __DARWIN_ALIGN_POWER 0
#define __options_decl(name, type, ...) typedef type name; enum __VA_ARGS__
