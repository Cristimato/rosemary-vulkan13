// compat (v4.9.2): extensões emuladas no vkshim. Veja compat.h.
#include "compat.h"
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vkshim", __VA_ARGS__)
#define HK(h) ((uint64_t)(uintptr_t)(h))

int compat_on = 1;
int compat_count_on;            /* o app ligou drawIndirectCount: endereço do contador por BDA */
void (*compat_drawid)(VkCommandBuffer cb, uint32_t i);
PFN_vkGetInstanceProcAddr compat_gipa;
VkInstance compat_inst;

/* ------------------------------------------------------------------ mapa simples handle -> ponteiro */
typedef struct CE { uint64_t k; void *v; struct CE *n; } CE;
typedef struct { CE *b[256]; pthread_mutex_t mu; } CMap;
#define CMAP_INIT { { 0 }, PTHREAD_MUTEX_INITIALIZER }
static unsigned cidx(uint64_t k) { k ^= k >> 29; k *= 0xbf58476d1ce4e5b9ull; k ^= k >> 32; return (unsigned)(k & 255); }
static void *cget(CMap *m, uint64_t k) {
    void *v = NULL; pthread_mutex_lock(&m->mu);
    for (CE *e = m->b[cidx(k)]; e; e = e->n) if (e->k == k) { v = e->v; break; }
    pthread_mutex_unlock(&m->mu); return v;
}
static void cput(CMap *m, uint64_t k, void *v) {
    CE *n = malloc(sizeof *n); if (!n) return;
    pthread_mutex_lock(&m->mu);
    for (CE *e = m->b[cidx(k)]; e; e = e->n) if (e->k == k) { free(e->v); e->v = v; free(n); pthread_mutex_unlock(&m->mu); return; }
    n->k = k; n->v = v; n->n = m->b[cidx(k)]; m->b[cidx(k)] = n;
    pthread_mutex_unlock(&m->mu);
}
static void *cdel(CMap *m, uint64_t k) {
    void *v = NULL; pthread_mutex_lock(&m->mu);
    for (CE **pp = &m->b[cidx(k)]; *pp; pp = &(*pp)->n) if ((*pp)->k == k) { CE *e = *pp; v = e->v; *pp = e->n; free(e); break; }
    pthread_mutex_unlock(&m->mu); return v;
}

/* ------------------------------------------------------------------ extensões */
enum { X_ROBUST2, X_PUSH, X_DIVISOR, X_DEPTHCLIP, X_BUDGET, X_PRIORITY, X_PAGEABLE, X_MULTIDRAW, X_DESCIDX, X_DRAWCOUNT, X_DCC, NX };
static const VkExtensionProperties xprops[NX] = {
    { "VK_EXT_robustness2", 1 }, { "VK_KHR_push_descriptor", 2 }, { "VK_EXT_vertex_attribute_divisor", 3 },
    { "VK_EXT_depth_clip_enable", 1 }, { "VK_EXT_memory_budget", 1 }, { "VK_EXT_memory_priority", 1 },
    { "VK_EXT_pageable_device_local_memory", 1 }, { "VK_EXT_multi_draw", 1 }, { "VK_EXT_descriptor_indexing", 2 },
    { "VK_KHR_draw_indirect_count", 1 }, { "VK_EXT_depth_clip_control", 1 },
};
static int have_khr_divisor = -1;

uint32_t compat_exts(VkExtensionProperties *out, uint32_t max, const VkExtensionProperties *real, uint32_t nreal) {
    if (!compat_on) return 0;
    uint32_t k = 0;
    int khrdiv = 0;
    for (uint32_t j = 0; j < nreal; j++) if (!strcmp(real[j].extensionName, "VK_KHR_vertex_attribute_divisor")) khrdiv = 1;
    have_khr_divisor = khrdiv;
    for (int i = 0; i < NX && k < max; i++) {
        if (i == X_DIVISOR && !khrdiv) continue;
        int dup = 0;
        for (uint32_t j = 0; j < nreal; j++) if (!strcmp(real[j].extensionName, xprops[i].extensionName)) dup = 1;
        if (!dup) out[k++] = xprops[i];
    }
    return k;
}

/* descriptor indexing (v4.9.3): o que o vkshim garante
 *   índice não uniforme: buffers nativos; imagens/texel por waterfall
 *   update-after-bind: imagens e texel nativos; buffers usam o descritor do momento do bind
 *   partially bound: buracos preenchidos com fantasmas; variable count: nativo */
#define DI_FIELDS(v) \
    v->shaderUniformTexelBufferArrayDynamicIndexing = v->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE; \
    v->shaderUniformBufferArrayNonUniformIndexing = v->shaderSampledImageArrayNonUniformIndexing = VK_TRUE; \
    v->shaderStorageBufferArrayNonUniformIndexing = v->shaderStorageImageArrayNonUniformIndexing = VK_TRUE; \
    v->shaderUniformTexelBufferArrayNonUniformIndexing = v->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE; \
    v->descriptorBindingUniformBufferUpdateAfterBind = v->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE; \
    v->descriptorBindingStorageImageUpdateAfterBind = v->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE; \
    v->descriptorBindingUniformTexelBufferUpdateAfterBind = v->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE; \
    v->descriptorBindingUpdateUnusedWhilePending = v->descriptorBindingPartiallyBound = VK_TRUE; \
    v->descriptorBindingVariableDescriptorCount = v->runtimeDescriptorArray = VK_TRUE;
int compat_di_on = 1;

void compat_features(VkPhysicalDevice pd, void *pnext) {
    (void)pd;
    if (!compat_on) return;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext) {
        if (compat_di_on && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *v = (void *)b; DI_FIELDS(v) v->descriptorIndexing = VK_TRUE; v->drawIndirectCount = VK_TRUE; continue;
        }
        if (compat_di_on && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            VkPhysicalDeviceDescriptorIndexingFeatures *v = (void *)b; DI_FIELDS(v) continue;
        }
        switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
            VkPhysicalDeviceRobustness2FeaturesEXT *f = (void *)b;
            f->robustBufferAccess2 = f->robustImageAccess2 = f->nullDescriptor = VK_TRUE; break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
            ((VkPhysicalDeviceDepthClipEnableFeaturesEXT *)b)->depthClipEnable = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT:
            ((VkPhysicalDeviceMemoryPriorityFeaturesEXT *)b)->memoryPriority = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT:
            ((VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT *)b)->pageableDeviceLocalMemory = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
            ((VkPhysicalDeviceDepthClipControlFeaturesEXT *)b)->depthClipControl = VK_TRUE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT:
            ((VkPhysicalDeviceMultiDrawFeaturesEXT *)b)->multiDraw = VK_TRUE; break;
        }
    }
}

