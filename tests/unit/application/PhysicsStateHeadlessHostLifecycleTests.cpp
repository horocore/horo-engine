#include "HeadlessNetworkServices.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateWorldTestSupport.h"

#include <exception>
#include <memory>
#include <optional>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        using Physics::StateTest::Check;
        using Physics::StateTest::Take;

        /** @brief Actual app factories own native resources; no mock provider or direct native handle injection. */
        struct ActualHeadlessPair final {
            NetworkNativeRetirementPoll poll;
            Network::NetworkModeFactories factories;
            std::unique_ptr<Network::INetworkModeService> scene;
            std::unique_ptr<Network::INetworkModeService> physics;
            Network::NetworkModeServiceRequest request{Network::NetworkModeServiceKind::Scene,
                                                       Network::NetworkProjectRole::Standalone,
                                                       {},
                                                       Network::NetworkModeWorldKind::Standalone,
                                                       {95}};

            ActualHeadlessPair() {
                Runtime::SceneDefinitionBuilder builder{{17}, {1}};
                auto definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(Take(std::move(builder).Build()));
                factories = ComposeHeadlessNetworkServices(std::move(definition), {}, {}, {}, &poll);
                REQUIRE(poll);
                scene = Take(factories.services[static_cast<std::size_t>(request.service)](request));
                request.service = Network::NetworkModeServiceKind::Physics;
                physics = Take(factories.services[static_cast<std::size_t>(request.service)](request));
                Check(scene->Prepare());
                Check(physics->Prepare());
                Check(scene->Activate());
                Check(physics->Activate());
            }

            void Shutdown() noexcept {
                if (physics)
                    physics->Shutdown();
                if (scene)
                    scene->Shutdown();
            }

            ~ActualHeadlessPair() {
                Shutdown();
                if (poll) {
                    auto retired = poll(64);
                    if (retired.HasError() || !retired.Value())
                        std::terminate();
                }
            }
        };

        TEST_CASE("Real headless Scene and native Physics services preserve their exact retirement callback through close",
                  "[physics][native][state][host][headless]") {
            ActualHeadlessPair pair;
            pair.Shutdown();
            std::optional<Error> foreignError;
            std::thread foreign([&] {
                auto rejected = pair.poll(64);
                if (rejected.HasError())
                    foreignError = rejected.ErrorValue();
            });
            foreign.join();
            REQUIRE(foreignError);
            CHECK(foreignError->code.Value() == Physics::PhysicsErrors::ThreadAffinityViolation.code.Value());
            CHECK(Take(pair.poll(64)));
            CHECK(Take(pair.poll(64)));
            pair.request.service = Network::NetworkModeServiceKind::Scene;
            const auto stale = pair.factories.services[static_cast<std::size_t>(pair.request.service)](pair.request);
            Physics::Test::RequireError(stale, Physics::PhysicsErrors::InvalidState);
        }
    }  // namespace
}  // namespace Horo::Application::Internal
