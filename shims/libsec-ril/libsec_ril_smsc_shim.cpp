// SPDX-License-Identifier: Apache-2.0
//
// Wrap Samsung libsec-ril.so and decode the non-standard SMSC address response
// returned by the modem.

#include <dlfcn.h>
#include <string.h>
#include <mutex>
#include <unordered_map>
#include <string>

#include <log/log.h>
#include <telephony/ril.h>

#ifndef REAL_LIB_NAME
#error "REAL_LIB_NAME must be defined by Android.bp, e.g. libsec-ril-impl.so"
#endif

#ifndef RIL_REQUEST_GET_SMSC_ADDRESS
#define RIL_REQUEST_GET_SMSC_ADDRESS 100
#endif

static const char* kRealPath = "/vendor/lib64/" REAL_LIB_NAME;

static void* gRealHandle = nullptr;
static const RIL_RadioFunctions* (*gReal_RIL_Init)(const struct RIL_Env*, int, char**) = nullptr;

// Samsung's onRequest has a 5th parameter for slotId/socketId
static void (*gReal_onRequest_1)(int, void*, size_t, RIL_Token, int) = nullptr;
static void (*gReal_onRequest_2)(int, void*, size_t, RIL_Token, int) = nullptr;

static struct RIL_Env gShimEnv;
static const struct RIL_Env* gRealEnv = nullptr;

static std::mutex gTokenMapMutex;
static std::unordered_map<RIL_Token, int> gTokenRequestMap;

