#include "HeadlessInitialStateAuthority.h"
#include "HeadlessNetworkWorldService.h"
#include "HeadlessPrefabIntent.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using HeadlessNetworkDetail::WorldRecord;
        using HeadlessNetworkDetail::WorldService;
        namespace Net = Network;
        using InitialCapture = Result<std::optional<Net::ReplicationInitialStateSnapshot>>;
        constexpr auto FlushPhase = Runtime::RuntimePhase::NetworkFlush;

        /** @brief Only the exact delayed Scene commitment is retryable during this stream. */
        InitialCapture InitialCaptureFailure(const Error &error) {
            const auto &pending = Net::ReplicationCaptureErrors::Uncommitted;
            if (error.domain.Value() == pending.domain.Value() && error.code.Value() == pending.code.Value())
                return InitialCapture::Success(std::nullopt);
            return InitialCapture::Failure(error);
        }

        /** @brief Finds the exact already-published authority record without retaining retired worlds. */
        std::shared_ptr<WorldRecord> FindInitialAuthority(const HeadlessNetworkDetail::WorldOwners &worlds, Runtime::SceneRuntimeId scene) {
            if (worlds.owner != std::this_thread::get_id() || worlds.retirementStarted)
                return {};
            for (const auto &candidate : worlds.known) {
                auto record = candidate.lock();
                if (record && record->request.scene == scene && record->request.world == Net::NetworkModeWorldKind::AuthorityServer)
                    return record->closeRequested ? nullptr : std::move(record);
            }
            return {};
        }

        /** @brief Initial state needs one full projection per current mapping row, all from the same grant. */
        Result<std::vector<std::shared_ptr<Net::ReplicationStateCodec>>> ProjectInitialCodecs(
            const Net::ReplicationWorldReadLease &world, const HeadlessInitialRecipient &grant,
            const std::shared_ptr<const Net::ReplicationSerializerRegistry> &serializers) {
            using Codecs = Result<std::vector<std::shared_ptr<Net::ReplicationStateCodec>>>;
            const auto entries = world.Mapping().Entries();
            std::vector<std::shared_ptr<Net::ReplicationStateCodec>> codecs;
            codecs.reserve(entries.size());
            for (std::size_t index{}; index < entries.size(); ++index) {
                const auto role =
                    Net::ProjectInitialReplicationRole(grant.ownership[index], entries[index], world.Descriptor().session, grant.peer);
                if (role.HasError())
                    return Codecs::Failure(role.ErrorValue());
                auto codec = Net::ReplicationStateCodec::Create(serializers, role.Value(), Net::ReplicationRecordKind::Spawn,
                                                                grant.descriptorGeneration);
                if (codec.HasError())
                    return Codecs::Failure(std::move(codec).ErrorValue());
                codecs.emplace_back(std::move(codec).Value());
            }
            return Codecs::Success(std::move(codecs));
        }

        /** @brief Samples every exact eligible row of this newly requested initial inventory at one canonical commit. */
        InitialCapture CaptureInitialTransforms(const WorldRecord &record, const Net::ReplicationWorldReadLease &world,
                                                const Net::ReplicationWorldLifecycle &lifecycle, const HeadlessInitialRecipient &grant,
                                                std::size_t maximumWireBytes) {
            const auto cancellation = world.Cancellation();
            const auto &source = record.publicTransforms;
            const auto read = source->AcquireCaptureReadAfterCommit(lifecycle, FlushPhase, cancellation);
            if (read.HasError())
                return InitialCaptureFailure(read.ErrorValue());
            if (read.Value().Descriptor() != world.Descriptor() || read.Value().MappingRevision() != world.MappingRevision())
                return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            const auto entries = world.Mapping().Entries();
            const auto tick = read.Value().SimulationTick();
            if (entries.empty())
                return InitialCapture::Success(
                    Net::ReplicationInitialStateSnapshot{world.Descriptor().session, world.Descriptor().authority, tick});
            Net::ReplicationCaptureLimits limits;
            if (entries.size() > limits.maximumTargets)
                return InitialCapture::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
            limits.maximumTargetsPerTick = entries.size();
            const auto *installed = record.activeGameplay->Replication();
            if (!installed)
                return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
            const auto lease = std::make_shared<const Gameplay::GameplayReplicationLease>(*installed);
            const auto serializers = std::shared_ptr<const Net::ReplicationSerializerRegistry>{lease, &lease->Serializers()};
            auto codecs = ProjectInitialCodecs(world, grant, serializers);
            if (codecs.HasError())
                return InitialCapture::Failure(std::move(codecs).ErrorValue());
            std::vector<Net::ReplicationCaptureTarget> targets;
            targets.reserve(entries.size());
            for (const auto &entry : entries)
                targets.push_back({entry, source});
            auto capture = Net::ReplicationStateCapture::Prepare(read.Value(), serializers, targets, limits);
            if (capture.HasError())
                return InitialCapture::Failure(std::move(capture).ErrorValue());
            const auto report = capture.Value()->CaptureAtCommit(read.Value(), tick, cancellation);
            if (report.HasError())
                return InitialCapture::Failure(report.ErrorValue());
            if (const auto &error = report.Value().firstError; error)
                return InitialCaptureFailure(*error);
            if (report.Value().published != entries.size())
                return InitialCapture::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
            auto encoded =
                Net::EncodeReplicationInitialState(world, *capture.Value(), tick, codecs.Value(), maximumWireBytes, cancellation);
            return encoded.HasError() ? InitialCapture::Failure(std::move(encoded).ErrorValue())
                                      : InitialCapture::Success(std::move(encoded).Value());
        }

        /** @brief Validates the complete transform field set and its actual installed scalar serializers. */
        bool MatchesPublicTransformFields(const HeadlessSceneTransformReplication &selection,
                                          const Gameplay::GameplayReplicationLease &installed) {
            const auto schema = installed.Descriptors()->Find(selection.schema);
            if (schema.HasError() || schema.Value()->version != selection.version ||
                schema.Value()->fields.size() != selection.fields.size())
                return false;

            return std::ranges::all_of(selection.fields, [&](const auto &field) {
                const auto descriptor = installed.Serializers().DescriptorFor(selection.schema, field.field);
                return descriptor.HasValue() && descriptor.Value()->valueKind == Network::ReplicationValueKind::FloatingPoint;
            });
        }

        /** @brief Resolves one object against the committed Scene and the admitted live schema registry. */
        Result<Network::ReplicationAuthoredSceneObject> ResolveAuthoredObject(const Runtime::RuntimeSceneView &scene,
                                                                              const Network::NetworkObjectProvenance &binding,
                                                                              std::span<const Network::ReplicationSchemaId> admitted,
                                                                              const Network::ReplicationDescriptorSnapshot &installed) {
            using Row = Result<Network::ReplicationAuthoredSceneObject>;
            if (!binding.IsValid() || !binding.authoredObject)
                return Row::Failure(MakeError(Network::NetworkErrors::NetworkObjectMappingInvalid));
            const auto schema = installed.Find(binding.schema);
            if (schema.HasError() || schema.Value()->version != binding.schemaVersion ||
                std::ranges::find(admitted, binding.schema) == admitted.end())
                return Row::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
            const auto entity = scene.Find(*binding.authoredObject);
            if (!entity)
                return Row::Failure(MakeError(Network::NetworkErrors::ReplicationWorldStale));
            return Row::Success({*entity, binding});
        }

        /** @brief Copies the finite admitted projection without retaining a Scene borrow. */
        Result<Network::NetworkAuthoredSceneProjection> ProjectAuthoredObjects(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Network::NetworkObjectProvenance> bindings,
                                                                               std::span<const Network::ReplicationSchemaId> admitted,
                                                                               const Network::ReplicationDescriptorSnapshot &installed,
                                                                               const CancellationToken &cancellation) {
            using Outcome = Result<Network::NetworkAuthoredSceneProjection>;
            if (bindings.size() > Network::ReplicationWorldLimits{}.maximumObjects)
                return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
            try {
                Network::NetworkAuthoredSceneProjection projection{.cancellation = cancellation};
                projection.objects.reserve(bindings.size());
                for (const auto &binding : bindings) {
                    auto row = ResolveAuthoredObject(scene, binding, admitted, installed);
                    if (row.HasError())
                        return Outcome::Failure(row.ErrorValue());
                    projection.objects.push_back(std::move(row).Value());
                }
                return Outcome::Success(std::move(projection));
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
            }
        }

        /** @brief Counts enabled registered owners on one Scene entity. Services have no entity occurrence. */
        std::size_t CountEntityOwners(const Runtime::RuntimeComponentSet &components, const Gameplay::GameplayReplicationOwner &owner) {
            if (const auto *behavior = std::get_if<Gameplay::BehaviorTypeId>(&owner))
                return static_cast<std::size_t>(std::ranges::count_if(components.behaviors, [&](const auto &binding) {
                    return binding.enabled && binding.typeId == *behavior;
                }));
            if (const auto *component = std::get_if<Gameplay::ComponentTypeId>(&owner))
                return static_cast<std::size_t>(
                    std::ranges::count(components.gameplayComponents, *component, &Gameplay::SerializedComponent::typeId));
            return 0;
        }

        struct EntitySchemaBinding final {
            const Gameplay::GameplayReplicationOwner *owner;
            const Network::ReplicationSchemaDescriptor *schema;
        };

        /** @brief Pins only registered and project-declared object schemas for this synchronous projection. */
        Result<std::vector<EntitySchemaBinding>> AdmitEntitySchemas(const Gameplay::GameplayReplicationLease &installed,
                                                                    std::span<const Network::ReplicationSchemaId> schemas) {
            using Bindings = Result<std::vector<EntitySchemaBinding>>;
            std::vector<EntitySchemaBinding> admitted;
            admitted.reserve(schemas.size());
            for (const auto &registration : installed.Registrations()) {
                if (std::holds_alternative<Gameplay::GameplayServiceId>(registration.owner))
                    continue;
                if (std::ranges::find(schemas, registration.schema) == schemas.end())
                    continue;
                auto schema = installed.Descriptors()->Find(registration.schema);
                if (schema.HasError())
                    return Bindings::Failure(schema.ErrorValue());
                // The registry was already checked by the role-specific project admission.
                admitted.push_back({&registration.owner, schema.Value()});
            }
            return Bindings::Success(std::move(admitted));
        }

        /** @brief Locates the sole admitted replication owner for one Scene entity. */
        Result<const Network::ReplicationSchemaDescriptor *> SelectEntitySchema(const Runtime::RuntimeComponentSet &components,
                                                                                std::span<const EntitySchemaBinding> bindings) {
            using Schema = Result<const Network::ReplicationSchemaDescriptor *>;
            const Network::ReplicationSchemaDescriptor *selected{};
            for (const auto &binding : bindings) {
                const auto count = CountEntityOwners(components, *binding.owner);
                if (count == 0)
                    continue;
                if (selected || count > 1)
                    return Schema::Failure(MakeError(Network::NetworkErrors::NetworkObjectMappingConflict,
                                                     "A network object must have one declared replication owner."));
                selected = binding.schema;
            }
            return Schema::Success(selected);
        }

        /** @brief Copies only authored Scene entities with a unique admitted replication owner. */
        Result<void> ProjectDiscoveredObjects(const Runtime::RuntimeSceneView &scene, std::span<const EntitySchemaBinding> bindings,
                                              Network::NetworkAuthoredSceneProjection &projection) {
            const auto slots = scene.SlotCount();
            for (std::size_t slot = 0; slot < slots; ++slot) {
                if (projection.cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCancelled));
                const auto entity = scene.EntityAt(slot);
                if (!entity || !entity->authoredObject)
                    continue;
                auto schema = SelectEntitySchema(*entity->components, bindings);
                if (schema.HasError())
                    return Result<void>::Failure(schema.ErrorValue());
                if (const auto *selected = schema.Value()) {
                    if (projection.objects.size() == Network::ReplicationWorldLimits{}.maximumObjects)
                        return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
                    projection.objects.push_back({entity->entity, {selected->id, selected->version, entity->authoredObject}});
                }
            }
            return Result<void>::Success();
        }

        /** @brief Resolves the admitted schema set before walking the committed Scene. */
        Result<Network::NetworkAuthoredSceneProjection> DiscoverAuthoredObjects(const Runtime::RuntimeSceneView &scene,
                                                                                const Gameplay::GameplayReplicationLease &installed,
                                                                                std::span<const Network::ReplicationSchemaId> schemas,
                                                                                const CancellationToken &cancellation) {
            using Outcome = Result<Network::NetworkAuthoredSceneProjection>;
            try {
                auto admitted = AdmitEntitySchemas(installed, schemas);
                if (admitted.HasError())
                    return Outcome::Failure(admitted.ErrorValue());
                Network::NetworkAuthoredSceneProjection projection{.cancellation = cancellation};
                if (!admitted.Value().empty()) {
                    if (auto copied = ProjectDiscoveredObjects(scene, admitted.Value(), projection); copied.HasError())
                        return Outcome::Failure(copied.ErrorValue());
                }
                return Outcome::Success(std::move(projection));
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
            }
        }

        /** @brief Admitted schema and exact position in the verified cooked entity order. */
        struct PrefabMemberSchema final {
            std::uint32_t index;
            Network::ReplicationSchemaId schema;
            Network::ReplicationSchemaVersion version;
        };

        /** @brief Retains the projected members through both structural publications of this occurrence. */
        class AuthorityPrefabPublication final : public Runtime::SceneGroupPublication {
        public:
            AuthorityPrefabPublication(Network::NetworkAuthorityObjectPublication authority, CancellationToken cancellation,
                                       Network::NetworkPrefabTemplateIdentity recipe, std::vector<PrefabMemberSchema> schemas,
                                       std::shared_ptr<const std::vector<std::byte>> intent)
                : authority_(std::move(authority)), cancellation_(std::move(cancellation)), recipe_(recipe), schemas_(std::move(schemas)),
                  intent_(std::move(intent)) {}

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &,
                                                                               std::span<const Runtime::EntityRef> members,
                                                                               bool creating) const override {
                using Outcome = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
                if (members.size() != recipe_.entityCount)
                    return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldInvalid));
                if (!creating) {
                    std::vector<Runtime::EntityRef> retired;
                    retired.reserve(schemas_.size());
                    for (const auto &schema : schemas_)
                        retired.push_back(members[schema.index]);
                    return authority_.Prepare({}, retired, cancellation_);
                }
                std::vector<Network::ReplicationDynamicSceneObject> added;
                added.reserve(schemas_.size());
                for (const auto &schema : schemas_)
                    added.push_back({members[schema.index], schema.schema, schema.version,
                                     Network::NetworkPrefabMemberSource{recipe_, schema.index}, added.empty() ? intent_ : nullptr});
                return authority_.Prepare(added, {}, cancellation_);
            }

        private:
            Network::NetworkAuthorityObjectPublication authority_;
            CancellationToken cancellation_;
            Network::NetworkPrefabTemplateIdentity recipe_;
            std::vector<PrefabMemberSchema> schemas_;
            std::shared_ptr<const std::vector<std::byte>> intent_;
        };

        /** @brief Captures only schemas admitted by the authority module and project before a cooked group is queued. */
        class AuthorityPrefabOwner final : public Prefab::IPrefabGroupPublicationOwner {
        public:
            AuthorityPrefabOwner(Network::NetworkAuthorityObjectPublication authority, std::weak_ptr<const WorldRecord> record)
                : authority_(std::move(authority)), record_(std::move(record)) {}

            Result<std::shared_ptr<const Runtime::SceneGroupPublication>> Bind(const Prefab::PreparedPrefabSpawn &recipe,
                                                                               const Prefab::PrefabSpawnRequest &request,
                                                                               std::span<const Runtime::RuntimeComponentSet> projected,
                                                                               std::string_view) const override {
                const auto record = record_.lock();
                if (!record || record->closeRequested || !record->activeGameplay || record->sceneCancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldUnavailable));
                if (recipe.Data().entities.size() != projected.size() || !std::in_range<std::uint32_t>(projected.size()))
                    return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldInvalid));
                const auto *installed = record->activeGameplay->Replication();
                if (!installed)
                    return Outcome::Success(nullptr);
                try {
                    const Network::NetworkPrefabTemplateIdentity source{recipe.Data().assetId, recipe.Source().Artifact().Digest(),
                                                                        static_cast<std::uint32_t>(projected.size())};
                    return BindProjected(source, projected, *installed, *record, request);
                } catch (const std::bad_alloc &) {
                    return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
                }
            }

        private:
            using Outcome = Result<std::shared_ptr<const Runtime::SceneGroupPublication>>;

            /** @brief Resolves each candidate member through the same admission rules used for authored entities. */
            Outcome BindProjected(Network::NetworkPrefabTemplateIdentity recipe, std::span<const Runtime::RuntimeComponentSet> projected,
                                  const Gameplay::GameplayReplicationLease &installed, const WorldRecord &record,
                                  const Prefab::PrefabSpawnRequest &request) const {
                auto admitted = AdmitEntitySchemas(installed, record.activeGameplay->AdmittedReplicationSchemas());
                if (admitted.HasError())
                    return Outcome::Failure(admitted.ErrorValue());
                if (admitted.Value().empty())
                    return Outcome::Success(nullptr);
                std::vector<PrefabMemberSchema> members;
                for (std::uint32_t index = 0; index < recipe.entityCount; ++index) {
                    auto schema = SelectEntitySchema(projected[index], admitted.Value());
                    if (schema.HasError())
                        return Outcome::Failure(schema.ErrorValue());
                    if (const auto *selected = schema.Value())
                        members.push_back({index, selected->id, selected->version});
                }
                if (members.empty())
                    return Outcome::Success(nullptr);
                const auto scene = record.scenes->ActiveScene();
                if (!record.templates || !scene)
                    return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldUnavailable));
                auto intent = EncodeHeadlessPrefabIntent(request, record.templates->LimitProfile(), *scene, authority_);
                if (intent.HasError())
                    return Outcome::Failure(intent.ErrorValue());
                return Outcome::Success(std::make_shared<const AuthorityPrefabPublication>(authority_, record.sceneCancellation, recipe,
                                                                                           std::move(members),
                                                                                           std::make_shared<const std::vector<std::byte>>(
                                                                                               std::move(intent.Value()))));
            }

            Network::NetworkAuthorityObjectPublication authority_;
            std::weak_ptr<const WorldRecord> record_;
        };
    }  // namespace

    /** @copydoc HeadlessNetworkDetail::WorldService::BindAuthorityPrefabs */
    Result<void> WorldService::BindAuthorityPrefabs(GameplayWorldSelection &selected) const {
        if (selected.prefabPermission != GameplayPrefabPermission::Granted || !request_.authorityObjects.IsBound())
            return Result<void>::Success();
        try {
            selected.prefabGroupPublication = std::make_shared<const AuthorityPrefabOwner>(request_.authorityObjects, record_);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
        }
    }

    HeadlessInitialStateHost::HeadlessInitialStateHost(std::shared_ptr<const IHeadlessInitialStateAuthority> authority,
                                                       std::shared_ptr<Network::INetworkInitialStateHost> client) noexcept
        : authority_(std::move(authority)), client_(std::move(client)) {}

    /** @copydoc HeadlessInitialStateHost::BindWorlds */
    Result<void> HeadlessInitialStateHost::BindWorlds(const std::shared_ptr<HeadlessNetworkDetail::WorldOwners> &worlds) {
        if (!worlds || !authority_ || !client_ || bound_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::AdmissionPolicyInvalid));
        worlds_ = worlds;
        bound_ = true;
        return Result<void>::Success();
    }

    /** @copydoc HeadlessInitialStateHost::ClientRecipient */
    Result<Network::NetworkPeerId> HeadlessInitialStateHost::ClientRecipient(const Network::PeerSessionLifecycle &peer) {
        return client_->ClientRecipient(peer);
    }

    /** @copydoc HeadlessInitialStateHost::Capture */
    InitialCapture HeadlessInitialStateHost::Capture(const Net::ReplicationWorldReadLease &world,
                                                     const Net::ReplicationWorldLifecycle &lifecycle, const Net::PeerSessionLifecycle &peer,
                                                     std::size_t maximumWireBytes) {
        const auto owners = worlds_.lock();
        if (!owners || !world.HasCurrentMapping() || !world.Capabilities().publishAuthority)
            return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
        const auto record = FindInitialAuthority(*owners, world.Descriptor().scene);
        if (!record || !record->publicTransforms || !record->activeGameplay)
            return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
        const auto *authenticated = peer.Authentication();
        if (peer.State() != Net::PeerSessionState::Active || !authenticated)
            return InitialCapture::Failure(MakeError(Net::NetworkErrors::AuthenticationRejected));
        const auto evidence = *authenticated;
        try {
            const auto grant = authority_->Resolve(evidence, world);
            if (grant.HasError())
                return InitialCapture::Failure(grant.ErrorValue());
            if (!world.HasCurrentMapping() || record->closeRequested)
                return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            const auto *current = peer.Authentication();
            if (peer.State() != Net::PeerSessionState::Active || !current || *current != evidence ||
                peer.Connection() != evidence.connection)
                return InitialCapture::Failure(MakeError(Net::NetworkErrors::AuthenticationRejected));
            const auto &recipient = grant.Value();
            if (!recipient.peer.IsValid() || recipient.descriptorGeneration == 0 ||
                recipient.ownership.size() != world.Mapping().Entries().size())
                return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
            return CaptureInitialTransforms(*record, world, lifecycle, recipient, maximumWireBytes);
        } catch (const std::bad_alloc &) {
            return InitialCapture::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
        }
    }

    /** @copydoc HeadlessInitialStateHost::PrepareReceiver */
    Result<Network::NetworkInitialStateTarget> HeadlessInitialStateHost::PrepareReceiver(
        const Network::ReplicationInboundAuthority &authority, Network::ReplicationWorldLifecycle &world,
        const std::span<const Network::ReplicationInitialOwnership> ownership, const std::uint64_t now) {
        return client_->PrepareReceiver(authority, world, ownership, now);
    }

    /** @copydoc HeadlessNetworkDetail::WorldService::BindAuthoritySceneTransforms */
    Result<void> WorldService::BindAuthoritySceneTransforms() const {
        const auto &selection = owners_->configured->publicTransforms;
        if (!selection || request_.world != Network::NetworkModeWorldKind::AuthorityServer)
            return Result<void>::Success();
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
        };
        const auto *installed = record_->activeGameplay->Replication();
        const auto admitted = record_->activeGameplay->AdmittedReplicationSchemas();
        if (!owners_->fixed || !installed || std::ranges::find(admitted, selection->schema) == admitted.end() ||
            !MatchesPublicTransformFields(*selection, *installed))
            return invalid();
        auto source = Network::SceneReplicationCommitSource::CreateObserved(request_.scene, owners_->fixed->Source(), selection->schema,
                                                                            selection->version, selection->fields, record_);
        if (source.HasError())
            return Result<void>::Failure(std::move(source).ErrorValue());
        record_->publicTransforms = std::move(source).Value();
        return Result<void>::Success();
    }

    /** @copydoc HeadlessNetworkDetail::WorldService::CaptureAuthoredScene */
    Result<Network::NetworkAuthoredSceneProjection> WorldService::CaptureAuthoredScene() const {
        using Outcome = Result<Network::NetworkAuthoredSceneProjection>;
        if (!owners_->configured)
            return Outcome::Success({});
        const auto scene = ActiveScene();
        if (!scene || request_.service != Network::NetworkModeServiceKind::Scene ||
            (request_.world != Network::NetworkModeWorldKind::AuthorityServer && request_.world != Network::NetworkModeWorldKind::Client))
            return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldUnavailable));
        if (record_->sceneCancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCancelled));
        const auto &explicitBindings = owners_->configured->replicatedSceneObjects;
        const auto *installed = record_->activeGameplay ? record_->activeGameplay->Replication() : nullptr;
        if (!installed)
            return explicitBindings.empty() ? Outcome::Success({})
                                            : Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
        const auto schemas = record_->activeGameplay->AdmittedReplicationSchemas();
        if (explicitBindings.empty())
            return DiscoverAuthoredObjects(*scene, *installed, schemas, record_->sceneCancellation);
        return ProjectAuthoredObjects(*scene, explicitBindings, schemas, *installed->Descriptors(), record_->sceneCancellation);
    }

}  // namespace Horo::Application::Internal
