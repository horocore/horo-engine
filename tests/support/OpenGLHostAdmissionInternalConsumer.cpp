#include "OpenGLSwapIntervalAdmission.h"

int main() {
    const auto query = [](int *interval) {
        *interval = 0;
        return true;
    };
    const auto unsupportedSet = [](int) {
        return false;
    };
    return Horo::Render::Detail::AdmitExactSwapInterval(0, query, unsupportedSet) == Horo::Render::Detail::SwapIntervalAdmission::Exact ? 0
                                                                                                                                        : 1;
}
