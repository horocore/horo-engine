#include "Horo/Physics/PhysicsStaticTileAdmission.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::Physics {
    namespace {
        using enum PhysicsStaticTileOperation;
        using enum PhysicsStaticTileOutcome;

        const PhysicsShapeCookTargetDigest Target{};

        PhysicsStaticTileKey Key(std::uint64_t tile) {
            return {Assets::AssetId::FromBytes({42}), PhysicsShapeSubresourceId::FromValue(tile)};
        }

        PhysicsStaticTileUpdate Update(PhysicsStaticTileOperation operation, PhysicsStaticTileKey key, std::uint64_t expected,
                                       std::uint64_t version) {
            return {operation, key, expected,
                    PhysicsStaticTileContent{version,
                                             {key.asset, key.tile, PhysicsCookedShapeKind::HeightField, Sha256Digest{}, Sha256Digest{},
                                              Target},
                                             {{0, 0, 0}, {10, 10, 10}}}};
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        PhysicsStaticTileAdmission Open(std::uint64_t identity = 1) {
            auto created = PhysicsStaticTileAdmission::Create(PhysicsWorldId::Create(identity).Value(), Target, 2, 1);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        TEST_CASE("Static collision tile receipts separate acceptance from safe-point acknowledgement", "[physics][static-tile]") {
            auto admission = Open();
            const auto producer = admission.OpenProducer().Value();
            const auto initial = admission.Admit(producer, Update(Submit, Key(1), 0, 1));
            REQUIRE(initial.HasValue());
            CHECK(initial.Value().stage == PhysicsStaticTileStage::Accepted);
            const auto pending = admission.ReadPending(initial.Value());
            REQUIRE(pending.HasValue());
            REQUIRE(pending.Value().content);
            CHECK(pending.Value().content->shape.kind == PhysicsCookedShapeKind::HeightField);
            const auto published = admission.Complete(initial.Value(), Applied);
            REQUIRE(published.HasValue());
            CHECK(published.Value().stage == PhysicsStaticTileStage::Activated);
            CheckError(admission.Admit(producer, Update(Submit, Key(1), 1, 2)), PhysicsErrors::StaticTileDuplicate);
            CheckError(admission.Complete(initial.Value(), Applied), PhysicsErrors::HandleStale);

            const auto replacement = admission.Admit(producer, Update(Replace, Key(1), 1, 2));
            REQUIRE(replacement.HasValue());
            CHECK(admission.Complete(replacement.Value(), Failed).Value().stage == PhysicsStaticTileStage::Failed);
            const auto retry = admission.Admit(producer, Update(Replace, Key(1), 1, 2));
            REQUIRE(retry.HasValue());
            CHECK(retry.Value().request != replacement.Value().request);
            CheckError(admission.Complete(replacement.Value(), Applied), PhysicsErrors::HandleStale);
            CHECK(admission.Complete(retry.Value(), Applied).Value().version == 2);

            CheckError(admission.CloseProducer(producer), PhysicsErrors::InvalidState);
            CheckError(admission.Admit(producer, {Retire, Key(1), 1}), PhysicsErrors::HandleStale);
            const auto retire = admission.Admit(producer, {Retire, Key(1), 2});
            REQUIRE(retire.HasValue());
            CHECK(retire.Value().stage == PhysicsStaticTileStage::Accepted);
            CheckError(admission.CloseProducer(producer), PhysicsErrors::InvalidState);
            CHECK(admission.Complete(retire.Value(), Applied).Value().stage == PhysicsStaticTileStage::Retired);
            CheckError(admission.Admit(producer, Update(Submit, Key(1), 0, 1)), PhysicsErrors::HandleStale);
            const auto reloaded = admission.Admit(producer, Update(Submit, Key(1), 2, 3));
            REQUIRE(reloaded.HasValue());
            REQUIRE(admission.CloseProducer(producer).HasValue());
            CheckError(admission.Complete(reloaded.Value(), Applied), PhysicsErrors::HandleStale);
        }

        TEST_CASE("Static collision tile admission preserves ownership on refusal and cancellation", "[physics][static-tile]") {
            auto admission = Open();
            const auto producer = admission.OpenProducer().Value();
            const auto other = admission.OpenProducer().Value();
            CheckError(admission.OpenProducer(), PhysicsErrors::CapacityExceeded);
            const auto request = Update(Submit, Key(1), 0, 1);
            auto invalid = request;
            invalid.content->bounds.maximum.x = std::numeric_limits<float>::infinity();
            CheckError(admission.Admit(producer, invalid), PhysicsErrors::DescriptorInvalid);
            auto foreign = Open(2);
            CheckError(foreign.Admit(producer, request), PhysicsErrors::HandleWorldMismatch);
            const auto accepted = admission.Admit(producer, request);
            REQUIRE(accepted.HasValue());
            CheckError(admission.Admit(producer, request), PhysicsErrors::StaticTileDuplicate);
            CheckError(admission.Admit(other, request), PhysicsErrors::StaticTileOwnerConflict);
            CheckError(admission.Admit(other, Update(Submit, Key(2), 0, 1)), PhysicsErrors::CapacityExceeded);
            CHECK(admission.Complete(accepted.Value(), Cancelled).Value().stage == PhysicsStaticTileStage::Cancelled);
            REQUIRE(admission.CloseProducer(producer).HasValue());
            const auto recycled = admission.OpenProducer().Value();
            CHECK(recycled.slot.index == producer.slot.index);
            CHECK(recycled.slot.generation != producer.slot.generation);
            CheckError(admission.Admit(producer, request), PhysicsErrors::HandleStale);
            const auto fresh = admission.Admit(recycled, request);
            REQUIRE(fresh.HasValue());
            CheckError(admission.ReadPending(accepted.Value()), PhysicsErrors::HandleStale);
            auto transferred = std::move(admission);
            CheckError(admission.ReadPending(fresh.Value()), PhysicsErrors::HandleStale);
            const auto applied = transferred.Complete(fresh.Value(), Applied);
            REQUIRE(applied.HasValue());
            CHECK(applied.Value().stage == PhysicsStaticTileStage::Activated);
        }
    }  // namespace
}  // namespace Horo::Physics
