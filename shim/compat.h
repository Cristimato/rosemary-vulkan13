#pragma once
// compat (v4.9.2): extensões que o r49 da G76 não tem, emuladas no vkshim.
//   VK_EXT_robustness2           nullDescriptor por recursos fantasma zerados; robust*Access2 anunciados
//   VK_KHR_push_descriptor       descriptor sets alocados por command buffer
//   VK_EXT_vertex_attribute_divisor  apelido do KHR que o driver já tem
//   VK_EXT_depth_clip_enable     vira o depthClamp emulado (o shim decide no pipeline)
//   VK_EXT_memory_budget         orçamento pela RAM livre + uso contado pelo vkshim
//   VK_EXT_memory_priority / VK_EXT_pageable_device_local_memory   aceitos e ignorados
//   VK_EXT_multi_draw            laço de draws (gl_DrawID = índice)
#include <vulkan/vulkan.h>

extern int compat_on;
extern int compat_count_on;

/* lista de extensões a mais (só as que o driver não tem); devolve quantas */
uint32_t compat_exts(VkExtensionProperties *out, uint32_t max, const VkExtensionProperties *real, uint32_t nreal);
void compat_features(VkPhysicalDevice pd, void *pnext);
void compat_props(VkPhysicalDevice pd, void *pnext);
void compat_memprops(VkPhysicalDevice pd, void *pnext);

/* vkCreateDevice: troca a lista de extensões e tira do pNext os structs das emuladas.
 * Devolve o create info a usar (pode ser o próprio ci); compat_device_done desfaz. */
typedef struct CompatDC CompatDC;
const VkDeviceCreateInfo *compat_device_begin(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, CompatDC **st);
void compat_device_done(CompatDC *st, VkDevice d, VkResult r, PFN_vkGetDeviceProcAddr gdpa);
void compat_device_destroy(VkDevice d);

/* funções interceptadas/novas; NULL = não é nossa */
PFN_vkVoidFunction compat_hook(const char *name, PFN_vkVoidFunction real);

/* ganchos chamados pelo shim */
void compat_cb_begin(VkCommandBuffer cb);
void compat_cb_free(VkCommandBuffer cb);
void compat_pool_destroy(VkCommandPool p);
int compat_depth_clip(const VkPipelineRasterizationStateCreateInfo *rs, int *clip_enable);   /* 1 se o struct existe */

/* o shim fornece o draw com gl_DrawID para o multi_draw */
extern void (*compat_drawid)(VkCommandBuffer cb, uint32_t i);
