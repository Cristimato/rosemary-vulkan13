// e47 <hal.so> <dir_spv> [filtro] : probe de erratas do blob Mali e das emulações do vkshim (v4.7)
// Build: tools/probe/e47/build.sh (compila os shaders de tools/probe/e47/ e este arquivo)
//
// Carrega a HAL direto (sem o loader do Android), renderiza num alvo 64x64 e confere pixels.
// Rodar contra vulkan.r49.so mostra os bugs do driver; contra vulkan.mt6785.so (vkshim)
// mostra se as correções funcionam.
//
//  wait_*     MK-01: espera interrompida por sinal não pode falhar (só SUCCESS/TIMEOUT)
//  cull_*     MK-16: cull mode dinâmico não pode sumir com pontos/linhas
//  dyn_*      MK-04: estado dinâmico definido antes do bind tem que valer depois do bind
//  clip_*     ClipDistance emulado (só se o device anuncia shaderClipDistance)
//  clamp_*    depthClamp (só se anunciado)
//  mdi_*      multiDrawIndirect com gl_DrawID (só se anunciado)
//  line_*     fillModeNonSolid (só se anunciado)
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

typedef struct { int (*open)(void *, const char *, void **); } M;
typedef struct { uint32_t tag; uint16_t a, b; const char *id, *name, *author; M *methods; } HM;
typedef struct { uint32_t tag, ver; void *mod; uint64_t r[12]; void *close; void *eiep; PFN_vkCreateInstance ci; PFN_vkGetInstanceProcAddr gipa; } DEV;

static PFN_vkGetInstanceProcAddr gi;
static PFN_vkGetDeviceProcAddr gd;
static VkInstance in;
static VkDevice dv;
static VkPhysicalDevice pd;
static VkQueue qu;
static VkPhysicalDeviceFeatures feat;
static int has_dce, has_dcc;
#define I(n) ((PFN_##n)gi(in, #n))
#define D(n) ((PFN_##n)gd(dv, #n))
static const char *dir;
static int n_ok, n_bug, n_skip;
static const char *only;

#define W 64
#define H 64
static VkImage cimg, dimg;
static VkImageView cview, dview;
static VkBuffer rbuf;
static VkDeviceMemory cmem, dmem, rmem;
static uint8_t *rptr;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkFence fence;
static VkPipelineLayout layout;

typedef struct { float col[4]; float z, w; } PC;

static int want(const char *name) { return !only || strstr(name, only); }

static void result(const char *name, int ok, const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    printf("%-22s %-5s %s\n", name, ok ? "OK" : "BUG", buf);
    if (ok) n_ok++; else n_bug++;
}
static void skip(const char *name, const char *why) { printf("%-22s %-5s %s\n", name, "-", why); n_skip++; }

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want_) {
    VkPhysicalDeviceMemoryProperties mp; I(vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want_) == want_) return i;
    return 0;
}

static VkShaderModule sm(const char *f) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, f);
    FILE *h = fopen(p, "rb"); if (!h) { printf("sem %s\n", p); exit(1); }
    fseek(h, 0, SEEK_END); size_t sz = ftell(h); fseek(h, 0, SEEK_SET);
    uint32_t *c = malloc(sz); if (fread(c, 1, sz, h) != sz) exit(1); fclose(h);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sz, c };
    VkShaderModule m = 0;
    VkResult r = D(vkCreateShaderModule)(dv, &ci, 0, &m);
    free(c);
    if (r) { printf("vkCreateShaderModule %s falhou %d\n", f, r); exit(1); }
    return m;
}

typedef struct {
    const char *vs, *fs;
    VkPrimitiveTopology topo;
    const VkDynamicState *dyn; uint32_t ndyn;
    VkCullModeFlags cull;
    VkBool32 dtest; VkCompareOp dcmp;
    VkBool32 clamp; VkPolygonMode poly;
    int vtx;               /* 1: binding 0 (stride estático 0) com vec4 na location 0 */
} PDesc;

