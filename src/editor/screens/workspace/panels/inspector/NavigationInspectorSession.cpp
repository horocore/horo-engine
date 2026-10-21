#include "NavigationInspectorSession.h"

#include "Horo/Scene/PrefabComponentProjection.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Copy only actual typed navigation occurrences, preserving the selected object's unrelated content. */
        template <class Components>
        void Capture(std::vector<NavigationInspectorDraft> &drafts, SceneObjectId object, const Components &components,
                     const std::string &name) {
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

        /** @brief Decode one synchronized instance while its immutable source projection remains pinned. */
        bool CaptureInstance(std::vector<NavigationInspectorDraft> &drafts, std::vector<Runtime::NavigationSurfaceComponent> &surfaces,
                             const SceneSource::ScenePrefabInstanceProjection &instance, const PrefabHierarchyTarget &target) {
            for (const auto &object : instance.expanded->Objects()) {
                Runtime::RuntimeComponentSet components;
                std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
                for (const auto &raw : object.object.components) {
                    if (const auto projected = SceneSource::ProjectPrefabBuiltinComponent(raw, components, primitive);
                        projected.HasError()) {
                        drafts.clear();
                        return false;
                    }
                }
                if (components.navigationSurface)
                    surfaces.push_back(*components.navigationSurface);
                if (object.key == *target.member)
                    Capture(drafts, {}, components, object.object.name);
            }
            return true;
        }

        /** @brief Retain only the selected synchronized Prefab instance's typed drafts and surface inventory. */
        void CapturePrefab(std::vector<NavigationInspectorDraft> &drafts, std::vector<Runtime::NavigationSurfaceComponent> &surfaces,
                           const PrefabHierarchyProjection &publication, const PrefabHierarchyTarget &target) {
            for (const auto &instance : publication.sources->instances) {
                if (instance.authored.instanceId != target.instance || !instance.IsSynchronized())
                    continue;
                if (!CaptureInstance(drafts, surfaces, instance, target))
                    return;
            }
        }
    }  // namespace

    /** @copydoc NavigationInspectorSession::Begin */
    void NavigationInspectorSession::Begin(const EditorWorkspaceViewModel &view) {
        if (const bool sameDefinitions =
                definitions_.size() == view.navigationDefinitions.size() && std::ranges::equal(definitions_, view.navigationDefinitions,
                                                                                               [](const auto &a, const auto &b) {
            return a.asset == b.asset && a.session == b.session && a.revision == b.revision && a.definition == b.definition;
        });
            context_.session == view.physicsContext.session && context_.revision == view.physicsContext.revision &&
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
            CapturePrefab(drafts_, surfaces_, *publication_, *prefab_);
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
