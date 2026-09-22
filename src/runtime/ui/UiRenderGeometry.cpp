#include "Horo/Runtime/Ui/UiRenderGeometry.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        inline constexpr std::uint64_t BuildingLease = std::numeric_limits<std::uint64_t>::max();

        [[nodiscard]] constexpr bool IsKnown(const UiRenderGeometryPrimitive value) noexcept {
            return value >= UiRenderGeometryPrimitive::SolidRectangle && value <= UiRenderGeometryPrimitive::NineSliceRectangle;
        }

        [[nodiscard]] bool FitsRange(const std::uint32_t first, const std::uint32_t count, const std::size_t size) noexcept {
            return first <= size && count <= size - first;
        }

        [[nodiscard]] UiLinearColor WithOpacity(const UiLinearColor color, const float opacity) noexcept {
            UiLinearColor result = color;
            result.alpha *= opacity;
            return result;
        }

        struct FloatPoint final {
            float x{};
            float y{};
        };

        struct QuadDescriptor final {
            const UiLogicalTransform *transform{};
            FloatPoint origin;
            FloatPoint extent;
            std::array<float, 4> uv;
            UiLinearColor color;
        };

        [[nodiscard]] FloatPoint TransformPoint(const UiLogicalTransform &transform, const float x, const float y) noexcept {
            const auto &m = transform.values;
            return {m[0] * x + m[1] * y + m[4], m[2] * x + m[3] * y + m[5]};
        }

        [[nodiscard]] std::array<FloatPoint, 4> RectangleCorners(const float x, const float y, const float width,
                                                                 const float height) noexcept {
            return {FloatPoint{x, y}, FloatPoint{x + width, y}, FloatPoint{x + width, y + height}, FloatPoint{x, y + height}};
        }

        [[nodiscard]] Result<UiRenderGeometryBatchKey> MakeBatchKey(const UiRenderSnapshot &snapshot, const UiDrawCommand &command) {
            UiRenderGeometryBatchKey key{.resource = NoUiRenderIndex,
                                         .transform = command.transform,
                                         .clip = command.clip,
                                         .mask = command.mask};
            const auto result = std::visit([&key, &snapshot](const auto &draw) -> Result<void> {
                using Draw = std::decay_t<decltype(draw)>;
                if constexpr (std::is_same_v<Draw, UiSolidDraw>)
                    key.primitive = UiRenderGeometryPrimitive::SolidRectangle;
                else if constexpr (std::is_same_v<Draw, UiBorderDraw>)
                    key.primitive = UiRenderGeometryPrimitive::BorderRectangle;
                else if constexpr (std::is_same_v<Draw, UiImageDraw>) {
                    key.primitive = UiRenderGeometryPrimitive::ImageRectangle;
                    key.resource = draw.resource;
                } else if constexpr (std::is_same_v<Draw, UiSpriteDraw>) {
                    key.primitive = UiRenderGeometryPrimitive::SpriteRectangle;
                    key.resource = draw.resource;
                } else if constexpr (std::is_same_v<Draw, UiNineSliceDraw>) {
                    key.primitive = UiRenderGeometryPrimitive::NineSliceRectangle;
                    key.resource = draw.resource;
                } else if constexpr (std::is_same_v<Draw, UiTextDraw>) {
                    if (draw.run >= snapshot.TextRuns().size())
                        return Failure(UiErrors::RenderGeometryInvalid);
                    key.primitive = UiRenderGeometryPrimitive::TextGlyphs;
                    key.resource = snapshot.TextRuns()[draw.run].fontResource;
                }
                return Result<void>::Success();
            }, command.payload);
            return result.HasError() ? Result<UiRenderGeometryBatchKey>::Failure(result.ErrorValue())
                                     : Result<UiRenderGeometryBatchKey>::Success(key);
        }

        [[nodiscard]] Result<std::array<std::size_t, 2>> RequiredGeometry(const UiRenderSnapshot &snapshot, const UiDrawCommand &command) {
            return std::visit([&snapshot, &command](const auto &draw) -> Result<std::array<std::size_t, 2>> {
                using Draw = std::decay_t<decltype(draw)>;
                if constexpr (std::is_same_v<Draw, UiSolidDraw> || std::is_same_v<Draw, UiImageDraw> ||
                              std::is_same_v<Draw, UiSpriteDraw>) {
                    return Result<std::array<std::size_t, 2>>::Success({4, 6});
                } else if constexpr (std::is_same_v<Draw, UiNineSliceDraw>) {
                    return Result<std::array<std::size_t, 2>>::Success({36, 54});
                } else if constexpr (std::is_same_v<Draw, UiBorderDraw>) {
                    const bool visible = draw.width > 0 && command.rect.extent.width > 0 && command.rect.extent.height > 0;
                    return Result<std::array<std::size_t, 2>>::Success(visible ? std::array<std::size_t, 2>{16, 24}
                                                                               : std::array<std::size_t, 2>{0, 0});
                } else {
                    if (draw.run >= snapshot.TextRuns().size())
                        return Failure<std::array<std::size_t, 2>>(UiErrors::RenderGeometryInvalid);
                    const auto &run = snapshot.TextRuns()[draw.run];
                    if (run.firstGlyph > snapshot.Glyphs().size() || run.glyphCount > snapshot.Glyphs().size() - run.firstGlyph)
                        return Failure<std::array<std::size_t, 2>>(UiErrors::RenderGeometryInvalid);
                    return Result<std::array<std::size_t, 2>>::Success(
                        {static_cast<std::size_t>(run.glyphCount) * 4U, static_cast<std::size_t>(run.glyphCount) * 6U});
                }
            }, command.payload);
        }

        void AppendQuad(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const QuadDescriptor &quad) {
            const auto corners = RectangleCorners(quad.origin.x, quad.origin.y, quad.extent.x, quad.extent.y);
            const std::array<FloatPoint, 4> transformed{
                TransformPoint(*quad.transform, corners[0].x, corners[0].y),
                TransformPoint(*quad.transform, corners[1].x, corners[1].y),
                TransformPoint(*quad.transform, corners[2].x, corners[2].y),
                TransformPoint(*quad.transform, corners[3].x, corners[3].y),
            };
            const auto first = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back({transformed[0].x, transformed[0].y, quad.uv[0], quad.uv[1], quad.color});
            vertices.push_back({transformed[1].x, transformed[1].y, quad.uv[2], quad.uv[1], quad.color});
            vertices.push_back({transformed[2].x, transformed[2].y, quad.uv[2], quad.uv[3], quad.color});
            vertices.push_back({transformed[3].x, transformed[3].y, quad.uv[0], quad.uv[3], quad.color});
            indices.insert(indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
        }

        void AppendBorder(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const UiLogicalTransform &transform,
                          const UiLogicalRect rect, const std::int32_t width, const UiLinearColor color) {
            const float x = static_cast<float>(rect.origin.x);
            const float y = static_cast<float>(rect.origin.y);
            const float extentX = static_cast<float>(rect.extent.width);
            const float extentY = static_cast<float>(rect.extent.height);
            const float border = static_cast<float>(width);
            if (border <= 0.0F || extentX <= 0.0F || extentY <= 0.0F)
                return;

            const float horizontal = std::min(extentY * 0.5F, border);
            const float vertical = std::min(extentX * 0.5F, border);
            const float innerHeight = std::max(0.0F, extentY - 2.0F * horizontal);
            const std::array<float, 4> uv{0.0F, 0.0F, 0.0F, 0.0F};
            AppendQuad(vertices, indices, {&transform, {x, y}, {extentX, horizontal}, uv, color});
            AppendQuad(vertices, indices, {&transform, {x, y + extentY - horizontal}, {extentX, horizontal}, uv, color});
            AppendQuad(vertices, indices, {&transform, {x, y + horizontal}, {vertical, innerHeight}, uv, color});
            AppendQuad(vertices, indices, {&transform, {x + extentX - vertical, y + horizontal}, {vertical, innerHeight}, uv, color});
        }

        void AppendNineSlice(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices,
                             const UiLogicalTransform &transform, const UiLogicalRect rect, const UiNineSliceDraw &draw,
                             const UiLinearColor color) {
            const float x = static_cast<float>(rect.origin.x);
            const float y = static_cast<float>(rect.origin.y);
            const float width = static_cast<float>(rect.extent.width);
            const float height = static_cast<float>(rect.extent.height);
            const float sourceWidth = static_cast<float>(draw.sourceExtent.width);
            const float sourceHeight = static_cast<float>(draw.sourceExtent.height);
            const float uvWidth = draw.uv[2] - draw.uv[0];
            const float uvHeight = draw.uv[3] - draw.uv[1];
            const float leftFraction = static_cast<float>(draw.insets.left) / sourceWidth;
            const float rightFraction = static_cast<float>(draw.insets.right) / sourceWidth;
            const float topFraction = static_cast<float>(draw.insets.top) / sourceHeight;
            const float bottomFraction = static_cast<float>(draw.insets.bottom) / sourceHeight;
            const std::array<float, 4> xPositions{x, x + width * leftFraction, x + width * (1.0F - rightFraction), x + width};
            const std::array<float, 4> yPositions{y, y + height * topFraction, y + height * (1.0F - bottomFraction), y + height};
            const std::array<float, 4> uCoordinates{draw.uv[0], draw.uv[0] + uvWidth * leftFraction, draw.uv[2] - uvWidth * rightFraction,
                                                    draw.uv[2]};
            const std::array<float, 4> vCoordinates{draw.uv[1], draw.uv[1] + uvHeight * topFraction, draw.uv[3] - uvHeight * bottomFraction,
                                                    draw.uv[3]};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 3; ++column)
                    AppendQuad(vertices, indices,
                               {&transform,
                                {xPositions[column], yPositions[row]},
                                {xPositions[column + 1] - xPositions[column], yPositions[row + 1] - yPositions[row]},
                                {uCoordinates[column], vCoordinates[row], uCoordinates[column + 1], vCoordinates[row + 1]},
                                color});
        }

        [[nodiscard]] Result<void> ValidateGenerated(const std::span<const UiRenderVertex> vertices,
                                                     const std::span<const std::uint32_t> indices,
                                                     const std::span<const UiRenderGeometryBatch> batches, const std::size_t commandCount) {
            if (!std::ranges::all_of(vertices, &UiRenderVertex::IsValid))
                return Failure(UiErrors::RenderGeometryInvalid);
            for (const auto index : indices)
                if (index >= vertices.size())
                    return Failure(UiErrors::RenderGeometryInvalid);
            std::size_t expectedVertex = 0;
            std::size_t expectedIndex = 0;
            std::size_t expectedCommand = 0;
            for (const auto &batch : batches)
                if (!batch.IsValid(vertices.size(), indices.size(), commandCount) ||
                    std::array<std::size_t, 3>{batch.firstVertex, batch.firstIndex, batch.firstCommand} !=
                        std::array<std::size_t, 3>{expectedVertex, expectedIndex, expectedCommand})
                    return Failure(UiErrors::RenderGeometryInvalid);
                else {
                    expectedVertex += batch.vertexCount;
                    expectedIndex += batch.indexCount;
                    expectedCommand += batch.commandCount;
                }
            if (std::array<std::size_t, 3>{expectedVertex, expectedIndex, expectedCommand} !=
                std::array<std::size_t, 3>{vertices.size(), indices.size(), commandCount})
                return Failure(UiErrors::RenderGeometryInvalid);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiRenderVertex::IsValid */
    bool UiRenderVertex::IsValid() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(u) && std::isfinite(v) && u >= 0.0F && u <= 1.0F && v >= 0.0F &&
               v <= 1.0F && color.IsValid();
    }

    /** @copydoc UiRenderGeometryBatchKey::IsValid */
    bool UiRenderGeometryBatchKey::IsValid() const noexcept {
        if (!IsKnown(primitive))
            return false;
        const bool textured =
            primitive == UiRenderGeometryPrimitive::ImageRectangle || primitive == UiRenderGeometryPrimitive::SpriteRectangle ||
            primitive == UiRenderGeometryPrimitive::TextGlyphs || primitive == UiRenderGeometryPrimitive::NineSliceRectangle;
        return textured ? resource != NoUiRenderIndex : resource == NoUiRenderIndex;
    }

    /** @copydoc UiRenderGeometryBatch::IsValid */
    bool UiRenderGeometryBatch::IsValid(const std::size_t vertexCount, const std::size_t indexCount,
                                        const std::size_t commandCount) const noexcept {
        return key.IsValid() && commandCount > 0 && this->commandCount > 0 && FitsRange(firstVertex, this->vertexCount, vertexCount) &&
               FitsRange(firstIndex, this->indexCount, indexCount) && FitsRange(firstCommand, this->commandCount, commandCount);
    }

    /** @copydoc UiRenderGeometryLimits::IsValid */
    bool UiRenderGeometryLimits::IsValid() const noexcept {
        return vertices > 0 && vertices <= MaximumUiRenderGeometryVertices && indices > 0 && indices <= MaximumUiRenderGeometryIndices &&
               batches > 0 && batches <= MaximumUiRenderGeometryBatches;
    }

    /** @copydoc UiRenderGeometryArenaDescriptor::IsValid */
    bool UiRenderGeometryArenaDescriptor::IsValid() const noexcept {
        return view.IsValid() && limits.IsValid() && concurrentPlans > 0 && concurrentPlans <= MaximumUiRenderGeometryPlansInFlight;
    }

    /** @brief One preallocated immutable geometry slot guarded by an explicit lease count. */
    struct UiRenderGeometryPlan::Storage final {
        class PublishLease final {
        public:
            explicit PublishLease(Storage &storage) noexcept : storage_(&storage) {}

            PublishLease(const PublishLease &) = delete;
            PublishLease &operator=(const PublishLease &) = delete;

            ~PublishLease() {
                if (storage_ != nullptr)
                    storage_->leases.store(0, std::memory_order_release);
            }

            void Commit() noexcept {
                storage_->leases.store(1, std::memory_order_release);
                storage_ = nullptr;
            }

        private:
            Storage *storage_;
        };

        mutable std::atomic<std::uint64_t> leases{};
        UiRenderGeometryPlanDescriptor descriptor;
        std::optional<UiRenderSnapshot> source;
        std::vector<UiRenderVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<UiRenderGeometryBatch> batches;
        UiRenderGeometryLimits limits;

        explicit Storage(const UiRenderGeometryLimits sourceLimits) : limits(sourceLimits) {
            vertices.reserve(limits.vertices);
            indices.reserve(limits.indices);
            batches.reserve(limits.batches);
        }

        void Reset() noexcept {
            source.reset();
            vertices.clear();
            indices.clear();
            batches.clear();
            descriptor = {};
        }

        [[nodiscard]] Result<UiRenderGeometryBatch *> BeginBatch(const UiRenderGeometryBatchKey key, const std::uint32_t commandIndex) {
            if (!batches.empty() && batches.back().key == key &&
                static_cast<std::uint64_t>(batches.back().firstCommand) + batches.back().commandCount == commandIndex) {
                ++batches.back().commandCount;
                return Result<UiRenderGeometryBatch *>::Success(&batches.back());
            }
            if (batches.size() >= limits.batches)
                return Failure<UiRenderGeometryBatch *>(UiErrors::RenderGeometryCapacityExceeded);
            batches.push_back(
                {key, static_cast<std::uint32_t>(vertices.size()), 0, static_cast<std::uint32_t>(indices.size()), 0, commandIndex, 1});
            return Result<UiRenderGeometryBatch *>::Success(&batches.back());
        }

        [[nodiscard]] Result<void> AppendCommand(const UiRenderSnapshot &snapshot, const UiDrawCommand &command,
                                                 const UiLogicalTransform &transform) {
            const auto appendRectangle = [this, &command, &transform](const std::array<float, 4> &uv, const UiLinearColor color) {
                AppendQuad(vertices, indices,
                           {&transform,
                            {static_cast<float>(command.rect.origin.x), static_cast<float>(command.rect.origin.y)},
                            {static_cast<float>(command.rect.extent.width), static_cast<float>(command.rect.extent.height)},
                            uv,
                            WithOpacity(color, command.opacity)});
            };
            return std::visit([this, &snapshot, &command, &transform, &appendRectangle](const auto &draw) -> Result<void> {
                using Draw = std::decay_t<decltype(draw)>;
                if constexpr (std::is_same_v<Draw, UiSolidDraw>) {
                    appendRectangle({0.0F, 0.0F, 1.0F, 1.0F}, draw.color);
                } else if constexpr (std::is_same_v<Draw, UiBorderDraw>) {
                    AppendBorder(vertices, indices, transform, command.rect, draw.width, WithOpacity(draw.color, command.opacity));
                } else if constexpr (std::is_same_v<Draw, UiImageDraw>) {
                    appendRectangle({0.0F, 0.0F, 1.0F, 1.0F}, draw.tint);
                } else if constexpr (std::is_same_v<Draw, UiSpriteDraw>) {
                    appendRectangle(draw.uv, draw.tint);
                } else if constexpr (std::is_same_v<Draw, UiNineSliceDraw>) {
                    AppendNineSlice(vertices, indices, transform, command.rect, draw, WithOpacity(draw.tint, command.opacity));
                } else if constexpr (std::is_same_v<Draw, UiTextDraw>) {
                    if (draw.run >= snapshot.TextRuns().size())
                        return Failure(UiErrors::RenderGeometryInvalid);
                    const auto &run = snapshot.TextRuns()[draw.run];
                    if (run.firstGlyph > snapshot.Glyphs().size() || run.glyphCount > snapshot.Glyphs().size() - run.firstGlyph)
                        return Failure(UiErrors::RenderGeometryInvalid);
                    for (std::uint32_t glyphOffset = 0; glyphOffset < run.glyphCount; ++glyphOffset) {
                        const auto &glyph = snapshot.Glyphs()[run.firstGlyph + glyphOffset];
                        AppendQuad(vertices, indices,
                                   {&transform,
                                    {static_cast<float>(glyph.origin.x), static_cast<float>(glyph.origin.y)},
                                    {static_cast<float>(glyph.extent.width), static_cast<float>(glyph.extent.height)},
                                    glyph.uv,
                                    WithOpacity(run.color, command.opacity)});
                    }
                }
                return Result<void>::Success();
            }, command.payload);
        }

        [[nodiscard]] Result<void> Build(const UiRenderSnapshot &snapshot) {
            Reset();
            source.emplace(snapshot);
            const auto commands = snapshot.Commands();
            if (commands.size() > std::numeric_limits<std::uint32_t>::max())
                return Failure(UiErrors::RenderGeometryCapacityExceeded);

            for (std::uint32_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
                const auto &command = commands[commandIndex];
                const auto keyResult = MakeBatchKey(snapshot, command);
                if (keyResult.HasError())
                    return Result<void>::Failure(keyResult.ErrorValue());
                const auto required = RequiredGeometry(snapshot, command);
                if (required.HasError())
                    return Result<void>::Failure(required.ErrorValue());
                if (vertices.size() > limits.vertices || indices.size() > limits.indices ||
                    required.Value()[0] > limits.vertices - vertices.size() || required.Value()[1] > limits.indices - indices.size())
                    return Failure(UiErrors::RenderGeometryCapacityExceeded);
                const auto batchResult = BeginBatch(keyResult.Value(), commandIndex);
                if (batchResult.HasError())
                    return Result<void>::Failure(batchResult.ErrorValue());
                auto *batch = batchResult.Value();
                const auto firstVertex = vertices.size();
                const auto firstIndex = indices.size();
                const auto &transform = snapshot.Transforms()[command.transform];
                const auto appendResult = AppendCommand(snapshot, command, transform);
                if (appendResult.HasError())
                    return appendResult;
                batch->vertexCount += static_cast<std::uint32_t>(vertices.size() - firstVertex);
                batch->indexCount += static_cast<std::uint32_t>(indices.size() - firstIndex);
            }

            if (const auto validated = ValidateGenerated(vertices, indices, batches, commands.size()); validated.HasError())
                return validated;
            descriptor = {snapshot.Descriptor().view,
                          snapshot.Descriptor().snapshotRevision,
                          static_cast<std::uint32_t>(commands.size()),
                          static_cast<std::uint32_t>(vertices.size()),
                          static_cast<std::uint32_t>(indices.size()),
                          static_cast<std::uint32_t>(batches.size())};
            return Result<void>::Success();
        }
    };

    /** @brief Preallocated geometry slots for one exact view. */
    struct UiRenderGeometryArena::Storage final {
        UiRenderGeometryArenaDescriptor descriptor;
        UiRenderGeometryArenaState lifecycle{UiRenderGeometryArenaState::Active};
        std::vector<std::shared_ptr<UiRenderGeometryPlan::Storage>> slots;
        std::size_t nextSlot{};
        std::uint64_t failedBuilds{};
        std::uint32_t peakVertices{};
        std::uint32_t peakIndices{};
        std::uint32_t peakBatches{};

        explicit Storage(const UiRenderGeometryArenaDescriptor &source) : descriptor(source) {
            slots.reserve(source.concurrentPlans);
            for (std::uint32_t index = 0; index < source.concurrentPlans; ++index)
                slots.push_back(std::make_shared<UiRenderGeometryPlan::Storage>(source.limits));
        }

        [[nodiscard]] std::shared_ptr<UiRenderGeometryPlan::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                std::uint64_t expected{};
                if (slots[index]->leases.compare_exchange_strong(expected, BuildingLease, std::memory_order_acq_rel)) {
                    nextSlot = (index + 1) % slots.size();
                    return slots[index];
                }
            }
            return {};
        }

        [[nodiscard]] bool IsDrained() const noexcept {
            return std::ranges::all_of(slots, [](const auto &slot) {
                return slot->leases.load() == 0;
            });
        }
    };

    /** @copydoc UiRenderGeometryPlan::UiRenderGeometryPlan(std::shared_ptr<const Storage>) */
    UiRenderGeometryPlan::UiRenderGeometryPlan(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiRenderGeometryPlan::~UiRenderGeometryPlan */
    UiRenderGeometryPlan::~UiRenderGeometryPlan() {
        Release();
    }

    /** @copydoc UiRenderGeometryPlan::UiRenderGeometryPlan(const UiRenderGeometryPlan&) */
    UiRenderGeometryPlan::UiRenderGeometryPlan(const UiRenderGeometryPlan &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiRenderGeometryPlan::operator=(const UiRenderGeometryPlan&) */
    UiRenderGeometryPlan &UiRenderGeometryPlan::operator=(const UiRenderGeometryPlan &other) noexcept {
        if (this != &other) {
            UiRenderGeometryPlan replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiRenderGeometryPlan::UiRenderGeometryPlan(UiRenderGeometryPlan&&) */
    UiRenderGeometryPlan::UiRenderGeometryPlan(UiRenderGeometryPlan &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiRenderGeometryPlan::operator=(UiRenderGeometryPlan&&) */
    UiRenderGeometryPlan &UiRenderGeometryPlan::operator=(UiRenderGeometryPlan &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiRenderGeometryPlan::Retain */
    void UiRenderGeometryPlan::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load(std::memory_order_acquire);
        while (current > 0 && current < BuildingLease - 1) {
            if (storage_->leases.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiRenderGeometryPlan::Release */
    void UiRenderGeometryPlan::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1, std::memory_order_acq_rel);
        storage_.reset();
    }

    /** @copydoc UiRenderGeometryPlan::Descriptor */
    const UiRenderGeometryPlanDescriptor &UiRenderGeometryPlan::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiRenderGeometryPlan::SourceSnapshot */
    const UiRenderSnapshot &UiRenderGeometryPlan::SourceSnapshot() const noexcept {
        return storage_->source.value();
    }

    /** @copydoc UiRenderGeometryPlan::Vertices */
    std::span<const UiRenderVertex> UiRenderGeometryPlan::Vertices() const noexcept {
        return storage_->vertices;
    }

    /** @copydoc UiRenderGeometryPlan::Indices */
    std::span<const std::uint32_t> UiRenderGeometryPlan::Indices() const noexcept {
        return storage_->indices;
    }

    /** @copydoc UiRenderGeometryPlan::Batches */
    std::span<const UiRenderGeometryBatch> UiRenderGeometryPlan::Batches() const noexcept {
        return storage_->batches;
    }

    /** @copydoc UiRenderGeometryArena::Create */
    Result<UiRenderGeometryArena> UiRenderGeometryArena::Create(const UiRenderGeometryArenaDescriptor &descriptor) {
        if (!descriptor.view.IsValid())
            return Failure<UiRenderGeometryArena>(UiErrors::HandleMalformed);
        if (!descriptor.IsValid())
            return Failure<UiRenderGeometryArena>(UiErrors::RenderGeometryCapacityExceeded);
        try {
            return Result<UiRenderGeometryArena>::Success(UiRenderGeometryArena{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiRenderGeometryArena>(UiErrors::RenderGeometryCapacityExceeded);
        }
    }

    /** @copydoc UiRenderGeometryArena::UiRenderGeometryArena(std::unique_ptr<Storage>) */
    UiRenderGeometryArena::UiRenderGeometryArena(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiRenderGeometryArena::~UiRenderGeometryArena */
    UiRenderGeometryArena::~UiRenderGeometryArena() {
        Close();
    }

    /** @copydoc UiRenderGeometryArena::UiRenderGeometryArena(UiRenderGeometryArena&&) */
    UiRenderGeometryArena::UiRenderGeometryArena(UiRenderGeometryArena &&) noexcept = default;

    /** @copydoc UiRenderGeometryArena::operator=(UiRenderGeometryArena&&) */
    UiRenderGeometryArena &UiRenderGeometryArena::operator=(UiRenderGeometryArena &&) noexcept = default;

    /** @copydoc UiRenderGeometryArena::Build */
    Result<UiRenderGeometryPlan> UiRenderGeometryArena::Build(const UiRenderSnapshot &snapshot) {
        if (!storage_)
            return Failure<UiRenderGeometryPlan>(UiErrors::RenderGeometryLifecycleUnavailable);
        if (storage_->lifecycle != UiRenderGeometryArenaState::Active) {
            ++storage_->failedBuilds;
            return Failure<UiRenderGeometryPlan>(UiErrors::RenderGeometryLifecycleUnavailable);
        }
        if (!snapshot.IsValid()) {
            ++storage_->failedBuilds;
            return Failure<UiRenderGeometryPlan>(UiErrors::RenderGeometryInvalid);
        }
        if (snapshot.Descriptor().view != storage_->descriptor.view) {
            ++storage_->failedBuilds;
            return Failure<UiRenderGeometryPlan>(UiErrors::HandleOwnerMismatch);
        }
        auto slot = storage_->TryAcquire();
        if (!slot) {
            ++storage_->failedBuilds;
            return Failure<UiRenderGeometryPlan>(UiErrors::RenderGeometryStorageExhausted);
        }
        UiRenderGeometryPlan::Storage::PublishLease publishLease{*slot};
        const auto built = slot->Build(snapshot);
        if (built.HasError()) {
            ++storage_->failedBuilds;
            return Result<UiRenderGeometryPlan>::Failure(built.ErrorValue());
        }
        publishLease.Commit();
        storage_->peakVertices = std::max(storage_->peakVertices, static_cast<std::uint32_t>(slot->vertices.size()));
        storage_->peakIndices = std::max(storage_->peakIndices, static_cast<std::uint32_t>(slot->indices.size()));
        storage_->peakBatches = std::max(storage_->peakBatches, static_cast<std::uint32_t>(slot->batches.size()));
        return Result<UiRenderGeometryPlan>::Success(UiRenderGeometryPlan{std::move(slot)});
    }

    /** @copydoc UiRenderGeometryArena::Close */
    void UiRenderGeometryArena::Close() noexcept {
        if (storage_)
            storage_->lifecycle = UiRenderGeometryArenaState::Closed;
    }

    /** @copydoc UiRenderGeometryArena::IsDrained */
    bool UiRenderGeometryArena::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiRenderGeometryArena::State */
    UiRenderGeometryArenaState UiRenderGeometryArena::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiRenderGeometryArenaState::Closed;
    }

    /** @copydoc UiRenderGeometryArena::Statistics */
    UiRenderGeometryStatistics UiRenderGeometryArena::Statistics() const noexcept {
        UiRenderGeometryStatistics result{};
        if (!storage_)
            return result;
        result.capacityVertices = storage_->descriptor.limits.vertices;
        result.capacityIndices = storage_->descriptor.limits.indices;
        result.capacityBatches = storage_->descriptor.limits.batches;
        result.failedBuilds = storage_->failedBuilds;
        result.accepting = storage_->lifecycle == UiRenderGeometryArenaState::Active;
        result.peakVertices = storage_->peakVertices;
        result.peakIndices = storage_->peakIndices;
        result.peakBatches = storage_->peakBatches;
        std::uint64_t activeLeases{};
        for (const auto &slot : storage_->slots) {
            auto leases = slot->leases.load(std::memory_order_acquire);
            while (leases > 0 && leases < BuildingLease - 1) {
                if (!slot->leases.compare_exchange_weak(leases, leases + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                    continue;
                activeLeases += leases;
                result.usedVertices += static_cast<std::uint32_t>(slot->vertices.size());
                result.usedIndices += static_cast<std::uint32_t>(slot->indices.size());
                result.usedBatches += static_cast<std::uint32_t>(slot->batches.size());
                slot->leases.fetch_sub(1, std::memory_order_release);
                break;
            }
        }
        result.activeLeases = static_cast<std::uint32_t>(std::min<std::uint64_t>(activeLeases, std::numeric_limits<std::uint32_t>::max()));
        return result;
    }
}  // namespace Horo::Runtime::Ui