static int g_depthclip = -1;
static int g_negz;        /* VkPipelineViewportDepthClipControlCreateInfoEXT negativeOneToOne */   /* -1: sem VkPipelineRasterizationDepthClipStateCreateInfoEXT; 0/1: depthClipEnable */
static VkPipeline mkpipe(const PDesc *p) {
    VkShaderModule v = sm(p->vs), f = p->fs ? sm(p->fs) : 0;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main", 0 },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main", 0 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkVertexInputBindingDescription vb = { 0, 0, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    if (p->vtx) { vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va; }
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, p->topo, 0 };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, 0, VK_TRUE };
    if (g_negz) vs.pNext = &dcc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = p->clamp; rs.polygonMode = p->poly; rs.cullMode = p->cull;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT dcs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
    if (g_depthclip >= 0) { dcs.depthClipEnable = (VkBool32)g_depthclip; rs.pNext = &dcs; }
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = p->dtest; ds.depthWriteEnable = p->dtest; ds.depthCompareOp = p->dcmp;
    VkPipelineColorBlendAttachmentState ba = { 0 }; ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &ba;
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, p->ndyn, p->dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf; ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    ci.stageCount = f ? 2 : 1; ci.pStages = st;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vs;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs; ci.pDynamicState = p->ndyn ? &dy : NULL; ci.layout = layout;
    VkPipeline pl = 0;
    VkResult r = D(vkCreateGraphicsPipelines)(dv, 0, 1, &ci, 0, &pl);
    D(vkDestroyShaderModule)(dv, v, 0);
    if (f) D(vkDestroyShaderModule)(dv, f, 0);
    if (r) { printf("vkCreateGraphicsPipelines (%s) falhou %d\n", p->vs, r); return 0; }
    return pl;
}

