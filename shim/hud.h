#pragma once
// HUD/telemetria do vkshim (v4.9): tempo de quadro, tempo de GPU por timestamp, compilações de
// pipeline, draws/submits por quadro, frequência/uso/temperatura da GPU. Uma linha CSV por
// segundo em <pasta externa do app>/vkshim_hud.csv e um resumo no logcat a cada 5 s.
#include <vulkan/vulkan.h>

extern int hud_on;
extern volatile unsigned hud_draws;

void hud_init(const char *pkg);
void hud_device(VkDevice d, VkPhysicalDevice pd, VkInstance inst, PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetInstanceProcAddr gipa);
void hud_device_destroy(VkDevice d);
void hud_queue(VkDevice d, VkQueue q, uint32_t family);
VkResult hud_submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f, PFN_vkQueueSubmit real);
VkResult hud_submit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f, PFN_vkQueueSubmit2 real);
void hud_present(void);
void hud_pipe(long long ns, unsigned n);
extern long long hud_wait_ns;   /* tempo bloqueado esperando a GPU (fences/semáforos/idle) */
