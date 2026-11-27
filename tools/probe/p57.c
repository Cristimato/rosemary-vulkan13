// p57 <hal.so> <dir_spv> [carga] : benchmark de GPU da v5.7 (ms por quadro, 1280x720)
//   p57 cmp <a.raw> <b.raw>      : PSNR entre duas imagens gravadas (qualidade)
// Build: tools/probe/p57/build.sh. Rodar como root com
//   LD_LIBRARY_PATH=/vendor/lib64/hw:/vendor/lib64:/vendor/lib64/mt6785
// Cargas: tex (4 amostras de textura 2048² com mipmaps, sampler anisotrópico 16x, plano inclinado),
//         alu (iluminação com 24 luzes), vary (5 varyings). Cada quadro = 4 desenhos de tela cheia.
// P57_OUT=<dir> grava <dir>/<carga>.raw (RGBA8) para comparar variantes com "p57 cmp".
// As otimizações do vkshim são escolhidas pelas props debug.vkshim.* antes de rodar.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef struct { int (*open)(void *, const char *, void **); } M;
typedef struct { uint32_t tag; uint16_t a, b; const char *id, *name, *author; M *methods; } HM;
typedef struct { uint32_t tag, ver; void *mod; uint64_t r[12]; void *close; void *eiep; PFN_vkCreateInstance ci; PFN_vkGetInstanceProcAddr gipa; } DEV;

#define W 1280
#define H 720
#define TEX 2048
#define MIPS 12
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
static const char *dir;

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
    D(vkMapMemory)(dv, r.m, 0, VK_WHOLE_SIZE, 0, &r.p);
    return r;
}
static VkCommandBuffer newcb(void) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; D(vkAllocateCommandBuffers)(dv, &ai, &cb); return cb;
}
static void submit_wait(VkCommandBuffer cb) {
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    D(vkResetFences)(dv, 1, &fence);
    D(vkQueueSubmit)(qu, 1, &si, fence);
    D(vkWaitForFences)(dv, 1, &fence, VK_TRUE, 10000000000ull);
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout a, VkImageLayout b, uint32_t mips,
                    VkPipelineStageFlags s0, VkAccessFlags a0, VkPipelineStageFlags s1, VkAccessFlags a1) {
    VkImageMemoryBarrier m = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, a0, a1, a, b, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                               img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 } };
    D(vkCmdPipelineBarrier)(cb, s0, s1, 0, 0, 0, 0, 0, 1, &m);
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int cmp_images(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { printf("não abri as imagens\n"); return 1; }
    static uint8_t pa[W * H * 4], pb[W * H * 4];
    if (fread(pa, 1, sizeof pa, fa) != sizeof pa || fread(pb, 1, sizeof pb, fb) != sizeof pb) { printf("tamanho errado\n"); return 1; }
    double se = 0; int maxd = 0; long dif = 0;
    for (size_t i = 0; i < sizeof pa; i++) {
        if ((i & 3) == 3) continue;
        int d = (int)pa[i] - (int)pb[i]; if (d < 0) d = -d;
        se += (double)d * d; if (d > maxd) maxd = d; if (d > 2) dif++;
    }
    double mse = se / (W * H * 3.0);
    double psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99.0;
    printf("PSNR %.1f dB | maior diferença %d/255 | %.2f%% dos canais com diferença > 2\n", psnr, maxd, 100.0 * dif / (W * H * 3.0));
    return 0;
}