#define DI_PROPS(v) \
    v->maxUpdateAfterBindDescriptorsInAllPools = 1u << 22; \
    v->shaderUniformBufferArrayNonUniformIndexingNative = v->shaderSampledImageArrayNonUniformIndexingNative = VK_FALSE; \
    v->shaderStorageBufferArrayNonUniformIndexingNative = v->shaderStorageImageArrayNonUniformIndexingNative = VK_FALSE; \
    v->shaderInputAttachmentArrayNonUniformIndexingNative = VK_FALSE; \
    v->robustBufferAccessUpdateAfterBind = VK_FALSE; v->quadDivergentImplicitLod = VK_FALSE; \
    v->maxPerStageDescriptorUpdateAfterBindSamplers = v->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 1000000; \
    v->maxPerStageDescriptorUpdateAfterBindStorageBuffers = v->maxPerStageDescriptorUpdateAfterBindSampledImages = 1000000; \
    v->maxPerStageDescriptorUpdateAfterBindStorageImages = 1000000; \
    if (v->maxPerStageDescriptorUpdateAfterBindInputAttachments < 8) v->maxPerStageDescriptorUpdateAfterBindInputAttachments = 8; \
    v->maxPerStageUpdateAfterBindResources = 1000000; \
    v->maxDescriptorSetUpdateAfterBindSamplers = v->maxDescriptorSetUpdateAfterBindUniformBuffers = 1000000; \
    v->maxDescriptorSetUpdateAfterBindStorageBuffers = v->maxDescriptorSetUpdateAfterBindSampledImages = 1000000; \
    v->maxDescriptorSetUpdateAfterBindStorageImages = 1000000; \
    if (v->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic < 8) v->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8; \
    if (v->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic < 4) v->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 4; \
    if (v->maxDescriptorSetUpdateAfterBindInputAttachments < 8) v->maxDescriptorSetUpdateAfterBindInputAttachments = 8;

void compat_props(VkPhysicalDevice pd, void *pnext) {
    if (!compat_on) return;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext) {
        if (compat_di_on && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) { VkPhysicalDeviceVulkan12Properties *v = (void *)b; DI_PROPS(v) continue; }
        if (compat_di_on && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES) { VkPhysicalDeviceDescriptorIndexingProperties *v = (void *)b; DI_PROPS(v) continue; }
        switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR:
            ((VkPhysicalDevicePushDescriptorPropertiesKHR *)b)->maxPushDescriptors = 32; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT: {
            VkPhysicalDeviceRobustness2PropertiesEXT *p = (void *)b;
            p->robustStorageBufferAccessSizeAlignment = 4; p->robustUniformBufferAccessSizeAlignment = 256; break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT:
            ((VkPhysicalDeviceMultiDrawPropertiesEXT *)b)->maxMultiDrawCount = 1u << 16; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
            /* o EXT tem só o máximo; vem do struct KHR que o driver conhece */
            VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR k = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR };
            VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &k };
            PFN_vkGetPhysicalDeviceProperties2 g = compat_gipa ? (void *)compat_gipa(compat_inst, "vkGetPhysicalDeviceProperties2") : NULL;
            if (g) g(pd, &p2);
            ((VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)b)->maxVertexAttribDivisor = k.maxVertexAttribDivisor ? k.maxVertexAttribDivisor : 0xffffffffu;
            break; }
        }
    }
}

/* ------------------------------------------------------------------ device */
static struct {
    VkDevice d;
    int robust2, push, budget, multidraw;
    VkPhysicalDeviceMemoryProperties mp;
    PFN_vkUpdateDescriptorSets uds; PFN_vkUpdateDescriptorSetWithTemplate udst;
    PFN_vkCreateDescriptorUpdateTemplate cdut; PFN_vkDestroyDescriptorUpdateTemplate ddut;
    PFN_vkCreateDescriptorSetLayout cdsl; PFN_vkGetDescriptorSetLayoutSupport gdsls;
    PFN_vkCreateDescriptorPool cdp; PFN_vkDestroyDescriptorPool ddp; PFN_vkResetDescriptorPool rdp;
    PFN_vkAllocateDescriptorSets ads; PFN_vkCmdBindDescriptorSets cbds;
    PFN_vkCmdBindVertexBuffers cbvb; PFN_vkCmdBindVertexBuffers2 cbvb2;
    PFN_vkAllocateMemory am; PFN_vkFreeMemory fm;
    PFN_vkCmdDraw draw; PFN_vkCmdDrawIndexed drawi;
    PFN_vkCreateBuffer cbuf; PFN_vkGetBufferMemoryRequirements gbmr; PFN_vkBindBufferMemory bbm;
    PFN_vkCreateImage cimg; PFN_vkGetImageMemoryRequirements gimr; PFN_vkBindImageMemory bim;
    PFN_vkCreateImageView civ; PFN_vkCreateBufferView cbv; PFN_vkCreateSampler csmp;
    PFN_vkMapMemory map; PFN_vkUnmapMemory unmap;
    PFN_vkDestroyBuffer dbuf; PFN_vkDestroyImage dimg; PFN_vkDestroyImageView div; PFN_vkDestroyBufferView dbv;
    PFN_vkDestroySampler dsmp;
} X;

struct CompatDC {
    VkBool32 *diz[40]; int ndiz;               /* campos de DI zerados durante o vkCreateDevice */
    const char **ext; VkDeviceCreateInfo c2;
    struct { VkBaseOutStructure *prev, *node; } un[16]; int nun;
    VkBool32 *rba; VkPhysicalDeviceFeatures rbaf;
    int want_robust2, want_push, want_budget, want_multidraw, want_count;
    VkBool32 *bda_set, *i64_set; VkPhysicalDeviceBufferDeviceAddressFeatures bdaf;
};

static int is_emulated(const char *e, int *which) {
    for (int i = 0; i < NX; i++) if (!strcmp(e, xprops[i].extensionName)) { *which = i; return 1; }
    return 0;
}

