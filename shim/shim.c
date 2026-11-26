// vkshim v4: wrapper da HAL Vulkan (Mali-G76 + blob r49p1) para ports de PC.
//
//  1. SPIR-V bindless: runtime descriptor arrays de imagem/sampler -> arrays fixos (v2).
//  2. SPIR-V Mali: passes do fork leegao/SPIRV-Tools contra bugs do compilador Mali
//     (spec-constant composite, shift folding, erratum 3843237, erratum 3779215,
//     lockstep de compute) + emulação de ClipDistance/CullDistance.
//  3. Recursos emulados/aproximados: depthClamp, fillModeNonSolid, clip/cull distance,
//     multiDrawIndirect (+ opcionais multiViewport, vertexPipelineStores, descriptor indexing).
//  4. Cache de pipelines persistente em /data/user/0/<pkg>/cache/vkshim_pcache.bin
//     quando o app não passa VkPipelineCache próprio.
//
// Propriedades (lidas quando o app abre a HAL; reabrir o app para aplicar):
//   debug.vkshim          1 = logs e dump de pipelines que falham
//   debug.vkshim.fake     lista: depthclamp,nonsolid,clipdist,mdi,multivp,vsstores,descidx | all | none
//                         (padrão: depthclamp,nonsolid,clipdist,mdi)
//   debug.vkshim.spv      0 = desliga os passes Mali de SPIR-V (padrão 1)
//   debug.vkshim.pcache   0 = desliga o cache persistente (padrão 1)
//   debug.vkshim.bcn      1 = encadeia o PixelBC (BC1-5/BC7 decodificados na GPU), 0 = não
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include "spvfix.h"
#include "clipcull.h"

#define VKSHIM_VERSION "4.0"
#ifndef BCN_DEFAULT
#define BCN_DEFAULT 0
#endif
#define TAG "vkshim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#ifndef REAL_HAL
#define REAL_HAL "/vendor/lib64/hw/vulkan.r49.so"
#endif
#ifndef BCN_HAL
#define BCN_HAL "/vendor/lib64/hw/vulkan.pixelbc.so"   /* PixelBC (texturas BC1-5/BC7) -> REAL_HAL */
#endif

/* ---- defs mínimas de hardware.h / hwvulkan.h ---- */
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

/* ---- configuração ---- */
enum {
    F_DEPTHCLAMP = 1u << 0, F_NONSOLID = 1u << 1, F_CLIPDIST = 1u << 2, F_MDI = 1u << 3,
    F_MULTIVP = 1u << 4, F_VSSTORES = 1u << 5, F_DESCIDX = 1u << 6,
};
static unsigned fake = F_DEPTHCLAMP | F_NONSOLID | F_CLIPDIST | F_MDI;
static int spv_on = 1, pcache_on = 1, dbg, bcn_on = BCN_DEFAULT;
static char pkg[128];

static int prop(const char *name, char *out) {
    out[0] = 0;
    return __system_property_get(name, out);
}

static void load_config(void) {
    char v[PROP_VALUE_MAX];
    if (prop("debug.vkshim", v) > 0 && v[0] == '1') dbg = 1;
    if (prop("debug.vkshim.spv", v) > 0 && v[0] == '0') spv_on = 0;
    if (prop("debug.vkshim.pcache", v) > 0 && v[0] == '0') pcache_on = 0;
    if (prop("debug.vkshim.bcn", v) > 0) bcn_on = v[0] == '1';
    if (prop("debug.vkshim.fake", v) > 0) {
        static const struct { const char *n; unsigned b; } tab[] = {
            {"depthclamp", F_DEPTHCLAMP}, {"nonsolid", F_NONSOLID}, {"clipdist", F_CLIPDIST},
            {"mdi", F_MDI}, {"multivp", F_MULTIVP}, {"vsstores", F_VSSTORES}, {"descidx", F_DESCIDX},
        };
        if (!strcmp(v, "all")) fake = 0x7f;
        else if (!strcmp(v, "none")) fake = 0;
        else {
            fake = 0;
            char *save = NULL;
            for (char *t = strtok_r(v, ",", &save); t; t = strtok_r(NULL, ",", &save))
                for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++)
                    if (!strcmp(t, tab[i].n)) fake |= tab[i].b;
        }
    }
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) { if (!fread(pkg, 1, sizeof pkg - 1, f)) pkg[0] = 0; fclose(f); }
    char *c = strchr(pkg, ':'); if (c) *c = 0;
}

/* ---- ponteiros reais ---- */
static hwvulkan_device_t *real_dev;
static hwvulkan_device_t my_dev;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateInstance real_ci;
static PFN_vkGetPhysicalDeviceFeatures real_gpdf;
static PFN_vkGetPhysicalDeviceFeatures2 real_gpdf2;
static PFN_vkGetPhysicalDeviceProperties real_gpdp;
static PFN_vkGetPhysicalDeviceProperties2 real_gpdp2;
static PFN_vkEnumerateDeviceExtensionProperties real_edep;
static PFN_vkCreateDevice real_cd;
static PFN_vkDestroyDevice real_dd;
static PFN_vkCreateShaderModule real_csm;
static PFN_vkDestroyShaderModule real_dsm;
static PFN_vkCreateGraphicsPipelines real_cgp;
static PFN_vkCreateComputePipelines real_ccp;
static PFN_vkCreatePipelineCache real_cpc;
static PFN_vkDestroyPipelineCache real_dpc;
static PFN_vkGetPipelineCacheData real_gpcd;
static PFN_vkCreateDescriptorSetLayout real_cdsl;
static PFN_vkCreateDescriptorPool real_cdp;
static PFN_vkAllocateDescriptorSets real_ads;
static PFN_vkCmdDrawIndirect real_cdi;
static PFN_vkCmdDrawIndexedIndirect real_cdii;
static PFN_vkCmdSetDepthClampEnableEXT real_csdce;
static PFN_vkCmdSetPolygonModeEXT real_cspm;
static PFN_vkCmdSetViewportWithCount real_csvwc;
static PFN_vkCmdSetScissorWithCount real_csswc;

