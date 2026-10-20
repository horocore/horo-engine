#include "navigation/NavigationProjectBakeNativeFixture.h"
#include "navigation/NavigationPublicationFixture.h"

namespace Horo::Application {
    namespace {
        namespace Native = NavigationInputNativeTestSupport;
        namespace Inputs = NavigationInputTestSupport;
        namespace Nav = Navigation;
        namespace Faults = TestSupport;
        using NavigationSourceTestSupport::Asset;
        using enum Nav::NavigationBakeExecutionMode;

        /** @brief Owns the real native service before worker drain and keeps its project root borrowed only within the test. */
        struct Host final {
            std::shared_ptr<BakeTestSupport::ControlledBuilder> builder{std::make_shared<BakeTestSupport::ControlledBuilder>()};
            NavigationBakeServiceConfig config;
            OperationStore operations{8, 16};
            JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
            std::unique_ptr<NavigationBakeService> service;

            explicit Host(const Inputs::Project &project, std::shared_ptr<DurableFileSystem> files = {})
                : config(Native::Config(project, builder)) {
                if (files)
                    config.files = std::move(files);
                auto created = NavigationBakeService::Create(config, operations, jobs);
                REQUIRE(created.HasValue());
                service = std::move(created).Value();
            }
        };

        /** @brief Captures genuine source under an optionally revocable retained authority callback. */
        NavigationProjectBakeInput Capture(const Inputs::Project &project, const Nav::NavigationBakeInputRevisions &revisions,
                                           std::shared_ptr<const std::atomic<bool>> revoked = {}) {
            auto context = project.source.Context();
            if (revoked)
                context.authorityStopped = [revoked] {
                    return revoked->load();
                };
            auto source = NavigationProjectBakeSource::Capture(project.source.files, std::move(context), project.source.Compatibility());
            REQUIRE(source.HasValue());
            auto input = CaptureNavigationProjectBakeInput(std::move(source).Value(), Asset(1), revisions);
            REQUIRE(input.HasValue());
            return std::move(input).Value();
        }

        /** @brief Changes actual canonical Scene bytes without updating the bake authority's captured DTO evidence. */
        void EditFirstScene(const Inputs::Project &project) {
            auto object = Inputs::Project::Object();
            object.components.colliders.front().localPose.translation.x += 1;
            const std::array objects{object};
            project.source.Write("Assets/first.horo", SceneSource::EncodeSceneSource({objects, {}}));
        }

        /** @brief Pauses the final check while retaining and delegating to genuine file-backed source verification. */
        class PausingValidation final : public Nav::INavigationBakeSourceValidation {
        public:
            explicit PausingValidation(std::shared_ptr<const NavigationProjectBakeSource> source) : source_(std::move(source)) {}

            /** @brief Exposes only the bounded test wait signal. @return Borrow tied to this validator's lifetime. */
            const std::atomic<bool> &Entered() const noexcept {
                return entered_;
            }

            /** @brief Releases the deliberately paused verification worker. */
            void Resume() const noexcept {
                paused_.store(false);
            }

            Result<void> ValidateCurrent(const CancellationToken &cancellation) const override {
                if (calls_.fetch_add(1) == 1) {
                    entered_.store(true);
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                    while (paused_.load() && !cancellation.IsCancellationRequested()) {
                        if (std::chrono::steady_clock::now() >= deadline)
                            return Result<void>::Failure(MakeError(Nav::NavigationErrors::BakeInputFailed));
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                }
                return source_->ValidateCurrent(cancellation);
            }

        private:
            std::shared_ptr<const NavigationProjectBakeSource> source_;
            mutable std::atomic<unsigned> calls_{};
            mutable std::atomic<bool> entered_{};
            mutable std::atomic<bool> paused_{true};
        };

        /** @brief Advances the complete fixture epoch for a separate coherent source capture. */
        Nav::NavigationBakeInputRevisions NextRevisions() {
            auto revisions = Inputs::Project::Revisions();
            revisions.requestGeneration = Nav::TestSupport::Id<Nav::NavigationBakeRequestGeneration>(2);
            revisions.scene = Nav::TestSupport::Id<Nav::NavigationSceneDocumentRevision>(2);
            revisions.geometry = Nav::TestSupport::Id<Nav::NavigationSourceSnapshotRevision>(2);
            return revisions;
        }
    }  // namespace

    TEST_CASE("File-backed bake and validate-only reject actual source drift before provider or output work",
              "[navigation][project][source_fence][native]") {
        Inputs::Project project;
        Host host{project};
        const auto input = Capture(project, Inputs::Project::Revisions());
        EditFirstScene(project);
        const auto mode = GENERATE(ValidateOnly, BuildAndPublish);
        const auto operation = Native::Submit(input, *host.service, host.config, mode);
        REQUIRE(operation.HasValue());
        const auto terminal = BakeTestSupport::Terminal(*host.service, host.operations, operation.Value());
        REQUIRE(terminal.state == OperationState::Cancelled);
        REQUIRE(terminal.error);
        CHECK(terminal.error->code.Value() == Nav::NavigationErrors::BakeInputStale.code.Value());
        CHECK(host.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists((project.source.directory.Path() / ".horo/cache/navigation")));
        CHECK_FALSE(std::filesystem::exists(host.config.targetRoot));
    }

