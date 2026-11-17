/** @file ExposureGpuCookMain.cpp @brief Explicit offline producer, never invoked by the runtime renderer. */
#include "ExposureGpuCook.h"

#include <exception>
#include <iostream>
#include <string_view>

namespace {
    /** @brief Prints bounded preserved tool failures without altering trust, source or retry policy. */
    void Report(const Horo::Error &error) {
        const auto *cause = &error;
        for (std::size_t depth = 0; cause && depth < 8; ++depth, cause = cause->cause.Get()) {
            std::cerr << cause->code.Value() << ": " << std::string_view{cause->message}.substr(0, 2048) << '\n';
            std::size_t count = 0;
            for (const auto &diagnostic : cause->diagnostics) {
                if (count++ == 64)
                    break;
                std::cerr << std::string_view{diagnostic.message}.substr(0, 2048) << '\n';
            }
        }
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc != 11) {
        std::cerr << "Usage: HoroExposureGpuShaderCook ABS-repository-root ABS-new-package-root ABS-existing-scratch "
                     "ABS-catalog ABS-detached-signature ABS-host-trust ABS-dxc ABS-spirv-val ABS-spirv-cross ABS-xcrun\n";
        return 2;
    }
    try {
        const auto result =
            Horo::Editor::Offline::CookExposureGpuFamily(argv[1], argv[2],
                                                         {argv[3], argv[4], argv[5], argv[6], {argv[7], argv[8], argv[9], argv[10]}});
        if (result.HasError()) {
            Report(result.ErrorValue());
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << "Exposure cook failed before publication: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
