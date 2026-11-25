#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
typedef struct hw_module_methods_t { int (*open)(void*,const char*,void**); } M;
typedef struct { uint32_t tag; uint16_t a,b; const char*id,*name,*author; M*methods; } HM;
typedef struct { uint32_t tag,ver; void*mod; uint64_t r[12]; void*close; void*eiep; void*ci; PFN_vkGetInstanceProcAddr gipa; } DEV;
int main(int c,char**v){
  void*h=dlopen(v[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  HM*hm=dlsym(h,"HMI"); DEV*d=0; int r=hm->methods->open(hm,"vk0",(void**)&d); printf("open=%d name=%s\n",r,hm->name); if(r)return 1;
  PFN_vkGetInstanceProcAddr g=d->gipa;
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&ai; VkInstance in;
  /* HAL espera dispatch do loader: criamos via driver direto */
  r=((PFN_vkCreateInstance)g(NULL,"vkCreateInstance"))(&ci,NULL,&in); printf("vkCreateInstance=%d\n",r); if(r)return 1;
  uint32_t n=1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)g(in,"vkEnumeratePhysicalDevices"))(in,&n,&pd);
  VkPhysicalDeviceProperties p; ((PFN_vkGetPhysicalDeviceProperties)g(in,"vkGetPhysicalDeviceProperties"))(pd,&p);
  printf("GPU %s API %u.%u.%u\n",p.deviceName,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion));
  printf("csm interceptado: %s | dsl interceptado: %s\n",
    dlsym(h,"HMI")&&g(in,"vkCreateShaderModule")!=0?"?":"-", "?");
  return 0;}
