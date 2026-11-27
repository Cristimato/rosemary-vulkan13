// c92 <hal.so> <dir_spv> [filtro] : probe de compatibilidade da v4.9.2 (extensões emuladas pelo vkshim)
// Build: tools/probe/c92/build.sh. Rodar como root com
//   LD_LIBRARY_PATH=/vendor/lib64/hw:/vendor/lib64:/vendor/lib64/mt6785
// Cada teste roda num processo filho: crash do driver vira "CRASH" em vez de derrubar o probe.
//
//  null_*     VK_EXT_robustness2 nullDescriptor: descritor nulo não trava e o shader lê 0
//  ext_*      extensões anunciadas
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct { int (*open)(void *, const char *, void **); } M;
typedef struct { uint32_t tag; uint16_t a, b; const char *id, *name, *author; M *methods; } HM;
typedef struct { uint32_t tag, ver; void *mod; uint64_t r[12]; void *close; void *eiep; PFN_vkCreateInstance ci; PFN_vkGetInstanceProcAddr gipa; } DEV;

static PFN_vkGetInstanceProcAddr gi;
static PFN_vkGetDeviceProcAddr gd;
static VkInstance in;
static VkDevice dv;
static VkPhysicalDevice pd;
static VkQueue qu;
static VkCommandPool pool;
static VkFence fence;
#define I(n) ((PFN_##n)gi(in, #n))
#define D(n) ((PFN_##n)gd(dv, #n))
static const char *dir, *only;
static int n_ok, n_bug, n_skip;
static char exts[256][VK_MAX_EXTENSION_NAME_SIZE]; static uint32_t next_;
static int robust2_null;

static int want(const char *n) { return !only || strstr(n, only); }
static void result(const char *name, int ok, const char *fmt, ...) {
    char b[300]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%-26s %-5s %s\n", name, ok ? "OK" : "BUG", b);
    if (ok) n_ok++; else n_bug++;
}
static void skip(const char *name, const char *why) { printf("%-26s %-5s %s\n", name, "-", why); n_skip++; }
static int has_ext(const char *e) { for (uint32_t i = 0; i < next_; i++) if (!strcmp(exts[i], e)) return 1; return 0; }

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags w) {
    VkPhysicalDeviceMemoryProperties mp; I(vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & w) == w) return i;
    return 0;
}
static VkShaderModule sm(const char *f) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, f);
    FILE *h = fopen(p, "rb"); if (!h) { printf("sem %s\n", p); exit(1); }
    fseek(h, 0, SEEK_END); size_t sz = ftell(h); fseek(h, 0, SEEK_SET);
    uint32_t *c = malloc(sz); if (fread(c, 1, sz, h) != sz) exit(1); fclose(h);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sz, c };
    VkShaderModule m = 0; D(vkCreateShaderModule)(dv, &ci, 0, &m); free(c); return m;
}
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; } Buf;
static Buf mkbuf(VkDeviceSize sz, VkBufferUsageFlags u) {
    Buf r = { 0 };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = sz; bi.usage = u;
    D(vkCreateBuffer)(dv, &bi, 0, &r.b);
    VkMemoryRequirements mr; D(vkGetBufferMemoryRequirements)(dv, r.b, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
        memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    D(vkAllocateMemory)(dv, &ai, 0, &r.m); D(vkBindBufferMemory)(dv, r.b, r.m, 0);
    D(vkMapMemory)(dv, r.m, 0, VK_WHOLE_SIZE, 0, &r.p); memset(r.p, 0, sz);
    return r;
}
static void submit_wait(VkCommandBuffer cb) {
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    D(vkResetFences)(dv, 1, &fence);
    if (D(vkQueueSubmit)(qu, 1, &si, fence)) { printf("submit falhou\n"); exit(1); }
    if (D(vkWaitForFences)(dv, 1, &fence, VK_TRUE, 5000000000ull)) { printf("fence falhou\n"); exit(1); }
}
static VkCommandBuffer newcb(void) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; D(vkAllocateCommandBuffers)(dv, &ai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; D(vkBeginCommandBuffer)(cb, &bi);
    return cb;
}

/* cada teste roda num processo novo (fork + exec): o driver tem threads e não sobrevive a um
 * fork depois de criar o device. No filho (--filho <teste>) só o teste pedido roda. */
static char **g_argv; static const char *child_test;
static void run(const char *name, void (*fn)(const char *)) {
    if (!want(name)) return;
    /* raw_*: investigação do driver cru (só com filtro raw_) */
    if (!child_test && !strncmp(name, "raw_", 4) && !(only && !strncmp(only, "raw_", 4))) return;
    if (child_test) { if (!strcmp(child_test, name)) { fn(name); fflush(stdout); _exit(n_bug ? 1 : 0); } return; }
    fflush(stdout);
    pid_t p = fork();
    if (!p) { execl("/proc/self/exe", g_argv[0], g_argv[1], g_argv[2], "--filho", name, (char *)NULL); _exit(3); }
    int st = 0; waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) { printf("%-26s %-5s sinal %d (driver travou)\n", name, "CRASH", WTERMSIG(st)); n_bug++; }
    else if (WEXITSTATUS(st) == 0) n_ok++;
    else if (!strncmp(name, "limite_", 7)) { printf("%-26s %-5s limitação conhecida (veja a linha acima)\n", name, "LIMITE"); n_skip++; }
    else n_bug++;
}

