#include "NavigationInspectorSession.h"

#include "Horo/Scene/PrefabComponentProjection.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Copy only actual typed navigation occurrences, preserving the selected object's unrelated content. */
        void Capture(std::vector<NavigationInspectorDraft> &drafts, SceneObjectId object, const auto &components, const std::string &name) {
            const auto append = [&](const auto &value) {
                if (value)
                    drafts.push_back({{object, *value}, *value, name});
            };
            append(components.navigationSurface);
            append(components.navigationRegion);
            append(components.navigationModifier);
            append(components.navigationLink);
            append(components.navigationAgent);
        }
    }  // namespace

    /** @copydoc NavigationInspectorSession::Begin */
    void NavigationInspectorSession::Begin(const EditorWorkspaceViewModel &view) {
        const bool sameDefinitions = definitions_.size() == view.navigationDefinitions.size() &&
                                     std::ranges::equal(definitions_, view.navigationDefinitions, [](const auto &a, const auto &b) {
            return a.asset == b.asset && a.session == b.session && a.revision == b.revision && a.definition == b.definition;
        });
        if (context_.session == view.physicsContext.session && context_.revision == view.physicsContext.revision &&
            context_.state == view.physicsContext.state && selection_ == view.selectionRevision && objects_ == view.selectedObjects &&
            prefab_ == view.prefabSelection && publication_ == view.prefabHierarchy && sameDefinitions)
            return;
        Clear();
        context_ = view.physicsContext;
        selection_ = view.selectionRevision;
        objects_ = view.selectedObjects;
        prefab_ = view.prefabSelection;
        publication_ = view.prefabHierarchy;
        definitions_ = view.navigationDefinitions;
        if (prefab_ && prefab_->member && publication_ && publication_->sources) {
            for (const auto &instance : publication_->sources->instances) {
                if (instance.authored.instanceId != prefab_->instance || !instance.IsSynchronized())
                    continue;
                for (const auto &object : instance.expanded->Objects()) {
                    Runtime::RuntimeComponentSet components;
                    std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
                    for (const auto &raw : object.object.components) {
                        const auto projected = SceneSource::ProjectPrefabBuiltinComponent(raw, components, primitive);
                        if (projected.HasError()) {
                            drafts_.clear();
                            return;
                        }
                    }
                    if (components.navigationSurface)
                        surfaces_.push_back(*components.navigationSurface);
                    if (object.key == *prefab_->member)
                        Capture(drafts_, {}, components, object.object.name);
                }
            }
            return;
        }
        for (const auto &object : view.objects)
            if (object.components.navigationSurface)
                surfaces_.push_back(*object.components.navigationSurface);
        for (const auto id : objects_) {
            const auto found = std::ranges::find(view.objects, id, &SceneObject::id);
            if (found != view.objects.end())
                Capture(drafts_, id, found->components, found->name);
        }
    }

    /** @copydoc NavigationInspectorSession::Clear */
    void NavigationInspectorSession::Clear() noexcept {
        drafts_.clear();
        surfaces_.clear();
        objects_.clear();
        definitions_.clear();
        prefab_.reset();
        publication_.reset();
        context_ = {};
        selection_ = 0;
    }

    /** @copydoc NavigationInspectorSession::Cancel */
    void NavigationInspectorSession::Cancel() {
        for (auto &draft : drafts_)
            draft.edit.value = draft.baseline;
    }

    /** @copydoc NavigationInspectorSession::Dirty */
    bool NavigationInspectorSession::Dirty() const {
        return std::ranges::any_of(drafts_, [](const auto &draft) {
            return draft.edit.value != draft.baseline;
        });
    }

    /** @copydoc NavigationInspectorSession::Request */
    NavigationInspectorRequest NavigationInspectorSession::Request() const {
        NavigationInspectorRequest request{context_, selection_, objects_, prefab_, publication_, definitions_, {}};
        for (const auto &draft : drafts_)
            if (draft.edit.value != draft.baseline)
                request.edits.push_back(draft.edit);
        return request;
    }
}  // namespace Horo::Editor
