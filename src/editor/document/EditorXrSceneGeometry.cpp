#include "editor/renderer/xr/EditorXrSceneGeometry.h"

#include "EditorRenderExtractionErrors.h"
#include "editor/document/EditorViewportSceneExtractor.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace Horo::Editor {
    namespace XrSceneDetail {
        struct SourceState final {
            const SceneDocument *document{};
            Runtime::PrimitiveMeshCache *primitives{};
            const EditorAssetMeshCache *assets{};
            std::thread::id lane;
            bool alive{true};
        };
    }  // namespace XrSceneDetail

    namespace {
        constexpr std::size_t NoParent = std::numeric_limits<std::size_t>::max();

        /** @brief Preserves a typed extraction failure without granting camera or Scene authority. */
        Error GeometryError(const char *message) {
            return MakeError(ViewportSceneErrors::InvalidResult, message);
        }

        /** @brief Checks the weak lifetime and lane BEFORE accessing any borrowed workspace parent. */
        Result<std::shared_ptr<XrSceneDetail::SourceState>> BorrowSource(const std::weak_ptr<XrSceneDetail::SourceState> &weak) {
            auto state = weak.lock();
            if (!state || state->lane != std::this_thread::get_id() || !state->alive)
                return Result<std::shared_ptr<XrSceneDetail::SourceState>>::Failure(
                    GeometryError("XR geometry workspace owner is retired or on another lane."));
            return Result<std::shared_ptr<XrSceneDetail::SourceState>>::Success(std::move(state));
        }
    }  // namespace

    struct PreparedEditorXrGeometry::Impl final {
        struct Node {
            Runtime::EntityRef entity;
            std::optional<Runtime::EntityRef> parent;
            std::optional<Runtime::SceneObjectId> authored;
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
            std::size_t parentIndex{NoParent};
        };

        std::weak_ptr<XrSceneDetail::SourceState> source;
        DocumentRevision revision;
        DocumentStateId state;
        std::uint32_t assetGeneration{};
        EditorViewportSceneSnapshot snapshot;
        std::vector<Node> nodes;
        std::vector<std::optional<Runtime::EntityRef>> slots;
        std::vector<std::size_t> order;
        std::vector<std::size_t> instances;
        std::vector<Math::Mat4> worlds;
        bool active{true};

        /** @brief Captures and orders bounded candidate topology cold, rejecting parents and cycles before publication. */
        Result<void> CaptureHierarchy(const Runtime::RuntimeSceneView candidate) {
            slots.resize(candidate.SlotCount());
            nodes.reserve(candidate.SlotCount());
            for (std::size_t slot = 0; slot < candidate.SlotCount(); ++slot) {
                auto entity = candidate.EntityAt(slot);
                if (!entity)
                    continue;
                slots[slot] = entity->entity;
                nodes.push_back({entity->entity, entity->parent, entity->authoredObject, *entity->primitiveMesh});
            }
            for (auto &node : nodes) {
                if (!node.parent)
                    continue;
                const auto parent = std::ranges::find(nodes, *node.parent, &PreparedEditorXrGeometry::Impl::Node::entity);
                if (parent == nodes.end())
                    return Result<void>::Failure(GeometryError("XR geometry parent is outside the candidate Scene."));
                node.parentIndex = static_cast<std::size_t>(parent - nodes.begin());
            }
            std::vector<bool> ordered(nodes.size());
            order.reserve(nodes.size());
            while (order.size() != nodes.size()) {
                const auto previous = order.size();
                for (std::size_t index = 0; index < nodes.size(); ++index) {
                    if (!ordered[index] && (nodes[index].parentIndex == NoParent || ordered[nodes[index].parentIndex])) {
                        ordered[index] = true;
                        order.push_back(index);
                    }
                }
                if (order.size() == previous)
                    return Result<void>::Failure(GeometryError("XR geometry candidate hierarchy contains a cycle."));
            }
            return Result<void>::Success();
        }

        /** @brief Maps exact authored draw IDs to retained candidate generations without retaining component borrows. */
        Result<void> MapInstances(const Runtime::RuntimeSceneView candidate) {
            worlds.resize(nodes.size());
            instances.reserve(snapshot.instanceObjects.size());
            for (std::size_t index = 0; index < snapshot.instanceObjects.size(); ++index) {
                const auto object = snapshot.instanceObjects[index];
                if (std::ranges::find(std::span(snapshot.instanceObjects).first(index), object) !=
                    std::span(snapshot.instanceObjects).first(index).end())
                    return Result<void>::Failure(GeometryError("XR geometry has duplicate authored draw identities."));
                const auto entity = candidate.Find(Runtime::SceneObjectId{object.value});
                if (!entity)
                    return Result<void>::Failure(GeometryError("XR geometry authored object is absent from the candidate Scene."));
                const auto node = std::ranges::find(nodes, *entity, &PreparedEditorXrGeometry::Impl::Node::entity);
                if (node == nodes.end())
                    return Result<void>::Failure(GeometryError("XR geometry candidate mapping is stale."));
                instances.push_back(static_cast<std::size_t>(node - nodes.begin()));
            }
            return Result<void>::Success();
        }
    };

    PreparedEditorXrGeometry::PreparedEditorXrGeometry(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    PreparedEditorXrGeometry::~PreparedEditorXrGeometry() = default;

    /** @copydoc PreparedEditorXrGeometry::ValidateSource */
    Result<void> PreparedEditorXrGeometry::ValidateSource() const {
        auto source = BorrowSource(impl_->source);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        if (!impl_->active)
            return Result<void>::Failure(GeometryError("XR geometry preparation was revoked."));
        if (source.Value()->document->Revision() != impl_->revision || source.Value()->document->State() != impl_->state ||
            source.Value()->assets->Generation() != impl_->assetGeneration)
            return Result<void>::Failure(GeometryError("XR geometry authored source changed after preparation."));
        return Result<void>::Success();
    }

    /** @copydoc PreparedEditorXrGeometry::Resources */
    std::span<const Render::RenderMeshResourceView> PreparedEditorXrGeometry::Resources() const noexcept {
        return impl_->snapshot.meshResources;
    }

    /** @copydoc PreparedEditorXrGeometry::Instances */
    std::span<const Render::RenderStaticMeshInstance> PreparedEditorXrGeometry::Instances() const noexcept {
        return impl_->snapshot.instances;
    }

    /** @copydoc PreparedEditorXrGeometry::Update */
    Result<std::span<const Render::RenderStaticMeshInstance>> PreparedEditorXrGeometry::Update(const Runtime::RuntimeSceneView scene) {
        using Output = Result<std::span<const Render::RenderStaticMeshInstance>>;
        const auto valid = ValidateSource();
        if (valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (scene.RuntimeId() != impl_->snapshot.runtimeSceneId || scene.SlotCount() != impl_->slots.size())
            return Output::Failure(GeometryError("XR geometry candidate Scene identity or topology changed."));
        // PERF: Cold slot/node/order arrays bound all normal-frame work; no lookup, hierarchy recursion or allocation.
        for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
            const auto entity = scene.EntityAt(index);
            if (entity.has_value() != impl_->slots[index].has_value() || (entity && entity->entity != *impl_->slots[index]))
                return Output::Failure(GeometryError("XR geometry candidate entity generation changed."));
        }
        for (const std::size_t index : impl_->order) {
            const auto &node = impl_->nodes[index];
            auto borrowed = scene.Get(node.entity);
            if (borrowed.HasError())
                return Output::Failure(borrowed.ErrorValue());
            const auto &entity = borrowed.Value();
            if (entity.parent != node.parent || entity.authoredObject != node.authored || *entity.primitiveMesh != node.primitive)
                return Output::Failure(GeometryError("XR geometry mapping or mesh shape changed."));
            const auto local = entity.localTransform->ToMatrix();
            impl_->worlds[index] = node.parentIndex == NoParent ? local : Math::Multiply(impl_->worlds[node.parentIndex], local);
            if (!Math::IsFinite(impl_->worlds[index]))
                return Output::Failure(GeometryError("XR geometry transform is not finite."));
        }
        // Commit only after every mapping and transform passed; an error cannot publish a partially updated instance span.
        for (std::size_t index = 0; index < impl_->instances.size(); ++index)
            impl_->snapshot.instances[index].localToWorld = impl_->worlds[impl_->instances[index]];
        return Output::Success(impl_->snapshot.instances);
    }

    /** @copydoc PreparedEditorXrGeometry::Revoke */
    void PreparedEditorXrGeometry::Revoke() noexcept {
        impl_->active = false;
    }

    /** @copydoc EditorXrSceneSourcePort::Capture */
    Result<std::unique_ptr<PreparedEditorXrGeometry>> EditorXrSceneSourcePort::Capture(const Runtime::SceneDefinitionRevision revision,
                                                                                       const Runtime::RuntimeSceneView candidate) const {
        using Output = Result<std::unique_ptr<PreparedEditorXrGeometry>>;
        auto source = BorrowSource(state_);
        if (source.HasError())
            return Output::Failure(source.ErrorValue());
        const auto &owner = *source.Value();
        // RuntimeSceneConversion binds definition revisions to content-state IDs, not the editor's monotonic command revision.
        if (!candidate.RuntimeId().IsValid() || candidate.DefinitionRevision() != revision ||
            owner.document->State().value != revision.value || candidate.SlotCount() > PreparedEditorXrGeometry::MaximumSceneSlots)
            return Output::Failure(GeometryError("XR geometry revision, candidate or cold Scene capacity is invalid."));
        const auto document = owner.document->Snapshot();
        auto extracted = ExtractEditorViewportGeometry(candidate, document.revision, *owner.primitives, &document, owner.assets, false,
                                                       {PreparedEditorXrGeometry::MaximumInstances, true});
        if (extracted.HasError())
            return Output::Failure(extracted.ErrorValue());
        auto impl = std::make_unique<PreparedEditorXrGeometry::Impl>();
        impl->source = state_;
        impl->revision = document.revision;
        impl->state = document.state;
        impl->assetGeneration = owner.assets->Generation();
        impl->snapshot = std::move(extracted).Value();
        if (auto hierarchy = impl->CaptureHierarchy(candidate); hierarchy.HasError())
            return Output::Failure(hierarchy.ErrorValue());
        if (auto mapped = impl->MapInstances(candidate); mapped.HasError())
            return Output::Failure(mapped.ErrorValue());
        auto prepared = std::unique_ptr<PreparedEditorXrGeometry>(new PreparedEditorXrGeometry(std::move(impl)));
        const auto valid = prepared->Update(candidate);
        if (valid.HasError())
            return Output::Failure(valid.ErrorValue());
        return Output::Success(std::move(prepared));
    }

    /** @copydoc EditorXrSceneSourceOwner::EditorXrSceneSourceOwner */
    EditorXrSceneSourceOwner::EditorXrSceneSourceOwner(const SceneDocument &document, Runtime::PrimitiveMeshCache &primitives,
                                                       const EditorAssetMeshCache &assets)
        : state_(std::make_shared<XrSceneDetail::SourceState>(
              XrSceneDetail::SourceState{&document, &primitives, &assets, std::this_thread::get_id()})) {}

    EditorXrSceneSourceOwner::~EditorXrSceneSourceOwner() {
        if (state_->lane != std::this_thread::get_id())
            std::terminate();
        state_->alive = false;
        state_->document = nullptr;
        state_->primitives = nullptr;
        state_->assets = nullptr;
    }

    /** @copydoc EditorXrSceneSourceOwner::Port */
    EditorXrSceneSourcePort EditorXrSceneSourceOwner::Port() const noexcept {
        EditorXrSceneSourcePort port;
        port.state_ = state_;
        return port;
    }
}  // namespace Horo::Editor
