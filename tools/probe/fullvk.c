#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct { int (*open)(void*,const char*,void**); } M;
typedef struct { uint32_t tag; uint16_t a,b; const char*id,*name,*author; M*methods; } HM;
typedef struct { uint32_t tag,ver; void*mod; uint64_t r[12]; void*close; void*eiep; void*ci; PFN_vkGetInstanceProcAddr gipa; } DEV;
#define PB(s,fld) printf(" %s=%u",#fld,(s).fld)
int main(int c,char**v){
  void*h=dlopen(v[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  HM*hm=dlsym(h,"HMI"); DEV*d=0; if(hm->methods->open(hm,"vk0",(void**)&d))return 1;
  PFN_vkGetInstanceProcAddr g=d->gipa;
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&ai; VkInstance in;
  if(((PFN_vkCreateInstance)g(NULL,"vkCreateInstance"))(&ci,NULL,&in))return 2;
  uint32_t n=1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)g(in,"vkEnumeratePhysicalDevices"))(in,&n,&pd);
  VkPhysicalDeviceProperties p; ((PFN_vkGetPhysicalDeviceProperties)g(in,"vkGetPhysicalDeviceProperties"))(pd,&p);
  printf("%s API %u.%u.%u drv 0x%x\n",p.deviceName,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion),p.driverVersion);
  int v13 = VK_VERSION_MINOR(p.apiVersion)>=3, v12=VK_VERSION_MINOR(p.apiVersion)>=2;
  VkPhysicalDeviceVulkan13Features f13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13?&f13:NULL};
  VkPhysicalDeviceVulkan11Features f11={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, v12?&f12:NULL};
  VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v12?&f11:NULL};
  ((PFN_vkGetPhysicalDeviceFeatures2)g(in,"vkGetPhysicalDeviceFeatures2"))(pd,&f2);
  printf("== core features (0 = falta):\n");
  VkBool32 *fb=(VkBool32*)&f2.features; const char*nm[]={"robustBufferAccess","fullDrawIndexUint32","imageCubeArray","independentBlend","geometryShader","tessellationShader","sampleRateShading","dualSrcBlend","logicOp","multiDrawIndirect","drawIndirectFirstInstance","depthClamp","depthBiasClamp","fillModeNonSolid","depthBounds","wideLines","largePoints","alphaToOne","multiViewport","samplerAnisotropy","textureCompressionETC2","textureCompressionASTC_LDR","textureCompressionBC","occlusionQueryPrecise","pipelineStatisticsQuery","vertexPipelineStoresAndAtomics","fragmentStoresAndAtomics","shaderTessellationAndGeometryPointSize","shaderImageGatherExtended","shaderStorageImageExtendedFormats","shaderStorageImageMultisample","shaderStorageImageReadWithoutFormat","shaderStorageImageWriteWithoutFormat","shaderUniformBufferArrayDynamicIndexing","shaderSampledImageArrayDynamicIndexing","shaderStorageBufferArrayDynamicIndexing","shaderStorageImageArrayDynamicIndexing","shaderClipDistance","shaderCullDistance","shaderFloat64","shaderInt64","shaderInt16","shaderResourceResidency","shaderResourceMinLod","sparseBinding","sparseResidencyBuffer","sparseResidencyImage2D","sparseResidencyImage3D","sparseResidency2Samples","sparseResidency4Samples","sparseResidency8Samples","sparseResidency16Samples","sparseResidencyAliased","variableMultisampleRate","inheritedQueries"};
  for(int i=0;i<55;i++) printf(" %s=%u%s",nm[i],fb[i],(i%4==3)?"\n":"");
  printf("\n");
  if(v12){ printf("== 1.1:"); PB(f11,storageBuffer16BitAccess);PB(f11,multiview);PB(f11,variablePointers);PB(f11,shaderDrawParameters);PB(f11,samplerYcbcrConversion);
    printf("\n== 1.2:"); PB(f12,drawIndirectCount);PB(f12,storageBuffer8BitAccess);PB(f12,shaderFloat16);PB(f12,shaderInt8);PB(f12,descriptorIndexing);
    PB(f12,shaderSampledImageArrayNonUniformIndexing);PB(f12,shaderStorageBufferArrayNonUniformIndexing);PB(f12,descriptorBindingSampledImageUpdateAfterBind);
    PB(f12,descriptorBindingPartiallyBound);PB(f12,descriptorBindingVariableDescriptorCount);PB(f12,runtimeDescriptorArray);PB(f12,samplerFilterMinmax);
    PB(f12,scalarBlockLayout);PB(f12,imagelessFramebuffer);PB(f12,uniformBufferStandardLayout);PB(f12,shaderSubgroupExtendedTypes);PB(f12,separateDepthStencilLayouts);
    PB(f12,hostQueryReset);PB(f12,timelineSemaphore);PB(f12,bufferDeviceAddress);PB(f12,vulkanMemoryModel);PB(f12,shaderOutputViewportIndex);PB(f12,shaderOutputLayer);PB(f12,samplerMirrorClampToEdge);
    printf("\n"); }
  if(v13){ printf("== 1.3:"); PB(f13,robustImageAccess);PB(f13,inlineUniformBlock);PB(f13,pipelineCreationCacheControl);PB(f13,privateData);PB(f13,shaderDemoteToHelperInvocation);
    PB(f13,shaderTerminateInvocation);PB(f13,subgroupSizeControl);PB(f13,computeFullSubgroups);PB(f13,synchronization2);PB(f13,textureCompressionASTC_HDR);
    PB(f13,shaderZeroInitializeWorkgroupMemory);PB(f13,dynamicRendering);PB(f13,shaderIntegerDotProduct);PB(f13,maintenance4); printf("\n"); }
  VkPhysicalDeviceLimits *L=&p.limits;
  printf("== limits: maxImageDimension2D=%u maxPerStageDescriptorSamplers=%u maxPerStageDescriptorSampledImages=%u maxPerStageDescriptorStorageBuffers=%u maxPerStageDescriptorUniformBuffers=%u maxPerStageResources=%u maxBoundDescriptorSets=%u maxPushConstantsSize=%u maxVertexInputAttributes=%u maxVertexInputBindings=%u maxColorAttachments=%u maxComputeSharedMemorySize=%u maxSamplerAnisotropy=%.0f maxDrawIndirectCount=%u maxUniformBufferRange=%u maxStorageBufferRange=%u maxFragmentOutputAttachments=%u timestampPeriod=%.1f\n",
    L->maxImageDimension2D,L->maxPerStageDescriptorSamplers,L->maxPerStageDescriptorSampledImages,L->maxPerStageDescriptorStorageBuffers,L->maxPerStageDescriptorUniformBuffers,L->maxPerStageResources,L->maxBoundDescriptorSets,L->maxPushConstantsSize,L->maxVertexInputAttributes,L->maxVertexInputBindings,L->maxColorAttachments,L->maxComputeSharedMemorySize,L->maxSamplerAnisotropy,L->maxDrawIndirectCount,L->maxUniformBufferRange,L->maxStorageBufferRange,L->maxFragmentOutputAttachments,L->timestampPeriod);
  uint32_t ne=0; PFN_vkEnumerateDeviceExtensionProperties ee=(void*)g(in,"vkEnumerateDeviceExtensionProperties"); ee(pd,NULL,&ne,NULL);
  VkExtensionProperties *ex=calloc(ne,sizeof *ex); ee(pd,NULL,&ne,ex); printf("== %u device extensions:\n",ne);
  for(uint32_t i=0;i<ne;i++) printf(" %s",ex[i].extensionName); printf("\n");
  PFN_vkGetPhysicalDeviceFormatProperties gf=(void*)g(in,"vkGetPhysicalDeviceFormatProperties");
  struct {VkFormat f; const char*n;} fm[]={{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,"BC1"},{VK_FORMAT_BC3_UNORM_BLOCK,"BC3"},{VK_FORMAT_BC5_UNORM_BLOCK,"BC5"},{VK_FORMAT_BC7_UNORM_BLOCK,"BC7"},{VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,"ETC2"},{VK_FORMAT_ASTC_4x4_UNORM_BLOCK,"ASTC4x4"},{VK_FORMAT_D24_UNORM_S8_UINT,"D24S8"},{VK_FORMAT_D32_SFLOAT_S8_UINT,"D32S8"},{VK_FORMAT_D32_SFLOAT,"D32"},{VK_FORMAT_X8_D24_UNORM_PACK32,"X8D24"},{VK_FORMAT_R16G16B16A16_UNORM,"RGBA16unorm"},{VK_FORMAT_R16G16B16A16_SFLOAT,"RGBA16F"},{VK_FORMAT_B10G11R11_UFLOAT_PACK32,"R11G11B10F"},{VK_FORMAT_A2B10G10R10_UNORM_PACK32,"RGB10A2"},{VK_FORMAT_R32G32B32_SFLOAT,"RGB32F"},{VK_FORMAT_R8G8B8_UNORM,"RGB8"},{VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,"RGB9E5"}};
  printf("== formatos (opt tiling: S=sampled F=filter C=color-att D=depth B=blend St=storage | buffer: V=vertex):\n");
  for(unsigned i=0;i<sizeof fm/sizeof*fm;i++){ VkFormatProperties fp; gf(pd,fm[i].f,&fp); VkFormatFeatureFlags o=fp.optimalTilingFeatures;
    printf(" %s:%s%s%s%s%s%s%s",fm[i].n,(o&1)?"S":"",(o&0x1000)?"F":"",(o&0x80)?"C":"",(o&0x200)?"D":"",(o&0x100)?"B":"",(o&2)?"St":"",(fp.bufferFeatures&0x40)?" V":""); if(!o&&!(fp.bufferFeatures&0x40)) printf("-"); }
  printf("\n"); return 0;}
