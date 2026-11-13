#include "Horo/Application/ProgressionUseCases.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/PlatformServices/PlatformProgressionReplay.h"

int main() {
    auto owner = Horo::PlatformServices::PlatformProgressionCoalescer::Create();
    return owner.HasValue() ? 0 : 1;
}