    TEST_CASE("Actual source drift or revoked project authority after staging cannot replace the durable navigation generation",
              "[navigation][project][source_fence][native][publication]") {
        Inputs::Project project;
        const auto files = std::make_shared<Faults::PublicationFiles>();
        Host host{project, files};
        const auto revoked = std::make_shared<std::atomic<bool>>(false);
        const auto previousInput = Capture(project, Inputs::Project::Revisions(), revoked);
        const auto previous = Native::Execute(previousInput, *host.service, host.config, host.operations);
        auto object = Inputs::Project::Object();
        object.components.colliders.front().localPose.translation.y += 1;
        project.Scenes(object);
        const auto replacement = Capture(project, NextRevisions(), revoked);
        files->beforeCurrentReached.store(false);
        files->pauseBeforeCurrent.store(true);
        Faults::PublicationPause resume{files};
        const auto operation = Native::Submit(replacement, *host.service, host.config);
        REQUIRE(operation.HasValue());
        REQUIRE(Faults::WaitFor(files->beforeCurrentReached));
        const bool revokeAuthority = GENERATE(false, true);
        if (revokeAuthority)
            revoked->store(true);
        else
            EditFirstScene(project);
        files->pauseBeforeCurrent.store(false);
        const auto terminal = BakeTestSupport::Terminal(*host.service, host.operations, operation.Value());
        REQUIRE(terminal.state == OperationState::Cancelled);
        REQUIRE(terminal.error);
        const auto &expected = revokeAuthority ? Nav::NavigationErrors::BakeInputCancelled : Nav::NavigationErrors::BakeInputStale;
        CHECK(ErrorChainContains(*terminal.error, expected.domain, expected.code));
        if (revokeAuthority)
            CHECK(ErrorChainContains(*terminal.error, Platform::ProjectReadErrors::Cancelled.domain,
                                     Platform::ProjectReadErrors::Cancelled.code));
        CHECK(host.service->Published() == previous);
        const auto durable = ResolveNavigationBakePublication(host.config);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value()->generation.manifestDigest == previous->generation.manifestDigest);
        CHECK(previous->tiles.tiles.size() == 2);
        CHECK(host.builder->builds.load() == 4);
    }

    TEST_CASE("Equal bake fingerprints with different retained source-validation owners cannot join an operation",
              "[navigation][project][source_fence][native][ownership]") {
        Inputs::Project project;
        Host host{project};
        const auto first = Capture(project, Inputs::Project::Revisions());
        const auto second = Capture(project, Inputs::Project::Revisions());
        REQUIRE(first.input->Fingerprint() == second.input->Fingerprint());
        REQUIRE(first.source != second.source);
        host.builder->pause.store(true);
        const auto firstOperation = Native::Submit(first, *host.service, host.config);
        REQUIRE(firstOperation.HasValue());
        REQUIRE(Faults::WaitFor(host.builder->entered));
        const auto secondOperation = Native::Submit(second, *host.service, host.config);
        REQUIRE(secondOperation.HasValue());
        CHECK(firstOperation.Value() != secondOperation.Value());
        host.builder->pause.store(false);
        CHECK(BakeTestSupport::Terminal(*host.service, host.operations, firstOperation.Value()).state == OperationState::Cancelled);
        CHECK(BakeTestSupport::Terminal(*host.service, host.operations, secondOperation.Value()).state == OperationState::Succeeded);
        REQUIRE(host.service->Published());
        CHECK(host.service->Published()->tiles.tiles.size() == 2);
    }

    TEST_CASE("Validate-only retains the authoritative source lease throughout actual external source verification",
              "[navigation][project][source_fence][native][lease]") {
        Inputs::Project project;
        Host host{project};
        const auto input = Capture(project, Inputs::Project::Revisions());
        const auto validation = std::make_shared<PausingValidation>(input.source);
        auto request = Native::Request(input, ValidateOnly);
        request.sourceValidation = validation;
        const auto observations = request.sources;
        REQUIRE(host.config.sourceAuthority->UpdateCurrent(input.input->Revisions(), observations).HasValue());
        const auto operation = host.service->Submit(std::move(request));
        REQUIRE(operation.HasValue());
        REQUIRE(Faults::WaitFor(validation->Entered()));
        const auto mutation = host.config.sourceAuthority->UpdateCurrent(input.input->Revisions(), observations);
        REQUIRE(mutation.HasError());
        CHECK(ErrorChainContains(mutation.ErrorValue(), Nav::NavigationErrors::BakeJobAdmissionRejected.domain,
                                 Nav::NavigationErrors::BakeJobAdmissionRejected.code));
        validation->Resume();
        CHECK(BakeTestSupport::Terminal(*host.service, host.operations, operation.Value()).state == OperationState::Succeeded);
        CHECK(host.config.sourceAuthority->UpdateCurrent(input.input->Revisions(), observations).HasValue());
        CHECK(host.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(host.config.targetRoot));
    }

}  // namespace Horo::Application
