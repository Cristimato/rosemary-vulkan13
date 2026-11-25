#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(){
  void*h=dlopen("/system/lib64/libvulkan.so",RTLD_NOW); if(!h){printf("dlopen: %s\n",dlerror());return 1;}
  PFN_vkGetInstanceProcAddr gipa=dlsym(h,"vkGetInstanceProcAddr");
  PFN_vkEnumerateInstanceVersion eiv=(void*)gipa(NULL,"vkEnumerateInstanceVersion");
  uint32_t iv=0; if(eiv)eiv(&iv);
  printf("loader instance version: %u.%u.%u\n",VK_VERSION_MAJOR(iv),VK_VERSION_MINOR(iv),VK_VERSION_PATCH(iv));
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&ai;
  VkInstance in; VkResult r=((PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance"))(&ci,NULL,&in);
  if(r){printf("vkCreateInstance=%d\n",r);return 1;}
  uint32_t n=1; VkPhysicalDevice pd;
  ((PFN_vkEnumeratePhysicalDevices)gipa(in,"vkEnumeratePhysicalDevices"))(in,&n,&pd);
  VkPhysicalDeviceVulkan13Features f13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&f13};
  VkPhysicalDeviceProperties p; ((PFN_vkGetPhysicalDeviceProperties)gipa(in,"vkGetPhysicalDeviceProperties"))(pd,&p);
  ((PFN_vkGetPhysicalDeviceFeatures2)gipa(in,"vkGetPhysicalDeviceFeatures2"))(pd,&f2);
  printf("GPU: %s | API %u.%u.%u | driver 0x%x\n",p.deviceName,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion),p.driverVersion);
  printf("1.3 feats: dynamicRendering=%u sync2=%u maintenance4=%u\n",f13.dynamicRendering,f13.synchronization2,f13.maintenance4);
  float q=1; VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount=1; qi.pQueuePriorities=&q;
  VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
  VkDevice d; r=((PFN_vkCreateDevice)gipa(in,"vkCreateDevice"))(pd,&di,NULL,&d); printf("vkCreateDevice=%d\n",r);
  FILE*m=fopen("/proc/self/maps","r"); char l[512]; char last[512]="";
  while(fgets(l,512,m)) if((strstr(l,"vulkan")||strstr(l,"GLES_ma"))){char*s=strchr(l,'/'); if(s&&strcmp(s,last)){printf("  lib: %s",s);strcpy(last,s);}}
  return 0;}
