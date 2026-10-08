#include "Horo/Gameplay/LuaBehavior.h"

int main() {
    const auto owner = Horo::Gameplay::BehaviorTypeId::Parse("game.consumer.example");
    if (owner.HasError())
        return 1;
    const auto program = Horo::Gameplay::LuaBehaviorProgram::Compile("return horo.behavior {display_name='Consumer'}", owner.Value(),
                                                                     "consumer.horo_script");
    return program.HasValue() && program.Value()->ReplicationDeclaration() == nullptr && program.Value()->AcquireReplication().HasError()
               ? 0
               : 1;
}
