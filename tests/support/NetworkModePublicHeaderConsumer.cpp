#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkModeComposition.h"

int main() {
    return Horo::Network::NetworkProjectRole::Standalone == Horo::Network::NetworkProjectRole::Count ? 1 : 0;
}
