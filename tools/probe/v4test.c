// v4test <hal.so> <dir_dos_spv>: teste de ponta a ponta do vkshim v4 (sem app)
// 1) recursos anunciados  2) vkCreateDevice pedindo os recursos emulados
// 3) shader modules (ClipDistance, bitwise, compute lockstep)  4) pipeline gráfico
//    com depthClamp + POLYGON_MODE_LINE  5) pipeline compute  6) grava e executa
//    vkCmdDrawIndirect com drawCount=4 (multiDrawIndirect emulado)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { int (*open)(void*,const char*,void**); } M;
typedef struct { uint32_t tag; uint16_t a,b; const char*id,*name,*author; M*methods; } HM;
typedef struct { uint32_t tag,ver; void*mod; uint64_t r[12]; void*close; void*eiep; PFN_vkCreateInstance ci; PFN_vkGetInstanceProcAddr gipa; } DEV;
static PFN_vkGetInstanceProcAddr gi; static PFN_vkGetDeviceProcAddr gd; static VkInstance in; static VkDevice dv;
#define I(n) ((PFN_##n)gi(in,#n))
#define D(n) ((PFN_##n)gd(dv,#n))
static int fails;
#define CK(x,msg) do{ VkResult _r=(x); printf("%-46s %s (%d)\n",msg,_r==VK_SUCCESS?"OK":"FALHOU",_r); if(_r) fails++; }while(0)
static uint32_t *rd(const char*dir,const char*f,size_t*sz){ char p[512]; snprintf(p,sizeof p,"%s/%s",dir,f); FILE*h=fopen(p,"rb"); if(!h){printf("sem %s\n",p);exit(1);} fseek(h,0,2); *sz=ftell(h); fseek(h,0,0); uint32_t*b=malloc(*sz); fread(b,1,*sz,h); fclose(h); return b; }
static VkShaderModule sm(const char*dir,const char*f){ size_t sz; uint32_t*c=rd(dir,f,&sz); VkShaderModuleCreateInfo ci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,0,0,sz,c}; VkShaderModule m=0; char msg[96]; snprintf(msg,sizeof msg,"vkCreateShaderModule %s",f); CK(D(vkCreateShaderModule)(dv,&ci,0,&m),msg); free(c); return m; }
int main(int c,char**v){
  setvbuf(stdout,0,_IONBF,0);
  if(c<3){printf("uso: v4test <hal.so> <dir_spv>\n");return 2;}
  void*h=dlopen(v[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  HM*hm=dlsym(h,"HMI"); DEV*d=0; if(hm->methods->open(hm,"vk0",(void**)&d)){printf("open falhou\n");return 1;}
  printf("HAL: %s\n",hm->name); gi=d->gipa;
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&ai;
  CK(d->ci(&ici,0,&in),"vkCreateInstance (via struct da HAL)");
  uint32_t n=1; VkPhysicalDevice pd; I(vkEnumeratePhysicalDevices)(in,&n,&pd);
  VkPhysicalDeviceFeatures f; I(vkGetPhysicalDeviceFeatures)(pd,&f);
  VkPhysicalDeviceProperties pr; I(vkGetPhysicalDeviceProperties)(pd,&pr);
  printf("anunciado: depthClamp=%u fillModeNonSolid=%u clip=%u cull=%u MDI=%u multiVP=%u vsStores=%u maxDrawIndirectCount=%u\n",
    f.depthClamp,f.fillModeNonSolid,f.shaderClipDistance,f.shaderCullDistance,f.multiDrawIndirect,f.multiViewport,f.vertexPipelineStoresAndAtomics,pr.limits.maxDrawIndirectCount);
  float pri=1; VkDeviceQueueCreateInfo q={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,0,0,0,1,&pri};
  VkPhysicalDeviceFeatures en={0}; en.depthClamp=f.depthClamp; en.fillModeNonSolid=f.fillModeNonSolid; en.shaderClipDistance=f.shaderClipDistance; en.shaderCullDistance=f.shaderCullDistance; en.multiDrawIndirect=f.multiDrawIndirect;
  VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,0,0,1,&q,0,0,0,0,&en};
  gd=(PFN_vkGetDeviceProcAddr)gi(in,"vkGetDeviceProcAddr");
  CK(I(vkCreateDevice)(pd,&dci,0,&dv),"vkCreateDevice c/ recursos emulados");
  if(fails) return 1;
  printf("struct do app preservado apos CreateDevice: depthClamp=%u clip=%u MDI=%u\n",en.depthClamp,en.shaderClipDistance,en.multiDrawIndirect);
  VkShaderModule vs=sm(v[2],getenv("VS")?getenv("VS"):"clip.vert.spv"), fs=sm(v[2],getenv("FS")?getenv("FS"):"bits.frag.spv"), cs=sm(v[2],"lock.comp.spv");
  /* render pass 64x64 RGBA8 */
  VkAttachmentDescription at={0,VK_FORMAT_R8G8B8A8_UNORM,1,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,0,VK_IMAGE_LAYOUT_GENERAL};
  VkAttachmentReference ar={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp={0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,0,1,&ar};
  VkRenderPassCreateInfo rpci={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,0,0,1,&at,1,&sp}; VkRenderPass rp;
  CK(D(vkCreateRenderPass)(dv,&rpci,0,&rp),"vkCreateRenderPass");
  VkPushConstantRange pcr={VK_SHADER_STAGE_COMPUTE_BIT,0,4};
  VkPipelineLayoutCreateInfo plci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VkPipelineLayout pl; CK(D(vkCreatePipelineLayout)(dv,&plci,0,&pl),"vkCreatePipelineLayout");
  VkPipelineShaderStageCreateInfo st[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main"},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs,"main"}};
  VkVertexInputBindingDescription vb={0,16,0}; VkVertexInputAttributeDescription va={0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0};
  VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,0,0,1,&vb,1,&va};
  VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,0,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport vp={0,0,64,64,0,1}; VkRect2D sc={{0,0},{64,64}};
  VkPipelineViewportStateCreateInfo vps={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,0,0,1,&vp,1,&sc};
  VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.depthClampEnable=VK_TRUE; rs.polygonMode=VK_POLYGON_MODE_LINE; rs.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=1;
  VkPipelineColorBlendAttachmentState cba={0}; cba.colorWriteMask=0xf;
  VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&cba;
  VkGraphicsPipelineCreateInfo gp={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,0,0,2,st,&vi,&ia,0,&vps,&rs,&ms,0,&cb,0,pl,rp,0};
  VkPipeline gpipe=0; CK(D(vkCreateGraphicsPipelines)(dv,0,1,&gp,0,&gpipe),"pipeline grafico (clip+depthClamp+LINE)");
  printf("rasterizacao do app preservada: depthClamp=%u poly=%d\n",rs.depthClampEnable,rs.polygonMode);
  VkDescriptorSetLayoutBinding b0={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT};
  VkDescriptorSetLayoutCreateInfo dl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,0,0,1,&b0}; VkDescriptorSetLayout dsl; D(vkCreateDescriptorSetLayout)(dv,&dl,0,&dsl);
  VkPipelineLayoutCreateInfo cpl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,0,0,1,&dsl,1,&pcr}; VkPipelineLayout pl2; D(vkCreatePipelineLayout)(dv,&cpl,0,&pl2);
  VkComputePipelineCreateInfo cp={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,0,0,{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_COMPUTE_BIT,cs,"main"},pl2};
  VkPipeline cpipe=0; CK(D(vkCreateComputePipelines)(dv,0,1,&cp,0,&cpipe),"pipeline compute (lockstep)");
  /* --- executa: imagem, framebuffer, buffers de vertice e indirect com 4 comandos --- */
  VkPhysicalDeviceMemoryProperties mp; I(vkGetPhysicalDeviceMemoryProperties)(pd,&mp);
  #define MEMTYPE(bits,flags) ({int _t=-1; for(uint32_t k=0;k<mp.memoryTypeCount;k++) if(((bits)>>k&1)&&(mp.memoryTypes[k].propertyFlags&(flags))==(flags)){_t=k;break;} _t;})
  VkImageCreateInfo ic={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,0,0,VK_IMAGE_TYPE_2D,VK_FORMAT_R8G8B8A8_UNORM,{64,64,1},1,1,1,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
  VkImage img; D(vkCreateImage)(dv,&ic,0,&img); VkMemoryRequirements mr; D(vkGetImageMemoryRequirements)(dv,img,&mr);
  VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,mr.size,MEMTYPE(mr.memoryTypeBits,0)}; VkDeviceMemory im; D(vkAllocateMemory)(dv,&ma,0,&im); D(vkBindImageMemory)(dv,img,im,0);
  VkImageViewCreateInfo ivc={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,0,0,img,VK_IMAGE_VIEW_TYPE_2D,VK_FORMAT_R8G8B8A8_UNORM,{0},{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}}; VkImageView iv; D(vkCreateImageView)(dv,&ivc,0,&iv);
  VkFramebufferCreateInfo fbc={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,0,0,rp,1,&iv,64,64,1}; VkFramebuffer fb; D(vkCreateFramebuffer)(dv,&fbc,0,&fb);
  VkBufferCreateInfo bc={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,4096,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
  VkBuffer buf; D(vkCreateBuffer)(dv,&bc,0,&buf); D(vkGetBufferMemoryRequirements)(dv,buf,&mr);
  VkMemoryAllocateInfo mb={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,mr.size,MEMTYPE(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)}; VkDeviceMemory bm; D(vkAllocateMemory)(dv,&mb,0,&bm); D(vkBindBufferMemory)(dv,buf,bm,0);
  char*p; D(vkMapMemory)(dv,bm,0,4096,0,(void**)&p); memset(p,0,4096);
  float tri[12]={-1,-1,0,1, 1,-1,0,1, 0,1,0,1}; memcpy(p,tri,sizeof tri);
  VkDrawIndirectCommand *ind=(void*)(p+1024); for(int k=0;k<4;k++){ ind[k].vertexCount=3; ind[k].instanceCount=1; ind[k].firstVertex=0; ind[k].firstInstance=0; }
  VkCommandPoolCreateInfo cpc={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,0,0,0}; VkCommandPool cpool; D(vkCreateCommandPool)(dv,&cpc,0,&cpool);
  VkCommandBufferAllocateInfo cba2={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,cpool,0,1}; VkCommandBuffer cmd; D(vkAllocateCommandBuffers)(dv,&cba2,&cmd);
  VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; D(vkBeginCommandBuffer)(cmd,&bi);
  VkClearValue cv={0}; VkRenderPassBeginInfo rpb={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,0,rp,fb,{{0,0},{64,64}},1,&cv};
  D(vkCmdBeginRenderPass)(cmd,&rpb,0); D(vkCmdBindPipeline)(cmd,0,gpipe); VkDeviceSize zo=0; D(vkCmdBindVertexBuffers)(cmd,0,1,&buf,&zo);
  D(vkCmdDrawIndirect)(cmd,buf,1024,4,sizeof(VkDrawIndirectCommand)); D(vkCmdEndRenderPass)(cmd);
  CK(D(vkEndCommandBuffer)(cmd),"grava vkCmdDrawIndirect(drawCount=4)");
  VkQueue qu; D(vkGetDeviceQueue)(dv,0,0,&qu); VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd};
  CK(D(vkQueueSubmit)(qu,1,&si,0),"vkQueueSubmit"); CK(D(vkQueueWaitIdle)(qu),"execucao na GPU (QueueWaitIdle)");
  D(vkDestroyDevice)(dv,0);
  printf("RESULTADO: %s (%d falhas)\n",fails?"FALHOU":"TUDO OK",fails); return fails?1:0;
}
