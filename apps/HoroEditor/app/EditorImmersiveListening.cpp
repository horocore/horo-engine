#include "EditorImmersiveListening.h"

#include "EditorMicrophonePermission.h"
#include "MacOnDeviceSpeechProvider.h"

#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Requests complete point or object evidence from the workspace owning the captured revision. */
        std::optional<EditorImmersiveListening::ReferenceReview> ReviewPointedReferences(
            const ImmersiveCommandEnvelope &preview, const ImmersivePlanningContext &context,
            const std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> &review,
            const AgentSceneReferenceSource source) {
            const auto kind = source == AgentSceneReferenceSource::Location ? ImmersivePointingReviewKind::Locations
                                                                            : ImmersivePointingReviewKind::Objects;
            const auto &identity = context.Identity();
            const auto pointing = CollectImmersivePointingHits(preview.Spatial(), identity.worldGeneration, kind);
            if (!pointing)
                return std::nullopt;
            AgentSceneReferenceRequest request{.documentEpoch = identity.documentOpening,
                                               .documentRevision = identity.documentRevision,
                                               .worldGeneration = identity.worldGeneration,
                                               .source = source,
                                               .structuralRevision = pointing->structuralRevision,
                                               .mounted = pointing->mounted,
                                               .lifetime = context.SourceLifetime()};
            std::array<AgentSceneReferencePoint, ImmersivePointingHits::MaximumObjects> locations;
            std::array<std::uint64_t, ImmersivePointingHits::MaximumObjects> objects;
            if (kind == ImmersivePointingReviewKind::Locations) {
                auto captured = std::span{locations}.first(pointing->count);
                for (std::size_t index = 0; index < captured.size(); ++index)
                    captured[index] = {pointing->hits[index].object.value, pointing->hits[index].worldPosition};
                request.locations = captured;
            } else {
                auto captured = std::span{objects}.first(pointing->count);
                for (std::size_t index = 0; index < captured.size(); ++index)
                    captured[index] = pointing->hits[index].object.value;
                request.objects = captured;
            }
            auto scene = review(request);
            if (!scene)
                return std::nullopt;
            return EditorImmersiveListening::ReferenceReview{std::move(*scene), pointing->hasMissedPress};
        }
    }  // namespace

    EditorImmersiveListening::EditorImmersiveListening(EditorMicrophonePermission &microphone, EditorMicrophoneSpeechSession &speech,
                                                       const Application::Internal::OpenXRPlayHost &xr, const Input::PlayerId player)
        : microphone_(microphone), speech_(speech), xr_(xr), player_(player) {}

    EditorImmersiveListening::~EditorImmersiveListening() {
        Cancel();
    }

    EditorImmersiveListening::State EditorImmersiveListening::Status() const noexcept {
        if (state_ == State::Spatial && speech_.PointingSourceRetired())
            return State::SpeechOnlySpatialRejected;
        return state_;
    }

    /** @copydoc EditorImmersiveListening::Transcript */
    std::string_view EditorImmersiveListening::Transcript() const & {
        if (!preview_ || !CurrentPresentation())
            return {};
        return preview_->Speech().PreviewText(SpeechHypothesisKind::Final);
    }

    /** @copydoc EditorImmersiveListening::PartialTranscript */
    std::string_view EditorImmersiveListening::PartialTranscript() const & {
        if (!partial_ || !CurrentPresentation())
            return {};
        return partial_->PreviewText(SpeechHypothesisKind::Partial);
    }

    /** @copydoc EditorImmersiveListening::ReadProgress */
    std::optional<ImmersiveSpeechProgress> EditorImmersiveListening::ReadProgress() const {
        return CurrentPresentation() ? speech_.ReadProgress() : std::nullopt;
    }

    /** @copydoc EditorImmersiveListening::ReviewReferences */
    std::optional<EditorImmersiveListening::ReferenceReview> EditorImmersiveListening::ReviewReferences(
        const std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> &review,
        const AgentSceneReferenceSource source) const {
        if (!preview_ || !review || !CurrentPresentation())
            return std::nullopt;
        const auto &context = preview_->Speech().Context();
        const auto &identity = context.Identity();
        if (source != AgentSceneReferenceSource::Selection) {
            if (speech_.PointingSourceRetired())
                return std::nullopt;
            return ReviewPointedReferences(*preview_, context, review, source);
        }
        std::array<std::uint64_t, AgentSceneSelectionSnapshot::MaximumObjects> objects;
        const auto selection = context.Selection();
        if (selection.size() > AgentSceneSelectionSnapshot::MaximumObjects)
            return std::nullopt;

        for (std::size_t index = 0; index < selection.size(); ++index)
            objects[index] = selection[index].value;
        auto scene = review({.documentEpoch = identity.documentOpening,
                             .documentRevision = identity.documentRevision,
                             .worldGeneration = identity.worldGeneration,
                             .objects = std::span{objects}.first(selection.size()),
                             .lifetime = context.SourceLifetime()});
        if (!scene)
            return std::nullopt;
        return ReferenceReview{std::move(*scene)};
    }

    /** @copydoc EditorImmersiveListening::ReviewSurfaces */
    std::optional<ImmersiveSurfaceHits> EditorImmersiveListening::ReviewSurfaces() const {
        if (!preview_ || preview_->Spatial().empty() || speech_.PointingSourceRetired())
            return std::nullopt;
        const auto current = CurrentPresentation();
        if (!current)
            return std::nullopt;
        const auto planes = xr_.ReadPlaneObservations();
        if (planes.HasError())
            return std::nullopt;
        return CollectImmersiveSurfaceHits(preview_->Spatial(), current->interaction.scene.value, planes.Value());
    }

    bool EditorImmersiveListening::CanListen() {
        return microphone_.CanRecord() && microphone_.Status() == EditorMicrophonePermissionStatus::Granted &&
               ObserveMacSpeechAuthorization() == MacSpeechAuthorization::Granted && xr_.ReadListeningScope(player_).has_value();
    }

    bool EditorImmersiveListening::NeedsMicrophonePermission() {
        return microphone_.Status() == EditorMicrophonePermissionStatus::Required;
    }

    bool EditorImmersiveListening::NeedsSpeechPermission() const noexcept {
        return ObserveMacSpeechAuthorization() == MacSpeechAuthorization::Required;
    }

    std::span<const std::string> EditorImmersiveListening::Inputs() const noexcept {
        return microphone_.Inputs();
    }

    std::optional<Audio::AudioCaptureDeviceId> EditorImmersiveListening::InputAt(const std::size_t index) const noexcept {
        return microphone_.SpeechInputAt(index);
    }

    void EditorImmersiveListening::RequestMicrophone() {
        microphone_.Request();
    }

    void EditorImmersiveListening::RequestSpeech() noexcept {
        static_cast<void>(RequestMacSpeechAuthorization());
    }

    void EditorImmersiveListening::RefreshInputs() {
        if (!xr_.ReadListeningScope(player_) || !microphone_.RefreshSpeechInputs(true))
            Fail();
    }

    void EditorImmersiveListening::Start(const Audio::AudioCaptureDeviceId input, const std::string_view locale,
                                         const std::uint64_t projectOpening, const AgentSceneSelectionSnapshot &selection) {
        Cancel();
        const auto scope = xr_.ReadListeningScope(player_);
        if (!scope || selection.truncated || selection.objectCount > selection.objects.size()) {
            Fail();
            return;
        }
        std::array<SceneSource::SceneObjectId, AgentSceneSelectionSnapshot::MaximumObjects> selected;
        for (std::size_t index = 0; index < selection.objectCount; ++index)
            selected[index] = {selection.objects[index]};
        auto context = owner_.Publish({.projectOpening = projectOpening,
                                       .documentOpening = selection.documentEpoch,
                                       .documentRevision = selection.documentRevision,
                                       .xrSession = scope->interaction.cutoff.session,
                                       .referenceSpace = scope->reference,
                                       .capabilities = scope->capabilities,
                                       .mode = ImmersiveAuthoringMode::Play,
                                       .worldGeneration = scope->interaction.scene.value,
                                       .diagnosticsAllowed = true,
                                       .localSpeechOnly = true},
                                      std::span{selected}.first(selection.objectCount), selection.lifetime);
        if (!context) {
            Fail();
            return;
        }
        constexpr float MinimumConfidence = 0.5F;
        const auto started = speech_.Start(input, true, owner_, *context, locale, MinimumConfidence);
        if (started != SpeechTranscriptionStatus::Accepted) {
            Fail(started);
            return;
        }
        scope_ = scope;
        context_ = std::move(context);
        state_ = State::Listening;
    }

    void EditorImmersiveListening::Finish() {
        if (state_ != State::Listening)
            return;
        if (!Current()) {
            Fail();
            return;
        }
        const auto sealed = speech_.Seal(true);
        if (sealed != SpeechTranscriptionStatus::Accepted) {
            Fail(sealed);
            return;
        }
        state_ = State::Finishing;
    }

    void EditorImmersiveListening::Poll() {
        if (!scope_) {
            static_cast<void>(speech_.Advance(false));
            return;
        }
        const auto current = Current();
        if (!current) {
            Fail();
            return;
        }
        auto update = speech_.Advance(true);
        using enum SpeechTranscriptionStatus;
        if (update.status == DraftWithdrawn) {
            partial_.reset();
            return;
        }
        if (update.status == Accepted) {
            if (update.evidence && !update.evidence->PreviewText(SpeechHypothesisKind::Partial).empty())
                partial_ = std::move(update.evidence);
            return;
        }
        if (update.status == Pending)
            return;
        partial_.reset();
        if (update.status != Finished || !update.evidence) {
            Fail(update.status);
            return;
        }
        AdoptFinalSpeech(*update.evidence, current->interaction.cutoff);
    }

    /** @copydoc EditorImmersiveListening::AdoptFinalSpeech */
    void EditorImmersiveListening::AdoptFinalSpeech(const ImmersiveSpeechEvidence &evidence, const XR::XRInteractionCutoff &cutoff) {
        auto result = speech_.AssembleCommand(true, evidence, cutoff);
        if (!result.envelope) {
            Fail();
            return;
        }
        switch (result.status) {
            case ImmersiveCommandStatus::SpeechOnlySpatialRejected:
                state_ = State::SpeechOnlySpatialRejected;
                break;
            case ImmersiveCommandStatus::SpeechOnly:
                state_ = State::SpeechOnly;
                break;
            default:
                state_ = State::Spatial;
                break;
        }
        preview_ = std::move(result.envelope);
    }

    /** @copydoc EditorImmersiveListening::Current */
    std::optional<EditorImmersiveListening::Scope> EditorImmersiveListening::Current() const {
        if (!scope_ || !context_ || !context_->IsCurrent())
            return std::nullopt;
        const auto current = xr_.ReadListeningScope(player_);
        if (!current || !scope_->SharesLineage(*current))
            return std::nullopt;
        if (preview_ && !preview_->IsCurrent(current->interaction.cutoff))
            return std::nullopt;
        return current;
    }

    /** @copydoc EditorImmersiveListening::CurrentPresentation */
    std::optional<EditorImmersiveListening::Scope> EditorImmersiveListening::CurrentPresentation() const {
        const auto current = Current();
        return current && speech_.CanPresent() ? current : std::nullopt;
    }

    /** @copydoc EditorImmersiveListening::Fail */
    void EditorImmersiveListening::Fail(const SpeechTranscriptionStatus reason) noexcept {
        Cancel();
        failureReason_ = reason;
        state_ = State::Interrupted;
    }

    void EditorImmersiveListening::Cancel() noexcept {
        partial_.reset();
        preview_.reset();
        scope_.reset();
        context_.reset();
        owner_.Revoke();
        speech_.Cancel();
        failureReason_ = SpeechTranscriptionStatus::CaptureInterrupted;
        state_ = State::Idle;
    }
}  // namespace Horo::Editor
