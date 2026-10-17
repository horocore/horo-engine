#include "SettingsModalCollisionInternal.h"

#include <imgui.h>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        const char *Text(const EditorGuiContext &ctx, const char *key) {
            return ctx.localization.Get("editor", key).c_str();
        }

        void Record(SettingsCollisionDraft &draft, const Result<void> &outcome) {
            if (outcome.HasError())
                draft.error = outcome.ErrorValue();
            else
                draft.error.reset();
        }

        const char *ImpactKey(const CollisionImpactReason reason) {
            using enum CollisionImpactReason;
            switch (reason) {
                case ProfileReference:
                    return "settings.collision.impact.profile";
                case OpaqueOwner:
                    return "settings.collision.impact.opaque";
                case InventoryIncomplete:
                    return "settings.collision.impact.inventory";
                case TriggerCaptureStale:
                    return "settings.collision.impact.trigger";
                case ActiveGeneration:
                    return "settings.collision.impact.active";
            }
            return "settings.collision.unavailable";
        }

        void DrawReview(SettingsCollisionDraft &draft, CollisionProjectSettingsSession &owner, const EditorGuiContext &ctx) {
            if (Ui::Button({.label = Text(ctx, "settings.collision.review")})) {
                auto review = owner.Preview(*draft.input);
                if (review.HasError()) {
                    draft.error = review.ErrorValue();
                    draft.review.reset();
                } else {
                    draft.review = std::move(review).Value();
                    draft.error.reset();
                }
            }
            if (!draft.review)
                return;
            for (const auto &impact : draft.review->Impacts()) {
                ImGui::TextWrapped("%s: %s", Text(ctx, ImpactKey(impact.reason)), impact.path.c_str());
            }
            if (Ui::Button({.label = Text(ctx, "settings.collision.commit"), .enabled = draft.review->CanCommit()})) {
                const auto committed = owner.Commit(*draft.review);
                Record(draft, committed);
                if (!committed.HasError())
                    RefreshCollisionDraft(draft, owner);
            }
        }

        void DrawHistory(SettingsCollisionDraft &draft, CollisionProjectSettingsSession &owner, const EditorGuiContext &ctx) {
            if (Ui::Button({.label = Text(ctx, "settings.collision.undo"), .variant = Ui::ButtonVariant::Secondary})) {
                const auto replayed = owner.Replay(false);
                Record(draft, replayed);
                if (!replayed.HasError())
                    RefreshCollisionDraft(draft, owner);
            }
            if (Ui::Button({.label = Text(ctx, "settings.collision.redo"), .variant = Ui::ButtonVariant::Secondary})) {
                const auto replayed = owner.Replay(true);
                Record(draft, replayed);
                if (!replayed.HasError())
                    RefreshCollisionDraft(draft, owner);
            }
            if (Ui::Button({.label = Text(ctx, "settings.collision.reset_candidate"), .variant = Ui::ButtonVariant::Secondary}))
                RefreshCollisionDraft(draft, owner);
            if (Ui::Button({.label = Text(ctx, "settings.collision.save"), .enabled = !draft.pending && owner.IsDirty()})) {
                auto saved = owner.Save();
                if (saved.HasError())
                    draft.error = saved.ErrorValue();
                else {
                    RefreshCollisionDraft(draft, owner);
                    if (saved.Value().durabilityError)
                        draft.error = *saved.Value().durabilityError;
                }
            }
        }

        void DrawActivation(const CollisionProjectSettingsSession &owner, const EditorGuiContext &ctx) {
            using enum CollisionActivationState;
            const auto evidence = owner.Activation();
            if (evidence.HasError())
                return;
            const char *key = "settings.collision.activation.unknown";
            switch (evidence.Value()->state) {
                case NoPhysics:
                    key = "settings.collision.activation.no_physics";
                    break;
                case Inactive:
                    key = "settings.collision.activation.inactive";
                    break;
                case Active:
                    key = "settings.collision.activation.active";
                    break;
                case Unknown:
                    break;
            }
            ImGui::TextWrapped("%s", Text(ctx, key));
            if (evidence.Value()->state == Active)
                ImGui::Text("%llu / %llu / %llu", static_cast<unsigned long long>(evidence.Value()->documentRevision),
                            static_cast<unsigned long long>(evidence.Value()->generation),
                            static_cast<unsigned long long>(evidence.Value()->fingerprint));
            if (evidence.Value()->playRunning)
                ImGui::TextWrapped("%s", Text(ctx, "settings.collision.stop_play"));
        }
    }  // namespace

    void DrawCollision(SettingsState &state, const EditorGuiContext &ctx) {
        auto &draft = state.collision;
        Ui::SectionTitle(Text(ctx, "settings.collision.title"), ctx.theme.fonts);
        ImGui::TextWrapped("%s", Text(ctx, "settings.collision.authority"));
        const auto owner = draft.owner.lock();
        if (!owner) {
            ImGui::TextWrapped("%s", Text(ctx, "settings.collision.unavailable"));
            if (draft.error)
                ImGui::TextWrapped("%s", draft.error->message.c_str());
            return;
        }
        // Resolves owner-thread admission each frame without I/O or a copied projection.
        if (const auto current = owner->Snapshot(); current.HasError()) {
            ImGui::TextWrapped("%s", current.ErrorValue().message.c_str());
            return;
        }
        if (!draft.input)
            return;
        DrawActivation(*owner, ctx);
        DrawCollisionDefinitions(draft, *owner, ctx);
        DrawCollisionProfiles(draft, ctx);
        DrawCollisionMatrix(draft, ctx);
        DrawReview(draft, *owner, ctx);
        DrawHistory(draft, *owner, ctx);
        if (draft.error)
            ImGui::TextWrapped("%s", draft.error->message.c_str());
    }
}  // namespace Horo::Editor::SettingsModalInternal
