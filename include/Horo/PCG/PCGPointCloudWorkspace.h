#pragma once

/**
 * @file PCGPointCloudWorkspace.h
 * @brief Operation-owned, pre-admitted intermediate point columns and plan-ordered reuse.
 */

#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointSchema.h"

#include <memory>
#include <span>

namespace Horo::PCG {
    /** @brief Worst-case shape of one PointSet output pin in a cooked plan. */
    struct PCGPointOutputBound final {
        std::uint32_t node{};                         /**< Index in the cooked plan. */
        PinId pin{};                                  /**< Exact output pin identity. */
        std::shared_ptr<const PCGPointSchema> schema; /**< Immutable column vocabulary. */
        std::size_t maximumPoints{};                  /**< Inclusive produced-record ceiling. */
    };

    /** @brief Guarded writer for one bounded output; stale copies cannot mutate sealed or cancelled columns. */
    class PCGPointWriteView final {
    public:
        /** @brief Writes one complete core record into preallocated columns. @return Success or invalid/stale write failure. */
        [[nodiscard]] Result<void> SetPoint(std::size_t index, const Math::Transform &transform, const Math::Aabb &bounds, float density,
                                            std::uint64_t seed) const;

        /** @brief Writes one typed attribute value without exposing mutable column storage. @return Success or invalid/stale write failure.
         */
        template <typename Column>
        [[nodiscard]] Result<void> SetColumnValue(std::string_view key, std::size_t index, const typename Column::value_type &value) const {
            if (!*writable_ || index >= count_)
                return Result<void>::Failure(MakeError(PCGErrors::PointDataInvalid));
            for (auto &column : candidate_->attributes) {
                if (column.key == key) {
                    if (auto *values = std::get_if<Column>(&column.values)) {
                        (*values)[index] = value;
                        return Result<void>::Success();
                    }
                    break;
                }
            }
            return Result<void>::Failure(MakeError(PCGErrors::PointDataInvalid));
        }

    private:
        friend class PCGPointCloudWorkspace;

        PCGPointWriteView(PCGPointStorageCandidate *candidate, const bool *writable, std::size_t count)
            : candidate_(candidate), writable_(writable), count_(count) {}

        PCGPointStorageCandidate *candidate_;
        const bool *writable_;
        std::size_t count_;
    };

    /** @brief Borrowed immutable prefix of a sealed output; expires when the current node finishes. */
    class PCGPointReadView final {
    public:
        [[nodiscard]] std::size_t PointCount() const noexcept {
            return count_;
        }

        [[nodiscard]] std::span<const Math::Transform> Transforms() const noexcept;
        [[nodiscard]] std::span<const Math::Aabb> Bounds() const noexcept;
        [[nodiscard]] std::span<const float> Densities() const noexcept;
        [[nodiscard]] std::span<const std::uint64_t> Seeds() const noexcept;

        /** @brief Finds an immutable typed column prefix. @param key Canonical attribute key. @return Span or empty span for absent/wrong
         * type. */
        template <typename Column>
        [[nodiscard]] std::span<const typename Column::value_type> FindColumn(std::string_view key) const noexcept {
            for (const auto &column : candidate_->attributes) {
                if (column.key == key) {
                    if (const auto *values = std::get_if<Column>(&column.values))
                        return {values->data(), count_};
                    break;
                }
            }
            return {};
        }

    private:
        friend class PCGPointCloudWorkspace;

        PCGPointReadView(const PCGPointStorageCandidate *candidate, std::size_t count) : candidate_(candidate), count_(count) {}

        const PCGPointStorageCandidate *candidate_;
        std::size_t count_;
    };

    /** @brief Single-operation point workspace; one owner thread advances nodes in cooked order. */
    class PCGPointCloudWorkspace final {
    public:
        struct State;
        PCGPointCloudWorkspace(const PCGPointCloudWorkspace &) = delete;
        PCGPointCloudWorkspace &operator=(const PCGPointCloudWorkspace &) = delete;
        ~PCGPointCloudWorkspace();

        /**
         * @brief Proves peak live records and preallocates reusable column slots before evaluation.
         * @param plan Exact immutable plan used for routing and node order.
         * @param bounds One bound for every PointSet output pin; order does not matter.
         * @param maximumBytes Caller-reserved scratch slice, no larger than the plan tier's slice.
         * @param retainedBytes Old operation bytes still charged during replacement; zero for a new operation.
         * @return Workspace or typed invalid-envelope, overflow, or capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<PCGPointCloudWorkspace>> Create(const PCGCookedPlan &plan,
                                                                                    std::span<const PCGPointOutputBound> bounds,
                                                                                    std::size_t maximumBytes,
                                                                                    std::size_t retainedBytes = 0);

        /** @brief Exact charged workspace allocation, including column and routing metadata. */
        [[nodiscard]] std::size_t ReservedBytes() const noexcept;
        /** @brief Worst-case simultaneously materialized records proven at admission. */
        [[nodiscard]] std::size_t PeakRecords() const noexcept;
        /** @brief Current node index; equals node count after completion. */
        [[nodiscard]] std::uint32_t CurrentNode() const noexcept;

        /** @brief Opens one current-node output with a bounded mutable prefix. Call SealOutput before FinishNode. */
        [[nodiscard]] Result<PCGPointWriteView> BeginOutput(std::uint32_t node, PinId pin, std::size_t count);
        /** @brief Validates and seals one begun output; failed validation closes this operation. */
        [[nodiscard]] Result<void> SealOutput(std::uint32_t node, PinId pin);
        /** @brief Reads a sealed source only when an exact cooked route feeds the current node. */
        [[nodiscard]] Result<PCGPointReadView> ReadInput(std::uint32_t sourceNode, PinId sourcePin) const;
        /** @brief Completes the current node after all its PointSet outputs are sealed, retiring last-use inputs. */
        [[nodiscard]] Result<void> FinishNode(std::uint32_t node);
        /** @brief Reads an unconnected final output after all nodes have finished. */
        [[nodiscard]] Result<PCGPointReadView> ReadFinal(std::uint32_t node, PinId pin) const;
        /** @brief Cancels evaluation; all views become invalid and no further work is accepted. */
        void Cancel() noexcept;

    private:
        explicit PCGPointCloudWorkspace(std::unique_ptr<State> state) noexcept;

        std::unique_ptr<State> state_;
    };
}  // namespace Horo::PCG
