// vkshim v4.7: wrapper da HAL Vulkan (Mali-G76 + blob r49p1) para ports de PC.
//
//  1. SPIR-V bindless: runtime descriptor arrays de imagem/sampler -> arrays fixos (v2).
//  2. SPIR-V Mali: passes do fork leegao/SPIRV-Tools contra bugs do compilador Mali
//     (spec-constant composite, shift folding, erratum 3843237, erratum 3779215,
//     lockstep de compute) + retirada de ClipDistance/CullDistance (o driver cai com eles).
//  3. Recursos que a G76 não tem:
//       shaderClipDistance  emulado de verdade (v4.7): varying + discard no fragment shader
//       depthClamp          emulado (v4.7): z preso a [0, w] por vértice no último estágio
//       multiDrawIndirect   N draws; gl_DrawID correto via push constant (v4.7)
//       shaderCullDistance  anunciado, efeito ignorado (nada é descartado)
//       fillModeNonSolid    emulado (v4.7) por geometry shader gerado: triângulos -> arestas/vértices
//     (+ opcionais multiViewport, vertexPipelineStores, descriptor indexing: não emulados)
//  4. Cache de pipelines persistente em /data/user/0/<pkg>/cache/vkshim_pcache.bin
//     quando o app não passa VkPipelineCache próprio.
//  5. Erratas do driver (v4.7):
//       MK-16 (Arm 3786517, r41p0-r49p2) cull mode dinâmico some com pontos/linhas: o vkshim
//             manda VK_CULL_MODE_NONE enquanto um pipeline de pontos/linhas está ligado
//             (confirmado no aparelho com tools/probe/e47)
//       MK-07 (Arm 4222073, r19p0-r54p0) view 2D/2D array de imagem 3D com
//             VK_REMAINING_ARRAY_LAYERS: as contagens viram explícitas
//       MK-11 (Arm 4021056, r41p0-r54p0) layout de stencil separado é ignorado no render pass:
//             os dois layouts viram o layout combinado equivalente
//       MK-06 (Arm 3786926, r49p1-r49p2) vkCmdBeginRendering guarda estado pelo valor do handle
//             da image view: views usadas em renderização dinâmica têm a destruição adiada
//             (fila de 4096) para o handle não voltar logo
//     Nomes MK-xx seguem a lista de erratas do MaliBridge (pesquisa; nenhum código copiado).
//
// Propriedades (lidas quando o app abre a HAL; reabrir o app para aplicar):
//   debug.vkshim          1 = logs e dump de pipelines que falham
//   debug.vkshim.fake     lista: depthclamp,nonsolid,clipdist,mdi,multivp,vsstores,descidx | all | none
//                         (padrão: depthclamp,nonsolid,clipdist,mdi)
//   debug.vkshim.clip     0 = planos de recorte ignorados como na v4 (padrão 1 = emulados)
//   debug.vkshim.clamp    0 = depthClamp ignorado como na v4; 1 = z preso por vértice (v4.7);
//                         2 = exato por pixel com gl_FragDepth (padrão desde a v4.8)
//   debug.vkshim.drawid   0 = gl_DrawID fica 0 no multiDrawIndirect como na v4 (padrão 1)
//   debug.vkshim.nonsolid 0 = LINE/POINT viram FILL como na v4 (padrão 1 = wireframe por geometry shader)
//   debug.vkshim.errata   0 = desliga as correções MK-16/MK-07/MK-11/MK-06 (padrão 1)
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
#include <errno.h>
#include <sys/stat.h>
#include "hud.h"
#include "compat.h"
extern PFN_vkGetInstanceProcAddr compat_gipa;
extern VkInstance compat_inst;
void compat_layout(VkPipelineLayout pl, const VkPipelineLayoutCreateInfo *ci);
void compat_layout_destroy(VkPipelineLayout pl);
static long long mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }
#include <android/log.h>
#include <sys/system_properties.h>
#include "spvfix.h"
#include "clipcull.h"
#include "spvemu.h"

#define VKSHIM_VERSION "5.7"
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
static int spv_on = 1, pcache_on = 1, dbg, bcn_on = BCN_DEFAULT, bcn_forced;
static int clip_on = 1, clamp_on = 2, drawid_on = 1, errata_on = 1, nonsolid_on = 1, gs_enabled;
static char pkg[128];
static int wf_on = 1, wf_iters = 16;
/* v5.7: samplers mais baratos (por jogo). aniso: -1 = do app, 0/1 = sem anisotrópico, N = teto */
static int smp_aniso = -1, smp_mipnear, fp16_on, n_fp16, fps_cap;
static float smp_lodbias;

static int prop(const char *name, char *out) {
    out[0] = 0;
    return __system_property_get(name, out);
}

/* ---- configuração (v4.9) ----
 * Ordem: padrão -> arquivo do jogo -> props debug.vkshim.* (a prop sempre ganha).
 * Arquivo: /storage/emulated/0/Android/data/<pkg>/vkshim.conf (o app lê a própria pasta externa
 * sem permissão; dá para editar sem root) ou /data/user/0/<pkg>/files/vkshim.conf.
 * Formato: chave=valor por linha, '#' comenta. Chaves: debug spv pcache clip clamp drawid errata
 * nonsolid bcn fake hud (mesmos valores das props). */
static char conf_used[300];
static void set_fake(const char *v) {
    static const struct { const char *n; unsigned b; } tab[] = {
        {"depthclamp", F_DEPTHCLAMP}, {"nonsolid", F_NONSOLID}, {"clipdist", F_CLIPDIST},
        {"mdi", F_MDI}, {"multivp", F_MULTIVP}, {"vsstores", F_VSSTORES}, {"descidx", F_DESCIDX},
    };
    if (!strcmp(v, "all")) { fake = 0x7f; return; }
    if (!strcmp(v, "none")) { fake = 0; return; }
    char buf[PROP_VALUE_MAX]; snprintf(buf, sizeof buf, "%s", v);
    fake = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++)
            if (!strcmp(t, tab[i].n)) fake |= tab[i].b;
}
/* aplica uma chave; devolve 0 se a chave não existe */
static int set_key(const char *k, const char *v) {
    int on = v[0] == '1', off = v[0] == '0';
    if (!strcmp(k, "debug")) { dbg = on; return 1; }
    if (!strcmp(k, "spv")) { if (off) spv_on = 0; else if (on) spv_on = 1; return 1; }
    if (!strcmp(k, "pcache")) { if (off) pcache_on = 0; else if (on) pcache_on = 1; return 1; }
    if (!strcmp(k, "clip")) { if (off) clip_on = 0; else if (on) clip_on = 1; return 1; }
    if (!strcmp(k, "clamp")) { if (v[0] >= '0' && v[0] <= '2') clamp_on = v[0] - '0'; return 1; }
    if (!strcmp(k, "drawid")) { if (off) drawid_on = 0; else if (on) drawid_on = 1; return 1; }
    if (!strcmp(k, "errata")) { if (off) errata_on = 0; else if (on) errata_on = 1; return 1; }
    if (!strcmp(k, "nonsolid")) { if (off) nonsolid_on = 0; else if (on) nonsolid_on = 1; return 1; }
    if (!strcmp(k, "bcn")) { if (on || off) { bcn_on = on; bcn_forced = 1; } return 1; }
    if (!strcmp(k, "fake")) { set_fake(v); return 1; }
    if (!strcmp(k, "hud")) { hud_on = on; return 1; }
    if (!strcmp(k, "waterfall")) { if (off) wf_on = 0; else if (on) wf_on = 1; return 1; }
    if (!strcmp(k, "aniso")) { smp_aniso = atoi(v); if (smp_aniso < 0) smp_aniso = -1; return 1; }
    if (!strcmp(k, "lodbias")) { smp_lodbias = (float)atof(v); return 1; }
    if (!strcmp(k, "mip")) { smp_mipnear = !strcmp(v, "near"); return 1; }
    if (!strcmp(k, "fp16")) { fp16_on = on; return 1; }
    if (!strcmp(k, "turbo")) return 1;      /* lido pelo Game Mode (piso de CPU no máximo), não pelo vkshim */
    if (!strcmp(k, "fps")) { fps_cap = atoi(v); if (fps_cap < 0 || fps_cap > 240) fps_cap = 0; return 1; }
    return 0;
}
static void load_conf_file(void) {
    if (!pkg[0] || strchr(pkg, '/')) return;
    char p[2][300];
    snprintf(p[0], sizeof p[0], "/storage/emulated/0/Android/data/%s/vkshim.conf", pkg);
    snprintf(p[1], sizeof p[1], "/data/user/0/%s/files/vkshim.conf", pkg);
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#'); if (h) *h = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = line, *v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            while (*v == ' ' || *v == '\t') v++;
            for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'); ) *--e = 0;
            if (*k && !set_key(k, v)) __android_log_print(ANDROID_LOG_WARN, "vkshim", "vkshim.conf: chave desconhecida '%s'", k);
        }
        fclose(f);
        snprintf(conf_used, sizeof conf_used, "%s", p[i]);
        break;
    }
}
static void load_config(void) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) { if (!fread(pkg, 1, sizeof pkg - 1, f)) pkg[0] = 0; fclose(f); }
    char *c = strchr(pkg, ':'); if (c) *c = 0;
    load_conf_file();
    static const char *keys[] = { "spv", "pcache", "clip", "clamp", "drawid", "errata", "nonsolid", "bcn", "fake", "hud", "waterfall",
                                  "aniso", "lodbias", "mip", "fp16", "fps" };
    char v[PROP_VALUE_MAX], name[64];
    if (prop("debug.vkshim", v) > 0) set_key("debug", v);
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        snprintf(name, sizeof name, "debug.vkshim.%s", keys[i]);
        if (prop(name, v) > 0 && v[0]) set_key(keys[i], v);
    }
}

/* ---- ponteiros reais ---- */
static hwvulkan_device_t *real_dev;
static hwvulkan_device_t my_dev;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
typedef VkResult (VKAPI_PTR *PFN_SignalRelease)(VkQueue, uint32_t, const VkSemaphore *, VkImage, int *);
static PFN_vkQueueSubmit real_qs; static PFN_vkQueueSubmit2 real_qs2;
static PFN_vkGetDeviceQueue real_gdq; static PFN_vkGetDeviceQueue2 real_gdq2; static PFN_SignalRelease real_sria;
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
static PFN_vkDestroyPipeline real_dp;
static PFN_vkCreatePipelineLayout real_cpl;
static PFN_vkDestroyPipelineLayout real_dpl;
static PFN_vkCreatePipelineCache real_cpc;
static PFN_vkDestroyPipelineCache real_dpc;
static PFN_vkGetPipelineCacheData real_gpcd;
static PFN_vkCreateDescriptorSetLayout real_cdsl;
static PFN_vkCreateDescriptorPool real_cdp;
static PFN_vkAllocateDescriptorSets real_ads;
static PFN_vkAllocateCommandBuffers real_acb;
static PFN_vkFreeCommandBuffers real_fcb;
static PFN_vkDestroyCommandPool real_dcp;
static PFN_vkBeginCommandBuffer real_bcb;
static PFN_vkCmdExecuteCommands real_cec;
static PFN_vkCmdBindPipeline real_cbp;
static PFN_vkGetBufferDeviceAddress real_gbda;
static VkDevice g_dev;
static PFN_vkCmdSetCullMode real_scm;
static PFN_vkCmdPushConstants real_cpush;
static PFN_vkCmdDraw real_cdraw;
static PFN_vkCmdDrawIndexed real_cdrawi;
static PFN_vkCmdDrawIndirect real_cdi;
static PFN_vkCmdDrawIndexedIndirect real_cdii;
static PFN_vkCmdSetDepthClampEnableEXT real_csdce;
static PFN_vkCmdSetPolygonModeEXT real_cspm;
static PFN_vkCmdSetViewportWithCount real_csvwc;
static PFN_vkCmdSetScissorWithCount real_csswc;
static PFN_vkCreateImage real_cimg;
static PFN_vkDestroyImage real_dimg;
static PFN_vkCreateImageView real_civ;
static PFN_vkDestroyImageView real_div;
static PFN_vkCmdBeginRendering real_cbr;
static PFN_vkCreateRenderPass2 real_crp2;

