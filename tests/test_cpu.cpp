// Check if CPU supports AVX512 and AVX2 instructions

#include <iostream>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

int cpu_feature_mask() {
  int info[4];
#if defined(_MSC_VER)
  __cpuidex(info, 7, 0);
#else
  __cpuid_count(7, 0, info[0], info[1], info[2], info[3]);
#endif
  int support_avx512f = (info[1] & (1 << 16)) != 0;   // AVX512F bit
  int support_avx512bw = (info[1] & (1 << 30)) != 0;  // AVX512BW bit
  int support_avx512dq = (info[1] & (1 << 17)) != 0;  // AVX512DQ bit
  int support_avx2 = (info[1] & (1 << 5)) != 0;       // AVX2 bit

  int mask = 0;
  if (support_avx512f)
    mask |= 0x1;
  if (support_avx512bw)
    mask |= 0x2;
  if (support_avx512dq)
    mask |= 0x4;
  if (support_avx2)
    mask |= 0x8;
  return mask;
}

int main() {
  return cpu_feature_mask();
}
