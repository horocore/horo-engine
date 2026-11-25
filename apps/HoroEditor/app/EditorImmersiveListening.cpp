#include "EditorImmersiveListening.h"

#include "EditorMicrophonePermission.h"
#include "MacOnDeviceSpeechProvider.h"

#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Requests complete point or object evidence from the workspace owning the captured revision. */
        std::optional<AgentSceneReferenceReview> ReviewPointedReferences(
            const ImmersiveCommandEnvelope &preview, const ImmersiveContextIdentity &identity,
            const std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> &review,
            const AgentSceneReferenceSource source) {
            const auto kind = source == AgentSceneReferenceSource::Location ? ImmersivePointingReviewKind::Locations
                                                                            : ImmersivePointingReviewKind::Objects;
            const auto pointing = CollectImmersivePointingHits(preview.Spatial(), identity.worldGeneration, kind);
            if (!pointing)
                return std::nullopt;
            AgentSceneReferenceRequest request{.documentEpoch = identity.documentOpening,
                                               .documentRevision = identity.documentRevision,
                                               .worldGeneration = identity.worldGeneration,
                                               .source = source,
                                               .structuralRevision = pointing->structuralRevision,
                                               .mounted = pointing->mounted};
            if (kind == ImmersivePointingReviewKind::Locations) {
                std::array<AgentSceneReferencePoint, ImmersivePointingHits::MaximumObjects> locations;
                auto captured = std::span{locations}.first(pointing->count);
                for (std::size_t index = 0; index < captured.size(); ++index)
                    captured[index] = {pointing->hits[index].object.value, pointing->hits[index].worldPosition};
                request.locations = captured;
                return review(request);
            }
            std::array<std::uint64_t, ImmersivePointingHits::MaximumObjects> objects;
            auto captured = std::span{objects}.first(pointing->count);
            for (std::size_t index = 0; index < captured.size(); ++index)
                captured[index] = pointing->hits[index].object.value;
            request.objects = captured;
            return review(request);
        }
    }  // namespace

    EditorImmersiveListening::EditorImmersiveListening(EditorMicrophonePermission &microphone, EditorMicrophoneSpeechSession &speech,
                                                       const Application::Internal::OpenXRPlayHost &xr, const Input::PlayerId player)
        : microphone_(microphone), speech_(speech), xr_(xr), player_(player) {}

    EditorImmersiveListening::~EditorImmersiveListening() {
        Cancel();
    }

    EditorImmersiveListening::State EditorImmersiveListening::Status() const noexcept {
        return state_;
    }

    std::string_view EditorImmersiveListening::Transcript() const & noexcept {
        return preview_ ? preview_->Speech().Hypothesis().text : std::string_view{};
    }

    /** @copydoc EditorImmersiveListening::ReviewReferences */
    std::optional<AgentSceneReferenceReview> EditorImmersiveListening::ReviewReferences(
        const std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> &review,
        const AgentSceneReferenceSource source) const {
        if (!preview_ || !review)
            return std::nullopt;
        const auto current = Current();
        if (!current || !preview_->IsCurrent(current->interaction.cutoff))
            return std::nullopt;
        const auto &context = preview_->Speech().Context();
        const auto &identity = context.Identity();
        if (source != AgentSceneReferenceSource::Selection)
            return ReviewPointedReferences(*preview_, identity, review, source);
        std::array<std::uint64_t, AgentSceneSelectionSnapshot::MaximumObjects> objects;
        const auto selection = context.Selection();
        if (selection.size() > AgentSceneSelectionSnapshot::MaximumObjects)
            return std::nullopt;

        for (std::size_t index = 0; index < selection.size(); ++index)
            objects[index] = selection[index].value;
        return review(
            {identity.documentOpening, identity.documentRevision, identity.worldGeneration, std::span{objects}.first(selection.size())});
    }

    /** @copydoc EditorImmersiveListening::ReviewSurfaces */
    std::optional<ImmersiveSurfaceHits> EditorImmersiveListening::ReviewSurfaces() const {
        if (!preview_ || preview_->Spatial().empty())
            return std::nullopt;
        const auto current = Current();
        if (!current || !preview_->IsCurrent(current->interaction.cutoff))
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
        if (!scope || selection.truncated || selection.lifetime.IsCancellationRequested() ||
            selection.objectCount > selection.objects.size()) {
            Fail();
            return;
        }
        std::array<SceneSource::SceneObjectId, AgentSceneSelectionSnapshot::MaximumObjects> selected;
        for (std::size_t index = 0; index < selection.objectCount; ++index)
            selected[index] = {selection.objects[index]};
        const auto context = owner_.Publish({.projectOpening = projectOpening,
                                             .documentOpening = selection.documentEpoch,
                                             .documentRevision = selection.documentRevision,
                                             .xrSession = scope->interaction.cutoff.session,
                                             .referenceSpace = scope->reference,
                                             .capabilities = scope->capabilities,
                                             .mode = ImmersiveAuthoringMode::Play,
                                             .worldGeneration = scope->interaction.scene.value,
                                             .diagnosticsAllowed = true,
                                             .localSpeechOnly = true},
                                            std::span{selected}.first(selection.objectCount));
        constexpr float MinimumConfidence = 0.5F;
        if (!context || speech_.Start(input, true, owner_, *context, locale, MinimumConfidence) != SpeechTranscriptionStatus::Accepted) {
            Fail();
            return;
        }
        scope_ = scope;
        selectionLifetime_ = selection.lifetime;
        state_ = State::Listening;
    }

    void EditorImmersiveListening::Finish() {
        if (state_ != State::Listening)
            return;
        if (!Current() || speech_.Seal(true) != SpeechTranscriptionStatus::Accepted) {
            Fail();
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
        const auto update = speech_.Advance(true);
        using enum SpeechTranscriptionStatus;
        if (update.status == Pending || update.status == Accepted)
            return;
        if (update.status != Finished || !update.evidence) {
            Fail();
            return;
        }
        auto result = speech_.AssembleCommand(true, *update.evidence, current->interaction.cutoff);
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
        if (!scope_ || selectionLifetime_.IsCancellationRequested())
            return std::nullopt;
        const auto current = xr_.ReadListeningScope(player_);
        if (!current || !scope_->SharesLineage(*current))
            return std::nullopt;
        return current;
    }

    /** @copydoc EditorImmersiveListening::Fail */
    void EditorImmersiveListening::Fail() noexcept {
        Cancel();
        state_ = State::Interrupted;
    }

    void EditorImmersiveListening::Cancel() noexcept {
        preview_.reset();
        scope_.reset();
        selectionLifetime_ = {};
        owner_.Revoke();
        speech_.Cancel();
        state_ = State::Idle;
    }
}  // namespace Horo::Editor