static VkInstance g_inst;
static VkPhysicalDeviceFeatures realf;   /* recursos reais do driver */
static int realf_ok;
static uint32_t max_push = 128, max_out_vec = 16;

/* emulações efetivas (recurso anunciado, driver sem ele, prop ligada) */
#define EMU_CLIP   ((fake & F_CLIPDIST) && clip_on && !realf.shaderClipDistance)
#define EMU_CLAMP  ((fake & F_DEPTHCLAMP) && clamp_on && !realf.depthClamp)
#define EMU_DRAWID ((fake & F_MDI) && drawid_on && !realf.multiDrawIndirect)

/* ---- estatística ---- */
static int n_wf;
static int n_clip, n_rt_patch, n_spv_fix, n_spv_err, n_pipe_fail, n_pipe_total, n_pc_hits;
static int n_emu_clip, n_emu_clamp, n_emu_clampx, n_emu_drawid, n_emu_poly, n_emu_fail, n_fix_cull, n_fix_views, n_fix_stencil, n_quar;

/* ======================================================================
 * Tabela de hash genérica (handle -> ponteiro)
 * ====================================================================== */
typedef struct HEnt { uint64_t k; void *v; struct HEnt *next; } HEnt;
typedef struct { HEnt *b[1024]; pthread_mutex_t mu; } HMap;
#define HMAP_INIT { { 0 }, PTHREAD_MUTEX_INITIALIZER }
static unsigned hidx(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (unsigned)(k & 1023); }
static void *hget(HMap *m, uint64_t k) {
    void *v = NULL;
    pthread_mutex_lock(&m->mu);
    for (HEnt *e = m->b[hidx(k)]; e; e = e->next) if (e->k == k) { v = e->v; break; }
    pthread_mutex_unlock(&m->mu);
    return v;
}
/* insere; devolve o valor antigo (para o chamador liberar) */
static void *hput(HMap *m, uint64_t k, void *v) {
    void *old = NULL;
    HEnt *n = malloc(sizeof *n);
    pthread_mutex_lock(&m->mu);
    HEnt **pp = &m->b[hidx(k)];
    for (HEnt *e = *pp; e; e = e->next) if (e->k == k) { old = e->v; e->v = v; free(n); n = NULL; break; }
    if (n) { n->k = k; n->v = v; n->next = *pp; *pp = n; }
    pthread_mutex_unlock(&m->mu);
    return old;
}
static void *hdel(HMap *m, uint64_t k) {
    void *v = NULL;
    pthread_mutex_lock(&m->mu);
    for (HEnt **pp = &m->b[hidx(k)]; *pp; pp = &(*pp)->next)
        if ((*pp)->k == k) { HEnt *e = *pp; v = e->v; *pp = e->next; free(e); break; }
    pthread_mutex_unlock(&m->mu);
    return v;
}
#define HK(h) ((uint64_t)(uintptr_t)(h))

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
        if (op == 17 && wc == 2 && (in[i+1] == 5301 || in[i+1] == 5302 || (in[i+1] >= 5304 && in[i+1] <= 5312))) has_rt = 1;
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
        if (op == 17 && wc == 2 && (w[1] == 5301 || w[1] == 5302 || (w[1] >= 5304 && w[1] <= 5312))) { removed++; i += wc; continue; }
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
    return m;
}

/* processamento de um módulo: bindless -> (clip/cull fora) -> passes Mali */
typedef struct {
    uint32_t *rt, *cc, *fx, *wf, *fp; /* buffers alocados (liberar com spv_free) */
    const uint32_t *pre; size_t pre_nw;    /* antes do clipcull (fonte das variantes com clip) */
    const uint32_t *base; size_t base_nw;  /* sem os passes Mali (fallback) */
    const uint32_t *fin; size_t fin_nw;    /* o que vai para o driver */
} Spv;

static void spv_passes(const uint32_t *in, size_t nw, uint32_t **out, size_t *out_nw) {
    *out = NULL;
    if (!spv_on) return;
    unsigned m = spv_scan(in, nw);
    if (!m) return;
    int r = spvfix_run(in, nw, m, out, out_nw);
    if (r == 0) __sync_fetch_and_add(&n_spv_fix, 1);
    else {
        *out = NULL;
        if (r < 0) { int e = __sync_fetch_and_add(&n_spv_err, 1); if (e < 5 || dbg) LOGE("passes Mali falharam (mask 0x%x), usando SPIR-V original", m); }
    }
}

static void spv_process(Spv *s, const uint32_t *code, size_t nw) {
    memset(s, 0, sizeof *s);
    const uint32_t *cur = code; size_t n = nw, t;
    /* v4.9.3: índice NonUniform de imagens -> waterfall (antes de as decorações saírem) */
    if (wf_on && spvemu_waterfall_needed(cur, n)) {
        int cnt = 0;
        if ((s->wf = spvemu_waterfall(cur, n, wf_iters, &t, &cnt))) { cur = s->wf; n = t; __sync_fetch_and_add(&n_wf, cnt); }
    }
    if ((s->rt = spv_patch_rt(cur, n, &t))) { cur = s->rt; n = t; }
    s->pre = cur; s->pre_nw = n;
    if (!realf.shaderClipDistance && (s->cc = clipcull_lower(cur, n, &t))) { cur = s->cc; n = t; __sync_fetch_and_add(&n_clip, 1); }
    /* v5.7: FP16 opcional nos fragment shaders */
    if (fp16_on) { int cnt = 0; if ((s->fp = spvemu_fp16(cur, n, &t, &cnt))) { cur = s->fp; n = t; __sync_fetch_and_add(&n_fp16, 1); } }
    s->base = cur; s->base_nw = n;
    spv_passes(cur, n, &s->fx, &t);
    if (s->fx) { cur = s->fx; n = t; }
    s->fin = cur; s->fin_nw = n;
}
static void spv_free(Spv *s) { free(s->rt); free(s->cc); free(s->fx); free(s->wf); free(s->fp); memset(s, 0, sizeof *s); }

/* ---- SPIR-V guardado por módulo (para as variantes na criação do pipeline) ---- */
typedef struct {
    uint32_t *code; size_t nw;       /* o que o driver recebeu */
    uint32_t *pre; size_t pre_nw;    /* antes do clipcull, só para módulos com clip/cull */
} ModRec;
static HMap mods = HMAP_INIT;
static size_t mods_bytes;
#define MODS_CAP ((size_t)160 << 20)
static int store_on;                 /* alguma emulação de pipeline precisa do SPIR-V (ou debug) */

static void mod_store(VkShaderModule h, const Spv *s) {
    size_t bytes = s->fin_nw * 4 + (s->cc ? s->pre_nw * 4 : 0);
    if (__sync_add_and_fetch(&mods_bytes, bytes) > MODS_CAP) {
        __sync_sub_and_fetch(&mods_bytes, bytes);
        static int warned; if (!warned) { warned = 1; LOGE("SPIR-V guardado passou de %zu MB: módulos novos sem emulação", MODS_CAP >> 20); }
        return;
    }
    ModRec *r = calloc(1, sizeof *r);
    if (!r) goto fail;
    r->code = malloc(s->fin_nw * 4); r->nw = s->fin_nw;
    if (!r->code) goto fail;
    memcpy(r->code, s->fin, s->fin_nw * 4);
    if (s->cc) {
        r->pre = malloc(s->pre_nw * 4); r->pre_nw = s->pre_nw;
        if (r->pre) memcpy(r->pre, s->pre, s->pre_nw * 4);
    }
    ModRec *old = hput(&mods, HK(h), r);
    if (old) { free(old->code); free(old->pre); free(old); }
    return;
fail:
    if (r) free(r->code);
    free(r);
    __sync_sub_and_fetch(&mods_bytes, bytes);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_csm(VkDevice d, const VkShaderModuleCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkShaderModule *out) {
    Spv s;
    spv_process(&s, ci->pCode, ci->codeSize / 4);
    VkShaderModuleCreateInfo ci2 = *ci;
    ci2.pCode = s.fin; ci2.codeSize = s.fin_nw * 4;
    VkResult r = real_csm(d, &ci2, a, out);
    if (r != VK_SUCCESS && s.fx) {
        /* o driver recusou o SPIR-V com os passes Mali: tenta sem eles */
        LOGE("vkCreateShaderModule recusou o SPIR-V com passes Mali (%d), tentando sem eles", r);
        s.fin = s.base; s.fin_nw = s.base_nw;
        ci2.pCode = s.fin; ci2.codeSize = s.fin_nw * 4;
        r = real_csm(d, &ci2, a, out);
    }
    if (r == VK_SUCCESS && (store_on || dbg)) mod_store(*out, &s);
    else if (r != VK_SUCCESS) LOGE("vkCreateShaderModule falhou %d (size %zu)", r, ci2.codeSize);
    spv_free(&s);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_dsm(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a) {
    ModRec *r = hdel(&mods, HK(h));
    if (r) {
        __sync_sub_and_fetch(&mods_bytes, r->nw * 4 + (r->pre ? r->pre_nw * 4 : 0));
        free(r->code); free(r->pre); free(r);
    }
    real_dsm(d, h, a);
}

/* ======================================================================
 * 3. Cache de pipelines persistente
 * ====================================================================== */
/* ---- PixelBC por jogo (v4.8) ----
 * O PixelBC é uma camada da HAL: ou entra na abertura, ou não entra. Para não pagar o custo dele
 * (~0,25 µs por vkCmdPushConstants) em jogos sem texturas BC, o vkshim anota por jogo, em
 * /data/user/0/<pkg>/cache/vkshim_bc.txt, quantas sessões seguidas (de 60 s ou mais) não criaram
 * nenhuma imagem BC. Com 3 ou mais, a próxima abertura segue sem o PixelBC. Se o jogo tentar criar
 * uma imagem BC sem ele, a marca volta a zero e a sessão seguinte já abre com o PixelBC.
 * debug.vkshim.bcn=0/1 força e ignora o arquivo. */
#define BC_SESSOES 3
static int bc_seen, bc_skipped, bc_prev_sessions, bc_written;
static char bc_path[300];
static time_t bc_t0;
static int is_bc_format(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
static void bc_write(int sessions, int seen) {
    if (!bc_path[0]) return;
    char tmp[320]; snprintf(tmp, sizeof tmp, "%s.tmp", bc_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "sessoes_sem_bc=%d\nbc=%d\n", sessions, seen);
    if (fclose(f) == 0) rename(tmp, bc_path); else unlink(tmp);
}
static void bc_load(void) {
    if (!pkg[0] || strchr(pkg, '/')) return;
    char dir[200]; snprintf(dir, sizeof dir, "/data/user/0/%s/cache", pkg);
    if (access(dir, W_OK)) return;
    snprintf(bc_path, sizeof bc_path, "%s/vkshim_bc.txt", dir);
    bc_t0 = time(NULL);
    FILE *f = fopen(bc_path, "r");
    if (!f) return;
    int n = 0, b = 0;
    if (fscanf(f, "sessoes_sem_bc=%d\nbc=%d", &n, &b) >= 1 && n > 0) bc_prev_sessions = n;
    fclose(f);
}
/* chamado pelo salvador periódico e no vkDestroyDevice */
static void bc_tick(int final) {
    if (!bc_path[0] || bc_written) return;
    if (bc_seen) return;                                  /* já gravado no my_cimg */
    if (!final && time(NULL) - bc_t0 < 60) return;
    if (final && time(NULL) - bc_t0 < 60) return;         /* sessão curta não conta */
    bc_written = 1;
    if (!bc_skipped) bc_write(bc_prev_sessions + 1, 0);
}

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
        bc_tick(0);
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
    compat_features(pd, f->pNext);
}

static void fake_props(VkPhysicalDeviceProperties *p) {
    if ((fake & F_MDI) && p->limits.maxDrawIndirectCount < (1u << 30)) p->limits.maxDrawIndirectCount = 1u << 30;
    /* o driver informa 0 planos; com o recurso anunciado o mínimo da especificação é 8 */
    if ((fake & F_CLIPDIST) && p->limits.maxClipDistances < 8) {
        p->limits.maxClipDistances = 8; p->limits.maxCullDistances = 8; p->limits.maxCombinedClipAndCullDistances = 8;
    }
}
static PFN_vkGetPhysicalDeviceMemoryProperties2 real_gpdmp2;
static VKAPI_ATTR void VKAPI_CALL my_gpdmp2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p) {
    real_gpdmp2(pd, p); compat_memprops(pd, p->pNext);
}
static VKAPI_ATTR void VKAPI_CALL my_gpdp(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    real_gpdp(pd, p); fake_props(p);
}
static VKAPI_ATTR void VKAPI_CALL my_gpdp2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    real_gpdp2(pd, p); fake_props(&p->properties); compat_props(pd, p->pNext);
}

