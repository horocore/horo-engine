#include "Horo/Runtime/Render/UiPaintFrameBufferPool.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "UiPaintFrontendState.h"

#include <limits>
#include <new>
#include <optional>

namespace Horo::Render {
    /** @copydoc UiPaintFrameBudget::IsValid */
    bool UiPaintFrameBudget::IsValid() const noexcept {
        return vertices > 0 && vertices <= UiPaintLimits::MaximumVertices && indices > 0 && indices <= UiPaintLimits::MaximumIndices &&
               parameters > 0 && parameters <= UiPaintLimits::MaximumDraws;
    }

    /** @copydoc UiPaintFrameBufferPool::UiPaintFrameBufferPool */
    UiPaintFrameBufferPool::UiPaintFrameBufferPool(RenderFrontend &frontend, UiPaintFrameBudget budget, std::uint32_t slots,
                                                   ConstructionKey)
        : frontendState_(frontend.uiPaintFrontendState_), budget_(budget), count_(slots), owner_(std::this_thread::get_id()) {}

    UiPaintFrameBufferPool::~UiPaintFrameBufferPool() {
        static_cast<void>(Shutdown());
    }

    /** @copydoc UiPaintFrameBufferPool::Create */
    Result<std::unique_ptr<UiPaintFrameBufferPool>> UiPaintFrameBufferPool::Create(RenderFrontend &frontend, UiPaintFrameBudget budget,
                                                                                   std::uint32_t slots) {
        using Creation = Result<std::unique_ptr<UiPaintFrameBufferPool>>;
        if (const auto admission = frontend.RequireOpenOwner(); admission.HasError())
            return Creation::Failure(admission.ErrorValue());
        if (!budget.IsValid() || slots == 0 || slots > 8)
            return Creation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (!frontend.Capabilities().support.features.Supports(RenderCapability::UiPaint))
            return Creation::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        if (slots > frontend.Capabilities().support.limits.maxFramesInFlight)
            return Creation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        try {
            if (!frontend.uiPaintFrontendState_)
                frontend.uiPaintFrontendState_ = std::make_shared<Detail::UiPaintFrontendState>(&frontend);
            auto pool = std::make_unique<UiPaintFrameBufferPool>(frontend, budget, slots, ConstructionKey{});
            const std::array<std::size_t, 3> bytes{std::size_t{budget.vertices} * sizeof(UiPaintVertex),
                                                   std::size_t{budget.indices} * sizeof(std::uint32_t),
                                                   std::size_t{budget.parameters} * UiPaintFrameUpdate::ParameterStride};
            constexpr std::array usages{RenderBufferUsage::Vertex, RenderBufferUsage::Index, RenderBufferUsage::Uniform};
            for (std::size_t slot = 0; slot < slots; ++slot) {
                auto &buffers = pool->slots_[slot];
                const std::array destinations{&buffers.vertices, &buffers.indices, &buffers.parameters};
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    const auto created =
                        frontend.CreateBuffer({.byteSize = bytes[index], .usage = usages[index], .access = RenderBufferAccess::HostVisible},
                                              {});
                    if (created.HasError())
                        return Creation::Failure(created.ErrorValue());
                    *destinations[index] = created.Value().handle;
                    pool->operations_[slot][index] = created.Value().operation;
                }
            }
            return Creation::Success(std::move(pool));
        } catch (const std::bad_alloc &) {
            return Creation::Failure(MakeError(RenderGraphExecutionErrors::AllocationFailed));
        }
    }

    /** @copydoc UiPaintFrameBufferPool::Update */
    Result<UploadedUiPaintFrame> UiPaintFrameBufferPool::Update(std::uint32_t slot, std::span<const UiPaintVertex> vertices,
                                                                std::span<const std::uint32_t> indices,
                                                                std::span<const UiPaintParameters> parameters,
                                                                const CancellationToken &cancellation) {
        using Upload = Result<UploadedUiPaintFrame>;
        if (closed_ || std::this_thread::get_id() != owner_ || slot >= count_ || vertices.size() > budget_.vertices ||
            indices.size() > budget_.indices || parameters.size() > budget_.parameters ||
            revisions_[slot] == std::numeric_limits<std::uint64_t>::max())
            return Upload::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto state = frontendState_.lock();
        if (!state || !state->frontend)
            return Upload::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        const std::array handles{&slots_[slot].vertices, &slots_[slot].indices, &slots_[slot].parameters};
        std::optional<Error> failure;
        for (std::size_t index = 0; index < handles.size(); ++index)
            if (const auto ready = Detail::ObserveUiPaintCreation(*state->frontend, operations_[slot][index], *handles[index]);
                ready.HasError() && !failure)
                failure = ready.ErrorValue();
        if (failure)
            return Upload::Failure(std::move(*failure));
        const auto revision = revisions_[slot] + 1;
        const UiPaintFrameUpdate update{revision, vertices, indices, parameters, cancellation};
        if (const auto uploaded = state->frontend->UpdateUiPaintFrame(slots_[slot], update); uploaded.HasError())
            return Upload::Failure(uploaded.ErrorValue());
        revisions_[slot] = revision;
        return Upload::Success({slots_[slot], revision});
    }

    /** @copydoc UiPaintFrameBufferPool::Shutdown */
    Result<void> UiPaintFrameBufferPool::Shutdown() {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        closed_ = true;
        const auto state = frontendState_.lock();
        if (!state || !state->frontend) {
            slots_ = {};
            return Result<void>::Success();
        }
        for (std::size_t slot = 0; slot < count_; ++slot) {
            auto &buffers = slots_[slot];
            const std::array handles{&buffers.vertices, &buffers.indices, &buffers.parameters};
            for (std::size_t index = 0; index < handles.size(); ++index)
                if (handles[index]->IsValid())
                    static_cast<void>(Detail::ObserveUiPaintCreation(*state->frontend, operations_[slot][index], *handles[index]));
        }
        for (auto &buffers : slots_)
            for (auto *handle : {&buffers.vertices, &buffers.indices, &buffers.parameters}) {
                if (!handle->IsValid())
                    continue;
                if (const auto released = state->frontend->ReleaseBuffer(*handle); released.HasError())
                    return released;
                *handle = {};
            }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
