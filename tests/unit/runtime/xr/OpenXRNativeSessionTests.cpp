#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "OpenXRProtectedAnchorStore.h"
#include "OpenXRSessionEvents.h"
#include "support/OpenXREventScriptTestSupport.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <array>
#include <string>

namespace Horo::Tests::OpenXR {
    namespace {
        template <typename T> T AnchorValue(Result<T> result) {
            if (result.HasError())
                UNSCOPED_INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct AnchorResources final : IXRSessionResources {
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        /** @brief Delivers focus loss and recovery in one native event poll. */
        void InterruptAnchorFocus(OpenXRNativeSession &owner, XRSessionLifecycle &lifecycle, XrSession session) {
            XRFrameLifecycle frames{lifecycle};
            EventScript script;
            script.session = session;
            OpenXRSessionEvents events{owner, lifecycle, frames};
            REQUIRE(events.Bind({&EventScript::GetProc}, {}).HasValue());
            script.Push(XR_SESSION_STATE_VISIBLE);
            script.Push(XR_SESSION_STATE_FOCUSED);
            EventScript::active = &script;
            const auto observed = events.Poll(true);
            EventScript::active = nullptr;
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().actionsInterrupted);
            CHECK(lifecycle.Snapshot().admitsActions);
        }

        struct AnchorScript final {
            static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession, const XrSpatialAnchorCreateInfoMSFT *info, XrSpatialAnchorMSFT *out) {
                REQUIRE(info->time == 10);
                REQUIRE(info->space == NativeHandle<XrSpace>(11));
                const auto result = Script::Record("anchor");
                if (result == XR_SUCCESS)
                    *out = NativeHandle<XrSpatialAnchorMSFT>(21);
                return result;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL CreateSpace(XrSession, const XrSpatialAnchorSpaceCreateInfoMSFT *info, XrSpace *out) {
                REQUIRE(info->anchor == NativeHandle<XrSpatialAnchorMSFT>(21));
                const auto result = Script::Record("anchor-space");
                *out = NativeHandle<XrSpace>(result == XR_SUCCESS ? 12 : 99);
                return result;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrSpatialAnchorMSFT anchor) {
                REQUIRE(anchor == NativeHandle<XrSpatialAnchorMSFT>(21));
                return Script::Record("destroy-anchor");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL DestroySpace(XrSpace space) {
                REQUIRE(space == NativeHandle<XrSpace>(12));
                return Script::Record("destroy-anchor-space");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Locate(XrSpace space, XrSpace world, XrTime, XrSpaceLocation *location) {
                REQUIRE(space == NativeHandle<XrSpace>(12));
                REQUIRE(world == NativeHandle<XrSpace>(11));
                location->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                          XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
                location->pose = {{0, 0, 0, 1}, {1, 2, 3}};
                return Script::Record("locate-anchor");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance, const char *name, PFN_xrVoidFunction *out) {
                const std::array symbols{std::pair{"xrCreateSpatialAnchorMSFT", reinterpret_cast<PFN_xrVoidFunction>(&Create)},
                                         std::pair{"xrCreateSpatialAnchorSpaceMSFT", reinterpret_cast<PFN_xrVoidFunction>(&CreateSpace)},
                                         std::pair{"xrDestroySpatialAnchorMSFT", reinterpret_cast<PFN_xrVoidFunction>(&Destroy)},
                                         std::pair{"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(&DestroySpace)},
                                         std::pair{"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(&Locate)}};
                const auto found = std::ranges::find_if(symbols, [name](const auto &entry) {
                    return std::string_view{entry.first} == name;
                });
                if (found == symbols.end())
                    return XR_ERROR_FUNCTION_UNSUPPORTED;
                *out = found->second;
                return XR_SUCCESS;
            }
        };

        struct AnchorStoreScript final {
            static inline thread_local AnchorStoreScript *active{};
            std::optional<XrSpatialAnchorPersistenceNameMSFT> entry;
            CancellationSource *revokeOnPersist{};

            AnchorStoreScript() {
                active = this;
            }

            ~AnchorStoreScript() {
                active = nullptr;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Open(XrSession, XrSpatialAnchorStoreConnectionMSFT *out) {
                *out = NativeHandle<XrSpatialAnchorStoreConnectionMSFT>(31);
                return Script::Record("anchor-store-open");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Close(XrSpatialAnchorStoreConnectionMSFT) {
                return Script::Record("anchor-store-close");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Persist(XrSpatialAnchorStoreConnectionMSFT,
                                                          const XrSpatialAnchorPersistenceInfoMSFT *info) {
                active->entry = info->spatialAnchorPersistenceName;
                if (active->revokeOnPersist)
                    active->revokeOnPersist->RequestCancellation();
                return Script::Record("anchor-store-persist");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Read(XrSpatialAnchorStoreConnectionMSFT, std::uint32_t capacity, std::uint32_t *count,
                                                       XrSpatialAnchorPersistenceNameMSFT *names) {
                *count = active->entry ? 1U : 0U;
                if (capacity < *count)
                    return XR_ERROR_SIZE_INSUFFICIENT;
                if (*count)
                    names[0] = *active->entry;
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Restore(XrSession, const XrSpatialAnchorFromPersistedAnchorCreateInfoMSFT *info,
                                                          XrSpatialAnchorMSFT *out) {
                if (!active->entry || std::string_view{active->entry->name} != info->spatialAnchorPersistenceName.name)
                    return XR_ERROR_SPATIAL_ANCHOR_NAME_NOT_FOUND_MSFT;
                *out = NativeHandle<XrSpatialAnchorMSFT>(21);
                return Script::Record("anchor-store-restore");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Remove(XrSpatialAnchorStoreConnectionMSFT, const XrSpatialAnchorPersistenceNameMSFT *) {
                const auto status = Script::Record("anchor-store-remove");
                if (status == XR_SUCCESS)
                    active->entry.reset();
                return status;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
                const std::array entries{std::pair{"xrCreateSpatialAnchorStoreConnectionMSFT", reinterpret_cast<PFN_xrVoidFunction>(&Open)},
                                         std::pair{"xrDestroySpatialAnchorStoreConnectionMSFT",
                                                   reinterpret_cast<PFN_xrVoidFunction>(&Close)},
                                         std::pair{"xrPersistSpatialAnchorMSFT", reinterpret_cast<PFN_xrVoidFunction>(&Persist)},
                                         std::pair{"xrEnumeratePersistedSpatialAnchorNamesMSFT",
                                                   reinterpret_cast<PFN_xrVoidFunction>(&Read)},
                                         std::pair{"xrCreateSpatialAnchorFromPersistedNameMSFT",
                                                   reinterpret_cast<PFN_xrVoidFunction>(&Restore)},
                                         std::pair{"xrUnpersistSpatialAnchorMSFT", reinterpret_cast<PFN_xrVoidFunction>(&Remove)}};
                const auto found = std::ranges::find_if(entries, [name](const auto &entry) {
                    return std::string_view{entry.first} == name;
                });
                if (found == entries.end())
                    return AnchorScript::Resolve(instance, name, out);
                *out = found->second;
                return XR_SUCCESS;
            }
        };
    }  // namespace

    TEST_CASE("OpenXR native anchors are grant scoped and retire before the session", "[unit][xr][native][anchors]") {
        Fixture fixture;
        AnchorResources resources;
        const auto lifecycle = std::make_shared<XRSessionLifecycle>(resources);
        const auto active =
            lifecycle->Activate(fixture.capabilities, fixture.capabilities.System(), fixture.capabilities.Revision(), fixture.plan);
        REQUIRE(active.HasValue());
        for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
            REQUIRE(lifecycle->ApplyEvent(active.Value(), event).HasValue());
        auto request = fixture.Request();
        request.candidate = active.Value();
        constexpr std::array advertised{std::string_view{XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME}};
        fixture.script.extraExtensions = advertised;
        fixture.script.extensionCount = 2;
        fixture.script.resolveExtra = AnchorScript::Resolve;
        const std::array extensions{NativeExtensionRequest{advertised.front()}};
        bool requested = true;
        SECTION("advertisement alone grants no native access") {
            requested = false;
        }
        SECTION("selected anchor route") {}
        if (requested)
            request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const XRAnchorScope scope{.space = {.id = {request.candidate, {0, 1}},
                                            .kind = XRSpaceKind::World,
                                            .worldOriginRevision = IdentityValue<XRWorldOriginRevision>(1)},
                                  .provider = IdentityValue<XRAnchorProviderGeneration>(1),
                                  .grant = IdentityValue<XRAnchorGrant>(1)};
        const auto world = NativeHandle<XrSpace>(11);
        const auto publication = std::make_shared<XRSpatialAnchors>(lifecycle);
        const auto start = fixture.owner.StartSpatialAnchors(scope, world, {1, Duration::FromNanoseconds(20)}, publication);
        if (!requested) {
            RequireFailureIdentity(start, XRErrors::OperationUnsupported);
            REQUIRE(fixture.owner.Close().HasValue());
            return;
        }
        REQUIRE(start.HasValue());
        const auto createdAt = XRRuntimeTime::Create(10).Value();
        fixture.script.fail = "anchor-space";
        REQUIRE(fixture.owner.CreateSpatialAnchor(scope, world, {}, createdAt).HasError());
        CHECK(fixture.script.calls.back() == "destroy-anchor");
        fixture.script.fail = {};
        const auto anchor = fixture.owner.CreateSpatialAnchor(scope, world, {}, createdAt);
        REQUIRE(anchor.HasValue());
        RequireFailureIdentity(fixture.owner.CreateSpatialAnchor(scope, world, {}, createdAt), XRErrors::CapacityExceeded);
        const auto observedAt = XRRuntimeTime::Create(20).Value();
        RequireFailureIdentity(fixture.owner.SampleSpatialAnchor(scope, NativeHandle<XrSpace>(99), anchor.Value(), observedAt),
                               XRErrors::IdentityStale);
        REQUIRE(fixture.owner.SampleSpatialAnchor(scope, world, anchor.Value(), observedAt).HasValue());
        const auto located = publication->Read(scope, anchor.Value(), observedAt);
        REQUIRE(located.HasValue());
        REQUIRE(located.Value().Observation().pose);
        REQUIRE(fixture.owner.RetireSpatialAnchor(scope, world, anchor.Value()).HasValue());
        RequireFailureIdentity(publication->Read(scope, anchor.Value(), observedAt), XRErrors::IdentityStale);
        const auto replacement = fixture.owner.CreateSpatialAnchor(scope, world, {}, createdAt);
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() != anchor.Value());
        REQUIRE(fixture.owner.SampleSpatialAnchor(scope, world, replacement.Value(), observedAt).HasValue());
        const auto revokedAt = XRRuntimeTime::Create(21).Value();
        fixture.script.fail = "locate-anchor";
        fixture.script.failureResult = XR_ERROR_PERMISSION_INSUFFICIENT;
        RequireFailureIdentity(fixture.owner.SampleSpatialAnchor(scope, world, replacement.Value(), revokedAt), XRErrors::PermissionDenied);
        RequireFailureIdentity(publication->Read(scope, replacement.Value(), revokedAt), XRErrors::PermissionDenied);
        fixture.script.fail = "destroy-anchor-space";
        fixture.script.failureResult = XR_ERROR_RUNTIME_FAILURE;
        RequireFailureIdentity(fixture.owner.Close(), XRErrors::OperationUnavailable);
        CHECK(fixture.script.liveSessions == 1);
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        const auto end = fixture.script.calls.end();
        REQUIRE(fixture.script.calls.size() >= 5);
        CHECK(*(end - 5) == "destroy-anchor-space");
        CHECK(*(end - 4) == "destroy-anchor");
        CHECK(*(end - 3) == "destroy-session");
    }

    TEST_CASE("OpenXR anchor store restores a fresh identity and revokes incomplete durable writes", "[unit][xr][native][anchors][save]") {
        Fixture fixture;
        AnchorStoreScript storage;
        AnchorResources resources;
        const auto lifecycle = std::make_shared<XRSessionLifecycle>(resources);
        const auto active =
            lifecycle->Activate(fixture.capabilities, fixture.capabilities.System(), fixture.capabilities.Revision(), fixture.plan);
        REQUIRE(active.HasValue());
        for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
            REQUIRE(lifecycle->ApplyEvent(active.Value(), event).HasValue());
        constexpr std::array<std::string_view, 2> advertised{XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME,
                                                             XR_MSFT_SPATIAL_ANCHOR_PERSISTENCE_EXTENSION_NAME};
        fixture.script.extraExtensions = advertised;
        fixture.script.extensionCount = 3;
        fixture.script.resolveExtra = &AnchorStoreScript::Resolve;
        const std::array dependencies{advertised.front()};
        const std::array extensions{NativeExtensionRequest{advertised.front()},
                                    NativeExtensionRequest{.name = advertised.back(), .dependencies = dependencies}};
        auto request = fixture.Request();
        request.candidate = active.Value();
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const XRAnchorScope scope{.space = {.id = {request.candidate, {0, 1}},
                                            .kind = XRSpaceKind::World,
                                            .worldOriginRevision = IdentityValue<XRWorldOriginRevision>(1)},
                                  .provider = IdentityValue<XRAnchorProviderGeneration>(1),
                                  .grant = IdentityValue<XRAnchorGrant>(1)};
        const auto world = NativeHandle<XrSpace>(11);
        const auto publication = std::make_shared<XRSpatialAnchors>(lifecycle);
        SECTION("focus interruption during provider preparation consumes its pending grant") {
            bool interrupt = true;
            fixture.fence.onValidate = [&fixture, &lifecycle, &interrupt] {
                if (std::exchange(interrupt, false))
                    InterruptAnchorFocus(fixture.owner, *lifecycle, fixture.script.lastSession);
            };
            const auto revoked = fixture.owner.StartSpatialAnchors(scope, world, {1, Duration::FromNanoseconds(30)}, publication);
            fixture.fence.onValidate = {};
            RequireFailureIdentity(revoked, XRErrors::PermissionDenied);
            RequireFailureIdentity(fixture.owner.StartSpatialAnchors(scope, world, {1, Duration::FromNanoseconds(30)}, publication),
                                   XRErrors::IdentityStale);
            REQUIRE(fixture.owner.Close().HasValue());
            return;
        }
        REQUIRE(fixture.owner.StartSpatialAnchors(scope, world, {1, Duration::FromNanoseconds(30)}, publication).HasValue());
        const auto createdAt = XRRuntimeTime::Create(10).Value();
        const auto observedAt = XRRuntimeTime::Create(20).Value();
        const auto anchor = fixture.owner.CreateSpatialAnchor(scope, world, {}, createdAt);
        REQUIRE(anchor.HasValue());
        RequireFailureIdentity(fixture.owner.PersistSpatialAnchor(scope, world, anchor.Value(), observedAt, "key-one"),
                               XRErrors::OperationUnavailable);
        CancellationSource consent;
        REQUIRE(fixture.owner.OpenSpatialAnchorStore(scope, world, consent).HasValue());
        RequireFailureIdentity(fixture.owner.PersistSpatialAnchor(scope, world, anchor.Value(), observedAt, "key-one"),
                               XRErrors::OperationUnavailable);
        REQUIRE(fixture.owner.SampleSpatialAnchor(scope, world, anchor.Value(), observedAt).HasValue());

        SECTION("one event batch cannot restore a withdrawn anchor or store grant") {
            InterruptAnchorFocus(fixture.owner, *lifecycle, fixture.script.lastSession);
            RequireFailureIdentity(publication->Read(scope, anchor.Value(), observedAt), XRErrors::IdentityStale);
            RequireFailureIdentity(fixture.owner.SampleSpatialAnchor(scope, world, anchor.Value(), XRRuntimeTime::Create(21).Value()),
                                   XRErrors::IdentityStale);
            RequireFailureIdentity(fixture.owner.CheckSpatialAnchorStore(scope, world), XRErrors::OperationUnavailable);
            REQUIRE(fixture.owner.StopSpatialAnchors(scope, XRAnchorWithdrawal::AccessRevoked).HasValue());
            RequireFailureIdentity(fixture.owner.StartSpatialAnchors(scope, world, {1, Duration::FromNanoseconds(30)}, publication),
                                   XRErrors::IdentityStale);
            REQUIRE(fixture.owner.Close().HasValue());
            return;
        }
        SECTION("existing records retain their stable name and restore without any old pose") {
            REQUIRE(fixture.owner.PersistSpatialAnchor(scope, world, anchor.Value(), observedAt, "key-one").HasValue());
            RequireFailureIdentity(fixture.owner.PersistSpatialAnchor(scope, world, anchor.Value(), observedAt, "key-one"),
                                   XRErrors::OperationInvalid);
            std::array<XrSpatialAnchorPersistenceNameMSFT, 1> names{};
            REQUIRE(fixture.owner.ReadSpatialAnchorStore(scope, world, names).Value() == 1);
            CHECK(std::string_view{names.front().name} == "key-one");
            REQUIRE(fixture.owner.RetireSession().HasValue());
            REQUIRE(fixture.owner.PrepareSession({fixture.preflight, fixture.preflight.Attempt()}).HasValue());
            const auto replacement =
                lifecycle->Activate(fixture.capabilities, fixture.capabilities.System(), fixture.capabilities.Revision(), fixture.plan);
            REQUIRE(replacement.HasValue());
            for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
                REQUIRE(lifecycle->ApplyEvent(replacement.Value(), event).HasValue());
            request.candidate = replacement.Value();
            REQUIRE(fixture.owner.Activate(request).HasValue());
            auto replacementScope = scope;
            replacementScope.space.id.session = request.candidate;
            replacementScope.grant = IdentityValue<XRAnchorGrant>(2);
            REQUIRE(fixture.owner.StartSpatialAnchors(replacementScope, world, {1, Duration::FromNanoseconds(30)}, publication).HasValue());
            REQUIRE(fixture.owner.OpenSpatialAnchorStore(replacementScope, world, consent).HasValue());
            const auto missing = fixture.owner.RestoreSpatialAnchor(replacementScope, world, observedAt, "absent");
            REQUIRE(missing.HasValue());
            CHECK_FALSE(missing.Value());
            const auto restored = fixture.owner.RestoreSpatialAnchor(replacementScope, world, observedAt, "key-one");
            REQUIRE(restored.HasValue());
            REQUIRE(restored.Value());
            CHECK(*restored.Value() != anchor.Value());
            const auto unlocalized = publication->Read(replacementScope, *restored.Value(), observedAt);
            REQUIRE(unlocalized.HasValue());
            CHECK(unlocalized.Value().Observation().state == XRAnchorLocalization::Locating);
            CHECK_FALSE(unlocalized.Value().Observation().pose);
            REQUIRE(fixture.owner.Close().HasValue());
            const auto close = std::ranges::find(fixture.script.calls, "anchor-store-close");
            const auto session = std::ranges::find(fixture.script.calls, "destroy-session");
            REQUIRE(session != fixture.script.calls.end());
            CHECK(close < session);
        }
        SECTION("revocation during write compensates the native record before returning") {
            storage.revokeOnPersist = &consent;
            RequireFailureIdentity(fixture.owner.PersistSpatialAnchor(scope, world, anchor.Value(), observedAt, "key-one"),
                                   XRErrors::PermissionDenied);
            CHECK_FALSE(storage.entry);
            CHECK(fixture.script.calls.back() == "anchor-store-remove");
            REQUIRE(fixture.owner.CloseSpatialAnchorStore(scope).HasValue());
            REQUIRE(publication->Read(scope, anchor.Value(), observedAt).HasValue());
            REQUIRE(fixture.owner.Close().HasValue());
        }
        SECTION("failed compensation fences the store until explicit retirement retries") {
            storage.revokeOnPersist = &consent;
            fixture.script.fail = "anchor-store-remove";
            RequireFailureIdentity(fixture.owner.PersistSpatialAnchor(scope, world, anchor.Value(), observedAt, "key-one"),
                                   XRErrors::OperationUnavailable);
            CHECK(storage.entry);
            std::array<XrSpatialAnchorPersistenceNameMSFT, 1> names{};
            RequireFailureIdentity(fixture.owner.ReadSpatialAnchorStore(scope, world, names), XRErrors::OperationUnavailable);
            RequireFailureIdentity(fixture.owner.CloseSpatialAnchorStore(scope), XRErrors::OperationUnavailable);
            fixture.script.fail = {};
            REQUIRE(fixture.owner.CloseSpatialAnchorStore(scope).HasValue());
            CHECK_FALSE(storage.entry);
            REQUIRE(fixture.owner.Close().HasValue());
        }
    }

    TEST_CASE("Canonical Save round trips an opaque reference backed by the native OpenXR store", "[unit][xr][native][anchors][save]") {
        using namespace Runtime;
        Fixture fixture;
        AnchorStoreScript storage;
        AnchorResources resources;
        const auto lifecycle = std::make_shared<XRSessionLifecycle>(resources);
        const auto active = AnchorValue(
            lifecycle->Activate(fixture.capabilities, fixture.capabilities.System(), fixture.capabilities.Revision(), fixture.plan));
        for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
            REQUIRE(lifecycle->ApplyEvent(active, event).HasValue());

        constexpr std::array<std::string_view, 2> advertised{XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME,
                                                             XR_MSFT_SPATIAL_ANCHOR_PERSISTENCE_EXTENSION_NAME};
        fixture.script.extraExtensions = advertised;
        fixture.script.extensionCount = 3;
        fixture.script.resolveExtra = &AnchorStoreScript::Resolve;
        const std::array dependencies{advertised.front()};
        const std::array extensions{NativeExtensionRequest{advertised.front()},
                                    NativeExtensionRequest{.name = advertised.back(), .dependencies = dependencies}};
        auto request = fixture.Request();
        request.candidate = active;
        request.extensions = extensions;
        const auto owner = std::make_shared<OpenXRNativeSession>(fixture.library, fixture.graphics, fixture.fence);
        REQUIRE(owner->Create(request).HasValue());
        const XRAnchorScope scope{.space = {.id = {active, {0, 1}},
                                            .kind = XRSpaceKind::World,
                                            .worldOriginRevision = IdentityValue<XRWorldOriginRevision>(1)},
                                  .provider = IdentityValue<XRAnchorProviderGeneration>(1),
                                  .grant = IdentityValue<XRAnchorGrant>(1)};
        const auto world = NativeHandle<XrSpace>(11);
        const auto publication = std::make_shared<XRSpatialAnchors>(lifecycle);
        REQUIRE(owner->StartSpatialAnchors(scope, world, {1, Duration::FromNanoseconds(30)}, publication).HasValue());
        const auto anchor = AnchorValue(owner->CreateSpatialAnchor(scope, world, {}, AnchorValue(XRRuntimeTime::Create(10))));
        const auto now = AnchorValue(XRRuntimeTime::Create(20));
        REQUIRE(owner->SampleSpatialAnchor(scope, world, anchor, now).HasValue());
        const XRAnchorStoreScope storeScope{OpenXRProtectedAnchorStore::Provider(), AnchorValue(XRAnchorStorePartition::FromBytes({2})), 1,
                                            7};
        const auto reference = AnchorValue(XRProtectedAnchorReference::FromBytes({3}));
        const auto clock = +[]() noexcept -> std::uint64_t {
            return 60;
        };
        CancellationSource consent;
        auto store = AnchorValue(OpenXRProtectedAnchorStore::Open(owner, scope, world, storeScope, consent, clock));
        REQUIRE(store->Record(anchor, now, {reference, 100}).HasValue());
        auto foreign = storeScope;
        foreign.partition = AnchorValue(XRAnchorStorePartition::FromBytes({9}));
        RequireFailureIdentity(store->Resolve(foreign, reference), XRErrors::PermissionDenied);

        const auto participant = AnchorValue(SaveParticipantId::Parse("horo.xr.anchor"));
        const auto record = AnchorValue(SaveRecordId::FromBytes({4}));
        auto source = AnchorValue(XRAnchorPersistence::Bind(participant, record, publication, scope, storeScope, store));
        REQUIRE(source->RecordLocalized(anchor, now).HasValue());
        CHECK_FALSE(AnchorValue(store->Relocalize(*source, now)));
        CanonicalStateParticipantRegistry captureBindings;
        REQUIRE(captureBindings.Register(source->Descriptor(), source).HasValue());
        const auto captureRegistry = AnchorValue(captureBindings.Snapshot());
        const RuntimeSaveCaptureProvenance provenance{AnchorValue(CapturedStateId::FromBytes({5})),
                                                      {10},
                                                      11,
                                                      12,
                                                      captureRegistry.Generation()};
        auto capture = AnchorValue(RuntimeSaveCaptureBuilder::Create(provenance, captureRegistry, {}));
        REQUIRE(capture.CaptureParticipants().HasValue());
        const auto saved = AnchorValue(capture.Seal());
        REQUIRE(saved.Records().size() == 1);
        const auto bytes = saved.Records().front().Segment(0);
        REQUIRE(bytes.size() == 72);
        REQUIRE(storage.entry);
        const std::string_view nativeName{storage.entry->name};
        CHECK(nativeName.starts_with("horo-a1-"));
        constexpr std::string_view nativePrefix = "horo-a1-";
        CHECK(std::ranges::search(bytes, std::as_bytes(std::span{nativePrefix})).empty());

        REQUIRE(owner->RetireSpatialAnchor(scope, world, anchor).HasValue());
        REQUIRE(owner->CloseSpatialAnchorStore(scope).HasValue());
        auto reopened = AnchorValue(OpenXRProtectedAnchorStore::Open(owner, scope, world, storeScope, consent, clock));
        RequireFailureIdentity(store->Admit(storeScope), XRErrors::IdentityStale);
        RequireFailureIdentity(reopened->Relocalize(*source, now), XRErrors::PermissionDenied);
        auto target = AnchorValue(XRAnchorPersistence::Bind(participant, record, publication, scope, storeScope, reopened));
        CanonicalStateParticipantRegistry restoreBindings;
        REQUIRE(restoreBindings.Register(target->Descriptor(), target).HasValue());
        const auto restoreRegistry = AnchorValue(restoreBindings.Snapshot());
        auto staged = AnchorValue(target->StageRestore(bytes));
        std::vector<std::unique_ptr<IStagedRestoreParticipant>> candidates;
        candidates.push_back(std::move(staged.participant));
        auto operation =
            AnchorValue(CreateSaveOperation({.operation = 121, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}));
        auto tx = AnchorValue(StagedRestoreTransaction::Create({121, restoreRegistry.Generation(), 19, 23, 1}, std::move(operation),
                                                               restoreRegistry, std::move(candidates)));
        REQUIRE(tx.Prepare().HasValue());
        CHECK(std::ranges::count(fixture.script.calls, "anchor-store-restore") == 0);
        CHECK_FALSE(AnchorValue(reopened->Relocalize(*target, now)));
        REQUIRE(tx.Activate({restoreRegistry.Generation(), 19, 23}, *staged.publication).HasValue());
        const auto durable = AnchorValue(target->Read());
        CHECK(durable.status == XRDurableAnchorStatus::Unlocalized);
        CHECK(durable.reference == reference);
        const auto restored = AnchorValue(reopened->Relocalize(*target, now));
        REQUIRE(restored);
        CHECK(*restored != anchor);
        const auto observation = AnchorValue(publication->Read(scope, *restored, now));
        CHECK(observation.Observation().state == XRAnchorLocalization::Locating);
        CHECK_FALSE(observation.Observation().pose);
        REQUIRE(owner->Close().HasValue());
    }

    TEST_CASE("OpenXR native transaction publishes only complete ownership and retires in reverse order", "[unit][xr][native]") {
        Fixture fixture;
        const auto request = fixture.Request();
        REQUIRE(fixture.owner.Create(request).HasValue());
        CHECK(fixture.owner.Session() == request.candidate);
        CHECK(fixture.owner.HasExtension("XR_KHR_vulkan_enable2"));
        REQUIRE(fixture.owner.Validate(request.candidate).HasValue());
        const auto borrowed = fixture.owner.Borrow(request.candidate);
        REQUIRE(borrowed.HasValue());
        CHECK(borrowed.Value().instance != XR_NULL_HANDLE);
        CHECK(borrowed.Value().session != XR_NULL_HANDLE);
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.calls.size() >= 3);
        const auto end = fixture.script.calls.end();
        CHECK(*(end - 3) == "destroy-session");
        CHECK(*(end - 2) == "release-graphics");
        CHECK(*(end - 1) == "destroy-instance");
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.owner.Borrow(request.candidate).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native failures at every acquisition stage roll back without publication", "[unit][xr][native]") {
        for (const auto phase : {"layers", "extensions", "instance", "system", "graphics", "session"}) {
            Fixture fixture;
            fixture.script.fail = phase;
            INFO(phase);
            CHECK(fixture.owner.Create(fixture.Request()).HasError());
            CHECK_FALSE(fixture.owner.Session().IsValid());
            CHECK(fixture.script.liveSessions == 0);
            CHECK(fixture.script.liveInstances == 0);
        }
    }

    TEST_CASE("OpenXR missing bootstrap or instance dispatch never strands native ownership", "[unit][xr][native]") {
        for (const auto symbol :
             {"xrGetInstanceProcAddr", "xrDestroyInstance", "xrCreateInstance", "xrDestroySession", "xrGetSystem", "xrCreateSession"}) {
            Fixture fixture;
            fixture.script.absent = symbol;
            INFO(symbol);
            CHECK(fixture.owner.Create(fixture.Request()).HasError());
            CHECK(fixture.script.liveInstances == 0);
            CHECK(fixture.script.liveSessions == 0);
        }
    }

    TEST_CASE("OpenXR stale device evidence is rejected before creation and after session acquisition", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("before any native work") {
            fixture.script.stale = true;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapabilityStale);
            CHECK(fixture.script.calls.empty());
        }
        SECTION("before publication") {
            fixture.script.staleAfterSession = true;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapabilityStale);
            CHECK(fixture.script.liveSessions == 0);
            CHECK(fixture.script.liveInstances == 0);
        }
        SECTION("retained owner") {
            REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
            fixture.script.stale = true;
            RequireFailureIdentity(fixture.owner.Validate(fixture.Request().candidate), XRErrors::CapabilityStale);
        }
    }

    TEST_CASE("OpenXR bounded negotiation rejects overflow malformed publication and unsupported requirements", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("native publication overflow") {
            fixture.script.extensionCount = MaximumNativeExtensions + 1;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapacityExceeded);
        }
        SECTION("corrupt native metadata") {
            fixture.script.malformedExtension = true;
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::OperationInvalid);
        }
        SECTION("required missing extension") {
            const std::array extensions{NativeExtensionRequest{"XR_TEST_missing", true}};
            auto request = fixture.Request();
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::OperationUnsupported);
        }
        SECTION("optional missing extension is observable disablement") {
            const std::array extensions{NativeExtensionRequest{"XR_TEST_missing", false}};
            auto request = fixture.Request();
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            CHECK_FALSE(fixture.owner.HasExtension("XR_TEST_missing"));
            REQUIRE(fixture.owner.Close().HasValue());
        }
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("An absent optional OpenXR extension preserves the usable baseline session", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view depth = "XR_KHR_composition_layer_depth";
        auto request = fixture.Request();
        const std::array extensions{NativeExtensionRequest{.name = depth}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const auto evidence = fixture.owner.ReadExtensionAdmission(request.candidate, depth);
        REQUIRE(evidence.HasValue());
        CHECK(evidence.Value().state == NativeExtensionState::Absent);
        CHECK(evidence.Value().discoveredVersion == 0);
        CHECK(evidence.Value().minimumVersion == 1);
        CHECK_FALSE(fixture.owner.HasExtension(depth));
        CHECK(fixture.owner.Borrow(request.candidate).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR extension evidence separates discovery version compatibility and missing dependencies", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view depth = "XR_KHR_composition_layer_depth";
        fixture.script.depthExtension = true;
        fixture.script.extensionCount = 2;
        auto request = fixture.Request();
        SECTION("discovered but unrequested") {
            REQUIRE(fixture.owner.Create(request).HasValue());
            const auto evidence = fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth);
            REQUIRE(evidence.HasValue());
            CHECK(evidence.Value().state == NativeExtensionState::Discovered);
            CHECK(evidence.Value().discoveredVersion == 1);
            CHECK(evidence.Value().minimumVersion == 0);
            CHECK_FALSE(fixture.owner.HasExtension(depth));
        }
        SECTION("optional request with a newer minimum version") {
            const std::array extensions{NativeExtensionRequest{.name = depth, .minimumVersion = 2}};
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            const auto evidence = fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth);
            REQUIRE(evidence.HasValue());
            CHECK(evidence.Value().state == NativeExtensionState::VersionIncompatible);
            CHECK(evidence.Value().discoveredVersion == 1);
            CHECK(evidence.Value().minimumVersion == 2);
            CHECK_FALSE(fixture.owner.HasExtension(depth));
        }
        SECTION("required request with a newer minimum version") {
            const std::array extensions{NativeExtensionRequest{.name = depth, .required = true, .minimumVersion = 2}};
            request.extensions = extensions;
            const auto failed = fixture.owner.Create(request);
            RequireFailureIdentity(failed, XRErrors::OperationIncompatible);
            CHECK(failed.ErrorValue().message.find(depth) != std::string::npos);
            CHECK(failed.ErrorValue().message.find("discovered version 1, required version 2") != std::string::npos);
            CHECK(fixture.script.liveInstances == 0);
        }
        SECTION("the declared core route suppresses a duplicate advertised extension") {
            const std::array extensions{
                NativeExtensionRequest{.name = depth, .required = true, .promotedToCore = request.preflight.LoaderApiVersion()}};
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            const auto evidence = fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth);
            REQUIRE(evidence.HasValue());
            CHECK(evidence.Value().state == NativeExtensionState::PromotedToCore);
            CHECK(evidence.Value().discoveredVersion == 1);
            CHECK(fixture.script.enabledExtensions.size() == 1);
        }
        SECTION("advertised feature depends on a missing request") {
            constexpr std::array dependencies{std::string_view{"XR_TEST_missing"}};
            const std::array extensions{NativeExtensionRequest{.name = depth, .dependencies = dependencies}};
            request.extensions = extensions;
            REQUIRE(fixture.owner.Create(request).HasValue());
            CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth).Value().state ==
                  NativeExtensionState::DependencyMissing);
            CHECK_FALSE(fixture.owner.HasExtension(depth));
        }
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, depth).HasError());
    }