/* ---------------------------------------------------------------- nullDescriptor */
static void null_test(const char *name, const char *spv, VkDescriptorType t) {
    VkDescriptorSetLayoutBinding bd[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 },
        { 1, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 } };
    VkSampler smp = 0;
    if (t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }; sci.maxLod = 1;
        D(vkCreateSampler)(dv, &sci, 0, &smp);
    }
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, bd };
    VkDescriptorSetLayout dsl; D(vkCreateDescriptorSetLayout)(dv, &lci, 0, &dsl);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl };
    VkPipelineLayout pl; D(vkCreatePipelineLayout)(dv, &pli, 0, &pl);
    VkComputePipelineCreateInfo cci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cci.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm(spv), "main", 0 };
    cci.layout = pl;
    VkPipeline pp; if (D(vkCreateComputePipelines)(dv, 0, 1, &cci, 0, &pp)) { result(name, 0, "pipeline falhou"); return; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, { t, 1 } };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 2, ps };
    VkDescriptorPool dp; D(vkCreateDescriptorPool)(dv, &pci, 0, &dp);
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl };
    VkDescriptorSet ds; D(vkAllocateDescriptorSets)(dv, &dai, &ds);
    Buf out = mkbuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorBufferInfo bo = { out.b, 0, VK_WHOLE_SIZE }, bn = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii = { smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    if (t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkBufferView bvn = VK_NULL_HANDLE;
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bo, 0 },
                                  { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 1, 0, 1, t, 0, 0, 0 } };
    if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) w[1].pBufferInfo = &bn;
    else if (t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) w[1].pTexelBufferView = &bvn;
    else w[1].pImageInfo = &ii;
    D(vkUpdateDescriptorSets)(dv, 2, w, 0, 0);
    VkCommandBuffer cb = newcb();
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp);
    D(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
    D(vkCmdDispatch)(cb, 1, 1, 1);
    D(vkEndCommandBuffer)(cb);
    submit_wait(cb);
    uint32_t *o = out.p;
    int ok = o[3] == 12345 && o[0] == 1 && (t != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || o[1] == 1);
    result(name, ok, "o = %u %u, marca %u (esperado 1 = leu zero; marca 12345)", o[0], o[1], o[3]);
}
static void t_null_ssbo(const char *n) { null_test(n, "nssbo.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); }
static void t_null_ubo(const char *n) { null_test(n, "nubo.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); }
static void t_null_img(const char *n) { null_test(n, "nimg.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); }
static void t_null_texel(const char *n) { null_test(n, "ntex.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER); }
static void t_null_simg(const char *n) { null_test(n, "nsimg.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); }

/* ---------------------------------------------------------------- push descriptor */
static void push_test(const char *name, int use_tmpl) {
    if (!has_ext("VK_KHR_push_descriptor")) { skip(name, "sem VK_KHR_push_descriptor"); return; }
    PFN_vkCmdPushDescriptorSetKHR push = (PFN_vkCmdPushDescriptorSetKHR)gd(dv, "vkCmdPushDescriptorSetKHR");
    PFN_vkCmdPushDescriptorSetWithTemplateKHR pusht = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)gd(dv, "vkCmdPushDescriptorSetWithTemplateKHR");
    if (!push || !pusht) { result(name, 0, "função ausente"); return; }
    VkDescriptorSetLayoutBinding bd[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 } };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0,
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 2, bd };
    VkDescriptorSetLayout dsl; if (D(vkCreateDescriptorSetLayout)(dv, &lci, 0, &dsl)) { result(name, 0, "layout push falhou"); return; }
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl };
    VkPipelineLayout pl; D(vkCreatePipelineLayout)(dv, &pli, 0, &pl);
    VkComputePipelineCreateInfo cci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cci.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm("nssbo.comp.spv"), "main", 0 };
    cci.layout = pl;
    VkPipeline pp; if (D(vkCreateComputePipelines)(dv, 0, 1, &cci, 0, &pp)) { result(name, 0, "pipeline falhou"); return; }
    VkDescriptorUpdateTemplate tp = 0;
    if (use_tmpl) {
        VkDescriptorUpdateTemplateEntry e[2] = { { 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, sizeof(VkDescriptorBufferInfo) },
                                                 { 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizeof(VkDescriptorBufferInfo), sizeof(VkDescriptorBufferInfo) } };
        VkDescriptorUpdateTemplateCreateInfo tci = { VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, 0, 0, 2, e,
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR, 0, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0 };
        if (D(vkCreateDescriptorUpdateTemplate)(dv, &tci, 0, &tp)) { result(name, 0, "template falhou"); return; }
    }
    Buf a = mkbuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), b = mkbuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buf i1 = mkbuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), i2 = mkbuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ((uint32_t *)i1.p)[0] = 41; ((uint32_t *)i2.p)[0] = 99;
    VkCommandBuffer cb = newcb();
    int ok = 1; uint32_t ra = 0, rb = 0;
    for (int round = 0; round < 2; round++) {          /* 2ª volta: regrava o mesmo command buffer */
        if (round) { memset(a.p, 0, 64); memset(b.p, 0, 64); VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; D(vkBeginCommandBuffer)(cb, &bi); }
        D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp);
        for (int k = 0; k < 2; k++) {
            VkDescriptorBufferInfo bi[2] = { { k ? b.b : a.b, 0, VK_WHOLE_SIZE }, { k ? i2.b : i1.b, 0, VK_WHOLE_SIZE } };
            if (use_tmpl) pusht(cb, tp, pl, 0, bi);
            else {
                VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi[0], 0 },
                                              { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi[1], 0 } };
                push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, w);
            }
            D(vkCmdDispatch)(cb, 1, 1, 1);
        }
        D(vkEndCommandBuffer)(cb);
        submit_wait(cb);
        ra = ((uint32_t *)a.p)[0]; rb = ((uint32_t *)b.p)[0];
        if (ra != 42 || rb != 100) ok = 0;
    }
    result(name, ok, "saídas %u %u (esperado 42 100, duas gravações do mesmo command buffer)", ra, rb);
}
static void t_push(const char *n) { push_test(n, 0); }
static void t_push_tmpl(const char *n) { push_test(n, 1); }

