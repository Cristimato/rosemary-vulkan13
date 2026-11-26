// vkshim v2: HAL Vulkan wrapper; patch SPIR-V bindless -> arrays fixos.
// Debug (logs DSL/POOL/ALLOC + dump de pipelines): setprop debug.vkshim 1
// Carrega a HAL real (/vendor/lib64/hw/vulkan.r49.so) e intercepta
// vkCreateShaderModule / vkCreateGraphicsPipelines. Pipelines que falham
// sao despejados (SPIR-V + estado) em /data/user/0/<pkg>/cache/vkdump/.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <android/log.h>
#include <sys/system_properties.h>

#define TAG "vkshim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define REAL_HAL "/vendor/lib64/hw/vulkan.r49.so"

/* ---- defs minimas de hardware.h / hwvulkan.h ---- */
struct hw_module_t;
struct hw_device_t;
typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *, const char *, struct hw_device_t **);
} hw_module_methods_t;
typedef struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char *id; const char *name; const char *author;
    hw_module_methods_t *methods; void *dso; uint64_t reserved[32 - 7];
} hw_module_t;
typedef struct hw_device_t {
    uint32_t tag; uint32_t version; struct hw_module_t *module;
    uint64_t reserved[12]; int (*close)(struct hw_device_t *);
} hw_device_t;
typedef struct hwvulkan_device_t {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;
#define MAKE_TAG(a,b,c,d) ((uint32_t)(((a)<<24)|((b)<<16)|((c)<<8)|(d)))

/* ---- estado ---- */
static hwvulkan_device_t *real_dev;
static hwvulkan_device_t my_dev;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateShaderModule real_csm;
static PFN_vkDestroyShaderModule real_dsm;
static PFN_vkCreateGraphicsPipelines real_cgp;
static PFN_vkCreateDescriptorSetLayout real_cdsl;
static PFN_vkCreateDescriptorPool real_cdp;
static PFN_vkAllocateDescriptorSets real_ads;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char dumpdir[256];
static int nfail, ntotal;
static int dbg; /* setprop debug.vkshim 1 => logs/dumps completos */

typedef struct Mod { VkShaderModule h; size_t sz; uint32_t *code; struct Mod *next; } Mod;
static Mod *mods;

static void init_dumpdir(void) {
    if (dumpdir[0]) return;
    char pkg[128] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) { fread(pkg, 1, sizeof pkg - 1, f); fclose(f); }
    char *c = strchr(pkg, ':'); if (c) *c = 0;
    if (!pkg[0] || strchr(pkg, '/')) snprintf(dumpdir, sizeof dumpdir, "/data/local/tmp/vkdump");
    else snprintf(dumpdir, sizeof dumpdir, "/data/user/0/%s/cache/vkdump", pkg);
    mkdir(dumpdir, 0777);
    LOGI("dumpdir %s", dumpdir);
}

static Mod *find_mod(VkShaderModule h) {
    for (Mod *m = mods; m; m = m->next) if (m->h == h) return m;
    return NULL;
}


/* ---- patch SPIR-V: runtime descriptor arrays -> arrays fixos ---- */
static uint32_t N_IMG = 8192, N_SMP = 256, N_OTHER = 256;
static int npatched;

static void load_cfg(void) {
    static int done; if (done) return; done = 1;
    FILE *f = fopen("/data/local/tmp/vkhw_cfg", "r");
    if (!f) f = fopen("/vendor/lib64/hw/vkshim.cfg", "r");
    if (f) { if (fscanf(f, "%u %u %u", &N_IMG, &N_SMP, &N_OTHER) < 1) {} fclose(f); }
    LOGI("cfg N_IMG=%u N_SMP=%u N_OTHER=%u", N_IMG, N_SMP, N_OTHER);
}

