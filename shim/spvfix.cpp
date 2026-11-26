// spvfix: passes SPIR-V para bugs/limitações do compilador Mali (fork leegao/SPIRV-Tools).
// Interface C para o vkshim. Retorno: 0 = alterado (*out alocado com malloc),
// 1 = nada a fazer, -1 = erro (usar o original).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "spirv-tools/optimizer.hpp"
#include "spvfix.h"

extern "C" int spvfix_run(const uint32_t *in, size_t nw, unsigned passes,
                          uint32_t **out, size_t *out_nw) {
    if (!passes) return 1;
    try {
        spvtools::Optimizer opt(SPV_ENV_VULKAN_1_3);
        opt.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &, const char *) {});
        // Ordem do compat layer do leegao: o de ClipDistance cria blocos novos, então roda por último.
        if (passes & SPVFIX_SPEC_COMPOSITE) opt.RegisterPass(spvtools::CreateFixMaliSpecConstantCompositePass());
        if (passes & SPVFIX_OPT_BARRIER)    opt.RegisterPass(spvtools::CreateMaliOptimizationBarrierPass());
        if (passes & SPVFIX_BITWISE_SCALAR) opt.RegisterPass(spvtools::CreateMaliBitwiseScalarPass());
        if (passes & SPVFIX_CLAMP_ZERO)     opt.RegisterPass(spvtools::CreateMaliClampZeroPass());
        if (passes & SPVFIX_LOCKSTEP)       opt.RegisterPass(spvtools::CreateMaliLockstepPass());
        if (passes & SPVFIX_CLIP_CULL)      opt.RegisterPass(spvtools::CreateRemoveClipCullDistPass());
        spvtools::OptimizerOptions oo;
        oo.set_run_validator(false);   // validação custa caro e o driver valida de novo
        std::vector<uint32_t> o;
        if (!opt.Run(in, nw, &o, oo)) return -1;
        if (o.size() == nw && !memcmp(o.data(), in, nw * 4)) return 1;
        uint32_t *p = (uint32_t *)malloc(o.size() * 4);
        if (!p) return -1;
        memcpy(p, o.data(), o.size() * 4);
        *out = p; *out_nw = o.size();
        return 0;
    } catch (...) {
        return -1;
    }
}