/* ---------------------------------------------------------------- multi draw (render 8x8) */
/* mode 0: vkCmdDrawMultiEXT com 3 draws; mode 1: vkCmdDrawIndirectCount com 4 comandos e contador 2 */
static void md_render(const char *name, int mode) {
    PFN_vkCmdDrawMultiEXT md = (PFN_vkCmdDrawMultiEXT)gd(dv, "vkCmdDrawMultiEXT");
    PFN_vkCmdDrawIndirectCount dic = (PFN_vkCmdDrawIndirectCount)gd(dv, "vkCmdDrawIndirectCountKHR");
    if (mode == 0 && !has_ext("VK_EXT_multi_draw")) { skip(name, "sem VK_EXT_multi_draw"); return; }
    if (mode == 1 && !has_ext("VK_KHR_draw_indirect_count")) { skip(name, "sem VK_KHR_draw_indirect_count"); return; }
    if ((mode == 0 && !md) || (mode == 1 && !dic)) { result(name, 0, "função ausente"); return; }
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 8, 8, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage img; D(vkCreateImage)(dv, &ic, 0, &img);
    VkMemoryRequirements mr; D(vkGetImageMemoryRequirements)(dv, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory im; D(vkAllocateMemory)(dv, &mai, 0, &im); D(vkBindImageMemory)(dv, img, im, 0);
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView iv; D(vkCreateImageView)(dv, &vc, 0, &iv);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; D(vkCreatePipelineLayout)(dv, &pli, 0, &pl);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, sm("md.vert.spv"), "main", 0 },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sm("md.frag.spv"), "main", 0 } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST };
    VkViewport vp = { 0, 0, 8, 8, 0, 1 }; VkRect2D sc = { { 0, 0 }, { 8, 8 } };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = 1;
    VkPipelineColorBlendAttachmentState ba = { 0 }; ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cbs.attachmentCount = 1; cbs.pAttachments = &ba;
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO }; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, st, &vi, &ia, 0, &vs, &rs, &ms, 0, &cbs, 0, pl };
    VkPipeline pp; if (D(vkCreateGraphicsPipelines)(dv, 0, 1, &gci, 0, &pp)) { result(name, 0, "pipeline falhou"); return; }
    Buf rb = mkbuf(8 * 8 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cb = newcb();
    VkImageMemoryBarrier b0 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &b0);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, iv, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rinf = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { 8, 8 } }, 1, 0, 1, &ca };
    D(vkCmdBeginRendering)(cb, &rinf);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
    Buf ib = mkbuf(4 * sizeof(VkDrawIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    Buf cbf = mkbuf(16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (mode == 0) { VkMultiDrawInfoEXT di[3] = { { 0, 1 }, { 1, 1 }, { 2, 1 } }; md(cb, 3, di, 1, 0, sizeof di[0]); }
    else {
        VkDrawIndirectCommand *c = ib.p;
        for (int k = 0; k < 4; k++) c[k] = (VkDrawIndirectCommand){ 1, 1, (uint32_t)k, 0 };
        ((uint32_t *)cbf.p)[0] = 2;
        dic(cb, ib.b, 0, cbf.b, 0, 4, sizeof(VkDrawIndirectCommand));
    }
    D(vkCmdEndRendering)(cb);
    VkImageMemoryBarrier b1 = b0; b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b1.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b1);
    VkBufferImageCopy rg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 8, 8, 1 } };
    D(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &rg);
    D(vkEndCommandBuffer)(cb);
    submit_wait(cb);
    uint8_t *px = rb.p;
    int r0 = px[0], r1 = px[4], r2 = px[8], r3 = px[12];
    if (mode == 0) result(name, r0 == 40 && r1 == 80 && r2 == 120, "pixels r = %d %d %d (esperado 40 80 120: um ponto por draw, cor pelo gl_DrawID)", r0, r1, r2);
    else result(name, r0 == 40 && r1 == 80 && r2 == 0 && r3 == 0, "pixels r = %d %d %d %d (esperado 40 80 0 0: 4 comandos, contador 2)", r0, r1, r2, r3);
}
static void t_multidraw(const char *n) { md_render(n, 0); }
static void t_drawcount(const char *n) { md_render(n, 1); }