/* devolve codigo novo (malloc) ou NULL se nao precisa patch */
static uint32_t *spv_patch(const uint32_t *in, size_t nw, size_t *out_nw) {
    if (nw < 5 || in[0] != 0x07230203) return NULL;
    int has_rt = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16; if (!wc || i + wc > nw) return NULL;
        if (op == 29) has_rt = 1;
        if (op == 17 && wc == 2 && (in[i+1] == 5301 || in[i+1] == 5302 || (in[i+1] >= 5306 && in[i+1] <= 5312))) has_rt = 1;
        i += wc;
    }
    if (!has_rt) return NULL;
    load_cfg();
    /* mapa id -> opcode de definicao de tipo (p/ imagem/sampler) */
    uint32_t bound = in[3];
    uint8_t *kind = calloc(bound, 1);            /* 1=image 2=sampler 3=sampledimage 4=struct */
    uint32_t uint_id = 0; size_t uint_pos = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (op == 25 && in[i+1] < bound) kind[in[i+1]] = 1;
        if (op == 26 && in[i+1] < bound) kind[in[i+1]] = 2;
        if (op == 27 && in[i+1] < bound) kind[in[i+1]] = 3;
        if (op == 30 && in[i+1] < bound) kind[in[i+1]] = 4;
        if (op == 21 && wc == 4 && in[i+2] == 32 && in[i+3] == 0 && !uint_id) { uint_id = in[i+1]; uint_pos = i; }
        i += wc;
    }
    /* saida: no maximo +4 words por runtime array +4 do int + folga */
    uint32_t *out = malloc((nw + 64 + nw / 2) * 4);
    size_t o = 0;
    memcpy(out, in, 5 * 4); o = 5;
    uint32_t next = bound;
    uint32_t c_img = 0, c_smp = 0, c_oth = 0;
    int need_int_decl = (uint_id == 0);
    if (need_int_decl) uint_id = next++;
    int converted = 0, kept = 0, removed = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *w = &in[i];
        if (op == 17 && wc == 2 && (w[1] == 5301 || w[1] == 5302 || (w[1] >= 5306 && w[1] <= 5312))) { removed++; i += wc; continue; } /* caps descriptor indexing */
        if (op == 10) { const char *nm = (const char *)&w[1]; if (strstr(nm, "descriptor_indexing")) { removed++; i += wc; continue; } }
        if (op == 71 && wc >= 3 && w[2] == 5300) { removed++; i += wc; continue; } /* OpDecorate NonUniform */
        if (op == 29 && wc == 3 && w[2] < bound && kind[w[2]] && kind[w[2]] != 4) {
            uint32_t k = kind[w[2]];
            uint32_t *cid = k == 2 ? &c_smp : (k == 4 ? &c_oth : &c_img);
            uint32_t n = k == 2 ? N_SMP : (k == 4 ? N_OTHER : N_IMG);
            if (need_int_decl) { out[o++] = (4u << 16) | 21; out[o++] = uint_id; out[o++] = 32; out[o++] = 0; need_int_decl = 0; }
            else if (uint_pos > i) { /* int declarado depois: declara outro */ uint_id = next++; out[o++] = (4u << 16) | 21; out[o++] = uint_id; out[o++] = 32; out[o++] = 0; uint_pos = 0; }
            if (!*cid) { *cid = next++; out[o++] = (4u << 16) | 43; out[o++] = uint_id; out[o++] = *cid; out[o++] = n; }
            out[o++] = (4u << 16) | 28; out[o++] = w[1]; out[o++] = w[2]; out[o++] = *cid;
            converted++; i += wc; continue;
        }
        if (op == 29) kept++;
        memcpy(&out[o], w, wc * 4); o += wc; i += wc;
    }
    out[3] = next;
    free(kind);
    if (!converted && !removed) { free(out); return NULL; }
    *out_nw = o;
    __sync_fetch_and_add(&npatched, 1);
    if (npatched <= 3 || npatched % 100 == 0) LOGI("spv patch #%d: %d arrays convertidos, %d runtime arrays mantidos (SSBO)", npatched, converted, kept);
    return out;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_csm(VkDevice d, const VkShaderModuleCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkShaderModule *out) {
    size_t nnw = 0;
    uint32_t *pc = spv_patch(ci->pCode, ci->codeSize / 4, &nnw);
    VkShaderModuleCreateInfo ci2 = *ci;
    if (pc) { ci2.pCode = pc; ci2.codeSize = nnw * 4; ci = &ci2; }
    VkResult r = real_csm(d, ci, a, out);
    if (r == VK_SUCCESS && dbg) {
        Mod *m = malloc(sizeof *m);
        m->h = *out; m->sz = ci->codeSize; m->code = malloc(ci->codeSize);
        memcpy(m->code, ci->pCode, ci->codeSize);
        pthread_mutex_lock(&mu); m->next = mods; mods = m; pthread_mutex_unlock(&mu);
    } else if (r != VK_SUCCESS) LOGE("vkCreateShaderModule falhou %d (size %zu)", r, ci->codeSize);
    free(pc);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_dsm(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a) {
    pthread_mutex_lock(&mu);
    for (Mod **p = &mods; *p; p = &(*p)->next)
        if ((*p)->h == h) { Mod *m = *p; *p = m->next; free(m->code); free(m); break; }
    pthread_mutex_unlock(&mu);
    real_dsm(d, h, a);
}

static void pnext_list(FILE *f, const char *what, const void *pn) {
    const VkBaseInStructure *b = pn;
    if (!b) return;
    fprintf(f, "  %s pNext:", what);
    for (; b; b = b->pNext) fprintf(f, " %d", (int)b->sType);
    fprintf(f, "\n");
}

static void dump_fail(int idx, VkResult r, const VkGraphicsPipelineCreateInfo *ci) {
    init_dumpdir();
    char p[400];
    snprintf(p, sizeof p, "%s/fail_%03d.txt", dumpdir, idx);
    FILE *f = fopen(p, "w");
    if (!f) { LOGE("nao abriu %s", p); return; }
    fprintf(f, "result %d flags 0x%x stages %u layout %p renderPass %p subpass %u\n", r, ci->flags,
            ci->stageCount, (void *)ci->layout, (void *)ci->renderPass, ci->subpass);
    pnext_list(f, "pipeline", ci->pNext);
    for (uint32_t s = 0; s < ci->stageCount; s++) {
        const VkPipelineShaderStageCreateInfo *st = &ci->pStages[s];
        fprintf(f, "stage %u: flag 0x%x entry '%s' module %p flags 0x%x\n", s, st->stage, st->pName,
                (void *)st->module, st->flags);
        pnext_list(f, "stage", st->pNext);
        if (st->pSpecializationInfo) {
            const VkSpecializationInfo *si = st->pSpecializationInfo;
            fprintf(f, "  spec %u entries:", si->mapEntryCount);
            for (uint32_t k = 0; k < si->mapEntryCount; k++) {
                const VkSpecializationMapEntry *e = &si->pMapEntries[k];
                uint32_t v = 0; memcpy(&v, (const char *)si->pData + e->offset, e->size < 4 ? e->size : 4);
                fprintf(f, " id%u=%u", e->constantID, v);
            }
            fprintf(f, "\n");
        }
        const uint32_t *code = NULL; size_t sz = 0;
        pthread_mutex_lock(&mu);
        Mod *m = find_mod(st->module);
        if (m) { code = m->code; sz = m->sz; }
        pthread_mutex_unlock(&mu);
        if (!code) {
            for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
                if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    const VkShaderModuleCreateInfo *mi = (const void *)b; code = mi->pCode; sz = mi->codeSize;
                }
        }
        if (code) {
            char sp[400];
            snprintf(sp, sizeof sp, "%s/fail_%03d_s%u_%x.spv", dumpdir, idx, s, st->stage);
            FILE *g = fopen(sp, "wb"); if (g) { fwrite(code, 1, sz, g); fclose(g); }
        } else fprintf(f, "  (sem SPIR-V conhecido)\n");
    }
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    if (vi) {
        pnext_list(f, "vertexInput", vi->pNext);
        for (uint32_t k = 0; k < vi->vertexBindingDescriptionCount; k++) {
            const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[k];
            fprintf(f, "binding %u stride %u rate %u\n", b->binding, b->stride, b->inputRate);
        }
        for (uint32_t k = 0; k < vi->vertexAttributeDescriptionCount; k++) {
            const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[k];
            fprintf(f, "attr loc %u binding %u format %d offset %u\n", a->location, a->binding, a->format, a->offset);
        }
    }
    if (ci->pInputAssemblyState)
        fprintf(f, "topology %d restart %u\n", ci->pInputAssemblyState->topology,
                ci->pInputAssemblyState->primitiveRestartEnable);
    if (ci->pTessellationState) fprintf(f, "tess patchCP %u\n", ci->pTessellationState->patchControlPoints);
    if (ci->pRasterizationState) {
        const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
        fprintf(f, "raster depthClamp %u discard %u poly %d cull %d front %d depthBias %u lineWidth %f\n",
                rs->depthClampEnable, rs->rasterizerDiscardEnable, rs->polygonMode, rs->cullMode,
                rs->frontFace, rs->depthBiasEnable, rs->lineWidth);
        pnext_list(f, "raster", rs->pNext);
    }
    if (ci->pMultisampleState)
        fprintf(f, "msaa %d sampleShading %u a2c %u\n", ci->pMultisampleState->rasterizationSamples,
                ci->pMultisampleState->sampleShadingEnable, ci->pMultisampleState->alphaToCoverageEnable);
    if (ci->pDepthStencilState)
        fprintf(f, "depth test %u write %u op %d bounds %u stencil %u\n", ci->pDepthStencilState->depthTestEnable,
                ci->pDepthStencilState->depthWriteEnable, ci->pDepthStencilState->depthCompareOp,
                ci->pDepthStencilState->depthBoundsTestEnable, ci->pDepthStencilState->stencilTestEnable);
    if (ci->pColorBlendState) {
        const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
        fprintf(f, "blend logicOp %u attachments %u\n", cb->logicOpEnable, cb->attachmentCount);
        for (uint32_t k = 0; k < cb->attachmentCount && cb->pAttachments; k++) {
            const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[k];
            fprintf(f, "  att%u en %u src %d dst %d op %d asrc %d adst %d aop %d mask 0x%x\n", k, a->blendEnable,
                    a->srcColorBlendFactor, a->dstColorBlendFactor, a->colorBlendOp, a->srcAlphaBlendFactor,
                    a->dstAlphaBlendFactor, a->alphaBlendOp, a->colorWriteMask);
        }
    }
    if (ci->pDynamicState) {
        fprintf(f, "dynamic:");
        for (uint32_t k = 0; k < ci->pDynamicState->dynamicStateCount; k++)
            fprintf(f, " %d", ci->pDynamicState->pDynamicStates[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cgp(VkDevice d, VkPipelineCache pc, uint32_t n,
                                              const VkGraphicsPipelineCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = real_cgp(d, pc, n, ci, a, out);
    __sync_fetch_and_add(&ntotal, n);
    if (r < 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (out[i] != VK_NULL_HANDLE) continue;
            int idx = __sync_fetch_and_add(&nfail, 1);
            if (idx < 20 || dbg) LOGE("vkCreateGraphicsPipelines FALHOU %d (falha #%d de %d pipelines)", r, idx, ntotal);
            if (dbg && idx < 60) dump_fail(idx, r, &ci[i]);
        }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cdsl(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
                                               const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    VkResult r = real_cdsl(d, ci, a, out);
    char buf[1024]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o, "DSL r=%d flags 0x%x n=%u", r, ci->flags, ci->bindingCount);
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bf = NULL;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        o += snprintf(buf + o, sizeof buf - o, " pNext %d", b->sType);
        if (b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) bf = (const void *)b;
    }
    for (uint32_t i = 0; i < ci->bindingCount && o < 900; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        o += snprintf(buf + o, sizeof buf - o, " | b%u type %d cnt %u stages 0x%x bflags 0x%x", b->binding,
                      b->descriptorType, b->descriptorCount, b->stageFlags,
                      bf && i < bf->bindingCount ? bf->pBindingFlags[i] : 0);
    }
    LOGI("%s", buf);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cdp(VkDevice d, const VkDescriptorPoolCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    VkResult r = real_cdp(d, ci, a, out);
    char buf[512]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o, "POOL r=%d flags 0x%x maxSets %u", r, ci->flags, ci->maxSets);
    for (uint32_t i = 0; i < ci->poolSizeCount && o < 450; i++)
        o += snprintf(buf + o, sizeof buf - o, " | type %d cnt %u", ci->pPoolSizes[i].type, ci->pPoolSizes[i].descriptorCount);
    LOGI("%s", buf);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ads(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    VkResult r = real_ads(d, ai, out);
    static int nlog;
    if (r < 0 || ai->pNext || nlog < 8) {
        nlog++;
        const VkBaseInStructure *b = ai->pNext;
        uint32_t vc = 0;
        if (b && b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
            const VkDescriptorSetVariableDescriptorCountAllocateInfo *v = (const void *)b;
            vc = v->descriptorSetCount ? v->pDescriptorCounts[0] : 0;
        }
        LOGI("ALLOC r=%d sets %u pNext %d varCount %u", r, ai->descriptorSetCount, b ? (int)b->sType : 0, vc);
    }
    return r;
}

static PFN_vkVoidFunction intercept(const char *name) {
    if (!dbg) {
        if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)my_csm;
        if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)my_cgp;
        return NULL;
    }
    if (!strcmp(name, "vkCreateDescriptorSetLayout")) return (PFN_vkVoidFunction)my_cdsl;
    if (!strcmp(name, "vkCreateDescriptorPool")) return (PFN_vkVoidFunction)my_cdp;
    if (!strcmp(name, "vkAllocateDescriptorSets")) return (PFN_vkVoidFunction)my_ads;
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)my_csm;
    if (!strcmp(name, "vkDestroyShaderModule")) return (PFN_vkVoidFunction)my_dsm;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)my_cgp;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gdpa(VkDevice d, const char *name) {
    PFN_vkVoidFunction r = real_gdpa(d, name);
    if (!r) return r;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)my_gdpa;
    if (!strcmp(name, "vkCreateShaderModule")) real_csm = (PFN_vkCreateShaderModule)r;
    else if (!strcmp(name, "vkDestroyShaderModule")) real_dsm = (PFN_vkDestroyShaderModule)r;
    else if (!strcmp(name, "vkCreateGraphicsPipelines")) real_cgp = (PFN_vkCreateGraphicsPipelines)r;
    else if (!strcmp(name, "vkCreateDescriptorSetLayout")) real_cdsl = (PFN_vkCreateDescriptorSetLayout)r;
    else if (!strcmp(name, "vkCreateDescriptorPool")) real_cdp = (PFN_vkCreateDescriptorPool)r;
    else if (!strcmp(name, "vkAllocateDescriptorSets")) real_ads = (PFN_vkAllocateDescriptorSets)r;
    PFN_vkVoidFunction i = intercept(name);
    return i ? i : r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance inst, const char *name) {
    PFN_vkVoidFunction r = real_gipa(inst, name);
    if (!r) return r;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)my_gipa;
    if (!strcmp(name, "vkGetDeviceProcAddr")) { real_gdpa = (PFN_vkGetDeviceProcAddr)r; return (PFN_vkVoidFunction)my_gdpa; }
    if (!strcmp(name, "vkCreateShaderModule")) real_csm = (PFN_vkCreateShaderModule)r;
    else if (!strcmp(name, "vkDestroyShaderModule")) real_dsm = (PFN_vkDestroyShaderModule)r;
    else if (!strcmp(name, "vkCreateGraphicsPipelines")) real_cgp = (PFN_vkCreateGraphicsPipelines)r;
    else if (!strcmp(name, "vkCreateDescriptorSetLayout")) real_cdsl = (PFN_vkCreateDescriptorSetLayout)r;
    else if (!strcmp(name, "vkCreateDescriptorPool")) real_cdp = (PFN_vkCreateDescriptorPool)r;
    else if (!strcmp(name, "vkAllocateDescriptorSets")) real_ads = (PFN_vkAllocateDescriptorSets)r;
    PFN_vkVoidFunction i = intercept(name);
    return i ? i : r;
}

