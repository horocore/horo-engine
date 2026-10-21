#pragma once

/** @file SourceDocumentTestProject.h
 * @brief Isolated source-document filesystem fixture with RAII cleanup.
 */
#include "Horo/Editor/SourceDocumentService.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>

namespace Horo::TestSupport {
    /** @brief Owns an isolated on-disk source project for document-service regressions. */
    struct SourceProject final {
        std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                          ("horo-document-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        SourceProject() {
            std::filesystem::create_directory(directory);
        }

        ~SourceProject() {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        void Put(std::string_view bytes, const char *name = "code.cpp") const {
            std::ofstream stream(directory / name, std::ios::binary);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
        }

        Editor::DocumentIdentity Identity(const char *name = "code.cpp", std::uint64_t instance = 1) const {
            return {{Editor::DocumentKind::Source, Editor::SourceDocumentId::Parse(name).Value()},
                    Editor::DocumentInstanceId::Create(instance).Value()};
        }
    };

}  // namespace Horo::TestSupport
