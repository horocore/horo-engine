/** @file RenderMaterialBatchPublicContract.cpp @brief Compile-only public material batch portability and borrowed ownership contract. */
#include "Horo/Runtime/Render/MaterialBinding.h"

#include <array>
#include <span>
#include <type_traits>
#include <utility>

namespace Horo::Tests {
    using namespace Render;
    static_assert(std::is_aggregate_v<RenderMaterialPass>);
    static_assert(std::is_aggregate_v<RenderMaterialExecutionRequest>);
    static_assert(std::is_trivially_copyable_v<RenderMaterialBorrowedBatch<RenderMaterialPass>>);
    static_assert(std::is_trivially_copyable_v<RenderMaterialBorrowedBatch<RenderMaterialExecutionRequest>>);
    static_assert(std::is_same_v<decltype(RenderMaterialPass{}.precedingPasses), RenderMaterialBorrowedBatch<RenderMaterialPass>>);
    static_assert(std::is_same_v<decltype(RenderMaterialExecutionRequest{}.precedingPasses),
                                 RenderMaterialBorrowedBatch<RenderMaterialExecutionRequest>>);
    static_assert(!std::is_constructible_v<RenderMaterialBorrowedBatch<RenderMaterialPass>, const RenderMaterialPass *, std::size_t>);
    static_assert(!std::is_constructible_v<RenderMaterialBorrowedBatch<RenderMaterialPass>, std::span<const RenderMaterialDraw>>);
    static_assert(std::is_same_v<decltype(std::declval<RenderMaterialBorrowedBatch<RenderMaterialPass>>()[0]), const RenderMaterialPass &>);

    /** @brief Compile-checks ordinary aggregate/span/array interoperability after actual pass types are complete.
     * @details This source is linked into the public consumer; it creates no native objects and makes no runtime qualification claim.
     */
    void CheckRenderMaterialBatchPublicContract() {
        std::array<RenderMaterialPass, 1> prior{};
        RenderMaterialPass pass{.extent = {1, 1}, .precedingPasses = prior};
        pass.precedingPasses = std::span{prior};
        auto copy = pass;
        copy = pass;
        auto moved = std::move(copy);
        const std::span<const RenderMaterialPass> converted = moved.precedingPasses;
        for (const auto &preceding : moved.precedingPasses)
            static_cast<void>(preceding);

        std::array<RenderMaterialExecutionRequest, 1> requests{};
        RenderMaterialExecutionRequest request{.frame = {1}, .precedingPasses = requests};
        request.precedingPasses = std::span{requests};
        auto requestCopy = request;
        requestCopy = request;
        auto requestMoved = std::move(requestCopy);
        const std::span<const RenderMaterialExecutionRequest> convertedRequests = requestMoved.precedingPasses;
        static_cast<void>(converted);
        static_cast<void>(convertedRequests);
    }
}  // namespace Horo::Tests