int main(int c, char **v) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (c >= 4 && !strcmp(v[1], "cmp")) return cmp_images(v[2], v[3]);
    if (c < 3) { printf("uso: p57 <hal.so> <dir_spv> [tex|alu|vary]\n       p57 cmp <a.raw> <b.raw>\n"); return 2; }
    dir = v[2];
    const char *only = c > 3 ? v[3] : NULL;
    void *h = dlopen(v[1], RTLD_NOW); if (!h) { printf("%s\n", dlerror()); return 1; }
    HM *hm = dlsym(h, "HMI"); DEV *d = 0;
    if (hm->methods->open(hm, "vk0", (void **)&d)) { printf("open falhou\n"); return 1; }
    gi = d->gipa;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &ai;
    if (d->ci(&ici, 0, &in)) { printf("vkCreateInstance falhou\n"); return 1; }
    uint32_t n = 1; I(vkEnumeratePhysicalDevices)(in, &n, &pd);
    float pri = 1; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pri };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES }; f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 }; f2.features.samplerAnisotropy = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &q };
    gd = (PFN_vkGetDeviceProcAddr)gi(in, "vkGetDeviceProcAddr");
    if (I(vkCreateDevice)(pd, &dci, 0, &dv)) { printf("vkCreateDevice falhou\n"); return 1; }
    D(vkGetDeviceQueue)(dv, 0, 0, &qu);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    D(vkCreateCommandPool)(dv, &pci, 0, &pool);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; D(vkCreateFence)(dv, &fci, 0, &fence);

    /* alvo de cor */
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { W, H, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage cimg; D(vkCreateImage)(dv, &ic, 0, &cimg);
    VkMemoryRequirements mr; D(vkGetImageMemoryRequirements)(dv, cimg, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory cmem; D(vkAllocateMemory)(dv, &mai, 0, &cmem); D(vkBindImageMemory)(dv, cimg, cmem, 0);
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, cimg, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView cview; D(vkCreateImageView)(dv, &vc, 0, &cview);
    /* textura 2048² com 12 mips (ruído por nível) */
    VkImageCreateInfo tc = ic; tc.extent = (VkExtent3D){ TEX, TEX, 1 }; tc.mipLevels = MIPS;
    tc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage timg; D(vkCreateImage)(dv, &tc, 0, &timg);
    D(vkGetImageMemoryRequirements)(dv, timg, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory tmem; D(vkAllocateMemory)(dv, &mai, 0, &tmem); D(vkBindImageMemory)(dv, timg, tmem, 0);
    VkDeviceSize total = 0; for (int m = 0; m < MIPS; m++) { uint32_t s = TEX >> m; total += (VkDeviceSize)s * s * 4; }
    Buf st = mkbuf(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkBufferImageCopy rg[MIPS]; VkDeviceSize off = 0; uint32_t seed = 12345;
    for (int m = 0; m < MIPS; m++) {
        uint32_t s = TEX >> m; uint8_t *p = (uint8_t *)st.p + off;
        for (uint32_t i = 0; i < s * s; i++) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t x = i % s, y = i / s; int chk = ((x >> 3) ^ (y >> 3)) & 1;
            /* xadrez + gradientes suaves + um pouco de ruído: parecido com textura de jogo */
            p[i * 4 + 0] = (uint8_t)(chk ? 200 : 40); p[i * 4 + 1] = (uint8_t)((x * 255u) / s * 3 / 4 + (seed >> 27));
            p[i * 4 + 2] = (uint8_t)((y * 255u) / s); p[i * 4 + 3] = 255;
        }
        rg[m] = (VkBufferImageCopy){ off, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, 1 }, { 0, 0, 0 }, { s, s, 1 } };
        off += (VkDeviceSize)s * s * 4;
    }
    VkCommandBuffer cb = newcb();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    D(vkBeginCommandBuffer)(cb, &bi);
    barrier(cb, timg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MIPS, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    D(vkCmdCopyBufferToImage)(cb, st.b, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MIPS, rg);
    barrier(cb, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, MIPS, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    D(vkEndCommandBuffer)(cb); submit_wait(cb);
    VkImageViewCreateInfo tvc = vc; tvc.image = timg; tvc.subresourceRange.levelCount = MIPS;
    VkImageView tview; D(vkCreateImageView)(dv, &tvc, 0, &tview);
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, VK_TRUE, 16.0f, 0, 0, 0, 1000.0f, 0, 0 };
    VkSampler smp; D(vkCreateSampler)(dv, &sci, 0, &smp);
    /* descriptor + layout + pipelines */
    VkDescriptorSetLayoutBinding bd = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &bd };
    VkDescriptorSetLayout dsl; D(vkCreateDescriptorSetLayout)(dv, &lci, 0, &dsl);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl };
    VkPipelineLayout pl; D(vkCreatePipelineLayout)(dv, &pli, 0, &pl);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps };
    VkDescriptorPool dp; D(vkCreateDescriptorPool)(dv, &dpc, 0, &dp);
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl };
    VkDescriptorSet ds; D(vkAllocateDescriptorSets)(dv, &dai, &ds);
    VkDescriptorImageInfo dii = { smp, tview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, 0, 0 };
    D(vkUpdateDescriptorSets)(dv, 1, &wds, 0, 0);
    Buf rb = mkbuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    const char *names[3] = { "tex", "alu", "vary" }, *frags[3] = { "tex.frag.spv", "alu.frag.spv", "vary.frag.spv" };
    VkShaderModule vs = sm("plano.vert.spv");
    const char *outdir = getenv("P57_OUT");
    int frames = getenv("P57_FRAMES") ? atoi(getenv("P57_FRAMES")) : 40;
    for (int k = 0; k < 3; k++) {
        if (only && strcmp(only, names[k])) continue;
        VkPipelineShaderStageCreateInfo stg[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sm(frags[k]), "main", 0 } };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
        VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = 1;
        /* blend aditivo: cada desenho realmente custa (sem eliminação de pixels escondidos) */
        VkPipelineColorBlendAttachmentState ba = { VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
                                                   VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xf };
        VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cbs.attachmentCount = 1; cbs.pAttachments = &ba;
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO }; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
        VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stg, &vi, &ia, 0, &vps, &rs, &ms, 0, &cbs, 0, pl };
        VkPipeline pp; if (D(vkCreateGraphicsPipelines)(dv, 0, 1, &gci, 0, &pp)) { printf("%s: pipeline falhou\n", names[k]); continue; }
        double t[512]; int nt = 0;
        for (int f = 0; f < frames + 5 && nt < 512; f++) {
            D(vkResetCommandBuffer)(cb, 0);
            D(vkBeginCommandBuffer)(cb, &bi);
            barrier(cb, cimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, cview, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rinf = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &ca };
            D(vkCmdBeginRendering)(cb, &rinf);
            D(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
            D(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, 0);
            for (int r = 0; r < 4; r++) D(vkCmdDraw)(cb, 3, 1, 0, 0);
            D(vkCmdEndRendering)(cb);
            int last = f == frames + 4;
            if (last) {
                barrier(cb, cimg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy rc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
                D(vkCmdCopyImageToBuffer)(cb, cimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &rc);
            }
            D(vkEndCommandBuffer)(cb);
            double t0 = now_ms();
            submit_wait(cb);
            if (f >= 5 && !last) t[nt++] = now_ms() - t0;
        }
        qsort(t, nt, sizeof *t, cmpd);
        double med = nt ? t[nt / 2] : 0, p90 = nt ? t[nt * 9 / 10] : 0;
        printf("%-5s %7.2f ms/quadro (p90 %.2f) = %5.1f fps equivalentes\n", names[k], med, p90, med > 0 ? 1000.0 / med : 0);
        if (outdir) {
            char p[512]; snprintf(p, sizeof p, "%s/%s.raw", outdir, names[k]);
            FILE *fo = fopen(p, "wb"); if (fo) { fwrite(rb.p, 1, W * H * 4, fo); fclose(fo); }
        }
        D(vkDestroyPipeline)(dv, pp, 0);
    }
    return 0;
}
