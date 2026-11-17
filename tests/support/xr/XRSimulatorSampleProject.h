#pragma once
#include <filesystem>

namespace Horo::Tests {
    /** @brief Explicit cold test-only materializer over the real project creator and canonical typed Scene serializer.
     * @param root New test/coordination destination; never an existing project. */
    void MaterializeXRSimulatorSample(const std::filesystem::path &root);
}  // namespace Horo::Tests
