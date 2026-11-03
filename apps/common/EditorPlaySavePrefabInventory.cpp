#include "EditorPlaySavePrefabInventory.h"

#include "Horo/Prefab/PrefabCookedOverrides.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"

#include <algorithm>
#include <new>
#include <set>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;
        using namespace Prefab;

        /** @brief Keeps unsupported/stale durable creation typed and actionable at the owning prefab boundary. */
        template <typename T> Result<T> Invalid(std::string message) {
            return Result<T>::Failure(MakeError(PrefabErrors::AdmissionRejected, std::move(message)));
        }
    }  // namespace

    /** @brief Actual owner-lane metadata and cooked pins; detached captures never retain this mutable state. */
    struct PlaySavePrefabInventory::State final {
        /** @brief Reserved complete identity recipe; pending entries are never captured as existing entities. */
        struct Entry final {
            PrefabTemplateLease source;
            PrefabPersistentInstance recipe;
            std::vector<EntityRef> runtime;
            bool published{};
            bool cancelled{};
            bool invalid{};
        };

        /** @brief Actual structural-receipt publication uses only storage reserved before queue admission. */
        struct Publication final : IPrefabPersistencePublication {
            explicit Publication(std::shared_ptr<Entry> entry) noexcept : entry(std::move(entry)) {}

            SavePrefabInstanceId InstanceIdentity() const noexcept override {
                return entry->recipe.identity;
            }

            void PublishSpawn(std::span<const EntityRef> entities) noexcept override {
                if (entry->published || entry->cancelled)
                    return;
                if (entities.size() != entry->runtime.size()) {
                    entry->invalid = true;
                    return;
                }
                std::ranges::copy(entities, entry->runtime.begin());
                entry->published = true;
            }

            void PublishDespawn() noexcept override {
                if (!entry->published || entry->cancelled)
                    return;
                entry->recipe.disposition = PersistentEntityDisposition::Tombstone;
                for (auto &member : entry->recipe.members)
                    member.record.disposition = PersistentEntityDisposition::Tombstone;
            }

            void Cancel() noexcept override {
                if (!entry->published)
                    entry->cancelled = true;
            }

            std::shared_ptr<Entry> entry;
        };

        RuntimeSceneService &scenes;
        SaveWorldId world;
        SceneRuntimeId scene;
        std::vector<PersistentEntityRecord> authored;
        std::shared_ptr<Security::SecureRandomSource> entropy;
        PrefabPersistenceLimits limits;
        PrefabLimitProfile profile;
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas;
        std::thread::id owner{std::this_thread::get_id()};
        std::vector<std::shared_ptr<Entry>> entries;
        std::vector<std::shared_ptr<Publication>> publicationLeases;
        bool closed{};

        /** @brief Checks the exact live owner before borrowing any Scene or requesting entropy. */
        Result<RuntimeSceneView> Current() const {
            if (owner != std::this_thread::get_id() || closed)
                return Invalid<RuntimeSceneView>("The Play prefab identity owner is closed or used from a foreign lane.");
            const auto active = scenes.ActiveScene();
            if (!active || active->RuntimeId() != scene)
                return Invalid<RuntimeSceneView>("The Play prefab inventory belongs to a different Scene incarnation.");
            return Result<RuntimeSceneView>::Success(*active);
        }

        /** @brief Binds current authored identities without issuing replacements for missing objects. */
        Result<void> Authored(RuntimeSceneView view, std::vector<PersistentEntityRecord> &records,
                              std::vector<PersistentEntityRuntimeBinding> &bindings) const {
            for (const auto &original : authored) {
                const auto *source = std::get_if<AuthoredPersistentEntityProvenance>(&original.provenance);
                if (!source || source->scene != view.DefinitionId())
                    return Invalid<void>("The authored identity inventory no longer matches this Play Scene.");
                auto record = original;
                const auto entity = view.Find(source->object);
                record.disposition = entity ? PersistentEntityDisposition::Live : PersistentEntityDisposition::Tombstone;
                records.push_back(record);
                if (entity)
                    bindings.emplace_back(record.identity, record.generation, *entity);
            }
            return Result<void>::Success();
        }

        /** @brief Reads exact committed dynamic entity incarnations; generation mismatch is deletion, never slot reuse. */
        Result<void> Dynamic(RuntimeSceneView view, std::vector<PersistentEntityRecord> &records,
                             std::vector<PersistentEntityRuntimeBinding> &bindings) const {
            for (const auto &entry : entries) {
                if (entry->invalid)
                    return Invalid<void>("An actual prefab receipt did not cover its reserved durable member inventory.");
                if (!entry->published || entry->cancelled)
                    continue;
                for (std::size_t index = 0; index < entry->recipe.members.size(); ++index) {
                    auto record = entry->recipe.members[index].record;
                    const bool live = record.disposition == PersistentEntityDisposition::Live && view.Get(entry->runtime[index]).HasValue();
                    record.disposition = live ? PersistentEntityDisposition::Live : PersistentEntityDisposition::Tombstone;
                    records.push_back(record);
                    if (live)
                        bindings.emplace_back(record.identity, record.generation, entry->runtime[index]);
                }
            }
            return Result<void>::Success();
        }

        /** @brief Reserves only actual admitted metadata payload, rather than the entire future capacity on every spawn. */
        std::size_t RecordCount() const noexcept {
            std::size_t count = authored.size();
            for (const auto &entry : entries)
                if (entry->published && !entry->cancelled)
                    count += entry->recipe.members.size();
            return count;
        }

        /** @brief Builds the same complete identity authority used by capture and actual Gameplay binding. */
        Result<PersistentEntityIdentityMap> Map(RuntimeSceneView view) const {
            std::vector<PersistentEntityRecord> records;
            std::vector<PersistentEntityRuntimeBinding> bindings;
            const auto count = RecordCount();
            records.reserve(count);
            bindings.reserve(count);
            if (auto captured = Authored(view, records, bindings); captured.HasError())
                return Result<PersistentEntityIdentityMap>::Failure(captured.ErrorValue());
            if (auto captured = Dynamic(view, records, bindings); captured.HasError())
                return Result<PersistentEntityIdentityMap>::Failure(captured.ErrorValue());
            return PersistentEntityIdentityMap::Create(scene, records, bindings, limits.maximumMembers);
        }

        /** @brief Resolves a real current parent/binding through the canonical complete identity map. */
        Result<PrefabPersistentEntityReference> Reference(const PersistentEntityIdentityMap &map, EntityRef runtime) const {
            const auto identity = map.Find(runtime);
            if (identity.HasError())
                return Result<PrefabPersistentEntityReference>::Failure(identity.ErrorValue());
            const auto records = map.Records();
            const auto record = std::ranges::lower_bound(records, identity.Value(), {}, &PersistentEntityRecord::identity);
            if (record == records.end() || record->identity != identity.Value())
                return Invalid<PrefabPersistentEntityReference>("A prefab relationship has no admitted durable identity.");
            return Result<PrefabPersistentEntityReference>::Success({{world, record->identity}, record->generation});
        }

        /** @brief Reserves all retained record/lease capacity before admitting the next actual structural queue. */
        Result<void> Admit(std::size_t members) {
            std::erase_if(entries, [](const auto &entry) {
                return !entry->published && entry->cancelled;
            });
            std::erase_if(publicationLeases, [](const auto &publication) {
                return publication->entry->cancelled;
            });
            if (entries.size() >= limits.maximumInstances)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            std::size_t occupied = authored.size();
            for (const auto &entry : entries) {
                if (entry->recipe.members.size() > limits.maximumMembers - occupied)
                    return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                occupied += entry->recipe.members.size();
            }
            if (members > limits.maximumMembers - occupied)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            entries.reserve(limits.maximumInstances);
            publicationLeases.reserve(limits.maximumInstances);
            return Result<void>::Success();
        }

        /** @brief Copies only declared external interfaces through actual durable relationships. */
        Result<void> Relationships(PrefabPersistentInstance &recipe, const PrefabSpawnRequest &request,
                                   const PersistentEntityIdentityMap &map) const {
            if (request.parent) {
                auto parent = Reference(map, *request.parent);
                if (parent.HasError())
                    return Result<void>::Failure(parent.ErrorValue());
                recipe.parent = std::move(parent).Value();
            }
            recipe.bindings.reserve(request.bindings.size());
            for (const auto &binding : request.bindings) {
                auto target = Reference(map, binding.target);
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                recipe.bindings.push_back({binding.id, std::move(target).Value()});
            }
            return Result<void>::Success();
        }

        /** @brief Captures only actual projected attachment identity against exact source type/schema occurrence. */
        Result<void> Behaviors(PrefabPersistentMember &member, const CookedPrefabEntity &source,
                               const RuntimeComponentSet &projected) const {
            std::size_t index{};
            member.behaviors.reserve(projected.behaviors.size());
            for (const auto &value : source.members) {
                const auto *behavior = std::get_if<Gameplay::BehaviorComponent>(&value);
                if (!behavior)
                    continue;
                if (index >= projected.behaviors.size())
                    return Invalid<void>("Actual prefab projection omitted a durable behavior occurrence.");
                const auto &actual = projected.behaviors[index++];
                if (actual.typeId != behavior->typeId || actual.schemaVersion != behavior->schemaVersion || !actual.instanceId.value)
                    return Invalid<void>("Actual prefab projection changed durable behavior type or schema identity.");
                member.behaviors.push_back({behavior->typeId, behavior->instanceId, actual.instanceId, behavior->schemaVersion});
            }
            if (index != projected.behaviors.size())
                return Invalid<void>("Actual prefab projection contains an undeclared durable behavior occurrence.");
            return Result<void>::Success();
        }

        /** @brief Issues opaque dynamic identities from the SAME host entropy and rejects any actual inventory collision. */
        Result<void> Members(Entry &entry, const PersistentEntityIdentityMap &map, std::span<const RuntimeComponentSet> projected,
                             const CookedPrefabData &effective) const {
            auto asset = SaveAssetId::FromBytes(entry.recipe.source.asset.Bytes());
            if (asset.HasError())
                return Result<void>::Failure(asset.ErrorValue());
            std::set<PersistentEntityId> issued;
            for (const auto &record : map.Records())
                issued.insert(record.identity);
            for (const auto &reserved : entries)
                for (const auto &member : reserved->recipe.members)
                    issued.insert(member.record.identity);
            const auto &entities = effective.entities;
            if (projected.size() != entities.size())
                return Invalid<void>("Actual prefab projection has an incomplete durable member inventory.");
            entry.recipe.members.reserve(entities.size());
            entry.runtime.resize(entities.size());
            for (const auto &source : entities) {
                auto identity = IssuePlaySaveIdentity<PersistentEntityId>(*entropy);
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                if (!issued.insert(identity.Value()).second)
                    return Result<void>::Failure(MakeError(PrefabErrors::IdentityCollision));
                entry.recipe.members.push_back({{identity.Value(),
                                                 {1},
                                                 SpawnedPersistentEntityProvenance{asset.Value(), entry.recipe.identity},
                                                 PersistentEntityDisposition::Live},
                                                source.provenance,
                                                {}});
                const auto index = entry.recipe.members.size() - 1;
                if (auto bound = Behaviors(entry.recipe.members.back(), source, projected[index]); bound.HasError())
                    return bound;
            }
            return Result<void>::Success();
        }

        /** @brief Transfers actual final root placement once and prepares child TRS intent through the same canonical core. */
        Result<PrefabValueOverrideSet> Transforms(const Entry &entry, RuntimeSceneView view, PrefabPersistentInstance &recipe) const {
            auto objects = PrepareCookedPrefabOverrideBaseline(*entry.source.Template(), profile);
            if (objects.HasError())
                return Result<PrefabValueOverrideSet>::Failure(objects.ErrorValue());
            const PrefabOverrideBaseline baseline{recipe.source.asset, recipe.rootSourceRevision, objects.Value()};
            const auto root = view.Get(entry.runtime.front());
            if (root.HasError() || !root.Value().localTransform)
                return Invalid<PrefabValueOverrideSet>("The saved prefab root has no current canonical local transform.");
            recipe.placement = *root.Value().localTransform;
            const PrefabValueEdit revertRoot{PrefabValueEditKind::Revert, objects.Value().front().key.object, {}};
            auto clean = PreparePrefabValueEdit(baseline, recipe.overrides, revertRoot, schemas.get(), profile);
            if (clean.HasError())
                return Result<PrefabValueOverrideSet>::Failure(clean.ErrorValue());
            auto intent = std::move(clean).Value().rebased;
            for (std::size_t index = 1; index < entry.runtime.size(); ++index) {
                if (recipe.members[index].record.disposition == PersistentEntityDisposition::Tombstone)
                    continue;
                const auto entity = view.Get(entry.runtime[index]);
                if (entity.HasError() || !entity.Value().localTransform)
                    return Invalid<PrefabValueOverrideSet>("A live saved prefab member has no current canonical transform.");
                const PrefabValueEdit edit{PrefabValueEditKind::Assign, objects.Value()[index].key.object, *entity.Value().localTransform};
                auto applied = PreparePrefabValueEdit(baseline, intent, edit, schemas.get(), profile);
                if (applied.HasError())
                    return Result<PrefabValueOverrideSet>::Failure(applied.ErrorValue());
                intent = std::move(applied).Value().rebased;
            }
            return Result<PrefabValueOverrideSet>::Success(std::move(intent));
        }

        /** @brief Captures exact current member incarnations and retains explicit deletion without flattening component memory. */
        Result<PrefabPersistentInstance> CaptureEntry(const Entry &entry, RuntimeSceneView view) const {
            auto recipe = entry.recipe;
            for (std::size_t index = 0; index < entry.runtime.size(); ++index) {
                auto &record = recipe.members[index].record;
                if (view.Get(entry.runtime[index]).HasError())
                    record.disposition = PersistentEntityDisposition::Tombstone;
            }
            if (recipe.members.front().record.disposition == PersistentEntityDisposition::Tombstone) {
                recipe.disposition = PersistentEntityDisposition::Tombstone;
                return Result<PrefabPersistentInstance>::Success(std::move(recipe));
            }
            auto transforms = Transforms(entry, view, recipe);
            if (transforms.HasError())
                return Result<PrefabPersistentInstance>::Failure(transforms.ErrorValue());
            recipe.overrides = std::move(transforms).Value();
            return Result<PrefabPersistentInstance>::Success(std::move(recipe));
        }

        /** @brief Stable creator authorization is checked before any member is exposed. */
        static bool AvailableTo(const Entry &entry, std::string_view creator) noexcept {
            return entry.published && !entry.cancelled && entry.recipe.creator == creator &&
                   entry.recipe.disposition == PersistentEntityDisposition::Live;
        }

        /** @brief Authorizes the stable creator before exposing any current member or publication lease. */
        Result<PrefabPersistentLookup> Lookup(SavePrefabInstanceId instance, std::string_view creator) const {
            using Outcome = Result<PrefabPersistentLookup>;
            auto view = Current();
            if (view.HasError())
                return Outcome::Failure(view.ErrorValue());
            const auto found = std::ranges::find_if(publicationLeases, [&](const auto &publication) {
                return publication->entry->recipe.identity == instance;
            });
            if (found == publicationLeases.end())
                return Invalid<PrefabPersistentLookup>("Unknown durable prefab occurrence.");
            const auto &entry = *(*found)->entry;
            if (!AvailableTo(entry, creator))
                return Invalid<PrefabPersistentLookup>("Durable prefab occurrence is unavailable to this creator grant.");
            PrefabPersistentLookup result;
            result.entities.reserve(entry.runtime.size());
            for (std::size_t index = 0; index < entry.runtime.size(); ++index) {
                if (entry.recipe.members[index].record.disposition == PersistentEntityDisposition::Tombstone)
                    continue;
                if (view.Value().Get(entry.runtime[index]).HasError())
                    return Invalid<PrefabPersistentLookup>("Durable prefab member incarnation is no longer live.");
                result.entities.push_back(entry.runtime[index]);
            }
            if (result.entities.empty())
                return Invalid<PrefabPersistentLookup>("Durable prefab occurrence has no live root.");
            result.publication = *found;
            return Outcome::Success(std::move(result));
        }

        /** @brief Adopts exact private reconstruction receipts using preallocated bounded inventory only. */
        Result<void> Adopt(const PrefabPersistentState &saved, std::span<const PrefabRestoredInstance> restored,
                           RuntimeSceneView candidate) {
            if (restored.size() != saved.instances.size())
                return Invalid<void>("Incomplete private prefab occurrence receipts.");
            entries.reserve(limits.maximumInstances);
            publicationLeases.reserve(limits.maximumInstances);
            for (std::size_t index = 0; index < saved.instances.size(); ++index) {
                const auto &recipe = saved.instances[index];
                const auto &receipt = restored[index];
                if (receipt.identity != recipe.identity || receipt.members.size() != recipe.members.size())
                    return Invalid<void>("Private prefab receipt and durable recipe disagree.");
                auto entry = std::make_shared<Entry>();
                entry->source = receipt.source;
                entry->recipe = recipe;
                entry->runtime = receipt.members;
                entry->published = true;  // Private inventory remains inaccessible until its Scene actually publishes.
                entries.push_back(entry);
                publicationLeases.push_back(std::make_shared<Publication>(std::move(entry)));
            }
            const auto map = Map(candidate);
            return map.HasError() ? Result<void>::Failure(map.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Captures only actually committed recipes; a pending request is not a persistent entity. */
        Result<PlaySavePrefabCapture> Capture() const {
            auto view = Current();
            if (view.HasError())
                return Result<PlaySavePrefabCapture>::Failure(view.ErrorValue());
            auto map = Map(view.Value());
            if (map.HasError())
                return Result<PlaySavePrefabCapture>::Failure(map.ErrorValue());
            PrefabPersistentState captured{world, {}};
            captured.instances.reserve(entries.size());
            for (const auto &entry : entries) {
                if (!entry->published || entry->cancelled)
                    continue;
                auto recipe = CaptureEntry(*entry, view.Value());
                if (recipe.HasError())
                    return Result<PlaySavePrefabCapture>::Failure(recipe.ErrorValue());
                captured.instances.push_back(std::move(recipe).Value());
            }
            if (auto valid = ValidatePrefabPersistentState(captured, limits, profile); valid.HasError())
                return Result<PlaySavePrefabCapture>::Failure(valid.ErrorValue());
            return Result<PlaySavePrefabCapture>::Success({std::move(map).Value(), std::move(captured)});
        }

        /** @brief Charges the real aggregate canonical recipe payload, including unpublished reservations, before Scene admission.
         * This private serialization measures/admit metadata only; it is never published as committed world state.
         */
        Result<void> Budget(const Entry &candidate) const {
            PrefabPersistentState bounded{world, {}};
            bounded.instances.reserve(entries.size() + 1);
            for (const auto &entry : entries)
                if (!entry->cancelled)
                    bounded.instances.push_back(entry->recipe);
            bounded.instances.push_back(candidate.recipe);
            const auto encoded = EncodePrefabPersistentState(bounded, limits, profile);
            return encoded.HasError() ? Result<void>::Failure(encoded.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Requires exact actual content authority before any new durable identity is issued. */
        Result<void> ValidateCreationSource(const PrefabTemplateLease &source, const PrefabSpawnRequest &request) const {
            if (!source.Template() || source.Scene() != scene || source.Template()->Data().assetId != request.source.asset ||
                source.Artifact().Digest() != request.source.artifactDigest || !source.Target() ||
                *source.Target() != request.source.target || !source.Template()->Data().rootSourceRevision)
                return Invalid<void>("Recook the actual prefab with complete root provenance before durable spawning.");
            return Result<void>::Success();
        }

        /** @brief Allocates and charges the complete recipe/publication payload before structural admission. */
        Result<std::shared_ptr<IPrefabPersistencePublication>> Reserve(const PreparedPrefabSpawn &prepared,
                                                                       const PrefabSpawnRequest &request,
                                                                       std::span<const RuntimeComponentSet> projected,
                                                                       std::span<const Assets::AssetId> lineage, std::string_view creator,
                                                                       const PersistentEntityIdentityMap &map) {
            using Outcome = Result<std::shared_ptr<IPrefabPersistencePublication>>;
            const auto &source = prepared.Source();
            auto identity = IssuePlaySaveIdentity<SavePrefabInstanceId>(*entropy);
            if (identity.HasError())
                return Outcome::Failure(identity.ErrorValue());
            for (const auto &entry : entries)
                if (entry->recipe.identity == identity.Value())
                    return Outcome::Failure(MakeError(PrefabErrors::IdentityCollision));
            auto entry = std::make_shared<Entry>();
            entry->source = source;
            entry->recipe.identity = identity.Value();
            entry->recipe.creator = creator;
            entry->recipe.source = request.source;
            entry->recipe.rootSourceRevision = *source.Template()->Data().rootSourceRevision;
            entry->recipe.placement = request.placement;
            entry->recipe.creationLineage.assign(lineage.begin(), lineage.end());
            entry->recipe.initialization = request.initialization;
            entry->recipe.overrides = request.overrides.value_or(PrefabValueOverrideSet{request.source.asset,
                                                                                        entry->recipe.rootSourceRevision,
                                                                                        schemas ? schemas->Revision() : Sha256Digest{},
                                                                                        {},
                                                                                        {},
                                                                                        {}});
            if (auto linked = Relationships(entry->recipe, request, map); linked.HasError())
                return Outcome::Failure(linked.ErrorValue());
            if (auto issued = Members(*entry, map, projected, prepared.Data()); issued.HasError())
                return Outcome::Failure(issued.ErrorValue());
            if (auto budget = Budget(*entry); budget.HasError())
                return Outcome::Failure(budget.ErrorValue());
            auto publication = std::make_shared<Publication>(entry);
            entries.push_back(std::move(entry));
            publicationLeases.push_back(publication);
            return Outcome::Success(std::move(publication));
        }

        /** @brief Stages one complete exact cooked recipe before allowing structural publication. */
        Result<std::shared_ptr<IPrefabPersistencePublication>> Prepare(const PreparedPrefabSpawn &prepared,
                                                                       const PrefabSpawnRequest &request,
                                                                       std::span<const RuntimeComponentSet> projected,
                                                                       std::span<const Assets::AssetId> lineage, std::string_view creator) {
            using Outcome = Result<std::shared_ptr<IPrefabPersistencePublication>>;
            const auto &source = prepared.Source();
            auto current = Current();
            if (current.HasError())
                return Outcome::Failure(current.ErrorValue());
            if (auto valid = ValidateCreationSource(source, request); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            if (auto admitted = Admit(source.Template()->Data().entities.size()); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            auto map = Map(current.Value());
            if (map.HasError())
                return Outcome::Failure(map.ErrorValue());
            return Reserve(prepared, request, projected, lineage, creator, map.Value());
        }
    };

    /** @copydoc PlaySavePrefabInventory::PlaySavePrefabInventory */
    PlaySavePrefabInventory::PlaySavePrefabInventory(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    namespace {
        /** @brief Trusted bounds are checked before any inventory allocation or subtraction. */
        bool ValidInventoryBounds(const PrefabPersistenceLimits &limits, std::size_t authored) noexcept {
            return IsValidPrefabPersistenceLimits(limits) && limits.maximumMembers <= 16'384 && authored <= limits.maximumMembers &&
                   limits.maximumInstances <= limits.maximumMembers;
        }
    }  // namespace

    /** @copydoc PlaySavePrefabInventory::Create */
    Result<std::shared_ptr<PlaySavePrefabInventory>> PlaySavePrefabInventory::Create(
        RuntimeSceneService &scenes, SaveWorldId world, std::vector<PersistentEntityRecord> authored,
        std::shared_ptr<Security::SecureRandomSource> entropy, PrefabPersistenceLimits limits, PrefabLimitProfile profile,
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas) {
        using Outcome = Result<std::shared_ptr<PlaySavePrefabInventory>>;
        const auto scene = scenes.ActiveScene();
        if (!scene || !world.IsValid() || !entropy || !ValidInventoryBounds(limits, authored.size()))
            return Invalid<std::shared_ptr<PlaySavePrefabInventory>>(
                "The actual Play world or aggregate prefab identity bounds are invalid.");
        try {
            auto state = std::make_shared<State>(State{scenes, world, scene->RuntimeId(), std::move(authored), std::move(entropy), limits,
                                                       std::move(profile), std::move(schemas)});
            if (auto map = state->Map(*scene); map.HasError())
                return Outcome::Failure(map.ErrorValue());
            return Outcome::Success(std::shared_ptr<PlaySavePrefabInventory>(new PlaySavePrefabInventory(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::PrepareSpawn */
    Result<std::shared_ptr<IPrefabPersistencePublication>> PlaySavePrefabInventory::PrepareSpawn(
        const PreparedPrefabSpawn &prepared, const PrefabSpawnRequest &request, std::span<const RuntimeComponentSet> projected,
        std::span<const Assets::AssetId> lineage, std::string_view creator) {
        try {
            return state_->Prepare(prepared, request, projected, lineage, creator);
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<IPrefabPersistencePublication>>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Lookup */
    Result<PrefabPersistentLookup> PlaySavePrefabInventory::Lookup(SavePrefabInstanceId instance, std::string_view creator) const {
        try {
            return state_->Lookup(instance, creator);
        } catch (const std::bad_alloc &) {
            return Result<PrefabPersistentLookup>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::PrepareForScene */
    Result<std::shared_ptr<PlaySavePrefabInventory>> PlaySavePrefabInventory::PrepareForScene(
        RuntimeSceneService &scenes, RuntimeScene &candidate, const PrefabPersistentState &saved,
        std::span<const PrefabRestoredInstance> restored, std::vector<PersistentEntityRecord> authored,
        std::shared_ptr<Security::SecureRandomSource> entropy, PrefabPersistenceLimits limits, PrefabLimitProfile profile,
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas) {
        using Outcome = Result<std::shared_ptr<PlaySavePrefabInventory>>;
        try {
            if (!entropy || authored.size() > limits.maximumMembers || !candidate.View().IsCurrent() ||
                !scenes.IsRestoreCandidate(candidate))
                return Invalid<std::shared_ptr<PlaySavePrefabInventory>>("Invalid private Scene inventory owner.");
            if (auto valid = EncodePrefabPersistentState(saved, limits, profile); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            auto state = std::make_shared<State>(State{scenes, saved.world, candidate.View().RuntimeId(), std::move(authored),
                                                       std::move(entropy), limits, std::move(profile), std::move(schemas)});
            if (auto adopted = state->Adopt(saved, restored, candidate.View()); adopted.HasError())
                return Outcome::Failure(adopted.ErrorValue());
            return Outcome::Success(std::shared_ptr<PlaySavePrefabInventory>(new PlaySavePrefabInventory(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Capture */
    Result<PlaySavePrefabCapture> PlaySavePrefabInventory::Capture() const {
        try {
            return state_->Capture();
        } catch (const std::bad_alloc &) {
            return Result<PlaySavePrefabCapture>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Identities */
    Result<PersistentEntityIdentityMap> PlaySavePrefabInventory::Identities() const {
        try {
            auto view = state_->Current();
            if (view.HasError())
                return Result<PersistentEntityIdentityMap>::Failure(view.ErrorValue());
            return state_->Map(view.Value());
        } catch (const std::bad_alloc &) {
            return Result<PersistentEntityIdentityMap>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Close */
    void PlaySavePrefabInventory::Close() noexcept {
        state_->closed = true;
    }
}  // namespace Horo::Application::Internal
