#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "OwnedTestDirectory.h"
#include "PerceptionSenseTestSupport.h"

#include <cstring>
#include <fstream>

namespace Horo::AI {
    namespace {
        /** @brief Reads the actual descriptor receipt emitted for the compiled native fixture. */
        Gameplay::GameModuleLoadExpectation Expectation() {
            std::ifstream file(HORO_TEST_AI_MODULE_REVISION_PATH);
            std::uint64_t revision{};
            file >> revision;
            REQUIRE(revision != 0);
            return {"game.ai.tests", Gameplay::CurrentGameplayBuildFingerprint(), revision};
        }

        /** @brief Loads a private real module image through the validating gameplay host. */
        std::unique_ptr<Gameplay::LoadedGameModule> Load(const std::filesystem::path &shadow) {
            auto loaded = Gameplay::GameModuleHost{}.LoadShadowCopy(HORO_TEST_AI_MODULE_PATH, shadow, Expectation());
            REQUIRE(loaded.HasValue());
            REQUIRE_FALSE(loaded.Value().diagnostic);
            return std::move(loaded).Value().owner;
        }

        /** @brief Installs a caller-owned counter buffer without keeping an extra loader reference alive. */
        void Observe(Platform::DynamicLibrary &image, std::array<std::uint32_t, 4> &counters, const bool failMetadata = false) {
            void *symbol = image.GetSymbol("HoroTestObservePerceptionSenseLifecycle");
            REQUIRE(symbol != nullptr);
            using Observer = void (*)(std::uint32_t *, bool) noexcept;
            Observer observer{};
            static_assert(sizeof(observer) == sizeof(symbol));
            std::memcpy(&observer, &symbol, sizeof(observer));
            observer(counters.data(), failMetadata);
        }

        /** @brief Releases the observation handle before testing native retirement or physical shadow cleanup. */
        void Observe(const std::filesystem::path &path, std::array<std::uint32_t, 4> &counters) {
            auto image = Platform::LoadDynamicLibraryAtPath(path);
            REQUIRE(image.HasValue());
            Observe(*image.Value(), counters);
        }

        TEST_CASE("Generated native custom senses retain actual code and revoke before reload", "[ai][sense-provider][native]") {
            std::array<std::uint32_t, 4> counters{};
            Horo::Tests::OwnedTestDirectory shadows{"custom sense native"};
            auto module = Load(shadows.Path());
            Observe(module->LoadedArtifactPath(), counters);
            auto acquired = module->AcquirePerceptionSenses();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            REQUIRE(generation->Contracts().size() == 1);
            CHECK(generation->Contracts()[0].sense.identity.Value() == 102);
            CHECK(generation->Contracts()[0].source.source.ends_with("AiGameModule.cpp"));
            CHECK(generation->Contracts()[0].source.line != 0);
            CHECK(counters[0] == 0);
            PerceptionSenseTestSupport::InputFixture fixture;
            auto instance = fixture.Instance(generation);
            AiWorkBudget budget{6};
            const auto evaluated = instance->Evaluate(fixture.Input(), budget, budget);
            REQUIRE(evaluated.HasValue());
            CHECK(evaluated.Value().count == 2);
            CHECK(counters[0] == 1);
            CHECK(counters[1] == 1);
            CHECK(module->PrepareReload().HasError());
            CHECK_FALSE(generation->CanAdmit());
            CHECK(module->AcquirePerceptionSenses().HasError());
            AiWorkBudget revoked{6};
            TestSupport::ExpectError(instance->Evaluate(fixture.Input(2), revoked, revoked), AIErrors::RuntimeUnavailable);
            CHECK(counters[1] == 1);
            instance.reset();
            CHECK(counters[2] == 1);
            CHECK(module->TryRetire().HasError());
            generation.reset();
            CHECK(module->TryRetire().HasValue());
        }

        TEST_CASE("Destroyed native custom-sense owner leaves cleanup pinned and callable metadata inert", "[ai][sense-provider][native]") {
            std::array<std::uint32_t, 4> counters{};
            Horo::Tests::OwnedTestDirectory shadows{"custom sense retire"};
            auto module = Load(shadows.Path());
            const auto shadow = module->LoadedArtifactPath();
            Observe(shadow, counters);
            auto acquired = module->AcquirePerceptionSenses();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            PerceptionSenseTestSupport::InputFixture fixture;
            auto instance = fixture.Instance(generation);
            const auto metadata = generation->Contracts()[0];
            module.reset();
            CHECK(std::filesystem::exists(shadow));
            CHECK_FALSE(generation->CanAdmit());
            AiWorkBudget budget{6};
            TestSupport::ExpectError(instance->Evaluate(fixture.Input(), budget, budget), AIErrors::RuntimeUnavailable);
            CHECK(counters[1] == 0);
            generation.reset();
            instance.reset();
            CHECK(counters[2] == 1);
            CHECK_FALSE(std::filesystem::exists(shadow));
            CHECK(metadata.sense.identity.Value() == 102);
            CHECK(metadata.source.source.ends_with("AiGameModule.cpp"));
        }

        TEST_CASE("Native custom-sense metadata exceptions reject the complete bundle before module factories",
                  "[ai][sense-provider][native]") {
            std::array<std::uint32_t, 4> counters{};
            auto image = Platform::LoadDynamicLibraryAtPath(HORO_TEST_AI_MODULE_PATH);
            REQUIRE(image.HasValue());
            Observe(*image.Value(), counters, true);
            auto loaded = Gameplay::GameModuleHost{}.Load(HORO_TEST_AI_MODULE_PATH, Expectation());
            REQUIRE(loaded.HasError());
            CHECK(loaded.ErrorValue().code.Value() == AIErrors::PerceptionDescriptorInvalid.code.Value());
            CHECK((counters == std::array<std::uint32_t, 4>{}));
        }

        TEST_CASE("Previous perception SDK is rejected before real bundle export and module creation", "[ai][sense-provider][native]") {
            auto image = Platform::LoadDynamicLibraryAtPath(HORO_TEST_LEGACY_PERCEPTION_MODULE_PATH);
            REQUIRE(image.HasValue());
            void *symbol = image.Value()->GetSymbol("HoroTestLegacyPerceptionEntries");
            REQUIRE(symbol != nullptr);
            using ReadEntries = const std::uint32_t *(*)() noexcept;
            ReadEntries read{};
            static_assert(sizeof(read) == sizeof(symbol));
            std::memcpy(&read, &symbol, sizeof(read));
            const auto *entries = read();
            REQUIRE(entries != nullptr);
            const Gameplay::GameModuleLoadExpectation expected{"game.ai.legacy", Gameplay::CurrentGameplayBuildFingerprint(), 1};
            Gameplay::GameModuleHost host;
            TestSupport::ExpectError(host.ValidateArtifact(HORO_TEST_LEGACY_PERCEPTION_MODULE_PATH, expected),
                                     Gameplay::GameplayErrors::IncompatibleGameModule);
            TestSupport::ExpectError(host.Load(HORO_TEST_LEGACY_PERCEPTION_MODULE_PATH, expected),
                                     Gameplay::GameplayErrors::IncompatibleGameModule);
            CHECK(entries[0] >= 2);
            CHECK(entries[1] == 0);
            CHECK(entries[2] == 0);
        }
    }  // namespace
}  // namespace Horo::AI
