#include "Horo/Runtime/Scene/SaveableComponentState.h"
#include "SceneTestIdentity.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using SceneTest::Id;

        Gameplay::ComponentTypeId Type(const std::string_view value = "game.tests.health") {
            return Gameplay::ComponentTypeId::Parse(value).Value();
        }

        Gameplay::ComponentRegistry Components(const std::uint32_t schemaVersion = 2) {
            Gameplay::ComponentRegistry components;
            REQUIRE(components.Register({.typeId = Type(), .schemaVersion = schemaVersion, .displayName = "Health"}).HasValue());
            REQUIRE(components.Register({.typeId = Type("game.tests.optional"), .schemaVersion = schemaVersion, .displayName = "Optional"})
                        .HasValue());
            REQUIRE(components.Freeze().HasValue());
            return components;
        }

        CanonicalEncodedValue Encoded(const std::uint32_t value) {
            CanonicalValueWriter writer;
            REQUIRE(writer.WriteUInt32(value).HasValue());
            return std::move(writer).Finalize().Value();
        }

        std::vector<std::byte> Bytes(const std::uint32_t value) {
            const auto encoded = Encoded(value);
            return {encoded.Bytes().begin(), encoded.Bytes().end()};
        }

        PersistentEntityIdentityMap Identities() {
            const std::array records{
                PersistentEntityRecord{.identity = Id<PersistentEntityId>(1),
                                       .generation = {3},
                                       .provenance = AuthoredPersistentEntityProvenance{SceneDefinitionId{4}, SceneObjectId{5}},
                                       .disposition = PersistentEntityDisposition::Live}};
            const std::array bindings{PersistentEntityRuntimeBinding{.identity = Id<PersistentEntityId>(1),
                                                                     .generation = {3},
                                                                     .runtime = EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}}};
            return PersistentEntityIdentityMap::Create(SceneRuntimeId{8}, records, bindings).Value();
        }

        class Prepared final : public IPreparedSaveableComponentState {
        public:
            explicit Prepared(bool &published) : published_(published) {}

            void PublishPrepared() noexcept override {
                published_ = true;
            }

        private:
            bool &published_;
        };

        class Authority final : public ISaveableComponentAuthority {
        public:
            bool allowed{true};
            Gameplay::ComponentTypeId expectedType{Type()};

            bool Contains(const EntityRef entity, const Gameplay::ComponentTypeId &type) const noexcept override {
                return allowed && entity == EntityRef{SceneRuntimeId{8}, EntityId{9, 1}} && type == expectedType;
            }
        };

        class Adapter final : public ISaveableComponentStateAdapter {
        public:
            explicit Adapter(const Gameplay::ComponentTypeId type = Type())
                : descriptor_{.type = type, .schemaVersion = 2, .maximumPayloadBytes = 64} {}

            const SaveableComponentAdapterDescriptor &Descriptor() const noexcept override {
                return descriptor_;
            }

            Result<std::optional<CanonicalEncodedValue>> Capture(const EntityRef) const override {
                if (omitCapture)
                    return Result<std::optional<CanonicalEncodedValue>>::Success(std::nullopt);
                return Result<std::optional<CanonicalEncodedValue>>::Success(std::optional{Encoded(capturedValue)});
            }

            Result<CanonicalEncodedValue> Migrate(const std::uint32_t sourceSchema, CanonicalValueReader source) const override {
                ++migrationCalls;
                CHECK(sourceSchema == 1);
                auto value = source.ReadUInt32();
                if (value.HasError())
                    return Result<CanonicalEncodedValue>::Failure(value.ErrorValue());
                return Result<CanonicalEncodedValue>::Success(Encoded(value.Value() + 1));
            }

            Result<void> Validate(CanonicalValueReader source) const override {
                ++validationCalls;
                auto value = source.ReadUInt32();
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                validatedValue = value.Value();
                return Result<void>::Success();
            }

            Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareApply(const EntityRef entity,
                                                                                  CanonicalValueReader source) const override {
                ++applyCalls;
                appliedEntity = entity;
                if (returnNullPrepared)
                    return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(nullptr);
                auto value = source.ReadUInt32();
                if (value.HasError())
                    return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(value.ErrorValue());
                appliedValue = value.Value();
                return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(std::make_unique<Prepared>(published));
            }

            Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareDefault(const EntityRef entity) const override {
                ++defaultCalls;
                appliedEntity = entity;
                if (returnNullPrepared)
                    return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(nullptr);
                return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(std::make_unique<Prepared>(published));
            }

            SaveableComponentAdapterDescriptor descriptor_;
            bool omitCapture{};
            bool returnNullPrepared{};
            std::uint32_t capturedValue{12};
            mutable std::uint32_t migrationCalls{};
            mutable std::uint32_t validationCalls{};
            mutable std::uint32_t applyCalls{};
            mutable std::uint32_t defaultCalls{};
            mutable std::uint32_t validatedValue{};
            mutable std::uint32_t appliedValue{};
            mutable EntityRef appliedEntity{};
            mutable bool published{};
        };

        void RequireCode(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        SaveableComponentAdapterRegistry Registry(const std::shared_ptr<Adapter> &adapter,
                                                  const SaveableComponentPresence presence = SaveableComponentPresence::Required) {
            const auto components = Components();
            const std::array requirements{SaveableComponentRequirement{Type(), presence}};
            const std::array<std::shared_ptr<const ISaveableComponentStateAdapter>, 1> adapters{adapter};
            return SaveableComponentAdapterRegistry::Create(components, requirements, adapters).Value();
        }

        SavedComponentStateRecord Record(const std::uint32_t schemaVersion = 2, const PersistentEntityGeneration generation = {3}) {
            return {.entity = Id<PersistentEntityId>(1),
                    .generation = generation,
                    .type = Type(),
                    .schemaVersion = schemaVersion,
                    .payload = Bytes(20)};
        }

        TEST_CASE("Required component adapters gate Scene activation while optional adapters may be absent",
                  "[runtime][scene][save][component]") {
            const auto components = Components();
            const std::array required{SaveableComponentRequirement{Type(), SaveableComponentPresence::Required}};
            RequireCode(SaveableComponentAdapterRegistry::Create(components, required,
                                                                 std::span<const std::shared_ptr<const ISaveableComponentStateAdapter>>{}),
                        "scene.save_component.adapter_missing");

            const std::array optional{SaveableComponentRequirement{Type("game.tests.optional"), SaveableComponentPresence::Optional}};
            auto registry =
                SaveableComponentAdapterRegistry::Create(components, optional,
                                                         std::span<const std::shared_ptr<const ISaveableComponentStateAdapter>>{});
            REQUIRE(registry.HasValue());
        }

        TEST_CASE("Registry composition rejects mutable duplicate and mismatched declarations", "[runtime][scene][save][component]") {
            Gameplay::ComponentRegistry mutableComponents;
            const std::array required{SaveableComponentRequirement{Type(), SaveableComponentPresence::Required}};
            auto adapter = std::make_shared<Adapter>();
            const std::array<std::shared_ptr<const ISaveableComponentStateAdapter>, 1> adapters{adapter};
            RequireCode(SaveableComponentAdapterRegistry::Create(mutableComponents, required, adapters), "scene.save_component.invalid");

            const auto components = Components(7);
            const std::array duplicateRequirements{required[0], required[0]};
            RequireCode(SaveableComponentAdapterRegistry::Create(components, duplicateRequirements, adapters),
                        "scene.save_component.duplicate");
            const std::array<std::shared_ptr<const ISaveableComponentStateAdapter>, 2> duplicateAdapters{adapter, adapter};
            RequireCode(SaveableComponentAdapterRegistry::Create(components, required, duplicateAdapters),
                        "scene.save_component.duplicate");
            RequireCode(SaveableComponentAdapterRegistry::Create(components, required, adapters, 0), "scene.save_component.invalid");

            auto registry = SaveableComponentAdapterRegistry::Create(components, required, adapters);
            REQUIRE(registry.HasValue());
            adapter->descriptor_.schemaVersion = 99;
            adapter->descriptor_.maximumPayloadBytes = 1;
            Authority authority;
            auto captured = registry.Value().Capture(EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}, Type(), authority);
            REQUIRE(captured.HasValue());
            REQUIRE(captured.Value().has_value());
            CHECK(captured.Value()->schemaVersion == 2);
        }

        TEST_CASE("Optional capture omission and payload bounds are explicit", "[runtime][scene][save][component]") {
            auto adapter = std::make_shared<Adapter>();
            adapter->omitCapture = true;
            adapter->descriptor_.maximumPayloadBytes = 1;
            auto registry = Registry(adapter, SaveableComponentPresence::Optional);
            Authority authority;
            auto omitted = registry.Capture(EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}, Type(), authority);
            REQUIRE(omitted.HasValue());
            CHECK_FALSE(omitted.Value().has_value());

            adapter->omitCapture = false;
            RequireCode(registry.Capture(EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}, Type(), authority),
                        "scene.save_component.payload_invalid");
        }

        TEST_CASE("Capture routes only validated entity and type ownership to canonical adapters", "[runtime][scene][save][component]") {
            auto adapter = std::make_shared<Adapter>();
            auto registry = Registry(adapter);
            Authority authority;
            auto captured = registry.Capture(EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}, Type(), authority);
            REQUIRE(captured.HasValue());
            REQUIRE(captured.Value().has_value());
            CHECK(captured.Value()->schemaVersion == 2);
            CHECK(std::ranges::equal(captured.Value()->payload.Bytes(), Encoded(12).Bytes()));

            authority.allowed = false;
            RequireCode(registry.Capture(EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}, Type(), authority),
                        "scene.save_component.identity_mismatch");
            adapter->omitCapture = true;
            authority.allowed = true;
            RequireCode(registry.Capture(EntityRef{SceneRuntimeId{8}, EntityId{9, 1}}, Type(), authority),
                        "scene.save_component.payload_invalid");
        }

        TEST_CASE("Restore migrates validates and stages state only after stable identity resolution",
                  "[runtime][scene][save][component]") {
            auto adapter = std::make_shared<Adapter>();
            auto registry = Registry(adapter);
            const auto identities = Identities();
            Authority authority;
            auto record = Record(1);
            auto prepared = registry.PrepareRestore(record, identities, authority);
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value() != nullptr);
            CHECK(adapter->migrationCalls == 1);
            CHECK(adapter->validationCalls == 1);
            CHECK(adapter->applyCalls == 1);
            CHECK(adapter->validatedValue == 21);
            CHECK(adapter->appliedValue == 21);
            CHECK_FALSE(adapter->published);
            prepared.Value()->PublishPrepared();
            CHECK(adapter->published);
        }

        TEST_CASE("Restore rejects stale entity component and schema identities before adapter application",
                  "[runtime][scene][save][component]") {
            auto adapter = std::make_shared<Adapter>();
            auto registry = Registry(adapter);
            const auto identities = Identities();
            Authority authority;
            auto record = Record(2, {2});
            RequireCode(registry.PrepareRestore(record, identities, authority), "scene.persistence.identity_stale");
            record.generation = {3};
            record.schemaVersion = 3;
            RequireCode(registry.PrepareRestore(record, identities, authority), "scene.save_component.schema_unsupported");
            record.schemaVersion = 2;
            authority.allowed = false;
            RequireCode(registry.PrepareRestore(record, identities, authority), "scene.save_component.identity_mismatch");
            CHECK(adapter->applyCalls == 0);
        }

        TEST_CASE("Restore rejects malformed and empty prepared component state", "[runtime][scene][save][component]") {
            auto adapter = std::make_shared<Adapter>();
            auto registry = Registry(adapter);
            const auto identities = Identities();
            Authority authority;
            auto record = Record(0);
            RequireCode(registry.PrepareRestore(record, identities, authority), "scene.save_component.invalid");
            record.schemaVersion = 2;
            record.payload = {std::byte{0xFF}};
            RequireCode(registry.PrepareRestore(record, identities, authority), "save.canonical_codec.corrupt");
            record.payload = Bytes(20);
            adapter->returnNullPrepared = true;
            RequireCode(registry.PrepareRestore(record, identities, authority), "scene.save_component.payload_invalid");
            RequireCode(registry.PrepareDefault(Id<PersistentEntityId>(1), {3}, Type(), identities, authority),
                        "scene.save_component.payload_invalid");
        }

        TEST_CASE("Defaults are staged through the same validated entity and type route", "[runtime][scene][save][component]") {
            auto adapter = std::make_shared<Adapter>();
            auto registry = Registry(adapter);
            const auto identities = Identities();
            Authority authority;
            auto prepared = registry.PrepareDefault(Id<PersistentEntityId>(1), {3}, Type(), identities, authority);
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value() != nullptr);
            CHECK(adapter->defaultCalls == 1);
            CHECK(adapter->appliedEntity == EntityRef{SceneRuntimeId{8}, EntityId{9, 1}});
        }
    }  // namespace
}  // namespace Horo::Runtime
