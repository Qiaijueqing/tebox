// Samsung OneUI radio extension HALs for the soft radio.
//
// OneUI's SemRIL sends getIccCardStatus (and a few other SIM/data/SMS
// requests) only through vendor.samsung.hardware.radio.*.ISehRadio*/slotN;
// without them UiccController sees RADIO_NOT_AVAILABLE and the SIMs never
// load. These are hand-written binder classes for the Samsung AIDL v1
// interfaces: getIccCardStatus reports the soft SIM, void requests succeed and
// everything else answers REQUEST_NOT_SUPPORTED with a default payload (some
// Java response handlers read the payload before checking the error).
// OneUI's status bar draws signal bars only from SignalBarInfo, which comes
// solely from ISehRadioNetworkIndication.signalLevelInfoChanged; the network
// service pushes it periodically once telephony has registered.
//
// Transaction codes and parcel layouts come from the Java stubs in the GSI's
// framework.jar. Request N is answered on the response interface's code for
// "<request>Response", which is not always N.
#include "seh_radio.h"

#include <android/binder_auto_utils.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <android/binder_stability.h>
#include <android/binder_status.h>
#include <android/log.h>

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "qemu-radio", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "qemu-radio", __VA_ARGS__)

namespace {

constexpr transaction_code_t kGetInterfaceVersion = 16777215;
constexpr transaction_code_t kGetInterfaceHash = 16777214;
constexpr int32_t kInterfaceVersion = 1;

// SehRadioResponseType / SehRadioError.
constexpr int32_t kSolicited = 0;
constexpr int32_t kErrorNone = 0;
constexpr int32_t kErrorRequestNotSupported = 6;
// RadioIndicationType UNSOLICITED.
constexpr int32_t kUnsolicited = 0;

constexpr const char* kNetworkIndication =
        "vendor.samsung.hardware.radio.network.ISehRadioNetworkIndication";
constexpr transaction_code_t kSignalLevelInfoChanged = 14;

struct SehCall {
    transaction_code_t request;
    transaction_code_t response;
    // Response arguments after SehRadioResponseInfo: I int, S String,
    // A array, P parcelable. Empty means a void response, answered with NONE.
    const char* payload;
};

struct SehInterface {
    const char* descriptor;
    const char* responseDescriptor;
    const char* hash;
    transaction_code_t setResponseFunctions;
    const SehCall* calls;
    size_t callCount;
};

const SehCall kSimCalls[] = {
        {1, 1, "P"},    // getIccCardStatus
        {2, 2, "I"},    // supplyNetworkDepersonalization
        {3, 3, "P"},    // getPhonebookStorageInfo
        {4, 4, "A"},    // getUsimPhonebookCapability
        {5, 5, ""},     // setSimOnOff
        {6, 6, ""},     // setSimInitEvent
        {7, 7, "P"},    // getSimLockInfo
        {8, 8, ""},     // supplyIccPersonalization
        {9, 9, ""},     // changeIccPersonalization
        {10, 10, "P"},  // getPhonebookEntry
        {11, 11, "I"},  // accessPhonebookEntry
        {12, 12, "S"},  // getAtr
};

const SehCall kNetworkCalls[] = {
        {1, 1, ""},      // emergencyControl
        {2, 2, "I"},     // emergencySearch
        {3, 3, "A"},     // getAvailableNetworks
        {4, 4, "I"},     // getCnap
        {5, 5, "A"},     // getCsgList
        {6, 6, "I"},     // getDisable2g
        {7, 7, "I"},     // getNrIconType
        {8, 8, "I"},     // getNrMode
        {9, 9, "A"},     // getPreferredNetworkList
        {10, 10, "A"},   // getRoamingNetworkInfoViaBLE
        {11, 11, "A"},   // getVendorSpecificConfiguration
        {12, 12, ""},    // selectCsgManual
        {13, 13, ""},    // sendEncodedUssd
        {14, 14, "A"},   // sendRequestRaw
        {15, 15, "A"},   // sendRequestStrings
        {16, 16, ""},    // setCurrentNetworkInfoViaBLE
        {17, 17, ""},    // setDisable2g
        {18, 18, ""},    // setImsCallList
        {19, 19, ""},    // setNrMode
        {20, 20, ""},    // setPreferredNetworkList
        {22, 21, ""},    // setRoamingNetworkInfoViaBLE
        {23, 22, ""},    // setScanResultViaBLE
        {24, 23, ""},    // setVendorSpecificConfiguration
        {25, 24, "IIA"}, // getCpaiModelVersion
        {26, 25, ""},    // execCpaiModelUpdate
        {27, 26, "IIA"}, // getCpaiFeatureInfo
        {28, 27, ""},    // cfrmCpaiFeatureInfo
        {29, 28, ""},    // setCpaiDataGathering
        {30, 29, ""},    // evtCpaiDataGathering
        {31, 30, ""},    // setCpaiDevAppMessage
};

const SehCall kDataCalls[] = {
        {1, 1, ""},  // setDataAllowed
        {2, 2, ""},  // setMobileDataSetting
};

const SehCall kMessagingCalls[] = {
        {1, 8, "P"},   // getCellBroadcastConfig
        {2, 9, "A"},   // getImsRegistrationState
        {3, 5, "P"},   // getStoredMsgCountFromSim
        {4, 6, "P"},   // readSmsFromSim
        {5, 3, "P"},   // sendCdmaSms
        {6, 4, "P"},   // sendCdmaSmsExpectMore
        {7, 2, "P"},   // sendSMSExpectMore
        {8, 1, "P"},   // sendSms
        {10, 7, "I"},  // writeSmsToSim
};

template <size_t N>
constexpr size_t countOf(const SehCall (&)[N]) {
    return N;
}

const SehInterface kSim = {
        "vendor.samsung.hardware.radio.sim.ISehRadioSim",
        "vendor.samsung.hardware.radio.sim.ISehRadioSimResponse",
        "e94363ef2041c98feabe84e756d983ad196394e8", 13, kSimCalls, countOf(kSimCalls)};
const SehInterface kNetwork = {
        "vendor.samsung.hardware.radio.network.ISehRadioNetwork",
        "vendor.samsung.hardware.radio.network.ISehRadioNetworkResponse",
        "0fc15a1faf82e2c79f263a7fd6e8b5ffba3644f3", 21, kNetworkCalls, countOf(kNetworkCalls)};
const SehInterface kData = {
        "vendor.samsung.hardware.radio.data.ISehRadioData",
        "vendor.samsung.hardware.radio.data.ISehRadioDataResponse",
        "1c18f89373d68cf0030dbdb95f4a9287fe232a2e", 3, kDataCalls, countOf(kDataCalls)};
const SehInterface kMessaging = {
        "vendor.samsung.hardware.radio.messaging.ISehRadioMessaging",
        "vendor.samsung.hardware.radio.messaging.ISehRadioMessagingResponse",
        "c1a8596db57e3bcc8e4e86f1eb7b2df7839ca140", 9, kMessagingCalls,
        countOf(kMessagingCalls)};

struct SehService {
    const SehInterface* iface;
    AIBinder_Class* responseClass;
    AIBinder_Class* indicationClass;  // network only
    int slot;
    SehSlot card;
    std::mutex lock;
    ndk::SpAIBinder response;
    ndk::SpAIBinder indication;
    bool signalLoop = false;
};

// Java writeTypedObject: non-null marker, then a size-prefixed parcelable.
template <class Body>
void writeParcelable(AParcel* p, Body body) {
    AParcel_writeInt32(p, 1);
    const int32_t start = AParcel_getDataPosition(p);
    AParcel_writeInt32(p, 0);
    body();
    const int32_t end = AParcel_getDataPosition(p);
    AParcel_setDataPosition(p, start);
    AParcel_writeInt32(p, end - start);
    AParcel_setDataPosition(p, end);
}

void writeString(AParcel* p, const char* s) {
    AParcel_writeString(p, s, static_cast<int32_t>(strlen(s)));
}

// SehCardStatus with one ready USIM; values are the RIL constants.
void writeCardStatus(AParcel* p, int slot, const SehSlot& card) {
    writeParcelable(p, [&] {
        AParcel_writeInt32(p, 1);   // cardState PRESENT
        AParcel_writeInt32(p, 3);   // universalPinState DISABLED
        AParcel_writeInt32(p, 0);   // gsmUmtsSubscriptionAppIndex
        AParcel_writeInt32(p, -1);  // cdmaSubscriptionAppIndex
        AParcel_writeInt32(p, -1);  // imsSubscriptionAppIndex
        AParcel_writeInt32(p, 1);   // applications[1]
        writeParcelable(p, [&] {
            AParcel_writeInt32(p, 2);  // appType USIM
            AParcel_writeInt32(p, 5);  // appState READY
            AParcel_writeInt32(p, 2);  // persoSubstate READY
            writeString(p, "A0000000871002");
            writeString(p, "USIM");
            AParcel_writeInt32(p, 0);   // pin1Replaced
            AParcel_writeInt32(p, 3);   // pin1 DISABLED
            AParcel_writeInt32(p, 3);   // pin2 DISABLED
            AParcel_writeInt32(p, 3);   // pin1NumRetries
            AParcel_writeInt32(p, 10);  // puk1NumRetries
            AParcel_writeInt32(p, 3);   // pin2NumRetries
            AParcel_writeInt32(p, 10);  // puk2NumRetries
            AParcel_writeInt32(p, 0);   // persoUnblockRetries
        });
        writeString(p, "");  // atr
        writeString(p, card.iccid);
        writeString(p, card.eid);
        writeParcelable(p, [&] {  // slotMap
            AParcel_writeInt32(p, slot - 1);
            AParcel_writeInt32(p, 0);
        });
        AParcel_writeInt32(p, 0);  // supportedMepMode
    });
}

void writeDefaultPayload(AParcel* p, const char* payload) {
    for (const char* k = payload; *k; ++k) {
        switch (*k) {
            case 'I':
            case 'A':  // zero-length array
                AParcel_writeInt32(p, 0);
                break;
            case 'S':
                writeString(p, "");
                break;
            case 'P':  // every field left at its default
                writeParcelable(p, [] {});
                break;
        }
    }
}

void respond(SehService* svc, const SehCall& call, int32_t serial) {
    ndk::SpAIBinder response;
    {
        std::lock_guard<std::mutex> guard(svc->lock);
        response = svc->response;
    }
    if (response.get() == nullptr) {
        ALOGW("%s/slot%d: request %u before setResponseFunctions", svc->iface->descriptor,
              svc->slot, call.request);
        return;
    }
    const bool cardStatus = svc->iface == &kSim && call.request == 1;
    const int32_t error =
            (cardStatus || call.payload[0] == '\0') ? kErrorNone : kErrorRequestNotSupported;

    AParcel* in = nullptr;
    if (AIBinder_prepareTransaction(response.get(), &in) != STATUS_OK) return;
    writeParcelable(in, [&] {  // SehRadioResponseInfo
        AParcel_writeInt32(in, kSolicited);
        AParcel_writeInt32(in, serial);
        AParcel_writeInt32(in, error);
    });
    if (cardStatus) {
        writeCardStatus(in, svc->slot, svc->card);
    } else {
        writeDefaultPayload(in, call.payload);
    }
    AParcel* out = nullptr;
    const binder_status_t status =
            AIBinder_transact(response.get(), call.response, &in, &out, FLAG_ONEWAY);
    if (out != nullptr) AParcel_delete(out);
    if (status != STATUS_OK) {
        ALOGW("%s/slot%d: response %u failed: %d", svc->iface->descriptor, svc->slot,
              call.response, status);
    }
}

// ISehRadioNetworkIndication.signalLevelInfoChanged(UNSOLICITED, SehSignalBar).
void pushSignalBar(SehService* svc) {
    ndk::SpAIBinder indication;
    {
        std::lock_guard<std::mutex> guard(svc->lock);
        indication = svc->indication;
    }
    if (indication.get() == nullptr) return;
    AParcel* in = nullptr;
    if (AIBinder_prepareTransaction(indication.get(), &in) != STATUS_OK) return;
    AParcel_writeInt32(in, kUnsolicited);
    writeParcelable(in, [&] {
        AParcel_writeInt32(in, 0);  // cdmaLevel
        AParcel_writeInt32(in, 0);  // evdoLevel
        AParcel_writeInt32(in, 0);  // gsmLevel
        AParcel_writeInt32(in, 0);  // wcdmaLevel
        AParcel_writeInt32(in, 0);  // tdscdmaLevel
        AParcel_writeInt32(in, svc->card.lteLevel);
        AParcel_writeInt32(in, svc->card.nrLevel);
    });
    AParcel* out = nullptr;
    const binder_status_t status =
            AIBinder_transact(indication.get(), kSignalLevelInfoChanged, &in, &out, FLAG_ONEWAY);
    if (out != nullptr) AParcel_delete(out);
    if (status != STATUS_OK) ALOGW("slot%d: signalLevelInfoChanged failed: %d", svc->slot, status);
}

// Telephony registers its SignalBarInfo listener independently of
// setResponseFunctions and keeps only the latest value, so repeat it.
void signalLoop(SehService* svc) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    for (;;) {
        pushSignalBar(svc);
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

binder_status_t writeOk(AParcel* out) {
    AStatus* ok = AStatus_newOk();
    const binder_status_t status = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    return status;
}

void* sehOnCreate(void* args) {
    return args;
}

void sehOnDestroy(void*) {}

binder_status_t sehOnTransact(AIBinder* binder, transaction_code_t code, const AParcel* in,
                              AParcel* out) {
    auto* svc = static_cast<SehService*>(AIBinder_getUserData(binder));
    const SehInterface* iface = svc->iface;

    if (code == kGetInterfaceVersion) {
        writeOk(out);
        return AParcel_writeInt32(out, kInterfaceVersion);
    }
    if (code == kGetInterfaceHash) {
        writeOk(out);
        return AParcel_writeString(out, iface->hash, static_cast<int32_t>(strlen(iface->hash)));
    }
    if (code == iface->setResponseFunctions) {
        AIBinder* response = nullptr;
        AIBinder* indication = nullptr;
        AParcel_readStrongBinder(in, &response);
        AParcel_readStrongBinder(in, &indication);
        if (response != nullptr && !AIBinder_associateClass(response, svc->responseClass)) {
            ALOGW("%s/slot%d: response binder is not %s", iface->descriptor, svc->slot,
                  iface->responseDescriptor);
        }
        if (svc->indicationClass == nullptr) {
            if (indication != nullptr) AIBinder_decStrong(indication);
            indication = nullptr;
        } else if (indication != nullptr &&
                   !AIBinder_associateClass(indication, svc->indicationClass)) {
            ALOGW("%s/slot%d: indication binder is not %s", iface->descriptor, svc->slot,
                  kNetworkIndication);
        }
        bool startLoop = false;
        {
            std::lock_guard<std::mutex> guard(svc->lock);
            svc->response = ndk::SpAIBinder(response);
            svc->indication = ndk::SpAIBinder(indication);
            startLoop = indication != nullptr && !svc->signalLoop;
            if (startLoop) svc->signalLoop = true;
        }
        if (startLoop) std::thread(signalLoop, svc).detach();
        return writeOk(out);
    }
    for (size_t i = 0; i < iface->callCount; ++i) {
        if (iface->calls[i].request != code) continue;
        int32_t serial = 0;
        AParcel_readInt32(in, &serial);
        respond(svc, iface->calls[i], serial);
        return writeOk(out);
    }
    return STATUS_UNKNOWN_TRANSACTION;
}

// The response interfaces are only ever called; they need a class so that
// prepareTransaction writes the right interface token.
binder_status_t responseOnTransact(AIBinder*, transaction_code_t, const AParcel*, AParcel*) {
    return STATUS_UNKNOWN_TRANSACTION;
}

// Called from main() before the thread pool serves requests.
void publish(const SehInterface& iface, int slot, const SehSlot& card) {
    // One class per descriptor, shared by both slots.
    static std::map<const SehInterface*, std::pair<AIBinder_Class*, AIBinder_Class*>> classes;
    auto& entry = classes[&iface];
    if (entry.first == nullptr) {
        entry.first =
                AIBinder_Class_define(iface.descriptor, sehOnCreate, sehOnDestroy, sehOnTransact);
        entry.second = AIBinder_Class_define(iface.responseDescriptor, sehOnCreate, sehOnDestroy,
                                             responseOnTransact);
    }
    AIBinder_Class* cls = entry.first;
    AIBinder_Class* responseCls = entry.second;
    static AIBinder_Class* indicationCls = AIBinder_Class_define(
            kNetworkIndication, sehOnCreate, sehOnDestroy, responseOnTransact);
    auto* svc = new SehService;
    svc->iface = &iface;
    svc->responseClass = responseCls;
    svc->indicationClass = &iface == &kNetwork ? indicationCls : nullptr;
    svc->slot = slot;
    svc->card = card;
    AIBinder* binder = AIBinder_new(cls, svc);
    const std::string instance = std::string(iface.descriptor) + "/slot" + std::to_string(slot);
    if (AServiceManager_isDeclared(instance.c_str())) AIBinder_markVintfStability(binder);
    const binder_exception_t status = AServiceManager_addService(binder, instance.c_str());
    ALOGI("registered %s: %d", instance.c_str(), status);
    // Kept for the life of the process.
}

}  // namespace

void publishSehSlot(int slot, const SehSlot& card) {
    publish(kSim, slot, card);
    publish(kNetwork, slot, card);
    publish(kData, slot, card);
    publish(kMessaging, slot, card);
}
