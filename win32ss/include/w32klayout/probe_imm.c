/* Types whose layout w32klayout.h pins; compiled with the imm32 flags by regen.sh. */
#include "precomp.h"
#define T(x) x *volatile probe_##x; typedef struct { char c; x t; } probe_al_##x; probe_al_##x *volatile probe_alp_##x;
T(IMC) T(IMEUI) T(IMEWND) T(CLIENTIMC) T(IMEDPI) T(IMEINFOEX)
