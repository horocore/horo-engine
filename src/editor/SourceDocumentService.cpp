#include "Horo/Foundation/PathUtils.h"
#include "SourceDocumentInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }
    }  // namespace

    namespace Detail {
        /** @brief Revalidates physical project containment and the exact registry key before each disk access. */
        Result<std::filesystem::path> SourcePath(const std::filesystem::path &projectRoot, const DocumentIdentity &identity,
                                                 const std::filesystem::path &path) {
            if (!identity.IsValid() || identity.key.kind != DocumentKind::Source || !path.is_absolute())
                return Failure<std::filesystem::path>(SourceDocumentErrors::Invalid);
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error || !Foundation::Paths::HasPathPrefix(projectRoot, canonical) ||
                canonical.lexically_relative(projectRoot).generic_string() != identity.key.source.Value())
                return Failure<std::filesystem::path>(SourceDocumentErrors::Invalid);
            return Result<std::filesystem::path>::Success(canonical);
        }

        /** @brief Advances only a successfully prepared observation; exhaustion rejects without wraparound. */
        Result<std::shared_ptr<SourceDocumentRoot>> NextRoot(const SourceDocumentRoot &previous) {
            if (previous.revision == std::numeric_limits<std::uint64_t>::max())
                return Failure<std::shared_ptr<Detail::SourceDocumentRoot>>(SourceDocumentErrors::Stale);
            auto root = std::make_shared<Detail::SourceDocumentRoot>(previous);
            ++root->revision;
            return Result<std::shared_ptr<Detail::SourceDocumentRoot>>::Success(std::move(root));
        }
    }  // namespace Detail

    /** @copydoc SourceDocumentService::SourceDocumentService */
    SourceDocumentService::SourceDocumentService(const std::filesystem::path &projectRoot, const SourceDocumentLimits limits)
        : storage_(std::make_unique<Storage>()) {
        storage_->limits = limits;
        std::error_code error;
        storage_->projectRoot = std::filesystem::canonical(projectRoot, error);
        const SourceDocumentLimits ceiling;
        if (error || !std::filesystem::is_directory(storage_->projectRoot, error) || error || limits.maximumDocumentBytes == 0 ||
            limits.maximumDocumentBytes > ceiling.maximumDocumentBytes || limits.maximumResidentBytes == 0 ||
            limits.maximumResidentBytes > ceiling.maximumResidentBytes || limits.maximumDocuments == 0 ||
            limits.maximumDocuments > ceiling.maximumDocuments)
            storage_->projectRoot.clear();
    }

    /** @copydoc SourceDocumentService::~SourceDocumentService */
    SourceDocumentService::~SourceDocumentService() = default;
    /** @copydoc SourceDocumentService::SourceDocumentService */
    SourceDocumentService::SourceDocumentService(SourceDocumentService &&) noexcept = default;
    /** @copydoc SourceDocumentService::operator= */
    SourceDocumentService &SourceDocumentService::operator=(SourceDocumentService &&) noexcept = default;

    /** @copydoc SourceDocumentService::Storage::Check */
    Result<void> SourceDocumentService::Storage::Check() const {
        if (owner != std::this_thread::get_id())
            return Failure<void>(SourceDocumentErrors::WrongThread);
        if (closed)
            return Failure<void>(SourceDocumentErrors::Closed);
        if (projectRoot.empty())
            return Failure<void>(SourceDocumentErrors::Invalid);
        return Result<void>::Success();
    }

    /** @copydoc SourceDocumentService::Storage::AdmitBytes */
    Result<void> SourceDocumentService::Storage::AdmitBytes(const std::size_t candidateBytes) const {
        std::size_t available = limits.maximumResidentBytes;
        if (candidateBytes > available)
            return Failure<void>(SourceDocumentErrors::TooLarge);
        available -= candidateBytes;
        for (const auto &record : records) {
            const auto &root = *record.root;
            const auto charge = root.text->bytes.size() + (root.text == root.base ? 0 : root.base->bytes.size());
            if (charge > available)
                return Failure<void>(SourceDocumentErrors::TooLarge);
            available -= charge;
        }
        return Result<void>::Success();
    }

    /** @copydoc SourceDocumentService::Storage::Find */
    SourceDocumentService::Storage::Record *SourceDocumentService::Storage::Find(const DocumentInstanceId instance) {
        const auto found = std::ranges::find_if(records, [instance](const auto &record) {
            return record.root->identity.instance == instance;
        });
        return found == records.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc SourceDocumentService::CheckAccess */
    Result<void> SourceDocumentService::CheckAccess() const {
        return storage_ ? storage_->Check() : Failure<void>(SourceDocumentErrors::Closed);
    }

    /** @copydoc SourceDocumentService::Snapshot */
    Result<SourceDocumentSnapshot> SourceDocumentService::Snapshot(const DocumentInstanceId instance) const {
        if (const auto valid = CheckAccess(); valid.HasError())
            return Result<SourceDocumentSnapshot>::Failure(valid.ErrorValue());
        const auto *record = storage_->Find(instance);
        if (!record)
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Stale);
        return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{record->root});
    }

    /** @copydoc SourceDocumentService::Open */
    Result<SourceDocumentSnapshot> SourceDocumentService::Open(const DocumentIdentity &identity, const std::filesystem::path &absolutePath,
                                                               const CancellationToken cancellation) {
        try {
            if (const auto valid = CheckAccess(); valid.HasError())
                return Result<SourceDocumentSnapshot>::Failure(valid.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
            auto root = storage_->OpenRoot(identity, absolutePath, cancellation);
            if (root.HasError())
                return Result<SourceDocumentSnapshot>::Failure(root.ErrorValue());
            return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{std::move(root).Value()});
        } catch (const std::bad_alloc &) {
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::Storage::AdmitNew */
    Result<void> SourceDocumentService::Storage::AdmitNew(const DocumentIdentity &identity) const {
        if (records.size() == limits.maximumDocuments || std::ranges::any_of(records, [&](const auto &record) {
            return record.root->identity.key == identity.key;
        }))
            return Failure<void>(SourceDocumentErrors::TooLarge);
        return AdmitBytes(limits.maximumDocumentBytes);
    }

    /** @copydoc SourceDocumentService::Storage::OpenRoot */
    Result<std::shared_ptr<const Detail::SourceDocumentRoot>> SourceDocumentService::Storage::OpenRoot(
        const DocumentIdentity &identity, const std::filesystem::path &absolutePath, const CancellationToken cancellation) {
        const auto path = Detail::SourcePath(projectRoot, identity, absolutePath);
        if (path.HasError())
            return Result<std::shared_ptr<const Detail::SourceDocumentRoot>>::Failure(path.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<std::shared_ptr<const Detail::SourceDocumentRoot>>(SourceDocumentErrors::Cancelled);
        if (const auto *existing = Find(identity.instance); existing) {
            if (existing->root->identity != identity || existing->path != path.Value())
                return Failure<std::shared_ptr<const Detail::SourceDocumentRoot>>(SourceDocumentErrors::Stale);
            return Result<std::shared_ptr<const Detail::SourceDocumentRoot>>::Success(existing->root);
        }
        if (const auto admitted = AdmitNew(identity); admitted.HasError())
            return Result<std::shared_ptr<const Detail::SourceDocumentRoot>>::Failure(admitted.ErrorValue());
        auto text = Detail::LoadSourceText(path.Value(), limits.maximumDocumentBytes, cancellation);
        if (text.HasError())
            return Result<std::shared_ptr<const Detail::SourceDocumentRoot>>::Failure(text.ErrorValue());
        auto root = std::make_shared<Detail::SourceDocumentRoot>();
        root->identity = identity;
        root->text = std::move(text).Value();
        root->base = root->text;
        if (cancellation.IsCancellationRequested())
            return Failure<std::shared_ptr<const Detail::SourceDocumentRoot>>(SourceDocumentErrors::Cancelled);
        records.push_back({path.Value(), root});
        return Result<std::shared_ptr<const Detail::SourceDocumentRoot>>::Success(std::move(root));
    }

    /** @copydoc SourceDocumentService::Close */
    Result<void> SourceDocumentService::Close(const DocumentInstanceId instance) {
        if (!storage_)
            return Failure<void>(SourceDocumentErrors::Closed);
        if (const auto valid = storage_->Check(); valid.HasError())
            return valid;
        const auto count = std::erase_if(storage_->records, [instance](const auto &record) {
            return record.root->identity.instance == instance;
        });
        return count == 1 ? Result<void>::Success() : Failure<void>(SourceDocumentErrors::Stale);
    }

    /** @copydoc SourceDocumentService::Shutdown */
    Result<void> SourceDocumentService::Shutdown() {
        if (!storage_)
            return Result<void>::Success();
        if (storage_->owner != std::this_thread::get_id())
            return Failure<void>(SourceDocumentErrors::WrongThread);
        storage_->closed = true;
        storage_->records.clear();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
