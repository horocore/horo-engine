#include "Horo/Audio/Internal/WasapiAudioBackend.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/NativeAudioFacts.h"
#include "WasapiNativeOwners.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <audioclient.h>
#include <avrt.h>
#include <chrono>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <limits>
#include <mmdeviceapi.h>
#include <numeric>
#include <string>
#include <wrl/client.h>

namespace Horo::Audio::Backend {
    namespace {
        using Microsoft::WRL::ComPtr;
        using WasapiDetail::Apartment;
        using WasapiDetail::Handle;
        using WasapiDetail::NativeStringDelete;
        using WasapiDetail::Notifications;
        using WasapiDetail::PropertyValue;
        using WasapiDetail::WaveDelete;

        /** @brief Retain the exact native status in a control-only diagnostic without exposing native types. */
        Error NativeError(const char *operation, const HRESULT status) {
            return MakeError(status == AUDCLNT_E_DEVICE_INVALIDATED ? AudioErrors::DeviceLost : AudioErrors::BackendFailed,
                             std::string(operation) + " HRESULT=" + std::to_string(static_cast<std::uint32_t>(status)));
        }

        /** @brief Monotonic host clock, independent of the endpoint hardware clock. */
        std::uint64_t Nanoseconds() noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /** @brief Bounded UTF-8 friendly name conversion; endpoint IDs never enter presentation metadata. */
        std::string FriendlyName(IMMDevice &device) {
            ComPtr<IPropertyStore> properties;
            PropertyValue property;
            auto &value = property.value;
            std::string name = "Windows audio output";
            if (SUCCEEDED(device.OpenPropertyStore(STGM_READ, &properties)) &&
                SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
                const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1 && bytes <= 257) {
                    std::string converted(static_cast<std::size_t>(bytes), '\0');
                    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.pwszVal, -1, converted.data(), bytes, nullptr, nullptr)) {
                        converted.pop_back();
                        name = std::move(converted);
                    }
                }
            }
            return name;
        }

        /** @brief Translate native bit order into explicit Horo speaker semantics without inventing missing channels. */
        AudioChannelLayout Layout(const WAVEFORMATEX &wave) {
            DWORD mask{};
            if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= 22)
                mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(wave).dwChannelMask;
            else if (wave.nChannels == 1)
                mask = SPEAKER_FRONT_CENTER;
            else if (wave.nChannels == 2)
                mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            const std::array<std::pair<DWORD, AudioSpeakerRole>, 12> roles{{{SPEAKER_FRONT_LEFT, AudioSpeakerRole::FrontLeft},
                                                                            {SPEAKER_FRONT_RIGHT, AudioSpeakerRole::FrontRight},
                                                                            {SPEAKER_FRONT_CENTER, AudioSpeakerRole::FrontCenter},
                                                                            {SPEAKER_LOW_FREQUENCY, AudioSpeakerRole::LowFrequency},
                                                                            {SPEAKER_BACK_LEFT, AudioSpeakerRole::BackLeft},
                                                                            {SPEAKER_BACK_RIGHT, AudioSpeakerRole::BackRight},
                                                                            {SPEAKER_SIDE_LEFT, AudioSpeakerRole::SideLeft},
                                                                            {SPEAKER_SIDE_RIGHT, AudioSpeakerRole::SideRight},
                                                                            {SPEAKER_TOP_FRONT_LEFT, AudioSpeakerRole::TopFrontLeft},
                                                                            {SPEAKER_TOP_FRONT_RIGHT, AudioSpeakerRole::TopFrontRight},
                                                                            {SPEAKER_TOP_BACK_LEFT, AudioSpeakerRole::TopBackLeft},
                                                                            {SPEAKER_TOP_BACK_RIGHT, AudioSpeakerRole::TopBackRight}}};
            AudioChannelLayout layout;
            for (const auto &[bit, role] : roles)
                if (mask & bit) {
                    layout.orderedChannels.emplace_back(role);
                    mask &= ~bit;
                }
            if (mask != 0 || layout.orderedChannels.size() != wave.nChannels)
                return {};
            return layout;
        }

        /** @brief WASAPI owns COM on MTA control and one event-driven, joinable render worker. */
        class WasapiDriver final : public NativeAudioDriver {
        public:
            explicit WasapiDriver(const NativeAudioBackendConfig config) : config_(config), facts_(config.owner) {}

            AudioBackendKind Kind() const noexcept override {
                return AudioBackendKind::WASAPI;
            }

            ResourceDisposition Resources() const noexcept override {
                if (worker_.value || cleanupRetained_)
                    return ResourceDisposition::Retained;
                return client_ || apartment_ ? ResourceDisposition::CallbackDetached : ResourceDisposition::Closed;
            }

            bool HasUndrainedRequiredFacts() const noexcept override {
                return facts_.HasLoss() || HasNativeLoss();
            }

            Result<AudioBackendProbe> Probe() override {
                Apartment apartment;
                ComPtr<IMMDeviceEnumerator> enumerator;
                auto status = apartment.status;
                if (SUCCEEDED(status))
                    status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
                if (FAILED(status))
                    return Result<AudioBackendProbe>::Failure(NativeError("WASAPI probe", status));
                AudioBackendProbe probe{.backend = Kind(),
                                        .compiled = true,
                                        .hostSupported = true,
                                        .availability = AudioBackendAvailability::Available,
                                        .revision = 1,
                                        .backendVersion = "WASAPI IAudioClient3"};
                probe.features.fill(AudioCapabilitySupport::Unsupported);
                for (const auto feature : {AudioBackendCapability::PhysicalEnumeration, AudioBackendCapability::NativeHotplug,
                                           AudioBackendCapability::NativeDiagnostics})
                    probe.features[static_cast<std::size_t>(feature)] = AudioCapabilitySupport::Available;
                return Result<AudioBackendProbe>::Success(std::move(probe));
            }

            Result<AudioDeviceSnapshot> Enumerate() override {
                Apartment apartment;
                if (FAILED(apartment.status))
                    return Result<AudioDeviceSnapshot>::Failure(NativeError("WASAPI MTA", apartment.status));
                ComPtr<IMMDeviceEnumerator> enumerator;
                auto status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
                if (FAILED(status))
                    return Result<AudioDeviceSnapshot>::Failure(NativeError("WASAPI enumerator", status));
                ComPtr<IMMDeviceCollection> collection;
                status = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
                UINT count{};
                if (SUCCEEDED(status))
                    status = collection->GetCount(&count);
                if (FAILED(status))
                    return Result<AudioDeviceSnapshot>::Failure(NativeError("WASAPI endpoints", status));
                if (count > MaximumAudioDiscoveredDevices)
                    return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
                auto registry = registry_;
                for (auto &entry : registry)
                    entry.present = false;
                AudioDeviceSnapshot next{.owner = config_.owner, .backend = Kind(), .revision = snapshot_.revision + 1};
                for (UINT index = 0; index < count; ++index) {
                    ComPtr<IMMDevice> device;
                    status = collection->Item(index, &device);
                    LPWSTR allocated{};
                    if (SUCCEEDED(status))
                        status = device->GetId(&allocated);
                    if (FAILED(status)) {
                        CoTaskMemFree(allocated);
                        return Result<AudioDeviceSnapshot>::Failure(NativeError("WASAPI endpoint ID", status));
                    }
                    const std::unique_ptr<wchar_t, NativeStringDelete> ownedId(allocated);
                    std::wstring id(ownedId.get());
                    auto found = std::find_if(registry.begin(), registry.end(), [&id](const auto &entry) {
                        return entry.native == id;
                    });
                    if (found == registry.end()) {
                        if (registry.size() == 4'096)
                            return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
                        registry.push_back({std::move(id), {config_.owner, static_cast<std::uint32_t>(registry.size() + 1), 1}, true});
                        found = std::prev(registry.end());
                    } else {
                        const auto old = std::find_if(registry_.begin(), registry_.end(), [&found](const auto &entry) {
                            return entry.id == found->id;
                        });
                        if (old != registry_.end() && !old->present) {
                            if (found->id.generation == std::numeric_limits<std::uint32_t>::max())
                                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
                            ++found->id.generation;
                        }
                        found->present = true;
                    }
                    next.devices.push_back({found->id, FriendlyName(*device.Get()), AudioDeviceClass::Unknown});
                }
                const auto defaultFor = [&enumerator, &registry](const ERole role) -> std::optional<AudioDeviceId> {
                    ComPtr<IMMDevice> device;
                    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device)))
                        return std::nullopt;
                    LPWSTR id{};
                    if (FAILED(device->GetId(&id)))
                        return std::nullopt;
                    const std::unique_ptr<wchar_t, NativeStringDelete> ownedId(id);
                    const auto found = std::find_if(registry.begin(), registry.end(), [id](const auto &entry) {
                        return entry.present && entry.native == id;
                    });
                    return found == registry.end() ? std::nullopt : std::optional(found->id);
                };
                next.defaults = {defaultFor(eConsole), defaultFor(eMultimedia), defaultFor(eCommunications)};
                if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max())
                    return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
                registry_ = std::move(registry);
                snapshot_ = next;
                return Result<AudioDeviceSnapshot>::Success(std::move(next));
            }

            Result<Opened> Open(const Backend::Open &request) override {
                auto result = Acquire(request);
                if (result.HasError()) {
                    const auto closed = Close();  // No worker or render port exists before Start.
                    if (closed.HasError())
                        return Result<Opened>::Failure(WithCause(closed.ErrorValue(), result.ErrorValue()));
                    return result;
                }
                return result;
            }

            Result<void> Start(const NativeAudioRenderPort render) override {
                if (!client_ || worker_.value || !render.process)
                    return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
                ResetEvent(stop_.value);
                render_ = render;
                renderFailure_.store(S_OK);
                const auto status = client_->Start();
                if (FAILED(status)) {
                    render_ = {};
                    return Result<void>::Failure(NativeError("WASAPI start", status));
                }
                worker_.Reset(CreateThread(nullptr, 0, WorkerEntry, this, 0, nullptr));
                if (!worker_.value) {
                    const auto failure = NativeError("WASAPI worker", HRESULT_FROM_WIN32(GetLastError()));
                    const auto stopped = client_->Stop();
                    render_ = {};
                    return Result<void>::Failure(FAILED(stopped) ? WithCause(NativeError("WASAPI worker rollback", stopped), failure)
                                                                 : failure);
                }
                return Result<void>::Success();
            }

            NativeRetirementResult Stop(const AudioMonotonicTimestamp deadline) override {
                if (worker_.value) {
                    SetEvent(stop_.value);
                    const auto now = Nanoseconds();
                    const auto remaining = deadline.nanoseconds > now ? deadline.nanoseconds - now : 0;
                    const auto milliseconds = std::min<std::uint64_t>(remaining / 1'000'000 + (remaining % 1'000'000 != 0), INFINITE - 1);
                    if (WaitForSingleObject(worker_.value, static_cast<DWORD>(milliseconds)) != WAIT_OBJECT_0)
                        return NativeRetirementResult::Failure(
                            MakeError(AudioErrors::BackendFailed, "WASAPI render worker did not detach by deadline"));
                    worker_.Reset();
                }
                if (client_) {
                    const auto status = client_->Stop();
                    if (FAILED(status) && status != AUDCLNT_E_DEVICE_INVALIDATED)
                        return NativeRetirementResult::Failure(NativeError("WASAPI stop", status));
                }
                render_ = {};
                return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
            }

            NativeRetirementResult Close() override {
                cleanupRetained_ = static_cast<bool>(client_) || static_cast<bool>(notifications_);
                if (worker_.value)
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::RuntimeInactive));
                if (notificationsRegistered_ && notifications_ && enumerator_) {
                    const auto status = enumerator_->UnregisterEndpointNotificationCallback(notifications_.Get());
                    if (FAILED(status))
                        return NativeRetirementResult::Failure(NativeError("WASAPI notification detach", status));
                    notificationsRegistered_ = false;
                }
                if (!CaptureNativeFacts())
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::EventQueueInvalid));
                notifications_.Reset();
                client_.Reset();
                enumerator_.Reset();
                streamEvent_.Reset();
                stop_.Reset();
                apartment_.reset();
                epoch_ = {};
                cleanupRetained_ = false;
                return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
            }

            std::size_t DrainEvents(const std::span<Event> output) noexcept override {
                (void)CaptureNativeFacts();
                return facts_.Drain(output);
            }

        private:
            struct Entry final {
                std::wstring native;
                AudioDeviceId id;
                bool present{};
            };

            NativeAudioBackendConfig config_;
            std::vector<Entry> registry_;
            AudioDeviceSnapshot snapshot_;
            std::unique_ptr<Apartment> apartment_;
            ComPtr<IMMDeviceEnumerator> enumerator_;
            ComPtr<IAudioClient3> client_;
            ComPtr<Notifications> notifications_;
            Handle streamEvent_, stop_, worker_;
            AudioDeviceEpoch epoch_;
            NativeAudioRenderPort render_;
            std::uint32_t bufferFrames_{}, channels_{};
            // Worker writes one native failure; notification object independently owns its callback facts.
            std::atomic<HRESULT> renderFailure_{S_OK};
            std::uint64_t drainedChanges_{};
            bool lossDrained_{};
            bool cleanupRetained_{};
            bool notificationsRegistered_{};
            NativeAudioFacts facts_;

            /** @brief Observe only the admitted epoch's unaccepted native loss, never stale worker status from a closed connection. */
            bool HasNativeLoss() const noexcept {
                return !lossDrained_ && epoch_.device.IsValid() &&
                       ((notifications_ && notifications_->loss.Read().has_value()) || FAILED(renderFailure_.load()));
            }

            /** @brief Transfer callback-source facts to control storage before unregister cleanup can release their source. */
            bool CaptureNativeFacts() noexcept {
                if (HasNativeLoss()) {
                    const auto status = renderFailure_.load();
                    const auto fallback =
                        status == AUDCLNT_E_DEVICE_INVALIDATED ? DeviceLossCause::Disconnected : DeviceLossCause::BackendFailure;
                    const auto cause = notifications_ ? notifications_->loss.Read().value_or(fallback) : fallback;
                    if (!facts_.CaptureLoss({epoch_.device, cause}))
                        return false;
                    lossDrained_ = true;
                }
                if (notifications_) {
                    const auto changes = notifications_->changes.load();
                    if (changes != drainedChanges_) {
                        facts_.CaptureChange(snapshot_.revision);
                        drainedChanges_ = changes;
                    }
                }
                return true;
            }

            /** @brief A role changed after discovery must force fresh control resolution instead of opening the old default. */
            Result<void> ValidateDefault(const Backend::Open &request, const Entry &endpoint) {
                const auto *role = std::get_if<AudioDefaultDeviceRole>(&request.format.device);
                if (!role)
                    return Result<void>::Success();
                ERole native;
                switch (*role) {
                    case AudioDefaultDeviceRole::Console:
                        native = eConsole;
                        break;
                    case AudioDefaultDeviceRole::Multimedia:
                        native = eMultimedia;
                        break;
                    case AudioDefaultDeviceRole::Communications:
                        native = eCommunications;
                        break;
                    default:
                        return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
                }
                ComPtr<IMMDevice> current;
                auto status = enumerator_->GetDefaultAudioEndpoint(eRender, native, &current);
                LPWSTR allocated{};
                if (SUCCEEDED(status))
                    status = current->GetId(&allocated);
                const std::unique_ptr<wchar_t, NativeStringDelete> id(allocated);
                if (FAILED(status))
                    return Result<void>::Failure(NativeError("WASAPI default revalidation", status));
                return endpoint.native == id.get() ? Result<void>::Success() : Result<void>::Failure(MakeError(AudioErrors::HandleStale));
            }

            /** @brief Resolve the discovered endpoint and revalidate a requested default before native activation. */
            Result<const Entry *> SelectEndpoint(const Backend::Open &request) {
                const auto selected = ResolveAudioDevice(snapshot_, request.format.device);
                const auto endpoint = std::find_if(registry_.begin(), registry_.end(), [&selected](const auto &entry) {
                    return entry.present && entry.id == selected.device;
                });
                if (endpoint == registry_.end() || selected.device != request.plannedEpoch.device)
                    return Result<const Entry *>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                if (const auto current = ValidateDefault(request, *endpoint); current.HasError())
                    return Result<const Entry *>::Failure(current.ErrorValue());
                return Result<const Entry *>::Success(&*endpoint);
            }

            using WaveFormat = std::unique_ptr<WAVEFORMATEX, WaveDelete>;

            /** @brief Acquire the selected native client and own its returned mix format through every failure. */
            Result<WaveFormat> OpenClient(const Entry &endpoint) {
                ComPtr<IMMDevice> device;
                auto status = enumerator_->GetDevice(endpoint.native.c_str(), &device);
                if (SUCCEEDED(status))
                    status = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client_);
                WAVEFORMATEX *allocated{};
                if (SUCCEEDED(status))
                    status = client_->GetMixFormat(&allocated);
                WaveFormat wave(allocated);
                if (FAILED(status))
                    return Result<WaveFormat>::Failure(NativeError("WASAPI mix format", status));
                return Result<WaveFormat>::Success(std::move(wave));
            }

            /** @brief Admit only complete interleaved float speaker tuples supported by this peer. */
            Result<AudioProcessingFormat> NativeFormat(const WAVEFORMATEX &wave) {
                const bool floating = wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                      (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= 22 &&
                                       reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(wave).SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                if (!floating || wave.wBitsPerSample != 32 || wave.nBlockAlign != wave.nChannels * sizeof(float))
                    return Result<AudioProcessingFormat>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                AudioProcessingFormat actual{wave.nSamplesPerSec, Layout(wave)};
                if (!ValidateAudioProcessingFormat(actual))
                    return Result<AudioProcessingFormat>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                return Result<AudioProcessingFormat>::Success(std::move(actual));
            }

            /** @brief Initialize one legal shared-engine period within the caller's admitted bounds. */
            Result<std::uint32_t> InitializeClient(const Backend::Open &request, const WAVEFORMATEX &wave) {
                UINT32 defaultPeriod{}, fundamental{}, minimum{}, maximum{};
                auto status = client_->GetSharedModeEnginePeriod(&wave, &defaultPeriod, &fundamental, &minimum, &maximum);
                if (FAILED(status))
                    return Result<std::uint32_t>::Failure(NativeError("WASAPI periods", status));
                const auto period = SelectNativeAudioPeriod(request.format.period, minimum, maximum, fundamental);
                if (!period)
                    return Result<std::uint32_t>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                status = client_->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, *period, &wave, nullptr);
                if (SUCCEEDED(status))
                    status = client_->GetBufferSize(&bufferFrames_);
                if (FAILED(status))
                    return Result<std::uint32_t>::Failure(NativeError("WASAPI initialize", status));
                if (bufferFrames_ > MaximumAudioCallbackFrames)
                    return Result<std::uint32_t>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                return Result<std::uint32_t>::Success(*period);
            }

            /** @brief Prepare kernel events and a separately owned native notification registration before Start. */
            Result<void> PrepareNotifications(const Entry &endpoint) {
                streamEvent_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
                if (!streamEvent_.value)
                    return Result<void>::Failure(NativeError("WASAPI stream event", HRESULT_FROM_WIN32(GetLastError())));
                stop_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!stop_.value)
                    return Result<void>::Failure(NativeError("WASAPI stop event", HRESULT_FROM_WIN32(GetLastError())));
                auto status = client_->SetEventHandle(streamEvent_.value);
                if (FAILED(status))
                    return Result<void>::Failure(NativeError("WASAPI event binding", status));
                notifications_.Attach(new Notifications(endpoint.native));
                status = enumerator_->RegisterEndpointNotificationCallback(notifications_.Get());
                if (FAILED(status)) {
                    notifications_.Reset();
                    return Result<void>::Failure(NativeError("WASAPI notifications", status));
                }
                notificationsRegistered_ = true;
                return Result<void>::Success();
            }

            /** @brief Capture API stream delay as an overlapping adapter observation, never physical end-to-end latency. */
            AudioDeviceTimingReport Timing() {
                AudioDeviceTimingReport timing{.epoch = epoch_, .capturedAt = {config_.clockDomain, Nanoseconds()}};
                REFERENCE_TIME latency{};
                if (SUCCEEDED(client_->GetStreamLatency(&latency)) && latency >= 0)
                    timing.adapterLatency = {.quality = AudioObservationQuality::Reported,
                                             .source = AudioObservationSource::BackendApi,
                                             .nanoseconds = static_cast<std::uint64_t>(latency) * 100,
                                             .observedAt = timing.capturedAt};
                return timing;
            }

            /** @brief Acquire native resources in order; Open unwinds this stopped-only prefix on failure. */
            Result<Opened> Acquire(const Backend::Open &request) {
                apartment_ = std::make_unique<Apartment>();
                if (FAILED(apartment_->status))
                    return Result<Opened>::Failure(NativeError("WASAPI MTA", apartment_->status));
                const auto status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
                if (FAILED(status))
                    return Result<Opened>::Failure(NativeError("WASAPI enumerator", status));
                const auto endpoint = SelectEndpoint(request);
                if (endpoint.HasError())
                    return Result<Opened>::Failure(endpoint.ErrorValue());
                auto wave = OpenClient(*endpoint.Value());
                if (wave.HasError())
                    return Result<Opened>::Failure(wave.ErrorValue());
                const auto actual = NativeFormat(*wave.Value());
                if (actual.HasError())
                    return Result<Opened>::Failure(actual.ErrorValue());
                const auto period = InitializeClient(request, *wave.Value());
                if (period.HasError())
                    return Result<Opened>::Failure(period.ErrorValue());
                auto format = NegotiateNativeFloatOutput(request, actual.Value(), snapshot_.revision, period.Value());
                if (format.HasError())
                    return Result<Opened>::Failure(format.ErrorValue());
                if (const auto prepared = PrepareNotifications(*endpoint.Value()); prepared.HasError())
                    return Result<Opened>::Failure(prepared.ErrorValue());
                epoch_ = request.plannedEpoch;
                renderFailure_.store(S_OK);
                channels_ = wave.Value()->nChannels;
                lossDrained_ = false;
                drainedChanges_ = 0;
                return Result<Opened>::Success({std::move(format).Value(), Timing(), AccessMode::Shared});
            }

            /** @brief Serialize all render packet pairs and render-interface lifetime on the same MTA worker. */
            static DWORD WINAPI WorkerEntry(void *context) noexcept {
                auto &self = *static_cast<WasapiDriver *>(context);
                Apartment apartment;
                ComPtr<IAudioRenderClient> renderer;
                HRESULT status = apartment.status;
                if (SUCCEEDED(status))
                    status = self.client_->GetService(IID_PPV_ARGS(&renderer));
                DWORD taskIndex{};
                const HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
                const std::array handles{self.stop_.value, self.streamEvent_.value};
                while (SUCCEEDED(status)) {
                    const auto signalled = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
                    if (signalled == WAIT_OBJECT_0)
                        break;
                    if (signalled != WAIT_OBJECT_0 + 1) {
                        status = HRESULT_FROM_WIN32(GetLastError());
                        break;
                    }
                    UINT32 padding{};
                    status = self.client_->GetCurrentPadding(&padding);
                    if (FAILED(status))
                        break;
                    if (padding > self.bufferFrames_) {
                        status = E_UNEXPECTED;
                        break;
                    }
                    const auto frames = self.bufferFrames_ - padding;
                    if (!frames)
                        continue;
                    BYTE *bytes{};
                    status = renderer->GetBuffer(frames, &bytes);
                    if (FAILED(status))
                        break;
                    self.render_.process(self.render_.context,
                                         {reinterpret_cast<float *>(bytes), static_cast<std::size_t>(frames) * self.channels_}, frames);
                    status = renderer->ReleaseBuffer(frames, 0);
                }
                if (task)
                    AvRevertMmThreadCharacteristics(task);
                if (FAILED(status) && self.notifications_)
                    self.notifications_->loss.Capture(status == AUDCLNT_E_DEVICE_INVALIDATED ? DeviceLossCause::Disconnected
                                                                                             : DeviceLossCause::BackendFailure);
                self.renderFailure_.store(status);
                return 0;
            }
        };
    }  // namespace

    /** @copydoc CreateWasapiAudioBackend */
    Result<std::unique_ptr<NativeAudioBackend>> CreateWasapiAudioBackend(const NativeAudioBackendConfig config) {
        try {
            return CreateNativeAudioBackend(config, std::make_unique<WasapiDriver>(config));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<NativeAudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio::Backend