const VkDeviceCreateInfo *compat_device_begin(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, CompatDC **out) {
    *out = NULL;
    if (!compat_on) return ci;
    CompatDC *s = calloc(1, sizeof *s);
    if (!s) return ci;
    s->c2 = *ci;
    s->ext = malloc((ci->enabledExtensionCount + 1) * sizeof *s->ext);
    uint32_t k = 0; int need_khr_div = 0, have_khr_div = 0;
    for (uint32_t i = 0; s->ext && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i]; int w;
        if (!strcmp(e, "VK_KHR_vertex_attribute_divisor")) have_khr_div = 1;
        if (is_emulated(e, &w)) {
            if (w == X_DIVISOR) need_khr_div = 1;
            if (w == X_ROBUST2) s->want_robust2 = 1;
            if (w == X_PUSH) s->want_push = 1;
            if (w == X_BUDGET) s->want_budget = 1;
            if (w == X_MULTIDRAW) s->want_multidraw = 1;
            if (w == X_DRAWCOUNT) s->want_count = 1;
            continue;
        }
        s->ext[k++] = e;
    }
    if (s->ext && need_khr_div && !have_khr_div) s->ext[k++] = "VK_KHR_vertex_attribute_divisor";
    if (s->ext) { s->c2.enabledExtensionCount = k; s->c2.ppEnabledExtensionNames = s->ext; }
    /* structs de recurso das emuladas saem do pNext durante a chamada */
    VkBaseOutStructure *prev = NULL;
    for (VkBaseOutStructure *b = (void *)ci->pNext; b; b = b->pNext) {
        int t = (int)b->sType, drop = 0;
        if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *f = (void *)b;
            if (f->robustBufferAccess2) s->want_robust2 |= 2;
            drop = 1;
        } else if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT ||
                   t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT ||
                   t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT ||
                   t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT ||
                   t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT) drop = 1;
        if (drop && s->nun < 16) {
            s->un[s->nun].prev = prev; s->un[s->nun].node = b; s->nun++;
            if (prev) prev->pNext = b->pNext; else s->c2.pNext = b->pNext;
            continue;   /* prev fica igual */
        }
        prev = b;
    }
    /* drawIndirectCount: o driver não tem; liga bufferDeviceAddress + shaderInt64 para o VS ler o contador */
    VkPhysicalDeviceVulkan12Features *v12 = NULL; VkPhysicalDeviceBufferDeviceAddressFeatures *bdas = NULL;
    for (VkBaseOutStructure *b = (void *)s->c2.pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) v12 = (void *)b;
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) bdas = (void *)b;
    }
    if (v12 && v12->drawIndirectCount) { s->want_count = 1; if (s->ndiz < 40) { s->diz[s->ndiz++] = &v12->drawIndirectCount; v12->drawIndirectCount = VK_FALSE; } }
    if (s->want_count) {
        if (v12) { if (!v12->bufferDeviceAddress) { s->bda_set = &v12->bufferDeviceAddress; *s->bda_set = VK_TRUE; } }
        else if (bdas) { if (!bdas->bufferDeviceAddress) { s->bda_set = &bdas->bufferDeviceAddress; *s->bda_set = VK_TRUE; } }
        else { memset(&s->bdaf, 0, sizeof s->bdaf); s->bdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
               s->bdaf.bufferDeviceAddress = VK_TRUE; s->bdaf.pNext = (void *)s->c2.pNext; s->c2.pNext = &s->bdaf; }
        VkPhysicalDeviceFeatures *pf = (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
        for (VkBaseOutStructure *b = (void *)s->c2.pNext; b && !pf; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) pf = &((VkPhysicalDeviceFeatures2 *)b)->features;
        if (pf) { if (!pf->shaderInt64) { s->i64_set = &pf->shaderInt64; *s->i64_set = VK_TRUE; } }
        else { memset(&s->rbaf, 0, sizeof s->rbaf); s->rbaf.shaderInt64 = VK_TRUE; s->c2.pEnabledFeatures = &s->rbaf; }
    }
    /* descriptor indexing: o driver não tem; os campos ligados pelo app saem durante a chamada */
    for (VkBaseOutStructure *b = (void *)s->c2.pNext; b; b = b->pNext) {
        VkBool32 *f0 = NULL, *f1 = NULL;
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *v = (void *)b;
            if (v->descriptorIndexing && s->ndiz < 40) { s->diz[s->ndiz++] = &v->descriptorIndexing; v->descriptorIndexing = VK_FALSE; }
            f0 = &v->shaderInputAttachmentArrayDynamicIndexing; f1 = &v->runtimeDescriptorArray;
        } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            VkPhysicalDeviceDescriptorIndexingFeatures *v = (void *)b;
            f0 = &v->shaderInputAttachmentArrayDynamicIndexing; f1 = &v->runtimeDescriptorArray;
        }
        if (f0) for (VkBool32 *q = f0; q <= f1; q++) if (*q && q != f0 && s->ndiz < 40) { s->diz[s->ndiz++] = q; *q = VK_FALSE; }
    }
    /* robustBufferAccess2 precisa do robustBufferAccess do core */
    if (s->want_robust2 & 2) {
        VkPhysicalDeviceFeatures *pf = (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
        for (VkBaseOutStructure *b = (void *)s->c2.pNext; b && !pf; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) pf = &((VkPhysicalDeviceFeatures2 *)b)->features;
        if (pf) { if (!pf->robustBufferAccess) { s->rba = &pf->robustBufferAccess; *s->rba = VK_TRUE; } }
        else { memset(&s->rbaf, 0, sizeof s->rbaf); s->rbaf.robustBufferAccess = VK_TRUE; s->c2.pEnabledFeatures = &s->rbaf; }
    }
    if (compat_gipa) {
        PFN_vkGetPhysicalDeviceMemoryProperties g = (void *)compat_gipa(compat_inst, "vkGetPhysicalDeviceMemoryProperties");
        if (g) g(pd, &X.mp);
    }
    *out = s;
    return &s->c2;
}

#define G(f, n) X.f = (void *)gdpa(d, n)
void compat_device_done(CompatDC *s, VkDevice d, VkResult r, PFN_vkGetDeviceProcAddr gdpa) {
    if (!s) return;
    /* restaura o pNext do app (na ordem inversa) */
    for (int i = s->nun - 1; i >= 0; i--) {
        if (s->un[i].prev) s->un[i].prev->pNext = s->un[i].node;
    }
    if (s->rba) *s->rba = VK_FALSE;
    if (s->bda_set) *s->bda_set = VK_FALSE;
    if (s->i64_set) *s->i64_set = VK_FALSE;
    for (int i = 0; i < s->ndiz; i++) *s->diz[i] = VK_TRUE;
    if (r == VK_SUCCESS && gdpa) {
        VkPhysicalDeviceMemoryProperties mp = X.mp;
        memset(&X, 0, sizeof X);
        X.mp = mp; X.d = d;
        X.robust2 = 1;                              /* nulos nunca podem chegar ao driver: sempre */
        X.push = s->want_push; X.budget = s->want_budget; X.multidraw = s->want_multidraw;
        compat_count_on = s->want_count;
        G(uds, "vkUpdateDescriptorSets"); G(udst, "vkUpdateDescriptorSetWithTemplate");
        G(cdut, "vkCreateDescriptorUpdateTemplate"); G(ddut, "vkDestroyDescriptorUpdateTemplate");
        G(cdsl, "vkCreateDescriptorSetLayout"); G(gdsls, "vkGetDescriptorSetLayoutSupport");
        G(cdp, "vkCreateDescriptorPool"); G(ddp, "vkDestroyDescriptorPool"); G(rdp, "vkResetDescriptorPool");
        G(ads, "vkAllocateDescriptorSets"); G(cbds, "vkCmdBindDescriptorSets");
        G(cbvb, "vkCmdBindVertexBuffers"); G(cbvb2, "vkCmdBindVertexBuffers2");
        G(am, "vkAllocateMemory"); G(fm, "vkFreeMemory");
        G(draw, "vkCmdDraw"); G(drawi, "vkCmdDrawIndexed");
        G(cbuf, "vkCreateBuffer"); G(gbmr, "vkGetBufferMemoryRequirements"); G(bbm, "vkBindBufferMemory");
        G(cimg, "vkCreateImage"); G(gimr, "vkGetImageMemoryRequirements"); G(bim, "vkBindImageMemory");
        G(civ, "vkCreateImageView"); G(cbv, "vkCreateBufferView"); G(csmp, "vkCreateSampler");
        G(map, "vkMapMemory"); G(unmap, "vkUnmapMemory");
        G(dbuf, "vkDestroyBuffer"); G(dimg, "vkDestroyImage"); G(div, "vkDestroyImageView"); G(dbv, "vkDestroyBufferView");
        G(dsmp, "vkDestroySampler");
        if (s->want_robust2 || s->want_push || s->want_budget || s->want_multidraw || s->want_count)
            LOGI("compat: robustness2=%d push_descriptor=%d memory_budget=%d multi_draw=%d draw_indirect_count=%d",
                 s->want_robust2 ? 1 : 0, s->want_push, s->want_budget, s->want_multidraw, s->want_count);
    }
    free(s->ext); free(s);
}

