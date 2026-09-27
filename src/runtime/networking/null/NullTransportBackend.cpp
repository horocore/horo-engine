#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkErrors.h"

#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        /** @brief Unique deterministic transport owner behind the composition-only lifetime. */
        class NullTransportBackendInstance final : public ITransportBackendInstance {
        public:
            explicit NullTransportBackendInstance(DeterministicTransport transport) noexcept : transport_(std::move(transport)) {}

            void RequestCancellation() noexcept override {
                static_cast<void>(transport_.Shutdown());
            }

            void Shutdown() noexcept override {
                static_cast<void>(transport_.Shutdown());
            }

        private:
            DeterministicTransport transport_;
        };
    }  // namespace

    /** @copydoc CreateDeterministicTransportBackend */
    Result<TransportBackendInstance> CreateDeterministicTransportBackend(const DeterministicTransportDescriptor &descriptor) {
        auto created = DeterministicTransport::Create(descriptor);
        if (created.HasError())
            return Result<TransportBackendInstance>::Failure(created.ErrorValue());
        try {
            return Result<TransportBackendInstance>::Success(std::make_unique<NullTransportBackendInstance>(std::move(created).Value()));
        } catch (const std::bad_alloc &) {
            return Result<TransportBackendInstance>::Failure(MakeError(NetworkErrors::TransportBackendFactoryFailed));
        }
    }
}  // namespace Horo::Network
