#pragma once

/** @file MetalUiPaintFixture.h @brief Private offline shader qualification input; never a production host catalog. */
#include "Horo/Runtime/Render/UiPaintKernel.h"

#include <filesystem>

namespace Horo::Tests {
    /** @brief Packages actual Apple offline libraries through canonical artifact admission.
     * @param root Same-run data directory already verified by ui_paint_fixture.py.
     * @param format Exact target color format to realize.
     * @return Owned canonical kernel candidate, independently checked by native pipeline reflection.
     * @throws Catch assertion failure for absent, oversized, tampered or malformed fixture data.
     */
    [[nodiscard]] Render::CookedUiPaintKernel LoadMetalUiPaintFixture(const std::filesystem::path &root,
                                                                      Render::RenderTextureFormat format);
}  // namespace Horo::Tests