/* ------------------------------------------------------------------ recursos fantasma (nullDescriptor) */
enum { V1D, V1DA, V2D, V2DA, VCUBE, VCUBEA, V3D, NV };
static struct {
    int ok, tried;
    VkBuffer buf; VkBufferView tview; VkSampler smp;
    VkImageView view[NV], sview;              /* sview: storage (R32_UINT, sem AFBC) */
    VkDeviceMemory mem[5]; VkImage img[4];
} Z;
static pthread_mutex_t zmu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t memtype(uint32_t bits) {
    VkMemoryPropertyFlags w = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < X.mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (X.mp.memoryTypes[i].propertyFlags & w) == w) return i;
    for (uint32_t i = 0; i < X.mp.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    return 0;
}
/* memória zerada pela CPU; imagens com STORAGE não usam AFBC, então zero = texel zero */
static int zmem(VkMemoryRequirements mr, VkDeviceMemory *m) {
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits) };
    if (X.am(X.d, &ai, NULL, m)) return 0;
    void *p;
    if (!X.map(X.d, *m, 0, VK_WHOLE_SIZE, 0, &p)) { memset(p, 0, mr.size); X.unmap(X.d, *m); }
    return 1;
}
static int zimg_f(VkImageType t, uint32_t layers, VkImageCreateFlags fl, VkFormat fmt, VkImage *img, VkDeviceMemory *m) {
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, fl, t, fmt, { 1, 1, 1 }, 1, layers,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    if (X.cimg(X.d, &ic, NULL, img)) return 0;
    VkMemoryRequirements mr; X.gimr(X.d, *img, &mr);
    return zmem(mr, m) && !X.bim(X.d, *img, *m, 0);
}
static int zimg(VkImageType t, uint32_t layers, VkImageCreateFlags fl, VkImage *img, VkDeviceMemory *m) {
    return zimg_f(t, layers, fl, VK_FORMAT_R8G8B8A8_UNORM, img, m);
}
static VkImageView zview_f(VkImage img, VkImageViewType vt, uint32_t layers, VkFormat fmt) {
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, vt, fmt, { 0 },
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    VkImageView v = VK_NULL_HANDLE;
    X.civ(X.d, &vc, NULL, &v);
    return v;
}
static VkImageView zview(VkImage img, VkImageViewType vt, uint32_t layers) { return zview_f(img, vt, layers, VK_FORMAT_R8G8B8A8_UNORM); }
static int zinit(void) {
    if (Z.ok) return 1;
    pthread_mutex_lock(&zmu);
    if (!Z.tried && X.d) {
        Z.tried = 1;
        int ok = 1;
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 65536,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        if (X.cbuf(X.d, &bi, NULL, &Z.buf)) ok = 0;
        else { VkMemoryRequirements mr; X.gbmr(X.d, Z.buf, &mr); ok = zmem(mr, &Z.mem[0]) && !X.bbm(X.d, Z.buf, Z.mem[0], 0); }
        if (ok) {
            VkBufferViewCreateInfo vi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, 0, 0, Z.buf, VK_FORMAT_R32G32B32A32_UINT, 0, VK_WHOLE_SIZE };
            if (X.cbv(X.d, &vi, NULL, &Z.tview)) ok = 0;
        }
        if (ok) {
            VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }; sc.maxLod = 0.25f;
            if (X.csmp(X.d, &sc, NULL, &Z.smp)) ok = 0;
        }
        if (ok && zimg(VK_IMAGE_TYPE_1D, 1, 0, &Z.img[0], &Z.mem[1])) {
            Z.view[V1D] = zview(Z.img[0], VK_IMAGE_VIEW_TYPE_1D, 1); Z.view[V1DA] = zview(Z.img[0], VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1);
        }
        if (ok && zimg(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &Z.img[1], &Z.mem[2])) {
            Z.view[V2D] = zview(Z.img[1], VK_IMAGE_VIEW_TYPE_2D, 1); Z.view[V2DA] = zview(Z.img[1], VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
            Z.view[VCUBE] = zview(Z.img[1], VK_IMAGE_VIEW_TYPE_CUBE, 6); Z.view[VCUBEA] = zview(Z.img[1], VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6);
        } else ok = 0;
        if (ok && zimg(VK_IMAGE_TYPE_3D, 1, 0, &Z.img[2], &Z.mem[3])) Z.view[V3D] = zview(Z.img[2], VK_IMAGE_VIEW_TYPE_3D, 1);
        if (ok && zimg_f(VK_IMAGE_TYPE_2D, 1, 0, VK_FORMAT_R32_UINT, &Z.img[3], &Z.mem[4])) Z.sview = zview_f(Z.img[3], VK_IMAGE_VIEW_TYPE_2D, 1, VK_FORMAT_R32_UINT);
        for (int i = 0; i < NV; i++) if (!Z.view[i]) Z.view[i] = Z.view[V2D];
        if (!Z.sview) Z.sview = Z.view[V2D];
        Z.ok = ok && Z.view[V2D];
        LOGI("compat: recursos fantasma do nullDescriptor %s", Z.ok ? "prontos" : "FALHARAM");
    }
    pthread_mutex_unlock(&zmu);
    return Z.ok;
}

/* tipo de view esperado por (layout, binding): desconhecido = 2D */
static VkImageView zview_for(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? Z.sview : Z.view[V2D]; }