/* ---------------------------------------------------------------- memory budget / divisor */
static void t_budget(const char *name) {
    if (!has_ext("VK_EXT_memory_budget")) { skip(name, "sem VK_EXT_memory_budget"); return; }
    PFN_vkGetPhysicalDeviceMemoryProperties2 g = (PFN_vkGetPhysicalDeviceMemoryProperties2)gi(in, "vkGetPhysicalDeviceMemoryProperties2");
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 mp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &bp };
    g(pd, &mp);
    VkDeviceSize u0 = bp.heapUsage[0], b0 = bp.heapBudget[0];
    Buf big = mkbuf(64 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    (void)big;
    g(pd, &mp);
    VkDeviceSize u1 = bp.heapUsage[0];
    result(name, b0 > 0 && b0 <= mp.memoryProperties.memoryHeaps[0].size && u1 >= u0 + (64 << 20),
           "heap0 %llu MB, orçamento %llu MB, uso %llu -> %llu MB depois de alocar 64 MB",
           (unsigned long long)(mp.memoryProperties.memoryHeaps[0].size >> 20), (unsigned long long)(b0 >> 20),
           (unsigned long long)(u0 >> 20), (unsigned long long)(u1 >> 20));
}
static void t_divisor(const char *name) {
    if (!has_ext("VK_EXT_vertex_attribute_divisor")) { skip(name, "sem VK_EXT_vertex_attribute_divisor"); return; }
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp };
    I(vkGetPhysicalDeviceProperties2)(pd, &p2);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT df = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &df };
    I(vkGetPhysicalDeviceFeatures2)(pd, &f2);
    result(name, dp.maxVertexAttribDivisor > 1 && df.vertexAttributeInstanceRateDivisor,
           "maxVertexAttribDivisor %u, instanceRateDivisor %u, zeroDivisor %u (device criado com o nome EXT)",
           dp.maxVertexAttribDivisor, df.vertexAttributeInstanceRateDivisor, df.vertexAttributeInstanceRateZeroDivisor);
}

