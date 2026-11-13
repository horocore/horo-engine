#include "SimulatorProjectArtifacts.h"

#include "Horo/Foundation/Platform.h"

#include <atomic>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        constexpr auto OperationDuration = std::chrono::seconds{30};

        /** @brief Reject invalid logical roots without inspecting or reopening native paths. */
        bool RootSpelling(const std::filesystem::path &root) {
            return root.is_absolute() && root == root.lexically_normal() &&
                   root.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
        }

        /** @brief Keep destination names disjoint from writer locks and private stages. */
        bool CaptureName(const std::string_view name) {
            return name.size() <= XR::XRSimulatorHardLimits::MaximumTextBytes && name.ends_with(".json") &&
                   name.find_first_of("/\\") == std::string_view::npos && Platform::IsSafeProjectReadPath(name);
        }

        /** @brief Validate the synthetic-only export channel before any filesystem mutation. */
        Result<void> CaptureBytes(const std::span<const std::byte> bytes) {
            const auto parsed = XR::ParseXRSimulatorScenario(bytes);
            if (parsed.HasError())
                return Result<void>::Failure(parsed.ErrorValue());
            if (parsed.Value().Provenance() != XR::XRSimulatorProvenance::RecordedSimulatorInteraction)
                return Result<void>::Failure(MakeError(XR::XRSimulatorErrors::ArtifactInvalid));
            return Result<void>::Success();
        }

        /** @brief Accept only the exact bounded source result requested through the retained project owner. */
        bool Matches(const Platform::ProjectReadTextSnapshot &snapshot, const Platform::ProjectReadContext &context,
                     const std::string_view path) {
            return snapshot.text && snapshot.text->size() <= XR::XRSimulatorHardLimits::MaximumEncodedBytes &&
                   snapshot.projectIdentity == context.projectIdentity && snapshot.projectGeneration == context.projectGeneration &&
                   snapshot.path == path;
        }

        /** @brief Clean only an invocation-created unpublished stage while its destination lease remains held. */
        struct PrivateStage final {
            DurableFileSystem &files;
            std::filesystem::path path;
            const AtomicFileReplacementReceipt &receipt;
            bool created{};

            PrivateStage(DurableFileSystem &owner, std::filesystem::path name, const AtomicFileReplacementReceipt &result)
                : files(owner), path(std::move(name)), receipt(result) {}

            PrivateStage(const PrivateStage &) = delete;
            PrivateStage &operator=(const PrivateStage &) = delete;

            ~PrivateStage() noexcept {
                if (!created || receipt.WasCommitted())
                    return;
                try {
                    static_cast<void>(files.RemoveDurable(path));
                } catch (...) {
                    // Preserve the publication failure and leave our private orphan for explicit recovery.
                }
            }
        };
    }  // namespace

    struct SimulatorProjectArtifacts::Impl final {
        SimulatorProjectArtifactsComposition composition;
        const std::thread::id lane{std::this_thread::get_id()};
        std::atomic<bool> revoked{}; /**< Terminal any-thread stop; other state belongs exclusively to lane. */
        bool busy{};

        explicit Impl(SimulatorProjectArtifactsComposition value) : composition(std::move(value)) {}

        /** @brief Admit one cold owner-lane operation, without allowing callback reentry. */
        Result<void> Admit() const {
            if (std::this_thread::get_id() != lane || busy)
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            if (revoked.load(std::memory_order_acquire))
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief Establish a fresh bounded deadline for this explicit I/O operation. */
        Platform::ProjectReadContext Context() const {
            auto context = composition.context;
            context.deadline = std::chrono::steady_clock::now() + OperationDuration;
            return context;
        }

        /** @brief Preserve original cancellation/authority/deadline errors and observe reentrant revocation. */
        Result<void> Check(const Platform::ProjectReadContext &context) const {
            if (revoked.load(std::memory_order_acquire))
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Stale));
            if (const auto checked = Platform::CheckProjectReadContext(context); checked.HasError())
                return checked;
            if (revoked.load(std::memory_order_acquire))
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief Hold callback reentry closed until every borrowed context and owned stage is released. */
        struct Operation final {
            Impl &owner;

            explicit Operation(Impl &value) : owner(value) {
                owner.busy = true;
            }

            Operation(const Operation &) = delete;
            Operation &operator=(const Operation &) = delete;

            ~Operation() {
                owner.busy = false;
            }
        };

        /** @brief Publish complete validated bytes using the supplied root-bound writer authority. */
        Result<void> Publish(const std::filesystem::path &destination, const std::span<const std::byte> bytes,
                             const Platform::ProjectReadContext &context, AtomicFileReplacementReceipt &receipt) {
            auto lockPath = destination;
            lockPath += ".lock";
            auto acquired = composition.publication->TryAcquireExclusive(lockPath, "horo.xr-simulator.capture");
            if (acquired.HasError())
                return Result<void>::Failure(acquired.ErrorValue());
            const auto writer = std::move(acquired).Value();
            if (!writer)
                return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Unavailable));
            auto prepared = destination;
            prepared += ".pending";
            PrivateStage stage{*composition.publication, std::move(prepared), receipt};
            if (const auto current = Check(context); current.HasError())
                return current;
            if (const auto written = composition.publication->WritePrivateDurable(stage.path, bytes, stage.created); written.HasError())
                return written;
            if (const auto current = Check(context); current.HasError())
                return current;
            return composition.publication->AtomicReplaceTracked(stage.path, destination, receipt);
        }
    };

    SimulatorProjectArtifacts::SimulatorProjectArtifacts(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    SimulatorProjectArtifacts::~SimulatorProjectArtifacts() = default;

    /** @copydoc SimulatorProjectArtifacts::Create */
    Result<std::shared_ptr<SimulatorProjectArtifacts>> SimulatorProjectArtifacts::Create(SimulatorProjectArtifactsComposition composition) {
        using Output = Result<std::shared_ptr<SimulatorProjectArtifacts>>;
        if (!composition.source || !composition.publication || !RootSpelling(composition.authorizedRoot) ||
            composition.context.projectIdentity.empty() || composition.context.projectIdentity.size() > 256 ||
            composition.context.projectGeneration == 0)
            return Output::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
        auto impl = std::make_unique<Impl>(std::move(composition));
        if (const auto current = impl->Check(impl->Context()); current.HasError())
            return Output::Failure(current.ErrorValue());
        return Output::Success(std::shared_ptr<SimulatorProjectArtifacts>{new SimulatorProjectArtifacts(std::move(impl))});
    }

    /** @copydoc SimulatorProjectArtifacts::ReadScenario */
    Result<std::vector<std::byte>> SimulatorProjectArtifacts::ReadScenario(const std::string_view relativePath) {
        using Output = Result<std::vector<std::byte>>;
        if (const auto admitted = impl_->Admit(); admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        Impl::Operation operation{*impl_};
        if (!Platform::IsSafeProjectReadPath(relativePath))
            return Output::Failure(MakeError(Platform::ProjectReadErrors::UnsafePath));
        const auto context = impl_->Context();
        if (const auto current = impl_->Check(context); current.HasError())
            return Output::Failure(current.ErrorValue());
        const auto loaded = impl_->composition.source->SourceText(relativePath, context,
                                                                  {.maximumFileBytes = XR::XRSimulatorHardLimits::MaximumEncodedBytes,
                                                                   .maximumDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                       OperationDuration)});
        if (loaded.HasError())
            return Output::Failure(loaded.ErrorValue());
        if (const auto current = impl_->Check(context); current.HasError())
            return Output::Failure(current.ErrorValue());
        const auto &snapshot = loaded.Value();
        if (!Matches(snapshot, context, relativePath))
            return Output::Failure(MakeError(Platform::ProjectReadErrors::Stale));
        const auto bytes = std::as_bytes(std::span{snapshot.text->data(), snapshot.text->size()});
        return Output::Success(std::vector<std::byte>{bytes.begin(), bytes.end()});
    }

    /** @copydoc SimulatorProjectArtifacts::WriteCapture */
    Result<void> SimulatorProjectArtifacts::WriteCapture(const std::string_view name, const std::span<const std::byte> bytes,
                                                         AtomicFileReplacementReceipt &receipt) {
        if (const auto admitted = impl_->Admit(); admitted.HasError())
            return admitted;
        Impl::Operation operation{*impl_};
        if (!CaptureName(name) || receipt.WasCommitted())
            return Result<void>::Failure(MakeError(XR::XRSimulatorErrors::ArtifactInvalid));
        const auto context = impl_->Context();
        if (const auto current = impl_->Check(context); current.HasError())
            return current;
        if (const auto validated = CaptureBytes(bytes); validated.HasError())
            return validated;
        if (const auto current = impl_->Check(context); current.HasError())
            return current;
        const auto destination =
            impl_->composition.authorizedRoot / std::filesystem::path{PublicationDirectory} / std::filesystem::path{std::string{name}};
        return impl_->Publish(destination, bytes, context, receipt);
    }

    /** @copydoc SimulatorProjectArtifacts::PublicationNamespace */
    std::string_view SimulatorProjectArtifacts::PublicationNamespace() const noexcept {
        return PublicationDirectory;
    }

    /** @copydoc SimulatorProjectArtifacts::Revoke */
    void SimulatorProjectArtifacts::Revoke() noexcept {
        impl_->revoked.store(true, std::memory_order_release);
    }
}  // namespace Horo::Application::Internal