static int type_is_image(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static int type_is_buffer(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
static int type_is_texel(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; }

static int img_null(VkDescriptorType t, const VkDescriptorImageInfo *ii) {
    if (t == VK_DESCRIPTOR_TYPE_SAMPLER) return 0;     /* sampler nulo só com imutável (válido) */
    return ii->imageView == VK_NULL_HANDLE;
}
static void img_fix(VkDescriptorType t, VkDescriptorImageInfo *ii) {
    if (ii->imageView == VK_NULL_HANDLE && t != VK_DESCRIPTOR_TYPE_SAMPLER) ii->imageView = zview_for(t);
}

static int write_has_null(const VkWriteDescriptorSet *w) {
    for (uint32_t j = 0; j < w->descriptorCount; j++) {
        if (type_is_image(w->descriptorType) && w->pImageInfo && img_null(w->descriptorType, &w->pImageInfo[j])) return 1;
        if (type_is_buffer(w->descriptorType) && w->pBufferInfo && w->pBufferInfo[j].buffer == VK_NULL_HANDLE) return 1;
        if (type_is_texel(w->descriptorType) && w->pTexelBufferView && w->pTexelBufferView[j] == VK_NULL_HANDLE) return 1;
    }
    return 0;
}

/* copia os writes trocando nulos por fantasmas; *mem guarda tudo para um free() só.
 * dst != NULL troca o dstSet de todos (push descriptor). */
static const VkWriteDescriptorSet *patch_writes(uint32_t n, const VkWriteDescriptorSet *w, VkDescriptorSet dst, void **mem) {
    *mem = NULL;
    int need = dst != VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n && !need; i++) if (write_has_null(&w[i])) need = 1;
    if (!need) return w;
    int any_null = 0;
    for (uint32_t i = 0; i < n; i++) if (write_has_null(&w[i])) any_null = 1;
    if (any_null && !zinit()) any_null = 0;
    size_t sz = n * sizeof *w;
    for (uint32_t i = 0; i < n; i++) sz += w[i].descriptorCount * (sizeof(VkDescriptorImageInfo) + sizeof(VkDescriptorBufferInfo) + sizeof(VkBufferView));
    char *p = malloc(sz);
    if (!p) return w;
    *mem = p;
    VkWriteDescriptorSet *o = (void *)p; p += n * sizeof *w;
    memcpy(o, w, n * sizeof *w);
    for (uint32_t i = 0; i < n; i++) {
        if (dst) o[i].dstSet = dst;
        if (!any_null || !write_has_null(&w[i])) continue;
        uint32_t c = w[i].descriptorCount;
        if (type_is_image(w[i].descriptorType) && w[i].pImageInfo) {
            VkDescriptorImageInfo *ii = (void *)p; p += c * sizeof *ii;
            memcpy(ii, w[i].pImageInfo, c * sizeof *ii);
            for (uint32_t j = 0; j < c; j++) img_fix(w[i].descriptorType, &ii[j]);
            o[i].pImageInfo = ii;
        } else if (type_is_buffer(w[i].descriptorType) && w[i].pBufferInfo) {
            VkDescriptorBufferInfo *bi = (void *)p; p += c * sizeof *bi;
            memcpy(bi, w[i].pBufferInfo, c * sizeof *bi);
            for (uint32_t j = 0; j < c; j++) if (!bi[j].buffer) { bi[j].buffer = Z.buf; bi[j].offset = 0; bi[j].range = VK_WHOLE_SIZE; }
            o[i].pBufferInfo = bi;
        } else if (type_is_texel(w[i].descriptorType) && w[i].pTexelBufferView) {
            VkBufferView *bv = (void *)p; p += c * sizeof *bv;
            memcpy(bv, w[i].pTexelBufferView, c * sizeof *bv);
            for (uint32_t j = 0; j < c; j++) if (!bv[j]) bv[j] = Z.tview;
            o[i].pTexelBufferView = bv;
        }
    }
    return o;
}

static VKAPI_ATTR void VKAPI_CALL my_uds(VkDevice d, uint32_t n, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    void *mem;
    const VkWriteDescriptorSet *pw = patch_writes(n, w, VK_NULL_HANDLE, &mem);
    X.uds(d, n, pw, nc, c);
    free(mem);
}

/* ------------------------------------------------------------------ templates */
typedef struct {
    VkDescriptorUpdateTemplateType type; VkPipelineBindPoint bp; VkDescriptorSetLayout dsl;
    uint32_t n; VkDescriptorUpdateTemplateEntry *e; size_t size;
} Tmpl;
static CMap tmpls = CMAP_INIT;
typedef struct { uint32_t n; VkDescriptorSetLayout *s; } PLay;
static CMap playouts = CMAP_INIT;
static CMap pushdsl = CMAP_INIT;          /* layouts criados com PUSH_DESCRIPTOR (valor = 1) */

static size_t elem_size(VkDescriptorType t) {
    if (type_is_image(t)) return sizeof(VkDescriptorImageInfo);
    if (type_is_buffer(t)) return sizeof(VkDescriptorBufferInfo);
    if (type_is_texel(t)) return sizeof(VkBufferView);
    return 1;   /* inline uniform: bytes */
}

static VKAPI_ATTR VkResult VKAPI_CALL my_cdut(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                               const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
    VkDescriptorUpdateTemplateCreateInfo c2 = *ci;
    VkPipelineBindPoint bp = ci->pipelineBindPoint;
    VkDescriptorSetLayout dsl = ci->descriptorSetLayout;
    if (ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) {
        PLay *pl = cget(&playouts, HK(ci->pipelineLayout));
        if (!pl || ci->set >= pl->n) return VK_ERROR_INITIALIZATION_FAILED;
        dsl = pl->s[ci->set];
        c2.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        c2.descriptorSetLayout = dsl;
    }
    VkResult r = X.cdut(d, &c2, a, out);
    if (r != VK_SUCCESS) return r;
    Tmpl *t = calloc(1, sizeof *t);
    if (!t) return r;
    t->type = ci->templateType; t->bp = bp; t->dsl = dsl; t->n = ci->descriptorUpdateEntryCount;
    t->e = malloc(t->n * sizeof *t->e);
    if (t->e) memcpy(t->e, ci->pDescriptorUpdateEntries, t->n * sizeof *t->e);
    for (uint32_t i = 0; i < t->n; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &ci->pDescriptorUpdateEntries[i];
        size_t es = elem_size(e->descriptorType);
        size_t end = e->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? e->offset + e->descriptorCount
                   : e->descriptorCount ? e->offset + (size_t)e->stride * (e->descriptorCount - 1) + es : 0;
        if (end > t->size) t->size = end;
    }
    cput(&tmpls, HK(*out), t);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL my_ddut(VkDevice d, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) {
    Tmpl *x = cdel(&tmpls, HK(t));
    if (x) { free(x->e); free(x); }
    X.ddut(d, t, a);
}

/* copia os dados do template trocando nulos; NULL = sem mudança */
static void *patch_tmpl_data(Tmpl *t, const void *data) {
    if (!t || !t->e || !data) return NULL;
    int need = 0;
    for (uint32_t i = 0; i < t->n && !need; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->e[i];
        for (uint32_t j = 0; j < e->descriptorCount && !need; j++) {
            const char *p = (const char *)data + e->offset + (size_t)e->stride * j;
            if (type_is_image(e->descriptorType)) need = img_null(e->descriptorType, (const VkDescriptorImageInfo *)p);
            else if (type_is_buffer(e->descriptorType)) need = ((const VkDescriptorBufferInfo *)p)->buffer == VK_NULL_HANDLE;
            else if (type_is_texel(e->descriptorType)) need = *(const VkBufferView *)p == VK_NULL_HANDLE;
        }
    }
    if (!need || !zinit()) return NULL;
    char *c = malloc(t->size);
    if (!c) return NULL;
    memcpy(c, data, t->size);
    for (uint32_t i = 0; i < t->n; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->e[i];
        for (uint32_t j = 0; j < e->descriptorCount; j++) {
            char *p = c + e->offset + (size_t)e->stride * j;
            if (type_is_image(e->descriptorType)) img_fix(e->descriptorType, (VkDescriptorImageInfo *)p);
            else if (type_is_buffer(e->descriptorType)) { VkDescriptorBufferInfo *b = (void *)p; if (!b->buffer) { b->buffer = Z.buf; b->offset = 0; b->range = VK_WHOLE_SIZE; } }
            else if (type_is_texel(e->descriptorType)) { VkBufferView *v = (void *)p; if (!*v) *v = Z.tview; }
        }
    }
    return c;
}
static VKAPI_ATTR void VKAPI_CALL my_udst(VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data) {
    void *c = patch_tmpl_data(cget(&tmpls, HK(t)), data);
    X.udst(d, s, t, c ? c : data);
    free(c);
}

/* ------------------------------------------------------------------ partially bound (v4.9.3)
 * Na G76 um descritor nunca escrito derruba o job inteiro (o shader lê 0 em tudo). Bindings
 * PARTIALLY_BOUND são preenchidos com os fantasmas assim que o set é alocado. */
typedef struct { uint32_t binding, count; VkDescriptorType type; int variable; } PBind;
typedef struct { uint32_t n; PBind b[]; } PLayout;
static CMap partial = CMAP_INIT;

static void record_partial(VkDescriptorSetLayout l, const VkDescriptorSetLayoutCreateInfo *ci) {
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bf = NULL;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) bf = (const void *)b;
    if (!bf || bf->bindingCount != ci->bindingCount) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < bf->bindingCount; i++) if (bf->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) n++;
    if (!n) return;
    PLayout *p = malloc(sizeof *p + n * sizeof(PBind));
    if (!p) return;
    p->n = 0;
    for (uint32_t i = 0; i < bf->bindingCount; i++) {
        if (!(bf->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)) continue;
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        if (!b->descriptorCount || b->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) continue;
        p->b[p->n++] = (PBind){ b->binding, b->descriptorCount, b->descriptorType,
                                (bf->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 };
    }
    cput(&partial, HK(l), p);
}

static void fill_partial(VkDescriptorSet ds, const PLayout *p, uint32_t var_count) {
    if (!zinit()) return;
    for (uint32_t k = 0; k < p->n; k++) {
        const PBind *b = &p->b[k];
        uint32_t c = b->variable ? var_count : b->count;
        if (!c) continue;
        size_t es = type_is_image(b->type) ? sizeof(VkDescriptorImageInfo) : type_is_buffer(b->type) ? sizeof(VkDescriptorBufferInfo) : sizeof(VkBufferView);
        if (!type_is_image(b->type) && !type_is_buffer(b->type) && !type_is_texel(b->type)) continue;
        void *arr = malloc(c * es);
        if (!arr) continue;
        for (uint32_t j = 0; j < c; j++) {
            if (type_is_image(b->type)) {
                VkImageLayout lay = b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ((VkDescriptorImageInfo *)arr)[j] = (VkDescriptorImageInfo){ Z.smp, zview_for(b->type), lay };
            } else if (type_is_buffer(b->type)) ((VkDescriptorBufferInfo *)arr)[j] = (VkDescriptorBufferInfo){ Z.buf, 0, VK_WHOLE_SIZE };
            else ((VkBufferView *)arr)[j] = Z.tview;
        }
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, b->binding, 0, c, b->type,
                                   type_is_image(b->type) ? arr : NULL, type_is_buffer(b->type) ? arr : NULL, type_is_texel(b->type) ? arr : NULL };
        X.uds(X.d, 1, &w, 0, NULL);
        free(arr);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL my_ads(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    VkResult r = X.ads(d, ai, out);
    if (r != VK_SUCCESS) return r;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *vc = NULL;
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) vc = (const void *)b;
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        PLayout *p = cget(&partial, HK(ai->pSetLayouts[i]));
        if (!p) continue;
        uint32_t var = (vc && i < vc->descriptorSetCount) ? vc->pDescriptorCounts[i] : 0;
        fill_partial(out[i], p, var);
    }
    return r;
}

