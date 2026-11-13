#include "PlatformServicesCliHost.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Editor/ProjectMutation.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Resolves only the explicit parser-owned project root; never selects ambient working-directory input. */
        Result<std::filesystem::path> ProjectRoot(const Cli::CliCommandRequest &request) {
            using Return = Result<std::filesystem::path>;
            const auto option = std::ranges::find(request.options, "project", &Cli::CliParsedOption::name);
            if (option == request.options.end() || option->values.size() != 1)
                return Return::Failure(MakeError(Cli::CliErrors::ParseFailed));
            const auto *value = std::get_if<Cli::CliPathValue>(&option->values.front().value);
            if (!value || value->value.empty() || value->value.find('\0') != std::string::npos)
                return Return::Failure(MakeError(Cli::CliErrors::ParseFailed));
            const std::filesystem::path input{std::u8string{value->value.begin(), value->value.end()}};
            if (!input.is_absolute() || input != input.lexically_normal())
                return Return::Failure(MakeError(Cli::CliErrors::ParseFailed));
            std::error_code error;
            auto root = std::filesystem::canonical(input, error);
            if (error)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            return Return::Success(std::move(root));
        }

        /** @brief Shares base admission and source validation across the read-only and cook commands. */
        Result<PlatformServicesProjectSourceSnapshot> CaptureAtRoot(const std::filesystem::path &root, Cli::CliExecutionContext &execution,
                                                                    const PlatformServicesProjectValidationContext &context) {
            using Return = Result<PlatformServicesProjectSourceSnapshot>;
            if (execution.IsStopRequested())
                return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
            const auto admission = InspectProjectCompatibility(root);
            if (admission.diagnostic)
                return Return::Failure(*admission.diagnostic);
            return CapturePlatformServicesProjectSources(root, admission, context, execution.Cancellation());
        }

        /** @brief Synchronous bounded read-only adapter; owns policy but no provider, window, cache or source mutation state. */
        class ValidationAdapter final : public Cli::ICliCommandAdapter {
        public:
            explicit ValidationAdapter(PlatformServicesProjectValidationContext context) : context_(std::move(context)) {}

            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &execution) override {
                using Return = Result<Cli::CliCommandResult>;
                if (execution.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                auto root = ProjectRoot(request);
                if (root.HasError())
                    return Return::Failure(std::move(root).ErrorValue());
                auto sources = CaptureAtRoot(root.Value(), execution, context_);
                if (sources.HasError())
                    return Return::Failure(std::move(sources).ErrorValue());
                Cli::CliCommandResult result{.fields = {{"validated", true},
                                                        {"sourceFingerprint", FormatSha256(sources.Value().SourceFingerprint())},
                                                        {"registryFingerprint", FormatSha256(sources.Value().StableIds().Fingerprint())},
                                                        {"nullProvider", sources.Value().Configuration().UsesNullProvider()}}};
                auto current = RevalidatePlatformServicesProjectSources(sources.Value(), execution.Cancellation());
                if (current.HasError())
                    return Return::Failure(std::move(current).ErrorValue());
                if (execution.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                return Return::Success(std::move(result));
            }

        private:
            PlatformServicesProjectValidationContext context_;
            Cli::CliCommandDescriptor descriptor_{DescribePlatformServicesValidationCommand()};
        };

        /** @brief Owns only explicit cook policy and durable authority; never fabricates a private provider mapping. */
        class CookAdapter final : public Cli::ICliCommandAdapter {
        public:
            CookAdapter(PlatformServicesProjectValidationContext context, std::shared_ptr<DurableFileSystem> files)
                : context_(std::move(context)), files_(std::move(files)) {}

            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &execution) override {
                using Return = Result<Cli::CliCommandResult>;
                if (!files_)
                    return Return::Failure(MakeError(Cli::CliErrors::CommandUnavailable));
                if (execution.IsStopRequested())
                    return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                auto admittedRoot = ProjectRoot(request);
                if (admittedRoot.HasError())
                    return Return::Failure(std::move(admittedRoot).ErrorValue());
                Editor::ProjectMutationCoordinator mutations{*files_};
                auto mutation = mutations.TryAcquire({admittedRoot.Value(), Editor::ProjectMutationOwner::Cli, "platform-services-cook"});
                if (mutation.HasError())
                    return Return::Failure(std::move(mutation).ErrorValue());
                auto source = CaptureAtRoot(admittedRoot.Value(), execution, context_);
                if (source.HasError())
                    return Return::Failure(std::move(source).ErrorValue());
                if (!source.Value().Configuration().UsesNullProvider())
                    return Return::Failure(MakeError(PlatformServices::PlatformProviderManifestCookErrors::MissingMapping));
                auto lease = AcquireCookLease(source.Value(), *files_);
                if (lease.HasError())
                    return Return::Failure(std::move(lease).ErrorValue());
                auto expected = CaptureCookIdentity(source.Value(), execution.Cancellation());
                if (expected.HasError())
                    return Return::Failure(std::move(expected).ErrorValue());
                auto result = PreparedResult(source.Value());
                auto published = CookAndPublishPlatformServicesProject({.sources = source.Value(),
                                                                        .validateMappingRevision =
                                                                            [&] {
                    return execution.IsStopRequested() ? Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionCancelled))
                                                       : Result<void>::Success();
                }},
                                                                       expected.Value(), lease.Value(), *files_, execution.Cancellation());
                if (published.HasError())
                    return Return::Failure(std::move(published).ErrorValue());
                auto receipt = std::move(published).Value();
                const auto durable = receipt.publication.status == Assets::AssetSourcePublicationStatus::Durable;
                std::get<bool>(result.fields[1].value) = durable;
                SetDigest(std::get<std::string>(result.fields[3].value), receipt.generationFingerprint);
                if (durable)
                    result.partialFailures.clear();
                else if (receipt.publication.durabilityError)
                    result.partialFailures.front().error = std::move(*receipt.publication.durabilityError);
                return Return::Success(std::move(result));
            }

        private:
            /** @brief Acquires the exact cook destination lease, retained explicitly by Execute through publication. */
            static Result<ExclusiveFileLock> AcquireCookLease(const PlatformServicesProjectSourceSnapshot &source,
                                                              DurableFileSystem &files) {
                const auto &root = source.ProjectRoot();
                auto path = Assets::AssetSourcePublicationLockPath(root, root / PlatformServicesProjectCookPath);
                if (path.HasError())
                    return Result<ExclusiveFileLock>::Failure(std::move(path).ErrorValue());
                return files.TryAcquireExclusive(path.Value(), "platform-services-cli-cook");
            }

            /** @brief Captures the exact previous pair under its lease and releases the temporary bytes before cooking. */
            static Result<Assets::AssetSourceIdentity> CaptureCookIdentity(const PlatformServicesProjectSourceSnapshot &source,
                                                                           const CancellationToken cancellation) {
                const auto &root = source.ProjectRoot();
                auto previous =
                    Assets::CaptureAssetSource(root, root / PlatformServicesProjectCookPath,
                                               PlatformServices::PlatformProviderManifestCookGenerationMaximumBytes, cancellation);
                if (previous.HasError())
                    return Result<Assets::AssetSourceIdentity>::Failure(std::move(previous).ErrorValue());
                return Result<Assets::AssetSourceIdentity>::Success(previous.Value().identity);
            }

            /** @brief Allocates owned protocol and fallback-error storage before irreversible publication. */
            static Cli::CliCommandResult PreparedResult(const PlatformServicesProjectSourceSnapshot &source) {
                static_assert(std::is_nothrow_move_assignable_v<Error>);
                return {.fields = {{"committed", true},
                                   {"durabilityConfirmed", false},
                                   {"sourceFingerprint", FormatSha256(source.SourceFingerprint())},
                                   {"generationFingerprint", FormatSha256({})}},
                        .partialFailures = {{"publication.durability", MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)}}};
            }

            /** @brief Fills preallocated canonical digest text without allocation after native commit. */
            static void SetDigest(std::string &text, const Sha256Digest &digest) noexcept {
                constexpr std::string_view digits = "0123456789abcdef";
                constexpr std::size_t prefixBytes = std::string_view{"sha256:"}.size();
                for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
                    text[prefixBytes + index * 2] = digits[digest.bytes[index] >> 4U];
                    text[prefixBytes + index * 2 + 1] = digits[digest.bytes[index] & 15U];
                }
            }

            PlatformServicesProjectValidationContext context_;
            std::shared_ptr<DurableFileSystem> files_;
            Cli::CliCommandDescriptor descriptor_{DescribePlatformServicesCookCommand()};
        };
    }  // namespace

    /** @copydoc DescribePlatformServicesValidationCommand */
    Cli::CliCommandDescriptor DescribePlatformServicesValidationCommand() {
        using namespace Cli;
        return {.path = {{"platform-services", "validate"}},
                .summary = "Validate the explicit project's Platform Services sources using the shared headless host policy.",
                .requiredCapabilities = {{"horo.platform-services.validate"}},
                .output = {.id = "horo.platform-services.validation",
                           .version = 1,
                           .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines},
                .interactive = CliInteractivePolicy::Forbidden,
                .hosts = CliHostAvailability::HoroEngine,
                .contractVersion = {1, 0, 0},
                .sideEffects = CliSideEffectPolicy::ReadsState,
                .cancellation = CliCancellationPolicy::Cooperative,
                .ownerId = "horo.platform.services.cli"};
    }

    /** @copydoc MakePlatformServicesValidationCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePlatformServicesValidationCommandAdapter(PlatformServicesProjectValidationContext context) {
        return {.adapter = std::make_unique<ValidationAdapter>(std::move(context)), .capabilities = {{"horo.platform-services.validate"}}};
    }

    /** @copydoc DescribePlatformServicesCookCommand */
    Cli::CliCommandDescriptor DescribePlatformServicesCookCommand() {
        auto descriptor = DescribePlatformServicesValidationCommand();
        descriptor.path = {{"platform-services", "cook"}};
        descriptor.summary = "Cook the explicit project's Platform Services and atomically publish the complete paired generation.";
        descriptor.requiredCapabilities = {{"horo.platform-services.cook"}};
        descriptor.output.id = "horo.platform-services.cook";
        descriptor.sideEffects = Cli::CliSideEffectPolicy::WritesFiles;
        return descriptor;
    }

    /** @copydoc MakePlatformServicesCookCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePlatformServicesCookCommandAdapter(PlatformServicesProjectValidationContext context,
                                                                              std::shared_ptr<DurableFileSystem> files) {
        return {.adapter = std::make_unique<CookAdapter>(std::move(context), std::move(files)),
                .capabilities = {{"horo.platform-services.cook"}}};
    }
}  // namespace Horo::Application::Internal
