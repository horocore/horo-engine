#pragma once
#include "EditorPlaySavePhysicsContent.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "PhysicsStateSaveContract.h"
#include "ScenePhysicsHost.h"

namespace Horo::Application::Internal {
    class PlaySavePhysicsServices;
    class PlaySavePhysicsRestoreFence;

    /** @brief Exact actual captured Physics coverage, not a bool/native-ready claim from schema metadata. */
    class PlaySavePhysicsCaptureCoverage final {
        friend class PlaySavePhysicsServices;

    public:
        [[nodiscard]] bool Matches(Runtime::RuntimeSceneView) const noexcept;

    private:
        std::weak_ptr<PlaySavePhysicsServices> owner_;
        Runtime::RuntimeSaveCaptureProvenance provenance_;
    };

    struct PlaySavePhysicsPolicy final {
        Physics::Detail::SceneStateStoragePolicy state;
        PlaySavePhysicsContentLimits content;
        std::size_t maximumWireBytes{};
    };

    /** @brief Explicit app-owned adapter over the EXISTING Scene Physics observer, not a provider registry or world owner.
     * Host retains this object and all borrowed Scene/native services until RuntimeSaveSession workers/delegate actually settle.
     * Workers never invoke these owner-only methods. CloseCapture releases only this adapter's opaque reader/configuration refs.
     */
    class PlaySavePhysicsServices final : public std::enable_shared_from_this<PlaySavePhysicsServices> {
        friend class PlaySavePhysicsCaptureCoverage;

    public:
        [[nodiscard]] static Result<std::shared_ptr<PlaySavePhysicsServices>> Create(ScenePhysicsHost &, Runtime::RuntimeSceneService &,
                                                                                     PlaySavePhysicsPolicy);
        /** @brief Same-owner published tick readiness; never performs a tick or derives readiness from metadata. */
        [[nodiscard]] Result<bool> CaptureReady() const;
        [[nodiscard]] Result<void> PrepareCapture(const Runtime::PersistentEntityIdentityMap &);
        [[nodiscard]] Result<PlaySavePhysicsCaptureCoverage> CaptureCoverage(const Runtime::CanonicalCaptureContext &);
        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &,
                                                                           Runtime::ICanonicalCaptureSink &);
        [[nodiscard]] Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>> StageRestore(
            std::shared_ptr<const std::vector<std::byte>> source, Runtime::StagedRestoreContext, Runtime::RuntimeSceneView,
            const Runtime::PersistentEntityIdentityMap &, const CancellationToken &, std::shared_ptr<PlaySavePhysicsRestoreFence>);
        void CloseCapture() noexcept;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        PlaySavePhysicsServices(ScenePhysicsHost &, Runtime::RuntimeSceneService &, PlaySavePhysicsPolicy) noexcept;
        [[nodiscard]] Result<void> ValidateCaptureScene(const Runtime::CanonicalCaptureContext &) const;
        [[nodiscard]] Result<void> EnsureCapture(const Runtime::CanonicalCaptureContext &);
        [[nodiscard]] bool MatchesCapture(Runtime::RuntimeSceneView, const Runtime::RuntimeSaveCaptureProvenance &) const noexcept;
        ScenePhysicsHost &native_;
        Runtime::RuntimeSceneService &scenes_;
        PlaySavePhysicsPolicy policy_;
        const std::thread::id owner_{std::this_thread::get_id()};
        std::shared_ptr<const PlaySavePhysicsContent> content_;
        std::shared_ptr<Physics::Detail::PreparedPhysicsSceneCapture> capture_;
        std::optional<Runtime::RuntimeSaveCaptureProvenance> provenance_;
        Physics::PhysicsStateSnapshotLease snapshot_;
    };

    /** @brief Installed before QueuePreparation. Actual receipt fills it once; final native check is delegated only at the real late gate.
     */
    class PlaySavePhysicsRestoreFence final : public Runtime::IStagedRestoreFinalPublicationCheck {
    public:
        [[nodiscard]] Result<void> Install(std::shared_ptr<Physics::Detail::PreparedPhysicsSceneRestore>);
        void Clear(const std::shared_ptr<Physics::Detail::PreparedPhysicsSceneRestore> &) noexcept;
        [[nodiscard]] Result<void> ValidateAndArm() override;

    private:
        const std::thread::id owner_{std::this_thread::get_id()};
        std::shared_ptr<Physics::Detail::PreparedPhysicsSceneRestore> prepared_;
    };

    /** @brief The actual registered Physics capture adapter; source-only metadata cannot construct one. */
    class PlaySavePhysicsCapture final : public Runtime::ICanonicalStateAdapter {
    public:
        explicit PlaySavePhysicsCapture(std::shared_ptr<PlaySavePhysicsServices> services) : services_(std::move(services)) {}

        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &context,
                                                                           Runtime::ICanonicalCaptureSink &sink) const override {
            return services_->Capture(context, sink);
        }

    private:
        std::shared_ptr<PlaySavePhysicsServices> services_;
    };
}  // namespace Horo::Application::Internal
