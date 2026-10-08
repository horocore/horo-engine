#include <Horo/XR/XRFakeRuntime.h>

int main() {
    Horo::XR::XRFakeRuntime runtime;
    runtime.Shutdown();
    return runtime.Snapshot().state == Horo::XR::XRSessionState::Destroyed ? 0 : 1;
}
