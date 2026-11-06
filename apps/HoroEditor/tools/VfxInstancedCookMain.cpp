/** @file VfxInstancedCookMain.cpp @brief Explicit authenticated offline VFX shader producer. */
#include "VfxInstancedCook.h"

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>

namespace {
    using Horo::Editor::Offline::VfxInstancedCookBackend;

    /** @brief Parses only the two supported explicit output routes. */
    std::optional<VfxInstancedCookBackend> Backend(std::string_view name) {
        if (name == "metal")
            return VfxInstancedCookBackend::Metal;
        if (name == "vulkan")
            return VfxInstancedCookBackend::Vulkan;
        return std::nullopt;
    }

    /** @brief Reports the fixed absolute-input protocol without discovering tools or runtime backends. */
    void Usage() {
        std::cerr << "Usage: HoroVfxInstancedShaderCook metal|vulkan ABS-repository-root ABS-new-package-root ABS-existing-scratch "
                     "ABS-catalog ABS-detached-signature ABS-host-trust ABS-dxc ABS-spirv-val ABS-spirv-cross [ABS-xcrun]\n";
    }

    /** @brief Prints a bounded preserved cause chain and diagnostics only on failure. */
    void Report(const Horo::Error &error) {
        const auto *cause = &error;
        for (std::size_t depth = 0; cause && depth < 8; ++depth, cause = cause->cause.Get()) {
            std::cerr << cause->code.Value() << ": " << std::string_view{cause->message}.substr(0, 2048) << '\n';
            std::size_t count{};
            for (const auto &diagnostic : cause->diagnostics) {
                if (count++ == 64)
                    break;
                std::cerr << std::string_view{diagnostic.message}.substr(0, 2048) << '\n';
            }
        }
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        Usage();
        return 2;
    }
    const auto backend = Backend(argv[1]);
    if (!backend || argc != (*backend == VfxInstancedCookBackend::Metal ? 12 : 11)) {
        Usage();
        return 2;
    }
    try {
        std::array<std::filesystem::path, 4> executables;
        const auto count = static_cast<std::size_t>(argc - 8);
        for (std::size_t index = 0; index < count; ++index)
            executables[index] = argv[index + 8];
        const auto result = Horo::Editor::Offline::CookVfxInstancedFamily(*backend, argv[2], argv[3], {argv[4], argv[5], argv[6], argv[7]},
                                                                          std::span{executables}.first(count));
        if (result.HasError()) {
            Report(result.ErrorValue());
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << "VFX instance cook failed before publication: " << std::string_view{error.what()}.substr(0, 2048) << '\n';
        return 1;
    }
    return 0;
}
