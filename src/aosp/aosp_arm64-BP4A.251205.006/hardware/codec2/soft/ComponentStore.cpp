#include "ComponentStore.h"

#include "ComponentInterface.h"
#include "Configurable.h"
#include "MesaAvcComponent.h"
#include "mesa_pipe_video.h"

#include <aidl/android/hardware/media/c2/Status.h>
#include <aidl/android/hardware/media/c2/BnInputSurface.h>
#include <aidl/android/view/Surface.h>

#include <android/log.h>

#include <chrono>
#include <thread>

#define LOG_TAG "mesa-c2-store"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using aidl::android::hardware::media::c2::IComponent;
using aidl::android::hardware::media::c2::IComponentInterface;
using aidl::android::hardware::media::c2::IComponentListener;
using aidl::android::hardware::media::c2::IComponentStore;
using aidl::android::hardware::media::c2::IConfigurable;
using aidl::android::hardware::media::c2::IInputSurface;
using aidl::android::hardware::media::c2::FieldDescriptor;
using aidl::android::hardware::media::c2::FieldId;
using aidl::android::hardware::media::c2::StructDescriptor;
using aidl::android::hardware::media::c2::Status;
using aidl::android::hardware::media::bufferpool2::IClientManager;
using ndk::ScopedAStatus;
using ndk::SharedRefBase;

