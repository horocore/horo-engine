#include "Horo/PCGScene/SceneOutputAdapter.h"

int main() {
    const Horo::SceneSource::SceneObjectSnapshot desired{.id = {1}, .name = "contract"};
    return Horo::PCG::FingerprintPCGSceneOutput(desired, 4096).HasValue() ? 0 : 1;
}
