#include "iramix/plugin/Vst3Host.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(IRAMIX_HAS_VST3_SDK)
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#endif

namespace iramix::plugin {

#if defined(IRAMIX_HAS_VST3_SDK)
namespace {

using namespace Steinberg;

[[nodiscard]] bool sameInterface(const TUID left, const TUID right)
    noexcept {
    return std::memcmp(left, right, sizeof(TUID)) == 0;
}

// Memory-backed IBStream. The SDK ships one, but it lives in public.sdk,
// which would mean compiling SDK sources; only the interface headers are
// used here, so the few methods a state transfer needs are implemented
// directly.
class MemoryStream final : public IBStream {
public:
    explicit MemoryStream(std::vector<std::byte> initial = {})
        : bytes_ {std::move(initial)} {}

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

    tresult PLUGIN_API queryInterface(const TUID interfaceId, void** object)
        override {
        if (object == nullptr) {
            return kInvalidArgument;
        }
        if (sameInterface(interfaceId, IBStream_iid)
            || sameInterface(interfaceId, FUnknown_iid)) {
            *object = static_cast<IBStream*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    // Reference counting is deliberately inert: the stream is a stack
    // object owned by the caller for the duration of one call, and a
    // plugin that retained it past that would be misbehaving in a way no
    // count here could rescue.
    uint32 PLUGIN_API addRef() override {
        return 1U;
    }

    uint32 PLUGIN_API release() override {
        return 1U;
    }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numRead)
        override {
        if (buffer == nullptr || numBytes < 0) {
            return kInvalidArgument;
        }
        const auto available = bytes_.size() - position_;
        const auto count = std::min(
            static_cast<std::size_t>(numBytes),
            available
        );
        if (count != 0U) {
            std::memcpy(buffer, bytes_.data() + position_, count);
            position_ += count;
        }
        if (numRead != nullptr) {
            *numRead = static_cast<int32>(count);
        }
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numWritten)
        override {
        if (buffer == nullptr || numBytes < 0) {
            return kInvalidArgument;
        }
        const auto count = static_cast<std::size_t>(numBytes);
        if (position_ + count > bytes_.size()) {
            bytes_.resize(position_ + count);
        }
        std::memcpy(
            bytes_.data() + position_,
            buffer,
            count
        );
        position_ += count;
        if (numWritten != nullptr) {
            *numWritten = static_cast<int32>(count);
        }
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 position, int32 mode, int64* result)
        override {
        std::int64_t target = 0;
        switch (mode) {
        case kIBSeekSet:
            target = position;
            break;
        case kIBSeekCur:
            target = static_cast<std::int64_t>(position_) + position;
            break;
        case kIBSeekEnd:
            target = static_cast<std::int64_t>(bytes_.size()) + position;
            break;
        default:
            return kInvalidArgument;
        }
        if (target < 0
            || target > static_cast<std::int64_t>(bytes_.size())) {
            return kInvalidArgument;
        }
        position_ = static_cast<std::size_t>(target);
        if (result != nullptr) {
            *result = target;
        }
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* position) override {
        if (position == nullptr) {
            return kInvalidArgument;
        }
        *position = static_cast<int64>(position_);
        return kResultOk;
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t position_ {0U};
};

// Plugins routinely query the host for its identity and refuse to
// initialise without one, so a minimal implementation is not optional.
class HostApplication final : public Vst::IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID interfaceId, void** object)
        override {
        if (object == nullptr) {
            return kInvalidArgument;
        }
        if (sameInterface(interfaceId, Vst::IHostApplication_iid)
            || sameInterface(interfaceId, FUnknown_iid)) {
            *object = static_cast<Vst::IHostApplication*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override {
        return 1U;
    }

    uint32 PLUGIN_API release() override {
        return 1U;
    }

    tresult PLUGIN_API getName(Vst::String128 name) override {
        static constexpr char16_t text[] = u"Iramix";
        std::size_t index = 0U;
        for (; text[index] != u'\0' && index < 127U; ++index) {
            name[index] = static_cast<Steinberg::char16>(text[index]);
        }
        name[index] = 0;
        return kResultOk;
    }

    // Iramix supplies no host-side objects to plugins in Phase 0. Saying
    // so honestly is better than handing back something half-implemented.
    tresult PLUGIN_API createInstance(TUID, TUID, void** object) override {
        if (object != nullptr) {
            *object = nullptr;
        }
        return kNotImplemented;
    }
};

// One parameter's value points within a single block. This host only
// ever delivers the newest queued value for a parameter at sample offset
// zero — never a ramp within the block — so a one-element queue is
// exact, not a simplification of something richer.
class ParameterChangeQueue final : public Vst::IParamValueQueue {
public:
    void reset(const Vst::ParamID parameterId, const Vst::ParamValue value) {
        parameterId_ = parameterId;
        value_ = value;
    }

    tresult PLUGIN_API queryInterface(const TUID interfaceId, void** object)
        override {
        if (object == nullptr) {
            return kInvalidArgument;
        }
        if (sameInterface(interfaceId, Vst::IParamValueQueue_iid)
            || sameInterface(interfaceId, FUnknown_iid)) {
            *object = static_cast<Vst::IParamValueQueue*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override {
        return 1U;
    }

    uint32 PLUGIN_API release() override {
        return 1U;
    }

    Vst::ParamID PLUGIN_API getParameterId() override {
        return parameterId_;
    }

    int32 PLUGIN_API getPointCount() override {
        return 1;
    }

    tresult PLUGIN_API getPoint(
        const int32 index,
        int32& sampleOffset,
        Vst::ParamValue& value
    ) override {
        if (index != 0) {
            return kInvalidArgument;
        }
        sampleOffset = 0;
        value = value_;
        return kResultOk;
    }

    tresult PLUGIN_API addPoint(int32, Vst::ParamValue, int32&) override {
        // Never called: this host only ever presents a queue it built
        // itself, and never hands it to the plugin to write into.
        return kNotImplemented;
    }

private:
    Vst::ParamID parameterId_ {0U};
    Vst::ParamValue value_ {0.0};
};

// Presents this block's pending parameter changes to
// IAudioProcessor::process(). Backed by fixed storage the caller owns —
// no allocation, matching the real-time-adjacent contract process()
// already promises.
class ParameterChanges final : public Vst::IParameterChanges {
public:
    explicit ParameterChanges(
        std::span<ParameterChangeQueue> storage
    ) : storage_ {storage} {}

    void setActiveCount(const std::size_t count) {
        count_ = std::min(count, storage_.size());
    }

    tresult PLUGIN_API queryInterface(const TUID interfaceId, void** object)
        override {
        if (object == nullptr) {
            return kInvalidArgument;
        }
        if (sameInterface(interfaceId, Vst::IParameterChanges_iid)
            || sameInterface(interfaceId, FUnknown_iid)) {
            *object = static_cast<Vst::IParameterChanges*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override {
        return 1U;
    }

    uint32 PLUGIN_API release() override {
        return 1U;
    }

    int32 PLUGIN_API getParameterCount() override {
        return static_cast<int32>(count_);
    }

    Vst::IParamValueQueue* PLUGIN_API getParameterData(const int32 index)
        override {
        if (index < 0 || static_cast<std::size_t>(index) >= count_) {
            return nullptr;
        }
        return &storage_[static_cast<std::size_t>(index)];
    }

    Vst::IParamValueQueue* PLUGIN_API addParameterData(
        const Vst::ParamID&,
        int32&
    ) override {
        // Never called for the same reason ParameterChangeQueue::addPoint
        // is not: this host presents its own pre-built queue rather than
        // accepting the plugin's.
        return nullptr;
    }

private:
    std::span<ParameterChangeQueue> storage_;
    std::size_t count_ {0U};
};

} // namespace
#endif

struct Vst3Host::Impl final {
    Vst3HostInfo info {};
    bool opened {false};

#if defined(_WIN32)
    HMODULE module {nullptr};
#else
    void* module {nullptr};
#endif

#if defined(IRAMIX_HAS_VST3_SDK)
    HostApplication hostApplication {};
    Steinberg::Vst::IComponent* component {nullptr};
    Steinberg::Vst::IAudioProcessor* processor {nullptr};
    // Null when the plugin exposes no IEditController at all. Equal to
    // component when the plugin implements both interfaces on the same
    // object, in which case it must not be terminated/released a second
    // time.
    Steinberg::Vst::IEditController* controller {nullptr};
    bool controllerIsComponent {false};
    bool processing {false};
    bool active {false};

    std::uint32_t inputChannels {0U};
    std::uint32_t outputChannels {0U};
    std::uint32_t maximumFrames {0U};
    std::uint32_t bridgeChannels {0U};

    // Deinterleaved scratch, sized once by open(). VST3 wants one pointer
    // per channel; the bridge carries interleaved audio.
    std::vector<std::vector<float>> inputStorage;
    std::vector<std::vector<float>> outputStorage;
    std::vector<float*> inputPointers;
    std::vector<float*> outputPointers;

    // Changes queued by setParameterNormalized() since the last
    // process() call. Bounded rather than grown: a block only ever needs
    // to carry however many distinct parameters actually changed since
    // the previous one, and an automation storm beyond that bound simply
    // keeps the newest value per parameter already queued rather than
    // allocating a bigger one.
    static constexpr std::size_t kMaxPendingParameters = 32U;
    std::array<ParameterChangeQueue, kMaxPendingParameters>
        pendingParameterQueues {};
    std::size_t pendingParameterCount {0U};
#endif
};

Vst3Host::Vst3Host() : impl_ {std::make_unique<Impl>()} {}

Vst3Host::~Vst3Host() {
    close();
}

bool Vst3Host::available() noexcept {
#if defined(IRAMIX_HAS_VST3_SDK)
    return true;
#else
    return false;
#endif
}

bool Vst3Host::opened() const noexcept {
    return impl_->opened;
}

const Vst3HostInfo& Vst3Host::info() const noexcept {
    return impl_->info;
}

#if !defined(IRAMIX_HAS_VST3_SDK)

bool Vst3Host::open(
    const std::filesystem::path&,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    double,
    std::string& error
) {
    error = "built without the VST3 SDK; set IRAMIX_VST3_SDK_PATH";
    return false;
}

void Vst3Host::close() noexcept {}

bool Vst3Host::process(const float*, float*, std::uint32_t) noexcept {
    return false;
}

bool Vst3Host::saveState(std::vector<std::byte>&) {
    return false;
}

bool Vst3Host::loadState(std::span<const std::byte>) {
    return false;
}

bool Vst3Host::parameterInfo(std::uint32_t, Vst3ParameterInfo&) const {
    return false;
}

bool Vst3Host::setParameterNormalized(std::uint32_t, float) noexcept {
    return false;
}

#else

bool Vst3Host::open(
    const std::filesystem::path& modulePath,
    const std::uint32_t classIndex,
    const std::uint32_t maximumFrames,
    const std::uint32_t channelCount,
    const double sampleRate,
    std::string& error
) {
    error.clear();
    close();

#if defined(_WIN32)
    // Suppress the modal bad-image dialog for the same reason the scanner
    // does: an unloadable module must fail, not wait for a click.
    SetErrorMode(
        SEM_FAILCRITICALERRORS
        | SEM_NOGPFAULTERRORBOX
        | SEM_NOOPENFILEERRORBOX
    );
    impl_->module = LoadLibraryExA(
        modulePath.string().c_str(),
        nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH
    );
#else
    impl_->module = ::dlopen(
        modulePath.string().c_str(),
        RTLD_NOW | RTLD_LOCAL
    );
#endif
    if (impl_->module == nullptr) {
        error = "cannot load the plugin module";
        return false;
    }

#if defined(_WIN32)
    using InitDllProc = bool (*)();
    if (const auto initDll = reinterpret_cast<InitDllProc>(
            reinterpret_cast<void*>(GetProcAddress(impl_->module, "InitDll"))
        );
        initDll != nullptr) {
        static_cast<void>(initDll());
    }
    auto* const rawFactory =
        reinterpret_cast<void*>(GetProcAddress(impl_->module, "GetPluginFactory"));
#else
    auto* const rawFactory = ::dlsym(impl_->module, "GetPluginFactory");
#endif
    if (rawFactory == nullptr) {
        error = "the module exports no VST3 factory";
        close();
        return false;
    }

    using FactoryProc = Steinberg::IPluginFactory* (*)();
    auto* const factory =
        reinterpret_cast<FactoryProc>(rawFactory)();
    if (factory == nullptr) {
        error = "the module returned no factory";
        close();
        return false;
    }

    // Locate the classIndex-th audio class. Controller classes are skipped
    // because they are not the thing that processes audio.
    Steinberg::PClassInfo chosen {};
    bool haveClass = false;
    std::uint32_t audioIndex = 0U;
    const auto classes = factory->countClasses();
    for (Steinberg::int32 index = 0; index < classes; ++index) {
        Steinberg::PClassInfo classInfo {};
        if (factory->getClassInfo(index, &classInfo)
            != Steinberg::kResultOk) {
            continue;
        }
        if (std::string {classInfo.category} != kVstAudioEffectClass) {
            continue;
        }
        if (audioIndex == classIndex) {
            chosen = classInfo;
            haveClass = true;
            break;
        }
        ++audioIndex;
    }
    if (!haveClass) {
        error = "the module has no audio class at that index";
        factory->release();
        close();
        return false;
    }
    impl_->info.name = chosen.name;

    if (factory->createInstance(
            chosen.cid,
            Steinberg::Vst::IComponent_iid,
            reinterpret_cast<void**>(&impl_->component)
        ) != Steinberg::kResultOk
        || impl_->component == nullptr) {
        error = "the plugin refused to create a component";
        factory->release();
        close();
        return false;
    }

    if (impl_->component->initialize(&impl_->hostApplication)
        != Steinberg::kResultOk) {
        error = "the plugin failed to initialise";
        factory->release();
        close();
        return false;
    }

    // Parameters are optional, and their absence is not a reason to
    // refuse the plugin: many effects process audio with no automatable
    // surface at all. Most plugins implement IEditController on the same
    // object as IComponent; the SDK also allows a separate controller
    // class, which needs the factory kept alive one call longer.
    if (impl_->component->queryInterface(
            Steinberg::Vst::IEditController_iid,
            reinterpret_cast<void**>(&impl_->controller)
        ) == Steinberg::kResultOk
        && impl_->controller != nullptr) {
        impl_->controllerIsComponent = true;
    } else {
        Steinberg::TUID controllerId {};
        if (impl_->component->getControllerClassId(controllerId)
                == Steinberg::kResultOk
            && factory->createInstance(
                controllerId,
                Steinberg::Vst::IEditController_iid,
                reinterpret_cast<void**>(&impl_->controller)
            ) == Steinberg::kResultOk
            && impl_->controller != nullptr) {
            if (impl_->controller->initialize(&impl_->hostApplication)
                != Steinberg::kResultOk) {
                impl_->controller->release();
                impl_->controller = nullptr;
            }
        }
    }
    factory->release();

    if (impl_->component->queryInterface(
            Steinberg::Vst::IAudioProcessor_iid,
            reinterpret_cast<void**>(&impl_->processor)
        ) != Steinberg::kResultOk
        || impl_->processor == nullptr) {
        error = "the plugin is not an audio processor";
        close();
        return false;
    }

    impl_->info.acceptsFloat32 =
        impl_->processor->canProcessSampleSize(Steinberg::Vst::kSample32)
        == Steinberg::kResultOk;
    if (!impl_->info.acceptsFloat32) {
        error = "the plugin does not accept 32-bit float";
        close();
        return false;
    }

    const auto busChannels = [this](const Steinberg::Vst::BusDirection
        direction) -> std::uint32_t {
        Steinberg::Vst::BusInfo bus {};
        if (impl_->component->getBusCount(
                Steinberg::Vst::kAudio,
                direction
            ) <= 0) {
            return 0U;
        }
        if (impl_->component->getBusInfo(
                Steinberg::Vst::kAudio,
                direction,
                0,
                bus
            ) != Steinberg::kResultOk) {
            return 0U;
        }
        return static_cast<std::uint32_t>(std::max(bus.channelCount, 0));
    };

    impl_->inputChannels = busChannels(Steinberg::Vst::kInput);
    impl_->outputChannels = busChannels(Steinberg::Vst::kOutput);
    impl_->info.inputChannels = impl_->inputChannels;
    impl_->info.outputChannels = impl_->outputChannels;
    impl_->info.parameterCount = impl_->controller != nullptr
        ? static_cast<std::uint32_t>(impl_->controller->getParameterCount())
        : 0U;
    if (impl_->outputChannels == 0U) {
        error = "the plugin has no audio output bus";
        close();
        return false;
    }

    if (impl_->inputChannels > 0U) {
        static_cast<void>(impl_->component->activateBus(
            Steinberg::Vst::kAudio,
            Steinberg::Vst::kInput,
            0,
            true
        ));
    }
    static_cast<void>(impl_->component->activateBus(
        Steinberg::Vst::kAudio,
        Steinberg::Vst::kOutput,
        0,
        true
    ));

    Steinberg::Vst::ProcessSetup setup {};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock =
        static_cast<Steinberg::int32>(maximumFrames);
    setup.sampleRate = sampleRate;
    if (impl_->processor->setupProcessing(setup) != Steinberg::kResultOk) {
        error = "the plugin rejected the processing setup";
        close();
        return false;
    }

    if (impl_->component->setActive(true) != Steinberg::kResultOk) {
        error = "the plugin refused to activate";
        close();
        return false;
    }
    impl_->active = true;
    static_cast<void>(impl_->processor->setProcessing(true));
    impl_->processing = true;

    // Every buffer the real-time path needs is allocated here, so
    // process() can promise to allocate nothing.
    impl_->maximumFrames = maximumFrames;
    impl_->bridgeChannels = channelCount;
    impl_->inputStorage.assign(
        std::max<std::size_t>(impl_->inputChannels, 1U),
        std::vector<float>(maximumFrames, 0.0F)
    );
    impl_->outputStorage.assign(
        std::max<std::size_t>(impl_->outputChannels, 1U),
        std::vector<float>(maximumFrames, 0.0F)
    );
    impl_->inputPointers.clear();
    for (auto& channel : impl_->inputStorage) {
        impl_->inputPointers.push_back(channel.data());
    }
    impl_->outputPointers.clear();
    for (auto& channel : impl_->outputStorage) {
        impl_->outputPointers.push_back(channel.data());
    }

    impl_->opened = true;
    return true;
}

void Vst3Host::close() noexcept {
    if (impl_->processor != nullptr) {
        if (impl_->processing) {
            static_cast<void>(impl_->processor->setProcessing(false));
            impl_->processing = false;
        }
        impl_->processor->release();
        impl_->processor = nullptr;
    }
    // A separate controller instance is this host's own object and must
    // be torn down; a controller that is the same object as the
    // component is released once, below, by the component's own release.
    if (impl_->controller != nullptr && !impl_->controllerIsComponent) {
        static_cast<void>(impl_->controller->terminate());
        impl_->controller->release();
    }
    impl_->controller = nullptr;
    impl_->controllerIsComponent = false;
    if (impl_->component != nullptr) {
        if (impl_->active) {
            static_cast<void>(impl_->component->setActive(false));
            impl_->active = false;
        }
        static_cast<void>(impl_->component->terminate());
        impl_->component->release();
        impl_->component = nullptr;
    }
    impl_->pendingParameterCount = 0U;
    // The module itself is deliberately left loaded. A plugin that
    // misbehaves in its unload path would turn a clean shutdown into a
    // crash, and this process is short-lived by design.
    impl_->opened = false;
}

bool Vst3Host::process(
    const float* const interleavedInput,
    float* const interleavedOutput,
    const std::uint32_t frameCount
) noexcept {
    if (!impl_->opened
        || frameCount == 0U
        || frameCount > impl_->maximumFrames
        || interleavedInput == nullptr
        || interleavedOutput == nullptr) {
        return false;
    }

    const auto bridgeChannels = impl_->bridgeChannels;
    for (std::uint32_t channel = 0U;
        channel < impl_->inputChannels;
        ++channel) {
        auto* const destination = impl_->inputPointers[channel];
        if (channel < bridgeChannels) {
            for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
                destination[frame] =
                    interleavedInput[frame * bridgeChannels + channel];
            }
        } else {
            // The plugin wants more channels than the bridge carries.
            // Silence is the only honest filler.
            std::fill_n(destination, frameCount, 0.0F);
        }
    }

    Steinberg::Vst::AudioBusBuffers inputBus {};
    inputBus.numChannels =
        static_cast<Steinberg::int32>(impl_->inputChannels);
    inputBus.silenceFlags = 0U;
    inputBus.channelBuffers32 = impl_->inputPointers.data();

    Steinberg::Vst::AudioBusBuffers outputBus {};
    outputBus.numChannels =
        static_cast<Steinberg::int32>(impl_->outputChannels);
    outputBus.silenceFlags = 0U;
    outputBus.channelBuffers32 = impl_->outputPointers.data();

    // Built fresh each call over storage owned by impl_: no allocation,
    // and it presents exactly the parameters that changed since the
    // previous process() call — automation delivered the way a real host
    // delivers it, not by writing into the plugin's state directly.
    ParameterChanges inputParameterChanges {
        std::span<ParameterChangeQueue> {impl_->pendingParameterQueues}
    };
    inputParameterChanges.setActiveCount(impl_->pendingParameterCount);

    Steinberg::Vst::ProcessData data {};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = static_cast<Steinberg::int32>(frameCount);
    data.numInputs = impl_->inputChannels > 0U ? 1 : 0;
    data.numOutputs = 1;
    data.inputs = impl_->inputChannels > 0U ? &inputBus : nullptr;
    data.outputs = &outputBus;
    data.inputParameterChanges = impl_->pendingParameterCount > 0U
        ? &inputParameterChanges
        : nullptr;

    if (impl_->processor->process(data) != Steinberg::kResultOk) {
        return false;
    }
    impl_->pendingParameterCount = 0U;

    for (std::uint32_t channel = 0U; channel < bridgeChannels; ++channel) {
        const auto source = channel < impl_->outputChannels
            ? channel
            : impl_->outputChannels - 1U;
        const auto* const values = impl_->outputPointers[source];
        for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
            interleavedOutput[frame * bridgeChannels + channel] =
                values[frame];
        }
    }
    return true;
}

bool Vst3Host::saveState(std::vector<std::byte>& state) {
    state.clear();
    if (!impl_->opened) {
        return false;
    }
    MemoryStream stream;
    if (impl_->component->getState(&stream) != Steinberg::kResultOk) {
        return false;
    }
    state = stream.bytes();
    return true;
}

bool Vst3Host::loadState(const std::span<const std::byte> state) {
    if (!impl_->opened) {
        return false;
    }
    MemoryStream stream {
        std::vector<std::byte> {state.begin(), state.end()},
    };
    Steinberg::int64 ignored = 0;
    static_cast<void>(
        stream.seek(0, Steinberg::IBStream::kIBSeekSet, &ignored)
    );
    return impl_->component->setState(&stream) == Steinberg::kResultOk;
}

bool Vst3Host::parameterInfo(
    const std::uint32_t index,
    Vst3ParameterInfo& info
) const {
    if (!impl_->opened || impl_->controller == nullptr) {
        return false;
    }
    if (index
        >= static_cast<std::uint32_t>(impl_->controller->getParameterCount())) {
        return false;
    }
    Steinberg::Vst::ParameterInfo raw {};
    if (impl_->controller->getParameterInfo(
            static_cast<Steinberg::int32>(index),
            raw
        ) != Steinberg::kResultOk) {
        return false;
    }
    info.id = static_cast<std::uint32_t>(raw.id);
    info.defaultNormalizedValue =
        static_cast<float>(raw.defaultNormalizedValue);
    // Best-effort ASCII: VST3 titles are UTF-16, and this host has no
    // general Unicode transcoder. A non-ASCII title degrades to '?'
    // rather than being silently truncated at the first such character.
    info.title.clear();
    for (std::size_t index2 = 0U;
        index2 < 128U && raw.title[index2] != 0;
        ++index2) {
        const auto character = raw.title[index2];
        info.title.push_back(
            character < 128 ? static_cast<char>(character) : '?'
        );
    }
    return true;
}

bool Vst3Host::setParameterNormalized(
    const std::uint32_t parameterId,
    const float value
) noexcept {
    if (!impl_->opened || impl_->controller == nullptr) {
        return false;
    }
    const auto id = static_cast<Steinberg::Vst::ParamID>(parameterId);
    const auto clamped = std::clamp(value, 0.0F, 1.0F);

    // The controller's own value follows immediately, exactly as a real
    // host keeps its UI in sync without waiting for the next block.
    if (impl_->controller->setParamNormalized(id, clamped)
        != Steinberg::kResultOk) {
        return false;
    }

    for (std::size_t index = 0U;
        index < impl_->pendingParameterCount;
        ++index) {
        if (impl_->pendingParameterQueues[index].getParameterId() == id) {
            impl_->pendingParameterQueues[index].reset(id, clamped);
            return true;
        }
    }
    if (impl_->pendingParameterCount
        >= Impl::kMaxPendingParameters) {
        // Bounded: keep the newest values rather than grow. The
        // controller already has the authoritative value from the call
        // above; only this block's delivery of the dropped-oldest
        // parameter is lost, and it will be delivered on its own next
        // change.
        for (std::size_t index = 1U;
            index < impl_->pendingParameterCount;
            ++index) {
            impl_->pendingParameterQueues[index - 1U] =
                impl_->pendingParameterQueues[index];
        }
        --impl_->pendingParameterCount;
    }
    impl_->pendingParameterQueues[impl_->pendingParameterCount]
        .reset(id, clamped);
    ++impl_->pendingParameterCount;
    return true;
}

#endif

} // namespace iramix::plugin
