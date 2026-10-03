#pragma once

/** @file SaveFilesystemTestSupport.h
 * @brief Shared isolated filesystem fixtures for root and locking qualification. */

#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveFilesystemStorage.h"
#include "Horo/Runtime/Save/SaveRootResolver.h"
#include "SaveTestUtils.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Horo::Runtime::SaveFilesystemTest {
    constexpr std::string_view kProductUuid = "00112233-4455-6677-8899-aabbccddeeff";

    class FixedEnvironment final : public ProcessService {
    public:
        [[nodiscard]] ProcessMetadata CurrentProcess() const override {
            return {.id = 1, .executableName = "save-root-test"};
        }

        [[nodiscard]] std::optional<std::string> EnvironmentValue(const std::string_view name) const override {
            ++environmentLookups;
            if (name == "LOCALAPPDATA")
                return localApplicationData;
            if (name == "HOME")
                return home;
            if (name == "XDG_STATE_HOME")
                return xdgStateHome;
            return std::nullopt;
        }

        std::optional<std::string> localApplicationData;
        std::optional<std::string> home;
        std::optional<std::string> xdgStateHome;
        mutable std::size_t environmentLookups{0};
    };

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic_uint64_t next{0};
            const auto base = std::filesystem::temp_directory_path();
            for (std::uint64_t attempt = 0; attempt < 256; ++attempt) {
                path_ = base / ("horo-save-root-" + std::to_string(next.fetch_add(1)));
                std::error_code error;
                if (std::filesystem::create_directory(path_, error))
                    return;
            }
            FAIL("Could not create a unique save-root test directory");
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::permissions(path_, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    [[nodiscard]] inline ProductStorageId Product() {
        auto parsed = ProductStorageId::Parse(kProductUuid);
        REQUIRE(parsed.HasValue());
        return parsed.Value();
    }

    [[nodiscard]] inline ProductSaveRoot Resolve(const SaveRootResolutionRequest &request, const FixedEnvironment &environment) {
        auto resolved = ResolveProductSaveRoot(request, environment);
        REQUIRE(resolved.HasValue());
        return resolved.Value();
    }

    struct StorageFixture final {
        TemporaryDirectory temporary;
        FixedEnvironment environment;
        ProductSaveRoot root =
            Resolve({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path() / "approved"},
                    environment);
        SaveNamespaceId name{.product = Product(),
                             .environment = Test::Id<EnvironmentStorageId>(2),
                             .owner = ServerWorldOwner{Test::Id<ServerStorageOwnerId>(3)}};

        [[nodiscard]] std::filesystem::path Slots() const {
            return root.CanonicalPath() / name.environment.ToString() / "server" / std::get<ServerWorldOwner>(name.owner).owner.ToString() /
                   "slots";
        }
    };

}  // namespace Horo::Runtime::SaveFilesystemTest
