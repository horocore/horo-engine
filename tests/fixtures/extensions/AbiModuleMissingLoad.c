#include "Horo/Extensions/ExtensionAbi.h"

/** @brief Export a non-entry symbol so the fixture remains a loadable native module. */
HORO_EXTENSION_EXPORT uint32_t horo_test_missing_load(void) {
    return 0;
}