static void barrier(VkImage img, VkImageAspectFlags asp, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = s0; b.srcAccessMask = a0; b.dstStageMask = s1; b.dstAccessMask = a1;
    b.oldLayout = from; b.newLayout = to; b.image = img;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = (VkImageSubresourceRange){ asp, 0, 1, 0, 1 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    D(vkCmdPipelineBarrier2)(cb, &di);
}

static int keep_depth;

/* começa a gravar: alvo limpo (cor 0, profundidade 'clr_depth') e renderização dinâmica aberta */
static void begin(float clr_depth) {
    D(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D(vkBeginCommandBuffer)(cb, &bi);
    barrier(cimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(dimg, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = cview; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = ca;
    da.imageView = dview; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.storeOp = keep_depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE; da.clearValue.depthStencil.depth = clr_depth;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ W, H }; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    D(vkCmdBeginRendering)(cb, &ri);
}

static void push(float r, float g, float b, float z) {
    PC pc = { { r, g, b, 1 }, z, 1.0f };
    D(vkCmdPushConstants)(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
}

/* fecha, copia para o buffer, executa e devolve quantos pixels têm vermelho > 0 */
static int finish(void) {
    D(vkCmdEndRendering)(cb);
    barrier(cimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy rg = { 0 }; rg.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rg.imageExtent = (VkExtent3D){ W, H, 1 };
    D(vkCmdCopyImageToBuffer)(cb, cimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &rg);
    D(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    D(vkResetFences)(dv, 1, &fence);
    VkResult r = D(vkQueueSubmit)(qu, 1, &si, fence);
    if (r) { printf("vkQueueSubmit falhou %d\n", r); exit(1); }
    r = D(vkWaitForFences)(dv, 1, &fence, VK_TRUE, 5000000000ull);
    if (r) { printf("vkWaitForFences falhou %d\n", r); exit(1); }
    int n = 0;
    for (int i = 0; i < W * H; i++) if (rptr[i * 4]) n++;
    return n;
}
/* como finish(), mas copia a PROFUNDIDADE (D32F) para o buffer de leitura */
static const float *finish_depth(void) {
    D(vkCmdEndRendering)(cb);
    barrier(dimg, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy rg = { 0 }; rg.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    rg.imageExtent = (VkExtent3D){ W, H, 1 };
    D(vkCmdCopyImageToBuffer)(cb, dimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &rg);
    D(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    D(vkResetFences)(dv, 1, &fence);
    if (D(vkQueueSubmit)(qu, 1, &si, fence)) { printf("vkQueueSubmit falhou\n"); exit(1); }
    if (D(vkWaitForFences)(dv, 1, &fence, VK_TRUE, 5000000000ull)) { printf("vkWaitForFences falhou\n"); exit(1); }
    return (const float *)rptr;
}
static int px_r(int x, int y) { return rptr[(y * W + x) * 4]; }

/* ---------------------------------------------------------------- MK-01 */
static volatile int sig_count;
static void on_sig(int s) { (void)s; sig_count++; }
typedef struct { int kind; VkResult r; long ms; } WaitArg;
static VkSemaphore tsem;
static void *waiter(void *a_) {
    WaitArg *a = a_;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (a->kind == 0) {
        VkFence f; VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        D(vkCreateFence)(dv, &fi, 0, &f);
        a->r = D(vkWaitForFences)(dv, 1, &f, VK_TRUE, 400000000ull);
        D(vkDestroyFence)(dv, f, 0);
    } else {
        uint64_t v = 1;
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO }; wi.semaphoreCount = 1; wi.pSemaphores = &tsem; wi.pValues = &v;
        a->r = D(vkWaitSemaphores)(dv, &wi, 400000000ull);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    a->ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    return NULL;
}
static void test_wait(int kind, const char *name) {
    if (!want(name)) return;
    struct sigaction sa = { 0 }; sa.sa_handler = on_sig; sigaction(SIGUSR1, &sa, NULL);   /* sem SA_RESTART, como o ART */
    if (kind == 1 && !tsem) {
        VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, 0, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti };
        D(vkCreateSemaphore)(dv, &si, 0, &tsem);
    }
    WaitArg a = { kind, 0, 0 };
    pthread_t t; sig_count = 0;
    pthread_create(&t, NULL, waiter, &a);
    for (int i = 0; i < 40; i++) { usleep(10000); pthread_kill(t, SIGUSR1); }
    pthread_join(t, NULL);
    result(name, a.r == VK_TIMEOUT, "retorno %d em %ld ms com %d sinais (esperado VK_TIMEOUT=2 em ~400 ms)", a.r, a.ms, sig_count);
}

/* ---------------------------------------------------------------- testes de render */
static const VkDynamicState DYN_CULL[] = { VK_DYNAMIC_STATE_CULL_MODE };

static void test_cull_nontri(VkPrimitiveTopology topo, const char *name) {
    if (!want(name)) return;
    PDesc p = { "pts.vert.spv", "col.frag.spv", topo, DYN_CULL, 1, VK_CULL_MODE_NONE, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    D(vkCmdSetCullMode)(cb, VK_CULL_MODE_FRONT_AND_BACK);
    push(1, 0, 0, 0.5f);
    D(vkCmdDraw)(cb, 16, 1, 0, 0);
    int n = finish();
    result(name, n > 0, "%d pixels (culling não vale para pontos/linhas: esperado > 0)", n);
    D(vkDestroyPipeline)(dv, pl, 0);
}

/* estado dinâmico antes do bind: 'mode' 0 = cull, 1 = depth compare, 2 = depth test enable, 3 = troca de pipeline */
static void test_dyn(int mode, const char *name) {
    if (!want(name)) return;
    VkDynamicState dyn[1]; PDesc p = { "tri.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, dyn, 1,
                                       VK_CULL_MODE_NONE, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL };
    if (mode == 0 || mode == 3) dyn[0] = VK_DYNAMIC_STATE_CULL_MODE;
    if (mode == 1) { dyn[0] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP; p.dtest = 1; p.dcmp = VK_COMPARE_OP_ALWAYS; }
    if (mode == 2) { dyn[0] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE; p.dtest = 0; p.dcmp = VK_COMPARE_OP_NEVER; }
    VkPipeline pl = mkpipe(&p), pl2 = mode == 3 ? mkpipe(&p) : 0;
    if (!pl || (mode == 3 && !pl2)) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    if (mode == 3) D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2);
    if (mode == 0 || mode == 3) D(vkCmdSetCullMode)(cb, VK_CULL_MODE_FRONT_AND_BACK);
    if (mode == 1) D(vkCmdSetDepthCompareOp)(cb, VK_COMPARE_OP_NEVER);
    if (mode == 2) D(vkCmdSetDepthTestEnable)(cb, VK_TRUE);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, 0.5f);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    int n = finish();
    result(name, n == 0, "%d pixels (estado definido antes do bind descarta tudo: esperado 0)", n);
    D(vkDestroyPipeline)(dv, pl, 0);
    if (pl2) D(vkDestroyPipeline)(dv, pl2, 0);
}

/* MK-04 strides: vkCmdBindVertexBuffers2 com stride ANTES do bind de um pipeline com stride dinâmico */
static void test_dyn_stride(const char *name) {
    if (!want(name)) return;
    static const VkDynamicState dy[] = { VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE };
    PDesc p = { "vtx.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, dy, 1, VK_CULL_MODE_NONE, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL, 1 };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = 3 * 16; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vb; D(vkCreateBuffer)(dv, &bi, 0, &vb);
    VkMemoryRequirements mr; D(vkGetBufferMemoryRequirements)(dv, vb, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory vm; D(vkAllocateMemory)(dv, &ai, 0, &vm); D(vkBindBufferMemory)(dv, vb, vm, 0);
    float *v; D(vkMapMemory)(dv, vm, 0, VK_WHOLE_SIZE, 0, (void **)&v);
    const float tri[12] = { -1, -1, 0.5f, 1,  3, -1, 0.5f, 1,  -1, 3, 0.5f, 1 };
    memcpy(v, tri, sizeof tri);
    begin(1.0f);
    VkDeviceSize off = 0, sz = VK_WHOLE_SIZE, st = 16;
    D(vkCmdBindVertexBuffers2)(cb, 0, 1, &vb, &off, &sz, &st);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, 0.5f);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    int n = finish();
    result(name, n == W * H, "%d pixels (stride 16 definido antes do bind: esperado %d; com o stride estático 0 do pipeline: 0)", n, W * H);
    D(vkDestroyPipeline)(dv, pl, 0); D(vkDestroyBuffer)(dv, vb, 0); D(vkFreeMemory)(dv, vm, 0);
}

static void test_cull_static_pts(const char *name) {
    if (!want(name)) return;
    PDesc p = { "pts.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_POINT_LIST, NULL, 0, VK_CULL_MODE_FRONT_AND_BACK, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, 0.5f);
    D(vkCmdDraw)(cb, 16, 1, 0, 0);
    int n = finish();
    result(name, n > 0, "%d pixels (cull estático FRONT_AND_BACK em pontos: esperado > 0)", n);
    D(vkDestroyPipeline)(dv, pl, 0);
}

static void test_clip(const char *vs, const char *fs, int axis, const char *name) {
    if (!want(name)) return;
    if (!feat.shaderClipDistance) { skip(name, "shaderClipDistance não anunciado"); return; }
    PDesc p = { vs, fs, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, VK_CULL_MODE_NONE, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, 0.5f);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    int n = finish();
    /* axis 0: x<0 cortado (esquerda vazia); axis 1: y<0 cortado (linhas de cima vazias) */
    int a = axis ? px_r(W / 2, 2) : px_r(2, H / 2), b = axis ? px_r(W / 2, H - 3) : px_r(W - 3, H / 2);
    result(name, n >= W * H / 2 - W && n <= W * H / 2 + W && !a && b, "%d pixels, lado cortado=%d lado mantido=%d (esperado ~%d, 0, >0)", n, a, b, W * H / 2);
    D(vkDestroyPipeline)(dv, pl, 0);
}

static void test_clamp(float z, const char *name) {
    if (!want(name)) return;
    if (!feat.depthClamp) { skip(name, "depthClamp não anunciado"); return; }
    PDesc p = { "tri.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, VK_CULL_MODE_NONE,
                1, VK_COMPARE_OP_LESS_OR_EQUAL, 1, VK_POLYGON_MODE_FILL };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, z);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    int n = finish();
    result(name, n == W * H, "%d pixels com z=%.2f fora de [0,1] (com clamp: esperado %d; sem clamp: 0)", n, z, W * H);
    D(vkDestroyPipeline)(dv, pl, 0);
}

/* chão que cruza o near e passa por trás da câmera (caso do BT3): conta os pixels contra a
 * referência calculada na CPU (raio de cada pixel contra o plano do chão, só d > 0) */
static int in_tri(float px, float pz) {
    const float ax = -3, az = 10, bx = 3, bz = 10, cx = 0, cz = -2;
    float d1 = (px - bx) * (az - bz) - (ax - bx) * (pz - bz);
    float d2 = (px - cx) * (bz - cz) - (bx - cx) * (pz - cz);
    float d3 = (px - ax) * (cz - az) - (cx - ax) * (pz - az);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}
static void test_clamp_floor(const char *name) {
    if (!want(name)) return;
    if (!feat.depthClamp) { skip(name, "depthClamp não anunciado"); return; }
    PDesc p = { "floor.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, VK_CULL_MODE_NONE,
                1, VK_COMPARE_OP_LESS_OR_EQUAL, 1, VK_POLYGON_MODE_FILL };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    keep_depth = 1;
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, 0.5f);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    const float *dep = finish_depth();
    keep_depth = 0;
    /* referência: raio de cada pixel contra o chão (y = -0.2), z_ndc = A + B/d grampeado em [0, 1] */
    const float n_ = 1.0f, f_ = 100.0f, A = f_ / (f_ - n_), B = -f_ * n_ / (f_ - n_);
    int ref = 0, cov = 0, bad = 0; float worst = 0;
    for (int j = 1; j < H - 1; j++) for (int i = 1; i < W - 1; i++) {
        float x = (i + 0.5f) / W * 2 - 1, y = (j + 0.5f) / H * 2 - 1;
        if (y >= 0) continue;
        float d = -0.2f / y;
        if (!in_tri(x * d, d)) continue;
        /* longe das bordas do triângulo (amostragem de borda varia) */
        if (!in_tri(x * d * 1.03f, d * 1.03f) || !in_tri(x * d * 0.97f, d * 0.97f)) continue;
        ref++;
        float zr = A + B / d; if (zr < 0) zr = 0; if (zr > 1) zr = 1;
        float g = dep[j * W + i];
        if (g < 1.0f) cov++;
        float e = g > zr ? g - zr : zr - g;
        if (e > worst) worst = e;
        if (e > 0.01f) bad++;
    }
    result(name, cov >= ref - 2 && bad == 0,
           "%d de %d pixels do chão desenhados; %d com profundidade errada (>0.01), pior erro %.3f (z achatado por vértice erra perto da câmera)",
           cov, ref, bad, worst);
    D(vkDestroyPipeline)(dv, pl, 0);
}

/* VK_EXT_depth_clip_enable: sem corte o triângulo fora de [0,1] aparece (grampeado); com corte some */
static void test_depthclip(int clip, VkBool32 clamp, const char *name) {
    if (!want(name)) return;
    if (!has_dce) { skip(name, "VK_EXT_depth_clip_enable não anunciado"); return; }
    PDesc p = { "tri.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, VK_CULL_MODE_NONE,
                1, VK_COMPARE_OP_LESS_OR_EQUAL, clamp, VK_POLYGON_MODE_FILL };
    g_depthclip = clip;
    VkPipeline pl = mkpipe(&p);
    g_depthclip = -1;
    if (!pl) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, 1.5f);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    int n = finish();
    int want_ = clip ? 0 : W * H;
    result(name, n == want_, "%d pixels com z=1.50 (depthClipEnable=%d depthClampEnable=%u: esperado %d)", n, clip, clamp, want_);
    D(vkDestroyPipeline)(dv, pl, 0);
}

/* VK_EXT_depth_clip_control: z = -0.5 na convenção do OpenGL vira profundidade 0.25 (sem: cortado) */
static void test_negz(const char *name) {
    if (!want(name)) return;
    if (!has_dcc) { skip(name, "VK_EXT_depth_clip_control não anunciado"); return; }
    PDesc p = { "tri.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, VK_CULL_MODE_NONE,
                1, VK_COMPARE_OP_LESS_OR_EQUAL, 0, VK_POLYGON_MODE_FILL };
    g_negz = 1; VkPipeline pl = mkpipe(&p); g_negz = 0;
    if (!pl) { skip(name, "pipeline falhou"); return; }
    keep_depth = 1;
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push(1, 0, 0, -0.5f);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    const float *dep = finish_depth();
    keep_depth = 0;
    float zc = dep[(H / 2) * W + W / 2];
    result(name, zc > 0.24f && zc < 0.26f, "profundidade no centro %.3f (esperado 0.250; sem a emulação o triângulo é cortado: 1.000)", zc);
    D(vkDestroyPipeline)(dv, pl, 0);
}

static void test_mdi(const char *name) {
    if (!want(name)) return;
    if (!feat.multiDrawIndirect) { skip(name, "multiDrawIndirect não anunciado"); return; }
    PDesc p = { "drawid.vert.spv", "drawid.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, VK_CULL_MODE_NONE, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = 4 * sizeof(VkDrawIndirectCommand); bi.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffer ib; D(vkCreateBuffer)(dv, &bi, 0, &ib);
    VkMemoryRequirements mr; D(vkGetBufferMemoryRequirements)(dv, ib, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory im; D(vkAllocateMemory)(dv, &ai, 0, &im); D(vkBindBufferMemory)(dv, ib, im, 0);
    VkDrawIndirectCommand *dc; D(vkMapMemory)(dv, im, 0, VK_WHOLE_SIZE, 0, (void **)&dc);
    for (int i = 0; i < 4; i++) dc[i] = (VkDrawIndirectCommand){ 3, 1, 0, 0 };
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    D(vkCmdDrawIndirect)(cb, ib, 0, 4, sizeof(VkDrawIndirectCommand));
    finish();
    int q[4] = { px_r(4, 4), px_r(W / 2 + 4, 4), px_r(4, H / 2 + 4), px_r(W / 2 + 4, H / 2 + 4) };
    int ok = 1; for (int i = 0; i < 4; i++) { int e = (int)((i + 1) / 8.0 * 255 + 0.5); if (q[i] < e - 2 || q[i] > e + 2) ok = 0; }
    result(name, ok, "gl_DrawID por quadrante -> r = %d %d %d %d (esperado 32 64 96 128)", q[0], q[1], q[2], q[3]);
    D(vkDestroyPipeline)(dv, pl, 0); D(vkDestroyBuffer)(dv, ib, 0); D(vkFreeMemory)(dv, im, 0);
}

static void push_w(float r, float w) {
    PC pc = { { r, 0, 0, 1 }, 0.5f, w };
    D(vkCmdPushConstants)(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
}
/* fillModeNonSolid: mode LINE/POINT, cull estático, ordem dos vértices (w < 0 = face da frente com CCW) */
static void test_poly(VkPolygonMode mode, VkCullModeFlags cull, float w, int expect, const char *name) {
    if (!want(name)) return;
    if (!feat.fillModeNonSolid) { skip(name, "fillModeNonSolid não anunciado"); return; }
    PDesc p = { "small.vert.spv", "colv.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0, cull, 0, VK_COMPARE_OP_ALWAYS, 0, mode };
    VkPipeline pl = mkpipe(&p); if (!pl) { skip(name, "pipeline falhou"); return; }
    begin(1.0f);
    D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push_w(1, w);
    D(vkCmdDraw)(cb, 3, 1, 0, 0);
    int n = finish();
    int center = px_r(W / 2, H / 2 + 4);
    int ok;
    if (expect == 0) ok = n == 0;
    else if (mode == VK_POLYGON_MODE_POINT) ok = n >= 3 && n <= 12 && !center;
    else ok = n > 40 && n < 400 && !center;
    const char *what = expect == 0 ? "culled: esperado 0" : mode == VK_POLYGON_MODE_POINT ? "3 pontos: esperado 3..12, centro vazio" : "3 arestas: esperado 40..400, centro vazio (FILL ~512)";
    result(name, ok, "%d pixels, centro=%d (%s)", n, center, what);
    D(vkDestroyPipeline)(dv, pl, 0);
}


/* custo de CPU dos hooks: grava muitos draws (sem executar) e mede o tempo de gravação */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void bench(void) {
    static const VkDynamicState dy[] = { VK_DYNAMIC_STATE_CULL_MODE };
    PDesc p = { "tri.vert.spv", "col.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, dy, 1, VK_CULL_MODE_NONE, 0, VK_COMPARE_OP_ALWAYS, 0, VK_POLYGON_MODE_FILL };
    VkPipeline a = mkpipe(&p), b = mkpipe(&p);
    const int N = 200000;
    double best[3] = { 1e9, 1e9, 1e9 };
    /* ponteiros buscados uma vez, como um app real faz */
    PFN_vkCmdDraw draw = D(vkCmdDraw); PFN_vkCmdBindPipeline bind = D(vkCmdBindPipeline);
    PFN_vkCmdSetCullMode scm = D(vkCmdSetCullMode); PFN_vkCmdPushConstants pushc = D(vkCmdPushConstants);
    for (int rep = 0; rep < 5; rep++) {
        for (int mode = 0; mode < 3; mode++) {
            D(vkResetCommandBuffer)(cb, 0);
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            D(vkBeginCommandBuffer)(cb, &bi);
            double t0 = now_ms();
            if (mode == 0) { bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a); scm(cb, VK_CULL_MODE_NONE); for (int i = 0; i < N; i++) draw(cb, 3, 1, 0, 0); }
            if (mode == 1) for (int i = 0; i < N / 10; i++) { bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (i & 1) ? a : b); scm(cb, (i & 2) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE); }
            if (mode == 2) { bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a); for (int i = 0; i < N / 10; i++) pushc(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rptr); }
            double t = now_ms() - t0;
            D(vkEndCommandBuffer)(cb);
            if (t < best[mode]) best[mode] = t;
        }
    }
    printf("BENCH vkCmdDraw: %.1f ns/chamada | bind+cull: %.1f ns/par | push (controle, sem hook): %.1f ns\n",
           best[0] * 1e6 / N, best[1] * 1e6 / (N / 10), best[2] * 1e6 / (N / 10));
}

int main(int c, char **v) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (c < 3) { printf("uso: e47 <hal.so> <dir_spv> [filtro]\n"); return 2; }
    dir = v[2]; if (c > 3) only = v[3];
    void *h = dlopen(v[1], RTLD_NOW); if (!h) { printf("%s\n", dlerror()); return 1; }
    HM *hm = dlsym(h, "HMI"); DEV *d = 0;
    if (hm->methods->open(hm, "vk0", (void **)&d)) { printf("open falhou\n"); return 1; }
    printf("HAL: %s\n", hm->name); gi = d->gipa;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &ai;
    if (d->ci(&ici, 0, &in)) { printf("vkCreateInstance falhou\n"); return 1; }
    uint32_t n = 1; I(vkEnumeratePhysicalDevices)(in, &n, &pd);
    I(vkGetPhysicalDeviceFeatures)(pd, &feat);
    VkPhysicalDeviceProperties pr; I(vkGetPhysicalDeviceProperties)(pd, &pr);
    {
        uint32_t ne = 0; I(vkEnumerateDeviceExtensionProperties)(pd, NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne, sizeof *ep); I(vkEnumerateDeviceExtensionProperties)(pd, NULL, &ne, ep);
        for (uint32_t i = 0; i < ne; i++) { if (!strcmp(ep[i].extensionName, "VK_EXT_depth_clip_enable")) has_dce = 1; if (!strcmp(ep[i].extensionName, "VK_EXT_depth_clip_control")) has_dcc = 1; }
        free(ep);
    }
    printf("GPU %s drv 0x%x | anunciado: clip=%u depthClamp=%u nonSolid=%u MDI=%u\n", pr.deviceName, pr.driverVersion,
           feat.shaderClipDistance, feat.depthClamp, feat.fillModeNonSolid, feat.multiDrawIndirect);
    float pri = 1; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pri };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    f2.features.depthClamp = feat.depthClamp; f2.features.fillModeNonSolid = feat.fillModeNonSolid;
    f2.features.shaderClipDistance = feat.shaderClipDistance; f2.features.shaderCullDistance = feat.shaderCullDistance;
    f2.features.multiDrawIndirect = feat.multiDrawIndirect; f2.features.largePoints = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &q };
    gd = (PFN_vkGetDeviceProcAddr)gi(in, "vkGetDeviceProcAddr");
    VkResult r = I(vkCreateDevice)(pd, &dci, 0, &dv);
    if (r) { printf("vkCreateDevice falhou %d\n", r); return 1; }
    D(vkGetDeviceQueue)(dv, 0, 0, &qu);

    /* alvo de cor, profundidade e buffer de leitura */
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R8G8B8A8_UNORM; ic.extent = (VkExtent3D){ W, H, 1 };
    ic.mipLevels = ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    D(vkCreateImage)(dv, &ic, 0, &cimg);
    ic.format = VK_FORMAT_D32_SFLOAT; ic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    D(vkCreateImage)(dv, &ic, 0, &dimg);
    VkMemoryRequirements mr;
    D(vkGetImageMemoryRequirements)(dv, cimg, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    D(vkAllocateMemory)(dv, &mai, 0, &cmem); D(vkBindImageMemory)(dv, cimg, cmem, 0);
    D(vkGetImageMemoryRequirements)(dv, dimg, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    D(vkAllocateMemory)(dv, &mai, 0, &dmem); D(vkBindImageMemory)(dv, dimg, dmem, 0);
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vc.image = cimg; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = VK_FORMAT_R8G8B8A8_UNORM;
    vc.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    D(vkCreateImageView)(dv, &vc, 0, &cview);
    vc.image = dimg; vc.format = VK_FORMAT_D32_SFLOAT; vc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    D(vkCreateImageView)(dv, &vc, 0, &dview);
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = W * H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    D(vkCreateBuffer)(dv, &bci, 0, &rbuf);
    D(vkGetBufferMemoryRequirements)(dv, rbuf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    D(vkAllocateMemory)(dv, &mai, 0, &rmem); D(vkBindBufferMemory)(dv, rbuf, rmem, 0);
    D(vkMapMemory)(dv, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&rptr);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    D(vkCreateCommandPool)(dv, &pci, 0, &pool);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    D(vkAllocateCommandBuffers)(dv, &cai, &cb);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    D(vkCreateFence)(dv, &fci, 0, &fence);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PC) };
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pcr;
    D(vkCreatePipelineLayout)(dv, &lci, 0, &layout);

    if (getenv("E47_BENCH")) { bench(); return 0; }
    test_wait(0, "wait_fence_sinal");
    test_wait(1, "wait_semaforo_sinal");
    test_cull_nontri(VK_PRIMITIVE_TOPOLOGY_POINT_LIST, "cull_pontos");
    test_cull_nontri(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, "cull_linhas");
    test_dyn(0, "dyn_cull_antes_bind");
    test_dyn(1, "dyn_depthop_antes_bind");
    test_dyn(2, "dyn_depthtest_antes");
    test_dyn(3, "dyn_cull_troca_pipe");
    test_dyn_stride("dyn_stride_antes");
    test_cull_static_pts("cull_estatico_pontos");
    test_clip("clip.vert.spv", "col.frag.spv", 0, "clip_bloco");
    test_clip("clipv.vert.spv", "colv.frag.spv", 1, "clip_solto");
    test_clamp(1.5f, "clamp_alem_far");
    test_clamp(-0.5f, "clamp_antes_near");
    test_clamp_floor("clamp_chao");
    test_depthclip(0, VK_FALSE, "depthclip_off_far");
    test_depthclip(1, VK_TRUE, "depthclip_on_far");
    test_negz("depthclip_control_gl");
    test_mdi("mdi_drawid");
    test_poly(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE, 1, 1, "line_wireframe");
    test_poly(VK_POLYGON_MODE_POINT, VK_CULL_MODE_NONE, 1, 1, "line_pontos");
    /* em Vulkan (y para baixo) a ordem w=1 do small.vert é horária na tela: face de trás com CCW */
    test_poly(VK_POLYGON_MODE_LINE, VK_CULL_MODE_BACK_BIT, 1, 0, "line_cull_tras");
    test_poly(VK_POLYGON_MODE_LINE, VK_CULL_MODE_BACK_BIT, -1, 1, "line_cull_frente_fica");
    printf("RESUMO: %d OK, %d BUG, %d pulados\n", n_ok, n_bug, n_skip);
    D(vkDeviceWaitIdle)(dv);
    return n_bug ? 3 : 0;
}