/* ------------------------------------------------------------------ push descriptor */
static VKAPI_ATTR VkResult VKAPI_CALL my_cdsl(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
                                               const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    VkDescriptorSetLayoutCreateInfo c2 = *ci;
    int push = (ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0;
    c2.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    VkResult r = X.cdsl(d, &c2, a, out);
    if (r != VK_SUCCESS) return r;
    if (push) cput(&pushdsl, HK(*out), calloc(1, 1));
    record_partial(*out, ci);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL my_gdsls(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *s) {
    VkDescriptorSetLayoutCreateInfo c2 = *ci;
    c2.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    X.gdsls(d, &c2, s);
}

void compat_layout(VkPipelineLayout pl, const VkPipelineLayoutCreateInfo *ci) {
    if (!compat_on || !pl) return;
    PLay *p = calloc(1, sizeof *p);
    if (!p) return;
    p->n = ci->setLayoutCount;
    p->s = malloc((p->n ? p->n : 1) * sizeof *p->s);
    if (!p->s) { free(p); return; }
    if (p->n) memcpy(p->s, ci->pSetLayouts, p->n * sizeof *p->s);
    cput(&playouts, HK(pl), p);
}
void compat_layout_destroy(VkPipelineLayout pl) {
    PLay *p = cdel(&playouts, HK(pl));
    if (p) { free(p->s); free(p); }
}

/* pools de descriptor sets por command buffer, zerados no vkBeginCommandBuffer */
typedef struct { VkDescriptorPool p[64]; int n, cur; } CbPools;
static CMap cbpools = CMAP_INIT;
static VkDescriptorPool new_pool(void) {
    static const VkDescriptorType ty[] = {
        VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT };
    VkDescriptorPoolSize ps[9];
    for (int i = 0; i < 9; i++) { ps[i].type = ty[i]; ps[i].descriptorCount = 256; }
    VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 64, 9, ps };
    VkDescriptorPool p = VK_NULL_HANDLE;
    X.cdp(X.d, &ci, NULL, &p);
    return p;
}
static VkDescriptorSet push_alloc(VkCommandBuffer cb, VkDescriptorSetLayout dsl) {
    CbPools *s = cget(&cbpools, HK(cb));
    if (!s) { s = calloc(1, sizeof *s); if (!s) return VK_NULL_HANDLE; cput(&cbpools, HK(cb), s); s = cget(&cbpools, HK(cb)); if (!s) return VK_NULL_HANDLE; }
    for (;;) {
        if (s->cur >= s->n) {
            if (s->n >= 64) return VK_NULL_HANDLE;
            if (!(s->p[s->n] = new_pool())) return VK_NULL_HANDLE;
            s->n++;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, s->p[s->cur], 1, &dsl };
        VkDescriptorSet ds = VK_NULL_HANDLE;
        VkResult r = X.ads(X.d, &ai, &ds);
        if (r == VK_SUCCESS) return ds;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
        s->cur++;
    }
}
void compat_cb_begin(VkCommandBuffer cb) {
    CbPools *s = cget(&cbpools, HK(cb));
    if (!s) return;
    for (int i = 0; i < s->n; i++) X.rdp(X.d, s->p[i], 0);
    s->cur = 0;
}
void compat_cb_free(VkCommandBuffer cb) {
    CbPools *s = cdel(&cbpools, HK(cb));
    if (!s) return;
    for (int i = 0; i < s->n; i++) X.ddp(X.d, s->p[i], NULL);
    free(s);
}
void compat_pool_destroy(VkCommandPool p) { (void)p; }

static void push_set(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                     uint32_t n, const VkWriteDescriptorSet *w) {
    PLay *pl = cget(&playouts, HK(layout));
    if (!pl || set >= pl->n) return;
    VkDescriptorSet ds = push_alloc(cb, pl->s[set]);
    if (!ds) { LOGI("compat: push descriptor sem pool"); return; }
    void *mem;
    const VkWriteDescriptorSet *pw = patch_writes(n, w, ds, &mem);
    X.uds(X.d, n, pw, 0, NULL);
    free(mem);
    X.cbds(cb, bp, layout, set, 1, &ds, 0, NULL);
}
static VKAPI_ATTR void VKAPI_CALL my_cpds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                          uint32_t n, const VkWriteDescriptorSet *w) {
    push_set(cb, bp, layout, set, n, w);
}
static void push_tmpl(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout, uint32_t set, const void *data, VkPipelineBindPoint bp_over) {
    Tmpl *t = cget(&tmpls, HK(tmpl));
    if (!t) return;
    VkDescriptorSet ds = push_alloc(cb, t->dsl);
    if (!ds) return;
    void *c = patch_tmpl_data(t, data);
    X.udst(X.d, ds, tmpl, c ? c : data);
    free(c);
    X.cbds(cb, bp_over != VK_PIPELINE_BIND_POINT_MAX_ENUM ? bp_over : t->bp, layout, set, 1, &ds, 0, NULL);
}
static VKAPI_ATTR void VKAPI_CALL my_cpdst(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout, uint32_t set, const void *data) {
    push_tmpl(cb, tmpl, layout, set, data, VK_PIPELINE_BIND_POINT_MAX_ENUM);
}
/* maintenance6: a versão 2 diz os estágios em vez do bind point */
static void each_bp(VkShaderStageFlags st, VkPipelineBindPoint *bps, int *n) {
    *n = 0;
    if (st & (VK_SHADER_STAGE_ALL_GRAPHICS)) bps[(*n)++] = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (st & VK_SHADER_STAGE_COMPUTE_BIT) bps[(*n)++] = VK_PIPELINE_BIND_POINT_COMPUTE;
}
static VKAPI_ATTR void VKAPI_CALL my_cpds2(VkCommandBuffer cb, const VkPushDescriptorSetInfoKHR *i) {
    VkPipelineBindPoint bps[2]; int n;
    each_bp(i->stageFlags, bps, &n);
    for (int k = 0; k < n; k++) push_set(cb, bps[k], i->layout, i->set, i->descriptorWriteCount, i->pDescriptorWrites);
}
static VKAPI_ATTR void VKAPI_CALL my_cpdst2(VkCommandBuffer cb, const VkPushDescriptorSetWithTemplateInfoKHR *i) {
    /* o bind point do template vale; stageFlags não existe neste struct */
    push_tmpl(cb, i->descriptorUpdateTemplate, i->layout, i->set, i->pData, VK_PIPELINE_BIND_POINT_MAX_ENUM);
}

