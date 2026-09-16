#include "Horo/Extensions/AssetCookerRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace Horo::Extensions::Tests {
    namespace {
        using CookBody = std::function<Result<void>(const AssetCookerInput &, AssetCookerOutputSink &, const CancellationToken &)>;

        class TestCooker final : public IAssetCooker {
        public:
            explicit TestCooker(CookBody body) : body_(std::move(body)) {}

            Result<void> Cook(const AssetCookerInput &input, AssetCookerOutputSink &output,
                              const CancellationToken &cancellation) const override {
                ++calls_;
                return body_(input, output, cancellation);
            }

            [[nodiscard]] std::size_t Calls() const noexcept {
                return calls_;
            }

        private:
            CookBody body_;
            mutable std::size_t calls_{};
        };

        class TestCookerException final : public std::runtime_error {
        public:
            TestCookerException() : std::runtime_error("provider exception") {}
        };

        [[nodiscard]] Assets::AssetId Asset(std::string_view value = "12345678-1234-4234-8234-123456789abc") {
            auto parsed = Assets::AssetId::Parse(value);
            REQUIRE(parsed.HasValue());
            return std::move(parsed).Value();
        }

        [[nodiscard]] Assets::AssetTypeId Type(std::string_view value = "core.mesh") {
            auto parsed = Assets::AssetTypeId::Parse(value);
            REQUIRE(parsed.HasValue());
            return std::move(parsed).Value();
        }

        [[nodiscard]] AssetCookTargetId Target(std::string_view value = "headless-null") {
            auto parsed = AssetCookTargetId::Parse(value);
            REQUIRE(parsed.HasValue());
            return std::move(parsed).Value();
        }

        [[nodiscard]] AssetCookerDescriptor Descriptor(std::string id = "com.example.mesh-cooker", std::uint64_t generation = 7U) {
            return {
                .cookerId = {std::move(id)},
                .providerId = "com.example.assets",
                .providerGeneration = generation,
                .assetType = Type(),
                .targets = {Target()},
                .cookerVersion = "2.1.0",
                .artifactFormatVersion = 3U,
            };
        }

        [[nodiscard]] AssetCookerRequest Request() {
            static const std::array<std::uint8_t, 4> source{1U, 2U, 3U, 4U};
            const auto sourceDigest = ComputeSha256(std::as_bytes(std::span{source}));
            const std::array<std::byte, 2> metadata{std::byte{9U}, std::byte{8U}};
            const std::array<std::byte, 1> settings{std::byte{7U}};
            return {
                .input =
                    {
                        .assetId = Asset(),
                        .assetType = Type(),
                        .target = Target(),
                        .sourceDigest = sourceDigest,
                        .metadataDigest = ComputeSha256(metadata),
                        .metadataSchemaVersion = 4U,
                        .settingsDigest = ComputeSha256(settings),
                        .settingsSchemaVersion = 5U,
                        .sourceBytes = source,
                    },
            };
        }

        template <typename T> void RequireError(const Result<T> &result, const std::string &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == "horo.extensions");
            CHECK(result.ErrorValue().code.Value() == code);
        }

        [[nodiscard]] std::shared_ptr<TestCooker> SuccessfulCooker() {
            return std::make_shared<TestCooker>(
                [](const AssetCookerInput &input, AssetCookerOutputSink &output, const CancellationToken &) {
                CHECK(input.target.Value() == "headless-null");
                const std::array<std::uint8_t, 3> payload{6U, 5U, 4U};
                if (auto written = output.WritePayload(payload); written.HasError())
                    return written;
                if (auto dependency = output.AddDependency(Asset("ffffffff-ffff-4fff-8fff-ffffffffffff")); dependency.HasError())
                    return dependency;
                return output.AddDiagnostic({DiagnosticCode{"asset.cook.optimized"}, DiagnosticSeverity::Note, "mesh optimized"});
            });
        }
    }  // namespace

    TEST_CASE("Asset cooker registry stages a complete attributed result and host-computed key",
              "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        const auto cooker = SuccessfulCooker();
        auto registration = registry.Register(Descriptor(), cooker);
        REQUIRE(registration.HasValue());
        const AssetCookerRequest request = Request();

        const auto cooked = registry.Cook(request, {});
        REQUIRE(cooked.HasValue());
        CHECK(cooker->Calls() == 1U);
        CHECK(cooked.Value().provider.cookerId.value == "com.example.mesh-cooker");
        CHECK(cooked.Value().provider.providerGeneration == 7U);
        CHECK(cooked.Value().payload == std::vector<std::uint8_t>{6U, 5U, 4U});
        REQUIRE(cooked.Value().dependencies.size() == 1U);
        REQUIRE(cooked.Value().diagnostics.size() == 1U);
        CHECK(cooked.Value().diagnostics.front().code.Value() == "asset.cook.optimized");

        const auto expected = Assets::BuildAssetCookCacheKey({
            .assetId = request.input.assetId,
            .assetType = request.input.assetType,
            .sourceDigest = request.input.sourceDigest,
            .metadataDigest = request.input.metadataDigest,
            .metadataSchemaVersion = request.input.metadataSchemaVersion,
            .settingsDigest = request.input.settingsDigest,
            .settingsSchemaVersion = request.input.settingsSchemaVersion,
            .cookerContributionId = "com.example.mesh-cooker",
            .cookerVersion = "2.1.0",
            .target = request.input.target,
            .artifactFormatVersion = 3U,
        });
        CHECK(cooked.Value().cacheKey == expected);
    }

    TEST_CASE("Asset cooker selection rejects ambiguity and honors one exact project-policy choice",
              "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        auto firstCooker = SuccessfulCooker();
        auto secondCooker = SuccessfulCooker();
        auto first = registry.Register(Descriptor("com.example.mesh-a"), firstCooker);
        auto second = registry.Register(Descriptor("com.example.mesh-b", 8U), secondCooker);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());

        auto request = Request();
        RequireError(registry.Cook(request, {}), "asset_cooker_ambiguous");
        request.selectedCooker = AssetCookerId{"com.example.mesh-b"};
        const auto selected = registry.Cook(request, {});
        REQUIRE(selected.HasValue());
        CHECK(selected.Value().provider.cookerId.value == "com.example.mesh-b");
        CHECK(firstCooker->Calls() == 0U);
        CHECK(secondCooker->Calls() == 1U);
    }

    TEST_CASE("Asset cooker provider failure remains unpublished and preserves its cause", "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        const auto cooker =
            std::make_shared<TestCooker>([](const AssetCookerInput &, AssetCookerOutputSink &output, const CancellationToken &) {
            const std::array<std::uint8_t, 1> partial{1U};
            REQUIRE(output.WritePayload(partial).HasValue());
            return Result<void>::Failure(MakeError(ExtensionErrors::InvocationFailed, "provider rejected input"));
        });
        auto registration = registry.Register(Descriptor(), cooker);
        REQUIRE(registration.HasValue());
        const auto failed = registry.Cook(Request(), {});
        RequireError(failed, "asset_cooker_invocation_failed");
        REQUIRE(failed.ErrorValue().cause.Get() != nullptr);
        CHECK(failed.ErrorValue().cause.Get()->code.Value() == "invocation_failed");
    }

    TEST_CASE("Asset cooker cancellation after staging discards the result", "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        CancellationSource cancellation;
        const auto cooker = std::make_shared<TestCooker>(
            [&cancellation](const AssetCookerInput &, AssetCookerOutputSink &output, const CancellationToken &) {
            const std::array<std::uint8_t, 1> partial{1U};
            REQUIRE(output.WritePayload(partial).HasValue());
            cancellation.RequestCancellation();
            return Result<void>::Success();
        });
        auto registration = registry.Register(Descriptor(), cooker);
        REQUIRE(registration.HasValue());
        RequireError(registry.Cook(Request(), cancellation.Token()), "asset_cook_cancelled");
    }

    TEST_CASE("Asset cooker success without writing a payload is rejected", "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        const auto cooker = std::make_shared<TestCooker>([](const AssetCookerInput &, AssetCookerOutputSink &, const CancellationToken &) {
            return Result<void>::Success();
        });
        auto registration = registry.Register(Descriptor(), cooker);
        REQUIRE(registration.HasValue());
        const auto failed = registry.Cook(Request(), {});
        RequireError(failed, "asset_cooker_invocation_failed");
        REQUIRE(failed.ErrorValue().cause.Get() != nullptr);
        CHECK(failed.ErrorValue().cause.Get()->code.Value() == "asset_cooker_output_invalid");
    }

    TEST_CASE("Asset cooker accepts an explicitly written empty payload", "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        const auto cooker =
            std::make_shared<TestCooker>([](const AssetCookerInput &, AssetCookerOutputSink &output, const CancellationToken &) {
            return output.WritePayload(std::span<const std::uint8_t>{});
        });
        auto registration = registry.Register(Descriptor(), cooker);
        REQUIRE(registration.HasValue());
        const auto cooked = registry.Cook(Request(), {});
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().payload.empty());
    }

    TEST_CASE("Asset cooker sink enforces payload dependency and diagnostic bounds", "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        const auto cooker =
            std::make_shared<TestCooker>([](const AssetCookerInput &, AssetCookerOutputSink &output, const CancellationToken &) {
            const std::array<std::uint8_t, 2> payload{1U, 2U};
            REQUIRE(output.WritePayload(payload).HasValue());
            CHECK(output.WritePayload(payload).HasError());
            return Result<void>::Success();
        });
        auto registration = registry.Register(Descriptor(), cooker);
        REQUIRE(registration.HasValue());
        RequireError(registry.Cook(Request(), {}), "asset_cooker_invocation_failed");

        AssetCookerRegistry boundedRegistry;
        const auto bounded =
            std::make_shared<TestCooker>([](const AssetCookerInput &, AssetCookerOutputSink &output, const CancellationToken &) {
            const std::array<std::uint8_t, 2> payload{1U, 2U};
            if (auto written = output.WritePayload(payload); written.HasError())
                return written;
            return output.AddDiagnostic({DiagnosticCode{"asset.cook.note"}, DiagnosticSeverity::Note, "bounded"});
        });
        auto boundedRegistration = boundedRegistry.Register(Descriptor(), bounded);
        REQUIRE(boundedRegistration.HasValue());
        auto request = Request();
        request.limits.maximumArtifactBytes = 1U;
        RequireError(boundedRegistry.Cook(request, {}), "asset_cooker_invocation_failed");

        request = Request();
        request.limits.maximumDiagnosticCodeBytes = 4U;
        RequireError(boundedRegistry.Cook(request, {}), "asset_cooker_invocation_failed");
    }

    TEST_CASE("Asset cooker registration owns discovery and shutdown closes admission", "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        auto registrationResult = registry.Register(Descriptor(), SuccessfulCooker());
        REQUIRE(registrationResult.HasValue());
        AssetCookerRegistration registration = std::move(registrationResult).Value();
        CHECK(registration.IsRegistered());
        RequireError(registry.Register(Descriptor(), SuccessfulCooker()), "asset_cooker_registry_duplicate");
        registration.Reset();
        CHECK_FALSE(registration.IsRegistered());
        RequireError(registry.Cook(Request(), {}), "asset_cooker_unavailable");

        auto replacement = registry.Register(Descriptor("com.example.mesh-replacement", 9U), SuccessfulCooker());
        REQUIRE(replacement.HasValue());
        AssetCookerRegistration ownedReplacement = std::move(replacement).Value();
        registry.BeginShutdown();
        CHECK(registry.IsShutdown());
        CHECK_FALSE(ownedReplacement.IsRegistered());
        RequireError(registry.Cook(Request(), {}), "asset_cooker_registry_shutdown");
        RequireError(registry.Register(Descriptor("com.example.after-shutdown"), SuccessfulCooker()), "asset_cooker_registry_shutdown");
    }

    TEST_CASE("Asset cooker registry validates descriptors requests and callback exceptions",
              "[unit][extensions][asset-cooker][headless]") {
        AssetCookerRegistry registry;
        auto malformed = Descriptor();
        malformed.targets.push_back(Target());
        RequireError(registry.Register(std::move(malformed), SuccessfulCooker()), "asset_cooker_registry_invalid");

        auto registration = registry.Register(Descriptor(), SuccessfulCooker());
        REQUIRE(registration.HasValue());
        auto request = Request();
        request.input.sourceDigest = {};
        RequireError(registry.Cook(request, {}), "asset_cooker_registry_invalid");

        AssetCookerRegistry throwingRegistry;
        auto throwing =
            std::make_shared<TestCooker>([](const AssetCookerInput &, AssetCookerOutputSink &, const CancellationToken &) -> Result<void> {
            throw TestCookerException{};
        });
        auto throwingRegistration = throwingRegistry.Register(Descriptor(), throwing);
        REQUIRE(throwingRegistration.HasValue());
        RequireError(throwingRegistry.Cook(Request(), {}), "asset_cooker_invocation_failed");
    }
}  // namespace Horo::Extensions::Tests
