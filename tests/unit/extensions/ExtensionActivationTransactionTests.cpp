#include "ExtensionActivationTransaction.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        struct CleanupRecord {
            std::vector<int> *order{};
            int module{};
            bool throwOnUnload{};
        };

        class TrackingImporter final : public Assets::IAssetImporter {
        public:
            TrackingImporter(std::vector<int> &order, const int identity) : order_(&order), identity_(identity) {}

            ~TrackingImporter() override {
                order_->push_back(identity_);
            }

            [[nodiscard]] Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &,
                                                                     const CancellationToken &) const override {
                return Result<Assets::PreparedAssetImport>::Failure(
                    MakeError(ExtensionErrors::InvocationFailed, "The rollback fixture is not callable."));
            }

        private:
            std::vector<int> *order_;
            int identity_;
        };

        [[nodiscard]] std::shared_ptr<ExtensionModuleLifetime> Lifetime(CleanupRecord &record) {
            auto lifetime = std::make_shared<ExtensionModuleLifetime>();
            lifetime->moduleId = "com.example.module." + std::to_string(record.module);
            lifetime->moduleApi.moduleContext = &record;
            lifetime->unload = [](HoroExtensionModuleApi *module) {
                auto &cleanup = *static_cast<CleanupRecord *>(module->moduleContext);
                cleanup.order->push_back(-cleanup.module);
                if (cleanup.throwOnUnload)
                    throw std::runtime_error{"fixture unload failure"};
            };
            lifetime->loaded = true;
            return lifetime;
        }

        [[nodiscard]] Assets::AssetImporterContribution Contribution(std::vector<int> &order, const int identity) {
            return {.contributionId = "com.example.contribution." + std::to_string(identity),
                    .packageId = "com.example.package",
                    .moduleId = "com.example.module." + std::to_string(identity),
                    .moduleVersion = "1.0.0",
                    .version = "1.0.0",
                    .strategy = std::make_shared<TrackingImporter>(order, identity)};
        }
    }  // namespace

    TEST_CASE("Activation rollback destroys contributions then unloads modules in deterministic reverse order",
              "[Extensions][Registration][Rollback]") {
        std::vector<int> order;
        CleanupRecord first{.order = &order, .module = 1};
        CleanupRecord second{.order = &order, .module = 2};
        ExtensionActivationTransaction transaction;
        transaction.Stage(Lifetime(first), {Contribution(order, 1)});
        transaction.Stage(Lifetime(second), {Contribution(order, 2)});

        const Error rolledBack = transaction.Rollback(MakeError(ExtensionErrors::LoadFailed, "fixture activation failure"));

        CHECK(rolledBack.code.Value() == ExtensionErrors::LoadFailed.code.Value());
        CHECK(rolledBack.diagnostics.empty());
        CHECK(order == std::vector<int>{2, 1, -2, -1});
    }

    TEST_CASE("Activation rollback preserves the primary failure and reports every unload failure",
              "[Extensions][Registration][Rollback]") {
        std::vector<int> order;
        CleanupRecord first{.order = &order, .module = 1, .throwOnUnload = true};
        CleanupRecord second{.order = &order, .module = 2, .throwOnUnload = true};
        ExtensionActivationTransaction transaction;
        transaction.Stage(Lifetime(first), {});
        transaction.Stage(Lifetime(second), {});

        const Error rolledBack = transaction.Rollback(MakeError(ExtensionErrors::ContributionRejected, "fixture commit failure"));

        CHECK(rolledBack.code.Value() == ExtensionErrors::ContributionRejected.code.Value());
        REQUIRE(rolledBack.diagnostics.size() == 2);
        CHECK(rolledBack.diagnostics[0].code.Value() == "extension.activation.rollback_cleanup_failed");
        CHECK(rolledBack.diagnostics[1].code.Value() == "extension.activation.rollback_cleanup_failed");
        CHECK(order == std::vector<int>{-2, -1});
    }

    TEST_CASE("Unreleased activation transaction invokes the reverse-order rollback safety net", "[Extensions][Registration][Rollback]") {
        std::vector<int> order;
        CleanupRecord first{.order = &order, .module = 1};
        CleanupRecord second{.order = &order, .module = 2, .throwOnUnload = true};
        {
            ExtensionActivationTransaction transaction;
            transaction.Stage(Lifetime(first), {Contribution(order, 1)});
            transaction.Stage(Lifetime(second), {Contribution(order, 2)});
        }

        CHECK(order == std::vector<int>{2, 1, -2, -1});
    }

    TEST_CASE("Module lifetime reports every contained unload exception category", "[Extensions][Registration][Rollback]") {
        const std::array<HoroExtensionUnloadFunc, 4> unloaders{
            +[](HoroExtensionModuleApi *) {
            throw std::logic_error{"fixture logic failure"};
        },
            +[](HoroExtensionModuleApi *) {
            throw std::bad_alloc{};
        },
            +[](HoroExtensionModuleApi *) {
            throw std::exception{};
        },
            +[](HoroExtensionModuleApi *) {
            throw 42;
        },
        };
        int context{};
        for (const HoroExtensionUnloadFunc unload : unloaders) {
            ExtensionModuleLifetime lifetime;
            lifetime.moduleApi.moduleContext = &context;
            lifetime.unload = unload;
            lifetime.loaded = true;

            CHECK_FALSE(lifetime.UnloadNow());
            CHECK_FALSE(lifetime.UnloadNow());
        }
    }
}  // namespace Horo::Extensions::Tests
