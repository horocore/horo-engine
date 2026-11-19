#include "HeadlessInitialStateAuthority.h"
#include "HeadlessNetworkWorldService.h"
#include "HeadlessPrefabIntent.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using HeadlessNetworkDetail::WorldOwners;
        using HeadlessNetworkDetail::WorldRecord;
        using HeadlessNetworkDetail::WorldService;
        namespace Net = Network;
        using InitialCapture = Result<std::optional<Net::ReplicationInitialStateSnapshot>>;
        using InitialReceiver = Result<Net::NetworkInitialStateTarget>;
        using UpdateCapture = Result<std::optional<Net::NetworkStateUpdateBatch>>;
        using SceneApplyOwner = Result<std::shared_ptr<Net::SceneReplicationApplyOwner>>;
        constexpr auto FlushPhase = Runtime::RuntimePhase::NetworkFlush;
        constexpr auto CommitPhase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;

        /** @brief Only the exact delayed Scene commitment is retryable during this stream. */
        InitialCapture InitialCaptureFailure(const Error &error) {
            const auto &pending = Net::ReplicationCaptureErrors::Uncommitted;
            if (error.domain.Value() == pending.domain.Value() && error.code.Value() == pending.code.Value())
                return InitialCapture::Success(std::nullopt);
            return InitialCapture::Failure(error);
        }

        /** @brief Finds the exact already-published role record without retaining retired worlds. */
        std::shared_ptr<WorldRecord> FindInitialWorld(const HeadlessNetworkDetail::WorldOwners &worlds, Runtime::SceneRuntimeId scene,
                                                      Net::NetworkModeWorldKind kind) {
            if (worlds.owner != std::this_thread::get_id() || worlds.retirementStarted)
                return {};
            for (const auto &candidate : worlds.known) {
                auto record = candidate.lock();
                if (record && record->request.scene == scene && record->request.world == kind)
                    return record->closeRequested ? nullptr : std::move(record);
            }
            return {};
        }

        /** @brief Detects retirement or replacement even if a host callback supplied another active session in place. */
        bool SameInitialAuthentication(const Net::PeerSessionLifecycle &peer, const Net::AuthenticationResult &expected) {
            const auto *current = peer.Authentication();
            return peer.State() == Net::PeerSessionState::Active && current && *current == expected &&
                   peer.Connection() == expected.connection;
        }

        /** @brief Resolves and rechecks the live remote recipient before opening canonical capture. */
        Result<HeadlessInitialRecipient> ResolveAuthorityGrant(const IHeadlessInitialStateAuthority &authority, const WorldRecord &record,
                                                               const Net::ReplicationWorldReadLease &world,
                                                               const Net::PeerSessionLifecycle &peer) {
            using Grant = Result<HeadlessInitialRecipient>;
            const auto *authenticated = peer.Authentication();
            if (peer.State() != Net::PeerSessionState::Active || !authenticated)
                return Grant::Failure(MakeError(Net::NetworkErrors::AuthenticationRejected));
            const auto evidence = *authenticated;
            auto grant = authority.Resolve(evidence, world);
            if (grant.HasError())
                return grant;
            if (!world.HasCurrentMapping() || record.closeRequested)
                return Grant::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            if (!SameInitialAuthentication(peer, evidence))
                return Grant::Failure(MakeError(Net::NetworkErrors::AuthenticationRejected));
            const auto &recipient = grant.Value();
            if (!recipient.peer.IsValid() || recipient.descriptorGeneration == 0 ||
                recipient.ownership.size() != world.Mapping().Entries().size())
                return Grant::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
            return grant;
        }

        /** @brief Current replica scope held only while preparing this one structural transaction. */
        struct InitialReplicaRead final {
            std::shared_ptr<WorldRecord> record;
            Net::ReplicationWorldReadLease mapping;
            Runtime::RuntimeDispatchEvidence dispatch;
        };

        /** @brief Acquires the actual committed replica mapping from the canonical live structural dispatch. */
        Result<InitialReplicaRead> OpenInitialReplica(const HeadlessNetworkDetail::WorldOwners &owners,
                                                      const Net::ReplicationInboundAuthority &authority,
                                                      const Net::ReplicationWorldLifecycle &world, std::size_t expectedObjects) {
            using Read = Result<InitialReplicaRead>;
            const auto record = FindInitialWorld(owners, authority.scene, Net::NetworkModeWorldKind::Client);
            if (!record || !record->scenes || !record->activeGameplay)
                return Read::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            const auto dispatch = owners.fixed->SceneDispatch();
            Runtime::RuntimeDispatchFacts facts;
            if (dispatch.Read(owners.fixed->Source(), CommitPhase, facts) != Runtime::RuntimeDispatchStatus::Valid)
                return Read::Failure(MakeError(Net::NetworkErrors::ReplicationWorldPhaseInvalid));
            auto mapping = world.Acquire({authority.scene, authority.networkSession, CommitPhase, facts.committedTick + 1});
            if (mapping.HasError())
                return Read::Failure(std::move(mapping).ErrorValue());
            if (expectedObjects != mapping.Value().Mapping().Entries().size())
                return Read::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
            return Read::Success({record, std::move(mapping).Value(), dispatch});
        }

        /** @brief Resolves the exact negotiated descriptor from the same live admitted replica session. */
        Result<HeadlessInitialRecipient> ResolveReplicaGrant(const IHeadlessInitialReplicaAuthority &replica,
                                                             const InitialReplicaRead &read,
                                                             const Net::ReplicationInboundAuthority &authority,
                                                             std::span<const Net::ReplicationInitialOwnership> ownership,
                                                             std::uint64_t now) {
            using Grant = Result<HeadlessInitialRecipient>;
            const auto &peer = authority.session;
            if (!peer || peer->AdmitGameplay(authority.connection, authority.generation, now).HasError() || !peer->Authentication() ||
                !peer->Negotiation())
                return Grant::Failure(MakeError(Net::NetworkErrors::AuthenticationRejected));
            const auto accepted = *peer->Authentication();
            const auto negotiated = *peer->Negotiation();
            const auto generation = replica.DescriptorGeneration(accepted, negotiated);
            if (generation.HasError())
                return Grant::Failure(generation.ErrorValue());
            if (!SameInitialAuthentication(*peer, accepted))
                return Grant::Failure(MakeError(Net::NetworkErrors::AuthenticationRejected));
            if (!read.mapping.HasCurrentMapping() || read.record->closeRequested)
                return Grant::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            if (generation.Value() == 0)
                return Grant::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
            return Grant::Success({authority.localRecipient, generation.Value(), {ownership.begin(), ownership.end()}});
        }

        /** @brief Pins the very same native module and immutable serializer generation selected by client or server activation. */
        std::shared_ptr<const Net::ReplicationSerializerRegistry> PinInitialSerializers(const WorldRecord &record) {
            const auto *installed = record.activeGameplay ? record.activeGameplay->Replication() : nullptr;
            if (!installed)
                return {};
            const auto lease = std::make_shared<const Gameplay::GameplayReplicationLease>(*installed);
            return {lease, &lease->Serializers()};
        }

        /** @brief Initial state needs one full projection per current mapping row, all from the same grant. */
        Result<std::vector<std::shared_ptr<Net::ReplicationStateCodec>>> ProjectReplicaCodecs(
            const Net::ReplicationWorldReadLease &world, const HeadlessInitialRecipient &grant,
            const std::shared_ptr<const Net::ReplicationSerializerRegistry> &serializers, Net::ReplicationRecordKind recordKind) {
            using Codecs = Result<std::vector<std::shared_ptr<Net::ReplicationStateCodec>>>;
            const auto entries = world.Mapping().Entries();
            std::vector<std::shared_ptr<Net::ReplicationStateCodec>> codecs;
            codecs.reserve(entries.size());
            for (std::size_t index{}; index < entries.size(); ++index) {
                const auto role =
                    Net::ProjectInitialReplicationRole(grant.ownership[index], entries[index], world.Descriptor().session, grant.peer);
                if (role.HasError())
                    return Codecs::Failure(role.ErrorValue());
                auto codec = Net::ReplicationStateCodec::Create(serializers, role.Value(), recordKind, grant.descriptorGeneration);
                if (codec.HasError())
                    return Codecs::Failure(std::move(codec).ErrorValue());
                codecs.emplace_back(std::move(codec).Value());
            }
            return Codecs::Success(std::move(codecs));
        }

        /** @brief Prepares one exact authority inventory before any recipient-specific projection. */
        Result<HeadlessNetworkDetail::InitialTransformCapture *> PrepareInitialInventory(
            WorldRecord &record, const Net::ReplicationWorldCaptureRead &read, std::span<const Net::NetworkObjectMappingEntry> entries) {
            using Inventory = Result<HeadlessNetworkDetail::InitialTransformCapture *>;
            auto &inventory = record.initialTransformCapture;
            if (inventory && inventory->activation == read.Descriptor() && inventory->mappingRevision == read.MappingRevision())
                return Inventory::Success(&*inventory);

            Net::ReplicationCaptureLimits limits;
            if (entries.size() > limits.maximumTargets)
                return Inventory::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
            limits.maximumTargetsPerTick = entries.size();
            limits.publicationMode = Net::ReplicationCapturePublicationMode::ExactCommit;
            const auto serializers = PinInitialSerializers(record);
            if (!serializers)
                return Inventory::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
            std::vector<Net::ReplicationCaptureTarget> targets;
            targets.reserve(entries.size());
            for (const auto &entry : entries)
                targets.push_back({entry, record.publicTransforms});
            auto capture = Net::ReplicationStateCapture::Prepare(read, serializers, targets, limits);
            if (capture.HasError())
                return Inventory::Failure(std::move(capture).ErrorValue());
            auto &prepared = inventory.emplace(read.Descriptor(), read.MappingRevision(), serializers, std::move(capture).Value());
            return Inventory::Success(&prepared);
        }

        /** @brief Validates complete publication of the shared inventory at the given actual commit. */
        Result<void> CaptureInitialInventory(HeadlessNetworkDetail::InitialTransformCapture &inventory,
                                             const Net::ReplicationWorldCaptureRead &read, std::size_t count,
                                             const CancellationToken &cancellation) {
            const auto report = inventory.capture->CaptureAtCommit(read, read.SimulationTick(), cancellation);
            if (report.HasError())
                return Result<void>::Failure(report.ErrorValue());
            if (const auto &error = report.Value().firstError; error)
                return Result<void>::Failure(*error);
            if (report.Value().published != count)
                return Result<void>::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
            return Result<void>::Success();
        }

        /** @brief Shares exactly one attempt for every recipient at the current committed tick. */
        Result<void> SampleInitialInventory(HeadlessNetworkDetail::InitialTransformCapture &inventory,
                                            const Net::ReplicationWorldCaptureRead &read, std::size_t count,
                                            const CancellationToken &cancellation) {
            const auto tick = read.SimulationTick();
            if (inventory.attemptedTick != tick) {
                inventory.sample.reset();
                inventory.attemptedTick = tick;
                inventory.sample.emplace(CaptureInitialInventory(inventory, read, count, cancellation));
            }
            return inventory.sample ? *inventory.sample : Result<void>::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
        }

        /** @brief Projects the shared canonical committed inventory for one exact remote recipient. */
        InitialCapture CaptureInitialTransforms(WorldRecord &record, const Net::ReplicationWorldReadLease &world,
                                                const Net::ReplicationWorldLifecycle &lifecycle, const HeadlessInitialRecipient &grant,
                                                std::size_t maximumWireBytes) {
            const auto cancellation = world.Cancellation();
            const auto opened = record.publicTransforms->AcquireCaptureReadAfterCommit(lifecycle, FlushPhase, cancellation);
            if (opened.HasError())
                return InitialCaptureFailure(opened.ErrorValue());
            const auto &read = opened.Value();
            if (read.Descriptor() != world.Descriptor() || read.MappingRevision() != world.MappingRevision())
                return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            const auto entries = world.Mapping().Entries();
            if (entries.empty())
                return InitialCapture::Success(
                    Net::ReplicationInitialStateSnapshot{world.Descriptor().session, world.Descriptor().authority, read.SimulationTick()});
            const auto prepared = PrepareInitialInventory(record, read, entries);
            if (prepared.HasError())
                return InitialCapture::Failure(prepared.ErrorValue());
            auto &inventory = *prepared.Value();
            auto codecs = ProjectReplicaCodecs(world, grant, inventory.serializers, Net::ReplicationRecordKind::Spawn);
            if (codecs.HasError())
                return InitialCapture::Failure(std::move(codecs).ErrorValue());
            if (const auto sampled = SampleInitialInventory(inventory, read, entries.size(), cancellation); sampled.HasError())
                return InitialCaptureFailure(sampled.ErrorValue());
            auto encoded = Net::EncodeReplicationInitialState(world, *inventory.capture, read.SimulationTick(), codecs.Value(),
                                                              maximumWireBytes, cancellation);
            return encoded.HasError() ? InitialCapture::Failure(std::move(encoded).ErrorValue())
                                      : InitialCapture::Success(std::move(encoded).Value());
        }

        /** @brief Binds a Scene transaction to this exact owner-acquired view and current structural dispatch. */
        SceneApplyOwner OpenReplicaSceneWriter(const WorldRecord &record, const HeadlessSceneTransformReplication &selection,
                                               const Runtime::RuntimeDispatchEvidence &dispatch) {
            const auto view = record.scenes->ActiveScene();
            if (!view)
                return SceneApplyOwner::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
            return Net::SceneReplicationApplyOwner::CreateForService(record.scenes, *view, dispatch, selection.schema, selection.version,
                                                                     selection.fields);
        }

        /** @brief Acquires a fresh Scene writer on the exact owner lane for each live Snapshot commit. */
        class LiveReplicaSceneApply final : public Net::IReplicationApplyOwner {
        public:
            LiveReplicaSceneApply(std::weak_ptr<const WorldOwners> owners, std::weak_ptr<const WorldRecord> record) noexcept
                : owners_(std::move(owners)), record_(std::move(record)) {}

            /** @copydoc Net::IReplicationApplyOwner::Prepare */
            Result<std::unique_ptr<Net::IReplicationApplyCandidate>> Prepare(
                std::span<const Net::ReplicationApplyUpdate> updates) override {
                using Candidate = Result<std::unique_ptr<Net::IReplicationApplyCandidate>>;
                const auto owners = owners_.lock();
                const auto record = record_.lock();
                if (!owners || !record || std::this_thread::get_id() != owners->owner || record->closeRequested ||
                    owners->retirementStarted || !record->scenes)
                    return Candidate::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
                if (!owners->fixed || !owners->configured || !owners->configured->publicTransforms)
                    return Candidate::Failure(MakeError(Net::NetworkErrors::ReplicationWorldUnavailable));
                const auto &dispatch = owners->fixed->SceneDispatch();
                Runtime::RuntimeDispatchFacts facts;
                if (dispatch.Read(owners->fixed->Source(), CommitPhase, facts) != Runtime::RuntimeDispatchStatus::Valid)
                    return Candidate::Failure(MakeError(Net::NetworkErrors::ReplicationWorldPhaseInvalid));
                auto owner = OpenReplicaSceneWriter(*record, *owners->configured->publicTransforms, dispatch);
                if (owner.HasError())
                    return Candidate::Failure(owner.ErrorValue());
                return owner.Value()->Prepare(updates);
            }

        private:
            std::weak_ptr<const WorldOwners> owners_;
            std::weak_ptr<const WorldRecord> record_;
        };

        /** @brief Creates one detached typed receiver against every exact rebuilt local mapping occurrence. */
        InitialReceiver PrepareReplicaReceiver(const WorldRecord &record, Net::ReplicationWorldLifecycle &lifecycle,
                                               const Net::ReplicationWorldReadLease &world,
                                               const Net::ReplicationInboundAuthority &authority, const HeadlessInitialRecipient &grant,
                                               std::shared_ptr<Net::IReplicationApplyOwner> owner, Net::ReplicationRecordKind recordKind,
                                               std::uint64_t now) {
            Net::ReplicationInboundLimits limits;
            if (grant.ownership.size() > limits.maximumObjects)
                return InitialReceiver::Failure(MakeError(Net::ReplicationStateErrors::Capacity));
            limits.maximumPending = std::max(limits.maximumPending, grant.ownership.size());
            const auto mode = recordKind == Net::ReplicationRecordKind::Spawn ? Net::ReplicationInboundApplyMode::InitialSnapshot
                                                                              : Net::ReplicationInboundApplyMode::Incremental;
            auto receiver = Net::ReplicationInboundApply::Create(authority, lifecycle, owner, now, limits, mode);
            if (receiver.HasError())
                return InitialReceiver::Failure(std::move(receiver).ErrorValue());
            const auto entries = world.Mapping().Entries();
            if (!entries.empty()) {
                auto serializers = PinInitialSerializers(record);
                if (!serializers)
                    return InitialReceiver::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
                auto codecs = ProjectReplicaCodecs(world, grant, serializers, recordKind);
                if (codecs.HasError())
                    return InitialReceiver::Failure(std::move(codecs).ErrorValue());
                for (std::size_t index{}; index < entries.size(); ++index) {
                    const auto &codec = codecs.Value()[index];
                    auto role = Net::ReplicationRoleState::Create(codec->Recipient());
                    if (role.HasError())
                        return InitialReceiver::Failure(std::move(role).ErrorValue());
                    auto registered =
                        receiver.Value()->RegisterObject(entries[index],
                                                         std::make_shared<Net::ReplicationRoleState>(std::move(role).Value()), codec);
                    if (registered.HasError())
                        return InitialReceiver::Failure(std::move(registered).ErrorValue());
                }
            }
            return InitialReceiver::Success({std::move(owner), std::move(receiver).Value()});
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
                                                       std::shared_ptr<Network::INetworkInitialStateHost> client,
                                                       std::shared_ptr<const IHeadlessInitialReplicaAuthority> replica,
                                                       std::shared_ptr<Network::INetworkStateUpdateHost> updates) noexcept
        : authority_(std::move(authority)), client_(std::move(client)), replica_(std::move(replica)), updates_(std::move(updates)) {}

    /** @copydoc HeadlessInitialStateHost::BindWorlds */
    Result<void> HeadlessInitialStateHost::BindWorlds(const std::shared_ptr<HeadlessNetworkDetail::WorldOwners> &worlds) {
        if (!worlds || (!authority_ && !replica_) || !client_ || bound_)
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
        if (!authority_)
            return client_->Capture(world, lifecycle, peer, maximumWireBytes);
        const auto owners = worlds_.lock();
        if (!owners || !world.HasCurrentMapping() || !world.Capabilities().publishAuthority)
            return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
        const auto record = FindInitialWorld(*owners, world.Descriptor().scene, Net::NetworkModeWorldKind::AuthorityServer);
        if (!record || !record->publicTransforms || !record->activeGameplay)
            return InitialCapture::Failure(MakeError(Net::NetworkErrors::ReplicationDescriptorIncompatible));
        try {
            auto grant = ResolveAuthorityGrant(*authority_, *record, world, peer);
            if (grant.HasError())
                return InitialCapture::Failure(std::move(grant).ErrorValue());
            return CaptureInitialTransforms(*record, world, lifecycle, grant.Value(), maximumWireBytes);
        } catch (const std::bad_alloc &) {
            return InitialCapture::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
        }
    }

    /** @copydoc HeadlessInitialStateHost::PrepareReceiver */
    Result<Network::NetworkInitialStateTarget> HeadlessInitialStateHost::PrepareReceiver(
        const Network::ReplicationInboundAuthority &authority, Network::ReplicationWorldLifecycle &world,
        const std::span<const Network::ReplicationInitialOwnership> ownership, const std::uint64_t now) {
        if (!replica_)
            return client_->PrepareReceiver(authority, world, ownership, now);
        return PrepareSceneReceiver(authority, world, ownership, now, Net::ReplicationRecordKind::Spawn);
    }

    /** @copydoc HeadlessInitialStateHost::CaptureUpdates */
    UpdateCapture HeadlessInitialStateHost::CaptureUpdates(const Net::ReplicationWorldReadLease &world,
                                                           const Net::ReplicationWorldLifecycle &lifecycle,
                                                           const Net::PeerSessionLifecycle &peer,
                                                           std::span<const Net::ReplicationInitialOwnership> ownership, std::uint64_t now,
                                                           const Net::NetworkStateUpdatePolicy &policy) {
        if (!updates_)
            return UpdateCapture::Failure(MakeError(Net::NetworkErrors::ReplicationWorldUnavailable));
        return updates_->CaptureUpdates(world, lifecycle, peer, ownership, now, policy);
    }

    /** @copydoc HeadlessInitialStateHost::PrepareUpdates */
    InitialReceiver HeadlessInitialStateHost::PrepareUpdates(const Net::ReplicationInboundAuthority &authority,
                                                             Net::ReplicationWorldLifecycle &world,
                                                             std::span<const Net::ReplicationInitialOwnership> ownership,
                                                             std::uint64_t now) {
        if (replica_)
            return PrepareSceneReceiver(authority, world, ownership, now, Net::ReplicationRecordKind::Update);
        if (!updates_)
            return InitialReceiver::Failure(MakeError(Net::NetworkErrors::ReplicationWorldUnavailable));
        return updates_->PrepareUpdates(authority, world, ownership, now);
    }

    /** @copydoc HeadlessInitialStateHost::PrepareSceneReceiver */
    InitialReceiver HeadlessInitialStateHost::PrepareSceneReceiver(const Net::ReplicationInboundAuthority &authority,
                                                                   Net::ReplicationWorldLifecycle &world,
                                                                   std::span<const Net::ReplicationInitialOwnership> ownership,
                                                                   std::uint64_t now, Net::ReplicationRecordKind recordKind) {
        const auto owners = worlds_.lock();
        if (!owners || !owners->configured || !owners->configured->publicTransforms || !owners->fixed)
            return InitialReceiver::Failure(MakeError(Net::NetworkErrors::ReplicationWorldStale));
        const auto opened = OpenInitialReplica(*owners, authority, world, ownership.size());
        if (opened.HasError())
            return InitialReceiver::Failure(opened.ErrorValue());
        const auto &read = opened.Value();
        try {
            auto grant = ResolveReplicaGrant(*replica_, read, authority, ownership, now);
            if (grant.HasError())
                return InitialReceiver::Failure(std::move(grant).ErrorValue());
            std::shared_ptr<Net::IReplicationApplyOwner> owner;
            if (recordKind == Net::ReplicationRecordKind::Spawn) {
                auto sceneWriter = OpenReplicaSceneWriter(*read.record, *owners->configured->publicTransforms, read.dispatch);
                if (sceneWriter.HasError())
                    return InitialReceiver::Failure(sceneWriter.ErrorValue());
                owner = std::move(sceneWriter).Value();
            } else {
                owner = std::make_shared<LiveReplicaSceneApply>(owners, read.record);
            }
            return PrepareReplicaReceiver(*read.record, world, read.mapping, authority, grant.Value(), std::move(owner), recordKind, now);
        } catch (const std::bad_alloc &) {
            return InitialReceiver::Failure(MakeError(Net::ReplicationCaptureErrors::Capacity));
        }
    }

    /** @copydoc HeadlessNetworkDetail::WorldService::BindReplicaSceneTransforms */
    Result<void> WorldService::BindReplicaSceneTransforms() const {
        const auto &host = owners_->configured->initialSceneState;
        if (request_.world != Net::NetworkModeWorldKind::Client || !host || !host->AppliesReplicaTransforms())
            return Result<void>::Success();
        if (!owners_->fixed)
            return Result<void>::Failure(MakeError(Net::NetworkErrors::ReplicationWorldPhaseInvalid));
        return record_->scenes->BindReplicaTransformDispatch(owners_->fixed->Source());
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
