#include "Horo/Application/NavigationBakeApplication.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace Horo::Application {
    namespace {
        namespace Nav = Navigation;
        namespace Errors = Nav::NavigationErrors;

        /** @brief Hashes exact sorted Scene identity/content pairs without native layout or path dependence. */
        Sha256Digest SceneDigest(const NavigationProjectBakeSource &source) {
            Sha256Builder builder;
            for (const auto &scene : source.Scenes()) {
                static_cast<void>(builder.Update(std::as_bytes(std::span{scene.asset.Bytes()})));
                static_cast<void>(builder.Update(std::as_bytes(std::span{scene.digest.bytes})));
            }
            return builder.Finalize();
        }

        /** @brief Projects captured content into an opaque revision; full digests and exact bytes still fence adoption. */
        std::uint64_t Revision(const Sha256Digest &digest) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < 8; ++index)
                value = (value << 8U) | digest.bytes[index];
            return value;
        }

        /** @brief Closes the host cooker/target stamp over exact captured policy, definition and optional package source. */
        Nav::NavigationTileBakeCompatibility Compatibility(const Nav::NavigationTileBakeCompatibility &host,
                                                           const NavigationProjectBakeInput &input) {
            Sha256Builder settings;
            static_cast<void>(settings.Update(std::as_bytes(std::span{host.settings.bytes})));
            const auto append = [&settings](std::string_view text) {
                const auto digest = ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
                static_cast<void>(settings.Update(std::as_bytes(std::span{digest.bytes})));
            };
            append(EncodeNavigationProjectSettings(input.source->Settings()));
            for (const auto &definition : input.source->Definitions())
                if (definition.asset == input.definition)
                    static_cast<void>(settings.Update(std::as_bytes(std::span{definition.digest.bytes})));
            for (const auto *source : {&input.source->Packages().intent, &input.source->Packages().lock}) {
                const std::array present{static_cast<std::byte>(source->has_value())};
                static_cast<void>(settings.Update(present));
                if (*source)
                    append(**source);
            }
            auto result = host;
            result.settings = settings.Finalize();
            return result;
        }

        /** @brief Builds stable source revisions, independent of invocation identity except requestGeneration. */
        Result<Nav::NavigationBakeInputRevisions> Revisions(const NavigationProjectBakeSource &source, Assets::AssetId asset,
                                                            std::uint64_t generation) {
            const auto definition = std::ranges::find(source.Definitions(), asset, &NavigationDefinitionBakeSource::asset);
            if (definition == source.Definitions().end())
                return Result<Nav::NavigationBakeInputRevisions>::Failure(MakeError(Errors::BakeInputInvalid));
            const auto definitionRevision = Revision(definition->digest);
            const auto sceneRevision = Revision(SceneDigest(source));
            if (definitionRevision == 0 || sceneRevision == 0)
                return Result<Nav::NavigationBakeInputRevisions>::Failure(MakeError(Errors::IdentityInvalid));
            // Definition/area/coordinate revisions conservatively share their exact authored source version.
            // Per-tile dependency capture still hashes actual source subsets, not the global Scene revision.
            return Result<Nav::NavigationBakeInputRevisions>::Success(
                {Nav::NavigationBakeRequestGeneration::Create(generation).Value(),
                 Nav::NavigationDefinitionRevision::Create(definitionRevision).Value(),
                 Nav::NavigationSceneDocumentRevision::Create(sceneRevision).Value(),
                 Nav::NavigationAreaRegistryRevision::Create(definitionRevision).Value(), source.Settings().RuntimeProfile().Revision(),
                 Nav::NavigationCoordinatePolicyRevision::Create(definitionRevision).Value(),
                 Nav::NavigationSourceSnapshotRevision::Create(sceneRevision).Value()});
        }

        /** @brief Runs source and geometry capture on the scheduler I/O lane with one cancellation ancestry. */
        Result<NavigationProjectBakeInput> Capture(const NavigationBakeApplicationConfig &config, std::uint64_t generation,
                                                   const NavigationBakeInvocation &invocation) {
            auto source = config.captureSource(invocation);
            if (source.HasError())
                return Result<NavigationProjectBakeInput>::Failure(std::move(source).ErrorValue());
            if (!source.Value() || source.Value()->Settings().Input().definition != config.service.definition)
                return Result<NavigationProjectBakeInput>::Failure(MakeError(Errors::BakeInputInvalid));
            auto revisions = Revisions(*source.Value(), config.service.definition, generation);
            if (revisions.HasError())
                return Result<NavigationProjectBakeInput>::Failure(std::move(revisions).ErrorValue());
            return CaptureNavigationProjectBakeInput(std::move(source).Value(), config.service.definition, revisions.Value(), config.limits,
                                                     invocation.cancellation);
        }
    }  // namespace

    struct NavigationBakeApplication::Impl final {
        NavigationBakeApplicationConfig config;
        OperationStore &operations;
        JobSystem &jobs;
        std::unique_ptr<NavigationBakeService> service;
        const std::thread::id owner{std::this_thread::get_id()};
        std::uint64_t nextId{1};
        enum class Lifecycle : std::uint8_t {
            Open,
            Closing,
            Closed
        };
        Lifecycle lifecycle{Lifecycle::Open};
        CancellationSource cancellation;
        std::optional<NavigationBakeCommandSnapshot> snapshot;
        std::optional<std::vector<Nav::NavigationBakeSurfaceKey>> selection;
        std::optional<JobHandle> captureJob;
        // The callback writes once. Only a terminal durable JobHandle snapshot permits the owner to read;
        // the scheduler's terminal-record mutex publishes callback writes before that observation.
        std::shared_ptr<std::optional<Result<NavigationProjectBakeInput>>> captured;

        /** @brief Enforces the facade's construction-lane lifetime contract. */
        Result<void> CheckLane() const {
            return owner == std::this_thread::get_id() ? Result<void>::Success()
                                                       : Result<void>::Failure(MakeError(Errors::BakeJobAdmissionRejected));
        }

        /** @brief Preserves a typed error and ends only an operation whose owned work has stopped. */
        void Fail(Error error) {
            snapshot->state = ErrorChainContains(error, Errors::BakeInputCancelled.domain, Errors::BakeInputCancelled.code) ||
                                      ErrorChainContains(error, Platform::ProjectReadErrors::Cancelled.domain,
                                                         Platform::ProjectReadErrors::Cancelled.code) ||
                                      IsJobCancelled(error)
                                  ? OperationState::Cancelled
                                  : OperationState::Failed;
            snapshot->phase = "completed";
            snapshot->diagnostic = std::move(error);
            ++snapshot->revision;
        }

        /** @brief Hands complete capture to the sole existing service after the capture worker has drained. */
        Result<void> SubmitCapture() {
            if (!captured || !captured->has_value())
                return Result<void>::Failure(MakeError(Errors::BakeInputInvalid));
            auto result = std::move(captured->value());
            captured.reset();
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            auto input = std::move(result).Value();
            NavigationBakeRequest request{.input = input.input,
                                          .compatibility = Compatibility(config.compatibility, input),
                                          .tiles = std::move(input.tiles),
                                          .cancellation = cancellation.Token(),
                                          .projectProfile = input.source->Settings().RuntimeProfile(),
                                          .rebuildSurfaces = std::move(selection),
                                          .execution = snapshot->execution,
                                          .authoredSurfaces = std::move(input.surfaces),
                                          .sourceValidation = std::move(input.source)};
            for (const auto &source : input.contributions)
                request.sources.push_back(source.observation);
            if (auto current = config.service.sourceAuthority->UpdateCurrent(request.input->Revisions(), request.sources);
                current.HasError())
                return current;
            snapshot->inputFingerprint = request.input->Fingerprint();
            snapshot->surfaceCount = request.authoredSurfaces.size();
            snapshot->tileCount = request.tiles.size();
            auto submitted = service->Submit(std::move(request));
            if (submitted.HasError())
                return Result<void>::Failure(std::move(submitted).ErrorValue());
            snapshot->bakeOperation = submitted.Value();
            snapshot->phase = "bake.queued";
            ++snapshot->revision;
            return Result<void>::Success();
        }

        /** @brief Projects the actual shared bake record without replacing validation with an older publication. */
        void ObserveBake() {
            const auto records = operations.SnapshotIfChanged(0);
            if (!records)
                return;
            const auto found = std::ranges::find(records->operations, *snapshot->bakeOperation, &OperationRecord::id);
            if (found == records->operations.end()) {
                Fail(MakeError(Errors::InvalidHandle));
                return;
            }
            if (snapshot->state != found->state || snapshot->phase != found->phase || snapshot->progress != found->progress) {
                snapshot->state = found->state;
                snapshot->phase = found->phase;
                snapshot->progress = found->progress;
                snapshot->diagnostic = found->error;
                ++snapshot->revision;
            }
            if (snapshot->state == OperationState::Succeeded && snapshot->execution == Nav::NavigationBakeExecutionMode::BuildAndPublish)
                snapshot->publication = service->Published();
        }
    };

    /** @copydoc NavigationBakeCommandSnapshot::IsTerminal */
    bool NavigationBakeCommandSnapshot::IsTerminal() const noexcept {
        using enum OperationState;
        return state == Succeeded || state == Failed || state == Cancelled;
    }

    /** @copydoc NavigationBakeApplication::Create */
    Result<std::unique_ptr<NavigationBakeApplication>> NavigationBakeApplication::Create(NavigationBakeApplicationConfig config,
                                                                                         OperationStore &operations, JobSystem &jobs) {
        if (!config.captureSource || config.compatibility.provider == Sha256Digest{} || config.compatibility.schemas == Sha256Digest{} ||
            config.compatibility.settings == Sha256Digest{})
            return Result<std::unique_ptr<NavigationBakeApplication>>::Failure(MakeError(Errors::BakeInputInvalid));
        auto service = NavigationBakeService::Create(config.service, operations, jobs);
        if (service.HasError())
            return Result<std::unique_ptr<NavigationBakeApplication>>::Failure(std::move(service).ErrorValue());
        auto impl = std::make_unique<Impl>(std::move(config), operations, jobs, std::move(service).Value());
        return Result<std::unique_ptr<NavigationBakeApplication>>::Success(
            std::make_unique<NavigationBakeApplication>(ConstructionKey{}, std::move(impl)));
    }

    /** @copydoc NavigationBakeApplication::NavigationBakeApplication */
    NavigationBakeApplication::NavigationBakeApplication(ConstructionKey, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    /** @copydoc NavigationBakeApplication::~NavigationBakeApplication */
    NavigationBakeApplication::~NavigationBakeApplication() {
        if (impl_->owner != std::this_thread::get_id() || impl_->lifecycle != Impl::Lifecycle::Closed)
            std::terminate();
    }

    /** @copydoc NavigationBakeApplication::Start */
    Result<std::uint64_t> NavigationBakeApplication::Start(NavigationBakeCommandRequest request) {
        if (auto lane = impl_->CheckLane(); lane.HasError())
            return Result<std::uint64_t>::Failure(lane.ErrorValue());
        if (impl_->lifecycle != Impl::Lifecycle::Open || (impl_->snapshot && !impl_->snapshot->IsTerminal()))
            return Result<std::uint64_t>::Failure(MakeError(Errors::BakeJobAdmissionRejected));
        if (request.execution >= Nav::NavigationBakeExecutionMode::Count ||
            (request.surfaces && (request.surfaces->empty() || request.surfaces->size() > impl_->config.limits.maximumSurfaces)))
            return Result<std::uint64_t>::Failure(MakeError(Errors::BakeInputInvalid));
        if (impl_->nextId == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(Errors::GenerationExhausted));
        const auto generation = impl_->nextId++;
        auto cancellation = CancellationSource{request.invocation.cancellation};
        auto captured = std::make_shared<std::optional<Result<NavigationProjectBakeInput>>>();
        auto job = impl_->jobs.SubmitResult({.parentCancellation = cancellation.Token(), .resource = JobResource::Io},
                                            [config = impl_->config, generation, captured,
                                             invocation = std::move(request.invocation)](const CancellationToken &token) mutable {
            invocation.cancellation = token;
            captured->emplace(Capture(config, generation, invocation));
            if (captured->value().HasError())
                return Result<void>::Failure(captured->value().ErrorValue());
            return Result<void>::Success();
        });
        if (job.HasError())
            return Result<std::uint64_t>::Failure(std::move(job).ErrorValue());
        impl_->cancellation = std::move(cancellation);
        impl_->selection = std::move(request.surfaces);
        impl_->captured = std::move(captured);
        impl_->captureJob = std::move(job).Value();
        impl_->snapshot = NavigationBakeCommandSnapshot{.id = generation,
                                                        .revision = 1,
                                                        .state = OperationState::Running,
                                                        .phase = "capture",
                                                        .execution = request.execution};
        return Result<std::uint64_t>::Success(generation);
    }

    /** @copydoc NavigationBakeApplication::Pump */
    Result<void> NavigationBakeApplication::Pump() {
        if (auto lane = impl_->CheckLane(); lane.HasError())
            return lane;
        if (!impl_->snapshot || impl_->snapshot->IsTerminal())
            return Result<void>::Success();
        if (impl_->captureJob) {
            const auto job = impl_->captureJob->Snapshot();
            if (!job || !job->terminalResult)
                return Result<void>::Success();
            impl_->captureJob.reset();
            if (job->error) {
                impl_->Fail(*job->error);
                impl_->captured.reset();
            } else if (auto submitted = impl_->SubmitCapture(); submitted.HasError())
                impl_->Fail(std::move(submitted).ErrorValue());
        }
        if (impl_->snapshot->bakeOperation.has_value() && !impl_->snapshot->IsTerminal()) {
            impl_->service->Pump();
            impl_->ObserveBake();
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationBakeApplication::Query */
    Result<NavigationBakeCommandSnapshot> NavigationBakeApplication::Query(std::uint64_t id) const {
        if (auto lane = impl_->CheckLane(); lane.HasError())
            return Result<NavigationBakeCommandSnapshot>::Failure(lane.ErrorValue());
        if (!impl_->snapshot || impl_->snapshot->id != id)
            return Result<NavigationBakeCommandSnapshot>::Failure(MakeError(Errors::InvalidHandle));
        return Result<NavigationBakeCommandSnapshot>::Success(*impl_->snapshot);
    }

    /** @copydoc NavigationBakeApplication::RequestCancel */
    Result<void> NavigationBakeApplication::RequestCancel(std::uint64_t id) {
        auto snapshot = Query(id);
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (!snapshot.Value().IsTerminal()) {
            impl_->cancellation.RequestCancellation();
            if (snapshot.Value().bakeOperation.has_value())
                static_cast<void>(impl_->operations.RequestCancel(*snapshot.Value().bakeOperation));
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationBakeApplication::Close */
    Result<void> NavigationBakeApplication::Close() {
        using enum Impl::Lifecycle;
        if (auto lane = impl_->CheckLane(); lane.HasError())
            return lane;
        if (impl_->lifecycle == Closed)
            return Result<void>::Success();
        impl_->lifecycle = Closing;
        if (impl_->snapshot && !impl_->snapshot->IsTerminal()) {
            static_cast<void>(RequestCancel(impl_->snapshot->id));
            return Result<void>::Failure(MakeError(Errors::BakeJobAdmissionRejected));
        }
        impl_->service->Close();
        impl_->lifecycle = Closed;
        return Result<void>::Success();
    }
}  // namespace Horo::Application