/* ---------------------------------------------------------------- descriptor indexing */
/* 4 recursos com valores 10 20 30 40; 64 threads, thread j lê o recurso j%4 */
typedef struct { Buf b[4]; VkBufferView bv[4]; VkImage im[4]; VkImageView iv[4]; VkDeviceMemory mm[4]; VkSampler smp; } DiRes;
static VkCommandBuffer onecb(void) { return newcb(); }
static void upload_img(DiRes *r, int k, VkFormat f, VkImageUsageFlags u, VkImageLayout final_, uint32_t val) {
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, f, { 1, 1, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, u | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    D(vkCreateImage)(dv, &ic, 0, &r->im[k]);
    VkMemoryRequirements mr; D(vkGetImageMemoryRequirements)(dv, r->im[k], &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    D(vkAllocateMemory)(dv, &ai, 0, &r->mm[k]); D(vkBindImageMemory)(dv, r->im[k], r->mm[k], 0);
    Buf st = mkbuf(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (f == VK_FORMAT_R8G8B8A8_UNORM) { uint8_t *p = st.p; p[0] = (uint8_t)val; p[3] = 255; } else ((uint32_t *)st.p)[0] = val;
    VkCommandBuffer cb = onecb();
    VkImageMemoryBarrier b0 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, r->im[k], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b0);
    VkBufferImageCopy rg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
    D(vkCmdCopyBufferToImage)(cb, st.b, r->im[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    VkImageMemoryBarrier b1 = b0; b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b1.newLayout = final_;
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 0, 0, 1, &b1);
    D(vkEndCommandBuffer)(cb); submit_wait(cb);
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, r->im[k], VK_IMAGE_VIEW_TYPE_2D, f, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    D(vkCreateImageView)(dv, &vc, 0, &r->iv[k]);
}
/* mode: bit0 = flags UPDATE_AFTER_BIND|PARTIALLY_BOUND no binding (e pool UAB);
 *       bit1 = grava o dispatch com valores 1..4 e só depois (antes do submit) atualiza para 10..40;
 *       bit2 = só escreve os recursos 0 e 2 (partially bound; shader lê todos, conferimos só 0 e 2) */
static void di_test(const char *name, const char *spv, VkDescriptorType t, int mode) {
    DiRes R; memset(&R, 0, sizeof R);
    int fixed_vals = (mode & 2) ? 1 : 10;
    for (int k = 0; k < 4; k++) {
        uint32_t val = (uint32_t)(k + 1) * 10;
        if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            R.b[k] = mkbuf(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
            ((uint32_t *)R.b[k].p)[0] = val;
            if (t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
                VkBufferViewCreateInfo vi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, 0, 0, R.b[k].b, VK_FORMAT_R32_UINT, 0, VK_WHOLE_SIZE };
                D(vkCreateBufferView)(dv, &vi, 0, &R.bv[k]);
            }
        } else if (t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) upload_img(&R, k, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, val);
        else upload_img(&R, k, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_LAYOUT_GENERAL, val);
    }
    if (mode & 2) { /* recursos "antigos" com 1..4 para o dispatch gravado antes da atualização */
        for (int k = 0; k < 4; k++) if (R.b[k].p) ((uint32_t *)R.b[k].p)[64] = (uint32_t)(k + 1);
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }; D(vkCreateSampler)(dv, &sci, 0, &R.smp);
    VkDescriptorSetLayoutBinding bd[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 },
                                           { 1, t, 4, VK_SHADER_STAGE_COMPUTE_BIT, 0 } };
    VkDescriptorBindingFlags bf[2] = { 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, 2, bf };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, (mode & 1) ? &bfi : NULL,
        (mode & 1) ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0, 2, bd };
    VkDescriptorSetLayout dsl; VkResult rr = D(vkCreateDescriptorSetLayout)(dv, &lci, 0, &dsl);
    if (rr) { result(name, 0, "layout falhou %d", rr); return; }
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl };
    VkPipelineLayout pl; D(vkCreatePipelineLayout)(dv, &pli, 0, &pl);
    VkComputePipelineCreateInfo cci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cci.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm(spv), "main", 0 };
    cci.layout = pl;
    VkPipeline pp; rr = D(vkCreateComputePipelines)(dv, 0, 1, &cci, 0, &pp);
    if (rr) { result(name, 0, "pipeline falhou %d", rr); return; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, { t, 4 } };
    if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) { ps[0].descriptorCount = 5; ps[1].descriptorCount = 5; }
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, (mode & 1) ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0, 1, 2, ps };
    VkDescriptorPool dp; D(vkCreateDescriptorPool)(dv, &pci, 0, &dp);
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl };
    VkDescriptorSet ds; rr = D(vkAllocateDescriptorSets)(dv, &dai, &ds);
    if (rr) { result(name, 0, "alocação falhou %d", rr); return; }
    Buf out = mkbuf(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorBufferInfo bo = { out.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w0 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bo, 0 };
    D(vkUpdateDescriptorSets)(dv, 1, &w0, 0, 0);
    /* escreve binding 1; off = deslocamento no buffer (0 = 10..40, 256 = 1..4) */
    VkDescriptorBufferInfo bi[4]; VkDescriptorImageInfo ii[4];
    #define WRITE1(off) do { \
        for (int k = 0; k < 4; k++) { bi[k] = (VkDescriptorBufferInfo){ R.b[k].b, (off), 16 }; \
            ii[k] = (VkDescriptorImageInfo){ R.smp, R.iv[k], t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; } \
        for (int k = 0; k < 4; k++) { if ((mode & 4) && (k & 1)) continue; \
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 1, (uint32_t)k, 1, t, &ii[k], &bi[k], &R.bv[k] }; \
            D(vkUpdateDescriptorSets)(dv, 1, &w, 0, 0); } } while (0)
    if (mode & 2) WRITE1(256); else WRITE1(0);
    VkCommandBuffer cb = newcb();
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp);
    D(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
    D(vkCmdDispatch)(cb, 1, 1, 1);
    D(vkEndCommandBuffer)(cb);
    if (mode & 2) WRITE1(0);   /* atualização depois de gravar: UPDATE_AFTER_BIND tem que valer */
    submit_wait(cb);
    uint32_t *o = out.p; int bad = 0, first_bad = -1;
    for (int j = 0; j < 64; j++) {
        if ((mode & 4) && ((j % 4) & 1)) continue;
        uint32_t want_ = (uint32_t)((j % 4) + 1) * 10;
        if (o[j] != want_) { bad++; if (first_bad < 0) first_bad = j; }
    }
    (void)fixed_vals;
    if (strstr(name, "subgroup")) { for (int j = 0; j < 16; j++) printf(" %08x", o[j]); printf("\n"); }
    result(name, !bad, "%d de %d threads erradas%s (o[0..7] = %u %u %u %u %u %u %u %u)", bad, (mode & 4) ? 32 : 64,
           (mode & 2) ? " | gravado antes da atualização" : "", o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
}
static void t_di_ssbo(const char *n) { di_test(n, "di_ssbo.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0); }
static void t_di_ssbo_semnu(const char *n) { di_test(n, "di_ssbo_semnu.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0); }
static void t_di_ubo(const char *n) { di_test(n, "di_ubo.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0); }
static void t_di_texel(const char *n) { di_test(n, "di_texel.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 0); }
static void t_di_img(const char *n) { di_test(n, "di_img.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_di_simg(const char *n) { di_test(n, "di_simg.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0); }
static void t_di_rt(const char *n) { di_test(n, "di_rt.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_di_ubo_s(const char *n) { di_test(n, "di_ubo_semnu.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0); }
static void t_di_texel_s(const char *n) { di_test(n, "di_texel_semnu.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 0); }
static void t_di_img_s(const char *n) { di_test(n, "di_img_semnu.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_di_simg_s(const char *n) { di_test(n, "di_simg_semnu.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0); }
static void t_di_uab_buf_s(const char *n) { di_test(n, "di_ssbo_semnu.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 | 2); }
static void t_di_uab_img_s(const char *n) { di_test(n, "di_img_semnu.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 | 2); }
static void t_di_uab_buf_nf(const char *n) { di_test(n, "di_ssbo_semnu.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2); }
static void t_di_uab_img_nf(const char *n) { di_test(n, "di_img_semnu.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2); }
static void t_di_partial_s(const char *n) { di_test(n, "di_img_semnu.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 | 4); }
static void t_di_partial_nf(const char *n) { di_test(n, "di_img_semnu.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4); }
static void t_wf_img(const char *n) { di_test(n, "di_img_wf.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_wf_simg(const char *n) { di_test(n, "di_simg_wf.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0); }
static void t_wf2(const char *n) { di_test(n, "di_img_wf2.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_wf3(const char *n) { di_test(n, "di_img_wf3.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_wf4(const char *n) { di_test(n, "di_img_wf4.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_uni_img(const char *n) { di_test(n, "di_img_uni.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0); }
static void t_uni_partial(const char *n) { di_test(n, "di_img_uni.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4); }
static void t_uni_uab(const char *n) { di_test(n, "di_img_uni.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2); }
static void t_sg(const char *n) {
    di_test(n, "sg.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0);
}
/* variable descriptor count: layout com 1024 imagens (VARIABLE), pool com 16; aloca 2 sets de 4 */
static void t_varcount(const char *name) {
    VkDescriptorSetLayoutBinding bd = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024, VK_SHADER_STAGE_COMPUTE_BIT, 0 };
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, 1, &bf };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi, 0, 1, &bd };
    VkDescriptorSetLayout dsl; VkResult r = D(vkCreateDescriptorSetLayout)(dv, &lci, 0, &dsl);
    if (r) { result(name, 0, "layout falhou %d", r); return; }
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4, 1, &ps };
    VkDescriptorPool dp; D(vkCreateDescriptorPool)(dv, &pci, 0, &dp);
    uint32_t cnt = 4;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, 0, 1, &cnt };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vc, dp, 1, &dsl };
    VkDescriptorSet a, b; VkResult r1 = D(vkAllocateDescriptorSets)(dv, &dai, &a), r2 = D(vkAllocateDescriptorSets)(dv, &dai, &b);
    result(name, r1 == 0 && r2 == 0, "2 sets de 4 (máx 1024) num pool de 16: %d %d (0 = ok; -1000069000 = pool sem espaço)", r1, r2);
}
/* maior array de imagens que o driver aceita num layout + alocação (UPDATE_AFTER_BIND/VARIABLE) */
static uint32_t bigarray_for(VkDescriptorType t) {
    uint32_t sizes[] = { 1024, 8192, 65536, 262144, 500000, 1000000 }, best = 0;
    for (int k = 0; k < 6; k++) {
        VkDescriptorSetLayoutBinding bd = { 0, t, sizes[k], VK_SHADER_STAGE_ALL, 0 };
        VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, 1, &bf };
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &bd };
        VkDescriptorSetLayout dsl; if (D(vkCreateDescriptorSetLayout)(dv, &lci, 0, &dsl)) break;
        VkDescriptorPoolSize ps = { t, sizes[k] };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, 1, &ps };
        VkDescriptorPool dp; if (D(vkCreateDescriptorPool)(dv, &pci, 0, &dp)) break;
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl };
        VkDescriptorSet ds; if (D(vkAllocateDescriptorSets)(dv, &dai, &ds)) break;
        best = sizes[k];
        D(vkDestroyDescriptorPool)(dv, dp, 0); D(vkDestroyDescriptorSetLayout)(dv, dsl, 0);
    }
    return best;
}
static void t_bigarray(const char *name) {
    uint32_t si = bigarray_for(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), sm_ = bigarray_for(VK_DESCRIPTOR_TYPE_SAMPLER),
             st = bigarray_for(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), sb = bigarray_for(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
             ub = bigarray_for(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER), ci = bigarray_for(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
             tb = bigarray_for(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
    result(name, si >= 8192, "maiores arrays aceitos: sampled %u sampler %u combined %u storage_img %u ssbo %u ubo %u texel %u", si, sm_, ci, st, sb, ub, tb);
}
static void t_di_uab_texel(const char *n) { di_test(n, "di_texel.comp.spv", VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1 | 2); }
static void t_di_uab_buf(const char *n) { di_test(n, "di_ssbo.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 | 2); }
static void t_di_uab_img(const char *n) { di_test(n, "di_img.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 | 2); }
static void t_di_partial(const char *n) { di_test(n, "di_img.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 | 4); }

int main(int c, char **v) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (c < 3) { printf("uso: c92 <hal.so> <dir_spv> [filtro]\n"); return 2; }
    dir = v[2]; if (c > 3) only = v[3];
    g_argv = v;
    if (c > 4 && !strcmp(v[3], "--filho")) { child_test = v[4]; only = NULL; }
    void *h = dlopen(v[1], RTLD_NOW); if (!h) { printf("%s\n", dlerror()); return 1; }
    HM *hm = dlsym(h, "HMI"); DEV *d = 0;
    if (hm->methods->open(hm, "vk0", (void **)&d)) { printf("open falhou\n"); return 1; }
    gi = d->gipa;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &ai;
    if (d->ci(&ici, 0, &in)) { printf("vkCreateInstance falhou\n"); return 1; }
    uint32_t n = 1; I(vkEnumeratePhysicalDevices)(in, &n, &pd);
    next_ = 256; VkExtensionProperties *ep = calloc(256, sizeof *ep);
    I(vkEnumerateDeviceExtensionProperties)(pd, NULL, &next_, ep);
    for (uint32_t i = 0; i < next_; i++) memcpy(exts[i], ep[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE);
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = { (VkStructureType)1000286000 };
    VkPhysicalDeviceFeatures2 f2q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, has_ext("VK_EXT_robustness2") ? &r2 : NULL };
    I(vkGetPhysicalDeviceFeatures2)(pd, &f2q);
    robust2_null = r2.nullDescriptor;
    VkPhysicalDeviceProperties pr; I(vkGetPhysicalDeviceProperties)(pd, &pr);
    if (!child_test) printf("GPU %s | %u extensões | robustness2: %s (nullDescriptor=%u)\n", pr.deviceName, next_,
           has_ext("VK_EXT_robustness2") ? "sim" : "não", r2.nullDescriptor);
    float pri = 1; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pri };
    VkPhysicalDeviceRobustness2FeaturesEXT r2e = { (VkStructureType)1000286000 };
    r2e.nullDescriptor = r2.nullDescriptor; r2e.robustBufferAccess2 = r2.robustBufferAccess2; r2e.robustImageAccess2 = r2.robustImageAccess2;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, has_ext("VK_EXT_robustness2") ? &r2e : NULL };
    f2.features.robustBufferAccess = getenv("C92_RBA") ? VK_TRUE : VK_FALSE;
    /* pede tudo de descriptor indexing que o device anuncia (como um jogo bindless) */
    VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 q2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12 };
    I(vkGetPhysicalDeviceFeatures2)(pd, &q2);
    VkPhysicalDeviceVulkan12Features f12 = q12; f12.pNext = f2.pNext;
    if (!child_test) printf("descriptorIndexing=%u runtimeArray=%u partiallyBound=%u nonUniformSampled=%u UAB sampled=%u ssbo=%u\n",
        q12.descriptorIndexing, q12.runtimeDescriptorArray, q12.descriptorBindingPartiallyBound,
        q12.shaderSampledImageArrayNonUniformIndexing, q12.descriptorBindingSampledImageUpdateAfterBind, q12.descriptorBindingStorageBufferUpdateAfterBind);
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f13 };
    f11.shaderDrawParameters = VK_TRUE;
    f2.pNext = &f11;
    f2.features.multiDrawIndirect = VK_FALSE;
    const char *en[12]; uint32_t nen = 0;
    if (has_ext("VK_EXT_robustness2")) en[nen++] = "VK_EXT_robustness2";
    if (has_ext("VK_KHR_push_descriptor")) en[nen++] = "VK_KHR_push_descriptor";
    if (has_ext("VK_EXT_vertex_attribute_divisor")) en[nen++] = "VK_EXT_vertex_attribute_divisor";
    if (has_ext("VK_EXT_depth_clip_enable")) en[nen++] = "VK_EXT_depth_clip_enable";
    if (has_ext("VK_EXT_multi_draw")) en[nen++] = "VK_EXT_multi_draw";
    if (has_ext("VK_EXT_memory_budget")) en[nen++] = "VK_EXT_memory_budget";
    if (has_ext("VK_EXT_memory_priority")) en[nen++] = "VK_EXT_memory_priority";
    if (has_ext("VK_KHR_draw_indirect_count")) en[nen++] = "VK_KHR_draw_indirect_count";
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &q, 0, 0, nen, en };
    gd = (PFN_vkGetDeviceProcAddr)gi(in, "vkGetDeviceProcAddr");
    VkResult r = I(vkCreateDevice)(pd, &dci, 0, &dv);
    if (r) { printf("vkCreateDevice falhou %d\n", r); return 1; }
    D(vkGetDeviceQueue)(dv, 0, 0, &qu);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    D(vkCreateCommandPool)(dv, &pci, 0, &pool);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; D(vkCreateFence)(dv, &fci, 0, &fence);

    run("null_ssbo", t_null_ssbo);
    run("null_ubo", t_null_ubo);
    run("null_imagem", t_null_img);
    run("null_texel_buffer", t_null_texel);
    run("null_storage_image", t_null_simg);
    run("push_descriptor", t_push);
    run("push_template", t_push_tmpl);
    run("multi_draw", t_multidraw);
    run("draw_indirect_count", t_drawcount);
    run("memory_budget", t_budget);
    run("divisor_ext", t_divisor);
    run("di_ssbo_nonuniform", t_di_ssbo);
    run("di_ssbo_sem_decoracao", t_di_ssbo_semnu);
    run("di_ubo_nonuniform", t_di_ubo);
    run("di_texel_nonuniform", t_di_texel);
    run("di_imagem_nonuniform", t_di_img);
    run("di_storage_img_nonuni", t_di_simg);
    run("di_runtime_array", t_di_rt);
    run("limite_uab_buffer", t_di_uab_buf);
    run("di_update_after_bind_i", t_di_uab_img);
    run("di_partially_bound", t_di_partial);
    run("raw_subgroup", t_sg);
    run("di_array_grande", t_bigarray);
    run("di_update_after_bind_t", t_di_uab_texel);
    run("raw_variable_count", t_varcount);
    run("raw_waterfall_img", t_wf_img);
    run("raw_waterfall_simg", t_wf_simg);
    run("raw_waterfall2_img", t_wf2);
    run("raw_waterfall3_img", t_wf3);
    run("raw_waterfall4_img", t_wf4);
    run("raw_uniforme_img", t_uni_img);
    run("raw_uniforme_partial", t_uni_partial);
    run("raw_uniforme_uab", t_uni_uab);
    run("raw_ubo_semnu", t_di_ubo_s);
    run("raw_texel_semnu", t_di_texel_s);
    run("raw_img_semnu", t_di_img_s);
    run("raw_simg_semnu", t_di_simg_s);
    run("raw_uab_buf_flags", t_di_uab_buf_s);
    run("raw_uab_img_flags", t_di_uab_img_s);
    run("raw_uab_buf_semflag", t_di_uab_buf_nf);
    run("raw_uab_img_semflag", t_di_uab_img_nf);
    run("raw_partial_flags", t_di_partial_s);
    run("raw_partial_semflag", t_di_partial_nf);
    printf("RESUMO: %d OK, %d BUG/CRASH, %d pulados/limites\n", n_ok, n_bug, n_skip);
    return n_bug;
}