static VkInstance g_inst;
static VkPhysicalDeviceFeatures realf;   /* recursos reais do driver */
static int realf_ok;

/* ---- estatística ---- */
static int n_clip, n_rt_patch, n_spv_fix, n_spv_err, n_pipe_fail, n_pipe_total, n_pc_hits;

/* ======================================================================
 * 1. SPIR-V: runtime descriptor arrays -> arrays fixos (vkshim v2)
 * ====================================================================== */
static uint32_t N_IMG = 8192, N_SMP = 256, N_OTHER = 256;

static uint32_t *spv_patch_rt(const uint32_t *in, size_t nw, size_t *out_nw) {
    if (nw < 5 || in[0] != 0x07230203) return NULL;
    int has_rt = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16; if (!wc || i + wc > nw) return NULL;
        if (op == 29) has_rt = 1;
        if (op == 17 && wc == 2 && (in[i+1] == 5301 || in[i+1] == 5302 || (in[i+1] >= 5306 && in[i+1] <= 5312))) has_rt = 1;
        i += wc;
    }
    if (!has_rt) return NULL;
    uint32_t bound = in[3];
    uint8_t *kind = calloc(bound, 1);            /* 1=image 2=sampler 3=sampledimage 4=struct */
    if (!kind) return NULL;
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
    uint32_t *out = malloc((nw + 64 + nw / 2) * 4);
    if (!out) { free(kind); return NULL; }
    size_t o;
    memcpy(out, in, 5 * 4); o = 5;
    uint32_t next = bound;
    uint32_t c_img = 0, c_smp = 0, c_oth = 0;
    int need_int_decl = (uint_id == 0);
    if (need_int_decl) uint_id = next++;
    int converted = 0, removed = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *w = &in[i];
        if (op == 17 && wc == 2 && (w[1] == 5301 || w[1] == 5302 || (w[1] >= 5306 && w[1] <= 5312))) { removed++; i += wc; continue; }
        if (op == 10) { const char *nm = (const char *)&w[1]; if (strstr(nm, "descriptor_indexing")) { removed++; i += wc; continue; } }
        if (op == 71 && wc >= 3 && w[2] == 5300) { removed++; i += wc; continue; } /* NonUniform */
        if (op == 29 && wc == 3 && w[2] < bound && kind[w[2]] && kind[w[2]] != 4) {
            uint32_t k = kind[w[2]];
            uint32_t *cid = k == 2 ? &c_smp : (k == 4 ? &c_oth : &c_img);
            uint32_t n = k == 2 ? N_SMP : (k == 4 ? N_OTHER : N_IMG);
            if (need_int_decl) { out[o++] = (4u << 16) | 21; out[o++] = uint_id; out[o++] = 32; out[o++] = 0; need_int_decl = 0; }
            else if (uint_pos > i) { uint_id = next++; out[o++] = (4u << 16) | 21; out[o++] = uint_id; out[o++] = 32; out[o++] = 0; uint_pos = 0; }
            if (!*cid) { *cid = next++; out[o++] = (4u << 16) | 43; out[o++] = uint_id; out[o++] = *cid; out[o++] = n; }
            out[o++] = (4u << 16) | 28; out[o++] = w[1]; out[o++] = w[2]; out[o++] = *cid;
            converted++; i += wc; continue;
        }
        memcpy(&out[o], w, wc * 4); o += wc; i += wc;
    }
    out[3] = next;
    free(kind);
    if (!converted && !removed) { free(out); return NULL; }
    *out_nw = o;
    __sync_fetch_and_add(&n_rt_patch, 1);
    return out;
}

/* ======================================================================
 * 2. SPIR-V: quais passes Mali valem para este módulo (varredura barata)
 * ====================================================================== */
static unsigned spv_scan(const uint32_t *in, size_t nw) {
    if (nw < 5 || in[0] != 0x07230203) return 0;
    unsigned m = 0; int compute = 0, workgroup = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) return 0;
        switch (op) {
        case 17: /* OpCapability */
            break;
        case 15: /* OpEntryPoint */
            if (in[i+1] == 5) compute = 1;
            break;
        case 59: /* OpVariable: storage class Workgroup = 4 */
            if (wc >= 4 && in[i+3] == 4) workgroup = 1;
            break;
        case 48: case 49: case 50: case 51: case 52: /* OpSpecConstant* */
            m |= SPVFIX_SPEC_COMPOSITE;
            break;
        case 194: case 195: case 196: /* shifts */
            m |= SPVFIX_OPT_BARRIER | SPVFIX_BITWISE_SCALAR;
            break;
        case 197: case 198: case 199: /* bitwise or/xor/and */
            m |= SPVFIX_BITWISE_SCALAR;
            break;
        case 12: /* OpExtInst: GLSL.std.450 min/max/clamp */
            if (wc >= 5) { uint32_t e = in[i+4]; if ((e >= 37 && e <= 45)) m |= SPVFIX_CLAMP_ZERO; }
            break;
        }
        i += wc;
    }
    if (compute && workgroup) m |= SPVFIX_LOCKSTEP;
    if (!(fake & F_CLIPDIST) && !realf.shaderClipDistance) { /* sem emulação: deixa o driver recusar */ }
    return m;
}