/* ------------------------------------------------------------------ vertex buffers nulos */
static VKAPI_ATTR void VKAPI_CALL my_cbvb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    int need = 0;
    for (uint32_t i = 0; i < n; i++) if (!b[i]) need = 1;
    if (!need || n > 64 || !zinit()) { X.cbvb(cb, first, n, b, o); return; }
    VkBuffer bb[64]; VkDeviceSize oo[64];
    for (uint32_t i = 0; i < n; i++) { bb[i] = b[i] ? b[i] : Z.buf; oo[i] = b[i] ? o[i] : 0; }
    X.cbvb(cb, first, n, bb, oo);
}
static VKAPI_ATTR void VKAPI_CALL my_cbvb2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o,
                                           const VkDeviceSize *sz, const VkDeviceSize *st) {
    int need = 0;
    for (uint32_t i = 0; i < n; i++) if (!b[i]) need = 1;
    if (!need || n > 64 || !zinit()) { X.cbvb2(cb, first, n, b, o, sz, st); return; }
    VkBuffer bb[64]; VkDeviceSize oo[64], ss[64];
    for (uint32_t i = 0; i < n; i++) {
        bb[i] = b[i] ? b[i] : Z.buf; oo[i] = b[i] ? o[i] : 0;
        ss[i] = sz ? (b[i] ? sz[i] : VK_WHOLE_SIZE) : VK_WHOLE_SIZE;
    }
    X.cbvb2(cb, first, n, bb, oo, sz ? ss : NULL, st);
}

/* ------------------------------------------------------------------ memória: orçamento e prioridade */
typedef struct { uint32_t heap; VkDeviceSize size; } MemRec;
static CMap mems = CMAP_INIT;
static VkDeviceSize heap_used[VK_MAX_MEMORY_HEAPS];

