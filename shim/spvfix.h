#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    SPVFIX_SPEC_COMPOSITE = 1u << 0, /* OpConstantComposite c/ spec constants (bool vec) */
    SPVFIX_OPT_BARRIER    = 1u << 1, /* constant folding errado de shift */
    SPVFIX_BITWISE_SCALAR = 1u << 2, /* erratum Arm 3843237 (r18p0-r53p0): bitwise/shift vetorial */
    SPVFIX_CLAMP_ZERO     = 1u << 3, /* erratum Arm 3779215: clamp com limite 0 */
    SPVFIX_LOCKSTEP       = 1u << 4, /* loops de compute escritos p/ wave 32 */
    SPVFIX_CLIP_CULL      = 1u << 5, /* emula ClipDistance/CullDistance */
};
int spvfix_run(const uint32_t *in, size_t nw, unsigned passes, uint32_t **out, size_t *out_nw);
#ifdef __cplusplus
}
#endif
