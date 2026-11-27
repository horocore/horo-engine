#include "PrefabCliHost.h"

#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Application/PrefabSourceMigrationPlan.h"
#include "Horo/Application/ProjectMigrationErrors.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "NativePrefabCookComposition.h"

#include <algorithm>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        constexpr std::string_view Capability = "horo.prefab.cook";
        constexpr std::string_view MigrationPlanCapability = "horo.prefab.migration-plan";
        using Return = Result<Cli::CliCommandResult>;

        /** @brief Borrow parser-owned typed values without reparsing argv. */
        std::span<const Cli::CliResolvedValue> Option(const Cli::CliCommandRequest &request, const std::string_view name) {
            const auto found = std::ranges::find(request.options, name, &Cli::CliParsedOption::name);
            return found == request.options.end() ? std::span<const Cli::CliResolvedValue>{} : std::span{found->values};
        }

        /** @brief Admit only a project path supplied in the current invocation. */
        Result<std::filesystem::path> ExplicitProjectRoot(const Cli::CliCommandRequest &request) {
            using Parsed = Result<std::filesystem::path>;
            const auto project = Option(request, "project");
            if (project.size() != 1 || project.front().source != Cli::CliValueSource::Invocation)
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            const auto *path = std::get_if<Cli::CliPathValue>(&project.front().value);
            if (!path || path->value.empty() || path->value.find('\0') != std::string::npos)
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            std::filesystem::path root{std::u8string{path->value.begin(), path->value.end()}};
            if (!root.is_absolute() || root != root.lexically_normal())
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            return Parsed::Success(std::move(root));
        }

        /** @brief Resolve the cook target and a bounded unique list of runtime root IDs. */
        Result<NativePrefabCookInvocation> Parse(const Cli::CliCommandRequest &request, const Cli::CliExecutionContext &context) {
            using Parsed = Result<NativePrefabCookInvocation>;
            auto root = ExplicitProjectRoot(request);
            if (root.HasError())
                return Parsed::Failure(std::move(root).ErrorValue());
            const auto targets = Option(request, "target");
            if (targets.size() != 1)
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            const auto *text = std::get_if<std::string>(&targets.front().value);
            if (!text)
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            auto target = AssetCookTargetId::Parse(*text);
            if (target.HasError() || target.Value().Value() != *text)
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            NativePrefabCookInvocation invocation{.project = std::move(root).Value(),
                                                  .target = std::move(target).Value(),
                                                  .deadline =
                                                      context.AbsoluteDeadline().value_or(std::chrono::steady_clock::time_point::max())};
            const auto roots = Option(request, "runtime-prefab");
            if (roots.size() > Assets::AssetCookLimits{}.maximumAssets)
                return Parsed::Failure(MakeError(Cli::CliErrors::InputCapacityExceeded));
            for (const auto &option : roots) {
                const auto *value = std::get_if<std::string>(&option.value);
                if (!value)
                    return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
                auto id = Assets::AssetId::Parse(*value);
                if (id.HasError() || !id.Value().IsValid() || id.Value().ToString() != *value)
                    return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
                invocation.runtimeRoots.push_back(id.Value());
            }
            std::ranges::sort(invocation.runtimeRoots);
            if (std::ranges::adjacent_find(invocation.runtimeRoots) != invocation.runtimeRoots.end())
                return Parsed::Failure(MakeError(Cli::CliErrors::ParseFailed));
            return Parsed::Success(std::move(invocation));
        }

        /** @brief Deliver only the authoritative cook's own operation and stable phase. */
        void ForwardProgress(void *opaque, const OperationId id, const std::string_view phase, const float progress) noexcept {
            try {
                auto &execution = *static_cast<Cli::CliExecutionContext *>(opaque);
                if (!execution.Correlation().operation && execution.BindOperation({id}).HasError())
                    return;
                if (execution.Correlation().operation->value == id)
                    static_cast<void>(execution.ReportProgress({std::string{phase}, progress, {}}));
            } catch (...) {
                // Progress is optional and cannot fail a cook or reverse publication.
            }
        }

        /** @brief Preserve stable identity and safe prefab source evidence without forwarding native causes or paths. */
        Error RedactCookFailure(const Error &error) {
            if (IsJobCancelled(error) ||
                ErrorChainContains(error, PrefabSceneCookErrors::Cancelled.domain, PrefabSceneCookErrors::Cancelled.code) ||
                ErrorChainContains(error, Assets::CookErrors::Cancelled.domain, Assets::CookErrors::Cancelled.code) ||
                ErrorChainContains(error, Prefab::PrefabErrors::Cancelled.domain, Prefab::PrefabErrors::Cancelled.code) ||
                ErrorChainContains(error, FileStorageErrors::Cancelled.domain, FileStorageErrors::Cancelled.code))
                return MakeError(Cli::CliErrors::ExecutionCancelled);
            return {.code = error.code, .domain = error.domain, .severity = error.severity, .message = "Prefab cook failed."};
        }

        /** @brief Publish immutable generation evidence without allocating after the commit boundary. */
        void FillCommittedResult(Cli::CliCommandResult &result, const Assets::AssetCookReport &report) noexcept {
            const auto &generation = report.generation;
            auto &fingerprint = std::get<std::string>(result.fields[2].value);
            constexpr std::string_view digits = "0123456789abcdef";
            constexpr auto prefix = std::string_view{"sha256:"}.size();
            for (std::size_t index = 0; index < generation.manifestDigest.bytes.size(); ++index) {
                const auto byte = generation.manifestDigest.bytes[index];
                fingerprint[prefix + index * 2] = digits[byte >> 4U];
                fingerprint[prefix + index * 2 + 1] = digits[byte & 0x0fU];
            }
            std::get<bool>(result.fields[1].value) = !generation.durabilityError;
            std::get<std::int64_t>(result.fields[3].value) = static_cast<std::int64_t>(generation.artifactCount);
            std::get<std::int64_t>(result.fields[4].value) = static_cast<std::int64_t>(report.cacheHits);
            if (!generation.durabilityError)
                result.partialFailures.clear();
        }

        /** @brief Parse and project the host-owned cook without package activation or migration. */
        class PrefabCookAdapter final : public Cli::ICliCommandAdapter {
        public:
            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Return Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                if (context.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                auto parsed = Parse(request, context);
                if (parsed.HasError())
                    return Return::Failure(std::move(parsed).ErrorValue());

                BuildOutputStore output{32};
                OperationStore operations{1, 1};
                std::optional<Assets::AssetRecord> failedSource;
                auto invocation = std::move(parsed).Value();
                invocation.output = &output;
                invocation.operations = &operations;
                invocation.progress = {&context, ForwardProgress};
                invocation.failedSource = &failedSource;
                Cli::CliCommandResult result{.fields = {{"committed", true},
                                                        {"durabilityConfirmed", false},
                                                        {"manifestDigest", FormatSha256({})},
                                                        {"artifacts", std::int64_t{}},
                                                        {"cacheHits", std::int64_t{}}},
                                             .partialFailures = {
                                                 {"publication.durability", MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)}}};
                auto cooked = CookNativePrefabProject(invocation, context.Cancellation());
                if (cooked.HasError()) {
                    if (failedSource)
                        static_cast<void>(context.BindAssetSource({failedSource->id.ToString(), std::move(failedSource->sourcePath)}));
                    return Return::Failure(RedactCookFailure(cooked.ErrorValue()));
                }
                FillCommittedResult(result, cooked.Value());
                return Return::Success(std::move(result));
            }

        private:
            Cli::CliCommandDescriptor descriptor_{DescribePrefabCookCommand()};
        };

        /** @brief Preserves registered migration error identities while dropping untrusted source details. */
        Error RedactMigrationFailure(const Error &error) {
            if (IsJobCancelled(error) ||
                ErrorChainContains(error, ProjectErrors::MigrationCancelled.domain, ProjectErrors::MigrationCancelled.code))
                return MakeError(Cli::CliErrors::ExecutionCancelled);
            return {.code = error.code, .domain = error.domain, .severity = error.severity, .message = "Project migration failed."};
        }

        /** @brief Retains a bounded ordered list of safe project-relative dry-run changes. */
        void AddVerificationFields(Cli::CliCommandResult &result, const ProjectMigrationDryRunResult *verified) {
            constexpr std::size_t MaximumPathListBytes = 16 * 1024;
            std::size_t changedFiles{};
            bool omitted{};
            std::string paths;
            if (verified) {
                changedFiles = verified->changedFiles.size();
                for (const auto &path : verified->changedFiles) {
                    if (path.size() + 1 > MaximumPathListBytes - paths.size() || !Platform::IsSafeProjectReadPath(path)) {
                        omitted = true;
                        continue;
                    }
                    if (!paths.empty())
                        paths.push_back('\n');
                    paths.append(path);
                }
            }
            result.fields.push_back({"verified", verified != nullptr});
            result.fields.push_back({"changedFileCount", static_cast<std::int64_t>(changedFiles)});
            result.fields.push_back({"changedProjectPaths", std::move(paths)});
            result.fields.push_back({"changedPathsOmitted", omitted});
        }

        /** @brief Project the authoritative chain or fail with the preflight's typed diagnostic. */
        Return ProjectAdoptionPlan(const Editor::ProjectOpenPreflightSnapshot &snapshot, const bool adoptionRequired) {
            const auto &admission = snapshot.compatibility;
            using enum ProjectCompatibilityStatus;
            const auto &plan = snapshot.migrationPlan;
            if (!plan && (!admission.sourceBaseline || (admission.status != Current && admission.status != CompatibleReleaseLine))) {
                const auto error = admission.diagnostic.value_or(MakeError(Prefab::PrefabErrors::MigrationRequired));
                return Return::Failure(RedactMigrationFailure(error));
            }
            std::string definitions;
            if (plan) {
                for (const auto &definition : plan->definitions) {
                    if (!definitions.empty())
                        definitions.push_back('\n');
                    definitions.append(definition.id.value);
                }
            }
            const auto &source = plan ? plan->source : *admission.sourceBaseline;
            const auto &target = plan ? plan->target : source;
            return Return::Success({.fields = {{"prefabAdoptionRequired", adoptionRequired},
                                               {"sourceBaseline", FormatHoroVersion(source.value)},
                                               {"targetBaseline", FormatHoroVersion(target.value)},
                                               {"definitionCount", static_cast<std::int64_t>(plan ? plan->definitions.size() : 0)},
                                               {"definitionChain", std::move(definitions)}}});
        }

        /** @brief Verifies the full selected chain with the ordinary transaction-owned terminal documents. */
        Result<ProjectMigrationDryRunResult> VerifyAdoption(const std::filesystem::path &root, const ProjectMigrationPlan &plan,
                                                            const ProjectCompatibilitySnapshot &admission,
                                                            const Editor::ProjectMigrationTransactionService &transactions,
                                                            const CancellationToken cancellation) {
            const auto *target = BuiltInReleaseCompatibilityRegistry().Find(admission.targetVersion);
            if (!target || !admission.metadata || !admission.sourceBaseline)
                return Result<ProjectMigrationDryRunResult>::Failure(MakeError(Prefab::PrefabErrors::MigrationRequired));
            return transactions.Verify({.projectRoot = root,
                                        .sourceMetadata = *admission.metadata,
                                        .sourceBaseline = *admission.sourceBaseline,
                                        .targetDecision = *target,
                                        .plan = plan,
                                        .engineBuildIdentity = "horo-engine-prefab-verification",
                                        .cancellation = cancellation});
        }

        /** @brief Inspect migration compatibility and recovery through the existing project-open authority. */
        class PrefabMigrationPlanAdapter final : public Cli::ICliCommandAdapter {
        public:
            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Return Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                if (context.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                auto input = ExplicitProjectRoot(request);
                if (input.HasError())
                    return Return::Failure(std::move(input).ErrorValue());
                std::error_code error;
                const auto root = std::filesystem::canonical(input.Value(), error);
                if (error)
                    return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));

                const auto flags = Option(request, "verify");
                const auto *flag = flags.size() == 1 && flags.front().source == Cli::CliValueSource::Invocation
                                       ? std::get_if<bool>(&flags.front().value)
                                       : nullptr;
                const bool verify = flag && *flag;
                NativeDurableFileSystem files;
                SystemWallClock clock;
                Editor::ProjectMutationCoordinator mutations{files};
                JobSystem jobs{{.workerCount = verify ? 2U : 0U, .maxQueuedJobs = 32}};
                const ProjectMigrationExecutionServices executionServices{.random = verify ? Platform::CreateNativeSecureRandomSource()
                                                                                           : nullptr};
                Editor::ProjectMigrationTransactionService transactions{files, clock, mutations, jobs, {}, executionServices};
                const Editor::ProjectOpenPreflightService preflight{transactions};
                const auto snapshot = preflight.Inspect(root);
                if (context.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                const bool adoption =
                    snapshot.migrationPlan && ProjectMigrations::PlanIncludesPrefabSourceAdoption(*snapshot.migrationPlan);
                auto result = ProjectAdoptionPlan(snapshot, adoption);
                if (result.HasError())
                    return result;
                auto output = std::move(result).Value();
                if (!verify || !adoption) {
                    AddVerificationFields(output, nullptr);
                    return Return::Success(std::move(output));
                }
                const auto verified =
                    VerifyAdoption(root, *snapshot.migrationPlan, snapshot.compatibility, transactions, context.Cancellation());
                if (context.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                if (verified.HasError())
                    return Return::Failure(RedactMigrationFailure(verified.ErrorValue()));
                AddVerificationFields(output, &verified.Value());
                return Return::Success(std::move(output));
            }

        private:
            Cli::CliCommandDescriptor descriptor_{DescribePrefabMigrationPlanCommand()};
        };
    }  // namespace

    /** @copydoc DescribePrefabCookCommand */
    Cli::CliCommandDescriptor DescribePrefabCookCommand() {
        using namespace Cli;
        return {.path = {{"prefab", "cook"}},
                .summary = "Cook an explicit project's scenes and selected runtime prefabs through the headless prefab host.",
                .options = {{.name = "runtime-prefab",
                             .summary = "Canonical ID of an explicitly spawnable prefab root; may repeat.",
                             .valueKind = CliOptionValueKind::String,
                             .repeatable = true},
                            {.name = "target",
                             .summary = "Cook target identity, for example headless-null.",
                             .valueKind = CliOptionValueKind::String,
                             .required = true}},
                .requiredCapabilities = {{std::string{Capability}}},
                .output = {.id = "horo.prefab.cook",
                           .version = 1,
                           .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines,
                           .progressRecords = true},
                .interactive = CliInteractivePolicy::Forbidden,
                .hosts = CliHostAvailability::HoroEngine,
                .contractVersion = {1, 0, 0},
                .sideEffects = CliSideEffectPolicy::WritesFiles,
                .cancellation = CliCancellationPolicy::Cooperative,
                .timeout = {.maximumMilliseconds = 86'400'000},
                .ownerId = "horo.prefab.cli"};
    }

    /** @copydoc MakePrefabCookCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePrefabCookCommandAdapter() {
        return {.adapter = std::make_unique<PrefabCookAdapter>(), .capabilities = {{std::string{Capability}}}};
    }

    /** @copydoc DescribePrefabMigrationPlanCommand */
    Cli::CliCommandDescriptor DescribePrefabMigrationPlanCommand() {
        using namespace Cli;
        return {.path = {{"prefab", "migration-plan"}},
                .summary = "Inspect the project migration chain for prefab source adoption.",
                .options = {{.name = "verify",
                             .summary = "Execute and validate the selected adoption chain in disposable staging.",
                             .valueKind = CliOptionValueKind::Flag}},
                .requiredCapabilities = {{std::string{MigrationPlanCapability}}},
                .output = {.id = "horo.prefab.migration-plan",
                           .version = 1,
                           .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines},
                .interactive = CliInteractivePolicy::Forbidden,
                .hosts = CliHostAvailability::HoroEngine,
                .contractVersion = {1, 0, 0},
                .sideEffects = CliSideEffectPolicy::WritesFiles,
                .cancellation = CliCancellationPolicy::Cooperative,
                .timeout = {.maximumMilliseconds = 86'400'000},
                .ownerId = "horo.prefab.cli"};
    }

    /** @copydoc MakePrefabMigrationPlanCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePrefabMigrationPlanCommandAdapter() {
        return {.adapter = std::make_unique<PrefabMigrationPlanAdapter>(), .capabilities = {{std::string{MigrationPlanCapability}}}};
    }
}  // namespace Horo::Application::Internal
