#pragma once

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace Horo::Assets::Internal {
    /** @brief Reads an exact byte count in bounded chunks with caller-owned error semantics.
     * @param input Open stream positioned at byte zero.
     * @param expectedBytes Size validated before allocation.
     * @param cancellation Cooperative cancellation token checked between bounded chunks.
     * @param cancellationError Error returned when cancellation is observed.
     * @param readError Error returned when the stream cannot provide the expected bytes.
     * @param readErrorMessage Optional context added to the read failure.
     * @return Owned payload or the selected typed failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadExactBytes(std::istream &input, std::size_t expectedBytes,
                                                                   const CancellationToken &cancellation,
                                                                   const ErrorCodeDescriptor &cancellationError,
                                                                   const ErrorCodeDescriptor &readError,
                                                                   std::string_view readErrorMessage = {});

    /** @brief Reads exactly the previously validated artifact size and rejects truncation or cancellation.
     * @param input Open cooked-artifact stream positioned at byte zero.
     * @param expectedBytes Size validated before allocation.
     * @param cancellation Cooperative cancellation token checked between bounded chunks.
     * @return Owned payload or a typed cancellation/truncation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadExactArtifact(std::istream &input, std::size_t expectedBytes,
                                                                      const CancellationToken &cancellation);
}  // namespace Horo::Assets::Internal
