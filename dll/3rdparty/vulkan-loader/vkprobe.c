/* vkprobe: load vulkan-1.dll (Khronos loader), enumerate instance version + physical devices.
 * Writes everything to the file given as argv[1] (default C:\harness\out\vk.txt) because serial is unreliable.
 * Built with system mingw: x86_64-w64-mingw32-gcc -I<vulkan headers> vkprobe.c -o vkprobe.exe */
#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

static FILE *out;
#define P(...) do { fprintf(out, __VA_ARGS__); fflush(out); } while (0)

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "C:\\harness\\out\\vk.txt";
    out = fopen(path, "w");
    if (!out) return 90;
    _putenv("VK_LOADER_DEBUG=error,warn,info,driver");
    char dbg[MAX_PATH]; lstrcpyA(dbg, path); lstrcatA(dbg, ".loaderdbg");
    freopen(dbg, "w", stderr);

    HMODULE h = LoadLibraryA("vulkan-1.dll");
    if (!h) { P("FAIL LoadLibrary(vulkan-1.dll) err=%lu\n", GetLastError()); return 1; }
    char mp[MAX_PATH]; GetModuleFileNameA(h, mp, sizeof mp);
    P("vulkan-1.dll loaded from %s\n", mp);

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(h, "vkGetInstanceProcAddr");
    PFN_vkEnumerateInstanceVersion eiv = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!gipa || !ci) { P("FAIL missing entry points\n"); return 2; }
    uint32_t iv = 0; if (eiv) eiv(&iv);
    P("loader instance version: %u.%u.%u\n", VK_VERSION_MAJOR(iv), VK_VERSION_MINOR(iv), VK_VERSION_PATCH(iv));

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkprobe", 1, "none", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    VkInstance inst;
    VkResult r = ci(&ici, NULL, &inst);
    P("vkCreateInstance = %d\n", r);
    if (r != VK_SUCCESS) return 3;
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0; r = epd(inst, &n, NULL);
    P("vkEnumeratePhysicalDevices = %d, count = %u\n", r, n);
    VkPhysicalDevice devs[8]; if (n > 8) n = 8;
    r = epd(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; gpp(devs[i], &p);
        P("GPU%u: deviceName=%s apiVersion=%u.%u.%u driverVersion=0x%x vendorID=0x%x deviceType=%d\n", i, p.deviceName,
          VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
          p.driverVersion, p.vendorID, p.deviceType);
    }
    P("RESULT: %s\n", n ? "PASS" : "FAIL(no devices)");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"); di(inst, NULL);
    return n ? 0 : 4;
}
