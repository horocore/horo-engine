#include "Horo/Gameplay/GameplayAccessibility.h"

int main() {
    Horo::ConfigurationSchema schema;
    for (const auto &descriptor : Horo::Gameplay::MakeGameplayAccessibilityContribution().settings)
        if (schema.Register(descriptor).HasError())
            return 1;
    if (schema.Seal().HasError())
        return 1;
    const auto snapshot = Horo::ConfigurationResolver::Resolve(schema, {}, 1);
    return snapshot.HasValue() && Horo::Gameplay::GameplayAssistTickSnapshot::Capture(snapshot.Value()).HasValue() ? 0 : 1;
}
