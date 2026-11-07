#include "Horo/XR/XRInputProjection.h"
#include "XRSimulatorHostComposition.h"
#include "support/AllocationProbe.h"
#include "support/xr/XRSimulatorTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using namespace Horo::XR::SimulatorFeatureTests;

    struct Neutralizer final : IXRBindingNeutralizer {
        void Neutralize(const XRSessionId &, std::uint64_t) noexcept override {}
    };

    struct BoundHost final : IXRSessionResources {
        Input::InputRouter router;
        Input::RawInputCollector collector;
        Input::ActionDescriptor action;
        Input::InputContextToken context;
        Neutralizer neutralizer;
        std::unique_ptr<XRActionBindingCoordinator> candidate;
        std::unique_ptr<XRActionBindingCoordinator> previous;
        std::unique_ptr<XRSimulatorRuntime> runtime;
        bool throwQuery{};
        bool shutdownOnQuery{};
        bool reenterOnQuery{};
        mutable bool reentryDenied{};

        explicit BoundHost(std::string_view name = "gameplay.move", std::string_view scope = "editor.workspace")
            : action{Input::ActionId{std::string{name}}, Input::ActionValueType::Axis2D, Input::InputContextId{std::string{scope}}},
              context(router.PushContext(action.context, Input::InputContextKind::Gameplay)) {
            REQUIRE(router.SetActionMap({action}).HasValue());
            auto created = RuntimeInternal::XRSimulatorHostAccess::Create({*this, [this] {
                return Query();
            }});
            if (created.HasError())
                INFO(created.ErrorValue().message);
            REQUIRE(created.HasValue());
            runtime = std::move(created).Value();
        }

        const XRActionBindingCoordinator *Query() const {
            if (throwQuery)
                throw 1;
            if (runtime && reenterOnQuery)
                reentryDenied = runtime->Reset().HasError();
            if (runtime && shutdownOnQuery)
                runtime->Shutdown();
            return candidate.get();
        }

        Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &session, const XRFeaturePlan &) override {
            if (stage != XRSessionPreparation::InputAndUi)
                return Result<void>::Success();
            previous = std::move(candidate);
            candidate = std::make_unique<XRActionBindingCoordinator>(session, neutralizer);
            const std::array declarations{XRActionDeclaration{action.id,
                                                              action.context,
                                                              action.valueType,
                                                              {1},
                                                              XRTrackedDeviceRole::LeftController,
                                                              action.required}};
            const std::array suggestions{XRSuggestedActionBinding{action.id, {1}, {1}}};
            const std::array controls{XRProfileControl{{1}, {1}, XRTrackedDeviceRole::LeftController, action.valueType}};
            const std::array profiles{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, {1}}};
            return candidate->Update(session, {1, declarations, suggestions, {}, {1}, router.Actions()}, controls, profiles);
        }

        void Release(XRSessionPreparation stage, const XRSessionId &session) noexcept override {
            if (stage != XRSessionPreparation::InputAndUi)
                return;
            if (candidate && candidate->Session() == session) {
                candidate.reset();
                candidate = std::move(previous);
            } else if (previous && previous->Session() == session)
                previous.reset();
        }

        XRSimulatorBindingFingerprint Fingerprint() const {
            auto value = RuntimeInternal::XRSimulatorHostAccess::BindingFingerprint(*candidate->Current());
            REQUIRE(value.HasValue());
            return value.Value();
        }

        XRSimulatorStep Controls() const {
            auto controls = runtime->Controls();
            controls.actionCount = 1;
            controls.actions[0] = {0, XRTrackedDeviceRole::LeftController, XRTrackedActionKind::Vector2, true, 0.5F, -0.25F};
            return controls;
        }

        XRSimulatorScenario Artifact() const {
            const std::array steps{Controls()};
            auto value = XRSimulatorScenario::Create({.steps = steps, .bindings = Fingerprint()});
            REQUIRE(value.HasValue());
            return std::move(value).Value();
        }
    };

    TEST_CASE("Simulator resolves actual semantic bindings and enters canonical Input commit", "[xr][simulator][bindings]") {
        BoundHost host;
        REQUIRE(host.candidate->Session() == host.runtime->Snapshot().session.session);
        REQUIRE(host.runtime->SetControls(host.Controls()).HasValue());
        REQUIRE(host.runtime->Step(Cutoff(1)).HasValue());
        const auto *snapshot = host.runtime->Tracking();
        REQUIRE(snapshot != nullptr);
        auto portResult = host.router.ActionCollection(host.context);
        REQUIRE(portResult.HasValue());
        auto port = std::move(portResult).Value();
        auto created = XRInputProjection::Create(port, host.candidate->Session(), *host.candidate->Current(), host.candidate->Revision());
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        auto opened = port.Begin(host.collector.Commit());
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        REQUIRE(projection
                    .Append(*snapshot, snapshot->Session(), snapshot->Revision(), snapshot->WorldOriginRevision(),
                            host.candidate->Revision(), frame)
                    .HasValue());
        REQUIRE(host.router.ReadAction(host.context, host.action.id).x == 0);
        REQUIRE(frame.Commit().HasValue());
        REQUIRE(host.router.ReadAction(host.context, host.action.id).x == 0.5F);
    }

    TEST_CASE("Simulator persisted binding ordinals reject changed semantic schema and accept fresh genuine sessions",
              "[xr][simulator][bindings]") {
        BoundHost first;
        const auto artifact = first.Artifact();
        const auto fingerprint = first.Fingerprint();
        BoundHost same;
        REQUIRE(same.Fingerprint() == fingerprint);
        REQUIRE(same.runtime->LoadScenario(artifact).HasValue());
        REQUIRE(same.runtime->Replay().HasValue());
        REQUIRE(same.runtime->Tick(Cutoff(1)).Value().consumed);
        SECTION("action changed") {
            BoundHost other{"gameplay.look"};
            const auto prior = other.runtime->Snapshot().session.session;
            REQUIRE(other.Fingerprint() != fingerprint);
            REQUIRE(other.runtime->LoadScenario(artifact).HasError());
            REQUIRE(other.runtime->Snapshot().session.session == prior);
            REQUIRE(other.runtime->SetControls(other.Controls()).HasValue());
            REQUIRE(other.runtime->Step(Cutoff(1)).HasValue());
        }
        SECTION("context changed") {
            BoundHost other{"gameplay.move", "other.context"};
            REQUIRE(other.Fingerprint() != fingerprint);
            REQUIRE(other.runtime->LoadScenario(artifact).HasError());
        }
    }

    TEST_CASE("Simulator changed binding revision and throwing authority revoke stale tracking", "[xr][simulator][bindings]") {
        BoundHost host;
        REQUIRE(host.runtime->SetControls(host.Controls()).HasValue());
        REQUIRE(host.runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(host.runtime->Tracking() != nullptr);
        SECTION("shutdown coordinator") {
            host.candidate->Shutdown();
        }
        SECTION("throwing query") {
            host.throwQuery = true;
        }
        REQUIRE(host.runtime->Step(Cutoff(2)).HasError());
        REQUIRE(host.runtime->Tracking() == nullptr);
        REQUIRE(host.runtime->Snapshot().cursor == 1);
    }

    TEST_CASE("Simulator compatibility encoding separates semantic field boundaries", "[xr][simulator][bindings]") {
        BoundHost first{"ab", "c"};
        BoundHost second{"a", "bc"};
        const XRSimulatorBindingFingerprint golden{std::byte{0xca}, std::byte{0x17}, std::byte{0x8e}, std::byte{0x1c}, std::byte{0x67},
                                                   std::byte{0x66}, std::byte{0xb3}, std::byte{0x8c}, std::byte{0x0a}, std::byte{0xc6},
                                                   std::byte{0x42}, std::byte{0xe3}, std::byte{0x8e}, std::byte{0xb1}, std::byte{0x4a},
                                                   std::byte{0xf5}, std::byte{0xd0}, std::byte{0x39}, std::byte{0x24}, std::byte{0xb2},
                                                   std::byte{0xdc}, std::byte{0x15}, std::byte{0x0d}, std::byte{0xf9}, std::byte{0x5c},
                                                   std::byte{0xac}, std::byte{0x0c}, std::byte{0x7f}, std::byte{0xfb}, std::byte{0x35},
                                                   std::byte{0x01}, std::byte{0x0f}};
        REQUIRE(first.Fingerprint() == golden);
        REQUIRE(first.Fingerprint() != second.Fingerprint());
    }

    TEST_CASE("Simulator retained old-owner Input evidence cannot issue new-owner commands", "[xr][simulator][bindings][identity]") {
        BoundHost host;
        REQUIRE(host.runtime->SetControls(host.Controls()).HasValue());
        REQUIRE(host.runtime->Step(Cutoff(1)).HasValue());
        const auto retained = *host.runtime->Tracking();
        REQUIRE(host.runtime->Reset().HasValue());
        auto portResult = host.router.ActionCollection(host.context);
        REQUIRE(portResult.HasValue());
        auto port = std::move(portResult).Value();
        const auto session = host.runtime->Snapshot().session.session;
        auto created = XRInputProjection::Create(port, session, *host.candidate->Current(), host.candidate->Revision());
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        auto opened = port.Begin(host.collector.Commit());
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        REQUIRE(projection.Append(retained, session, retained.Revision(), retained.WorldOriginRevision(), host.candidate->Revision(), frame)
                    .HasError());
        REQUIRE(frame.Commit().HasValue());
        REQUIRE(host.router.ReadAction(host.context, host.action.id).x == 0);
    }

    TEST_CASE("Simulator prepared genuine binding sampling and recording allocate no C++ storage",
              "[xr][simulator][bindings][allocation]") {
        BoundHost host;
        REQUIRE(host.runtime->SetControls(host.Controls()).HasValue());
        REQUIRE(host.runtime->ArmCapture({.maximumSteps = 1}).HasValue());
        const auto cutoff = Cutoff(1);
        const auto before = Tests::AllocationProbe::Count();
        const auto advanced = host.runtime->Step(cutoff);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(advanced.HasValue());
        REQUIRE(advanced.Value().consumed);
        REQUIRE(after == before);
        REQUIRE(host.runtime->Tracking()->Actions().size() == 1);
    }

    TEST_CASE("Simulator trusted query contains reentry and deferred shutdown", "[xr][simulator][bindings][lifecycle]") {
        BoundHost host;
        host.reenterOnQuery = true;
        REQUIRE(host.runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(host.reentryDenied);
        host.reenterOnQuery = false;
        host.shutdownOnQuery = true;
        REQUIRE(host.runtime->Step(Cutoff(2)).HasError());
        REQUIRE(host.runtime->Tracking() == nullptr);
        REQUIRE(host.runtime->Snapshot().playback == XRSimulatorPlayback::Stopped);
    }
}  // namespace
