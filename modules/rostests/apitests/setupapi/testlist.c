
#define STANDALONE
#include <apitest.h>

extern void func_CM_Is_Dock_Station_Present(void);
extern void func_devclass(void);
extern void func_RootDeviceRemove(void);
extern void func_SetupDiGetActualModelsSection(void);
extern void func_SetupDiGetDeviceProperty(void);
extern void func_SetupDiInstallClassExA(void);
extern void func_SetupInstallServicesFromInfSectionEx(void);

const struct test winetest_testlist[] =
{
    { "CM_Is_Dock_Station_Present", func_CM_Is_Dock_Station_Present },
    { "devclass", func_devclass },
    { "RootDeviceRemove", func_RootDeviceRemove },
    { "SetupDiGetActualModelsSection", func_SetupDiGetActualModelsSection },
    { "SetupDiGetDeviceProperty", func_SetupDiGetDeviceProperty },
    { "SetupDiInstallClassExA", func_SetupDiInstallClassExA },
    { "SetupInstallServicesFromInfSectionEx", func_SetupInstallServicesFromInfSectionEx },
    { 0, 0 }
};
