#include "Horo/Application/GameplayBuildService.h"

#include "GameplayBuildInputs.h"
#include "GameplayBuildInvocation.h"
#include "Horo/Application/CompilerDiagnosticParser.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Horo::Application {
    namespace {
        const ErrorDomainId Domain{"horo.application.gameplay_build"};
        using Detail::BuildConfigureArguments;
        using Detail::BuildEnvironment;
        using Detail::CompilerIdentity;
        using Detail::ComputeInputHash;
        using Detail::HashFile;
        using Detail::ReadSuccessfulHash;
        using Detail::ResolveCompilerIdentity;
        using Detail::TransparentStringHash;
        const ErrorCodeDescriptor BuildFailed{Domain,
                                              ErrorCode{"build_failed"},
                                              ErrorSeverity::Error,
                                              "Gameplay module build failed.",
                                              "Open Build Output and fix the reported source error.",
                                              true,
                                              true};
        const ErrorCodeDescriptor ValidationFailed{Domain,
                                                   ErrorCode{"validation_failed"},
                                                   ErrorSeverity::Error,
                                                   "Gameplay artifact validation failed.",
                                                   "Rebuild the module with the active Horo gameplay SDK.",
                                                   true,
                                                   true};
        const ErrorCodeDescriptor CancelledDescriptor{Domain,
                                                      ErrorCode{"cancelled"},
                                                      ErrorSeverity::Info,
                                                      "Gameplay build was cancelled.",
                                                      "Start the build again when ready.",
                                                      true,
                                                      false};
        const ErrorCodeDescriptor TimedOutDescriptor{Domain,
                                                     ErrorCode{"timed_out"},
                                                     ErrorSeverity::Error,
                                                     "Gameplay build timed out.",
                                                     "Inspect the toolchain and retry the build.",
                                                     true,
                                                     true};
        const ErrorCodeDescriptor OperationAdmissionFailed{Domain,
                                                           ErrorCode{"operation_admission_failed"},
                                                           ErrorSeverity::Error,
                                                           "Gameplay build could not be admitted to the operation store.",
                                                           "Wait for an active build to finish and retry.",
                                                           true,
                                                           true};
        const ErrorCodeDescriptor BuildInputsChanged{Domain,
                                                     ErrorCode{"inputs_changed"},
                                                     ErrorSeverity::Info,
                                                     "Gameplay build inputs changed while the build was running.",
                                                     "The build will be retried with the latest inputs.",
                                                     true,
                                                     false};

        [[nodiscard]] bool IsTerminal(const GameplayBuildState state) noexcept {
            using enum GameplayBuildState;
            return state == Succeeded || state == Failed || state == Cancelled || state == TimedOut;
        }

        [[nodiscard]] std::uint64_t CurrentProcessId() noexcept {
#if defined(_WIN32)
            return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
            return static_cast<std::uint64_t>(getpid());
#endif
        }

        [[nodiscard]] std::int64_t ProcessStartedAtSeconds() noexcept {
            static const std::int64_t started =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            return started;
        }
    }  // namespace

    struct GameplayBuildService::State {
        struct Session {
            [[nodiscard]] std::mutex &Mutex() noexcept {
                return mutex_;
            }

            GameplayBuildSnapshot snapshot;
            std::optional<BuildOutputSessionId> outputSessionId;
            GameplayBuildRequest request;
            std::optional<GameplayBuildRequest> pendingRequest;
            CancellationSource cancellation;
            bool cacheHit{false};
            JobId jobId{};
            std::shared_ptr<JobHandle> job;

            std::mutex transitionMutex;
            std::mutex mutex_;
        };

        [[nodiscard]] std::mutex &Mutex() noexcept {
            return mutex_;
        }

        IExternalProcessRunner *processes{};
        JobSystem *jobs{};
        DurableFileSystem *files{};
        BuildOutputStore *output{};
        OperationStore *operations{};
        GameplayBuildSessionId nextId{1};
        std::unordered_map<GameplayBuildSessionId, std::shared_ptr<Session>> sessions;
        std::unordered_map<std::string, GameplayBuildSessionId, TransparentStringHash, std::equal_to<>> activeProjects;
        bool shutdown{false};
        bool shutdownFinished{false};
        std::size_t activeAdmissions{};
        std::condition_variable admissionsFinished;
        std::condition_variable shutdownCompleted;

        std::mutex mutex_;
    };

    namespace {
        [[nodiscard]] std::shared_ptr<GameplayBuildService::State::Session> FindSession(
            const std::shared_ptr<GameplayBuildService::State> &state, const GameplayBuildSessionId id) {
            std::lock_guard lock(state->Mutex());
            const auto found = state->sessions.find(id);
            return found == state->sessions.end() ? nullptr : found->second;
        }

        void Update(const std::shared_ptr<GameplayBuildService::State::Session> &session, const GameplayBuildState state, std::string phase,
                    std::optional<Error> error = std::nullopt) {
            std::lock_guard lock(session->Mutex());
            session->snapshot.state = state;
            session->snapshot.phase = std::move(phase);
            session->snapshot.error = std::move(error);
            if (IsTerminal(state))
                session->snapshot.timeoutDeadline.reset();
        }

        void UpdateOperation(GameplayBuildService::State &state, const std::shared_ptr<GameplayBuildService::State::Session> &session,
                             const OperationState operationState, const char *phase, const char *message,
                             const std::optional<float> progress = std::nullopt, std::optional<Error> error = std::nullopt) {
            std::optional<OperationId> id;
            {
                std::lock_guard lock(session->Mutex());
                id = session->snapshot.operationId;
            }
            if (state.operations != nullptr && id.has_value())
                static_cast<void>(state.operations->Update(*id, {operationState, phase, message, progress, std::move(error)}));
        }

        struct OutputBudget {
            std::size_t bytes{};
            std::size_t informationalBytes{};
            std::size_t records{};
            std::size_t informationalRecords{};
            std::size_t diagnosticRecords{};
            std::size_t suppressedBytes{};
            std::size_t suppressedLines{};
        };

        void PublishRecord(GameplayBuildService::State &state, const std::shared_ptr<GameplayBuildService::State::Session> &session,
                           BuildOutputRecord record) {
            if (state.output == nullptr)
                return;
            {
                std::lock_guard lock(session->Mutex());
                record.sessionId = session->outputSessionId;
                record.operationId = session->snapshot.operationId;
            }
            state.output->Append(std::move(record));
        }

        void PublishStage(GameplayBuildService::State &state, const std::shared_ptr<GameplayBuildService::State::Session> &session,
                          const char *stage, const char *code, std::string message,
                          const DiagnosticSeverity severity = DiagnosticSeverity::Note) {
            PublishRecord(state, session,
                          BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                            .severity = severity,
                                            .stage = stage,
                                            .code = DiagnosticCode{code},
                                            .message = std::move(message)});
        }

        void PublishOutput(GameplayBuildService::State &state, const std::shared_ptr<GameplayBuildService::State::Session> &session,
                           OutputBudget &budget, const char *phase, ProcessOutputLine line) {
            constexpr std::size_t MaximumBytes = 8U * 1024U * 1024U;
            constexpr std::size_t MaximumRecords = 8192U;
            constexpr std::size_t DiagnosticRecordReserve = 1024U;
            constexpr std::size_t DiagnosticByteReserve = 1024U * 1024U;
            if (!state.output)
                return;
            const auto diagnostic = ParseCompilerDiagnostic(line.text, session->request.projectRoot, line.truncated);
            std::string message = std::move(line.text);
            if (line.truncated)
                message = std::format("{} … [line truncated]", message);
            BuildOutputRecord record{.timestampUtc = std::chrono::system_clock::now(),
                                     .severity = DiagnosticSeverity::Note,
                                     .stage = phase,
                                     .code = DiagnosticCode{"gameplay.build.output"},
                                     .message = std::move(message)};
            if (diagnostic.has_value()) {
                record.source = diagnostic->source;
                record.severity = diagnostic->severity;
                record.code = DiagnosticCode{diagnostic->severity == DiagnosticSeverity::Error ? "gameplay.build.compiler_error"
                                                                                               : "gameplay.build.compiler_warning"};
                record.toolCode = diagnostic->compilerCode;
            }
            const bool diagnosticRecord = record.source.has_value();
            const bool byteBudgetAvailable = budget.bytes + record.message.size() <= MaximumBytes;
            const bool recordBudgetAvailable = budget.records < MaximumRecords;
            const auto hasClassBudget = [&] {
                if (diagnosticRecord)
                    return budget.diagnosticRecords < DiagnosticRecordReserve;
                return budget.informationalRecords < MaximumRecords - DiagnosticRecordReserve &&
                       budget.informationalBytes + record.message.size() <= MaximumBytes - DiagnosticByteReserve;
            };
            if (const bool classBudgetAvailable = hasClassBudget();
                !byteBudgetAvailable || !recordBudgetAvailable || !classBudgetAvailable) {
                budget.suppressedBytes += record.message.size();
                ++budget.suppressedLines;
                return;
            }
            budget.bytes += record.message.size();
            ++budget.records;
            if (diagnosticRecord)
                ++budget.diagnosticRecords;
            else {
                budget.informationalBytes += record.message.size();
                ++budget.informationalRecords;
            }
            PublishRecord(state, session, std::move(record));
        }

        struct OutputSummaryGuard final {
            GameplayBuildService::State *state{};
            std::shared_ptr<GameplayBuildService::State::Session> session;
            OutputBudget *budget{};

            OutputSummaryGuard(GameplayBuildService::State &owner, std::shared_ptr<GameplayBuildService::State::Session> buildSession,
                               OutputBudget &outputBudget) noexcept
                : state(&owner), session(std::move(buildSession)), budget(&outputBudget) {}

            OutputSummaryGuard(const OutputSummaryGuard &) = delete;
            OutputSummaryGuard &operator=(const OutputSummaryGuard &) = delete;
            OutputSummaryGuard(OutputSummaryGuard &&) = delete;
            OutputSummaryGuard &operator=(OutputSummaryGuard &&) = delete;

            ~OutputSummaryGuard() noexcept {
                try {
                    PublishSummary();
                } catch (const std::exception &error) {
                    // Destructors cannot safely report an allocation failure from diagnostic output.
                    static_cast<void>(error);
                }
            }

            void PublishSummary() const {
                if (state == nullptr || state->output == nullptr || budget == nullptr || budget->suppressedLines == 0)
                    return;
                PublishRecord(*state, session,
                              BuildOutputRecord{
                                  .timestampUtc = std::chrono::system_clock::now(),
                                  .severity = DiagnosticSeverity::Warning,
                                  .stage = "output",
                                  .code = DiagnosticCode{"gameplay.build.output_suppressed"},
                                  .message =
                                      std::format("Suppressed {} build output lines ({} bytes) after the session budget was reached.",
                                                  budget->suppressedLines, budget->suppressedBytes),
                              });
            }
        };

        [[nodiscard]] Result<void> RunPhase(GameplayBuildService::State &state,
                                            const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                            const GameplayBuildState buildState, const char *phase, std::vector<std::string> arguments,
                                            const std::chrono::milliseconds timeout, OutputBudget &budget) {
            Update(session, buildState, phase);
            {
                std::lock_guard lock(session->Mutex());
                session->snapshot.timeoutDeadline = std::chrono::steady_clock::now() + timeout;
            }
            const bool configuring = std::string_view{phase} == "configure";
            UpdateOperation(state, session, OperationState::Running, phase,
                            configuring ? "Configuring gameplay module." : "Building gameplay module.",
                            configuring ? std::optional<float>{0.1F} : std::optional<float>{0.4F});
            PublishStage(state, session, phase, configuring ? "gameplay.build.configure_started" : "gameplay.build.build_started",
                         configuring ? "Configuring gameplay module." : "Building gameplay module.");
            ExternalProcessRequest request;
            request.executable = session->request.environment.cmakeExecutable;
            request.arguments = std::move(arguments);
            request.workingDirectory = session->request.projectRoot;
            request.environment = BuildEnvironment();
            request.timeout = timeout;
            request.gracefulTermination = session->request.timeouts.gracefulTermination;
            request.onOutput = [&state, session, &budget, phase](ProcessOutputLine line) {
                PublishOutput(state, session, budget, phase, std::move(line));
            };
            Result<ExternalProcessResult> executed = state.processes->Run(request, session->cancellation.Token());
            if (executed.HasError())
                return Result<void>::Failure(executed.ErrorValue());
            if (executed.Value().reason == ProcessTerminationReason::TimedOut)
                return Result<void>::Failure(MakeError(TimedOutDescriptor));
            if (executed.Value().reason == ProcessTerminationReason::Cancelled)
                return Result<void>::Failure(MakeError(CancelledDescriptor));

            if (executed.Value().reason != ProcessTerminationReason::Exited || executed.Value().exitCode != 0)
                return Result<void>::Failure(
                    MakeError(BuildFailed, std::format("{} exited with code {}.", phase, executed.Value().exitCode)));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<nlohmann::json> ReadCandidateManifest(const std::filesystem::path &candidate) {
            std::ifstream stream{candidate, std::ios::binary};
            if (!stream)
                return Result<nlohmann::json>::Failure(MakeError(ValidationFailed, "Candidate manifest is missing."));
            try {
                return Result<nlohmann::json>::Success(nlohmann::json::parse(stream, nullptr, true, true));
            } catch (const nlohmann::json::exception &) {
                return Result<nlohmann::json>::Failure(MakeError(ValidationFailed, "Candidate manifest is malformed."));
            }
        }

        [[nodiscard]] Result<std::filesystem::path> ValidateCandidateManifest(const nlohmann::json &manifest) {
            if (manifest.value("schemaVersion", 0) != 1 ||
                manifest.value("buildFingerprint", "") != Gameplay::CurrentGameplayBuildFingerprint() || !manifest.contains("moduleId") ||
                !manifest["moduleId"].is_string() || manifest.value("descriptorRevision", std::uint64_t{0}) == 0 ||
                !manifest.contains("artifactPath") || !manifest["artifactPath"].is_string())
                return Result<std::filesystem::path>::Failure(MakeError(ValidationFailed, "Candidate manifest identity is incompatible."));
            return Result<std::filesystem::path>::Success(manifest["artifactPath"].get<std::string>());
        }

        [[nodiscard]] Result<void> ValidateArtifactLoad(const std::filesystem::path &artifact, const std::filesystem::path &buildRoot,
                                                        const nlohmann::json &manifest) {
            Gameplay::GameModuleHost host;
            auto loaded = host.LoadShadowCopy(artifact, buildRoot / "validation-shadow",
                                              {.moduleId = manifest.value("moduleId", ""),
                                               .buildFingerprint = Gameplay::CurrentGameplayBuildFingerprint(),
                                               .descriptorRevision = manifest.value("descriptorRevision", std::uint64_t{0})});
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            std::unique_ptr<Gameplay::LoadedGameModule> validatedModule = std::move(loaded).Value();
            validatedModule.reset();
            return Result<void>::Success();
        }

        struct PublishedArtifactIdentity {
            std::uintmax_t size{};
            std::int64_t lastWrite{};
            std::string hash;
        };

        [[nodiscard]] Result<PublishedArtifactIdentity> InspectPublishedArtifact(const std::filesystem::path &artifact) {
            Result<std::string> artifactHash = HashFile(artifact, 512U * 1024U * 1024U);
            if (artifactHash.HasError())
                return Result<PublishedArtifactIdentity>::Failure(artifactHash.ErrorValue());
            std::error_code error;
            const std::uintmax_t artifactSize = std::filesystem::file_size(artifact, error);
            const auto artifactWriteTime = std::filesystem::last_write_time(artifact, error);
            if (error)
                return Result<PublishedArtifactIdentity>::Failure(MakeError(ValidationFailed, "Artifact file identity is unavailable."));
            return Result<PublishedArtifactIdentity>::Success(
                {artifactSize, static_cast<std::int64_t>(artifactWriteTime.time_since_epoch().count()), std::move(artifactHash).Value()});
        }

        [[nodiscard]] Result<std::string> HashResolvedInputManifest(const std::filesystem::path &local) {
            std::error_code error;
            if (const std::filesystem::path resolvedInputManifest = local / "gameplay_build_inputs.txt";
                std::filesystem::is_regular_file(resolvedInputManifest, error))
                return HashFile(resolvedInputManifest, 1024U * 1024U);
            return Result<std::string>::Success({});
        }

        [[nodiscard]] Result<nlohmann::json> BuildCompilerIdentityDocument(const GameplayBuildRequest &request) {
            if (!request.environment.cxxCompiler.has_value())
                return Result<nlohmann::json>::Success(nullptr);
            Result<CompilerIdentity> identity = ResolveCompilerIdentity(*request.environment.cxxCompiler);
            if (identity.HasError())
                return Result<nlohmann::json>::Failure(identity.ErrorValue());
            return Result<nlohmann::json>::Success({{"canonicalPath", identity.Value().canonicalPath.string()},
                                                    {"nativeFileIdentity", identity.Value().nativeFileIdentity},
                                                    {"size", identity.Value().size},
                                                    {"lastWrite", identity.Value().lastWrite},
                                                    {"binarySha256", identity.Value().binaryHash}});
        }

        struct SuccessfulStateInputs {
            const GameplayBuildRequest &request;
            std::string_view inputHash;
            nlohmann::json compiler;
            std::string_view resolvedInputManifestHash;
            const std::filesystem::path &artifact;
            const PublishedArtifactIdentity &artifactIdentity;
            std::string_view manifestHash;
            const nlohmann::json &manifest;
        };

        [[nodiscard]] nlohmann::json BuildSuccessfulStateDocument(const SuccessfulStateInputs &inputs) {
            const nlohmann::json toolchain{{"generator", inputs.request.environment.generator.value_or("")},
                                           {"generatorPlatform", inputs.request.environment.generatorPlatform.value_or("")},
                                           {"generatorToolset", inputs.request.environment.generatorToolset.value_or("")},
                                           {"toolchainFile",
                                            inputs.request.environment.toolchainFile.value_or(std::filesystem::path{}).string()},
                                           {"configuration", inputs.request.environment.configuration}};
            return {{"schemaVersion", 1},
                    {"successfulInputHash", inputs.inputHash},
                    {"sdkFingerprint", Gameplay::CurrentGameplayBuildFingerprint()},
                    {"compiler", inputs.compiler},
                    {"toolchain", toolchain},
                    {"configuration", inputs.request.environment.configuration},
                    {"resolvedInputManifestHash", inputs.resolvedInputManifestHash},
                    {"artifactIdentity",
                     {{"path", inputs.artifact.string()},
                      {"size", inputs.artifactIdentity.size},
                      {"lastWrite", inputs.artifactIdentity.lastWrite},
                      {"sha256", inputs.artifactIdentity.hash}}},
                    {"moduleManifestHash", inputs.manifestHash},
                    {"descriptorRevision", inputs.manifest.value("descriptorRevision", 0)}};
        }

        [[nodiscard]] Result<void> PublishValidatedState(GameplayBuildService::State &state, const std::filesystem::path &candidate,
                                                         const std::filesystem::path &local, const std::string_view encodedState) {
            const std::filesystem::path manifestTemporary = local / "gameplay_module.json.next";
            if (Result<void> copied = state.files->CopyDurable(candidate, manifestTemporary); copied.HasError())
                return copied;
            const std::filesystem::path stateTemporary = local / "gameplay_build_state.json.next";
            if (Result<void> stateWritten = state.files->WriteDurable(stateTemporary, std::as_bytes(std::span{encodedState}));
                stateWritten.HasError())
                return stateWritten;

            std::error_code error;
            const std::filesystem::path publishedManifest = local / "gameplay_module.json";
            const std::filesystem::path manifestBackup = local / "gameplay_module.json.previous";
            const bool hadPublishedManifest = std::filesystem::is_regular_file(publishedManifest, error);
            if (hadPublishedManifest) {
                Result<void> backedUp = state.files->CopyDurable(publishedManifest, manifestBackup);
                if (backedUp.HasError())
                    return backedUp;
            }
            if (Result<void> published = state.files->AtomicReplace(manifestTemporary, publishedManifest); published.HasError())
                return published;
            if (Result<void> statePublished = state.files->AtomicReplace(stateTemporary, local / "gameplay_build_state.json");
                statePublished.HasError()) {
                if (hadPublishedManifest)
                    static_cast<void>(state.files->AtomicReplace(manifestBackup, publishedManifest));
                else
                    static_cast<void>(state.files->RemoveDurable(publishedManifest));
                return statePublished;
            }
            if (hadPublishedManifest)
                static_cast<void>(state.files->RemoveDurable(manifestBackup));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAndPublish(GameplayBuildService::State &state,
                                                      const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                      const std::string_view inputHash) {
            Update(session, GameplayBuildState::Validating, "validate");
            UpdateOperation(state, session, OperationState::Running, "validate", "Validating gameplay artifact.", 0.8F);
            PublishStage(state, session, "validate", "gameplay.build.validation_started", "Validating gameplay artifact.");
            const std::filesystem::path buildRoot = session->request.projectRoot / ".horo/local/build/gameplay-debug";
            const std::filesystem::path candidate = buildRoot / "candidate_gameplay_module.json";
            Result<nlohmann::json> manifest = ReadCandidateManifest(candidate);
            if (manifest.HasError())
                return Result<void>::Failure(manifest.ErrorValue());
            Result<std::filesystem::path> artifact = ValidateCandidateManifest(manifest.Value());
            if (artifact.HasError())
                return Result<void>::Failure(artifact.ErrorValue());
            if (Result<void> loaded = ValidateArtifactLoad(artifact.Value(), buildRoot, manifest.Value()); loaded.HasError())
                return loaded;

            Result<std::string> postHash = ComputeInputHash(session->request);
            if (postHash.HasError())
                return Result<void>::Failure(postHash.ErrorValue());
            if (postHash.Value() != inputHash)
                return Result<void>::Failure(MakeError(BuildInputsChanged, "Gameplay sources changed during the build."));

            const std::filesystem::path local = session->request.projectRoot / ".horo/local";
            Result<std::string> manifestHash = HashFile(candidate, 1024U * 1024U);
            if (manifestHash.HasError())
                return Result<void>::Failure(manifestHash.ErrorValue());
            Result<PublishedArtifactIdentity> artifactIdentity = InspectPublishedArtifact(artifact.Value());
            if (artifactIdentity.HasError())
                return Result<void>::Failure(artifactIdentity.ErrorValue());
            Result<std::string> resolvedInputManifestHash = HashResolvedInputManifest(local);
            if (resolvedInputManifestHash.HasError())
                return Result<void>::Failure(resolvedInputManifestHash.ErrorValue());
            Result<nlohmann::json> compiler = BuildCompilerIdentityDocument(session->request);
            if (compiler.HasError())
                return Result<void>::Failure(compiler.ErrorValue());
            const nlohmann::json buildState =
                BuildSuccessfulStateDocument({session->request, inputHash, std::move(compiler).Value(), resolvedInputManifestHash.Value(),
                                              artifact.Value(), artifactIdentity.Value(), manifestHash.Value(), manifest.Value()});
            return PublishValidatedState(state, candidate, local, std::format("{}\n", buildState.dump(2)));
        }

        [[nodiscard]] std::string ReadExternalLockOwner(const std::filesystem::path &lockPath) {
            std::ifstream metadata{lockPath, std::ios::binary};
            if (!metadata)
                return {};
            std::array<char, 512> ownerBuffer{};
            metadata.read(ownerBuffer.data(), static_cast<std::streamsize>(ownerBuffer.size()));
            const std::streamsize bytesRead = metadata.gcount();
            return bytesRead > 0 ? std::string{ownerBuffer.data(), static_cast<std::size_t>(bytesRead)} : std::string{};
        }

        [[nodiscard]] Result<ExclusiveFileLock> AcquireBuildLock(GameplayBuildService::State &state,
                                                                 const std::shared_ptr<GameplayBuildService::State::Session> &session) {
            Update(session, GameplayBuildState::AcquiringLock, "lock");
            UpdateOperation(state, session, OperationState::Running, "lock", "Acquiring gameplay build lock.", 0.05F);
            const std::filesystem::path lockPath = session->request.projectRoot / ".horo/local/locks/gameplay-build.lock";
            const auto waitStarted = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(session->Mutex());
                session->snapshot.timeoutDeadline = waitStarted + session->request.timeouts.externalWait;
            }
            bool waitingRecordPublished = false;
            for (;;) {
                if (session->cancellation.Token().IsCancellationRequested())
                    return Result<ExclusiveFileLock>::Failure(MakeError(CancelledDescriptor));
                const std::string owner =
                    std::format("pid={};started={};session={}", CurrentProcessId(), ProcessStartedAtSeconds(), session->snapshot.id);
                Result<ExclusiveFileLock> acquired = state.files->TryAcquireExclusive(lockPath, owner);
                if (acquired.HasValue()) {
                    Update(session, GameplayBuildState::AcquiringLock, "lock");
                    UpdateOperation(state, session, OperationState::Running, "lock", "Gameplay build lock acquired.", 0.05F);
                    std::lock_guard lock(session->Mutex());
                    session->snapshot.externalLockOwner.clear();
                    return acquired;
                }
                if (acquired.ErrorValue().code.Value() != "filesystem.lock_busy")
                    return Result<ExclusiveFileLock>::Failure(acquired.ErrorValue());

                Update(session, GameplayBuildState::WaitingForExternalBuild, "waiting_external_build");
                std::string externalOwner = ReadExternalLockOwner(lockPath);
                const std::string waitingMessage = externalOwner.empty()
                                                       ? "Waiting for external gameplay build lock held by an unknown owner."
                                                       : std::format("Waiting for external gameplay build lock held by {}.", externalOwner);
                UpdateOperation(state, session, OperationState::Waiting, "waiting_external_build", waitingMessage.c_str(), 0.05F);
                if (!waitingRecordPublished) {
                    PublishStage(state, session, "lock", "gameplay.build.waiting_for_external_lock", waitingMessage);
                    waitingRecordPublished = true;
                }
                {
                    std::lock_guard lock(session->Mutex());
                    session->snapshot.externalLockOwner = std::move(externalOwner);
                }
                if (std::chrono::steady_clock::now() - waitStarted >= session->request.timeouts.externalWait)
                    return Result<ExclusiveFileLock>::Failure(
                        MakeError(TimedOutDescriptor, "Timed out waiting for the project gameplay build lock."));
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }

        [[nodiscard]] bool AdoptPendingRequest(const std::shared_ptr<GameplayBuildService::State::Session> &session) {
            std::lock_guard lock(session->Mutex());
            if (!session->pendingRequest.has_value())
                return false;
            session->request = std::move(*session->pendingRequest);
            session->pendingRequest.reset();
            session->snapshot.pendingInputHash.reset();
            session->snapshot.newerInputsPending = false;
            return true;
        }

        [[nodiscard]] bool HasSupersedingRequest(const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                 const std::string_view activeHash) {
            std::lock_guard lock(session->Mutex());
            return session->pendingRequest.has_value() && session->snapshot.pendingInputHash != activeHash;
        }

        [[nodiscard]] bool RecordSupersedingInputs(const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                   const std::string_view successorHash) {
            std::lock_guard lock(session->Mutex());
            if (session->pendingRequest.has_value())
                return false;
            session->snapshot.newerInputsPending = true;
            session->snapshot.pendingInputHash = successorHash;
            return true;
        }

        [[nodiscard]] Result<std::string> PrepareActiveInput(const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                             std::optional<std::string> reusableHash) {
            const bool adoptedPendingRequest = AdoptPendingRequest(session);
            Result<std::string> hash = !adoptedPendingRequest && reusableHash.has_value()
                                           ? Result<std::string>::Success(std::move(*reusableHash))
                                           : ComputeInputHash(session->request);
            if (hash.HasError())
                return hash;
            {
                std::lock_guard lock(session->Mutex());
                session->snapshot.activeInputHash = hash.Value();
                if (!adoptedPendingRequest && session->snapshot.pendingInputHash == hash.Value()) {
                    session->snapshot.pendingInputHash.reset();
                    session->snapshot.newerInputsPending = false;
                }
                if (!session->snapshot.pendingInputHash.has_value())
                    session->snapshot.desiredInputHash = hash.Value();
            }
            return hash;
        }

        [[nodiscard]] Result<bool> TryUseCachedBuild(GameplayBuildService::State &state,
                                                     const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                     const std::string_view inputHash) {
            if (ReadSuccessfulHash(session->request.projectRoot) != inputHash ||
                !std::filesystem::is_regular_file(session->request.projectRoot / ".horo/local/gameplay_module.json"))
                return Result<bool>::Success(false);
            UpdateOperation(state, session, OperationState::Running, "cache", "Gameplay module is up to date; build skipped.", 0.9F);
            PublishStage(state, session, "cache", "gameplay.build.cache_hit",
                         "Gameplay module is up to date; configure and build were skipped.");
            {
                std::lock_guard lock(session->Mutex());
                session->cacheHit = true;
            }
            return Result<bool>::Success(true);
        }

        [[nodiscard]] Result<bool> RunBuildCycle(GameplayBuildService::State &state,
                                                 const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                 const std::string_view inputHash, OutputBudget &budget,
                                                 std::optional<std::string> &retryHash) {
            const std::filesystem::path buildRoot = session->request.projectRoot / ".horo/local/build/gameplay-debug";
            const std::filesystem::path candidateManifest = buildRoot / "candidate_gameplay_module.json";
            if (Result<void> configured = RunPhase(state, session, GameplayBuildState::Configuring, "configure",
                                                   BuildConfigureArguments(session->request, buildRoot, candidateManifest),
                                                   session->request.timeouts.configure, budget);
                configured.HasError())
                return Result<bool>::Failure(configured.ErrorValue());
            if (Result<void> built = RunPhase(state, session, GameplayBuildState::Building, "build",
                                              {"--build", buildRoot.string(), "--target", "HoroGameGameplay", "--config",
                                               session->request.environment.configuration, "--parallel"},
                                              session->request.timeouts.build, budget);
                built.HasError())
                return Result<bool>::Failure(built.ErrorValue());
            if (HasSupersedingRequest(session, inputHash))
                return Result<bool>::Success(true);
            Result<void> validated = ValidateAndPublish(state, session, inputHash);
            if (validated.HasValue())
                return Result<bool>::Success(false);
            if (validated.ErrorValue().code.Value() != BuildInputsChanged.code.Value())
                return Result<bool>::Failure(validated.ErrorValue());
            Result<std::string> successorHash = ComputeInputHash(session->request);
            if (successorHash.HasError())
                return Result<bool>::Failure(successorHash.ErrorValue());
            if (const bool recorded = RecordSupersedingInputs(session, successorHash.Value()); recorded)
                retryHash = std::move(successorHash).Value();
            return Result<bool>::Success(true);
        }

        Result<void> RunBuildWithLock(const std::shared_ptr<GameplayBuildService::State> &state,
                                      const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                      [[maybe_unused]] ExclusiveFileLock buildLock) {
            OutputBudget budget;
            OutputSummaryGuard outputSummary{*state, session, budget};
            std::optional<std::string> retryHash;
            for (;;) {
                Result<std::string> hash = PrepareActiveInput(session, std::move(retryHash));
                retryHash.reset();
                if (hash.HasError())
                    return Result<void>::Failure(hash.ErrorValue());
                Result<bool> cached = TryUseCachedBuild(*state, session, hash.Value());
                if (cached.HasError())
                    return Result<void>::Failure(cached.ErrorValue());
                if (cached.Value())
                    return Result<void>::Success();
                Result<bool> cycle = RunBuildCycle(*state, session, hash.Value(), budget, retryHash);
                if (cycle.HasError())
                    return Result<void>::Failure(cycle.ErrorValue());
                if (!cycle.Value())
                    return Result<void>::Success();
            }
        }

        Result<void> RunBuild(const std::shared_ptr<GameplayBuildService::State> &state,
                              const std::shared_ptr<GameplayBuildService::State::Session> &session) {
            if (Result<ExclusiveFileLock> buildLock = AcquireBuildLock(*state, session); buildLock.HasError())
                return Result<void>::Failure(buildLock.ErrorValue());
            else
                return RunBuildWithLock(state, session, std::move(buildLock).Value());
        }

        struct SessionPreparation {
            std::shared_ptr<GameplayBuildService::State::Session> session;
            std::optional<GameplayBuildSessionId> joinedSessionId;
        };

        [[nodiscard]] Result<SessionPreparation> PrepareSession(const std::shared_ptr<GameplayBuildService::State> &state,
                                                                GameplayBuildRequest &request, const std::string_view inputHash,
                                                                const std::string_view projectKey) {
            std::lock_guard lock(state->Mutex());
            if (state->shutdown)
                return Result<SessionPreparation>::Failure(MakeError(CancelledDescriptor, "Gameplay build service is shut down."));
            if (const auto active = state->activeProjects.find(projectKey); active != state->activeProjects.end()) {
                std::shared_ptr<GameplayBuildService::State::Session> session = state->sessions.at(active->second);
                std::lock_guard sessionLock(session->Mutex());
                if (!IsTerminal(session->snapshot.state)) {
                    ++session->snapshot.coalescedRequestCount;
                    if (session->snapshot.desiredInputHash != inputHash) {
                        session->pendingRequest = request;
                        session->snapshot.pendingInputHash = inputHash;
                        session->snapshot.newerInputsPending = true;
                        session->snapshot.desiredInputHash = inputHash;
                    }
                    return Result<SessionPreparation>::Success({session, session->snapshot.id});
                }
                state->activeProjects.erase(active);
            }

            auto session = std::make_shared<GameplayBuildService::State::Session>();
            session->snapshot.id = state->nextId++;
            if (state->output != nullptr) {
                session->outputSessionId = state->output->BeginSession();
                if (!session->outputSessionId.has_value())
                    return Result<SessionPreparation>::Failure(MakeError(BuildFailed, "Build-output session identity space is exhausted."));
            }
            session->snapshot.activeInputHash = inputHash;
            session->snapshot.desiredInputHash = inputHash;
            session->request = std::move(request);
            state->sessions.try_emplace(session->snapshot.id, session);
            state->activeProjects.try_emplace(std::string{projectKey}, session->snapshot.id);
            return Result<SessionPreparation>::Success({std::move(session), std::nullopt});
        }

        void RequestSessionCancellation(const std::weak_ptr<GameplayBuildService::State> &weakState, const GameplayBuildSessionId id) {
            const std::shared_ptr<GameplayBuildService::State> state = weakState.lock();
            if (!state)
                return;
            std::shared_ptr<GameplayBuildService::State::Session> target;
            {
                std::lock_guard lock(state->Mutex());
                if (const auto found = state->sessions.find(id); found != state->sessions.end())
                    target = found->second;
            }
            if (target) {
                std::lock_guard transitionLock(target->transitionMutex);
                std::string phase;
                {
                    std::lock_guard lock(target->Mutex());
                    if (IsTerminal(target->snapshot.state))
                        return;
                    target->cancellation.RequestCancellation();
                    phase = target->snapshot.phase;
                    target->pendingRequest.reset();
                    target->snapshot.pendingInputHash.reset();
                    target->snapshot.newerInputsPending = false;
                }
                UpdateOperation(*state, target, OperationState::Cancelling, phase.c_str(), "Gameplay build cancellation requested.");
            }
        }

        [[nodiscard]] Result<void> BeginBuildOperation(GameplayBuildService::State &state,
                                                       const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                       const std::weak_ptr<GameplayBuildService::State> &weakState) {
            if (state.operations == nullptr)
                return Result<void>::Success();
            const GameplayBuildSessionId id = session->snapshot.id;
            if (std::optional<OperationId> operationId = state.operations->Begin({OperationKind::Build, "Build gameplay module", "queued",
                                                                                  "Waiting for a build worker.", std::nullopt, true,
                                                                                  [weakState, id] {
                RequestSessionCancellation(weakState, id);
            }});
                operationId.has_value()) {
                std::lock_guard lock(session->Mutex());
                session->snapshot.operationId = *operationId;
                return Result<void>::Success();
            }
            return Result<void>::Failure(MakeError(OperationAdmissionFailed));
        }

        [[nodiscard]] GameplayBuildState TerminalStateFor(const Result<void> &result) {
            using enum GameplayBuildState;
            if (result.HasValue())
                return Succeeded;
            if (result.ErrorValue().code.Value() == TimedOutDescriptor.code.Value())
                return TimedOut;
            if (result.ErrorValue().code.Value() == CancelledDescriptor.code.Value())
                return Cancelled;
            return Failed;
        }

        struct BuildTerminalOutput {
            BuildOutputResult result{BuildOutputResult::Succeeded};
            DiagnosticSeverity severity{DiagnosticSeverity::Note};
            DiagnosticCode code{"gameplay.build.succeeded"};
            std::string message{"Gameplay module built successfully."};
            const char *stage{"complete"};
        };

        [[nodiscard]] BuildTerminalOutput MakeTerminalOutput(const Result<void> &result, const GameplayBuildState terminal,
                                                             const bool cacheHit) {
            if (result.HasValue() && cacheHit)
                return {BuildOutputResult::Cached, DiagnosticSeverity::Note, DiagnosticCode{"gameplay.build.cached"},
                        "Gameplay module was already up to date; no external build was required.", "complete"};
            if (result.HasValue())
                return {};

            BuildTerminalOutput output{.result = BuildOutputResult::Failed,
                                       .severity = DiagnosticSeverity::Error,
                                       .code = DiagnosticCode{"gameplay.build.failed"},
                                       .message = result.ErrorValue().message,
                                       .stage = "terminal"};
            if (terminal == GameplayBuildState::Cancelled) {
                output.result = BuildOutputResult::Cancelled;
                output.severity = DiagnosticSeverity::Warning;
                output.code = DiagnosticCode{"gameplay.build.cancelled"};
            } else if (terminal == GameplayBuildState::TimedOut) {
                output.result = BuildOutputResult::TimedOut;
                output.code = DiagnosticCode{"gameplay.build.timed_out"};
            }
            return output;
        }

        void UpdateTerminalOperation(GameplayBuildService::State &state,
                                     const std::shared_ptr<GameplayBuildService::State::Session> &session, const Result<void> &result,
                                     const GameplayBuildState terminal, const bool cacheHit) {
            if (result.HasError()) {
                const OperationState operationState =
                    terminal == GameplayBuildState::Cancelled ? OperationState::Cancelled : OperationState::Failed;
                UpdateOperation(state, session, operationState, "terminal", result.ErrorValue().message.c_str(), std::nullopt,
                                result.ErrorValue());
                return;
            }
            UpdateOperation(state, session, OperationState::Succeeded, "complete",
                            cacheHit ? "Gameplay module was already up to date." : "Gameplay module built successfully.", 1.0F);
        }

        void RemoveActiveProject(const std::shared_ptr<GameplayBuildService::State> &state, const std::string_view projectKey,
                                 const GameplayBuildSessionId sessionId) {
            std::lock_guard lock(state->Mutex());
            if (const auto active = state->activeProjects.find(projectKey);
                active != state->activeProjects.end() && active->second == sessionId)
                state->activeProjects.erase(active);
        }

        [[nodiscard]] Result<void> CompleteBuild(const std::shared_ptr<GameplayBuildService::State> &state,
                                                 const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                 const std::string_view projectKey) {
            Result<void> result = RunBuild(state, session);
            std::lock_guard transitionLock(session->transitionMutex);
            const GameplayBuildState terminal = TerminalStateFor(result);
            bool cacheHit = false;
            {
                std::lock_guard lock(session->Mutex());
                cacheHit = session->cacheHit;
            }
            BuildTerminalOutput output = MakeTerminalOutput(result, terminal, cacheHit);
            PublishRecord(*state, session,
                          BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                            .severity = output.severity,
                                            .result = output.result,
                                            .stage = output.stage,
                                            .code = std::move(output.code),
                                            .message = std::move(output.message)});
            UpdateTerminalOperation(*state, session, result, terminal, cacheHit);
            Update(session, terminal, result.HasValue() ? "complete" : "terminal",
                   result.HasError() ? std::optional<Error>{result.ErrorValue()} : std::nullopt);
            RemoveActiveProject(state, projectKey, session->snapshot.id);
            return result;
        }

        void RemoveUnsubmittedSession(const std::shared_ptr<GameplayBuildService::State> &state,
                                      const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                      const std::string_view projectKey) {
            std::lock_guard lock(state->Mutex());
            state->sessions.erase(session->snapshot.id);
            if (const auto active = state->activeProjects.find(projectKey);
                active != state->activeProjects.end() && active->second == session->snapshot.id)
                state->activeProjects.erase(active);
        }

        /** @brief Publishes a failed job admission before removing the unsubmitted session. */
        Result<GameplayBuildSessionId> FailJobSubmission(const std::shared_ptr<GameplayBuildService::State> &state,
                                                         const std::shared_ptr<GameplayBuildService::State::Session> &session,
                                                         const std::string_view projectKey, const Error &error) {
            PublishRecord(*state, session,
                          BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                            .severity = DiagnosticSeverity::Error,
                                            .result = BuildOutputResult::Failed,
                                            .stage = "terminal",
                                            .code = DiagnosticCode{"gameplay.build.failed"},
                                            .message = error.message});
            UpdateOperation(*state, session, OperationState::Failed, "terminal", error.message.c_str(), std::nullopt, error);
            Update(session, GameplayBuildState::Failed, "terminal", error);
            RemoveUnsubmittedSession(state, session, projectKey);
            return Result<GameplayBuildSessionId>::Failure(error);
        }
    }  // namespace

    GameplayBuildService::GameplayBuildService(IExternalProcessRunner &processes, JobSystem &jobs, DurableFileSystem &files,
                                               BuildOutputStore *output, OperationStore *operations)
        : state_(std::make_shared<State>()) {
        state_->processes = &processes;
        state_->jobs = &jobs;
        state_->files = &files;
        state_->output = output;
        state_->operations = operations;
    }

    GameplayBuildService::~GameplayBuildService() {
        Shutdown();
    }

    Result<GameplayBuildSessionId> GameplayBuildService::Start(GameplayBuildRequest request) const {
        Result<std::string> hash = ComputeInputHash(request);
        if (hash.HasError())
            return Result<GameplayBuildSessionId>::Failure(hash.ErrorValue());
        {
            std::lock_guard lock(state_->Mutex());
            if (state_->shutdown)
                return Result<GameplayBuildSessionId>::Failure(MakeError(CancelledDescriptor, "Gameplay build service is shut down."));
            ++state_->activeAdmissions;
        }

        struct AdmissionGuard final {
            State &state;

            explicit AdmissionGuard(State &target) : state(target) {}

            AdmissionGuard(const AdmissionGuard &) = delete;
            AdmissionGuard &operator=(const AdmissionGuard &) = delete;
            AdmissionGuard(AdmissionGuard &&) = delete;
            AdmissionGuard &operator=(AdmissionGuard &&) = delete;

            ~AdmissionGuard() {
                std::lock_guard lock(state.Mutex());
                --state.activeAdmissions;
                state.admissionsFinished.notify_all();
            }
        };

        AdmissionGuard admission{*state_};

        const std::string projectKey = std::filesystem::absolute(request.projectRoot).lexically_normal().generic_string();
        Result<SessionPreparation> prepared = PrepareSession(state_, request, hash.Value(), projectKey);
        if (prepared.HasError())
            return Result<GameplayBuildSessionId>::Failure(prepared.ErrorValue());
        if (prepared.Value().joinedSessionId.has_value())
            return Result<GameplayBuildSessionId>::Success(*prepared.Value().joinedSessionId);
        const std::shared_ptr<State::Session> session = prepared.Value().session;
        if (Result<void> admitted = BeginBuildOperation(*state_, session, state_); admitted.HasError()) {
            RemoveUnsubmittedSession(state_, session, projectKey);
            return Result<GameplayBuildSessionId>::Failure(admitted.ErrorValue());
        }
        PublishStage(*state_, session, "started", "gameplay.build.started", "Gameplay build started.");

        Result<JobHandle> submitted = state_->jobs->SubmitResult({}, [state = state_, session, projectKey](const CancellationToken &) {
            return CompleteBuild(state, session, projectKey);
        });
        if (submitted.HasError())
            return FailJobSubmission(state_, session, projectKey, submitted.ErrorValue());
        {
            std::lock_guard lock(session->Mutex());
            session->jobId = submitted.Value().Id();
            session->job = std::make_shared<JobHandle>(std::move(submitted).Value());
        }
        return Result<GameplayBuildSessionId>::Success(session->snapshot.id);
    }

    std::optional<GameplayBuildSnapshot> GameplayBuildService::Query(const GameplayBuildSessionId id) const {
        const std::shared_ptr<State::Session> session = FindSession(state_, id);
        if (!session)
            return std::nullopt;
        std::lock_guard lock(session->Mutex());
        return session->snapshot;
    }

    bool GameplayBuildService::RequestCancel(const GameplayBuildSessionId id) const {
        const std::shared_ptr<State::Session> session = FindSession(state_, id);
        if (!session)
            return false;
        std::lock_guard transitionLock(session->transitionMutex);
        std::string phase;
        {
            std::lock_guard lock(session->Mutex());
            if (IsTerminal(session->snapshot.state))
                return false;
            session->cancellation.RequestCancellation();
            session->pendingRequest.reset();
            session->snapshot.pendingInputHash.reset();
            session->snapshot.newerInputsPending = false;
            phase = session->snapshot.phase;
        }
        UpdateOperation(*state_, session, OperationState::Cancelling, phase.c_str(), "Gameplay build cancellation requested.");
        return true;
    }

    bool GameplayBuildService::IsUpToDate(const GameplayBuildRequest &request) const {
        const Result<std::string> hash = ComputeInputHash(request);
        return hash.HasValue() && ReadSuccessfulHash(request.projectRoot) == hash.Value() &&
               std::filesystem::is_regular_file(request.projectRoot / ".horo/local/gameplay_module.json");
    }

    void GameplayBuildService::Shutdown() const noexcept {
        std::vector<std::shared_ptr<State::Session>> sessions;
        {
            std::unique_lock lock(state_->Mutex());
            if (state_->shutdown) {
                state_->shutdownCompleted.wait(lock, [this] {
                    return state_->shutdownFinished;
                });
                return;
            }
            state_->shutdown = true;
            state_->admissionsFinished.wait(lock, [this] {
                return state_->activeAdmissions == 0;
            });
            for (const auto &[sessionId, session] : state_->sessions) {
                static_cast<void>(sessionId);
                sessions.push_back(session);
            }
        }
        for (const auto &session : sessions) {
            std::lock_guard lock(session->Mutex());
            if (!IsTerminal(session->snapshot.state))
                session->cancellation.RequestCancellation();
        }
        for (const auto &session : sessions) {
            std::shared_ptr<JobHandle> job;
            {
                std::lock_guard lock(session->Mutex());
                job = session->job;
            }
            if (job)
                static_cast<void>(job->Wait());
        }
        {
            std::lock_guard lock(state_->Mutex());
            state_->shutdownFinished = true;
        }
        state_->shutdownCompleted.notify_all();
    }
}  // namespace Horo::Application
