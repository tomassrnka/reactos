
#define STANDALONE
#include <wine/test.h>

extern void func_icd_default(void);
extern void func_make_current(void);
extern void func_sw_extensions(void);
extern void func_sw_pixelformat(void);
extern void func_sw_render(void);

const struct test winetest_testlist[] =
{
    { "icd_default", func_icd_default },
    { "make_current", func_make_current },
    { "sw_extensions", func_sw_extensions },
    { "sw_pixelformat", func_sw_pixelformat },
    { "sw_render", func_sw_render },

    { 0, 0 }
};
