#include "ImmersiveAuthoringContext.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace Horo::Editor {
    struct ImmersiveContextEpoch final {
        std::atomic<std::uint64_t> current{};
    };

    namespace {
        /** @brief Capture-only Play admits no user, conversation or tool dispatch credentials. */
        bool ValidAuthority(const ImmersiveContextIdentity &identity) noexcept {
            if (identity.localSpeechOnly)
                return identity.mode == ImmersiveAuthoringMode::Play && identity.diagnosticsAllowed && !identity.planningAllowed &&
                       !identity.user && !identity.conversation && !identity.toolContext;
            return identity.user && identity.conversation && identity.toolContext;
        }

        /** @brief Verifies that the mode, optional world and spatial scope come from the same owner. */
        bool ValidXrScope(const ImmersiveContextIdentity &identity) noexcept {
            using enum ImmersiveAuthoringMode;
            const bool validMode = identity.mode == Edit || identity.mode == Simulate || identity.mode == Play;
            const bool validWorld = (identity.worldGeneration == 0) == (identity.mode == Edit);
            return validMode && validWorld && identity.capabilities.IsValid() && identity.xrSession.IsValid() &&
                   identity.referenceSpace.IsValid() && identity.referenceSpace.session == identity.xrSession;
        }

        /** @brief Admits only a complete document observation and the matching authority. */
        bool ValidIdentity(const ImmersiveContextIdentity &identity) noexcept {
            return identity.projectOpening && identity.documentOpening && identity.documentRevision && ValidAuthority(identity) &&
                   ValidXrScope(identity);
        }

        /** @brief Play can consume speech locally. Other modes use the planning admission. */
        ImmersiveAdmission AdmitPlanningOrSpeech(const ImmersiveContextIdentity &identity, const ImmersiveRequestKind kind) noexcept {
            using enum ImmersiveAdmission;
            if (identity.mode != ImmersiveAuthoringMode::Play)
                return identity.planningAllowed ? Admitted : PermissionDenied;
            return kind == ImmersiveRequestKind::SpeechCapture && identity.localSpeechOnly ? Admitted : ModeDenied;
        }

        /** @brief Verifies selection without allocations or reordering the owner projection. */
        bool ValidSelection(const std::span<const SceneSource::SceneObjectId> selection) noexcept {
            if (selection.size() > ImmersivePlanningContext::MaximumSelection)
                return false;
            for (std::size_t index = 0; index < selection.size(); ++index) {
                const auto previous = selection.first(index);
                if (!selection[index].IsValid() || std::ranges::find(previous, selection[index]) != previous.end())
                    return false;
            }
            return true;
        }

        /** @brief Rejects tool-bearing history and malformed payloads before transferring them to the queue. */
        bool ValidTextRequest(const Agent::ModelRequest &request) {
            using Agent::ModelFeature;
            constexpr Agent::ModelFeatures textFeatures{static_cast<std::uint32_t>(ModelFeature::Streaming) |
                                                        static_cast<std::uint32_t>(ModelFeature::Usage)};
            return Agent::AdmitModelRequest(request, textFeatures).Succeeded() &&
                   std::ranges::none_of(request.messages, [](const Agent::ModelMessage &message) {
                return message.role == Agent::ModelRole::Tool;
            });
        }
    }  // namespace

    bool ImmersivePlanningContext::IsCurrent() const noexcept {
        return epoch_ && generation_ != 0 && epoch_->current.load(std::memory_order_acquire) == generation_;
    }

    ImmersiveAgentRequest::ImmersiveAgentRequest(ImmersivePlanningContext context, Agent::ModelRequest request)
        : context_(std::move(context)), request_(std::move(request)) {}

    /** @copydoc ImmersiveAgentRequest::Run */
    Agent::AgentTurnResult ImmersiveAgentRequest::Run(Agent::IModelProvider &provider, std::uint64_t turnId, std::stop_token stop) {
        Agent::AgentTurnRuntime runtime{[this] {
            return IsCurrent();
        }};
        return runtime.Run(provider, nullptr, turnId, std::exchange(request_, {}), {}, stop);
    }

    ImmersiveAuthoringContext::ImmersiveAuthoringContext() : epoch_(std::make_shared<ImmersiveContextEpoch>()) {}

    ImmersiveAuthoringContext::~ImmersiveAuthoringContext() {
        Revoke();
    }

    std::optional<ImmersivePlanningContext> ImmersiveAuthoringContext::Publish(
        const ImmersiveContextIdentity &identity, const std::span<const SceneSource::SceneObjectId> selection) noexcept {
        if (!ValidIdentity(identity) || !ValidSelection(selection)) {
            Revoke();
            return std::nullopt;
        }
        if (current_ && identity == current_->Identity() && std::ranges::equal(selection, current_->Selection()))
            return current_;
        Revoke();
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        auto &context = current_.emplace();
        context.identity_ = identity;
        std::ranges::copy(selection, context.selection_.begin());
        context.selectionCount_ = selection.size();
        context.generation_ = ++generation_;
        context.epoch_ = epoch_;
        epoch_->current.store(context.generation_, std::memory_order_release);
        return current_;
    }

    void ImmersiveAuthoringContext::Revoke() noexcept {
        epoch_->current.store(0, std::memory_order_release);
        current_.reset();
    }

    ImmersiveAdmission ImmersiveAuthoringContext::Admit(const ImmersivePlanningContext &context,
                                                        const ImmersiveRequestKind kind) const noexcept {
        if (context.epoch_ != epoch_ || !context.IsCurrent())
            return ImmersiveAdmission::StaleContext;
        const auto &identity = context.Identity();
        using enum ImmersiveAdmission;
        using enum ImmersiveRequestKind;
        if (kind == Diagnostics)
            return identity.diagnosticsAllowed ? Admitted : PermissionDenied;
        if (kind != Plan && kind != SpeechCapture)
            return InvalidRequest;
        return AdmitPlanningOrSpeech(identity, kind);
    }

    /** @copydoc ImmersiveAuthoringContext::CaptureRequest */
    std::optional<ImmersiveAgentRequest> ImmersiveAuthoringContext::CaptureRequest(const ImmersivePlanningContext &context,
                                                                                   const ImmersiveRequestKind kind,
                                                                                   Agent::ModelRequest &&request) const {
        if (context.Identity().localSpeechOnly || kind == ImmersiveRequestKind::SpeechCapture)
            return std::nullopt;
        if (Admit(context, kind) != ImmersiveAdmission::Admitted || !ValidTextRequest(request))
            return std::nullopt;
        return ImmersiveAgentRequest{context, std::move(request)};
    }
}  // namespace Horo::Editor