namespace gki {

namespace {

class MesaInputSurface final : public aidl::android::hardware::media::c2::BnInputSurface {
public:
    ndk::ScopedAStatus getConfigurable(
            std::shared_ptr<aidl::android::hardware::media::c2::IConfigurable>* out) override {
        *out = nullptr;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getSurface(aidl::android::view::Surface* out) override {
        // The current component consumes linear AIDL blocks. Keep the input
        // surface object valid so MediaCodec can negotiate the encoder; a
        // non-zero native Surface is supplied only by a full BufferQueue path.
        out->nativeHandle = 0;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus connect(
            const std::shared_ptr<aidl::android::hardware::media::c2::IInputSink>&,
            std::shared_ptr<aidl::android::hardware::media::c2::IInputSurfaceConnection>*)
            override {
        return ndk::ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
    }
};

constexpr const char* kDecName = "c2.mesa.avc.decoder";
constexpr const char* kEncName = "c2.mesa.avc.encoder";
constexpr const char* kMime = "video/avc";

IComponentStore::ComponentTraits makeTraits(const char* name, IComponentStore::ComponentTraits::Kind kind,
                                            int rank) {
    IComponentStore::ComponentTraits t;
    t.name = name;
    t.domain = IComponentStore::ComponentTraits::Domain::VIDEO;
    t.kind = kind;
    t.rank = rank;
    t.mediaType = kMime;
    t.aliases = {};
    return t;
}

}  // namespace

ComponentStore::ComponentStore() {
    // init starts this HAL alongside the graphics stack. Give renderD128 and
    // the guest VirGL capset a short window to appear before cataloguing.
    const mesa_video::Caps* capsPtr = nullptr;
    for (int attempt = 0; attempt < 10; ++attempt) {
        capsPtr = &mesa_video::queryCaps();
        if (capsPtr->avc_decode || capsPtr->avc_encode) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto& caps = *capsPtr;
    ALOGI("VirGL caps: avc_decode=%d avc_encode=%d max=%ux%u",
          caps.avc_decode ? 1 : 0, caps.avc_encode ? 1 : 0,
          caps.max_width, caps.max_height);
    // A capset is only an advertisement. Publish a component after the
    // Gallium pipe-video bridge reports that it can create a real codec.
    if (caps.avc_decode && mesa_video::backendAvailable(mesa_video::Entrypoint::kDecode)) {
        ComponentDesc d;
        d.traits = makeTraits(kDecName, IComponentStore::ComponentTraits::Kind::DECODER, 16);
        d.kind = MesaCodecKind::kDecoder;
        mCatalog[kDecName] = d;
    }
    // Probe the real Gallium encoder even if the capset query raced guest
    // device initialization; backendAvailable is the authoritative gate.
    if (mesa_video::backendAvailable(mesa_video::Entrypoint::kEncode)) {
        ComponentDesc d;
        d.traits = makeTraits(kEncName, IComponentStore::ComponentTraits::Kind::ENCODER, 16);
        d.kind = MesaCodecKind::kEncoder;
        mCatalog[kEncName] = d;
    }
    ALOGI("catalog size=%zu (caps dec=%d enc=%d)", mCatalog.size(),
          caps.avc_decode ? 1 : 0, caps.avc_encode ? 1 : 0);
}

std::shared_ptr<Configurable> ComponentStore::storeConfigurable() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mStoreCfg) {
        mStoreCfg = SharedRefBase::make<Configurable>(0, "default");
    }
    return mStoreCfg;
}

std::shared_ptr<Configurable> ComponentStore::interfaceFor(const std::string& name) {
    std::lock_guard<std::mutex> lock(mLock);
    auto it = mInterfaces.find(name);
    if (it != mInterfaces.end()) return it->second;
    int32_t id = mNextId++;
    auto cfg = SharedRefBase::make<Configurable>(id, name);
    mInterfaces[name] = cfg;
    return cfg;
}

ScopedAStatus ComponentStore::listComponents(
        std::vector<IComponentStore::ComponentTraits>* out) {
    out->clear();
    for (const auto& [name, desc] : mCatalog) {
        (void)name;
        out->push_back(desc.traits);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::createInterface(const std::string& name,
                                            std::shared_ptr<IComponentInterface>* out) {
    if (mCatalog.find(name) == mCatalog.end()) {
        return ScopedAStatus::fromServiceSpecificError(Status::NOT_FOUND);
    }
    auto cfg = interfaceFor(name);
    *out = SharedRefBase::make<ComponentInterface>(cfg->id(), name, cfg);
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::createComponent(
        const std::string& name, const std::shared_ptr<IComponentListener>& listener,
        const std::shared_ptr<IClientManager>& /*pool*/, std::shared_ptr<IComponent>* out) {
    auto it = mCatalog.find(name);
    if (it == mCatalog.end()) {
        return ScopedAStatus::fromServiceSpecificError(Status::NOT_FOUND);
    }
    auto cfg = interfaceFor(name);
    auto intf = SharedRefBase::make<ComponentInterface>(cfg->id(), name, cfg);
    auto comp = SharedRefBase::make<MesaAvcComponent>(it->second.kind, name, intf, cfg, listener);
    *out = comp;
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::getConfigurable(std::shared_ptr<IConfigurable>* out) {
    *out = storeConfigurable();
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::getPoolClientManager(std::shared_ptr<IClientManager>* out) {
    *out = nullptr;
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::copyBuffer(
        const aidl::android::hardware::media::c2::Buffer& /*src*/,
        const aidl::android::hardware::media::c2::Buffer& /*dst*/) {
    return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
}

ScopedAStatus ComponentStore::getStructDescriptors(
        const std::vector<int32_t>& indices,
        std::vector<aidl::android::hardware::media::c2::StructDescriptor>* out) {
    out->clear();
    // CCodecConfig needs the standard raw.size layout to translate the
    // MediaFormat width/height into C2StreamPictureSizeInfo.  The rest of the
    // component uses fixed, framework-defined layouts and can be reflected by
    // the platform.
    bool requested = indices.empty();
    for (int32_t index : indices) {
        if ((static_cast<uint32_t>(index) & 0x1ffffu) == 0x1800u) {
            requested = true;
            break;
        }
    }
    if (requested) {
        StructDescriptor d;
        d.type = 0x1800;
        FieldDescriptor width;
        // StructDescriptor field offsets are relative to the payload struct;
        // the framework adds sizeof(C2Param) when applying them to a param.
        width.fieldId = FieldId{0, 4};
        width.type = FieldDescriptor::Type::UINT32;
        width.extent = 1;
        width.name = "width";
        FieldDescriptor height;
        height.fieldId = FieldId{4, 4};
        height.type = FieldDescriptor::Type::UINT32;
        height.extent = 1;
        height.name = "height";
        d.fields = {std::move(width), std::move(height)};
        out->push_back(std::move(d));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::createInputSurface(std::shared_ptr<IInputSurface>* out) {
    *out = nullptr;
    return ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
}

}  // namespace gki
