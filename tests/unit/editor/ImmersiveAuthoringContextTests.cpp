#include "../../../apps/HoroEditor/app/ImmersiveAuthoringContext.h"

#include <catch2/catch_test_macros.hpp>
#include <future>

namespace Horo::Editor {
    namespace {
        ImmersiveContextIdentity Opening() {
            const XR::XRSessionId session{{XR::XRRuntimeGeneration::Create(1).Value(), {0, 1}}, {0, 1}};
            return {1, 1, 1, session, {session, {0, 1}}, 1, 1, 1, XR::XRCapabilityRevision::Create(1).Value(),
                    ImmersiveAuthoringMode::Edit, 0, true, true};
        }

        TEST_CASE("Immersive planning captures a bounded owned selection", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            std::array<SceneSource::SceneObjectId, 2> selection{{{3}, {2}}};
            const auto first = owner.Publish(Opening(), selection);
            REQUIRE(first);
            const auto same = owner.Publish(Opening(), selection);
            REQUIRE(same);
            CHECK(same->Generation() == first->Generation());
            selection[0] = {4};
            CHECK(first->Selection()[0].value == 3);
            CHECK(owner.Admit(*first, ImmersiveRequestKind::Plan) == ImmersiveAdmission::Admitted);
            const auto next = owner.Publish(Opening(), selection);
            REQUIRE(next);
            CHECK(next->Generation() > first->Generation());
            CHECK_FALSE(first->IsCurrent());
            CHECK(owner.Admit(*first, ImmersiveRequestKind::Plan) == ImmersiveAdmission::StaleContext);
        }

        TEST_CASE("Immersive admission rejects another owner's context and changes of authority", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            ImmersiveAuthoringContext other;
            auto identity = Opening();
            const auto first = owner.Publish(identity, {});
            const auto foreign = other.Publish(identity, {});
            REQUIRE(first);
            REQUIRE(foreign);
            CHECK(owner.Admit(*foreign, ImmersiveRequestKind::Plan) == ImmersiveAdmission::StaleContext);
            SECTION("project replacement") { ++identity.projectOpening; }
            SECTION("document replacement") { ++identity.documentOpening; }
            SECTION("document edit") { ++identity.documentRevision; }
            SECTION("XR session replacement") {
                ++identity.xrSession.slot.generation;
                identity.referenceSpace.session = identity.xrSession;
            }
            SECTION("reference space replacement") { ++identity.referenceSpace.slot.generation; }
            SECTION("user change") { ++identity.user; }
            SECTION("conversation change") { ++identity.conversation; }
            SECTION("tool change") { ++identity.toolContext; }
            SECTION("capability change") { identity.capabilities = XR::XRCapabilityRevision::Create(2).Value(); }
            SECTION("mode transition") {
                identity.mode = ImmersiveAuthoringMode::Simulate;
                identity.worldGeneration = 2;
            }
            SECTION("permission withdrawal") { identity.planningAllowed = false; }
            REQUIRE(owner.Publish(identity, {}));
            CHECK_FALSE(first->IsCurrent());
            CHECK(owner.Admit(*first, ImmersiveRequestKind::Plan) == ImmersiveAdmission::StaleContext);
        }

        TEST_CASE("Immersive context rejects bad observations and fences Play planning", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            identity.mode = ImmersiveAuthoringMode::Play;
            identity.worldGeneration = 6;
            const auto play = owner.Publish(identity, {});
            REQUIRE(play);
            CHECK(owner.Admit(*play, ImmersiveRequestKind::Plan) == ImmersiveAdmission::ModeDenied);
            CHECK(owner.Admit(*play, ImmersiveRequestKind::Diagnostics) == ImmersiveAdmission::Admitted);
            SECTION("reference space belongs to a different session") {
                ++identity.referenceSpace.session.slot.generation;
                CHECK_FALSE(owner.Publish(identity, {}));
            }
            SECTION("selection too large") {
                std::array<SceneSource::SceneObjectId, ImmersivePlanningContext::MaximumSelection + 1> selection{};
                CHECK_FALSE(owner.Publish(identity, selection));
            }
            SECTION("duplicate selection") {
                const std::array<SceneSource::SceneObjectId, 2> selection{{{5}, {5}}};
                CHECK_FALSE(owner.Publish(identity, selection));
            }
            CHECK_FALSE(play->IsCurrent());
        }

        TEST_CASE("Immersive observation does not grant authority without host admission", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            identity.planningAllowed = false;
            identity.diagnosticsAllowed = false;
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);
            CHECK(owner.Admit(*context, ImmersiveRequestKind::Plan) == ImmersiveAdmission::PermissionDenied);
            CHECK(owner.Admit(*context, ImmersiveRequestKind::Diagnostics) == ImmersiveAdmission::PermissionDenied);
            CHECK(owner.Admit(*context, static_cast<ImmersiveRequestKind>(255)) == ImmersiveAdmission::InvalidRequest);
        }

        TEST_CASE("Immersive worker observes revocation after owner teardown", "[agent][immersive]") {
            auto owner = std::make_unique<ImmersiveAuthoringContext>();
            const auto context = owner->Publish(Opening(), {});
            REQUIRE(context);
            std::promise<void> teardown;
            auto worker = std::async(std::launch::async, [capture = *context, ready = teardown.get_future()] {
                ready.wait();
                return capture.IsCurrent();
            });
            owner.reset();
            teardown.set_value();
            CHECK_FALSE(worker.get());
        }
    }  // namespace
}  // namespace Horo::Editor
