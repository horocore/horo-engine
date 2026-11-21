#include "../../support/NativePublicationFiles.h"
#include "../../support/project/PlatformServicesMigrationFixture.h"
#include "CliHostErrorTranslation.h"
#include "Horo/Application/PlatformServicesProjectCook.h"
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/Editor/ProjectMutation.h"
#include "PlatformServicesCliHost.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Retains real native commit while injecting a subsequent durability failure. */
        class CookDurabilityFiles final : public TestSupport::NativePublicationFiles {
        public:
            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                return native.WritePrivateDurable(path, bytes, created, permissions);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
                if (receipt.WasCommitted())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
                return replaced;
            }
        };

        /** @brief Composes a real typed cook dispatcher with explicit filesystem and write capability authority. */
        Cli::CliDispatcher CookDispatcher(std::shared_ptr<DurableFileSystem> files) {
            using namespace Cli;
            auto adapter = MakePlatformServicesCookCommandAdapter({.profile = PlatformServices::PlatformServicesHostProfile::Cook}, files);
            const auto descriptor = adapter.adapter->GetDescriptor();
            const std::vector<CliCapabilityId> grants{{"horo.platform-services.cook"}};
            auto registry = CliCommandRegistry::Create(std::span{&descriptor, 1}, {.activeHost = CliHostKind::HoroEngine,
                                                                                   .supportedContractVersion = {1, 0, 0},
                                                                                   .grantedCapabilities = grants});
            REQUIRE(registry.HasValue());
            std::vector<CliCommandAdapterRegistration> registrations;
            registrations.push_back(std::move(adapter));
            auto dispatcher = CliDispatcher::Create(std::move(registry).Value(), std::move(registrations),
                                                    {.activeHost = CliHostKind::HoroEngine,
                                                     .maximumSideEffects = CliSideEffectPolicy::WritesFiles,
                                                     .grantedCapabilities = grants});
            REQUIRE(dispatcher.HasValue());
            return std::move(dispatcher).Value();
        }
    }  // namespace

    TEST_CASE("Cook CLI reports committed partial failure and releases both native leases after durability uncertainty", "[platform-cli]") {
        using namespace Cli;
        Tests::PlatformMigration::Project project;
        project.InitializeCurrent();
        auto files = std::make_shared<CookDurabilityFiles>();
        auto admitted = CookDispatcher(files);
        const auto descriptor = DescribePlatformServicesCookCommand();
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        const auto spelling = project.root.generic_u8string();
        CliCommandRequest request{.command = descriptor.path,
                                  .options = {{.name = "project",
                                               .values = {{.value = CliPathValue{std::string{spelling.begin(), spelling.end()}}}}}}};
        const auto terminal = admitted.Dispatch(request, {.invocation = {1}, .configuration = &configuration.Value()});
        REQUIRE(terminal.Outcome().HasValue());
        const auto &result = terminal.Outcome().Value();
        REQUIRE(result.partialFailures.size() == 1);
        CHECK(result.partialFailures.front().error.code.Value() == Assets::AssetSourceErrors::DurabilityUnconfirmed.code.Value());
        const auto field = [&](std::string_view name) -> const CliResultValue & {
            const auto found = std::ranges::find(result.fields, name, &CliResultField::name);
            REQUIRE(found != result.fields.end());
            return found->value;
        };
        CHECK(std::get<bool>(field("committed")));
        CHECK_FALSE(std::get<bool>(field("durabilityConfirmed")));
        auto pair = Assets::CaptureAssetSource(project.root, project.root / PlatformServicesProjectCookPath,
                                               PlatformServices::PlatformProviderManifestCookGenerationMaximumBytes);
        REQUIRE(pair.HasValue());
        CHECK(std::get<std::string>(field("generationFingerprint")) == FormatSha256(pair.Value().identity.digest));
        CHECK(PlatformServices::DecodePlatformProviderManifestCookGeneration(pair.Value().bytes, pair.Value().identity.digest).HasValue());
        Editor::ProjectMutationCoordinator mutations{files->native};
        CHECK(mutations.TryAcquire({project.root, Editor::ProjectMutationOwner::Migration, "post-cook-lease-probe"}).HasValue());
        auto lockPath = Assets::AssetSourcePublicationLockPath(project.root, project.root / PlatformServicesProjectCookPath);
        REQUIRE(lockPath.HasValue());
        CHECK(files->TryAcquireExclusive(lockPath.Value(), "post-cook-output-lease-probe").HasValue());
    }

    TEST_CASE("Production CLI retains Platform Services authoring error identities", "[platform-cli][errors]") {
        using namespace PlatformServices;
        auto translator = MakeCliHostErrorTranslator();
        REQUIRE(translator.has_value());
        const std::array descriptors{&StableIdErrors::InvalidSalt,
                                     &StableIdErrors::DuplicateKey,
                                     &StableIdErrors::InvalidLedger,
                                     &AchievementDefinitionErrors::InvalidDefinition,
                                     &PlatformDefinitionErrors::InvalidCrossReference,
                                     &PlatformProjectConfigurationErrors::InvalidConfiguration,
                                     &PlatformProviderManifestCookErrors::StaleGeneration,
                                     &Assets::AssetSourceErrors::Conflict};
        for (const auto *descriptor : descriptors) {
            const auto presented = translator->Translate(MakeError(*descriptor));
            REQUIRE(presented.has_value());
            CHECK(presented->Category() == Hosts::ExitCategory::Validation);
            CHECK(presented->Failure().code.Value() == descriptor->code.Value());
            CHECK(presented->Failure().domain.Value() == descriptor->domain.Value());
        }
    }

    TEST_CASE("Production CLI distinguishes missing providers, trust refusal and cancelled capture", "[platform-cli][errors]") {
        using namespace PlatformServices;
        auto translator = MakeCliHostErrorTranslator();
        REQUIRE(translator.has_value());
        for (const auto *descriptor :
             {&PlatformProjectConfigurationErrors::ProviderNotFound, &PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported}) {
            const auto presented = translator->Translate(MakeError(*descriptor));
            REQUIRE(presented.has_value());
            CHECK(presented->Category() == Hosts::ExitCategory::Capability);
        }
        const auto denied = translator->Translate(MakeError(PlatformProjectConfigurationErrors::UntrustedContribution));
        REQUIRE(denied.has_value());
        CHECK(denied->Category() == Hosts::ExitCategory::Permission);
        const auto cancelled = translator->Translate(MakeError(Assets::AssetSourceErrors::Cancelled));
        REQUIRE(cancelled.has_value());
        CHECK(cancelled->Category() == Hosts::ExitCategory::Cancelled);
        const auto cookCancelled = translator->Translate(MakeError(PlatformProviderManifestCookErrors::Cancelled));
        REQUIRE(cookCancelled.has_value());
        CHECK(cookCancelled->Category() == Hosts::ExitCategory::Cancelled);
        const auto mappingMissing = translator->Translate(MakeError(PlatformProviderManifestCookErrors::MissingMapping));
        REQUIRE(mappingMissing.has_value());
        CHECK(mappingMissing->Category() == Hosts::ExitCategory::Capability);
        const auto mutation = Editor::ProjectMutationErrorDomain();
        REQUIRE(mutation.descriptors.size() == 1);
        const auto busy = translator->Translate(MakeError(*mutation.descriptors.front()));
        REQUIRE(busy.has_value());
        CHECK(busy->Category() == Hosts::ExitCategory::Operation);
        CHECK(busy->Failure().domain.Value() == mutation.id.Value());
    }
}  // namespace Horo::Application::Internal
