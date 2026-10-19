#include "Horo/Application/GameplayBuildService.h"

#if __has_include("Horo/Packages/GameplaySourceLibrary.h")
#error "Gameplay build consumers without package imports must not inherit package implementation headers"
#endif

int main() {
    const Horo::Application::GameplayBuildRequest request;
    return request.sourceLibraries ? 1 : 0;
}