static int my_close(hw_device_t *d) { (void)d; return real_dev->common.close(&real_dev->common); }

static int my_open(const hw_module_t *mod, const char *id, hw_device_t **out) {
    char pv[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.vkshim", pv) > 0 && pv[0] == '1') dbg = 1;
#ifdef PORTABLE
    /* Pacote portatil (driver importado no app, namespace do adrenotools-like):
     * 1) pelo nome, no namespace do proprio driver (pasta do pacote);
     * 2) caminho absoluto ao lado deste .so;
     * 3) HAL do modulo vk49_g76, se instalado. */
    const char *which = "vulkan.r49.so";
    void *h = dlopen(which, RTLD_NOW | RTLD_LOCAL);
    char side[512];
    if (!h) {
        LOGI("dlopen %s: %s", which, dlerror());
        Dl_info di;
        if (dladdr((const void *)mod, &di) && di.dli_fname) {
            snprintf(side, sizeof side, "%s", di.dli_fname);
            char *sl = strrchr(side, '/');
            if (sl) { strcpy(sl + 1, "vulkan.r49.so"); which = side; h = dlopen(which, RTLD_NOW | RTLD_LOCAL); }
            if (!h) LOGI("dlopen %s: %s", which, dlerror());
        }
    }
    if (!h) { which = REAL_HAL; h = dlopen(which, RTLD_NOW | RTLD_LOCAL); }
    if (!h) { LOGE("dlopen %s: %s", which, dlerror()); return -1; }
#else
    const char *which = REAL_HAL;
    void *h = dlopen(REAL_HAL, RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOGE("dlopen %s: %s", REAL_HAL, dlerror()); return -1; }
#endif
    hw_module_t *rm = dlsym(h, "HMI");
    if (!rm) { LOGE("sem HMI"); return -1; }
    hw_device_t *rd = NULL;
    int ret = rm->methods->open(rm, id, &rd);
    if (ret || !rd) { LOGE("open real falhou %d", ret); return ret ? ret : -1; }
    real_dev = (hwvulkan_device_t *)rd;
    real_gipa = real_dev->GetInstanceProcAddr;
    my_dev = *real_dev;
    my_dev.common.module = (hw_module_t *)mod;
    my_dev.common.close = my_close;
    my_dev.GetInstanceProcAddr = my_gipa;
    *out = &my_dev.common;
    LOGI("vkshim v2 ativo sobre %s (debug %s)", which, dbg ? "ON" : "off");
    return 0;
}

static hw_module_methods_t my_methods = { .open = my_open };

__attribute__((visibility("default"))) hw_module_t HMI = {
    .tag = MAKE_TAG('H', 'W', 'M', 'T'),
    .module_api_version = 0x0001, /* HWVULKAN_MODULE_API_VERSION_0_1 */
    .hal_api_version = 0x0100,    /* HARDWARE_HAL_API_VERSION */
    .id = "vulkan",
    .name = "vkshim (r49 wrapper)",
    .author = "rosemary",
    .methods = &my_methods,
};
