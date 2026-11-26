#pragma once
#include <stdint.h>
#include <stddef.h>
/* devolve SPIR-V novo (malloc) sem ClipDistance/CullDistance, ou NULL se não precisa/não sabe */
uint32_t *clipcull_lower(const uint32_t *in, size_t nw, size_t *out_nw);
