#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateHostState.h"

#include <algorithm>
#include <exception>
#include <type_traits>

namespace Horo::Application {
    static_assert(std::is_nothrow_move_constructible_v<UiTemplateHostCook>);
    static_assert(std::is_nothrow_copy_assignable_v<UiTemplateHostCook>);

    /** @copydoc UiTemplateHost::Matches */
    Result<void> UiTemplateHost::Matches(const UiTemplates::UiTemplateDocumentSnapshot &source,
                                         const Runtime::Ui::UiReloadGeneration &runtime, const UiTemplateHostCook &cook) {
        auto durable = UiTemplates::SerializeTemplateDocument(source);
        if (durable.HasError())
            return Result<void>::Failure(durable.ErrorValue());
        const auto &expected = cook.Document();
        if (ComputeSha256(std::as_bytes(std::span{durable.Value()})) != expected.sourceIdentity ||
            runtime.Instance().DocumentId() != expected.ordinary.Id() ||
            runtime.Instance().DocumentRevision() != expected.ordinary.SourceRevision() ||
            !std::ranges::equal(runtime.Instance().Payload(), expected.ordinary.Payload()))
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
        const bool rich = !expected.styles.empty() || !expected.localizedTexts.empty() || !expected.bindings.empty();
        if (rich && cook.state_->projections.size() != runtime.Canvases().size())
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RenderCompositionInvalid));
        for (const auto &projection : cook.state_->projections) {
            const auto canvas = std::ranges::find(runtime.Canvases(), projection.canvas, &Runtime::Ui::UiReloadCanvas::id);
            if (canvas == runtime.Canvases().end() || !canvas->layout)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
            const auto root = canvas->tree.Root();
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            if (projection.styles.Get(root.Value().handle).HasError() ||
                projection.styles.Descriptor().sources.tree != canvas->tree.Revision() ||
                projection.layout.interaction != canvas->layout->Descriptor().interaction ||
                projection.layout.sources != canvas->layout->Descriptor().sources)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
        }
        return Result<void>::Success();
    }

    UiTemplateRuntimeLease::UiTemplateRuntimeLease(Runtime::Ui::UiReloadLease runtime, UiTemplateHostCook cook) noexcept
        : cook_(std::move(cook)), runtime_(std::move(runtime)) {}

    /** @copydoc UiTemplateRuntimeLease::operator= */
    UiTemplateRuntimeLease &UiTemplateRuntimeLease::operator=(const UiTemplateRuntimeLease &other) noexcept {
        if (this != &other) {
            runtime_ = other.runtime_;
            cook_ = other.cook_;
        }
        return *this;
    }

    /** @copydoc UiTemplateRuntimeLease::operator= */
    UiTemplateRuntimeLease &UiTemplateRuntimeLease::operator=(UiTemplateRuntimeLease &&other) noexcept {
        if (this != &other) {
            runtime_ = std::move(other.runtime_);
            cook_ = std::move(other.cook_);
        }
        return *this;
    }

    /** @copydoc UiTemplateRuntimeLease::Get */
    const Runtime::Ui::UiReloadGeneration *UiTemplateRuntimeLease::Get() const noexcept {
        return runtime_.Get();
    }

    /** @copydoc UiTemplateRuntimeLease::CookedDocument */
    const UiTemplates::UiTemplateCookedDocument &UiTemplateRuntimeLease::CookedDocument() const noexcept {
        return cook_.Document();
    }

    /** @copydoc UiTemplateRuntimeLease::Projections */
    std::span<const UiTemplateCanvasProjection> UiTemplateRuntimeLease::Projections() const noexcept {
        return cook_.state_->projections;
    }

    UiTemplateHost::Prepared::Prepared(UiTemplates::UiTemplatePreparedTransaction document, Runtime::Ui::UiHotReload::Prepared runtime,
                                       UiTemplateHostCook cook, UiTemplateHostCook base) noexcept
        : document_(std::move(document)), cook_(std::move(cook)), base_(std::move(base)), runtime_(std::move(runtime)) {}

    /** @copydoc UiTemplateHost::Prepared::operator= */
    UiTemplateHost::Prepared &UiTemplateHost::Prepared::operator=(Prepared &&other) noexcept {
        if (this != &other) {
            runtime_ = std::move(other.runtime_);
            document_ = std::move(other.document_);
            cook_ = std::move(other.cook_);
            base_ = std::move(other.base_);
        }
        return *this;
    }

    UiTemplateHost::UiTemplateHost(std::unique_ptr<UiTemplates::UiTemplateDocumentOwner> document, Runtime::Ui::UiHotReload runtime,
                                   UiTemplateHostCook cook, std::vector<std::optional<UiTemplateHostCook>> retired) noexcept
        : document_(std::move(document)), current_(std::move(cook)), retired_(std::move(retired)), runtime_(std::move(runtime)) {}

    /** @copydoc UiTemplateHost::~UiTemplateHost */
    UiTemplateHost::~UiTemplateHost() {
        Shutdown();
        (void)CollectRetired();
    }

    /** @copydoc UiTemplateHost::Create */
    Result<std::unique_ptr<UiTemplateHost>> UiTemplateHost::Create(std::unique_ptr<UiTemplates::UiTemplateDocumentOwner> &&document,
                                                                   Runtime::Ui::UiReloadGeneration &&runtime, UiTemplateHostCook cook,
                                                                   const Runtime::Ui::UiHotReloadLimits limits) {
        using Output = Result<std::unique_ptr<UiTemplateHost>>;
        try {
            if (!document || !cook.state_)
                return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
            if (const auto thread = document->CheckOwnerThread(); thread.HasError())
                return Output::Failure(thread.ErrorValue());
            if (const auto valid = Matches(document->Capture(), runtime, cook); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            if (!limits.IsValid())
                return Output::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
            // All retirement storage is allocated before consuming the sole document owner.
            std::vector<std::optional<UiTemplateHostCook>> retired(limits.maximumRetiredGenerations);
            auto publisher = Runtime::Ui::UiHotReload::Create(std::move(runtime), limits);
            if (publisher.HasError())
                return Output::Failure(publisher.ErrorValue());
            return Output::Success(std::unique_ptr<UiTemplateHost>{
                new UiTemplateHost{std::move(document), std::move(publisher).Value(), std::move(cook), std::move(retired)}});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateHost::Prepare */
    Result<UiTemplateHost::Prepared> UiTemplateHost::Prepare(UiTemplates::UiTemplatePreparedTransaction document,
                                                             Runtime::Ui::UiReloadGeneration &&runtime, UiTemplateHostCook cook,
                                                             const CancellationToken &cancellation) {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<Prepared>::Failure(MakeError(UiTemplates::UiErrors::TemplateDocumentWrongThread));
        if (!cook.state_)
            return Result<Prepared>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
        if (const auto valid = document_->CanCommit(document); valid.HasError())
            return Result<Prepared>::Failure(valid.ErrorValue());
        if (const auto valid = Matches(document.Preview(), runtime, cook); valid.HasError())
            return Result<Prepared>::Failure(valid.ErrorValue());
        auto replacement = runtime_.Prepare(std::move(runtime), cancellation);
        if (replacement.HasError())
            return Result<Prepared>::Failure(replacement.ErrorValue());
        const auto *candidate = replacement.Value().Candidate();
        if (!candidate)
            return Result<Prepared>::Failure(MakeError(Runtime::Ui::UiErrors::InstanceStateInvalid));
        auto refreshed = RefreshSemantics(std::move(cook), *candidate);
        if (refreshed.HasError())
            return Result<Prepared>::Failure(refreshed.ErrorValue());
        // Candidate's bare borrow is not retained: only owned snapshots and the actual runtime reservation cross this boundary.
        return Result<Prepared>::Success(
            Prepared{std::move(document), std::move(replacement).Value(), std::move(refreshed).Value(), current_});
    }

    /** @copydoc UiTemplateHost::Commit */
    Result<Runtime::Ui::UiReloadReconciliation> UiTemplateHost::Commit(Prepared &prepared,
                                                                       const Runtime::Ui::UiStructuralCommitPoint point) {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<Runtime::Ui::UiReloadReconciliation>::Failure(MakeError(UiTemplates::UiErrors::TemplateDocumentWrongThread));
        if (const auto valid = document_->CanCommit(prepared.document_); valid.HasError())
            return Result<Runtime::Ui::UiReloadReconciliation>::Failure(valid.ErrorValue());
        if (const auto valid = runtime_.CanCommit(prepared.runtime_, point); valid.HasError())
            return Result<Runtime::Ui::UiReloadReconciliation>::Failure(valid.ErrorValue());
        const auto free = std::ranges::find_if(retired_, [](const auto &slot) {
            return !slot.has_value();
        });
        if (free == retired_.end())
            return Result<Runtime::Ui::UiReloadReconciliation>::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
        // This is the last fallible step and the pair's cancellation linearization point. Runtime Commit
        // validates before its allocation-free/nonthrowing swap and invokes no callback. On failure neither changed.
        auto committed = runtime_.Commit(prepared.runtime_, point);
        if (committed.HasError())
            return committed;
        // No second check, callback, allocation, final lease destruction or other owner mutation can split the pair.
        document_->CommitValidated(prepared.document_);
        free->emplace(std::move(current_));
        current_ = prepared.cook_;
        ++pendingRetirements_;
        return committed;
    }

    /** @copydoc UiTemplateHost::Capture */
    UiTemplates::UiTemplateDocumentSnapshot UiTemplateHost::Capture() const noexcept {
        if (ownerThread_ != std::this_thread::get_id())
            std::terminate();
        return document_->Capture();
    }

    /** @copydoc UiTemplateHost::Acquire */
    Result<UiTemplateRuntimeLease> UiTemplateHost::Acquire() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<UiTemplateRuntimeLease>::Failure(MakeError(UiTemplates::UiErrors::TemplateDocumentWrongThread));
        auto runtime = runtime_.Acquire();
        if (runtime.HasError())
            return Result<UiTemplateRuntimeLease>::Failure(runtime.ErrorValue());
        return Result<UiTemplateRuntimeLease>::Success(UiTemplateRuntimeLease{std::move(runtime).Value(), current_});
    }

    /** @copydoc UiTemplateHost::IsCurrent */
    Result<bool> UiTemplateHost::IsCurrent(const UiTemplateRuntimeLease &lease) const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(UiTemplates::UiErrors::TemplateDocumentWrongThread));
        return Result<bool>::Success(runtime_.IsCurrent(lease.runtime_) && current_.state_ == lease.cook_.state_);
    }

    /** @copydoc UiTemplateHost::Authoring */
    const UiTemplates::UiTemplateDocumentOwner &UiTemplateHost::Authoring() const noexcept {
        if (ownerThread_ != std::this_thread::get_id())
            std::terminate();
        return *document_;
    }

    /** @copydoc UiTemplateHost::CollectRetired */
    Result<std::size_t> UiTemplateHost::CollectRetired() {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<std::size_t>::Failure(MakeError(UiTemplates::UiErrors::TemplateDocumentWrongThread));
        // Evidence remains alive while the existing runtime owner drains abandonment callbacks.
        // External paired read leases and prepared candidates independently retain the same exact evidence.
        auto collected = runtime_.CollectRetired();
        if (collected.HasError())
            return collected;
        pendingRetirements_ -= std::min(pendingRetirements_, collected.Value());
        // UiHotReload deliberately exposes no retired-slot identity. Retain the bounded entire evidence batch
        // until every runtime retirement in it has drained, rather than guessing which package lease is reclaimable.
        if (pendingRetirements_ == 0)
            for (auto &slot : retired_)
                slot.reset();
        return collected;
    }

    /** @copydoc UiTemplateHost::Shutdown */
    void UiTemplateHost::Shutdown() noexcept {
        if (ownerThread_ != std::this_thread::get_id())
            std::terminate();
        document_->Shutdown();
        if (runtime_.Current())
            ++pendingRetirements_;
        runtime_.Shutdown();
    }
}  // namespace Horo::Application
