#pragma once
/** @file EditorPlaySavePrefabInventoryState.h
 * @brief Target-private owner-lane prefab inventory storage and receipt publication.
 */
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

    /** @brief Actual owner-lane metadata and cooked pins; detached captures never retain this mutable state. */
    struct PlaySavePrefabInventory::State final {
        /** @brief Preserves the prefab admission error at the private state boundary. */
        template <typename T> static Result<T> Invalid(std::string message) {
            return Result<T>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected, std::move(message)));
        }

        /** @brief Reserved complete identity recipe; pending entries are never captured as existing entities. */
        struct Entry final {
            Prefab::PrefabTemplateLease source;
            Prefab::PrefabPersistentInstance recipe;
            std::vector<Runtime::EntityRef> runtime;
            bool published{};
            bool cancelled{};
            bool invalid{};
        };

        /** @brief Actual structural-receipt publication uses only storage reserved before queue admission. */
        struct Publication final : Prefab::IPrefabPersistencePublication {
            explicit Publication(std::shared_ptr<Entry> entry) noexcept : entry(std::move(entry)) {}

            Runtime::SavePrefabInstanceId InstanceIdentity() const noexcept override {
                return entry->recipe.identity;
            }

            void PublishSpawn(std::span<const Runtime::EntityRef> entities) noexcept override {
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
                entry->recipe.disposition = Runtime::PersistentEntityDisposition::Tombstone;
                for (auto &member : entry->recipe.members)
                    member.record.disposition = Runtime::PersistentEntityDisposition::Tombstone;
            }

            void Cancel() noexcept override {
                if (entry->published)
                    return;
                entry->cancelled = true;
                entry->source = {};
            }

            std::shared_ptr<Entry> entry;
        };

        Runtime::RuntimeSceneService &scenes;
        Runtime::SaveWorldId world;
        Runtime::SceneRuntimeId scene;
        std::vector<Runtime::PersistentEntityRecord> authored;
        std::shared_ptr<Security::SecureRandomSource> entropy;
        Prefab::PrefabPersistenceLimits limits;
        Prefab::PrefabLimitProfile profile;
        std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas;
        std::thread::id owner{std::this_thread::get_id()};
        std::vector<std::shared_ptr<Entry>> entries;
        std::vector<std::shared_ptr<Publication>> publicationLeases;
        bool closed{};

        /** @brief Checks the exact live owner before borrowing any Scene or requesting entropy. */
        Result<Runtime::RuntimeSceneView> Current() const {
            if (owner != std::this_thread::get_id() || closed)
                return Invalid<Runtime::RuntimeSceneView>("The Play prefab identity owner is closed or used from a foreign lane.");
            const auto active = scenes.ActiveScene();
            if (!active || active->RuntimeId() != scene)
                return Invalid<Runtime::RuntimeSceneView>("The Play prefab inventory belongs to a different Scene incarnation.");
            return Result<Runtime::RuntimeSceneView>::Success(*active);
        }

        /** @brief Binds current authored identities without issuing replacements for missing objects. */
        Result<void> Authored(const Runtime::RuntimeSceneView &view, std::vector<Runtime::PersistentEntityRecord> &records,
                              std::vector<Runtime::PersistentEntityRuntimeBinding> &bindings) const {
            for (const auto &original : authored) {
                const auto *source = std::get_if<Runtime::AuthoredPersistentEntityProvenance>(&original.provenance);
                if (!source || source->scene != view.DefinitionId())
                    return Invalid<void>("The authored identity inventory no longer matches this Play Scene.");
                auto record = original;
                const auto entity = view.Find(source->object);
                record.disposition = entity ? Runtime::PersistentEntityDisposition::Live : Runtime::PersistentEntityDisposition::Tombstone;
                records.push_back(record);
                if (entity)
                    bindings.emplace_back(record.identity, record.generation, *entity);
            }
            return Result<void>::Success();
        }

        /** @brief Reads exact committed dynamic entity incarnations; generation mismatch is deletion, never slot reuse. */
        Result<void> Dynamic(const Runtime::RuntimeSceneView &view, std::vector<Runtime::PersistentEntityRecord> &records,
                             std::vector<Runtime::PersistentEntityRuntimeBinding> &bindings) const {
            using enum Horo::Runtime::PersistentEntityDisposition;
            for (const auto &entry : entries) {
                if (entry->invalid)
                    return Invalid<void>("An actual prefab receipt did not cover its reserved durable member inventory.");
                if (!entry->published || entry->cancelled)
                    continue;
                for (std::size_t index = 0; index < entry->recipe.members.size(); ++index) {
                    auto record = entry->recipe.members[index].record;
                    const bool live = record.disposition == Live && view.Get(entry->runtime[index]).HasValue();
                    record.disposition = live ? Live : Tombstone;
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
        Result<Runtime::PersistentEntityIdentityMap> Map(const Runtime::RuntimeSceneView &view) const {
            std::vector<Runtime::PersistentEntityRecord> records;
            std::vector<Runtime::PersistentEntityRuntimeBinding> bindings;
            const auto count = RecordCount();
            records.reserve(count);
            bindings.reserve(count);
            if (auto captured = Authored(view, records, bindings); captured.HasError())
                return Result<Runtime::PersistentEntityIdentityMap>::Failure(captured.ErrorValue());
            if (auto captured = Dynamic(view, records, bindings); captured.HasError())
                return Result<Runtime::PersistentEntityIdentityMap>::Failure(captured.ErrorValue());
            return Runtime::PersistentEntityIdentityMap::Create(scene, records, bindings, limits.maximumMembers);
        }

        /** @brief Resolves a real current parent/binding through the canonical complete identity map. */
        Result<Prefab::PrefabPersistentEntityReference> Reference(const Runtime::PersistentEntityIdentityMap &map,
                                                                  Runtime::EntityRef runtime) const {
            const auto identity = map.Find(runtime);
            if (identity.HasError())
                return Result<Prefab::PrefabPersistentEntityReference>::Failure(identity.ErrorValue());
            const auto records = map.Records();
            const auto record = std::ranges::lower_bound(records, identity.Value(), {}, &Runtime::PersistentEntityRecord::identity);
            if (record == records.end() || record->identity != identity.Value())
                return Invalid<Prefab::PrefabPersistentEntityReference>("A prefab relationship has no admitted durable identity.");
            return Result<Prefab::PrefabPersistentEntityReference>::Success({{world, record->identity}, record->generation});
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
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
            std::size_t occupied = authored.size();
            for (const auto &entry : entries) {
                if (entry->recipe.members.size() > limits.maximumMembers - occupied)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                occupied += entry->recipe.members.size();
            }
            if (members > limits.maximumMembers - occupied)
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
            entries.reserve(limits.maximumInstances);
            publicationLeases.reserve(limits.maximumInstances);
            return Result<void>::Success();
        }

        /** @brief Copies only declared external interfaces through actual durable relationships. */
        Result<void> Relationships(Prefab::PrefabPersistentInstance &recipe, const Prefab::PrefabSpawnRequest &request,
                                   const Runtime::PersistentEntityIdentityMap &map) const {
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
                recipe.bindings.emplace_back(binding.id, std::move(target).Value());
            }
            return Result<void>::Success();
        }

        /** @brief Captures only actual projected attachment identity against exact source type/schema occurrence. */
        Result<void> Behaviors(Prefab::PrefabPersistentMember &member, const Prefab::CookedPrefabEntity &source,
                               const Runtime::RuntimeComponentSet &projected) const {
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
                member.behaviors.emplace_back(behavior->typeId, behavior->instanceId, actual.instanceId, behavior->schemaVersion);
            }
            if (index != projected.behaviors.size())
                return Invalid<void>("Actual prefab projection contains an undeclared durable behavior occurrence.");
            return Result<void>::Success();
        }

        /** @brief Issues opaque dynamic identities from the SAME host entropy and rejects any actual inventory collision. */
        Result<void> Members(Entry &entry, const Runtime::PersistentEntityIdentityMap &map,
                             std::span<const Runtime::RuntimeComponentSet> projected, const Prefab::CookedPrefabData &effective) const {
            auto asset = Runtime::SaveAssetId::FromBytes(entry.recipe.source.asset.Bytes());
            if (asset.HasError())
                return Result<void>::Failure(asset.ErrorValue());
            std::set<Runtime::PersistentEntityId> issued;
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
                auto identity = IssuePlaySaveIdentity<Runtime::PersistentEntityId>(*entropy);
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                if (!issued.insert(identity.Value()).second)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
                entry.recipe.members.push_back({{identity.Value(),
                                                 {1},
                                                 Runtime::SpawnedPersistentEntityProvenance{asset.Value(), entry.recipe.identity},
                                                 Runtime::PersistentEntityDisposition::Live},
                                                source.provenance,
                                                {}});
                const auto index = entry.recipe.members.size() - 1;
                if (auto bound = Behaviors(entry.recipe.members.back(), source, projected[index]); bound.HasError())
                    return bound;
            }
            return Result<void>::Success();
        }

        /** @brief Transfers actual final root placement once and prepares child TRS intent through the same canonical core. */
        Result<Prefab::PrefabValueOverrideSet> Transforms(const Entry &entry, const Runtime::RuntimeSceneView &view,
                                                          Prefab::PrefabPersistentInstance &recipe) const {
            auto objects = Prefab::PrepareCookedPrefabOverrideBaseline(*entry.source.Template(), profile);
            if (objects.HasError())
                return Result<Prefab::PrefabValueOverrideSet>::Failure(objects.ErrorValue());
            const Prefab::PrefabOverrideBaseline baseline{recipe.source.asset, recipe.rootSourceRevision, objects.Value()};
            const auto root = view.Get(entry.runtime.front());
            if (root.HasError() || !root.Value().localTransform)
                return Invalid<Prefab::PrefabValueOverrideSet>("The saved prefab root has no current canonical local transform.");
            recipe.placement = *root.Value().localTransform;
            const Prefab::PrefabValueEdit revertRoot{Prefab::PrefabValueEditKind::Revert, objects.Value().front().key.object, {}};
            auto clean = Prefab::PreparePrefabValueEdit(baseline, recipe.overrides, revertRoot, schemas.get(), profile);
            if (clean.HasError())
                return Result<Prefab::PrefabValueOverrideSet>::Failure(clean.ErrorValue());
            auto intent = std::move(clean).Value().rebased;
            for (std::size_t index = 1; index < entry.runtime.size(); ++index) {
                if (recipe.members[index].record.disposition == Runtime::PersistentEntityDisposition::Tombstone)
                    continue;
                const auto entity = view.Get(entry.runtime[index]);
                if (entity.HasError() || !entity.Value().localTransform)
                    return Invalid<Prefab::PrefabValueOverrideSet>("A live saved prefab member has no current canonical transform.");
                const Prefab::PrefabValueEdit edit{Prefab::PrefabValueEditKind::Assign, objects.Value()[index].key.object,
                                                   *entity.Value().localTransform};
                auto applied = Prefab::PreparePrefabValueEdit(baseline, intent, edit, schemas.get(), profile);
                if (applied.HasError())
                    return Result<Prefab::PrefabValueOverrideSet>::Failure(applied.ErrorValue());
                intent = std::move(applied).Value().rebased;
            }
            return Result<Prefab::PrefabValueOverrideSet>::Success(std::move(intent));
        }

        /** @brief Captures exact current member incarnations and retains explicit deletion without flattening component memory. */
        Result<Prefab::PrefabPersistentInstance> CaptureEntry(const Entry &entry, const Runtime::RuntimeSceneView &view) const {
            using enum Horo::Runtime::PersistentEntityDisposition;
            auto recipe = entry.recipe;
            for (std::size_t index = 0; index < entry.runtime.size(); ++index) {
                if (view.Get(entry.runtime[index]).HasError())
                    recipe.members[index].record.disposition = Tombstone;
            }
            if (recipe.members.front().record.disposition == Tombstone) {
                recipe.disposition = Tombstone;
                return Result<Prefab::PrefabPersistentInstance>::Success(std::move(recipe));
            }
            auto transforms = Transforms(entry, view, recipe);
            if (transforms.HasError())
                return Result<Prefab::PrefabPersistentInstance>::Failure(transforms.ErrorValue());
            recipe.overrides = std::move(transforms).Value();
            return Result<Prefab::PrefabPersistentInstance>::Success(std::move(recipe));
        }

        /** @brief Stable creator authorization is checked before any member is exposed. */
        static bool AvailableTo(const Entry &entry, std::string_view creator) noexcept {
            return entry.published && !entry.cancelled && entry.recipe.creator == creator &&
                   entry.recipe.disposition == Runtime::PersistentEntityDisposition::Live;
        }

        /** @brief Authorizes the stable creator before exposing any current member or publication lease. */
        Result<Prefab::PrefabPersistentLookup> Lookup(Runtime::SavePrefabInstanceId instance, std::string_view creator) const {
            using Outcome = Result<Prefab::PrefabPersistentLookup>;
            auto view = Current();
            if (view.HasError())
                return Outcome::Failure(view.ErrorValue());
            const auto found = std::ranges::find_if(publicationLeases, [&](const auto &publication) {
                return publication->entry->recipe.identity == instance;
            });
            if (found == publicationLeases.end())
                return Invalid<Prefab::PrefabPersistentLookup>("Unknown durable prefab occurrence.");
            const auto &entry = *(*found)->entry;
            if (!AvailableTo(entry, creator))
                return Invalid<Prefab::PrefabPersistentLookup>("Durable prefab occurrence is unavailable to this creator grant.");
            if (entry.runtime.empty() || entry.runtime.size() != entry.recipe.members.size() ||
                entry.recipe.members.front().record.disposition != Runtime::PersistentEntityDisposition::Live)
                return Invalid<Prefab::PrefabPersistentLookup>("Durable prefab occurrence has no live root.");
            Prefab::PrefabPersistentLookup result;
            result.entities.reserve(entry.runtime.size());
            result.provenance.reserve(entry.runtime.size());
            for (std::size_t index = 0; index < entry.runtime.size(); ++index) {
                if (entry.recipe.members[index].record.disposition == Runtime::PersistentEntityDisposition::Tombstone)
                    continue;
                if (view.Value().Get(entry.runtime[index]).HasError())
                    return Invalid<Prefab::PrefabPersistentLookup>("Durable prefab member incarnation is no longer live.");
                result.entities.push_back(entry.runtime[index]);
                result.provenance.push_back(entry.recipe.members[index].source);
            }
            result.publication = *found;
            return Outcome::Success(std::move(result));
        }

        /** @brief Adopts exact private reconstruction receipts using preallocated bounded inventory only. */
        Result<void> Adopt(const Prefab::PrefabPersistentState &saved, std::span<const Prefab::PrefabRestoredInstance> restored,
                           const Runtime::RuntimeSceneView &candidate) {
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
            Prefab::PrefabPersistentState captured{world, {}};
            captured.instances.reserve(entries.size());
            for (const auto &entry : entries) {
                if (!entry->published || entry->cancelled)
                    continue;
                auto recipe = CaptureEntry(*entry, view.Value());
                if (recipe.HasError())
                    return Result<PlaySavePrefabCapture>::Failure(recipe.ErrorValue());
                captured.instances.push_back(std::move(recipe).Value());
            }
            if (auto valid = Prefab::ValidatePrefabPersistentState(captured, limits, profile); valid.HasError())
                return Result<PlaySavePrefabCapture>::Failure(valid.ErrorValue());
            return Result<PlaySavePrefabCapture>::Success({std::move(map).Value(), std::move(captured)});
        }

        /** @brief Charges the real aggregate canonical recipe payload, including unpublished reservations, before Scene admission.
         * This private serialization measures/admit metadata only; it is never published as committed world state.
         */
        Result<void> Budget(const Entry &candidate) const {
            Prefab::PrefabPersistentState bounded{world, {}};
            bounded.instances.reserve(entries.size() + 1);
            for (const auto &entry : entries)
                if (!entry->cancelled)
                    bounded.instances.push_back(entry->recipe);
            bounded.instances.push_back(candidate.recipe);
            const auto encoded = Prefab::EncodePrefabPersistentState(bounded, limits, profile);
            return encoded.HasError() ? Result<void>::Failure(encoded.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Requires exact actual content authority before any new durable identity is issued. */
        Result<void> ValidateCreationSource(const Prefab::PrefabTemplateLease &source, const Prefab::PrefabSpawnRequest &request) const {
            if (!source.Template() || source.Scene() != scene || source.Template()->Data().assetId != request.source.asset ||
                source.Artifact().Digest() != request.source.artifactDigest || !source.Target() ||
                *source.Target() != request.source.target || !source.Template()->Data().rootSourceRevision)
                return Invalid<void>("Recook the actual prefab with complete root provenance before durable spawning.");
            return Result<void>::Success();
        }

        /** @brief Allocates and charges the complete recipe/publication payload before structural admission. */
        Result<std::shared_ptr<Prefab::IPrefabPersistencePublication>> Reserve(const Prefab::PreparedPrefabSpawn &prepared,
                                                                               const Prefab::PrefabSpawnRequest &request,
                                                                               std::span<const Runtime::RuntimeComponentSet> projected,
                                                                               std::span<const Assets::AssetId> lineage,
                                                                               std::string_view creator,
                                                                               const Runtime::PersistentEntityIdentityMap &map) {
            using Outcome = Result<std::shared_ptr<Prefab::IPrefabPersistencePublication>>;
            const auto &source = prepared.Source();
            auto identity = IssuePlaySaveIdentity<Runtime::SavePrefabInstanceId>(*entropy);
            if (identity.HasError())
                return Outcome::Failure(identity.ErrorValue());
            for (const auto &entry : entries)
                if (entry->recipe.identity == identity.Value())
                    return Outcome::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
            auto entry = std::make_shared<Entry>();
            entry->source = source;
            entry->recipe.identity = identity.Value();
            entry->recipe.creator = creator;
            entry->recipe.source = request.source;
            entry->recipe.rootSourceRevision = *source.Template()->Data().rootSourceRevision;
            entry->recipe.placement = request.placement;
            entry->recipe.creationLineage.assign(lineage.begin(), lineage.end());
            entry->recipe.initialization = request.initialization;
            entry->recipe.overrides =
                request.overrides.value_or(Prefab::PrefabValueOverrideSet{request.source.asset,
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
        Result<std::shared_ptr<Prefab::IPrefabPersistencePublication>> Prepare(const Prefab::PreparedPrefabSpawn &prepared,
                                                                               const Prefab::PrefabSpawnRequest &request,
                                                                               std::span<const Runtime::RuntimeComponentSet> projected,
                                                                               std::span<const Assets::AssetId> lineage,
                                                                               std::string_view creator) {
            using Outcome = Result<std::shared_ptr<Prefab::IPrefabPersistencePublication>>;
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

}  // namespace Horo::Application::Internal
