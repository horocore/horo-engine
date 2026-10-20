#pragma once

/** @file RecordingImmutableFiles.h
 * @brief Deterministic path-free cache authority for delegation and failure regressions.
 */

#include "Horo/Foundation/ImmutableFileStorage.h"

#include <string>

namespace Horo::TestSupport {
    /** @brief Records synchronous calls; tests inspect state only after joining the owning service. */
    class RecordingImmutableFiles final : public ImmutableFileStorage {
    public:
        std::optional<Error> loadError;
        std::optional<Error> storeError;
        std::optional<std::vector<std::uint8_t>> response;
        mutable std::size_t loads{};
        mutable std::size_t stores{};
        mutable std::size_t bound{};
        mutable std::string loadedPath;
        mutable std::string storedPath;
        mutable std::vector<std::uint8_t> stored;

        Result<std::optional<std::vector<std::uint8_t>>> Load(std::string_view path, std::size_t maximumBytes,
                                                              const CancellationToken &) const override {
            ++loads;
            loadedPath = path;
            bound = maximumBytes;
            if (loadError)
                return Result<std::optional<std::vector<std::uint8_t>>>::Failure(*loadError);
            return Result<std::optional<std::vector<std::uint8_t>>>::Success(response);
        }

        Result<void> Store(std::string_view path, std::span<const std::uint8_t> bytes, const CancellationToken &) const override {
            ++stores;
            storedPath = path;
            stored.assign(bytes.begin(), bytes.end());
            return storeError ? Result<void>::Failure(*storeError) : Result<void>::Success();
        }
    };
}  // namespace Horo::TestSupport
