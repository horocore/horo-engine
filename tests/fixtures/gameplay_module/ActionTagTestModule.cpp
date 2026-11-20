#include "ActionTagTestSchema.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/ReplicationRegistration.h"

#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;

    /** @brief Registers only the signed presentation field used by the two-process qualification. */
    class Module final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &context) override {
            const auto owner = ComponentTypeId::Parse("game.action_tag_test.presentation").Value();
            ComponentDescriptor descriptor{.typeId = owner, .schemaVersion = 1, .displayName = "Action Tag"};
            if (auto result = context.components.Register(std::move(descriptor)); result.HasError())
                return result;
            auto schema = Tests::ActionTagDeclaration().schema;
            const auto &field = schema.fields.front();
            auto scalar = Network::CanonicalScalarReplicationSerializer::Create(
                {field.valueType, field.codec, schema.owner, Network::ReplicationValueKind::SignedInteger, {}, 8, 1});
            if (scalar.HasError())
                return Result<void>::Failure(scalar.ErrorValue());
            return context.replication.Register({.owner = owner,
                                                 .schema = std::move(schema),
                                                 .serializers = {std::move(scalar).Value()},
                                                 .schedule = {.captureAccess = {.reads = {owner}}, .applyAccess = {.writes = {owner}}}});
        }

        Result<void> Start(GameRuntimeContext &) override {
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {}
    };
}  // namespace

extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    return std::make_unique<Module>().release();
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *module) noexcept {
    const std::unique_ptr<Horo::Gameplay::IGameModule> owned{module};
}
