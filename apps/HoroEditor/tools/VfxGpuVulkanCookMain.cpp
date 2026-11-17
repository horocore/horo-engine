/** @file VfxGpuVulkanCookMain.cpp @brief Explicit authenticated offline producer; the editor never invokes this executable. */
#include "VfxGpuVulkanCook.h"

#include <iostream>
#include <string_view>

namespace {
    /** @brief Presents bounded typed causes and actual tool diagnostics without changing admission or retry policy. */
    void ReportCookFailure(const Horo::Error &error) {
        constexpr std::size_t MaximumCauseDepth = 8;
        constexpr std::size_t MaximumDiagnostics = 64;
        constexpr std::size_t MaximumMessageBytes = 2048;
        const auto *cause = &error;
        for (std::size_t depth = 0; cause != nullptr && depth < MaximumCauseDepth; ++depth, cause = cause->cause.Get()) {
            std::cerr << cause->code.Value() << ": " << std::string_view{cause->message}.substr(0, MaximumMessageBytes) << '\n';
            std::size_t count = 0;
            for (const auto &diagnostic : cause->diagnostics) {
                if (count++ == MaximumDiagnostics)
                    break;
                std::cerr << std::string_view{diagnostic.message}.substr(0, MaximumMessageBytes) << '\n';
            }
        }
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc != 10) {
        std::cerr << "Usage: HoroVfxGpuVulkanShaderCook ABS-repository-root ABS-new-package-root ABS-existing-scratch "
                     "ABS-catalog ABS-detached-signature ABS-host-trust ABS-dxc ABS-spirv-val ABS-spirv-cross\n";
        return 2;
    }
    try {
        const auto result =
            Horo::Editor::Offline::CookVfxGpuVulkanFamily(argv[1], argv[2],
                                                          {argv[3], argv[4], argv[5], argv[6], {argv[7], argv[8], argv[9]}});
        if (result.HasError()) {
            ReportCookFailure(result.ErrorValue());
            std::cerr << "Offline package was not published; inspect the typed cause and tool diagnostics above.\n";
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << "VFX GPU cook failed before package publication: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