static PFN_vkCreateBuffer real_cbuf_app;
static VKAPI_ATTR VkResult VKAPI_CALL my_cbuf(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    if (!compat_count_on || !(ci->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) || (ci->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
        return real_cbuf_app(d, ci, a, out);
    VkBufferCreateInfo c2 = *ci;
    c2.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;   /* o contador do draw_indirect_count é lido pelo VS */
    return real_cbuf_app(d, &c2, a, out);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_am(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    /* VkMemoryPriorityAllocateInfoEXT sai da cadeia (o driver não conhece) */
    VkMemoryAllocateInfo c2 = *ai;
    /* draw_indirect_count: toda alocação aceita buffers com endereço */
    VkMemoryAllocateFlagsInfo maf = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    VkMemoryAllocateFlagsInfo *app_maf = NULL; VkMemoryAllocateFlags old_fl = 0;
    if (compat_count_on) {
        for (VkBaseOutStructure *b = (void *)ai->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) app_maf = (void *)b;
        if (app_maf) { old_fl = app_maf->flags; app_maf->flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; }
    }
    VkBaseOutStructure *prev = NULL, *drop = NULL, *dprev = NULL;
    for (VkBaseOutStructure *b = (void *)ai->pNext; b; prev = b, b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT) { drop = b; dprev = prev; break; }
    if (drop) { if (dprev) dprev->pNext = drop->pNext; else c2.pNext = drop->pNext; }
    if (compat_count_on && !app_maf) { maf.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; maf.pNext = c2.pNext; c2.pNext = &maf; }
    VkResult r = X.am(d, &c2, a, out);
    if (app_maf) app_maf->flags = old_fl;
    if (drop && dprev) dprev->pNext = drop;
    if (r == VK_SUCCESS && ai->memoryTypeIndex < X.mp.memoryTypeCount) {
        MemRec *m = malloc(sizeof *m);
        if (m) {
            m->heap = X.mp.memoryTypes[ai->memoryTypeIndex].heapIndex; m->size = ai->allocationSize;
            __sync_fetch_and_add(&heap_used[m->heap], m->size);
            cput(&mems, HK(*out), m);
        }
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL my_fm(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    MemRec *r = m ? cdel(&mems, HK(m)) : NULL;
    if (r) { __sync_fetch_and_sub(&heap_used[r->heap], r->size); free(r); }
    X.fm(d, m, a);
}
static VKAPI_ATTR void VKAPI_CALL my_sdmp(VkDevice d, VkDeviceMemory m, float p) { (void)d; (void)m; (void)p; }

static VkDeviceSize mem_available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char l[128]; unsigned long long kb = 0;
    while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f);
    return (VkDeviceSize)kb * 1024;
}
void compat_memprops(VkPhysicalDevice pd, void *pnext) {
    (void)pd;
    if (!compat_on) return;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *p = (void *)b;
        VkDeviceSize avail = mem_available();
        memset(p->heapBudget, 0, sizeof p->heapBudget); memset(p->heapUsage, 0, sizeof p->heapUsage);
        for (uint32_t h = 0; h < X.mp.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
            VkDeviceSize used = heap_used[h], size = X.mp.memoryHeaps[h].size;
            /* memória unificada: o que o vkshim já alocou + 80% da RAM livre, limitado ao heap */
            VkDeviceSize bud = used + avail / 10 * 8;
            p->heapUsage[h] = used;
            p->heapBudget[h] = bud < size ? bud : size;
        }
    }
}

/* ------------------------------------------------------------------ multi draw */
static VKAPI_ATTR void VKAPI_CALL my_cdm(VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *vi, uint32_t ic, uint32_t fi, uint32_t stride) {
    for (uint32_t i = 0; i < n; i++) {
        const VkMultiDrawInfoEXT *d = (const void *)((const char *)vi + (size_t)i * stride);
        if (compat_drawid) compat_drawid(cb, i);
        X.draw(cb, d->vertexCount, ic, d->firstVertex, fi);
    }
}
static VKAPI_ATTR void VKAPI_CALL my_cdmi(VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *ii, uint32_t ic, uint32_t fi,
                                          uint32_t stride, const int32_t *vo) {
    for (uint32_t i = 0; i < n; i++) {
        const VkMultiDrawIndexedInfoEXT *d = (const void *)((const char *)ii + (size_t)i * stride);
        if (compat_drawid) compat_drawid(cb, i);
        X.drawi(cb, d->indexCount, ic, d->firstIndex, vo ? *vo : d->vertexOffset, fi);
    }
}

/* ------------------------------------------------------------------ depth clip enable */
int compat_depth_clip(const VkPipelineRasterizationStateCreateInfo *rs, int *clip_enable) {
    if (!compat_on || !rs) return 0;
    for (const VkBaseInStructure *b = rs->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) {
            *clip_enable = ((const VkPipelineRasterizationDepthClipStateCreateInfoEXT *)b)->depthClipEnable;
            return 1;
        }
    return 0;
}

/* ------------------------------------------------------------------ tabela */
#define CH(nm, mine, realvar) if (!strcmp(name, nm)) { if (real && !X.realvar) X.realvar = (void *)real; return (PFN_vkVoidFunction)mine; }
PFN_vkVoidFunction compat_hook(const char *name, PFN_vkVoidFunction real) {
    if (!compat_on || name[0] != 'v') return NULL;
    CH("vkUpdateDescriptorSets", my_uds, uds)
    CH("vkUpdateDescriptorSetWithTemplate", my_udst, udst)
    CH("vkUpdateDescriptorSetWithTemplateKHR", my_udst, udst)
    CH("vkCreateDescriptorUpdateTemplate", my_cdut, cdut)
    CH("vkCreateDescriptorUpdateTemplateKHR", my_cdut, cdut)
    CH("vkDestroyDescriptorUpdateTemplate", my_ddut, ddut)
    CH("vkDestroyDescriptorUpdateTemplateKHR", my_ddut, ddut)
    CH("vkCreateDescriptorSetLayout", my_cdsl, cdsl)
    CH("vkGetDescriptorSetLayoutSupport", my_gdsls, gdsls)
    CH("vkAllocateDescriptorSets", my_ads, ads)
    CH("vkGetDescriptorSetLayoutSupportKHR", my_gdsls, gdsls)
    CH("vkCmdBindVertexBuffers", my_cbvb, cbvb)
    CH("vkCmdBindVertexBuffers2", my_cbvb2, cbvb2)
    CH("vkCmdBindVertexBuffers2EXT", my_cbvb2, cbvb2)
    CH("vkAllocateMemory", my_am, am)
    if (!strcmp(name, "vkCreateBuffer")) { if (real && !real_cbuf_app) real_cbuf_app = (void *)real; if (real_cbuf_app) return (PFN_vkVoidFunction)my_cbuf; }
    CH("vkFreeMemory", my_fm, fm)
    /* funções que o driver não tem */
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR") || !strcmp(name, "vkCmdPushDescriptorSet")) return (PFN_vkVoidFunction)my_cpds;
    if (!strcmp(name, "vkCmdPushDescriptorSetWithTemplateKHR") || !strcmp(name, "vkCmdPushDescriptorSetWithTemplate")) return (PFN_vkVoidFunction)my_cpdst;
    if (!strcmp(name, "vkCmdPushDescriptorSet2KHR") || !strcmp(name, "vkCmdPushDescriptorSet2")) return (PFN_vkVoidFunction)my_cpds2;
    if (!strcmp(name, "vkCmdPushDescriptorSetWithTemplate2KHR") || !strcmp(name, "vkCmdPushDescriptorSetWithTemplate2")) return (PFN_vkVoidFunction)my_cpdst2;
    if (!strcmp(name, "vkSetDeviceMemoryPriorityEXT")) return (PFN_vkVoidFunction)my_sdmp;
    if (!strcmp(name, "vkCmdDrawMultiEXT")) return (PFN_vkVoidFunction)my_cdm;
    if (!strcmp(name, "vkCmdDrawMultiIndexedEXT")) return (PFN_vkVoidFunction)my_cdmi;
    return NULL;
}

void compat_device_destroy(VkDevice d) {
    if (X.d != d) return;
    pthread_mutex_lock(&zmu);
    if (Z.ok || Z.tried) {
        for (int i = 0; i < NV; i++) if (Z.view[i] && (i == V2D || Z.view[i] != Z.view[V2D])) X.div(d, Z.view[i], NULL);
        if (Z.sview && Z.sview != Z.view[V2D]) X.div(d, Z.sview, NULL);
        for (int i = 0; i < 4; i++) if (Z.img[i]) X.dimg(d, Z.img[i], NULL);
        if (Z.tview) X.dbv(d, Z.tview, NULL);
        if (Z.buf) X.dbuf(d, Z.buf, NULL);
        if (Z.smp) X.dsmp(d, Z.smp, NULL);
        for (int i = 0; i < 5; i++) if (Z.mem[i]) X.fm(d, Z.mem[i], NULL);
    }
    memset(&Z, 0, sizeof Z);
    pthread_mutex_unlock(&zmu);
}
