#pragma once

#include "Horo/Extensions/ExtensionRetirement.h"

#include <utility>

namespace Horo::Extensions::Detail {
    /** @brief Revokes a real host registry registration during module retirement. */
    template <typename Registration> class RetirementRegistration final : public IExtensionRetirementContribution {
    public:
        explicit RetirementRegistration(Registration registration) : registration_(std::move(registration)) {}

        void Revoke() noexcept override {
            (void)registration_.Reset();
        }

    private:
        Registration registration_;
    };
}  // namespace Horo::Extensions::Detail