/* extensão anunciada a mais quando descidx está ligado */
static const VkExtensionProperties ext_descidx = { VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, 2 };

static VKAPI_ATTR VkResult VKAPI_CALL my_edep(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    if (layer || (!(fake & F_DESCIDX) && !compat_on)) return real_edep(pd, layer, n, p);
    uint32_t rn = 0;
    VkResult r = real_edep(pd, NULL, &rn, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = calloc(rn + 32, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    real_edep(pd, NULL, &rn, tmp);
    if (fake & F_DESCIDX) {
        int have = 0;
        for (uint32_t i = 0; i < rn; i++) if (!strcmp(tmp[i].extensionName, ext_descidx.extensionName)) have = 1;
        if (!have) tmp[rn++] = ext_descidx;
    }
    rn += compat_exts(&tmp[rn], 30, tmp, rn);
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

static void quar_flush(VkDevice d, VkImage only);

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
    /* fillModeNonSolid emulado por geometry shader: o recurso precisa estar ligado no device */
    VkDeviceCreateInfo c2 = *ci;
    VkPhysicalDeviceFeatures gsf;
    VkBool32 *gsp = NULL;
    gs_enabled = 0;
    if ((fake & F_NONSOLID) && nonsolid_on && !realf.fillModeNonSolid && realf.geometryShader) {
        VkPhysicalDeviceFeatures *pf = (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
        for (VkBaseOutStructure *b = (void *)ci->pNext; b && !pf; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) pf = &((VkPhysicalDeviceFeatures2 *)b)->features;
        if (pf) { if (!pf->geometryShader) { gsp = &pf->geometryShader; *gsp = VK_TRUE; } }
        else { memset(&gsf, 0, sizeof gsf); gsf.geometryShader = VK_TRUE; c2.pEnabledFeatures = &gsf; }
        gs_enabled = 1;
    }
    /* tira a extensão fingida da lista */
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
    CompatDC *cst = NULL;
    const VkDeviceCreateInfo *cdc = compat_device_begin(pd, &c2, &cst);
    VkResult r = real_cd(pd, cdc, a, out);
    if (gsp) *gsp = VK_FALSE;
    if (r != VK_SUCCESS) gs_enabled = 0;
    if (cst && r != VK_SUCCESS) { compat_device_done(cst, *out, r, NULL); cst = NULL; }
    for (int i = ns - 1; i >= 0; i--) *s[i].p = s[i].v;
    free(ext);
    if (ns) LOGI("vkCreateDevice: %d recursos emulados retirados da criação", ns);
    if (r == VK_SUCCESS) {
        if (!real_gdpa && g_inst) real_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(g_inst, "vkGetDeviceProcAddr");
        if (hud_on && real_gdpa) hud_device(*out, pd, g_inst, real_gdpa, real_gipa);
        compat_device_done(cst, *out, r, real_gdpa); cst = NULL;
        if (real_gdpa) {
            VkDevice d = *out;
#define GET(var, name) if (!var) var = (void *)real_gdpa(d, name)
            real_cpc = (PFN_vkCreatePipelineCache)real_gdpa(d, "vkCreatePipelineCache");
            real_dpc = (PFN_vkDestroyPipelineCache)real_gdpa(d, "vkDestroyPipelineCache");
            real_gpcd = (PFN_vkGetPipelineCacheData)real_gdpa(d, "vkGetPipelineCacheData");
            GET(real_dd, "vkDestroyDevice");
            GET(real_csm, "vkCreateShaderModule");
            GET(real_dsm, "vkDestroyShaderModule");
            GET(real_cpush, "vkCmdPushConstants");
            GET(real_scm, "vkCmdSetCullMode");
            GET(real_div, "vkDestroyImageView");
            GET(real_cdi, "vkCmdDrawIndirect");
            GET(real_cdii, "vkCmdDrawIndexedIndirect");
            GET(real_cbp, "vkCmdBindPipeline");
            real_gbda = (PFN_vkGetBufferDeviceAddress)real_gdpa(d, "vkGetBufferDeviceAddress");
            g_dev = d;
#undef GET
            pc_setup(d, pd);
        }
        VkPhysicalDeviceProperties pr; real_gpdp(pd, &pr);
        max_push = pr.limits.maxPushConstantsSize;
        if (real_gpdp2) {   /* waterfall: uma volta por thread do subgroup basta */
            VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
            VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg };
            real_gpdp2(pd, &p2);
            if (sg.subgroupSize >= 4 && sg.subgroupSize <= 64) wf_iters = (int)sg.subgroupSize;
        }
        max_out_vec = pr.limits.maxVertexOutputComponents / 4;
        if (pr.limits.maxFragmentInputComponents / 4 < max_out_vec) max_out_vec = pr.limits.maxFragmentInputComponents / 4;
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
    quar_flush(d, VK_NULL_HANDLE);
    bc_tick(1);
    hud_device_destroy(d);
    compat_device_destroy(d);
    LOGI("stats: waterfall=%d clip=%d rt_patch=%d spv_fix=%d spv_err=%d pipelines=%d falhas=%d pcache=%d | emu clip=%d clamp=%d(exato %d) drawid=%d wire=%d falhou=%d | erratas cull=%d views=%d stencil=%d quarentena=%d",
         n_wf, n_clip, n_rt_patch, n_spv_fix, n_spv_err, n_pipe_total, n_pipe_fail, n_pc_hits,
         n_emu_clip, n_emu_clamp, n_emu_clampx, n_emu_drawid, n_emu_poly, n_emu_fail, n_fix_cull, n_fix_views, n_fix_stencil, n_quar);
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
        ModRec *m = hget(&mods, HK(st->module));
        if (m) {
            char sp[400]; snprintf(sp, sizeof sp, "%s/fail_%03d_s%u_%x.spv", dumpdir, idx, s, st->stage);
            FILE *g = fopen(sp, "wb"); if (g) { fwrite(m->code, 4, m->nw, g); fclose(g); }
        }
    }
    if (ci->pRasterizationState)
        fprintf(f, "raster depthClamp %u poly %d cull %d\n", ci->pRasterizationState->depthClampEnable,
                ci->pRasterizationState->polygonMode, ci->pRasterizationState->cullMode);
    fclose(f);
}

/* ======================================================================
 * 5. Pipeline layouts: slot de push constant para o gl_DrawID
 * ====================================================================== */
typedef struct { uint32_t off, stages, off8; } LInfo;   /* off8: endereço do contador (0 = sem) */
static HMap layouts = HMAP_INIT;

static VkResult cpl_inner(VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out);
static VKAPI_ATTR VkResult VKAPI_CALL my_cpl(VkDevice d, const VkPipelineLayoutCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    VkResult r = cpl_inner(d, ci, a, out);
    if (r == VK_SUCCESS) compat_layout(*out, ci);
    return r;
}
static VkResult cpl_inner(VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    if (!EMU_DRAWID && !compat_count_on) return real_cpl(d, ci, a, out);
    /* 4 bytes (+ 8 alinhados para o endereço do contador do draw_indirect_count) depois de todos os ranges do app, no range que já tem o vertex shader (um range por
     * estágio) se ele é o último, ou num range novo só de vertex se nenhum range tem vertex */
    uint32_t end = 0, n = ci->pushConstantRangeCount;
    int vr = -1;
    for (uint32_t i = 0; i < n; i++) {
        const VkPushConstantRange *r = &ci->pPushConstantRanges[i];
        if (r->offset + r->size > end) end = r->offset + r->size;
        if (r->stageFlags & VK_SHADER_STAGE_VERTEX_BIT) vr = (int)i;
    }
    uint32_t off = (end + 3) & ~3u, off8 = 0, fim = off + 4;
    if (compat_count_on) { off8 = (off + 4 + 7) & ~7u; fim = off8 + 8; }
    int ok = fim <= max_push;
    if (ok && vr >= 0) {
        /* estender o range do VS não pode cobrir bytes de outro range */
        const VkPushConstantRange *v = &ci->pPushConstantRanges[vr];
        for (uint32_t i = 0; i < n && ok; i++) {
            const VkPushConstantRange *r = &ci->pPushConstantRanges[i];
            if ((int)i != vr && r->offset + r->size > v->offset + v->size) ok = 0;
        }
    }
    if (!ok) return real_cpl(d, ci, a, out);
    VkPushConstantRange *rr = malloc((n + 1) * sizeof *rr);
    if (!rr) return real_cpl(d, ci, a, out);
    if (n) memcpy(rr, ci->pPushConstantRanges, n * sizeof *rr);
    uint32_t stages;
    if (vr >= 0) { rr[vr].size = fim - rr[vr].offset; stages = rr[vr].stageFlags; }
    else { rr[n] = (VkPushConstantRange){ VK_SHADER_STAGE_VERTEX_BIT, off, fim - off }; n++; stages = VK_SHADER_STAGE_VERTEX_BIT; }
    VkPipelineLayoutCreateInfo c2 = *ci;
    c2.pushConstantRangeCount = n; c2.pPushConstantRanges = rr;
    VkResult r = real_cpl(d, &c2, a, out);
    free(rr);
    if (r == VK_SUCCESS) {
        LInfo *li = malloc(sizeof *li);
        if (li) { li->off = off; li->stages = stages; li->off8 = off8; free(hput(&layouts, HK(*out), li)); }
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL my_dpl(VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) {
    free(hdel(&layouts, HK(l)));
    compat_layout_destroy(l);
    real_dpl(d, l, a);
}

/* ======================================================================
 * 6. Pipelines: variantes com as emulações + informação para os comandos
 * ====================================================================== */
typedef struct {
    uint8_t cls;        /* 0 triângulos, 1 pontos/linhas */
    uint8_t dyncull;    /* VK_DYNAMIC_STATE_CULL_MODE */
    uint8_t skip;       /* tem geometry/tessellation: a topologia não diz o que é rasterizado */
    uint8_t drawid;     /* lê gl_DrawID do push constant */
    uint8_t cnt;        /* VS com o teste do draw_indirect_count (endereço em pc_off8) */
    VkPipelineLayout layout;
    uint32_t pc_off, pc_stages, pc_off8;
} PInfo;
static HMap pipes = HMAP_INIT;
static int any_drawid_pipe;

static VKAPI_ATTR void VKAPI_CALL my_dp(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) {
    free(hdel(&pipes, HK(p)));
    real_dp(d, p, a);
}

#define MAXST 8
typedef struct {
    VkPipelineShaderStageCreateInfo st[MAXST];
    uint32_t nst;
    VkShaderModule tmp[MAXST + 2]; int ntmp;
    /* nós VkShaderModuleCreateInfo desligados da cadeia do app (restaurados depois) */
    struct { VkBaseInStructure *prev; const VkBaseInStructure *node; } unl[MAXST]; int nunl;
    uint8_t drawid, poly, cnt;
} PWork;

static const VkShaderModuleCreateInfo *inline_module(const VkPipelineShaderStageCreateInfo *s, const VkBaseInStructure **prev) {
    *prev = NULL;
    for (const VkBaseInStructure *b = s->pNext, *p = NULL; b; p = b, b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) { *prev = p; return (const void *)b; }
    return NULL;
}

static VkShaderModule make_module(VkDevice d, const uint32_t *w, size_t nw) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nw * 4, w };
    VkShaderModule m = VK_NULL_HANDLE;
    if (real_csm(d, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}
static void add_tmp(PWork *w, VkShaderModule m) { if (w->ntmp < MAXST + 2) w->tmp[w->ntmp++] = m; }

static int stage_rank(VkShaderStageFlagBits s) {
    switch (s) {
    case VK_SHADER_STAGE_VERTEX_BIT: return 1;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return 2;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return 3;
    default: return 0;
    }
}
static int has_dyn(const VkGraphicsPipelineCreateInfo *ci, VkDynamicState st) {
    const VkPipelineDynamicStateCreateInfo *dy = ci->pDynamicState;
    if (dy) for (uint32_t i = 0; i < dy->dynamicStateCount; i++) if (dy->pDynamicStates[i] == st) return 1;
    return 0;
}

/* variante do último estágio com clip/clamp/drawid; devolve o módulo e guarda as palavras em *keep */
static VkShaderModule build_prerast(VkDevice d, const uint32_t *src, size_t src_nw, const char *ep, int loc, int clamp,
                                    int drawid, int cnt8, int negz, int *nvec, unsigned *done, uint32_t **keep, size_t *keep_nw) {
    size_t o = 0;
    uint32_t *p = spvemu_prerast2(src, src_nw, ep, loc, clamp, drawid, cnt8, negz, &o, nvec, done);
    if (!p) return VK_NULL_HANDLE;
    uint32_t *final = p, *cc = NULL, *fx = NULL; size_t final_nw = o, cc_nw = 0, t;
    if (*done & SPVEMU_DID_CLIP) {
        /* veio de antes do clipcull: tira o built-in e roda os passes Mali */
        if ((cc = clipcull_lower(final, final_nw, &t))) { final = cc; final_nw = cc_nw = t; }
        spv_passes(final, final_nw, &fx, &t);
        if (fx) { final = fx; final_nw = t; }
    }
    VkShaderModule m = make_module(d, final, final_nw);
    if (!m && fx) { final = cc ? cc : p; final_nw = cc ? cc_nw : o; m = make_module(d, final, final_nw); }
    if (m) {
        *keep = malloc(final_nw * 4);
        if (*keep) { memcpy(*keep, final, final_nw * 4); *keep_nw = final_nw; }
    }
    free(p); free(cc); free(fx);
    return m;
}

#define EMU_NONSOLID ((fake & F_NONSOLID) && nonsolid_on && !realf.fillModeNonSolid && gs_enabled)

/* prepara os estágios de um create info: módulos inline e variantes emuladas.
 * Devolve 1 se os estágios mudaram (w->st/w->nst valem), 0 se não. */
static int prep_stages(VkDevice d, const VkGraphicsPipelineCreateInfo *ci, PWork *w) {
    uint32_t n = ci->stageCount;
    if (!n || n > MAXST - 2 || !ci->pStages) return 0;
    memcpy(w->st, ci->pStages, n * sizeof *w->st);
    w->nst = n;
    int changed = 0;
    const uint32_t *code[MAXST] = { 0 }, *pre[MAXST] = { 0 };
    size_t nw[MAXST] = { 0 }, pre_nw[MAXST] = { 0 };
    Spv inl[MAXST]; memset(inl, 0, sizeof inl);
    uint32_t *vsw = NULL; size_t vsw_n = 0;          /* palavras do último estágio usado (variante) */
    for (uint32_t s = 0; s < n; s++) {
        if (w->st[s].module != VK_NULL_HANDLE) {
            ModRec *r = hget(&mods, HK(w->st[s].module));
            if (r) { code[s] = r->code; nw[s] = r->nw; pre[s] = r->pre; pre_nw[s] = r->pre_nw; }
            continue;
        }
        const VkBaseInStructure *prev;
        const VkShaderModuleCreateInfo *mi = inline_module(&ci->pStages[s], &prev);
        if (!mi) continue;
        /* maintenance5: SPIR-V direto no estágio; passa pelo mesmo processamento do vkCreateShaderModule */
        spv_process(&inl[s], mi->pCode, mi->codeSize / 4);
        VkShaderModule m = make_module(d, inl[s].fin, inl[s].fin_nw);
        if (!m && inl[s].fx) { inl[s].fin = inl[s].base; inl[s].fin_nw = inl[s].base_nw; m = make_module(d, inl[s].fin, inl[s].fin_nw); }
        if (!m) continue;
        add_tmp(w, m);
        w->st[s].module = m;
        /* o nó VkShaderModuleCreateInfo sai da cadeia durante a chamada ao driver */
        if (prev) {
            w->unl[w->nunl].prev = (VkBaseInStructure *)prev; w->unl[w->nunl].node = (const void *)mi; w->nunl++;
            ((VkBaseInStructure *)prev)->pNext = mi->pNext;
        } else w->st[s].pNext = mi->pNext;            /* era o primeiro: basta a cópia do estágio */
        code[s] = inl[s].fin; nw[s] = inl[s].fin_nw;
        if (inl[s].cc) { pre[s] = inl[s].pre; pre_nw[s] = inl[s].pre_nw; }
        changed = 1;
    }
    /* último estágio antes da rasterização e fragment shader */
    int last = -1, fs = -1, best = 0, only_vs = 1;
    for (uint32_t s = 0; s < n; s++) {
        int r = stage_rank(w->st[s].stage);
        if (r > best) { best = r; last = (int)s; }
        if (w->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = (int)s;
        else if (w->st[s].stage != VK_SHADER_STAGE_VERTEX_BIT) only_vs = 0;
    }
    const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
    int discard = rs && rs->rasterizerDiscardEnable;
    if (last < 0 || !code[last] || discard) goto out;
    const char *ep = w->st[last].pName;
    int want_clip = 0, want_clamp = 0, want_drawid = -1, want_poly = 0, want_cnt8 = -1, want_negz = 0;
    /* VK_EXT_depth_clip_control (v4.9.4): z do app em [-w, w] */
    if (ci->pViewportState)
        for (const VkBaseInStructure *b = ci->pViewportState->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT &&
                ((const VkPipelineViewportDepthClipControlCreateInfoEXT *)b)->negativeOneToOne) want_negz = 1;
    SpvInfo li, fi;
    memset(&fi, 0, sizeof fi);
    if (EMU_CLIP && pre[last] && !spvemu_info(pre[last], pre_nw[last], ep, &li) && li.clip_n > 0) want_clip = 1;
    if (EMU_CLAMP && rs && rs->depthClampEnable) {
        want_clamp = clamp_on;
        /* com gl_FragDepth o depth bias do rasterizador se perde: esses pipelines ficam no modo por vértice */
        if (want_clamp == 2 && (rs->depthBiasEnable || has_dyn(ci, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE))) want_clamp = 1;
    }
    if (EMU_NONSOLID && rs && rs->polygonMode != VK_POLYGON_MODE_FILL && only_vs && ci->pInputAssemblyState &&
        ci->pInputAssemblyState->topology >= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
        ci->pInputAssemblyState->topology <= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) want_poly = 1;
    if ((EMU_DRAWID || compat_count_on) && ci->layout) {
        int vs = -1;
        for (uint32_t s = 0; s < n; s++) if (w->st[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = (int)s;
        SpvInfo vi;
        LInfo *lay = hget(&layouts, HK(ci->layout));
        int cnt_here = compat_count_on && lay && lay->off8;
        if (vs >= 0 && code[vs] && lay && !spvemu_info(code[vs], nw[vs], w->st[vs].pName, &vi) && (vi.has_drawindex || cnt_here)) {
            want_drawid = (int)lay->off;
            if (cnt_here) want_cnt8 = (int)lay->off8;
            if (vs != last) {
                /* VS não é o último estágio: variante só do VS com o gl_DrawID */
                int nv; unsigned done; uint32_t *k = NULL; size_t kn;
                VkShaderModule m = build_prerast(d, code[vs], nw[vs], w->st[vs].pName, -1, 0, want_drawid, want_cnt8, 0, &nv, &done, &k, &kn);
                free(k);
                if (m) { add_tmp(w, m); w->st[vs].module = m; w->drawid = 1; if (done & SPVEMU_DID_COUNT) w->cnt = 1; changed = 1; __sync_fetch_and_add(&n_emu_drawid, 1); }
                else __sync_fetch_and_add(&n_emu_fail, 1);
                want_drawid = -1; want_cnt8 = -1;
            }
        }
    }
    /* clip e depthClamp exato precisam de location livre dos dois lados e de um fragment shader */
    int loc = -1, cloc = -1, fs_new = 0, fs_ok = 0, clip_nv = 0;
    float mind = 0.f, maxd = 1.f;
    if (want_clip || want_clamp == 2) {
        if (fs >= 0 && code[fs] && !spvemu_info(code[fs], nw[fs], w->st[fs].pName, &fi) && fi.model == 4) fs_ok = 1;
        if (fs < 0) {
            /* sem fragment shader: só se nenhuma cor é escrita */
            const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
            int writes = 0;
            if (cb && cb->pAttachments) for (uint32_t i = 0; i < cb->attachmentCount; i++) if (cb->pAttachments[i].colorWriteMask) writes = 1;
            if (!writes) { fs_ok = 1; fs_new = 1; }
        }
    }
    if (want_clip) {
        clip_nv = (li.clip_n + 3) / 4;
        loc = li.out_loc_end > fi.in_loc_end ? li.out_loc_end : fi.in_loc_end;
        if (!fs_ok || loc + clip_nv > (int)max_out_vec) { want_clip = 0; __sync_fetch_and_add(&n_emu_fail, 1); }
    }
    if (want_clamp == 2) {
        SpvInfo lc;
        if (want_clip) lc = li;
        else if (spvemu_info(code[last], nw[last], ep, &lc)) { want_clamp = 1; goto clamp_done; }
        cloc = lc.out_loc_end > fi.in_loc_end ? lc.out_loc_end : fi.in_loc_end;
        if (want_clip && cloc < loc + clip_nv) cloc = loc + clip_nv;
        if (!fs_ok || cloc + 1 > (int)max_out_vec) { want_clamp = 1; goto clamp_done; }
        /* intervalo de profundidade do viewport estático; dinâmico: assume [0, 1] */
        if (!has_dyn(ci, VK_DYNAMIC_STATE_VIEWPORT) && !has_dyn(ci, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) &&
            ci->pViewportState && ci->pViewportState->pViewports) {
            mind = ci->pViewportState->pViewports[0].minDepth; maxd = ci->pViewportState->pViewports[0].maxDepth;
        }
    }
clamp_done:
    if (want_clip || want_clamp || want_drawid >= 0 || want_negz) {
        int nvec = 0; unsigned done = 0;
        int cl = want_clamp == 2 ? SPVEMU_CLAMP_EXACT + cloc : want_clamp;
        VkShaderModule m = build_prerast(d, want_clip ? pre[last] : code[last], want_clip ? pre_nw[last] : nw[last], ep,
                                         want_clip ? loc : -1, cl, want_drawid, want_cnt8, want_negz, &nvec, &done, &vsw, &vsw_n);
        VkShaderModule fsm = VK_NULL_HANDLE;
        if (m && (done & (SPVEMU_DID_CLIP | SPVEMU_DID_CLAMPX))) {
            size_t fo = 0;
            uint32_t *f = NULL;
            const char *fep = fs_new ? "main" : w->st[fs].pName;
            if (done & SPVEMU_DID_CLIP)
                f = fs_new ? spvemu_fs_new(loc, nvec, &fo) : spvemu_fs_clip(code[fs], nw[fs], fep, loc, nvec, &fo);
            if (done & SPVEMU_DID_CLAMPX) {
                if (done & SPVEMU_DID_CLIP) {
                    size_t go = 0;
                    uint32_t *g = f ? spvemu_fs_clamp(f, fo, fep, cloc, mind, maxd, &go) : NULL;
                    free(f); f = g; fo = go;
                } else {
                    f = fs_new ? spvemu_fs_depth_new(cloc, mind, maxd, &fo) : spvemu_fs_clamp(code[fs], nw[fs], fep, cloc, mind, maxd, &fo);
                }
            }
            fsm = f ? make_module(d, f, fo) : VK_NULL_HANDLE;
            free(f);
            if (!fsm) {   /* sem o FS a variante não serve: refaz sem clip e com o clamp por vértice */
                real_dsm(d, m, NULL); m = VK_NULL_HANDLE;
                free(vsw); vsw = NULL;
                __sync_fetch_and_add(&n_emu_fail, 1);
                if (want_clamp || want_drawid >= 0 || want_negz)
                    m = build_prerast(d, code[last], nw[last], ep, -1, want_clamp ? 1 : 0, want_drawid, want_cnt8, want_negz, &nvec, &done, &vsw, &vsw_n);
            }
        }
        if (m) {
            add_tmp(w, m);
            w->st[last].module = m;
            if (fsm) {
                add_tmp(w, fsm);
                if (fs_new) {
                    VkPipelineShaderStageCreateInfo *ns = &w->st[w->nst++];
                    memset(ns, 0, sizeof *ns);
                    ns->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    ns->stage = VK_SHADER_STAGE_FRAGMENT_BIT; ns->module = fsm; ns->pName = "main";
                } else w->st[fs].module = fsm;
                if (done & SPVEMU_DID_CLIP) __sync_fetch_and_add(&n_emu_clip, 1);
            }
            if (done & SPVEMU_DID_CLAMP) __sync_fetch_and_add(&n_emu_clamp, 1);
            if ((done & SPVEMU_DID_CLAMPX) && fsm) __sync_fetch_and_add(&n_emu_clampx, 1);
            if (done & SPVEMU_DID_DRAWID) { __sync_fetch_and_add(&n_emu_drawid, 1); w->drawid = 1; }
            if (done & SPVEMU_DID_COUNT) w->cnt = 1;
            changed = 1;
        } else if (want_clip || want_clamp || want_drawid >= 0 || want_negz) __sync_fetch_and_add(&n_emu_fail, 1);
    }
    if (want_poly) {
        /* fillModeNonSolid: GS que transforma triângulos em arestas/vértices */
        int cull = 0;
        if (!has_dyn(ci, VK_DYNAMIC_STATE_CULL_MODE) && !has_dyn(ci, VK_DYNAMIC_STATE_FRONT_FACE) &&
            !has_dyn(ci, VK_DYNAMIC_STATE_VIEWPORT) && !has_dyn(ci, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) &&
            ci->pViewportState && ci->pViewportState->pViewports && (rs->cullMode & VK_CULL_MODE_FRONT_AND_BACK)) {
            cull = (int)(rs->cullMode & 3) | (rs->frontFace == VK_FRONT_FACE_CLOCKWISE ? 4 : 0) |
                   (ci->pViewportState->pViewports[0].height < 0 ? 8 : 0);
        }
        size_t go = 0;
        uint32_t *g = spvemu_gs_poly(vsw ? vsw : code[last], vsw ? vsw_n : nw[last], ep, rs->polygonMode == VK_POLYGON_MODE_POINT, cull, &go);
        VkShaderModule gm = g ? make_module(d, g, go) : VK_NULL_HANDLE;
        free(g);
        if (gm && w->nst < MAXST) {
            add_tmp(w, gm);
            VkPipelineShaderStageCreateInfo *ns = &w->st[w->nst++];
            memset(ns, 0, sizeof *ns);
            ns->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ns->stage = VK_SHADER_STAGE_GEOMETRY_BIT; ns->module = gm; ns->pName = "main";
            w->poly = 1; changed = 1;
            __sync_fetch_and_add(&n_emu_poly, 1);
        } else {
            if (gm) real_dsm(d, gm, NULL);
            __sync_fetch_and_add(&n_emu_fail, 1);
        }
    }
out:
    free(vsw);
    for (uint32_t s = 0; s < n; s++) spv_free(&inl[s]);
    return changed;
}

static void pinfo_register(const VkGraphicsPipelineCreateInfo *ci, VkPipeline p, int drawid, int poly, int cnt) {
    PInfo pi = { 0 };
    const VkPipelineDynamicStateCreateInfo *dy = ci->pDynamicState;
    if (dy) for (uint32_t i = 0; i < dy->dynamicStateCount; i++) if (dy->pDynamicStates[i] == VK_DYNAMIC_STATE_CULL_MODE) pi.dyncull = 1;
    for (uint32_t s = 0; s < ci->stageCount; s++)
        if (ci->pStages[s].stage & (VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) pi.skip = 1;
    if (ci->pInputAssemblyState && ci->pInputAssemblyState->topology <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) pi.cls = 1;
    if (ci->pInputAssemblyState && (ci->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
                                    ci->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY)) pi.cls = 1;
    if (poly) { pi.cls = 1; pi.skip = 0; }    /* o GS do wireframe rasteriza linhas/pontos (MK-16) */
    if (drawid) {
        LInfo *li = hget(&layouts, HK(ci->layout));
        if (li) {
            pi.drawid = 1; pi.layout = ci->layout; pi.pc_off = li->off; pi.pc_stages = li->stages; any_drawid_pipe = 1;
            if (cnt && li->off8) { pi.cnt = 1; pi.pc_off8 = li->off8; }
        }
    }
    if (!pi.dyncull && !pi.drawid) return;
    PInfo *h = malloc(sizeof *h);
    if (h) { *h = pi; free(hput(&pipes, HK(p), h)); }
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cgp(VkDevice d, VkPipelineCache pc, uint32_t n,
                                              const VkGraphicsPipelineCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkPipeline *out) {
    /* saneia estado que o hardware não tem (cópias; nunca escreve nos structs do app) */
    VkGraphicsPipelineCreateInfo *cc = malloc(n * sizeof *cc);
    VkPipelineRasterizationStateCreateInfo *rs = calloc(n, sizeof *rs);
    VkPipelineViewportStateCreateInfo *vs = calloc(n, sizeof *vs);
    VkPipelineRasterizationStateCreateInfo *rsc = calloc(n, sizeof *rsc);
    PWork *pw = calloc(n, sizeof *pw);
    if (!cc || !rs || !vs || !pw || !rsc) {
        free(cc); free(rs); free(vs); free(pw); free(rsc);
        return real_cgp(d, pc_for(d, pc), n, ci, a, out);
    }
    memcpy(cc, ci, n * sizeof *cc);
    for (uint32_t i = 0; i < n; i++) {
        /* VK_EXT_depth_clip_enable (v4.9.2): sem corte de profundidade = depthClamp emulado;
         * com corte, o clamp não muda nada visível e fica desligado (sem custo) */
        int clip_en;
        if (compat_depth_clip(ci[i].pRasterizationState, &clip_en)) {
            rsc[i] = *ci[i].pRasterizationState;
            rsc[i].depthClampEnable = clip_en ? VK_FALSE : VK_TRUE;
            const VkBaseInStructure *f = rsc[i].pNext;
            if (f && f->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) rsc[i].pNext = f->pNext;
            cc[i].pRasterizationState = &rsc[i];
        }
        if (prep_stages(d, &cc[i], &pw[i])) { cc[i].pStages = pw[i].st; cc[i].stageCount = pw[i].nst; }
        const VkPipelineRasterizationStateCreateInfo *r = cc[i].pRasterizationState;
        const VkPipelineViewportStateCreateInfo *v = ci[i].pViewportState;
        if (v && v->pNext && ((const VkBaseInStructure *)v->pNext)->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT) {
            vs[i] = *v; vs[i].pNext = ((const VkBaseInStructure *)v->pNext)->pNext;
            cc[i].pViewportState = &vs[i]; v = &vs[i];
        }
        int fix_r = r && ((!realf.depthClamp && r->depthClampEnable) ||
                          (!realf.fillModeNonSolid && r->polygonMode != VK_POLYGON_MODE_FILL));
        int fix_v = v && !realf.multiViewport && (v->viewportCount > 1 || v->scissorCount > 1);
        if (fix_r) {
            rs[i] = *r; rs[i].depthClampEnable = VK_FALSE; rs[i].polygonMode = VK_POLYGON_MODE_FILL;
            cc[i].pRasterizationState = &rs[i];
        }
        if (fix_v) {
            if (v != &vs[i]) vs[i] = *v;
            if (vs[i].viewportCount > 1) vs[i].viewportCount = 1;
            if (vs[i].scissorCount > 1) vs[i].scissorCount = 1;
            cc[i].pViewportState = &vs[i];
        }
    }
    if (dbg) for (uint32_t i = 0; i < n; i++) {
        const VkPipelineRasterizationStateCreateInfo *r0 = ci[i].pRasterizationState;
        const VkPipelineViewportStateCreateInfo *v0 = ci[i].pViewportState;
        LOGI("pipe: estagios=%u clamp=%d bias=%d(dyn %d) poly=%d cull=%d vp=%s%.2f..%.2f(dyn %d) zfmt_dyn=%d",
             ci[i].stageCount, r0 ? (int)r0->depthClampEnable : -1, r0 ? (int)r0->depthBiasEnable : -1,
             has_dyn(&ci[i], VK_DYNAMIC_STATE_DEPTH_BIAS), r0 ? (int)r0->polygonMode : -1, r0 ? (int)r0->cullMode : -1,
             (v0 && v0->pViewports) ? "" : "-", (v0 && v0->pViewports) ? v0->pViewports[0].minDepth : 0.f,
             (v0 && v0->pViewports) ? v0->pViewports[0].maxDepth : 0.f,
             has_dyn(&ci[i], VK_DYNAMIC_STATE_VIEWPORT) || has_dyn(&ci[i], VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT),
             has_dyn(&ci[i], VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT));
    }
    long long t0 = hud_on ? mono_ns() : 0;
    VkResult r = real_cgp(d, pc_for(d, pc), n, cc, a, out);
    if (hud_on) hud_pipe(mono_ns() - t0, n);
    int tot = __sync_add_and_fetch(&n_pipe_total, n);
    if (dbg && (tot / 16) != ((tot - (int)n) / 16))
        LOGI("stats: pipelines=%d falhas=%d | emu clip=%d clamp=%d(exato %d) drawid=%d wire=%d falhou=%d",
             tot, n_pipe_fail, n_emu_clip, n_emu_clamp, n_emu_clampx, n_emu_drawid, n_emu_poly, n_emu_fail);
    for (uint32_t i = 0; i < n; i++) {
        /* restaura as cadeias do app e destrói os módulos temporários */
        for (int k = 0; k < pw[i].nunl; k++) pw[i].unl[k].prev->pNext = pw[i].unl[k].node;
        for (int k = 0; k < pw[i].ntmp; k++) real_dsm(d, pw[i].tmp[k], NULL);
        if (out[i] != VK_NULL_HANDLE && (errata_on || pw[i].drawid || pw[i].poly)) pinfo_register(&ci[i], out[i], pw[i].drawid, pw[i].poly, pw[i].cnt);
    }
    if (r < 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (out[i] != VK_NULL_HANDLE) continue;
            int idx = __sync_fetch_and_add(&n_pipe_fail, 1);
            if (idx < 20 || dbg) LOGE("vkCreateGraphicsPipelines FALHOU %d (falha #%d de %d)", r, idx, n_pipe_total);
            if (dbg && idx < 60) dump_fail(idx, r, &ci[i]);
        }
    }
    free(cc); free(rs); free(vs); free(pw); free(rsc);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ccp(VkDevice d, VkPipelineCache pc, uint32_t n,
                                              const VkComputePipelineCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkPipeline *out) {
    /* maintenance5: módulo inline no compute também passa pelos passes */
    VkComputePipelineCreateInfo *cc = NULL;
    VkShaderModule *tmp = NULL;
    struct { VkBaseInStructure *prev; const VkBaseInStructure *node; } *unl = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (ci[i].stage.module != VK_NULL_HANDLE) continue;
        const VkBaseInStructure *prev;
        const VkShaderModuleCreateInfo *mi = inline_module(&ci[i].stage, &prev);
        if (!mi) continue;
        if (!cc) {
            cc = malloc(n * sizeof *cc); tmp = calloc(n, sizeof *tmp); unl = calloc(n, sizeof *unl);
            if (!cc || !tmp || !unl) { free(cc); free(tmp); free(unl); cc = NULL; break; }
            memcpy(cc, ci, n * sizeof *cc);
        }
        Spv s; spv_process(&s, mi->pCode, mi->codeSize / 4);
        VkShaderModule m = make_module(d, s.fin, s.fin_nw);
        if (!m && s.fx) m = make_module(d, s.base, s.base_nw);
        spv_free(&s);
        if (!m) continue;
        tmp[i] = m; cc[i].stage.module = m;
        unl[i].prev = (VkBaseInStructure *)prev; unl[i].node = (const void *)mi;
        if (prev) ((VkBaseInStructure *)prev)->pNext = mi->pNext; else cc[i].stage.pNext = mi->pNext;
    }
    VkResult r = real_ccp(d, pc_for(d, pc), n, cc ? cc : ci, a, out);
    if (cc) {
        for (uint32_t i = 0; i < n; i++) {
            if (unl[i].prev) unl[i].prev->pNext = unl[i].node;
            if (tmp[i]) real_dsm(d, tmp[i], NULL);
        }
        free(cc); free(tmp); free(unl);
    }
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

/* ======================================================================
 * 7. Estado por command buffer (MK-16 e gl_DrawID)
 * ====================================================================== */
typedef struct {
    VkCommandPool pool;
    PInfo pipe; int has_pipe;
    VkCullModeFlags app_cull, sent; uint8_t cull_valid, sent_valid;
    uint32_t pc_val; uint8_t pc_valid;
} CbSt;
static HMap cbs = HMAP_INIT;

static void cb_reset(CbSt *s) { VkCommandPool p = s->pool; memset(s, 0, sizeof *s); s->pool = p; }
static CbSt *cb_get(VkCommandBuffer cb) {
    CbSt *s = hget(&cbs, HK(cb));
    if (!s) {   /* command buffer que não passou pelo vkAllocateCommandBuffers do vkshim */
        s = calloc(1, sizeof *s);
        if (s) { CbSt *o = hput(&cbs, HK(cb), s); if (o) { free(s); s = o; hput(&cbs, HK(cb), o); } }
    }
    return s;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_acb(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    VkResult r = real_acb(d, ai, out);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
            CbSt *s = calloc(1, sizeof *s);
            if (s) { s->pool = ai->commandPool; free(hput(&cbs, HK(out[i]), s)); }
        }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL my_fcb(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *c) {
    for (uint32_t i = 0; i < n; i++) if (c[i]) { free(hdel(&cbs, HK(c[i]))); compat_cb_free(c[i]); }
    real_fcb(d, p, n, c);
}
static VKAPI_ATTR void VKAPI_CALL my_dcp(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) {
    /* os command buffers do pool somem junto */
    pthread_mutex_lock(&cbs.mu);
    for (int b = 0; b < 1024; b++)
        for (HEnt **pp = &cbs.b[b]; *pp;) {
            CbSt *s = (*pp)->v;
            if (s->pool == p) { HEnt *e = *pp; *pp = e->next; compat_cb_free((VkCommandBuffer)(uintptr_t)e->k); free(e->v); free(e); }
            else pp = &(*pp)->next;
        }
    pthread_mutex_unlock(&cbs.mu);
    real_dcp(d, p, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_bcb(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    CbSt *s = hget(&cbs, HK(cb));
    if (s) cb_reset(s);
    compat_cb_begin(cb);
    return real_bcb(cb, bi);
}
static VKAPI_ATTR void VKAPI_CALL my_cec(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec) {
    real_cec(cb, n, sec);
    CbSt *s = hget(&cbs, HK(cb));
    if (s) cb_reset(s);     /* depois de executar secundários o estado do primário é indefinido */
}

/* MK-16: com cull mode dinâmico o driver descarta pontos e linhas */
static void cull_apply(VkCommandBuffer cb, CbSt *s) {
    if (!s->cull_valid) return;
    VkCullModeFlags want = s->app_cull;
    if (s->has_pipe && s->pipe.dyncull && s->pipe.cls && !s->pipe.skip) want = VK_CULL_MODE_NONE;
    if (!s->sent_valid || s->sent != want) {
        real_scm(cb, want);
        if (want != s->app_cull) __sync_fetch_and_add(&n_fix_cull, 1);
        s->sent = want; s->sent_valid = 1;
    }
}
static VKAPI_ATTR void VKAPI_CALL my_scm(VkCommandBuffer cb, VkCullModeFlags m) {
    CbSt *s = errata_on ? cb_get(cb) : NULL;
    if (!s) { real_scm(cb, m); return; }
    s->app_cull = m; s->cull_valid = 1;
    if (!s->has_pipe || !s->pipe.dyncull) { real_scm(cb, m); s->sent = m; s->sent_valid = 1; return; }
    cull_apply(cb, s);
}

static VKAPI_ATTR void VKAPI_CALL my_cbp(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    real_cbp(cb, bp, p);
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    CbSt *s = cb_get(cb);
    if (!s) return;
    PInfo *pi = hget(&pipes, HK(p));
    if (pi) { s->pipe = *pi; s->has_pipe = 1; }
    else { memset(&s->pipe, 0, sizeof s->pipe); s->has_pipe = 0; }
    s->pc_valid = 0;
    if (s->has_pipe && s->pipe.cnt) { uint64_t z = 0; real_cpush(cb, s->pipe.layout, s->pipe.pc_stages, s->pipe.pc_off8, 8, &z); }
    if (!errata_on) return;
    if (s->has_pipe && s->pipe.dyncull) cull_apply(cb, s);
    else { s->cull_valid = 0; s->sent_valid = 0; }   /* pipeline com cull estático encerra o valor dinâmico */
}

/* gl_DrawID: grava o índice do draw no push constant do pipeline ligado */
static inline CbSt *drawid_cb(VkCommandBuffer cb) {
    if (!any_drawid_pipe) return NULL;
    CbSt *s = hget(&cbs, HK(cb));
    return (s && s->has_pipe && s->pipe.drawid) ? s : NULL;
}
static inline void drawid_set(VkCommandBuffer cb, CbSt *s, uint32_t v) {
    if (s->pc_valid && s->pc_val == v) return;
    real_cpush(cb, s->pipe.layout, s->pipe.pc_stages, s->pipe.pc_off, 4, &v);
    s->pc_val = v; s->pc_valid = 1;
}
static void shim_drawid(VkCommandBuffer cb, uint32_t i) { CbSt *s = drawid_cb(cb); if (s) drawid_set(cb, s, i); hud_draws++; }
static VKAPI_ATTR void VKAPI_CALL my_cdraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    hud_draws++;
    CbSt *s = drawid_cb(cb);
    if (s) drawid_set(cb, s, 0);
    real_cdraw(cb, vc, ic, fv, fi);
}
static VKAPI_ATTR void VKAPI_CALL my_cdrawi(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi) {
    hud_draws++;
    CbSt *s = drawid_cb(cb);
    if (s) drawid_set(cb, s, 0);
    real_cdrawi(cb, xc, ic, fx, vo, fi);
}
static VKAPI_ATTR void VKAPI_CALL my_cdi(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) {
    hud_draws += n;
    CbSt *s = drawid_cb(cb);
    if (n <= 1 || realf.multiDrawIndirect) { if (s) drawid_set(cb, s, 0); real_cdi(cb, b, off, n, stride); return; }
    for (uint32_t i = 0; i < n; i++) { if (s) drawid_set(cb, s, i); real_cdi(cb, b, off + (VkDeviceSize)i * stride, 1, stride); }
}
static VKAPI_ATTR void VKAPI_CALL my_cdii(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) {
    hud_draws += n;
    CbSt *s = drawid_cb(cb);
    if (n <= 1 || realf.multiDrawIndirect) { if (s) drawid_set(cb, s, 0); real_cdii(cb, b, off, n, stride); return; }
    for (uint32_t i = 0; i < n; i++) { if (s) drawid_set(cb, s, i); real_cdii(cb, b, off + (VkDeviceSize)i * stride, 1, stride); }
}
static VKAPI_ATTR void VKAPI_CALL my_csdce(VkCommandBuffer cb, VkBool32 e) { real_csdce(cb, realf.depthClamp ? e : VK_FALSE); }
static VKAPI_ATTR void VKAPI_CALL my_cspm(VkCommandBuffer cb, VkPolygonMode m) { real_cspm(cb, realf.fillModeNonSolid ? m : VK_POLYGON_MODE_FILL); }
static VKAPI_ATTR void VKAPI_CALL my_csvwc(VkCommandBuffer cb, uint32_t n, const VkViewport *v) { real_csvwc(cb, (!realf.multiViewport && n > 1) ? 1 : n, v); }
static VKAPI_ATTR void VKAPI_CALL my_csswc(VkCommandBuffer cb, uint32_t n, const VkRect2D *s) { real_csswc(cb, (!realf.multiViewport && n > 1) ? 1 : n, s); }

/* ======================================================================
 * 8. Imagens e views (MK-07, MK-06)
 * ====================================================================== */
typedef struct { uint32_t depth, mips; uint8_t is3d, external; } ImgInfo;
static HMap imgs = HMAP_INIT;      /* só imagens 3D e externas */
typedef struct { VkImage img; uint8_t seen; } ViewInfo;
static HMap views = HMAP_INIT;

static VKAPI_ATTR VkResult VKAPI_CALL my_cimg(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    if (is_bc_format(ci->format) && __sync_fetch_and_add(&bc_seen, 1) == 0) {
        bc_write(0, 1);
        if (bc_skipped) LOGI("jogo criou imagem BC sem o PixelBC: na próxima abertura ele volta");
    }
    VkResult r = real_cimg(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    int ext = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO || b->sType == (VkStructureType)1000010000 /* NATIVE_BUFFER_ANDROID */ ||
            b->sType == VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR || b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT ||
            b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT || b->sType == (VkStructureType)1000129005 /* EXTERNAL_FORMAT_ANDROID */) ext = 1;
    if (ci->imageType == VK_IMAGE_TYPE_3D || ext) {
        ImgInfo *ii = calloc(1, sizeof *ii);
        if (ii) { ii->depth = ci->extent.depth; ii->mips = ci->mipLevels; ii->is3d = ci->imageType == VK_IMAGE_TYPE_3D; ii->external = (uint8_t)ext; free(hput(&imgs, HK(*out), ii)); }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_civ(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    VkImageViewCreateInfo c2;
    const VkImageViewCreateInfo *use = ci;
    ImgInfo *ii = hget(&imgs, HK(ci->image));
    /* MK-07: view 2D/2D array de imagem 3D com VK_REMAINING_*: contagens explícitas */
    if (ii && ii->is3d && (ci->viewType == VK_IMAGE_VIEW_TYPE_2D || ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) &&
        (ci->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS || ci->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS)) {
        c2 = *ci;
        VkImageSubresourceRange *sr = &c2.subresourceRange;
        if (sr->levelCount == VK_REMAINING_MIP_LEVELS) sr->levelCount = ii->mips > sr->baseMipLevel ? ii->mips - sr->baseMipLevel : 1;
        if (sr->layerCount == VK_REMAINING_ARRAY_LAYERS) {
            uint32_t sl = ii->depth >> sr->baseMipLevel; if (!sl) sl = 1;
            sr->layerCount = sl > sr->baseArrayLayer ? sl - sr->baseArrayLayer : 1;
        }
        use = &c2;
        __sync_fetch_and_add(&n_fix_views, 1);
    }
    VkResult r = real_civ(d, use, a, out);
    if (r == VK_SUCCESS && !(ii && ii->external)) {
        ViewInfo *vi = calloc(1, sizeof *vi);
        if (vi) { vi->img = ci->image; free(hput(&views, HK(*out), vi)); }
    }
    return r;
}

/* MK-06: fila de views usadas em renderização dinâmica cuja destruição foi adiada */
#define QCAP 4096
static struct { VkDevice dev; VkImageView v; VkImage img; } quar[QCAP];
static int q_head, q_count;
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;

static void quar_flush(VkDevice d, VkImage only) {
    if (!real_div) return;
    pthread_mutex_lock(&q_mu);
    int keep = 0;
    for (int k = 0; k < q_count; k++) {
        int i = (q_head + k) % QCAP;
        if (quar[i].dev == d && (only == VK_NULL_HANDLE || quar[i].img == only)) real_div(d, quar[i].v, NULL);
        else quar[(q_head + keep++) % QCAP] = quar[i];
    }
    q_count = keep;
    pthread_mutex_unlock(&q_mu);
}

static VKAPI_ATTR void VKAPI_CALL my_div(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) {
    ViewInfo *vi = v ? hdel(&views, HK(v)) : NULL;
    if (vi && vi->seen && errata_on && !a) {
        pthread_mutex_lock(&q_mu);
        if (q_count == QCAP) {   /* fila cheia: a mais antiga vai de verdade */
            real_div(quar[q_head].dev, quar[q_head].v, NULL);
            q_head = (q_head + 1) % QCAP; q_count--;
        }
        int i = (q_head + q_count) % QCAP;
        quar[i].dev = d; quar[i].v = v; quar[i].img = vi->img;
        q_count++;
        pthread_mutex_unlock(&q_mu);
        free(vi);
        __sync_fetch_and_add(&n_quar, 1);
        return;
    }
    free(vi);
    real_div(d, v, a);
}

static VKAPI_ATTR void VKAPI_CALL my_dimg(VkDevice d, VkImage img, const VkAllocationCallbacks *a) {
    if (img) {
        quar_flush(d, img);       /* views em quarentena desta imagem vão antes dela */
        free(hdel(&imgs, HK(img)));
    }
    real_dimg(d, img, a);
}

/* ======================================================================
 * 9. Layout de stencil separado (MK-11) e renderização dinâmica
 * ====================================================================== */
static int has_stencil(VkFormat f) {
    return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
static int has_depth(VkFormat f) { return f != VK_FORMAT_S8_UINT; }
/* 0 = escrita, 1 = só leitura, -1 = outro */
static int dclass(VkImageLayout l) {
    switch ((int)l) {
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL: return 0;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL: return 1;
    }
    return -1;
}
static int sclass(VkImageLayout l) {
    switch ((int)l) {
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL: return 0;
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL: return 1;
    }
    return -1;
}
/* layout combinado que significa (depth = d, stencil = s); 0 se não existe */
static VkImageLayout combine(VkImageLayout d, VkImageLayout s, VkFormat f) {
    if (!has_depth(f)) return s;
    if (d == s) return d;
    int dc = dclass(d), sc = sclass(s);
    if (dc < 0 || sc < 0) return (VkImageLayout)0;
    static const VkImageLayout t[2][2] = {
        { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL },
        { VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
    };
    return t[dc][sc];
}

typedef struct { VkImageLayout *p; VkImageLayout v; } SavedL;
typedef struct { VkBaseInStructure *prev; const VkBaseInStructure *node; const void **headp; } SavedN;

/* tira um nó de cadeia (guarda para restaurar) */
static void unlink_node(const void **headp, VkStructureType st, SavedN *sv, int *nsv, int max) {
    const VkBaseInStructure *prev = NULL;
    for (const VkBaseInStructure *b = *headp; b; prev = b, b = b->pNext) {
        if (b->sType != st) continue;
        if (*nsv >= max) return;
        sv[*nsv].prev = (VkBaseInStructure *)prev; sv[*nsv].node = b; sv[*nsv].headp = headp; (*nsv)++;
        if (prev) ((VkBaseInStructure *)prev)->pNext = b->pNext; else *headp = b->pNext;
        return;
    }
}
static const void *find_node(const void *head, VkStructureType st) {
    for (const VkBaseInStructure *b = head; b; b = b->pNext) if (b->sType == st) return b;
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_crp2(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    if (!errata_on) return real_crp2(d, ci, a, out);
    enum { MAXS = 256 };
    SavedL sl[MAXS]; int nsl = 0;
    SavedN sn[MAXS]; int nsn = 0;
    int fixed = 0;
    /* descrições de anexos */
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        VkAttachmentDescription2 *ad = (VkAttachmentDescription2 *)&ci->pAttachments[i];
        const VkAttachmentDescriptionStencilLayout *sl_ = find_node(ad->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT);
        if (!sl_ || !has_stencil(ad->format)) continue;
        VkImageLayout ci0 = combine(ad->initialLayout, sl_->stencilInitialLayout, ad->format);
        VkImageLayout cf0 = combine(ad->finalLayout, sl_->stencilFinalLayout, ad->format);
        if (!ci0 || !cf0 || nsl + 2 > MAXS) continue;
        sl[nsl].p = &ad->initialLayout; sl[nsl++].v = ad->initialLayout; ad->initialLayout = ci0;
        sl[nsl].p = &ad->finalLayout; sl[nsl++].v = ad->finalLayout; ad->finalLayout = cf0;
        unlink_node(&ad->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, sn, &nsn, MAXS);
        fixed++;
    }
    /* referências: depth/stencil, input e resolve de depth/stencil */
    for (uint32_t s = 0; s < ci->subpassCount; s++) {
        const VkSubpassDescription2 *sp = &ci->pSubpasses[s];
        const VkAttachmentReference2 *refs[64]; int nr = 0;
        if (sp->pDepthStencilAttachment) refs[nr++] = sp->pDepthStencilAttachment;
        for (uint32_t k = 0; k < sp->inputAttachmentCount && nr < 63; k++) refs[nr++] = &sp->pInputAttachments[k];
        const VkSubpassDescriptionDepthStencilResolve *rv = find_node(sp->pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
        if (rv && rv->pDepthStencilResolveAttachment && nr < 64) refs[nr++] = rv->pDepthStencilResolveAttachment;
        for (int k = 0; k < nr; k++) {
            VkAttachmentReference2 *ref = (VkAttachmentReference2 *)refs[k];
            if (ref->attachment == VK_ATTACHMENT_UNUSED || ref->attachment >= ci->attachmentCount) continue;
            const VkAttachmentReferenceStencilLayout *rs_ = find_node(ref->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT);
            VkFormat f = ci->pAttachments[ref->attachment].format;
            if (!rs_ || !has_stencil(f)) continue;
            VkImageLayout c = combine(ref->layout, rs_->stencilLayout, f);
            if (!c || nsl + 1 > MAXS) continue;
            sl[nsl].p = &ref->layout; sl[nsl++].v = ref->layout; ref->layout = c;
            unlink_node(&ref->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, sn, &nsn, MAXS);
            fixed++;
        }
    }
    VkResult r = real_crp2(d, ci, a, out);
    for (int i = nsn - 1; i >= 0; i--) {
        if (sn[i].prev) sn[i].prev->pNext = sn[i].node; else *sn[i].headp = sn[i].node;
    }
    for (int i = nsl - 1; i >= 0; i--) *sl[i].p = sl[i].v;
    if (fixed) __sync_fetch_and_add(&n_fix_stencil, fixed);
    return r;
}

static void mark_seen(VkImageView v) {
    if (!v) return;
    ViewInfo *vi = hget(&views, HK(v));
    if (vi) vi->seen = 1;
}
static VKAPI_ATTR void VKAPI_CALL my_cbr(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (!errata_on) { real_cbr(cb, ri); return; }
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) { mark_seen(ri->pColorAttachments[i].imageView); mark_seen(ri->pColorAttachments[i].resolveImageView); }
    if (ri->pDepthAttachment) { mark_seen(ri->pDepthAttachment->imageView); mark_seen(ri->pDepthAttachment->resolveImageView); }
    if (ri->pStencilAttachment) { mark_seen(ri->pStencilAttachment->imageView); mark_seen(ri->pStencilAttachment->resolveImageView); }
    /* MK-11 também na renderização dinâmica: depth e stencil na mesma view com layouts diferentes */
    const VkRenderingAttachmentInfo *da = ri->pDepthAttachment, *sa = ri->pStencilAttachment;
    if (da && sa && da->imageView && da->imageView == sa->imageView && da->imageLayout != sa->imageLayout) {
        VkImageLayout c = combine(da->imageLayout, sa->imageLayout, VK_FORMAT_D24_UNORM_S8_UINT);
        if (c) {
            VkRenderingInfo r2 = *ri;
            VkRenderingAttachmentInfo d2 = *da, s2 = *sa;
            d2.imageLayout = s2.imageLayout = c;
            r2.pDepthAttachment = &d2; r2.pStencilAttachment = &s2;
            __sync_fetch_and_add(&n_fix_stencil, 1);
            real_cbr(cb, &r2);
            return;
        }
    }
    real_cbr(cb, ri);
}

/* ---- draw_indirect_count (v4.9.4) ----
 * N = maxDrawCount draws indiretos de 1, cada um com o índice no push constant e o endereço do
 * contador ao lado; o VS do pipeline (variante) descarta os draws com índice >= contador. */
static int warned_cnt;
static void draw_count(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkBuffer cbuf, VkDeviceSize coff,
                       uint32_t maxn, uint32_t stride, int indexed) {
    CbSt *s = cb_get(cb);
    int ok = s && s->has_pipe && s->pipe.cnt && real_gbda && g_dev;
    if (ok) {
        VkBufferDeviceAddressInfo bi = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, cbuf };
        uint64_t addr = real_gbda(g_dev, &bi) + coff;
        real_cpush(cb, s->pipe.layout, s->pipe.pc_stages, s->pipe.pc_off8, 8, &addr);
    } else if (!warned_cnt) { warned_cnt = 1; LOGE("draw_indirect_count: pipeline sem a variante do contador (desenha os %u)", maxn); }
    hud_draws += maxn;
    for (uint32_t i = 0; i < maxn; i++) {
        if (s && s->has_pipe && s->pipe.drawid) drawid_set(cb, s, i);
        if (indexed) real_cdii(cb, b, off + (VkDeviceSize)i * stride, 1, stride);
        else real_cdi(cb, b, off + (VkDeviceSize)i * stride, 1, stride);
    }
    if (ok) { uint64_t z = 0; real_cpush(cb, s->pipe.layout, s->pipe.pc_stages, s->pipe.pc_off8, 8, &z); }
}
static VKAPI_ATTR void VKAPI_CALL my_cdic(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkBuffer cbuf, VkDeviceSize coff, uint32_t maxn, uint32_t stride) {
    draw_count(cb, b, off, cbuf, coff, maxn, stride, 0);
}
static VKAPI_ATTR void VKAPI_CALL my_cdiic(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkBuffer cbuf, VkDeviceSize coff, uint32_t maxn, uint32_t stride) {
    draw_count(cb, b, off, cbuf, coff, maxn, stride, 1);
}

/* ---- samplers (v5.7) ---- */
static PFN_vkCreateSampler real_csmp;
static int n_smp_mod;
static VKAPI_ATTR VkResult VKAPI_CALL my_csmp(VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    VkSamplerCreateInfo c = *ci;
    if (smp_aniso >= 0 && c.anisotropyEnable) {
        if (smp_aniso <= 1) c.anisotropyEnable = VK_FALSE;
        else if (c.maxAnisotropy > (float)smp_aniso) c.maxAnisotropy = (float)smp_aniso;
    }
    if (smp_lodbias != 0.0f) c.mipLodBias += smp_lodbias;
    if (smp_mipnear && c.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR) c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (memcmp(&c, ci, sizeof c)) __sync_fetch_and_add(&n_smp_mod, 1);
    return real_csmp(d, &c, a, out);
}

/* ---- HUD (v4.9) ---- */
static VKAPI_ATTR VkResult VKAPI_CALL my_qs(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) { return hud_submit(q, n, s, f, real_qs); }
static VKAPI_ATTR VkResult VKAPI_CALL my_qs2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f) { return hud_submit2(q, n, s, f, real_qs2); }
static VKAPI_ATTR void VKAPI_CALL my_gdq(VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) { real_gdq(d, fam, idx, q); hud_queue(d, *q, fam); }
static VKAPI_ATTR void VKAPI_CALL my_gdq2(VkDevice d, const VkDeviceQueueInfo2 *i, VkQueue *q) { real_gdq2(d, i, q); hud_queue(d, *q, i->queueFamilyIndex); }
static PFN_vkWaitForFences real_wff; static PFN_vkWaitSemaphores real_wsem;
static PFN_vkQueueWaitIdle real_qwi; static PFN_vkDeviceWaitIdle real_dwi;
#define TIMED(call) do { long long _t = mono_ns(); r = call; __sync_fetch_and_add(&hud_wait_ns, mono_ns() - _t); } while (0)
static VKAPI_ATTR VkResult VKAPI_CALL my_wff(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t to) { VkResult r; TIMED(real_wff(d, n, f, all, to)); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL my_wsem(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t to) { VkResult r; TIMED(real_wsem(d, i, to)); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL my_qwi(VkQueue q) { VkResult r; TIMED(real_qwi(q)); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL my_dwi(VkDevice d) { VkResult r; TIMED(real_dwi(d)); return r; }
/* limitador de FPS (v5.7): cada apresentação sai num horário fixo (relógio absoluto), então o
 * intervalo entre quadros fica constante. Atraso de mais de um quadro reinicia o relógio. */
static long long fps_next;
static void fps_pace(void) {
    long long period = 1000000000LL / fps_cap, now = mono_ns();
    if (!fps_next || now - fps_next > period) { fps_next = now + period; return; }
    if (now < fps_next) {
        struct timespec ts = { (time_t)(fps_next / 1000000000LL), (long)(fps_next % 1000000000LL) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    fps_next += period;
}
static VKAPI_ATTR VkResult VKAPI_CALL my_sria(VkQueue q, uint32_t n, const VkSemaphore *w, VkImage img, int *fd) {
    if (fps_cap) fps_pace();
    VkResult r = real_sria(q, n, w, img, fd);
    if (hud_on) hud_present();
    return r;
}

/* ======================================================================
 * Tabela de interceptação
 * ====================================================================== */
#define HOOK(name, realvar, mine) \
    if (!strcmp(n, name)) { if (r) realvar = (void *)r; return r ? (PFN_vkVoidFunction)mine : NULL; }

static PFN_vkVoidFunction hook(const char *n, PFN_vkVoidFunction r) {
    if (n[0] != 'v' || n[1] != 'k') return r;
    { PFN_vkVoidFunction cf = compat_hook(n, r); if (cf) return cf; }
    if (compat_on) {     /* o driver não tem (ou não deve ser usado: recurso 0) */
        if (!strcmp(n, "vkCmdDrawIndirectCount") || !strcmp(n, "vkCmdDrawIndirectCountKHR") || !strcmp(n, "vkCmdDrawIndirectCountAMD")) return (PFN_vkVoidFunction)my_cdic;
        if (!strcmp(n, "vkCmdDrawIndexedIndirectCount") || !strcmp(n, "vkCmdDrawIndexedIndirectCountKHR") || !strcmp(n, "vkCmdDrawIndexedIndirectCountAMD")) return (PFN_vkVoidFunction)my_cdiic;
    }
    if (!r) return r;
    if (compat_on) {
        HOOK("vkCreatePipelineLayout", real_cpl, my_cpl)
        HOOK("vkDestroyPipelineLayout", real_dpl, my_dpl)
        HOOK("vkGetPhysicalDeviceMemoryProperties2", real_gpdmp2, my_gpdmp2)
        HOOK("vkGetPhysicalDeviceMemoryProperties2KHR", real_gpdmp2, my_gpdmp2)
    }
    /* dispositivo */
    HOOK("vkCreateShaderModule", real_csm, my_csm)
    HOOK("vkDestroyShaderModule", real_dsm, my_dsm)
    HOOK("vkCreateGraphicsPipelines", real_cgp, my_cgp)
    HOOK("vkCreateComputePipelines", real_ccp, my_ccp)
    HOOK("vkDestroyPipeline", real_dp, my_dp)
    HOOK("vkDestroyDevice", real_dd, my_dd)
    HOOK("vkAllocateCommandBuffers", real_acb, my_acb)
    HOOK("vkFreeCommandBuffers", real_fcb, my_fcb)
    HOOK("vkDestroyCommandPool", real_dcp, my_dcp)
    HOOK("vkBeginCommandBuffer", real_bcb, my_bcb)
    HOOK("vkCmdExecuteCommands", real_cec, my_cec)
    HOOK("vkCmdBindPipeline", real_cbp, my_cbp)
    HOOK("vkCmdSetCullMode", real_scm, my_scm)
    HOOK("vkCmdSetCullModeEXT", real_scm, my_scm)
    if (smp_aniso >= 0 || smp_lodbias != 0.0f || smp_mipnear) { HOOK("vkCreateSampler", real_csmp, my_csmp) }
    if (fps_cap && !hud_on) { HOOK("vkQueueSignalReleaseImageANDROID", real_sria, my_sria) }
    if (hud_on) {
        HOOK("vkQueueSubmit", real_qs, my_qs)
        HOOK("vkQueueSubmit2", real_qs2, my_qs2)
        HOOK("vkQueueSubmit2KHR", real_qs2, my_qs2)
        HOOK("vkGetDeviceQueue", real_gdq, my_gdq)
        HOOK("vkGetDeviceQueue2", real_gdq2, my_gdq2)
        HOOK("vkQueueSignalReleaseImageANDROID", real_sria, my_sria)
        HOOK("vkWaitForFences", real_wff, my_wff)
        HOOK("vkWaitSemaphores", real_wsem, my_wsem)
        HOOK("vkWaitSemaphoresKHR", real_wsem, my_wsem)
        HOOK("vkQueueWaitIdle", real_qwi, my_qwi)
        HOOK("vkDeviceWaitIdle", real_dwi, my_dwi)
        HOOK("vkCmdDraw", real_cdraw, my_cdraw)
        HOOK("vkCmdDrawIndexed", real_cdrawi, my_cdrawi)
        HOOK("vkCmdDrawIndirect", real_cdi, my_cdi)
        HOOK("vkCmdDrawIndexedIndirect", real_cdii, my_cdii)
    }
    if (fake & F_MDI) {
        HOOK("vkCmdDrawIndirect", real_cdi, my_cdi)
        HOOK("vkCmdDrawIndexedIndirect", real_cdii, my_cdii)
        if (drawid_on) {
            HOOK("vkCmdDraw", real_cdraw, my_cdraw)
            HOOK("vkCmdDrawIndexed", real_cdrawi, my_cdrawi)
            HOOK("vkCreatePipelineLayout", real_cpl, my_cpl)
            HOOK("vkDestroyPipelineLayout", real_dpl, my_dpl)
        }
    }
    if (errata_on) {
        HOOK("vkCreateImage", real_cimg, my_cimg)
        HOOK("vkDestroyImage", real_dimg, my_dimg)
        HOOK("vkCreateImageView", real_civ, my_civ)
        HOOK("vkDestroyImageView", real_div, my_div)
        HOOK("vkCmdBeginRendering", real_cbr, my_cbr)
        HOOK("vkCmdBeginRenderingKHR", real_cbr, my_cbr)
        HOOK("vkCreateRenderPass2", real_crp2, my_crp2)
        HOOK("vkCreateRenderPass2KHR", real_crp2, my_crp2)
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
    if (r && !strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)my_gdpa;
    return hook(name, r);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance inst, const char *name) {
    PFN_vkVoidFunction r = real_gipa(inst, name);
    if (!r) return inst ? hook(name, r) : r;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)my_gipa;
    if (!strcmp(name, "vkGetDeviceProcAddr")) { real_gdpa = (PFN_vkGetDeviceProcAddr)r; return (PFN_vkVoidFunction)my_gdpa; }
    if (!strcmp(name, "vkCreateInstance")) { real_ci = (PFN_vkCreateInstance)r; return (PFN_vkVoidFunction)my_ci; }
    return hook(name, r);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ci(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    VkResult r = real_ci(ci, a, out);
    if (r == VK_SUCCESS) {
        g_inst = *out;
        compat_gipa = real_gipa; compat_inst = *out;
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
    compat_drawid = shim_drawid;
    hud_init(pkg);
    bc_load();
    if (bcn_on && !bcn_forced && bc_prev_sessions >= BC_SESSOES) {
        bcn_on = 0; bc_skipped = 1;
        LOGI("PixelBC fora: %d sessões seguidas sem textura BC neste jogo (debug.vkshim.bcn=1 força)", bc_prev_sessions);
    }
    store_on = (fake & (F_CLIPDIST | F_DEPTHCLAMP | F_MDI | F_NONSOLID)) && (clip_on || clamp_on || drawid_on || nonsolid_on);
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
    LOGI("vkshim v%s sobre %s | fake=0x%x clip=%d clamp=%d drawid=%d wire=%d errata=%d spv=%d pcache=%d bcn=%d hud=%d debug=%d | aniso=%d lodbias=%.2f mip=%s fp16=%d fps=%d | %s%s%s",
         VKSHIM_VERSION, hal, fake, clip_on, clamp_on, drawid_on, nonsolid_on, errata_on, spv_on, pcache_on, bcn_on, hud_on, dbg,
         smp_aniso, smp_lodbias, smp_mipnear ? "near" : "app", fp16_on, fps_cap, pkg,
         conf_used[0] ? " | conf " : "", conf_used);
    return 0;
}

static hw_module_methods_t my_methods = { .open = my_open };

__attribute__((visibility("default"))) hw_module_t HMI = {
    .tag = MAKE_TAG('H', 'W', 'M', 'T'),
    .module_api_version = 0x0001,
    .hal_api_version = 0x0100,
    .id = "vulkan",
#ifdef PORTABLE
    .name = "vkshim v4.7 portatil (r49 wrapper)",
#else
    .name = "vkshim v4.7 (r49 wrapper)",
#endif
    .author = "rosemary",
    .methods = &my_methods,
};