static int parseHexByte(const std::string& str, size_t start) {
    if (start + 2 > str.length()) return -1;
    char c1 = str[start];
    char c2 = str[start + 1];
    int val1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') :
               (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) :
               (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : -1;
    int val2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') :
               (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) :
               (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : -1;
    if (val1 == -1 || val2 == -1) return -1;
    return (val1 << 4) | val2;
}

static bool isHexString(const std::string& str) {
    if (str.length() < 12) return false;
    for (char c : str) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool isDecimalString(const std::string& str) {
    if (str.length() < 10) return false;
    for (char c : str) {
        if (!(c >= '0' && c <= '9')) {
            return false;
        }
    }
    return true;
}

// Helper to BCD-decode the hex SMSC string back to a phone number
static std::string decodeBcdSmsc(const std::string& pdu) {
    if (pdu.length() < 4) return "";

    int len = parseHexByte(pdu, 0);
    if (len < 0) return "";

    if (pdu.length() < static_cast<size_t>(2 + len * 2)) return "";

    int toa = parseHexByte(pdu, 2);
    if (toa < 0) return "";

    std::string num = "";
    for (size_t i = 4; i < static_cast<size_t>(2 + len * 2); i += 2) {
        if (i + 1 >= pdu.length()) break;
        char c1 = pdu[i + 1];
        char c2 = pdu[i];
        if (c1 != 'F' && c1 != 'f') num += c1;
        if (c2 != 'F' && c2 != 'f') num += c2;
    }

    if ((toa & 0xF0) == 0x90) {
        num = "+" + num;
    }
    return num;
}

// Parses and cleans the SMSC string matching Java framework rules
static std::string fixSmscAddress(const std::string& orig_smsc) {
    std::string smsc = orig_smsc;

    // 1. Quoted format check: split and get contents of quotes
    if (smsc.find('"') != std::string::npos || smsc.find(',') != std::string::npos) {
        size_t first_quote = smsc.find('"');
        if (first_quote != std::string::npos) {
            size_t second_quote = smsc.find('"', first_quote + 1);
            if (second_quote != std::string::npos) {
                smsc = smsc.substr(first_quote + 1, second_quote - first_quote - 1);
            }
        }
    }

    // 2. BCD Hex PDU format check
    if (isHexString(smsc) && smsc.rfind("07", 0) == 0) {
        std::string decoded = decodeBcdSmsc(smsc);
        if (!decoded.empty()) {
            smsc = decoded;
        }
    }
    // 3. Plain phone number missing '+' check
    else {
        if (isDecimalString(smsc) && smsc.rfind("+", 0) != 0) {
            smsc = "+" + smsc;
        }
    }

    return smsc;
}

static void Shim_OnRequestComplete(RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
    int request = -1;
    {
        std::lock_guard<std::mutex> lock(gTokenMapMutex);
        auto it = gTokenRequestMap.find(t);
        if (it != gTokenRequestMap.end()) {
            request = it->second;
            gTokenRequestMap.erase(it);
        }
    }

    if (request == RIL_REQUEST_GET_SMSC_ADDRESS && e == RIL_E_SUCCESS && response != nullptr && responselen > 0) {
        const char* original_smsc = static_cast<const char*>(response);
        std::string fixed_smsc = fixSmscAddress(original_smsc);

        ALOGI("sec-ril-smsc-shim: GET_SMSC_ADDRESS response intercepted. Original: %s -> Fixed: %s",
              original_smsc, fixed_smsc.c_str());

        gRealEnv->OnRequestComplete(t, e, const_cast<char*>(fixed_smsc.c_str()), fixed_smsc.length() + 1);
        return;
    }

    gRealEnv->OnRequestComplete(t, e, response, responselen);
}

static void Shim_onRequest_1(int request, void* data, size_t datalen, RIL_Token token, int slotId) {
    {
        std::lock_guard<std::mutex> lock(gTokenMapMutex);
        gTokenRequestMap[token] = request;
    }
    gReal_onRequest_1(request, data, datalen, token, slotId);
}

static void Shim_onRequest_2(int request, void* data, size_t datalen, RIL_Token token, int slotId) {
    {
        std::lock_guard<std::mutex> lock(gTokenMapMutex);
        gTokenRequestMap[token] = request;
    }
    gReal_onRequest_2(request, data, datalen, token, slotId);
}

extern "C"
const RIL_RadioFunctions* RIL_Init(const struct RIL_Env* env, int argc, char** argv) {
    ALOGI("sec-ril-smsc-shim: loading real RIL from %s", kRealPath);

    gRealHandle = dlopen(kRealPath, RTLD_NOW);
    if (gRealHandle == nullptr) {
        ALOGE("sec-ril-smsc-shim: dlopen(%s) failed: %s", kRealPath, dlerror());
        return nullptr;
    }

    gReal_RIL_Init = reinterpret_cast<decltype(gReal_RIL_Init)>(dlsym(gRealHandle, "RIL_Init"));
    if (gReal_RIL_Init == nullptr) {
        ALOGE("sec-ril-smsc-shim: dlsym(RIL_Init) failed: %s", dlerror());
        return nullptr;
    }

    gRealEnv = env;
    gShimEnv = *env;
    gShimEnv.OnRequestComplete = Shim_OnRequestComplete;

    const RIL_RadioFunctions* real = gReal_RIL_Init(&gShimEnv, argc, argv);
    if (real == nullptr) {
        ALOGE("sec-ril-smsc-shim: real RIL_Init returned null");
        return nullptr;
    }

    // Modify the real RIL_RadioFunctions array in-place to preserve
    // the structure size (56 bytes on Samsung RIL instead of standard 48 bytes)
    // and avoid stack/global overflow when accessed by libril_sem.so
    uintptr_t real_addr = reinterpret_cast<uintptr_t>(real);
    
    // Slot 0 (offset 0)
    RIL_RadioFunctions* slot0 = reinterpret_cast<RIL_RadioFunctions*>(real_addr);
    gReal_onRequest_1 = reinterpret_cast<decltype(gReal_onRequest_1)>(slot0->onRequest);
    slot0->onRequest = reinterpret_cast<RIL_RequestFunc>(Shim_onRequest_1);
    ALOGI("sec-ril-smsc-shim: installed onRequest hook for Slot 0");

    // Slot 1 (offset 56 bytes)
    int* version_slot1 = reinterpret_cast<int*>(real_addr + 56);
    if (*version_slot1 == slot0->version) {
        RIL_RadioFunctions* slot1 = reinterpret_cast<RIL_RadioFunctions*>(real_addr + 56);
        gReal_onRequest_2 = reinterpret_cast<decltype(gReal_onRequest_2)>(slot1->onRequest);
        slot1->onRequest = reinterpret_cast<RIL_RequestFunc>(Shim_onRequest_2);
        ALOGI("sec-ril-smsc-shim: installed onRequest hook for Slot 1");
    } else {
        ALOGW("sec-ril-smsc-shim: Slot 1 not found or version mismatch at offset 56 (found %d, expected %d)",
              *version_slot1, slot0->version);
    }

    return real; // Return the original pointer so libril_sem receives the original 56-byte structures!
}
