#pragma once

#include "Horo/Application/ProjectCodeQuery.h"
#include "Horo/Platform/NativeProjectReadFiles.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace Horo::Test::CodeQuery {
    inline bool Matches(const Error &error, const ErrorCodeDescriptor &descriptor) {
        return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
    }

    inline Application::CodeQueryContext Context() {
        return {"project-one", 3, {}, std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}};
    }

    class Directory final {
    public:
        Directory() {
            static std::atomic<unsigned> sequence{};
            root = std::filesystem::temp_directory_path() /
                   ("horo code query " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + " " +
                    std::to_string(++sequence));
            std::filesystem::create_directory(root);
            root = std::filesystem::canonical(root);
        }

        ~Directory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        Directory(const Directory &) = delete;
        Directory &operator=(const Directory &) = delete;

        void Write(const std::filesystem::path &path, const std::string_view text) const {
            std::filesystem::create_directories((root / path).parent_path());
            std::ofstream output{root / path, std::ios::binary};
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(output.good());
        }

        std::shared_ptr<const Platform::IProjectReadFiles> Files() const {
            auto result = Platform::CreateNativeProjectReadFiles(root, "project-one", 3);
#ifdef _WIN32
            if (result.HasError() && Matches(result.ErrorValue(), Platform::ProjectReadErrors::Unavailable))
                SKIP("This Windows volume lacks independent identity-based directory cursor support.");
#endif
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        std::filesystem::path root;
    };

    inline Application::CodeQueryProviders Text(std::shared_ptr<const std::string> bytes, std::string revision = "text:1") {
        Application::CodeQueryProviders providers;
        providers.existingText = [bytes = std::move(bytes), revision = std::move(revision)](const std::string_view path,
                                                                                            const Application::CodeQueryContext &context) {
            return Result<std::optional<Application::CodeQueryTextSnapshot>>::Success(
                Application::CodeQueryTextSnapshot{context.projectIdentity, context.projectGeneration, std::string{path}, revision, bytes});
        };
        return providers;
    }

    inline std::shared_ptr<Application::ProjectCodeQuery> Service(Application::CodeQueryProviders providers = {},
                                                                  Application::CodeQueryLimits limits = {}) {
        auto result = Application::ProjectCodeQuery::Create({}, "project-one", 3, std::move(providers), limits);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace Horo::Test::CodeQuery
