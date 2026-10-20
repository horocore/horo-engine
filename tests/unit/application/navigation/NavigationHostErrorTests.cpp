#include "CliHostErrorTranslation.h"
#include "CliHostInterrupts.h"
#include "Horo/Assets/AssetErrorDomain.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Platform/NativeFileSystemErrors.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "Horo/Scene/SceneSource.h"
#include "Horo/Security/SecurityErrors.h"
#include "NavigationCliHost.h"
#include "ProjectProtocolIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Composes the real production table before testing exact categories and cause registration. */
        Hosts::ErrorTranslator Translator() {
            auto translated = MakeCliHostErrorTranslator();
            REQUIRE(translated.has_value());
            return std::move(*translated);
        }
    }  // namespace

    TEST_CASE("Native navigation host preserves validation cancellation and infrastructure exit categories", "[navigation][cli][errors]") {
        using enum Hosts::ExitCategory;
        namespace N = Navigation::NavigationErrors;
        const auto translator = Translator();
        const std::array cases{std::pair{&N::BakeInputInvalid, Validation},     std::pair{&N::BakeInputCancelled, Cancelled},
                               std::pair{&N::ProviderFailed, Operation},        std::pair{&N::BakeJobAdmissionRejected, Operation},
                               std::pair{&N::OperationUnsupported, Capability}, std::pair{&N::GenerationExhausted, Invariant}};
        for (const auto &[descriptor, category] : cases) {
            const auto presented = translator.Translate(MakeError(*descriptor));
            REQUIRE(presented.has_value());
            CHECK(presented->Category() == category);
            CHECK(presented->Failure().code.Value() == descriptor->code.Value());
            CHECK(presented->Failure().domain.Value() == descriptor->domain.Value());
        }
    }

    TEST_CASE("Native navigation host declares real Math scheduler and source error causes", "[navigation][cli][errors]") {
        const auto translator = Translator();
        const auto invalidMath = Math::TryNormalize(Math::Vec3{});
        REQUIRE(invalidMath.HasError());
        const auto cancelled = JobCancelled();
        REQUIRE(cancelled.HasError());
        const auto scene = SceneSource::DecodeSceneSource("{");
        REQUIRE(scene.HasError());
        for (const auto &cause : {invalidMath.ErrorValue(), cancelled.ErrorValue(), scene.ErrorValue()}) {
            const auto presented = translator.Translate(WrapError(Navigation::NavigationErrors::BakeInputInvalid, cause));
            REQUIRE(presented.has_value());
            REQUIRE(presented->Failure().cause.Get() != nullptr);
            CHECK(presented->Failure().cause.Get()->code.Value() == cause.code.Value());
            CHECK(presented->Failure().cause.Get()->domain.Value() == cause.domain.Value());
        }
        const auto cancellation = translator.Translate(cancelled.ErrorValue());
        REQUIRE(cancellation.has_value());
        CHECK(cancellation->Category() == Hosts::ExitCategory::Cancelled);
    }

    TEST_CASE("Native navigation host registers canonical project read identities without legacy aliases", "[navigation][cli][errors]") {
        const auto translator = Translator();
        const auto domain = Platform::ProjectReadErrorDomain();
        REQUIRE(domain.descriptors.size() == 8);
        for (const auto *descriptor : domain.descriptors) {
            CHECK(descriptor->code.Value().starts_with("project_read."));
            CHECK(translator.Translate(MakeError(*descriptor)).has_value());
        }
        const auto expired = Platform::CheckProjectReadContext({.deadline = std::chrono::steady_clock::now()});
        REQUIRE(expired.HasError());
        CHECK(expired.ErrorValue().code.Value() == "project_read.deadline");
        const auto timeout = translator.Translate(expired.ErrorValue());
        REQUIRE(timeout.has_value());
        CHECK(timeout->Category() == Hosts::ExitCategory::Timeout);
        auto legacy = expired.ErrorValue();
        legacy.code = ErrorCode{"deadline"};
        CHECK_FALSE(translator.Translate(legacy).has_value());
    }

    TEST_CASE("Native navigation host preserves protocol and entropy failure identity while redacting details",
              "[navigation][cli][errors]") {
        const auto translator = Translator();
        const auto denied = translator.Translate(MakeError(Mcp::McpErrors::ApprovalRequired));
        REQUIRE(denied.has_value());
        CHECK(denied->Category() == Hosts::ExitCategory::Permission);
        CHECK(denied->Failure().code.Value() == "mcp.approval_required");
        const auto entropy = translator.Translate(MakeError(SecurityErrors::EntropyUnavailable));
        REQUIRE(entropy.has_value());
        CHECK(entropy->Category() == Hosts::ExitCategory::Operation);
        CHECK(entropy->Failure().code.Value() == "security.entropy_unavailable");
        const auto files = Platform::NativeFileSystemErrorDomain();
        REQUIRE(files.descriptors.size() == 2);
        const auto io = translator.Translate(MakeError(*files.descriptors.front(), "PRIVATE PROJECT LOCATION"));
        REQUIRE(io.has_value());
        CHECK(io->Category() == Hosts::ExitCategory::Operation);
        CHECK(io->Json().find("PRIVATE PROJECT LOCATION") == std::string::npos);
    }

    TEST_CASE("Native navigation host refuses undeclared roots and causes without weakening the registry", "[navigation][cli][errors]") {
        const auto translator = Translator();
        const ErrorCodeDescriptor unknown{.domain = ErrorDomainId{"horo.navigation"},
                                          .code = ErrorCode{"navigation.undeclared"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Unregistered test failure."};
        CHECK_FALSE(translator.Translate(MakeError(unknown)).has_value());
        CHECK_FALSE(translator.Translate(WrapError(Navigation::NavigationErrors::BakeInputInvalid, MakeError(unknown))).has_value());
    }

    TEST_CASE("Native host translates all immutable storage failures with exact identities", "[navigation][cli][errors]") {
        using enum Hosts::ExitCategory;
        const auto translator = Translator();
        const std::array cases{std::pair{&FileStorageErrors::InvalidPath, Invariant},
                               std::pair{&FileStorageErrors::UnsafeObject, Permission},
                               std::pair{&FileStorageErrors::Capacity, Operation},
                               std::pair{&FileStorageErrors::Cancelled, Cancelled},
                               std::pair{&FileStorageErrors::Conflict, Operation},
                               std::pair{&FileStorageErrors::IoFailed, Operation}};
        for (const auto &[descriptor, category] : cases) {
            const auto result = translator.Translate(MakeError(*descriptor));
            REQUIRE(result);
            CHECK(result->Category() == category);
            CHECK(result->Failure().domain.Value() == descriptor->domain.Value());
            CHECK(result->Failure().code.Value() == descriptor->code.Value());
        }
    }

#if !defined(_WIN32)
    TEST_CASE("Native CLI signal ownership relays cooperative cancellation and repeated escalation", "[navigation][cli][signals]") {
        Cli::CliInvocationStopController stop{std::chrono::milliseconds{0}};
        NativeCliInterrupts signals;
        REQUIRE(signals.Start(stop).HasValue());
        REQUIRE(std::raise(SIGINT) == 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!stop.Token().IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(stop.Token().IsCancellationRequested());
        CHECK(stop.Reason() == Cli::CliStopReason::Interrupted);
        REQUIRE(std::raise(SIGINT) == 0);
        while (!stop.ForceToken().IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        CHECK(stop.ForceToken().IsCancellationRequested());
    }
#endif

    TEST_CASE("Native navigation admission preserves cancellation before any project read", "[navigation][cli][errors]") {
        CancellationSource cancelled;
        NavigationCliHost host;
        const auto descriptor = Cli::DescribeNavigationBakeCommand();
        auto registry = Cli::CliCommandRegistry::Create(std::span{&descriptor, 1}, {.activeHost = Cli::CliHostKind::HoroEngine,
                                                                                    .supportedContractVersion = {1, 0, 0},
                                                                                    .grantedCapabilities = {{"horo.navigation.bake"}}});
        REQUIRE(registry.HasValue());
        bool acquired{};
        std::vector<Cli::CliCommandAdapterRegistration> adapters;
        adapters.push_back(Cli::MakeNavigationBakeCommandAdapter(
            [&acquired, &cancelled, &host](const Cli::CliCommandRequest &request, const Cli::CliExecutionContext &context) {
            acquired = true;
            cancelled.RequestCancellation();
            return host.Bind(request, context);
        }));
        auto created = Cli::CliDispatcher::Create(std::move(registry).Value(), std::move(adapters),
                                                  {.activeHost = Cli::CliHostKind::HoroEngine,
                                                   .maximumSideEffects = Cli::CliSideEffectPolicy::WritesFiles,
                                                   .grantedCapabilities = {{"horo.navigation.bake"}}});
        REQUIRE(created.HasValue());
        auto dispatcher = std::move(created).Value();
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        const auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        const Cli::CliCommandRequest request{.command = {{"navigation", "bake"}},
                                             .options = {{.name = "definition",
                                                          .values = {{{std::string{"00000000-0000-0000-0000-000000000001"}}}}}}};
        const auto result =
            dispatcher.Dispatch(request, {.invocation = {1}, .configuration = &configuration.Value(), .cancellation = cancelled.Token()});
        CHECK(acquired);
        REQUIRE(result.Outcome().HasError());
        CHECK(result.Outcome().ErrorValue().code.Value() == Cli::CliErrors::ExecutionCancelled.code.Value());
    }

    TEST_CASE("Native project protocol identity satisfies the real MCP authority token policy", "[navigation][cli][mcp]") {
        const auto root = std::filesystem::canonical(std::filesystem::current_path());
        const auto identity = ProjectProtocolIdentity(root);
        CHECK(identity.starts_with("horo.project."));
        CHECK(identity.find(':') == std::string::npos);
        CHECK(identity == ProjectProtocolIdentity(root));
        auto authorization = std::make_shared<Mcp::McpAuthorization>(nullptr);
        CHECK(authorization->SetTrust(identity, 1, true).HasValue());
    }
}  // namespace Horo::Application::Internal