    TEST_CASE("OpenXR core promotion admits a declared dependency without enabling its extension name", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view depth = "XR_KHR_composition_layer_depth";
        constexpr std::string_view promoted = "XR_TEST_promoted";
        constexpr std::array needsCore{promoted};
        fixture.script.depthExtension = true;
        fixture.script.extensionCount = 2;
        auto request = fixture.Request();
        const std::array extensions{NativeExtensionRequest{.name = depth, .required = true, .dependencies = needsCore},
                                    NativeExtensionRequest{.name = promoted,
                                                           .required = true,
                                                           .promotedToCore = request.preflight.LoaderApiVersion()}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const auto core = fixture.owner.ReadExtensionAdmission(request.activeAttempt, promoted);
        REQUIRE(core.HasValue());
        CHECK(core.Value().state == NativeExtensionState::PromotedToCore);
        CHECK(core.Value().discoveredVersion == 0);
        CHECK_FALSE(fixture.owner.HasExtension(promoted));
        CHECK(fixture.owner.HasExtension(depth));
        CHECK(fixture.owner.ReadExtensionAdmission(request.candidate, depth).Value().state == NativeExtensionState::Enabled);
        CHECK(std::ranges::find(fixture.script.enabledExtensions, promoted) == fixture.script.enabledExtensions.end());
        CHECK(std::ranges::find(fixture.script.enabledExtensions, depth) != fixture.script.enabledExtensions.end());
        REQUIRE(fixture.owner.RetireSession().HasValue());
        CHECK_FALSE(fixture.owner.HasExtension(depth));
        CHECK(fixture.owner.ReadExtensionAdmission(request.candidate, depth).HasError());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, promoted).Value().state == NativeExtensionState::PromotedToCore);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, promoted).HasError());
    }

    TEST_CASE("OpenXR feature providers bind the admitted core route and fence every publication generation", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view extension = "XR_TEST_core_provider";
        const NativeFeatureProviderRoute route{extension, "xrAbsentExtensionSymbol", "xrGetSystem"};
        auto request = fixture.Request();
        const std::array extensions{NativeExtensionRequest{.name = extension, .promotedToCore = request.preflight.LoaderApiVersion()}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const auto beforeRegistration = fixture.script.dispatchLookups;
        const auto first = fixture.owner.RegisterFeatureProvider(request.candidate, route);
        REQUIRE(first.HasValue());
        CHECK(fixture.script.dispatchLookups == beforeRegistration + 1);
        REQUIRE_FALSE(fixture.owner.RegisterFeatureProvider(request.candidate, route).HasValue());
        const auto borrowed = fixture.owner.BorrowFeatureProvider(first.Value());
        REQUIRE(borrowed.HasValue());
        CHECK(borrowed.Value().admission.state == NativeExtensionState::PromotedToCore);
        CHECK(borrowed.Value().admission.coreVersion == request.preflight.LoaderApiVersion());
        CHECK(borrowed.Value().function == reinterpret_cast<PFN_xrVoidFunction>(&Script::System));
        CHECK_FALSE(fixture.owner.HasExtension(extension));
        CHECK(fixture.owner.BorrowFeatureProvider(first.Value()).HasValue());
        CHECK(fixture.script.dispatchLookups == beforeRegistration + 1);

        fixture.owner.RevokeFeatureProvider(first.Value());
        CHECK(fixture.owner.BorrowFeatureProvider(first.Value()).HasError());
        const auto second = fixture.owner.RegisterFeatureProvider(request.candidate, route);
        REQUIRE(second.HasValue());
        fixture.owner.RevokeFeatureProvider(first.Value());
        CHECK(fixture.owner.BorrowFeatureProvider(first.Value()).HasError());
        CHECK(fixture.owner.BorrowFeatureProvider(second.Value()).HasValue());

        fixture.script.failRetirement = true;
        CHECK(fixture.owner.RetireSession().HasError());
        CHECK(fixture.owner.BorrowFeatureProvider(second.Value()).HasError());
        CHECK(fixture.owner.RegisterFeatureProvider(request.candidate, route).HasError());
        fixture.script.failRetirement = false;
        REQUIRE(fixture.owner.RetireSession().HasValue());
        REQUIRE(fixture.owner.PrepareSession({request.preflight, request.activeAttempt}).HasValue());
        ++request.candidate.slot.generation;
        REQUIRE(fixture.owner.Activate(request).HasValue());
        const auto replacement = fixture.owner.RegisterFeatureProvider(request.candidate, route);
        REQUIRE(replacement.HasValue());
        fixture.owner.RevokeFeatureProvider(second.Value());
        CHECK(fixture.owner.BorrowFeatureProvider(replacement.Value()).HasValue());
        CHECK(fixture.owner.BorrowFeatureProvider(second.Value()).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.BorrowFeatureProvider(replacement.Value()).HasError());
    }

    TEST_CASE("OpenXR provider registration rejects stale sessions and protects its native owners", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view extension = "XR_TEST_registration_fence";
        const NativeFeatureProviderRoute route{extension, {}, "xrGetSystem"};
        auto request = fixture.Request();
        const std::array extensions{NativeExtensionRequest{.name = extension, .promotedToCore = request.preflight.LoaderApiVersion()}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());

        auto stale = request.candidate;
        ++stale.slot.generation;
        RequireFailureIdentity(fixture.owner.RegisterFeatureProvider(stale, {"XR_TEST_unavailable", "xrAbsent"}), XRErrors::IdentityStale);

        std::size_t fenced{};
        fixture.fence.onValidate = [&fixture, &fenced] {
            ++fenced;
            RequireFailureIdentity(fixture.owner.RetireSession(), XRErrors::OperationUnavailable);
            RequireFailureIdentity(fixture.owner.Close(), XRErrors::OperationUnavailable);
            CHECK(fixture.script.liveInstances == 1);
            CHECK(fixture.script.liveSessions == 1);
        };
        const auto provider = fixture.owner.RegisterFeatureProvider(request.candidate, route);
        fixture.fence.onValidate = {};
        REQUIRE(provider.HasValue());
        CHECK(fenced == 2);
        CHECK(fixture.owner.BorrowFeatureProvider(provider.Value()).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.BorrowFeatureProvider(provider.Value()).HasError());
    }

    TEST_CASE("OpenXR provider receipts reject a backend recreated at the same address", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view extension = "XR_TEST_recreated_provider";
        const NativeFeatureProviderRoute route{extension, {}, "xrGetSystem"};
        auto request = fixture.Request();
        const std::array extensions{NativeExtensionRequest{.name = extension, .promotedToCore = request.preflight.LoaderApiVersion()}};
        request.extensions = extensions;

        std::optional<OpenXRNativeSession> backend;
        backend.emplace(fixture.library, fixture.graphics, fixture.fence);
        REQUIRE(backend->Create(request).HasValue());
        const auto first = backend->RegisterFeatureProvider(request.candidate, route);
        REQUIRE(first.HasValue());
        REQUIRE(backend->Close().HasValue());
        backend.reset();

        backend.emplace(fixture.library, fixture.graphics, fixture.fence);
        REQUIRE(backend->Create(request).HasValue());
        const auto replacement = backend->RegisterFeatureProvider(request.candidate, route);
        REQUIRE(replacement.HasValue());
        CHECK(backend->BorrowFeatureProvider(first.Value()).HasError());
        backend->RevokeFeatureProvider(first.Value());
        CHECK(backend->BorrowFeatureProvider(replacement.Value()).HasValue());
        REQUIRE(backend->Close().HasValue());
    }

    TEST_CASE("OpenXR extension evidence distinguishes host rejection and optional provider failure", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view depth = "XR_KHR_composition_layer_depth";
        fixture.script.depthExtension = true;
        fixture.script.extensionCount = 2;
        auto request = fixture.Request();
        auto eligibility = NativeExtensionEligibility::Eligible;
        auto expected = NativeExtensionState::ProviderFailed;
        SECTION("unsupported host leaves the discovered route disabled") {
            eligibility = NativeExtensionEligibility::Unsupported;
            expected = NativeExtensionState::Unsupported;
        }
        SECTION("host policy denial leaves the discovered route disabled") {
            eligibility = NativeExtensionEligibility::Denied;
            expected = NativeExtensionState::Denied;
        }
        SECTION("missing optional dispatch preserves the live baseline session") {}
        const std::array extensions{NativeExtensionRequest{.name = depth, .eligibility = eligibility}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        const auto failed = fixture.owner.RegisterFeatureProvider(request.candidate, {depth, "xrAbsentFeatureProvider"});
        REQUIRE(failed.HasError());
        const auto evidence = fixture.owner.ReadExtensionAdmission(request.candidate, depth);
        REQUIRE(evidence.HasValue());
        CHECK(evidence.Value().state == expected);
        CHECK(evidence.Value().discoveredVersion == 1);
        CHECK(evidence.Value().minimumVersion == 1);
        CHECK(fixture.owner.HasExtension(depth) == (eligibility == NativeExtensionEligibility::Eligible));
        CHECK(fixture.owner.Borrow(request.candidate).HasValue());
    }

    TEST_CASE("OpenXR cannot admit optional dependency cycles", "[unit][xr][native]") {
        Fixture fixture;
        constexpr std::string_view first = "XR_TEST_first";
        constexpr std::string_view second = "XR_TEST_second";
        constexpr std::array firstNeeds{second};
        constexpr std::array secondNeeds{first};
        auto request = fixture.Request();
        const auto core = request.preflight.LoaderApiVersion();
        const std::array extensions{NativeExtensionRequest{.name = first, .dependencies = firstNeeds, .promotedToCore = core},
                                    NativeExtensionRequest{.name = second, .dependencies = secondNeeds, .promotedToCore = core}};
        request.extensions = extensions;
        REQUIRE(fixture.owner.Create(request).HasValue());
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, first).Value().state == NativeExtensionState::DependencyMissing);
        CHECK(fixture.owner.ReadExtensionAdmission(request.activeAttempt, second).Value().state == NativeExtensionState::DependencyMissing);
        CHECK(fixture.script.enabledExtensions.size() == 1);
    }

    TEST_CASE("OpenXR invalid graphics binding and host exception roll back the instance", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("native type mismatch") {
            fixture.graphics.binding.type = XR_TYPE_UNKNOWN;
        }
        SECTION("unrelated chain") {
            fixture.graphics.binding.next = &fixture.graphics.binding;
        }
        SECTION("host exception") {
            fixture.script.throwAfterInstance = true;
        }
        SECTION("non-standard host exception") {
            fixture.script.throwUnknownAfterInstance = true;
        }
        CHECK(fixture.owner.Create(fixture.Request()).HasError());
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
    }

    TEST_CASE("OpenXR failed retirement remains explicit and retains the loader until a successful retry", "[unit][xr][native]") {
        bool unloaded{};
        Fixture fixture;
        fixture.library->destroyed = &unloaded;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        fixture.library.reset();
        fixture.script.failRetirement = true;
        REQUIRE(fixture.owner.Close().HasError());
        CHECK_FALSE(unloaded);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.script.liveSessions == 1);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.owner.Create(fixture.Request(2)).HasError());
        fixture.script.failRetirement = false;
        REQUIRE(fixture.owner.Close().HasValue());
        // The owner intentionally retains the verified library for safe future activation.
        CHECK_FALSE(unloaded);
    }

    TEST_CASE("OpenXR owner reuse rejects retired generations and admits only an explicit new candidate", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        CHECK(fixture.owner.Create(fixture.Request(2)).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
        RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::IdentityStale);
        REQUIRE(fixture.owner.Create(fixture.Request(2)).HasValue());
        RequireFailureIdentity(fixture.owner.Validate(fixture.Request().candidate), XRErrors::IdentityStale);
    }

    TEST_CASE("OpenXR replacement is separately owned and candidate failure cannot retire the active session", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        Graphics replacementGraphics;
        OpenXRNativeSession replacement{fixture.library, replacementGraphics, fixture.fence};
        fixture.script.fail = "session";
        CHECK(replacement.Create(fixture.Request(2)).HasError());
        CHECK(fixture.script.liveSessions == 1);
        CHECK(fixture.script.liveInstances == 1);
        REQUIRE(fixture.owner.Validate(fixture.Request().candidate).HasValue());
        fixture.script.fail = {};
        REQUIRE(replacement.Create(fixture.Request(3)).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveSessions == 1);
        REQUIRE(replacement.Validate(fixture.Request(3).candidate).HasValue());
    }

    TEST_CASE("OpenXR malformed policy and shipping API layers fail before object creation", "[unit][xr][native]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("invalid owner") {
            request.candidate = {};
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::IdentityInvalid);
        }
        SECTION("overcapacity requests") {
            const std::array<NativeExtensionRequest, MaximumEnabledExtensions> extensions{};
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::CapacityExceeded);
        }
        SECTION("embedded nul") {
            const std::array extensions{NativeExtensionRequest{std::string_view{"XR_BAD\0NAME", 11}, true}};
            request.extensions = extensions;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::OperationInvalid);
        }
        SECTION("shipping layers cannot be enabled by approval alone") {
            const std::array<std::string_view, 1> layers{"XR_APILAYER_TEST"};
            request.layers = layers;
            request.developmentLayersApproved = true;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::RuntimeOverrideRejected);
        }
        SECTION("preflight cannot cross product mode before native creation") {
            request.productMode = XRPreflightProductMode::Development;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::LoaderPreflightStale);
            CHECK(fixture.script.calls.empty());
        }
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
    }

    TEST_CASE("OpenXR corrupted instance retirement dispatch rolls back through the secured loader export", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.instanceDispatchAbsent = "xrDestroyInstance";
        REQUIRE(fixture.owner.Create(fixture.Request()).HasError());
        CHECK(std::ranges::find(fixture.script.calls, "instance") != fixture.script.calls.end());
        CHECK(std::ranges::find(fixture.script.calls, "destroy-instance") != fixture.script.calls.end());
        CHECK(fixture.script.liveInstances == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
    }

    TEST_CASE("OpenXR failed instance retirement retains only the remaining native owner for retry", "[unit][xr][native]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        fixture.script.fail = "destroy-instance";
        REQUIRE(fixture.owner.Close().HasError());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 1);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveInstances == 0);
        CHECK(std::ranges::count(fixture.script.calls, "destroy-session") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "release-graphics") == 1);
    }

    TEST_CASE("OpenXR explicit development layers contribute required extensions before instance activation", "[unit][xr][native]") {
        Fixture fixture;
        fixture.preflight = Preflight(XRPreflightProductMode::Development);
        fixture.script.advertiseLayer = true;
        const std::array<std::string_view, 1> layers{"XR_APILAYER_TEST"};
        const std::array extensions{NativeExtensionRequest{"XR_TEST_layer_extension", true}};
        auto request = fixture.Request();
        request.layers = layers;
        request.extensions = extensions;
        request.developmentLayersApproved = true;
        REQUIRE(fixture.owner.Create(request).HasValue());
        CHECK(fixture.script.enabledLayer);
        CHECK(fixture.owner.HasExtension("XR_TEST_layer_extension"));
    }

    TEST_CASE("OpenXR absent loader fails explicitly without native acquisition or fallback", "[unit][xr][native]") {
        Fixture fixture;
        OpenXRNativeSession absent{nullptr, fixture.graphics, fixture.fence};
        RequireFailureIdentity(absent.Create(fixture.Request()), XRErrors::LoaderAbsent);
        CHECK(fixture.script.calls.empty());
        CHECK_FALSE(absent.Session().IsValid());
    }

    TEST_CASE("OpenXR native system failure preserves category phase and numeric API context", "[unit][xr][native]") {
        Fixture fixture;
        fixture.script.fail = "system";
        fixture.script.failureResult = XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        const auto result = fixture.owner.Create(fixture.Request());
        RequireFailureIdentity(result, XRErrors::SystemUnsupported);
        CHECK(result.ErrorValue().message.find("xrGetSystem") != std::string::npos);
        CHECK(result.ErrorValue().message.find(std::to_string(XR_ERROR_FORM_FACTOR_UNSUPPORTED)) != std::string::npos);
        CHECK(fixture.script.liveInstances == 0);
        CHECK(fixture.script.liveSessions == 0);
    }

    TEST_CASE("OpenXR native result translation preserves every mapped category", "[unit][xr][native]") {
        struct Mapping {
            XrResult result;
            const ErrorCodeDescriptor *error;
        };

        const std::array mappings{Mapping{XR_ERROR_RUNTIME_UNAVAILABLE, &XRErrors::RuntimeUnavailable},
                                  Mapping{XR_ERROR_FORM_FACTOR_UNAVAILABLE, &XRErrors::SystemTemporarilyUnavailable},
                                  Mapping{XR_ERROR_FORM_FACTOR_UNSUPPORTED, &XRErrors::SystemUnsupported},
                                  Mapping{XR_ERROR_PERMISSION_INSUFFICIENT, &XRErrors::PermissionDenied},
                                  Mapping{XR_ERROR_EXTENSION_NOT_PRESENT, &XRErrors::OperationUnsupported},
                                  Mapping{XR_ERROR_API_LAYER_NOT_PRESENT, &XRErrors::OperationUnsupported},
                                  Mapping{XR_ERROR_RUNTIME_FAILURE, &XRErrors::OperationUnavailable}};
        for (const auto &mapping : mappings) {
            Fixture fixture;
            fixture.script.fail = "system";
            fixture.script.failureResult = mapping.result;
            const auto result = fixture.owner.Create(fixture.Request());
            RequireFailureIdentity(result, *mapping.error);
            CHECK(result.ErrorValue().message.find("xrGetSystem failed; native result=") == 0);
            CHECK(result.ErrorValue().message.find(std::to_string(mapping.result)) != std::string::npos);
            CHECK(fixture.script.liveInstances == 0);
            CHECK_FALSE(fixture.owner.Session().IsValid());
        }
    }

    TEST_CASE("OpenXR host exception preserves failed rollback ownership and its cause", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("standard exception") {
            fixture.script.throwAfterInstance = true;
        }
        SECTION("non-standard exception") {
            fixture.script.throwUnknownAfterInstance = true;
        }
        fixture.script.fail = "destroy-instance";
        const auto result = fixture.owner.Create(fixture.Request());
        RequireFailureIdentity(result, XRErrors::OperationUnavailable);
        REQUIRE(result.ErrorValue().cause.Get() != nullptr);
        CHECK(result.ErrorValue().cause.Get()->message.find("xrDestroyInstance failed; native result=") == 0);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.script.liveSessions == 0);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveInstances == 0);
    }

    TEST_CASE("OpenXR plan admission preserves stale system and capacity errors before native work", "[unit][xr][native]") {
        Fixture fixture;
        SECTION("replaced system") {
            auto request = fixture.Request();
            ++request.candidate.system.slot.generation;
            RequireFailureIdentity(fixture.owner.Create(request), XRErrors::IdentityStale);
        }
        SECTION("capacity no longer fits retained plan") {
            fixture.capabilities = Capabilities(4);
            RequireFailureIdentity(fixture.owner.Create(fixture.Request()), XRErrors::CapacityExceeded);
        }
        CHECK(fixture.script.calls.empty());
        CHECK(fixture.script.liveInstances == 0);
    }
}  // namespace Horo::Tests::OpenXR