typedef struct Mod { VkShaderModule h; size_t sz; uint32_t *code; struct Mod *next; } Mod;
static Mod *mods;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static VKAPI_ATTR VkResult VKAPI_CALL my_csm(VkDevice d, const VkShaderModuleCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkShaderModule *out) {
    VkShaderModuleCreateInfo ci2 = *ci;
    size_t nw = ci->codeSize / 4, nnw = 0;
    uint32_t *p1 = spv_patch_rt(ci->pCode, nw, &nnw);
    if (p1) { ci2.pCode = p1; ci2.codeSize = nnw * 4; }
    uint32_t *p0 = NULL;
    size_t n0 = 0;
    if (!realf.shaderClipDistance) {      /* Mali: sem clip/cull distance -> retira (senão o driver cai) */
        p0 = clipcull_lower(ci2.pCode, ci2.codeSize / 4, &n0);
        if (p0) { ci2.pCode = p0; ci2.codeSize = n0 * 4; __sync_fetch_and_add(&n_clip, 1); }
    }
    const VkShaderModuleCreateInfo base = ci2;   /* versão sem os passes Mali (fallback) */
    uint32_t *p2 = NULL;
    if (spv_on) {
        unsigned m = spv_scan(ci2.pCode, ci2.codeSize / 4);
        if (m) {
            size_t n2 = 0;
            int r = spvfix_run(ci2.pCode, ci2.codeSize / 4, m, &p2, &n2);
            if (r == 0) { ci2.pCode = p2; ci2.codeSize = n2 * 4; __sync_fetch_and_add(&n_spv_fix, 1); }
            else if (r < 0) { int e = __sync_fetch_and_add(&n_spv_err, 1); if (e < 5 || dbg) LOGE("passes Mali falharam (mask 0x%x), usando SPIR-V original", m); }
        }
    }
    VkResult r = real_csm(d, &ci2, a, out);
    if (r != VK_SUCCESS && p2) {
        /* o driver recusou o SPIR-V com os passes Mali: tenta sem eles */
        LOGE("vkCreateShaderModule recusou o SPIR-V com passes Mali (%d), tentando sem eles", r);
        ci2 = base;
        r = real_csm(d, &ci2, a, out);
    }
    if (r == VK_SUCCESS && dbg) {
        Mod *m = malloc(sizeof *m);
        if (m) {
            m->h = *out; m->sz = ci2.codeSize; m->code = malloc(ci2.codeSize);
            if (m->code) memcpy(m->code, ci2.pCode, ci2.codeSize);
            pthread_mutex_lock(&mu); m->next = mods; mods = m; pthread_mutex_unlock(&mu);
        }
    } else if (r != VK_SUCCESS) LOGE("vkCreateShaderModule falhou %d (size %zu)", r, ci2.codeSize);
    free(p0); free(p1); free(p2);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_dsm(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a) {
    if (dbg) {
        pthread_mutex_lock(&mu);
        for (Mod **p = &mods; *p; p = &(*p)->next)
            if ((*p)->h == h) { Mod *m = *p; *p = m->next; free(m->code); free(m); break; }
        pthread_mutex_unlock(&mu);
    }
    real_dsm(d, h, a);
}

/* ======================================================================
 * 3. Cache de pipelines persistente
 * ====================================================================== */
typedef struct { VkDevice dev; VkPipelineCache pc; int dirty; uint8_t uuid[VK_UUID_SIZE]; uint32_t vendor, devid; } DevSt;
static DevSt devs[4];
static pthread_mutex_t pc_mu = PTHREAD_MUTEX_INITIALIZER;
static char pc_path[300];
static int saver_started;

static void pc_save_locked(DevSt *s) {
    if (!s->pc || !s->dirty || !pc_path[0]) return;
    size_t sz = 0;
    if (real_gpcd(s->dev, s->pc, &sz, NULL) != VK_SUCCESS || !sz || sz > (64u << 20)) return;
    void *buf = malloc(sz);
    if (!buf) return;
    if (real_gpcd(s->dev, s->pc, &sz, buf) == VK_SUCCESS) {
        char tmp[320]; snprintf(tmp, sizeof tmp, "%s.tmp", pc_path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(buf, 1, sz, f) == sz;
            ok &= fclose(f) == 0;
            if (ok && rename(tmp, pc_path) == 0) { s->dirty = 0; if (dbg) LOGI("pcache salvo: %zu bytes", sz); }
            else unlink(tmp);
        }
    }
    free(buf);
}

static void *pc_saver(void *arg) {
    (void)arg;
    for (;;) {
        sleep(20);
        pthread_mutex_lock(&pc_mu);
        for (int i = 0; i < 4; i++) pc_save_locked(&devs[i]);
        pthread_mutex_unlock(&pc_mu);
    }
    return NULL;
}

static void pc_setup(VkDevice dev, VkPhysicalDevice pd) {
    if (!pcache_on || !real_cpc || !real_gpcd || !real_dpc) return;
    char dir[200];
    const char *td = getenv("VKSHIM_PCACHE_DIR");   /* só para testes fora de app */
    if (td) snprintf(dir, sizeof dir, "%s", td);
    else if (!pkg[0] || strchr(pkg, '/')) return;  /* só apps (não binários de shell) */
    else snprintf(dir, sizeof dir, "/data/user/0/%s/cache", pkg);
    if (access(dir, W_OK)) return;
    snprintf(pc_path, sizeof pc_path, "%s/vkshim_pcache.bin", dir);
    VkPhysicalDeviceProperties pr; real_gpdp(pd, &pr);
    void *data = NULL; size_t sz = 0;
    FILE *f = fopen(pc_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
        if (l > 32 && l < (64 << 20) && (data = malloc(l)) && fread(data, 1, l, f) == (size_t)l) {
            const uint32_t *h = data;  /* headerSize, version, vendorID, deviceID, UUID */
            if (h[0] >= 32 && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == pr.vendorID &&
                h[3] == pr.deviceID && !memcmp(&h[4], pr.pipelineCacheUUID, VK_UUID_SIZE)) sz = l;
            else LOGI("pcache de outro driver, ignorado");
        }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = sz; ci.pInitialData = sz ? data : NULL;
    VkPipelineCache pc = VK_NULL_HANDLE;
    VkResult r = real_cpc(dev, &ci, NULL, &pc);
    if (r != VK_SUCCESS && sz) { ci.initialDataSize = 0; ci.pInitialData = NULL; r = real_cpc(dev, &ci, NULL, &pc); }
    free(data);
    if (r != VK_SUCCESS) return;
    pthread_mutex_lock(&pc_mu);
    for (int i = 0; i < 4; i++) if (!devs[i].dev) { devs[i].dev = dev; devs[i].pc = pc; devs[i].dirty = 0; pc = VK_NULL_HANDLE; break; }
    if (!saver_started) { pthread_t t; if (!pthread_create(&t, NULL, pc_saver, NULL)) { pthread_detach(t); saver_started = 1; } }
    pthread_mutex_unlock(&pc_mu);
    if (pc) real_dpc(dev, pc, NULL);
    else LOGI("pcache %s (%zu bytes carregados)", pc_path, sz);
}

static VkPipelineCache pc_for(VkDevice d, VkPipelineCache app) {
    if (app != VK_NULL_HANDLE) return app;
    for (int i = 0; i < 4; i++) if (devs[i].dev == d) { devs[i].dirty = 1; __sync_fetch_and_add(&n_pc_hits, 1); return devs[i].pc; }
    return VK_NULL_HANDLE;
}

/* ======================================================================
 * 4. Recursos: anunciar, retirar no vkCreateDevice, sanear pipelines
 * ====================================================================== */
static void ensure_realf(VkPhysicalDevice pd) {
    if (!realf_ok && real_gpdf) { real_gpdf(pd, &realf); realf_ok = 1; }
}

static void fake_core(VkPhysicalDeviceFeatures *f) {
    if (fake & F_DEPTHCLAMP) f->depthClamp = VK_TRUE;
    if (fake & F_NONSOLID) f->fillModeNonSolid = VK_TRUE;
    if (fake & F_CLIPDIST) { f->shaderClipDistance = VK_TRUE; f->shaderCullDistance = VK_TRUE; }
    if (fake & F_MDI) f->multiDrawIndirect = VK_TRUE;
    if (fake & F_MULTIVP) f->multiViewport = VK_TRUE;
    if (fake & F_VSSTORES) f->vertexPipelineStoresAndAtomics = VK_TRUE;
}

static void fake_chain(void *pnext) {
    if (!(fake & F_DESCIDX)) return;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *v = (void *)b;
            v->descriptorIndexing = v->runtimeDescriptorArray = v->descriptorBindingPartiallyBound = VK_TRUE;
            v->descriptorBindingVariableDescriptorCount = v->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            v->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            VkPhysicalDeviceDescriptorIndexingFeatures *v = (void *)b;
            v->runtimeDescriptorArray = v->descriptorBindingPartiallyBound = VK_TRUE;
            v->descriptorBindingVariableDescriptorCount = v->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            v->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL my_gpdf(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    real_gpdf(pd, f);
    if (!realf_ok) { realf = *f; realf_ok = 1; }
    fake_core(f);
}

static VKAPI_ATTR void VKAPI_CALL my_gpdf2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    ensure_realf(pd);
    real_gpdf2(pd, f);
    fake_core(&f->features);
    fake_chain(f->pNext);
}

static void fake_props(VkPhysicalDeviceProperties *p) {
    if ((fake & F_MDI) && p->limits.maxDrawIndirectCount < (1u << 30)) p->limits.maxDrawIndirectCount = 1u << 30;
}
static VKAPI_ATTR void VKAPI_CALL my_gpdp(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    real_gpdp(pd, p); fake_props(p);
}
static VKAPI_ATTR void VKAPI_CALL my_gpdp2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    real_gpdp2(pd, p); fake_props(&p->properties);
}

/* extensão anunciada a mais quando descidx está ligado */
static const VkExtensionProperties ext_descidx = { VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, 2 };

static VKAPI_ATTR VkResult VKAPI_CALL my_edep(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    if (layer || !(fake & F_DESCIDX)) return real_edep(pd, layer, n, p);
    uint32_t rn = 0;
    VkResult r = real_edep(pd, NULL, &rn, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = calloc(rn + 1, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    real_edep(pd, NULL, &rn, tmp);
    int have = 0;
    for (uint32_t i = 0; i < rn; i++) if (!strcmp(tmp[i].extensionName, ext_descidx.extensionName)) have = 1;
    if (!have) tmp[rn++] = ext_descidx;
    if (!p) { *n = rn; free(tmp); return VK_SUCCESS; }
    uint32_t c = *n < rn ? *n : rn;
    memcpy(p, tmp, c * sizeof *p);
    *n = c; free(tmp);
    return c < rn ? VK_INCOMPLETE : VK_SUCCESS;
}

/* zera nos structs do app os recursos fingidos que o driver não tem; restaura depois */
typedef struct { VkBool32 *p; VkBool32 v; } Saved;
#define MAXSAVE 64
static void clear_if(Saved *s, int *ns, VkBool32 *field, VkBool32 real) {
    if (*field && !real && *ns < MAXSAVE) { s[*ns].p = field; s[*ns].v = *field; (*ns)++; *field = VK_FALSE; }
}
static void strip_core(Saved *s, int *ns, VkPhysicalDeviceFeatures *f) {
    clear_if(s, ns, &f->depthClamp, realf.depthClamp);
    clear_if(s, ns, &f->fillModeNonSolid, realf.fillModeNonSolid);
    clear_if(s, ns, &f->shaderClipDistance, realf.shaderClipDistance);
    clear_if(s, ns, &f->shaderCullDistance, realf.shaderCullDistance);
    clear_if(s, ns, &f->multiDrawIndirect, realf.multiDrawIndirect);
    clear_if(s, ns, &f->multiViewport, realf.multiViewport);
    clear_if(s, ns, &f->vertexPipelineStoresAndAtomics, realf.vertexPipelineStoresAndAtomics);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cd(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                             const VkAllocationCallbacks *a, VkDevice *out) {
    ensure_realf(pd);
    Saved s[MAXSAVE]; int ns = 0;
    if (ci->pEnabledFeatures) strip_core(s, &ns, (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures);
    for (VkBaseOutStructure *b = (void *)ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            strip_core(s, &ns, &((VkPhysicalDeviceFeatures2 *)b)->features);
        else if ((fake & F_DESCIDX) && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *v = (void *)b;
            clear_if(s, &ns, &v->descriptorIndexing, 0); clear_if(s, &ns, &v->runtimeDescriptorArray, 0);
            clear_if(s, &ns, &v->descriptorBindingPartiallyBound, 0);
            clear_if(s, &ns, &v->descriptorBindingVariableDescriptorCount, 0);
            clear_if(s, &ns, &v->shaderSampledImageArrayNonUniformIndexing, 0);
            clear_if(s, &ns, &v->descriptorBindingSampledImageUpdateAfterBind, 0);
        } else if ((fake & F_DESCIDX) && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            VkPhysicalDeviceDescriptorIndexingFeatures *v = (void *)b;
            clear_if(s, &ns, &v->runtimeDescriptorArray, 0);
            clear_if(s, &ns, &v->descriptorBindingPartiallyBound, 0);
            clear_if(s, &ns, &v->descriptorBindingVariableDescriptorCount, 0);
            clear_if(s, &ns, &v->shaderSampledImageArrayNonUniformIndexing, 0);
            clear_if(s, &ns, &v->descriptorBindingSampledImageUpdateAfterBind, 0);
        }
    }
    /* tira a extensão fingida da lista */
    VkDeviceCreateInfo c2 = *ci;
    const char **ext = NULL;
    if ((fake & F_DESCIDX) && ci->enabledExtensionCount) {
        ext = malloc(ci->enabledExtensionCount * sizeof *ext);
        if (ext) {
            uint32_t k = 0;
            for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
                if (strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) ext[k++] = ci->ppEnabledExtensionNames[i];
            c2.enabledExtensionCount = k; c2.ppEnabledExtensionNames = ext;
        }
    }
    VkResult r = real_cd(pd, &c2, a, out);
    for (int i = ns - 1; i >= 0; i--) *s[i].p = s[i].v;
    free(ext);
    if (ns) LOGI("vkCreateDevice: %d recursos emulados retirados da criação", ns);
    if (r == VK_SUCCESS) {
        if (!real_gdpa && g_inst) real_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(g_inst, "vkGetDeviceProcAddr");
        if (real_gdpa) {
            VkDevice d = *out;
            real_cpc = (PFN_vkCreatePipelineCache)real_gdpa(d, "vkCreatePipelineCache");
            real_dpc = (PFN_vkDestroyPipelineCache)real_gdpa(d, "vkDestroyPipelineCache");
            real_gpcd = (PFN_vkGetPipelineCacheData)real_gdpa(d, "vkGetPipelineCacheData");
            if (!real_dd) real_dd = (PFN_vkDestroyDevice)real_gdpa(d, "vkDestroyDevice");
            pc_setup(d, pd);
        }
    } else LOGE("vkCreateDevice falhou %d", r);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_dd(VkDevice d, const VkAllocationCallbacks *a) {
    pthread_mutex_lock(&pc_mu);
    for (int i = 0; i < 4; i++) if (devs[i].dev == d) {
        pc_save_locked(&devs[i]);
        real_dpc(d, devs[i].pc, NULL);
        memset(&devs[i], 0, sizeof devs[i]);
    }
    pthread_mutex_unlock(&pc_mu);
    if (dbg) LOGI("stats: clip=%d rt_patch=%d spv_fix=%d spv_err=%d pipelines=%d falhas=%d pcache=%d",
                  n_clip, n_rt_patch, n_spv_fix, n_spv_err, n_pipe_total, n_pipe_fail, n_pc_hits);
    real_dd(d, a);
}

/* ---- dump de pipeline que falha (debug) ---- */
static char dumpdir[256];
static void init_dumpdir(void) {
    if (dumpdir[0]) return;
    if (!pkg[0] || strchr(pkg, '/')) snprintf(dumpdir, sizeof dumpdir, "/data/local/tmp/vkdump");
    else snprintf(dumpdir, sizeof dumpdir, "/data/user/0/%s/cache/vkdump", pkg);
    mkdir(dumpdir, 0777);
}
static void dump_fail(int idx, VkResult r, const VkGraphicsPipelineCreateInfo *ci) {
    init_dumpdir();
    char p[400];
    snprintf(p, sizeof p, "%s/fail_%03d.txt", dumpdir, idx);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "result %d flags 0x%x stages %u\n", r, ci->flags, ci->stageCount);
    for (uint32_t s = 0; s < ci->stageCount; s++) {
        const VkPipelineShaderStageCreateInfo *st = &ci->pStages[s];
        fprintf(f, "stage %u: 0x%x entry '%s'\n", s, st->stage, st->pName);
        const uint32_t *code = NULL; size_t sz = 0;
        pthread_mutex_lock(&mu);
        for (Mod *m = mods; m; m = m->next) if (m->h == st->module) { code = m->code; sz = m->sz; break; }
        if (code) {
            char sp[400]; snprintf(sp, sizeof sp, "%s/fail_%03d_s%u_%x.spv", dumpdir, idx, s, st->stage);
            FILE *g = fopen(sp, "wb"); if (g) { fwrite(code, 1, sz, g); fclose(g); }
        }
        pthread_mutex_unlock(&mu);
    }
    if (ci->pRasterizationState)
        fprintf(f, "raster depthClamp %u poly %d cull %d\n", ci->pRasterizationState->depthClampEnable,
                ci->pRasterizationState->polygonMode, ci->pRasterizationState->cullMode);
    fclose(f);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cgp(VkDevice d, VkPipelineCache pc, uint32_t n,
                                              const VkGraphicsPipelineCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkPipeline *out) {
    /* saneia estado que o hardware não tem (cópias; nunca escreve nos structs do app) */
    VkGraphicsPipelineCreateInfo *cc = NULL;
    VkPipelineRasterizationStateCreateInfo *rs = NULL;
    VkPipelineViewportStateCreateInfo *vs = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const VkPipelineRasterizationStateCreateInfo *r = ci[i].pRasterizationState;
        const VkPipelineViewportStateCreateInfo *v = ci[i].pViewportState;
        int fix_r = r && ((!realf.depthClamp && r->depthClampEnable) ||
                          (!realf.fillModeNonSolid && r->polygonMode != VK_POLYGON_MODE_FILL));
        int fix_v = v && !realf.multiViewport && (v->viewportCount > 1 || v->scissorCount > 1);
        if (!fix_r && !fix_v) continue;
        if (!cc) {
            cc = malloc(n * sizeof *cc); rs = calloc(n, sizeof *rs); vs = calloc(n, sizeof *vs);
            if (!cc || !rs || !vs) { free(cc); free(rs); free(vs); cc = NULL; break; }
            memcpy(cc, ci, n * sizeof *cc);
        }
        if (fix_r) {
            rs[i] = *r; rs[i].depthClampEnable = VK_FALSE; rs[i].polygonMode = VK_POLYGON_MODE_FILL;
            cc[i].pRasterizationState = &rs[i];
        }
        if (fix_v) {
            vs[i] = *v;
            if (vs[i].viewportCount > 1) vs[i].viewportCount = 1;
            if (vs[i].scissorCount > 1) vs[i].scissorCount = 1;
            cc[i].pViewportState = &vs[i];
        }
    }
    VkResult r = real_cgp(d, pc_for(d, pc), n, cc ? cc : ci, a, out);
    __sync_fetch_and_add(&n_pipe_total, n);
    if (r < 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (out[i] != VK_NULL_HANDLE) continue;
            int idx = __sync_fetch_and_add(&n_pipe_fail, 1);
            if (idx < 20 || dbg) LOGE("vkCreateGraphicsPipelines FALHOU %d (falha #%d de %d)", r, idx, n_pipe_total);
            if (dbg && idx < 60) dump_fail(idx, r, &ci[i]);
        }
    }
    free(cc); free(rs); free(vs);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ccp(VkDevice d, VkPipelineCache pc, uint32_t n,
                                              const VkComputePipelineCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = real_ccp(d, pc_for(d, pc), n, ci, a, out);
    if (r < 0) LOGE("vkCreateComputePipelines falhou %d", r);
    return r;
}

/* ---- descriptor indexing (opcional): tira flags que o driver não aceita ---- */
static VKAPI_ATTR VkResult VKAPI_CALL my_cdsl(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
                                               const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    if (!(fake & F_DESCIDX)) return real_cdsl(d, ci, a, out);
    VkDescriptorSetLayoutCreateInfo c2 = *ci;
    c2.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    /* reconstrói a cadeia sem o BindingFlags (cópia rasa dos outros nós não é possível: pula só esse) */
    const VkBaseInStructure *prev = NULL, *b = ci->pNext;
    if (b && b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) c2.pNext = b->pNext;
    else {
        for (prev = b; prev && prev->pNext; prev = prev->pNext)
            if (prev->pNext->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
                VkBaseInStructure *pp = (VkBaseInStructure *)prev; const VkBaseInStructure *skip = prev->pNext;
                pp->pNext = skip->pNext;
                VkResult r = real_cdsl(d, &c2, a, out);
                pp->pNext = skip;
                return r;
            }
    }
    return real_cdsl(d, &c2, a, out);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_cdp(VkDevice d, const VkDescriptorPoolCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    if (!(fake & F_DESCIDX)) return real_cdp(d, ci, a, out);
    VkDescriptorPoolCreateInfo c2 = *ci;
    c2.flags &= ~VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    return real_cdp(d, &c2, a, out);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_ads(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    if (!(fake & F_DESCIDX)) return real_ads(d, ai, out);
    VkDescriptorSetAllocateInfo a2 = *ai;
    const VkBaseInStructure *b = ai->pNext;
    if (b && b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) a2.pNext = b->pNext;
    return real_ads(d, &a2, out);
}

/* ---- comandos ---- */
static VKAPI_ATTR void VKAPI_CALL my_cdi(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) {
    if (n <= 1 || realf.multiDrawIndirect) { real_cdi(cb, b, off, n, stride); return; }
    for (uint32_t i = 0; i < n; i++) real_cdi(cb, b, off + (VkDeviceSize)i * stride, 1, stride);
}
static VKAPI_ATTR void VKAPI_CALL my_cdii(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) {
    if (n <= 1 || realf.multiDrawIndirect) { real_cdii(cb, b, off, n, stride); return; }
    for (uint32_t i = 0; i < n; i++) real_cdii(cb, b, off + (VkDeviceSize)i * stride, 1, stride);
}
static VKAPI_ATTR void VKAPI_CALL my_csdce(VkCommandBuffer cb, VkBool32 e) { real_csdce(cb, realf.depthClamp ? e : VK_FALSE); }
static VKAPI_ATTR void VKAPI_CALL my_cspm(VkCommandBuffer cb, VkPolygonMode m) { real_cspm(cb, realf.fillModeNonSolid ? m : VK_POLYGON_MODE_FILL); }
static VKAPI_ATTR void VKAPI_CALL my_csvwc(VkCommandBuffer cb, uint32_t n, const VkViewport *v) { real_csvwc(cb, (!realf.multiViewport && n > 1) ? 1 : n, v); }
static VKAPI_ATTR void VKAPI_CALL my_csswc(VkCommandBuffer cb, uint32_t n, const VkRect2D *s) { real_csswc(cb, (!realf.multiViewport && n > 1) ? 1 : n, s); }

/* ======================================================================
 * Tabela de interceptação
 * ====================================================================== */
#define HOOK(name, realvar, mine) \
    if (!strcmp(n, name)) { if (r) realvar = (void *)r; return r ? (PFN_vkVoidFunction)mine : NULL; }

static PFN_vkVoidFunction hook(const char *n, PFN_vkVoidFunction r) {
    if (n[0] != 'v' || n[1] != 'k') return r;
    /* dispositivo */
    HOOK("vkCreateShaderModule", real_csm, my_csm)
    HOOK("vkDestroyShaderModule", real_dsm, my_dsm)
    HOOK("vkCreateGraphicsPipelines", real_cgp, my_cgp)
    HOOK("vkCreateComputePipelines", real_ccp, my_ccp)
    HOOK("vkDestroyDevice", real_dd, my_dd)
    if (fake & F_MDI) {
        HOOK("vkCmdDrawIndirect", real_cdi, my_cdi)
        HOOK("vkCmdDrawIndexedIndirect", real_cdii, my_cdii)
    }
    if (fake & F_DEPTHCLAMP) { HOOK("vkCmdSetDepthClampEnableEXT", real_csdce, my_csdce) }
    if (fake & F_NONSOLID) { HOOK("vkCmdSetPolygonModeEXT", real_cspm, my_cspm) }
    if (fake & F_MULTIVP) {
        HOOK("vkCmdSetViewportWithCount", real_csvwc, my_csvwc)
        HOOK("vkCmdSetViewportWithCountEXT", real_csvwc, my_csvwc)
        HOOK("vkCmdSetScissorWithCount", real_csswc, my_csswc)
        HOOK("vkCmdSetScissorWithCountEXT", real_csswc, my_csswc)
    }
    if (fake & F_DESCIDX) {
        HOOK("vkCreateDescriptorSetLayout", real_cdsl, my_cdsl)
        HOOK("vkCreateDescriptorPool", real_cdp, my_cdp)
        HOOK("vkAllocateDescriptorSets", real_ads, my_ads)
    }
    /* instância / physical device */
    HOOK("vkGetPhysicalDeviceFeatures", real_gpdf, my_gpdf)
    HOOK("vkGetPhysicalDeviceFeatures2", real_gpdf2, my_gpdf2)
    HOOK("vkGetPhysicalDeviceFeatures2KHR", real_gpdf2, my_gpdf2)
    HOOK("vkGetPhysicalDeviceProperties", real_gpdp, my_gpdp)
    HOOK("vkGetPhysicalDeviceProperties2", real_gpdp2, my_gpdp2)
    HOOK("vkGetPhysicalDeviceProperties2KHR", real_gpdp2, my_gpdp2)
    HOOK("vkEnumerateDeviceExtensionProperties", real_edep, my_edep)
    HOOK("vkCreateDevice", real_cd, my_cd)
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ci(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gdpa(VkDevice d, const char *name) {
    PFN_vkVoidFunction r = real_gdpa(d, name);
    if (!r) return r;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)my_gdpa;
    return hook(name, r);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance inst, const char *name) {
    PFN_vkVoidFunction r = real_gipa(inst, name);
    if (!r) return r;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)my_gipa;
    if (!strcmp(name, "vkGetDeviceProcAddr")) { real_gdpa = (PFN_vkGetDeviceProcAddr)r; return (PFN_vkVoidFunction)my_gdpa; }
    if (!strcmp(name, "vkCreateInstance")) { real_ci = (PFN_vkCreateInstance)r; return (PFN_vkVoidFunction)my_ci; }
    return hook(name, r);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ci(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    VkResult r = real_ci(ci, a, out);
    if (r == VK_SUCCESS) {
        g_inst = *out;
        /* garante os ponteiros de physical device mesmo se o loader não pedir por nome */
        if (!real_gpdf) real_gpdf = (PFN_vkGetPhysicalDeviceFeatures)real_gipa(*out, "vkGetPhysicalDeviceFeatures");
        if (!real_gpdp) real_gpdp = (PFN_vkGetPhysicalDeviceProperties)real_gipa(*out, "vkGetPhysicalDeviceProperties");
    }
    return r;
}

#ifdef PORTABLE
/* abre <nome> pelo search path do namespace e, se falhar, ao lado deste .so */
static void *open_sibling(const hw_module_t *mod, const char *name, char *path, size_t n) {
    void *h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (h) { snprintf(path, n, "%s", name); return h; }
    Dl_info di;
    if (dladdr((const void *)mod, &di) && di.dli_fname) {
        snprintf(path, n, "%s", di.dli_fname);
        char *sl = strrchr(path, '/');
        if (sl && (size_t)(sl - path) + 1 + strlen(name) < n) {
            strcpy(sl + 1, name);
            h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        }
    }
    return h;
}
#endif

static int my_close(hw_device_t *d) { (void)d; return real_dev->common.close(&real_dev->common); }

static int my_open(const hw_module_t *mod, const char *id, hw_device_t **out) {
    load_config();
    const char *hal = REAL_HAL;
    void *h = NULL;
#ifdef PORTABLE
    /* pacote importado no app: as libs estão na pasta do pacote (search path do namespace) */
    static char path[512];
    if (bcn_on) {
        h = open_sibling(mod, "vulkan.pixelbc.so", path, sizeof path);
        if (h) hal = path; else LOGI("sem vulkan.pixelbc.so no pacote (seguindo sem BCn)");
    }
    if (!h) { h = open_sibling(mod, "vulkan.r49.so", path, sizeof path); if (h) hal = path; }
    if (!h) { hal = REAL_HAL; h = dlopen(REAL_HAL, RTLD_NOW | RTLD_LOCAL); }   /* módulo vk49_g76, se houver */
#else
    if (bcn_on && !access(BCN_HAL, R_OK)) {
        h = dlopen(BCN_HAL, RTLD_NOW | RTLD_LOCAL);
        if (h) hal = BCN_HAL; else LOGE("dlopen %s: %s (seguindo sem BCn)", BCN_HAL, dlerror());
    }
    if (!h) h = dlopen(REAL_HAL, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) { LOGE("dlopen %s: %s", hal, dlerror()); return -1; }
    hw_module_t *rm = dlsym(h, "HMI");
    if (!rm) { LOGE("sem HMI"); return -1; }
    hw_device_t *rd = NULL;
    int ret = rm->methods->open(rm, id, &rd);
    if (ret || !rd) { LOGE("open real falhou %d", ret); return ret ? ret : -1; }
    real_dev = (hwvulkan_device_t *)rd;
    real_gipa = real_dev->GetInstanceProcAddr;
    real_ci = real_dev->CreateInstance;
    my_dev = *real_dev;
    my_dev.common.module = (hw_module_t *)mod;
    my_dev.common.close = my_close;
    my_dev.GetInstanceProcAddr = my_gipa;
    my_dev.CreateInstance = my_ci;
    *out = &my_dev.common;
    LOGI("vkshim v%s sobre %s | fake=0x%x spv=%d pcache=%d bcn=%d debug=%d | %s", VKSHIM_VERSION, hal,
         fake, spv_on, pcache_on, bcn_on, dbg, pkg);
    return 0;
}

static hw_module_methods_t my_methods = { .open = my_open };

__attribute__((visibility("default"))) hw_module_t HMI = {
    .tag = MAKE_TAG('H', 'W', 'M', 'T'),
    .module_api_version = 0x0001,
    .hal_api_version = 0x0100,
    .id = "vulkan",
#ifdef PORTABLE
    .name = "vkshim v4 portatil (r49 wrapper)",
#else
    .name = "vkshim v4 (r49 wrapper)",
#endif
    .author = "rosemary",
    .methods = &my_methods,
};
