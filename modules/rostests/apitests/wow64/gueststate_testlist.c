#define STANDALONE
/* The production DLL sources bring libwine's debug helpers as well. */
#define wine_dbgstr_an gueststate_dbgstr_an
#define wine_dbgstr_wn gueststate_dbgstr_wn
#include <apitest.h>
extern void func_GuestState(void);
const struct test winetest_testlist[] =
{
    { "GuestState", func_GuestState },
    { 0, 0 }
};
