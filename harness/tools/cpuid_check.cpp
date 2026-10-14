// Quick CPU feature probe: prints which matvec dispatch path will be used.
#include <cstdio>
#include <cstring>

static unsigned cpuid_flag(int leaf, int subleaf, int reg, int bit) {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(leaf), "c"(subleaf));
    unsigned v = (reg == 0 ? eax : reg == 1 ? ebx : reg == 2 ? ecx : edx);
    return (v >> bit) & 1u;
}

static void cpuid_str(int leaf, char* out) {
    unsigned r[4];
    __asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]),
                                       "=d"(r[3])
                         : "a"(leaf));
    const unsigned* w = r;
    for (int i = 0; i < 16; i++) out[i] = char(w[i]);
    out[16] = 0;
}

int main() {
    unsigned max_leaf = 0;
    __asm__ __volatile__("cpuid" : "=a"(max_leaf) : "a"(0));
    bool avx2 = max_leaf >= 7 && cpuid_flag(7, 0, 1, 5);
    bool fma = cpuid_flag(1, 0, 2, 12);
    bool avx = cpuid_flag(1, 0, 2, 28);
    bool hv = cpuid_flag(1, 0, 2, 31);  // hypervisor bit
    char brand[64] = {0};
    if (max_leaf >= 0x80000002) {
        // max extended leaf
        unsigned ext_max = 0;
        __asm__ __volatile__("cpuid" : "=a"(ext_max) : "a"(0x80000000));
        if (ext_max >= 0x80000004) {
            char part[17];
            cpuid_str(0x80000002, part);
            std::strcat(brand, part);
            cpuid_str(0x80000003, part);
            std::strcat(brand, part);
            cpuid_str(0x80000004, part);
            std::strcat(brand, part);
        }
    }
    printf("max_cpuid_leaf=%u avx=%d avx2=%d fma=%d hypervisor=%d\n", max_leaf,
           avx, avx2, fma, hv);
    printf("cpu: %s\n", brand[0] ? brand : "(unknown)");
    printf("dispatch: %s\n", (avx && fma) ? "AVX+FMA (256-bit)" : "SSE2");
    return 0;
}
