#pragma once

/** @file PlatformPackageFixture.h @brief Test-only C observations of provider package resource ownership. */
#include <stdint.h>

enum {
    HORO_PACKAGE_PROVIDER_DELAY = 1,
    HORO_PACKAGE_PROVIDER_BUSY = 2,
    HORO_PACKAGE_PROVIDER_SDK_UNAVAILABLE = 4,
    HORO_PACKAGE_PROVIDER_MALFORMED = 8,
    HORO_PACKAGE_PROVIDER_PARTIAL_CREATE = 16,
    HORO_PACKAGE_PROVIDER_UNAPPROVED = 32,
    HORO_PACKAGE_PROVIDER_WRONG_PROFILE = 64,
    HORO_PACKAGE_PROVIDER_INCOMPATIBLE_ABI = 128,
    HORO_PACKAGE_PROVIDER_CONTRACT_V2 = 256,
    HORO_PACKAGE_PROVIDER_CLOSE_ADMISSION_FAILED = 512,
    HORO_PACKAGE_PROVIDER_CLOSE_INGRESS_FAILED = 1024,
    HORO_PACKAGE_PROVIDER_CANCEL_REJECTED = 2048
};

typedef struct HoroPackageProviderAudit {
    uint32_t created;
    uint32_t destroyed;
    uint32_t submitted;
    uint32_t cancelled;
    uint32_t servicesStopped;
    uint32_t sessionStopped;
    uint32_t sinkRetained;
    uint32_t unloaded;
} HoroPackageProviderAudit;

typedef void (*HoroPackageProviderConfigure)(uint32_t flags);
typedef void (*HoroPackageProviderInspect)(HoroPackageProviderAudit *audit);
