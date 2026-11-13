#include <stdint.h>

#if defined(_WIN32)
#define HORO_LOADER_FIXTURE_EXPORT __declspec(dllexport)
#else
#define HORO_LOADER_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

/* No C++ initializers or callbacks: allocation faults belong to the host loader. */
HORO_LOADER_FIXTURE_EXPORT uint32_t Horo_Platform_DynamicLibrary_Allocation_Probe(void) {
    return UINT32_C(0x484F524F);
}
