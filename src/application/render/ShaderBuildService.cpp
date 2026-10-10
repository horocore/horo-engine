#include "Horo/Application/ShaderBuildService.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace Horo::Application {
    namespace ShaderBuildErrors {
        const ErrorCodeDescriptor OwnerThread{ErrorDomainId{"application.shader_build"}, ErrorCode{"application.shader_build.owner_thread"},
                                              ErrorSeverity::Error, "Shader compilation requires a host worker.",
                                              "Schedule compilation outside editor/render owner callbacks."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"application.shader_build"}, ErrorCode{"application.shader_build.closed"},
                                         ErrorSeverity::Warning, "Shader compilation admission is closed.",
                                         "Retry in a live host session."};
        const ErrorCodeDescriptor Capacity{ErrorDomainId{"application.shader_build"}, ErrorCode{"application.shader_build.capacity"},
                                           ErrorSeverity::Warning, "The concurrent shader compilation limit is reached.",
                                           "Retry after an admitted compilation finishes."};
        const ErrorCodeDescriptor InvalidSources{ErrorDomainId{"application.shader_build"},
                                                 ErrorCode{"application.shader_build.invalid_sources"}, ErrorSeverity::Error,
                                                 "Shader diagnostic navigation mappings are invalid.",
                                                 "Provide unique bounded logical identities and absolute paths."};
    }  // namespace ShaderBuildErrors

    namespace {
        using namespace Render;

        /** @brief Releases one admitted slot exactly once while service-owned synchronization remains alive. */
        class AdmissionGuard final {
        public:
            AdmissionGuard(std::mutex &mutex, std::condition_variable &drained, std::optional<CancellationSource> &slot)
                : mutex_(mutex), drained_(drained), slot_(slot) {}

            AdmissionGuard(const AdmissionGuard &) = delete;
            AdmissionGuard &operator=(const AdmissionGuard &) = delete;
            AdmissionGuard(AdmissionGuard &&) = delete;
            AdmissionGuard &operator=(AdmissionGuard &&) = delete;

            ~AdmissionGuard() {
                std::lock_guard lock(mutex_);
                slot_.reset();
                drained_.notify_all();
            }

        private:
            std::mutex &mutex_;
            std::condition_variable &drained_;
            std::optional<CancellationSource> &slot_;
        };

        static_assert(!std::is_copy_constructible_v<AdmissionGuard> && !std::is_move_constructible_v<AdmissionGuard>);

        /** @brief Stable phase labels independent of localized presentation. */
        std::string_view PhaseName(const ShaderCompilerPhase phase) {
            using enum ShaderCompilerPhase;
            switch (phase) {
                case PipelineValidation:
                    return "validation";
                case SourceCompilation:
                    return "compile";
                case IntermediateValidation:
                    return "intermediate-validation";
                case Translation:
                    return "translate";
                case NativeCompilation:
                    return "native-compile";
                case NativeLink:
                    return "native-link";
                case DebugCompilation:
                    return "debug-compile";
            }
            return "unknown";
        }

        /** @brief Copies compiler provenance without inferring a stage from diagnostic text. */
        std::string StageLabel(const std::optional<ShaderCompilerDiagnosticContext> &context) {
            if (!context)
                return "shader/diagnostic";
            std::string label = std::format("shader/{} target={} revision={} entry={}", PhaseName(context->phase),
                                            static_cast<unsigned>(context->backend), context->sourceRevision, context->entryPoint);
            if (context->shaderStage)
                label += std::format(" stage={}", static_cast<unsigned>(*context->shaderStage));
            if (context->tool)
                label += std::format(" tool={} release={} build={}", static_cast<unsigned>(context->tool->tool), context->tool->release,
                                     FormatSha256(context->tool->buildDigest));
            return label;
        }

        /** @brief Converts independent compiler severity without changing terminal outcome. */
        DiagnosticSeverity Severity(const ShaderCompilerDiagnosticSeverity severity) {
            switch (severity) {
                case ShaderCompilerDiagnosticSeverity::Information:
                    return DiagnosticSeverity::Note;
                case ShaderCompilerDiagnosticSeverity::Warning:
                    return DiagnosticSeverity::Warning;
                case ShaderCompilerDiagnosticSeverity::Error:
                    return DiagnosticSeverity::Error;
            }
            return DiagnosticSeverity::Error;
        }

        /** @brief Stable Horo categories remain separate from optional tool-native codes. */
        std::string_view DiagnosticIdentity(const ShaderCompilerDiagnosticCategory category) {
            using enum ShaderCompilerDiagnosticCategory;
            switch (category) {
                case Source:
                    return "render.shader_compiler.source";
                case Include:
                    return "render.shader_compiler.include";
                case UnsupportedFeature:
                    return "render.shader_compiler.unsupported_feature";
                case Toolchain:
                    return "render.shader_compiler.toolchain";
                case Validation:
                    return "render.shader_compiler.validation";
            }
            return "render.shader_compiler.invalid_diagnostic";
        }

        /** @brief Publishes synchronously; each record owns its correlation and mapped navigation values. */
        class OutputSink final : public IShaderCompilerDiagnosticSink {
        public:
            OutputSink(BuildOutputStore &store, const BuildOutputSessionId session, const ShaderBuildRequest &request)
                : store_(store), session_(session), request_(request) {}

            Result<void> BeginPhase(const ShaderCompilerDiagnosticContext &context) override {
                Append(DiagnosticSeverity::Note, BuildOutputResult::None, "render.shader_compiler.phase", StageLabel(context),
                       "Shader compiler phase started.");
                return Result<void>::Success();
            }

            Result<void> Publish(const ShaderCompilerDiagnostic &diagnostic) override {
                BuildOutputRecord record =
                    Base(Severity(diagnostic.severity), BuildOutputResult::None, DiagnosticIdentity(diagnostic.category),
                         StageLabel(diagnostic.context), diagnostic.message);
                if (!diagnostic.sourceIdentity.empty())
                    record.stage += " source=" + diagnostic.sourceIdentity;
                record.toolCode = diagnostic.toolCode;
                if (diagnostic.truncated)
                    record.message += " [compiler output truncated]";
                if (const auto mapping =
                        std::ranges::find(request_.sources, diagnostic.sourceIdentity, &ShaderDiagnosticSource::logicalPath);
                    mapping != request_.sources.end() && diagnostic.line != 0)
                    record.source = DiagnosticSourceLocation{mapping->absolutePath.string(), diagnostic.line, diagnostic.column};
                store_.Append(std::move(record));
                return Result<void>::Success();
            }

            void Finish(const Result<ShaderCompilationBatch> &result) {
                using enum DiagnosticSeverity;
                using enum BuildOutputResult;
                if (result.HasValue()) {
                    Append(Note, Succeeded, "render.shader_compiler.succeeded", "shader", "Shader compilation succeeded.");
                    return;
                }
                const Horo::Error &error = result.ErrorValue();
                const bool cancelled = ErrorChainContains(error, ShaderCompilerPipelineErrors::CancellationRequested.domain,
                                                          ShaderCompilerPipelineErrors::CancellationRequested.code);
                const bool timedOut = ErrorChainContains(error, ShaderCompilerPipelineErrors::ToolTimedOut.domain,
                                                         ShaderCompilerPipelineErrors::ToolTimedOut.code);
                BuildOutputResult outcome = Failed;
                if (cancelled)
                    outcome = Cancelled;
                else if (timedOut)
                    outcome = TimedOut;
                Append(cancelled ? Warning : Error, outcome, error.code.Value(), "shader", error.message);
            }

        private:
            BuildOutputRecord Base(const DiagnosticSeverity severity, const BuildOutputResult result, const std::string_view code,
                                   std::string stage, std::string message) const {
                BuildOutputRecord record;
                record.timestampUtc = std::chrono::system_clock::now();
                record.sessionId = session_;
                record.operationId = request_.operationId;
                record.severity = severity;
                record.result = result;
                record.code = DiagnosticCode{std::string{code}};
                record.stage = std::move(stage);
                record.message = std::move(message);
                return record;
            }

            void Append(const DiagnosticSeverity severity, const BuildOutputResult result, const std::string_view code, std::string stage,
                        std::string message) {
                store_.Append(Base(severity, result, code, std::move(stage), std::move(message)));
            }

            BuildOutputStore &store_;
            BuildOutputSessionId session_;
            const ShaderBuildRequest &request_;
        };

        /** @brief Validates only explicit navigation authority; no file discovery or canonicalization occurs here. */
        bool ValidSources(const ShaderBuildRequest &request) {
            if (request.sources.size() > 257U)
                return false;
            for (std::size_t index = 0; index < request.sources.size(); ++index) {
                const auto &source = request.sources[index];
                if (source.logicalPath.empty() || source.logicalPath.size() > request.limits.maximumIdentityBytes ||
                    !source.absolutePath.is_absolute() || source.absolutePath.native().size() > 4096U)
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (request.sources[previous].logicalPath == source.logicalPath)
                        return false;
            }
            return true;
        }

        /** @brief Executes a synchronous admitted request while preserving typed allocation and compiler failures. */
        Result<Render::ShaderCompilationBatch> RunCompilation(const ShaderBuildRequest &request,
                                                              const Render::IShaderCompilerAdapter *adapter, const CancellationToken &token,
                                                              OutputSink &sink) {
            using Batch = Render::ShaderCompilationBatch;
            Result<Batch> result = Result<Batch>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ToolMissing));
            try {
                if (!ValidSources(request))
                    result = Result<Batch>::Failure(MakeError(ShaderBuildErrors::InvalidSources));
                else if (token.IsCancellationRequested())
                    result = Result<Batch>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
                else if (adapter)
                    result = Render::CompileShaderTargets(request.compilation, *adapter, token, request.limits, &sink);
            } catch (const std::bad_alloc &) {
                result = Result<Batch>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
            }
            return result;
        }
    }  // namespace

    struct ShaderBuildService::State {
        std::shared_ptr<const Render::IShaderCompilerAdapter> adapter;
        BuildOutputStore &output;
        const std::thread::id owner = std::this_thread::get_id();
        // Any host worker may enter. Mutex protects admission/active count; no compiler callback holds it.
        // Shutdown closes admission under this mutex, cancels each parent-linked token and waits for every slot to drain.
        std::mutex mutex;
        std::condition_variable drained;
        std::array<std::optional<CancellationSource>, 8> active;
        bool closed{};

        State(std::shared_ptr<const Render::IShaderCompilerAdapter> compiler, BuildOutputStore &store)
            : adapter(std::move(compiler)), output(store) {}
    };

    /** @copydoc ShaderBuildService::ShaderBuildService */
    ShaderBuildService::ShaderBuildService(std::shared_ptr<const Render::IShaderCompilerAdapter> adapter, BuildOutputStore &output)
        : state_(std::make_unique<State>(std::move(adapter), output)) {}

    /** @copydoc ShaderBuildService::~ShaderBuildService */
    ShaderBuildService::~ShaderBuildService() {
        Shutdown();
    }

    /** @copydoc ShaderBuildService::Compile */
    Result<Render::ShaderCompilationBatch> ShaderBuildService::Compile(const ShaderBuildRequest &request,
                                                                       const CancellationToken &cancellation) const {
        using Batch = Render::ShaderCompilationBatch;
        if (std::this_thread::get_id() == state_->owner)
            return Result<Batch>::Failure(MakeError(ShaderBuildErrors::OwnerThread));
        ShaderBuildRequest ownedRequest;
        try {
            ownedRequest = request;
        } catch (const std::bad_alloc &) {
            return Result<Batch>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        }
        std::size_t slot{};
        CancellationToken token;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed)
                return Result<Batch>::Failure(MakeError(ShaderBuildErrors::Closed));
            const auto available = std::ranges::find_if(state_->active, [](const auto &entry) {
                return !entry;
            });
            if (available == state_->active.end())
                return Result<Batch>::Failure(MakeError(ShaderBuildErrors::Capacity));
            slot = static_cast<std::size_t>(available - state_->active.begin());
            available->emplace(cancellation);
            token = available->value().Token();
        }

        const AdmissionGuard admission{state_->mutex, state_->drained, state_->active[slot]};
        const auto session = state_->output.BeginSession();
        if (!session)
            return Result<Batch>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        OutputSink sink{state_->output, *session, ownedRequest};
        auto result = RunCompilation(ownedRequest, state_->adapter.get(), token, sink);
        try {
            sink.Finish(result);
        } catch (const std::bad_alloc &) {
            // Append may already have consumed sequence/retention state; retry could duplicate terminal publication.
            // Preserve a typed failure and unwind admission rather than treating a missing terminal as success.
            return Result<Batch>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        }
        return result;
    }

    /** @copydoc ShaderBuildService::Shutdown */
    void ShaderBuildService::Shutdown() const noexcept {
        std::unique_lock lock(state_->mutex);
        state_->closed = true;
        for (const auto &entry : state_->active)
            if (entry)
                entry->RequestCancellation();
        state_->drained.wait(lock, [this] {
            return std::ranges::none_of(state_->active, [](const auto &entry) {
                return entry.has_value();
            });
        });
    }
}  // namespace Horo::Application
