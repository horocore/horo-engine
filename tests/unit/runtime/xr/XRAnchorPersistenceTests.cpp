#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/XR/XRAnchorPersistence.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::XR {
    namespace {
        using namespace Runtime;
        using Tests::RequireFailureIdentity;

        template <typename T> T Take(Result<T> value) {
            if (value.HasError())
                UNSCOPED_INFO(value.ErrorValue().message);
            REQUIRE(value.HasValue());
            return std::move(value).Value();
        }

        struct Resources final : IXRSessionResources {
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        struct ProtectedStore final : IXRProtectedAnchorStore {
            std::uint64_t now{20};
            XRProtectedAnchorEntry entry{Take(XRProtectedAnchorReference::FromBytes({3U})), 100};
            XRSpatialAnchorId live;
            bool authorized{true};
            bool available{true};
            bool fault{};
            mutable unsigned lookups{};

            Result<void> Admit(const XRAnchorStoreScope &) const override {
                return authorized ? Result<void>::Success() : Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
            }

            std::uint64_t CurrentUnixSeconds() const noexcept override {
                return now;
            }

            Result<XRProtectedAnchorEntry> FindLive(const XRAnchorStoreScope &, const XRSpatialAnchorId &anchor) const override {
                return anchor == live ? Result<XRProtectedAnchorEntry>::Success(entry)
                                      : Result<XRProtectedAnchorEntry>::Failure(MakeError(XRErrors::IdentityStale));
            }

            Result<std::optional<XRProtectedAnchorEntry>> Resolve(const XRAnchorStoreScope &,
                                                                  const XRProtectedAnchorReference &reference) const override {
                ++lookups;
                if (fault)
                    return Result<std::optional<XRProtectedAnchorEntry>>::Failure(MakeError(XRErrors::OperationUnavailable));
                return Result<std::optional<XRProtectedAnchorEntry>>::Success(
                    available && reference == entry.reference ? std::optional{entry} : std::nullopt);
            }
        };

        XRAnchorScope Activate(XRSessionLifecycle &lifecycle, XRSpatialAnchors &anchors, const std::uint64_t runtime) {
            const XRSystemLimits limits{.maximumViews = 2, .maximumSpaces = 2, .maximumActions = 4, .maximumDevices = 1};
            XRCapabilityDescriptor descriptor{.system = {.runtime = Take(XRRuntimeGeneration::Create(runtime)), .slot = {0, 1}},
                                              .revision = Take(XRCapabilityRevision::Create(1)),
                                              .limits = limits};
            descriptor.states.fill(XRCapabilityState::Available);
            const auto snapshot = Take(XRCapabilitySnapshot::Create(descriptor));
            const auto features = NegotiateXRFeatures(snapshot, snapshot.System(), snapshot.Revision(),
                                                      {.profile = XRFeatureProfile::Projection1_0, .requestedLimits = limits});
            REQUIRE(features.plan);
            const auto session = Take(lifecycle.Activate(snapshot, snapshot.System(), snapshot.Revision(), *features.plan));
            for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
                REQUIRE(lifecycle.ApplyEvent(session, event).HasValue());
            const XRAnchorScope scope{.space = {.id = {session, {0, 1}},
                                                .kind = XRSpaceKind::World,
                                                .worldOriginRevision = Take(XRWorldOriginRevision::Create(1))},
                                      .provider = Take(XRAnchorProviderGeneration::Create(runtime)),
                                      .grant = Take(XRAnchorGrant::Create(runtime))};
            REQUIRE(anchors.Activate(scope, {1, Duration::FromNanoseconds(20)}, XRCapabilityState::Available).HasValue());
            return scope;
        }

        struct Fixture final {
            Resources resources;
            XRSessionLifecycle lifecycle{resources};
            std::shared_ptr<XRSpatialAnchors> anchors{std::make_shared<XRSpatialAnchors>(lifecycle)};
            XRAnchorScope live;
            XRAnchorStoreScope scope{Take(XRAnchorStoreProvider::FromBytes({1U})), Take(XRAnchorStorePartition::FromBytes({2U})), 1, 7};
            std::shared_ptr<ProtectedStore> store{std::make_shared<ProtectedStore>()};
            SaveParticipantId participant{Take(SaveParticipantId::Parse("horo.xr.anchor"))};
            SaveRecordId record{Take(SaveRecordId::FromBytes({4U}))};
            std::shared_ptr<XRAnchorPersistence> owner;
            SaveParticipantRegistrySnapshot registry;

            explicit Fixture(const std::uint64_t runtime = 1) : live(Activate(lifecycle, *anchors, runtime)) {
                owner = Take(XRAnchorPersistence::Bind(participant, record, anchors, live, scope, store));
                CanonicalStateParticipantRegistry bindings;
                REQUIRE(bindings.Register(owner->Descriptor(), owner).HasValue());
                registry = Take(bindings.Snapshot());
            }

            void RecordLocalized() {
                store->live = Take(anchors->Create(live, Take(XRRuntimeTime::Create(50))));
                const auto now = Take(XRRuntimeTime::Create(60));
                REQUIRE(anchors
                            ->Publish(live, store->live,
                                      {now, XRAnchorLocalization::Localized, XRAnchorPose{.positionMeters = {2.0F, 4.0F, 8.0F}}})
                            .HasValue());
                REQUIRE(owner->RecordLocalized(store->live, now).HasValue());
            }

            auto Capture(const RuntimeSaveCaptureLimits &limits = {}) {
                const RuntimeSaveCaptureProvenance provenance{Take(CapturedStateId::FromBytes({5U})), {10}, 11, 12, registry.Generation()};
                auto builder = Take(RuntimeSaveCaptureBuilder::Create(provenance, registry, limits));
                REQUIRE(builder.CaptureParticipants().HasValue());
                return Take(builder.Seal());
            }

            auto Transaction(std::unique_ptr<IStagedRestoreParticipant> staged, const OperationId id = 121) const {
                std::vector<std::unique_ptr<IStagedRestoreParticipant>> candidates;
                candidates.push_back(std::move(staged));
                auto operation =
                    Take(CreateSaveOperation({.operation = id, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}));
                return Take(StagedRestoreTransaction::Create({id, registry.Generation(), 19, 23, 1}, std::move(operation), registry,
                                                             std::move(candidates)));
            }

            StagedRestoreActivationEvidence Evidence() const noexcept {
                return {registry.Generation(), 19, 23};
            }
        };
    }  // namespace

    TEST_CASE("XR durable anchor records restore through canonical Save with only an opaque protected lookup", "[xr][anchors][save]") {
        Fixture original;
        REQUIRE(original.Capture().Records().empty());
        original.RecordLocalized();
        const auto saved = original.Capture();
        REQUIRE(saved.Records().size() == 1);
        CHECK(saved.Records().front().Record().record == original.record);
        const auto bytes = saved.Records().front().Segment(0);
        constexpr std::array<std::byte, 72> wire = [] {
            std::array<std::byte, 72> value{};
            value[0] = std::byte{'X'};
            value[1] = std::byte{'R'};
            value[2] = std::byte{'A'};
            value[3] = std::byte{'1'};
            value[4] = std::byte{1};
            value[20] = std::byte{2};
            value[36] = std::byte{3};
            value[52] = std::byte{1};
            value[56] = std::byte{7};
            value[64] = std::byte{100};
            return value;
        }();
        CHECK(std::ranges::equal(bytes, wire));

        Fixture restored{2};
        auto staged = Take(restored.owner->StageRestore(bytes));
        auto transaction = restored.Transaction(std::move(staged.participant));
        REQUIRE(transaction.Prepare().HasValue());
        CHECK(Take(restored.owner->Read()).status == XRDurableAnchorStatus::Missing);
        REQUIRE(transaction.Activate(restored.Evidence(), *staged.publication).HasValue());
        const auto loaded = Take(restored.owner->Read());
        CHECK(loaded.status == XRDurableAnchorStatus::Unlocalized);
        REQUIRE(loaded.reference.has_value());
        CHECK(*loaded.reference == restored.store->entry.reference);
        RequireFailureIdentity(restored.anchors->Read(restored.live, original.store->live, Take(XRRuntimeTime::Create(60))),
                               XRErrors::IdentityStale);
        restored.store->now = restored.store->entry.expiresAtUnixSeconds;
        CHECK(Take(restored.owner->Read()).status == XRDurableAnchorStatus::Stale);
        CHECK_FALSE(Take(restored.owner->Read()).reference.has_value());
        --restored.store->now;
        RequireFailureIdentity(restored.owner->Read(), XRErrors::TimeDomainIncompatible);
    }

    TEST_CASE("XR anchor restore returns explicit missing or stale and refuses malformed records", "[xr][anchors][save]") {
        Fixture source;
        source.RecordLocalized();
        const auto capture = source.Capture();
        const auto bytes = capture.Records().front().Segment(0);
        Fixture target{2};

        SECTION("missing protected lookup") {
            target.store->available = false;
            auto staged = Take(target.owner->StageRestore(bytes));
            auto transaction = target.Transaction(std::move(staged.participant));
            REQUIRE(transaction.Prepare().HasValue());
            REQUIRE(transaction.Activate(target.Evidence(), *staged.publication).HasValue());
            CHECK(Take(target.owner->Read()).status == XRDurableAnchorStatus::Missing);
            CHECK(target.store->lookups == 1);
        }
        SECTION("expired lookup bypasses the provider entirely") {
            target.store->now = target.store->entry.expiresAtUnixSeconds;
            auto staged = Take(target.owner->StageRestore(bytes));
            auto transaction = target.Transaction(std::move(staged.participant));
            REQUIRE(transaction.Prepare().HasValue());
            REQUIRE(transaction.Activate(target.Evidence(), *staged.publication).HasValue());
            CHECK(Take(target.owner->Read()).status == XRDurableAnchorStatus::Stale);
            CHECK(target.store->lookups == 0);
        }
        SECTION("untrusted routing cannot select a different protected partition") {
            std::vector<std::byte> foreign{bytes.begin(), bytes.end()};
            foreign[20] = std::byte{9};
            auto staged = Take(target.owner->StageRestore(foreign));
            auto transaction = target.Transaction(std::move(staged.participant));
            REQUIRE(transaction.Prepare().HasValue());
            REQUIRE(transaction.Activate(target.Evidence(), *staged.publication).HasValue());
            CHECK(Take(target.owner->Read()).status == XRDurableAnchorStatus::Stale);
            CHECK(target.store->lookups == 0);
        }
        SECTION("malformed or over-capacity record stays unpublished") {
            RequireFailureIdentity(target.owner->StageRestore(bytes.first(bytes.size() - 1)), XRErrors::OperationInvalid);
            std::array<std::byte, 73> oversized{};
            RequireFailureIdentity(target.owner->StageRestore(oversized), XRErrors::CapacityExceeded);
            std::vector<std::byte> corrupt{bytes.begin(), bytes.end()};
            corrupt[3] = std::byte{'2'};
            auto staged = Take(target.owner->StageRestore(corrupt));
            auto transaction = target.Transaction(std::move(staged.participant));
            RequireFailureIdentity(transaction.Prepare(), XRErrors::OperationInvalid);
            CHECK(target.store->lookups == 0);
        }
    }

    TEST_CASE("XR anchor restore fails closed for absent providers, revocation and shutdown", "[xr][anchors][save]") {
        Fixture fixture;
        RequireFailureIdentity(XRAnchorPersistence::Bind(fixture.participant, fixture.record, fixture.anchors, fixture.live, fixture.scope,
                                                         {}),
                               XRErrors::OperationUnsupported);
        fixture.RecordLocalized();
        const auto capture = fixture.Capture();
        const auto bytes = capture.Records().front().Segment(0);
        auto staged = Take(fixture.owner->StageRestore(bytes));
        auto transaction = fixture.Transaction(std::move(staged.participant));
        Fixture separate{2};
        CHECK(Take(separate.owner->Read()).status == XRDurableAnchorStatus::Missing);
        SECTION("failed provider lookup rolls back before publishing") {
            fixture.store->fault = true;
            RequireFailureIdentity(transaction.Prepare(), XRErrors::OperationUnavailable);
            CHECK(Take(fixture.owner->Read()).reference == fixture.store->entry.reference);
        }
        SECTION("revoked local access fails the aggregate gate") {
            REQUIRE(transaction.Prepare().HasValue());
            fixture.store->authorized = false;
            RequireFailureIdentity(transaction.Activate(fixture.Evidence(), *staged.publication), XRErrors::PermissionDenied);
            RequireFailureIdentity(fixture.owner->Read(), XRErrors::PermissionDenied);
        }
        SECTION("shutdown invalidates the gate and durable reads") {
            REQUIRE(transaction.Prepare().HasValue());
            fixture.lifecycle.Shutdown();
            RequireFailureIdentity(transaction.Activate(fixture.Evidence(), *staged.publication), XRErrors::IdentityStale);
            RequireFailureIdentity(fixture.owner->Read(), XRErrors::IdentityStale);
        }
    }
}  // namespace Horo::XR
