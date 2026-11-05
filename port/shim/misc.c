#include <string.h>
#include <math.h>
#include "port.h"
#undef bzero
void bzero(void *p, size_t n) { memset(p, 0, n); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
const unsigned int __libm_qnan_f = 0x7F810000u;
// RSP code is never run. taskman only checks these are non-null.
long long gspF3DEX2_fifoTextStart[1], gspF3DEX2_fifoTextEnd[1], gspF3DEX2_fifoDataStart[1], gspF3DEX2_fifoDataEnd[1];
long long n_aspMainTextStart[1], n_aspMainTextEnd[1], n_aspMainDataStart[1], n_aspMainDataEnd[1];
