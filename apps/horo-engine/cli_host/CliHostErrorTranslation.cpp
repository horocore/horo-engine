#include "CliHostErrorTranslation.h"

#include "Horo/Application/ProjectErrorDomain.h"
#include "Horo/Assets/AssetErrorDomain.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"
#include "Horo/Foundation/FoundationErrorDomains.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Platform/NativeFileSystemErrors.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "Horo/Scene/SceneSource.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace Horo::Application::Internal {
    namespace {
        using enum Hosts::ExitCategory;
        using Category = Hosts::ExitCategory;

        struct Override final {
            std::string_view code;
            Category category;
        };

        struct Entry final {
            const ErrorCodeDescriptor *descriptor;
            Category category;
        };

        /** @brief Matches only a declared exact code while validating host policy overrides. */
        bool HasCode(const ModuleErrorDomainDescriptor &domain, std::string_view code) {
            return std::ranges::any_of(domain.descriptors, [code](const ErrorCodeDescriptor *descriptor) {
                return descriptor != nullptr && descriptor->code.Value() == code;
            });
        }

        /** @brief Registers one explicit producer contribution and materializes exact mappings for its declared identities. */
        bool AddDomain(std::vector<ModuleDescriptor> &modules, std::vector<Hosts::ErrorMapping> &mappings, std::string owner,
                       ModuleErrorDomainDescriptor domain, Category category, std::initializer_list<Override> overrides = {}) {
            if (std::ranges::any_of(overrides, [&domain](const Override &entry) {
                return !HasCode(domain, entry.code);
            }))
                return false;
            for (const auto *descriptor : domain.descriptors) {
                if (descriptor == nullptr)
                    return false;
                const auto selected = std::ranges::find_if(overrides, [descriptor](const Override &entry) {
                    return entry.code == descriptor->code.Value();
                });
                mappings.emplace_back(descriptor->domain, descriptor->code, selected == overrides.end() ? category : selected->category);
            }
            auto contribution = std::ranges::find_if(modules, [&owner](const ModuleDescriptor &entry) {
                return entry.id.value == owner;
            });
            if (contribution == modules.end()) {
                modules.push_back({.id = {std::move(owner)}, .version = {1, 0, 0}});
                contribution = std::prev(modules.end());
            }
            contribution->errorDomains.push_back(std::move(domain));
            return true;
        }

        /** @brief Composes an explicitly enumerated public producer table; no domain-prefix or message inference occurs. */
        bool AddEntries(std::vector<ModuleDescriptor> &modules, std::vector<Hosts::ErrorMapping> &mappings, std::string owner,
                        std::span<const Entry> entries) {
            ModuleErrorDomainDescriptor domain{.id = entries.front().descriptor->domain};
            for (const auto &entry : entries)
                domain.descriptors.push_back(entry.descriptor);
            if (!AddDomain(modules, mappings, std::move(owner), std::move(domain), Operation))
                return false;
            for (std::size_t i = 0; i < entries.size(); ++i)
                mappings[mappings.size() - entries.size() + i].category = entries[i].category;
            return true;
        }

        constexpr std::array NavigationEntries{Entry{&Navigation::NavigationErrors::IdentityInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::InvalidHandle, Invariant},
                                               Entry{&Navigation::NavigationErrors::GenerationExhausted, Invariant},
                                               Entry{&Navigation::NavigationErrors::CapabilityDescriptorInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::CapabilityStale, Validation},
                                               Entry{&Navigation::NavigationErrors::OperationUnsupported, Capability},
                                               Entry{&Navigation::NavigationErrors::CapabilityUnavailable, Capability},
                                               Entry{&Navigation::NavigationErrors::QueryLimitExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::AdmissionRejected, Operation},
                                               Entry{&Navigation::NavigationErrors::QueryCancelled, Cancelled},
                                               Entry{&Navigation::NavigationErrors::StaleSnapshot, Validation},
                                               Entry{&Navigation::NavigationErrors::NoNavigationData, Validation},
                                               Entry{&Navigation::NavigationErrors::ProviderFailed, Operation},
                                               Entry{&Navigation::NavigationErrors::OutcomeDescriptorInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::InvalidWorld, Validation},
                                               Entry{&Navigation::NavigationErrors::CapacityExceeded, Operation},
                                               Entry{&Navigation::NavigationErrors::PathPortalDegenerate, Validation},
                                               Entry{&Navigation::NavigationErrors::AreaDescriptorInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::FilterDescriptorInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::DescriptorConflict, Validation},
                                               Entry{&Navigation::NavigationErrors::AreaUnknown, Validation},
                                               Entry{&Navigation::NavigationErrors::FilterUnknown, Validation},
                                               Entry{&Navigation::NavigationErrors::AgentProfileInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::AgentProfileReferenced, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceGeometryInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceGeometryUnsupported, Capability},
                                               Entry{&Navigation::NavigationErrors::SourceGeometryCapacityExceeded, Operation},
                                               Entry{&Navigation::NavigationErrors::SourceGeometryStale, Validation},
                                               Entry{&Navigation::NavigationErrors::BakeInputInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::BakeInputReferenceMissing, Validation},
                                               Entry{&Navigation::NavigationErrors::BakeSelectionIncomplete, Validation},
                                               Entry{&Navigation::NavigationErrors::BakeInputCapacityExceeded, Operation},
                                               Entry{&Navigation::NavigationErrors::BakeInputStale, Validation},
                                               Entry{&Navigation::NavigationErrors::BakeInputCancelled, Cancelled},
                                               Entry{&Navigation::NavigationErrors::BakeInputFailed, Operation},
                                               Entry{&Navigation::NavigationErrors::BakeInputShuttingDown, Cancelled},
                                               Entry{&Navigation::NavigationErrors::BakeJobInvalid, Invariant},
                                               Entry{&Navigation::NavigationErrors::BakeJobBudgetExceeded, Operation},
                                               Entry{&Navigation::NavigationErrors::BakeJobAdmissionRejected, Operation},
                                               Entry{&Navigation::NavigationErrors::ProjectProfileInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::ProjectProfileStale, Validation},
                                               Entry{&Navigation::NavigationErrors::ProjectProfileCapacityExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::NavMeshArtifactInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::NavMeshArtifactCorrupt, Validation},
                                               Entry{&Navigation::NavigationErrors::NavMeshArtifactCapacityExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::UnsupportedCookedVersion, Validation},
                                               Entry{&Navigation::NavigationErrors::NavMeshTileUnknown, Validation},
                                               Entry{&Navigation::NavigationErrors::NavMeshProviderPayloadUnavailable, Validation},
                                               Entry{&Navigation::NavigationErrors::NavMeshProviderPayloadIncompatible, Validation},
                                               Entry{&Navigation::NavigationErrors::SceneComponentInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::SceneComponentConflict, Validation},
                                               Entry{&Navigation::NavigationErrors::SceneSurfaceMissing, Validation},
                                               Entry{&Navigation::NavigationErrors::SceneProfileMismatch, Validation},
                                               Entry{&Navigation::NavigationErrors::DynamicRegistryInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::DynamicRegistryConflict, Validation},
                                               Entry{&Navigation::NavigationErrors::DynamicRegistryStale, Validation},
                                               Entry{&Navigation::NavigationErrors::DynamicRegistryCapacityExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::DynamicRegistryUpdateRateExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::DynamicRegistryShuttingDown, Cancelled},
                                               Entry{&Navigation::NavigationErrors::AgentDescriptorInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::AgentRegistryConflict, Validation},
                                               Entry{&Navigation::NavigationErrors::AgentRegistryCapacityExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::AgentRegistryStale, Validation},
                                               Entry{&Navigation::NavigationErrors::AgentRegistryShuttingDown, Cancelled},
                                               Entry{&Navigation::NavigationErrors::SourceEnvelopeInvalid, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceUnsupportedVersion, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceDuplicateIdentity, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceChecksumMismatch, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceSerializationCapacityExceeded, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceUnknownAuthoredRecord, Validation},
                                               Entry{&Navigation::NavigationErrors::GeneratedPayloadQuarantined, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceMigrationMissing, Validation},
                                               Entry{&Navigation::NavigationErrors::SourceMigrationInvalid, Validation}};

        constexpr std::array PhysicsEntries{Entry{&Physics::PhysicsErrors::WorldInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::HandleMalformed, Validation},
                                            Entry{&Physics::PhysicsErrors::HandleWorldMismatch, Validation},
                                            Entry{&Physics::PhysicsErrors::HandleStale, Validation},
                                            Entry{&Physics::PhysicsErrors::GenerationExhausted, Invariant},
                                            Entry{&Physics::PhysicsErrors::CapabilityUnavailable, Capability},
                                            Entry{&Physics::PhysicsErrors::OperationUnsupported, Capability},
                                            Entry{&Physics::PhysicsErrors::TransformAuthorityViolation, Validation},
                                            Entry{&Physics::PhysicsErrors::InvalidState, Invariant},
                                            Entry{&Physics::PhysicsErrors::ThreadAffinityViolation, Invariant},
                                            Entry{&Physics::PhysicsErrors::SolverDeadlineExceeded, Timeout},
                                            Entry{&Physics::PhysicsErrors::SolverValidationMessage, Validation},
                                            Entry{&Physics::PhysicsErrors::SolverAssertionFailed, Invariant},
                                            Entry{&Physics::PhysicsErrors::SolverFatalCondition, Invariant},
                                            Entry{&Physics::PhysicsErrors::BodyStateNonFinite, Validation},
                                            Entry{&Physics::PhysicsErrors::DescriptorInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::CommandOrderInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::SeedPolicyInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::ProfileUnsupported, Capability},
                                            Entry{&Physics::PhysicsErrors::CapacityExceeded, Operation},
                                            Entry{&Physics::PhysicsErrors::CapabilityStale, Validation},
                                            Entry{&Physics::PhysicsErrors::CapabilityRevoked, Validation},
                                            Entry{&Physics::PhysicsErrors::QuerySnapshotStale, Validation},
                                            Entry{&Physics::PhysicsErrors::QueryCancelled, Cancelled},
                                            Entry{&Physics::PhysicsErrors::InitializationFailed, Operation},
                                            Entry{&Physics::PhysicsErrors::ShapeCookSourceInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::ShapeCookImporterFailed, Validation},
                                            Entry{&Physics::PhysicsErrors::ShapeCookLimitExceeded, Validation},
                                            Entry{&Physics::PhysicsErrors::ShapeCookCancelled, Cancelled},
                                            Entry{&Physics::PhysicsErrors::ShapeMotionUnsupported, Validation},
                                            Entry{&Physics::PhysicsErrors::ShapeArtifactInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::MaterialDescriptorInvalid, Validation},
                                            Entry{&Physics::PhysicsErrors::MaterialCombineUnsupported, Validation}};

        constexpr std::array McpEntries{Entry{&Mcp::McpErrors::AuthorizationDenied, Permission},
                                        Entry{&Mcp::McpErrors::ApprovalRequired, Permission},
                                        Entry{&Mcp::McpErrors::ConfigurationInvalid, Invariant},
                                        Entry{&Mcp::McpErrors::AdmissionInvalid, Validation},
                                        Entry{&Mcp::McpErrors::SessionCapacityExceeded, Operation},
                                        Entry{&Mcp::McpErrors::SessionUnavailable, Validation},
                                        Entry{&Mcp::McpErrors::ShuttingDown, Cancelled},
                                        Entry{&Mcp::McpErrors::RequestInvalid, Validation},
                                        Entry{&Mcp::McpErrors::RequestCapacityExceeded, Operation},
                                        Entry{&Mcp::McpErrors::InputCapacityExceeded, Validation},
                                        Entry{&Mcp::McpErrors::ResultCapacityExceeded, Validation},
                                        Entry{&Mcp::McpErrors::RequestCancelled, Cancelled},
                                        Entry{&Mcp::McpErrors::RequestTimedOut, Timeout},
                                        Entry{&Mcp::McpErrors::ControllerFailed, Operation},
                                        Entry{&Mcp::McpErrors::DrainTimedOut, Timeout},
                                        Entry{&Mcp::McpErrors::ToolDescriptorInvalid, Invariant},
                                        Entry{&Mcp::McpErrors::ToolDuplicate, Validation},
                                        Entry{&Mcp::McpErrors::ToolIncompatible, Validation},
                                        Entry{&Mcp::McpErrors::ToolUnavailable, Capability},
                                        Entry{&Mcp::McpErrors::ToolCapabilityUnavailable, Permission},
                                        Entry{&Mcp::McpErrors::ToolInputInvalid, Validation},
                                        Entry{&Mcp::McpErrors::ToolOutputInvalid, Invariant},
                                        Entry{&Mcp::McpErrors::OwnerUnavailable, Invariant},
                                        Entry{&Mcp::McpErrors::OperationCapacityExceeded, Operation},
                                        Entry{&Mcp::McpErrors::OperationUnavailable, Validation},
                                        Entry{&Mcp::McpErrors::RegistryRevisionStale, Validation}};

        constexpr std::array SecurityEntries{Entry{&SecurityErrors::InvalidInput, Permission},
                                             Entry{&SecurityErrors::EntropyUnavailable, Operation},
                                             Entry{&SecurityErrors::CredentialBackendUnavailable, Operation},
                                             Entry{&SecurityErrors::CredentialNotFound, Permission},
                                             Entry{&SecurityErrors::CredentialExpired, Permission},
                                             Entry{&SecurityErrors::CredentialRevoked, Permission},
                                             Entry{&SecurityErrors::UnsupportedAlgorithm, Permission},
                                             Entry{&SecurityErrors::UnknownSigningKey, Permission},
                                             Entry{&SecurityErrors::SignatureProviderUnavailable, Operation},
                                             Entry{&SecurityErrors::InvalidSignature, Permission},
                                             Entry{&SecurityErrors::IntegrityMismatch, Permission},
                                             Entry{&SecurityErrors::MissingEvidence, Permission},
                                             Entry{&SecurityErrors::StaleEvidence, Permission}};

        /** @brief Composes the exact peer producer tables without native backend activation. */
        bool AddHostDependencies(std::vector<ModuleDescriptor> &modules, std::vector<Hosts::ErrorMapping> &mappings) {
            return AddEntries(modules, mappings, "horo.physics", PhysicsEntries) && AddEntries(modules, mappings, "horo.mcp", McpEntries) &&
                   AddEntries(modules, mappings, "horo.security", SecurityEntries);
        }

        /** @brief Registers closed producer scopes; every resulting mapping is an exact domain/code pair. */
        bool AddSourceDomains(std::vector<ModuleDescriptor> &modules, std::vector<Hosts::ErrorMapping> &mappings) {
            return AddDomain(modules, mappings, "horo.foundation", JobSystemErrorDomain(), Operation,
                             {{"job.cancelled", Cancelled},
                              {"job.wait_timed_out", Timeout},
                              {"job.submission_invalid", Invariant},
                              {"job.invalid_handle", Invariant},
                              {"job.wait_forbidden", Invariant},
                              {"job.wait_capacity_deadlock", Invariant}}) &&
                   AddDomain(modules, mappings, "horo.foundation", SceneMathErrorDomain(), Validation) &&
                   AddDomain(modules, mappings, "horo.foundation", ImmutableFileStorageErrorDomain(), Operation,
                             {{"file_storage.invalid_path", Invariant},
                              {"file_storage.unsafe_object", Permission},
                              {"file_storage.cancelled", Cancelled}}) &&
                   AddDomain(modules, mappings, "horo.platform", Platform::ProjectReadErrorDomain(), Validation,
                             {{"project_read.unsafe_path", Permission},
                              {"project_read.read_failed", Operation},
                              {"project_read.capacity", Operation},
                              {"project_read.cancelled", Cancelled},
                              {"project_read.deadline", Timeout}}) &&
                   AddDomain(modules, mappings, "horo.platform", Platform::NativeFileSystemErrorDomain(), Operation) &&
                   AddDomain(modules, mappings, "horo.application", ProjectErrorDomain(), Validation,
                             {{"project.metadata.read_failed", Operation},
                              {"project.migration.cancelled", Cancelled},
                              {"project.migration.stage_failed", Operation},
                              {"project.migration.publish_failed", Operation},
                              {"project.migration.recovery_failed", Operation}}) &&
                   AddDomain(modules, mappings, "horo.scene.source", SceneSource::SceneSourceErrorDomain(), Validation,
                             {{"scene_persistence.read_failed", Operation}}) &&
                   AddDomain(modules, mappings, "horo.assets", Assets::AssetErrorDomain(), Validation,
                             {{"asset.registry.deadline", Timeout},
                              {"asset.registry.index_io", Operation},
                              {"asset.provider.read_failed", Operation},
                              {"asset.cook.source_read_failed", Operation},
                              {"asset.cook.cancelled", Cancelled},
                              {"asset.load.cancelled", Cancelled},
                              {"asset.preview.cancelled", Cancelled},
                              {"asset.import.cancelled", Cancelled}});
        }
    }  // namespace

    using namespace Cli;

    /** @copydoc MakeCliHostErrorTranslator */
    std::optional<Hosts::ErrorTranslator> MakeCliHostErrorTranslator() {
        using enum Hosts::ExitCategory;
        const auto cliErrors = CliErrors::ErrorDomain();
        const auto bundleErrors = Diagnostics::DiagnosticBundleErrorDomain();
        std::vector<ModuleDescriptor> modules{{.id = {"horo.cli"}, .version = {1, 0, 0}, .errorDomains = {cliErrors}},
                                              {.id = {"horo.foundation"}, .version = {1, 0, 0}, .errorDomains = {bundleErrors}}};
        const std::vector<std::pair<const ErrorCodeDescriptor *, Hosts::ExitCategory>>
            scope{{&CliErrors::DescriptorInvalid, Invariant},
                  {&CliErrors::RegistryCapacityExceeded, Invariant},
                  {&CliErrors::CommandPathDuplicate, Invariant},
                  {&CliErrors::OptionNameDuplicate, Invariant},
                  {&CliErrors::OptionSchemaIncompatible, Invariant},
                  {&CliErrors::OutputSchemaIncompatible, Invariant},
                  {&CliErrors::CapabilityUnauthorized, Permission},
                  {&CliErrors::HostUnsupported, Capability},
                  {&CliErrors::ContractVersionIncompatible, Capability},
                  {&CliErrors::ParserPolicyInvalid, Invariant},
                  {&CliErrors::CommandUnknown, Usage},
                  {&CliErrors::ParseFailed, Usage},
                  {&CliErrors::InputModeUnsupported, Usage},
                  {&CliErrors::InputCapacityExceeded, Usage},
                  {&CliErrors::InteractiveInputUnavailable, Usage},
                  {&CliErrors::DispatchRegistrationInvalid, Invariant},
                  {&CliErrors::CommandUnavailable, Capability},
                  {&CliErrors::SideEffectUnauthorized, Permission},
                  {&CliErrors::ExecutionContextInvalid, Invariant},
                  {&CliErrors::ExecutionCancelled, Cancelled},
                  {&CliErrors::ExecutionTimedOut, Timeout},
                  {&CliErrors::ExecutionCapacityExceeded, Operation},
                  {&CliErrors::HostFailure, Invariant}};
        std::vector<Hosts::ErrorMapping> mappings;
        for (const auto &[descriptor, category] : scope)
            mappings.emplace_back(descriptor->domain, descriptor->code, category);
        mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.invalid_request"}, Validation);
        mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.read_failed"}, Operation);
        mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.write_failed"}, Operation);
        mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.size_exceeded"}, Validation);
        if (!AddEntries(modules, mappings, "horo.navigation", NavigationEntries) || !AddHostDependencies(modules, mappings) ||
            !AddSourceDomains(modules, mappings))
            return std::nullopt;
        auto registry = BuildErrorCodeRegistry(modules);
        if (registry.HasError())
            return std::nullopt;
        return Hosts::ErrorTranslator::Create(std::move(registry).Value(), mappings);
    }

}  // namespace Horo::Application::Internal
