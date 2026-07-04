// vst3_probe.cpp - Minimal Windows VST3 host harness (DEV-ONLY, not shipped).
//
// Purpose: reproduce the LUNA (Universal Audio) instance-creation failure from
// issue #89 by driving a VST3 module through host conditions one variable at a
// time, recording for each: return codes, whether a C++ exception escapes the
// COM boundary, hard structured exceptions (access violation / stack overflow),
// hangs, or clean soft-failures (createInstance -> null, i.e. LUNA's "mInstance = 0").
//
// This tool is intentionally NOT wired into the root CMake build or CI. It has no
// effect on plugin binaries or release artifacts. See tools/vst3-probe/CMakeLists.txt.
//
// It exercises only the minimal COM surface a host needs. To keep vtable ABI
// exactly correct it includes the VST3 SDK "pluginterfaces" headers that ship
// inside the JUCE checkout (header-only, no linking) rather than redeclaring them.
//
// Build (from a SHORT path to avoid MAX_PATH on VST3 helper builds is irrelevant
// here since this is a single console exe):
//   cmake -S tools/vst3-probe -B <builddir> -DVST3_SDK_DIR=<...>/VST3_SDK
//   cmake --build <builddir> --config Release
// or compile directly with cl.exe (see CMakeLists.txt header for the flags).
//
// Usage:
//   vst3_probe.exe --module "<path to .vst3 bundle or inner DLL>" [--all]
//       Orchestrates every scenario, each in an isolated child process, and
//       prints a result matrix.
//   vst3_probe.exe --child --case <name> --module <path> [--threads N]
//       [--samplerate SR] [--maxblock MB] [--iters K]
//       Runs a single scenario in this process (used internally by --all).
//
// Scenarios (--case):
//   good           functional host context, single (main) thread
//   refuse-msg     host IHostApplication::createInstance returns kResultFalse
//                  for IMessage / IAttributeList (LUNA "VST3_HostContext" suspect)
//   notimpl-msg    same but returns kNotImplemented
//   null-obj-msg   returns kResultOk but leaves *obj == nullptr (odd host)
//   null-ctx       initialize(nullptr) / no host context at all
//   worker-thread  full create+initialize+activate on a non-main thread
//   concurrent     N threads create+initialize+activate concurrently (default 5)
//   reload-cycle   LoadLibrary -> InitDll -> factory -> instance -> ExitDll ->
//                  FreeLibrary, repeated K times (default 8)
//   hi-sr          functional host, 192 kHz, large maxBlock (8192)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// --- VST3 SDK pluginterfaces (header-only, no link) ---------------------------
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// ============================================================================
//  Small utilities
// ============================================================================

static bool tuidEqual (const TUID a, const TUID b)
{
    return std::memcmp (a, b, sizeof (TUID)) == 0;
}

static const char* tresultName (tresult r)
{
    switch (r)
    {
        case kResultOk:        return "kResultOk";        // == kResultTrue
        case kResultFalse:     return "kResultFalse";
        case kInvalidArgument: return "kInvalidArgument";
        case kNotImplemented:  return "kNotImplemented";
        case kInternalError:   return "kInternalError";
        case kNotInitialized:  return "kNotInitialized";
        case kOutOfMemory:     return "kOutOfMemory";
        case kNoInterface:     return "kNoInterface";
        default:               return "<other>";
    }
}

static void logf (const char* fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    vprintf (fmt, ap);
    va_end (ap);
    fflush (stdout);
}

// ============================================================================
//  Structured-exception guard.
//
//  Compiled with /EHa so C++ exceptions are also delivered as SEH exceptions
//  (code 0xE06D7363). This lets a single __except distinguish a C++ exception
//  escaping the COM boundary from a hard fault (access violation 0xC0000005,
//  stack overflow 0xC00000FD, etc). The guarded function has no locals that
//  require unwinding, so C2712 does not apply; the callee's own frames unwind
//  normally within fn().
// ============================================================================

static constexpr DWORD kCppExceptionCode = 0xE06D7363u;

static bool guardedCall (const std::function<void()>& fn, DWORD& codeOut)
{
    codeOut = 0;
    __try
    {
        fn();
        return true;
    }
    __except (codeOut = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static const char* sehName (DWORD code)
{
    switch (code)
    {
        case kCppExceptionCode:              return "C++ exception (escaped COM boundary)";
        case EXCEPTION_ACCESS_VIOLATION:     return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:       return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:  return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
        default:                             return "structured exception";
    }
}

// Result accumulation for a scenario.
enum class StepOutcome { Ok = 0, SoftFail = 10, CppException = 20, HardFault = 30 };

struct Accumulator
{
    std::mutex m;
    std::atomic<int> worst { 0 }; // max StepOutcome int seen
    void note (StepOutcome o)
    {
        int v = (int) o;
        int cur = worst.load();
        while (v > cur && ! worst.compare_exchange_weak (cur, v)) {}
    }
};

// ============================================================================
//  Host-side COM object implementations (minimal).
// ============================================================================

// How the host context should behave when the plug-in asks it to create an
// IMessage / IAttributeList (the LUNA "VST3_HostContext" behaviour under test).
enum class MsgMode { Good, RefuseFalse, RefuseNotImpl, OkButNull };

static std::atomic<int> g_liveHostObjects { 0 };

// ---- IAttributeList --------------------------------------------------------
class HostAttributeList final : public IAttributeList
{
public:
    HostAttributeList() { ++g_liveHostObjects; }
    virtual ~HostAttributeList() { --g_liveHostObjects; }

    tresult PLUGIN_API queryInterface (const TUID queryIid, void** obj) override
    {
        if (tuidEqual (queryIid, IAttributeList_iid) || tuidEqual (queryIid, FUnknown_iid))
        {
            addRef();
            *obj = static_cast<IAttributeList*> (this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override  { return (uint32) ++refCount; }
    uint32 PLUGIN_API release() override
    {
        const int32 c = --refCount;
        if (c == 0) { delete this; return 0; }
        return (uint32) c;
    }

    tresult PLUGIN_API setInt (AttrID id, int64 v) override
    {
        ints[id] = v; return kResultTrue;
    }
    tresult PLUGIN_API getInt (AttrID id, int64& v) override
    {
        auto it = ints.find (id);
        if (it == ints.end()) return kResultFalse;
        v = it->second; return kResultTrue;
    }
    tresult PLUGIN_API setFloat (AttrID id, double v) override
    {
        floats[id] = v; return kResultTrue;
    }
    tresult PLUGIN_API getFloat (AttrID id, double& v) override
    {
        auto it = floats.find (id);
        if (it == floats.end()) return kResultFalse;
        v = it->second; return kResultTrue;
    }
    tresult PLUGIN_API setString (AttrID id, const TChar* s) override
    {
        std::u16string str;
        if (s != nullptr) { const char16_t* p = reinterpret_cast<const char16_t*> (s); str = p; }
        strings[id] = std::move (str);
        return kResultTrue;
    }
    tresult PLUGIN_API getString (AttrID id, TChar* s, uint32 sizeInBytes) override
    {
        auto it = strings.find (id);
        if (it == strings.end()) return kResultFalse;
        const uint32 maxChars = sizeInBytes / (uint32) sizeof (TChar);
        if (maxChars == 0) return kResultFalse;
        uint32 n = 0;
        for (; n < it->second.size() && n + 1 < maxChars; ++n)
            s[n] = (TChar) it->second[n];
        s[n] = 0;
        return kResultTrue;
    }
    tresult PLUGIN_API setBinary (AttrID id, const void* data, uint32 sizeInBytes) override
    {
        const char* p = static_cast<const char*> (data);
        blobs[id].assign (p, p + sizeInBytes);
        return kResultTrue;
    }
    tresult PLUGIN_API getBinary (AttrID id, const void*& data, uint32& sizeInBytes) override
    {
        auto it = blobs.find (id);
        if (it == blobs.end()) { data = nullptr; sizeInBytes = 0; return kResultFalse; }
        data = it->second.data();
        sizeInBytes = (uint32) it->second.size();
        return kResultTrue;
    }

private:
    std::atomic<int32> refCount { 1 };
    std::map<std::string, int64> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::u16string> strings;
    std::map<std::string, std::vector<char>> blobs;
};

// ---- IMessage --------------------------------------------------------------
class HostMessage final : public IMessage
{
public:
    HostMessage() { ++g_liveHostObjects; }
    virtual ~HostMessage() { if (attrs) attrs->release(); --g_liveHostObjects; }

    tresult PLUGIN_API queryInterface (const TUID queryIid, void** obj) override
    {
        if (tuidEqual (queryIid, IMessage_iid) || tuidEqual (queryIid, FUnknown_iid))
        {
            addRef();
            *obj = static_cast<IMessage*> (this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override  { return (uint32) ++refCount; }
    uint32 PLUGIN_API release() override
    {
        const int32 c = --refCount;
        if (c == 0) { delete this; return 0; }
        return (uint32) c;
    }

    FIDString PLUGIN_API getMessageID() override { return messageId.c_str(); }
    void PLUGIN_API setMessageID (FIDString id) override { messageId = id ? id : ""; }
    IAttributeList* PLUGIN_API getAttributes() override
    {
        if (attrs == nullptr) attrs = new HostAttributeList();
        return attrs;
    }

private:
    std::atomic<int32> refCount { 1 };
    std::string messageId;
    HostAttributeList* attrs { nullptr };
};

// ---- IComponentHandler (minimal, never rejects) ----------------------------
class HostComponentHandler final : public IComponentHandler
{
public:
    HostComponentHandler() { ++g_liveHostObjects; }
    virtual ~HostComponentHandler() { --g_liveHostObjects; }

    tresult PLUGIN_API queryInterface (const TUID queryIid, void** obj) override
    {
        if (tuidEqual (queryIid, IComponentHandler_iid) || tuidEqual (queryIid, FUnknown_iid))
        {
            addRef();
            *obj = static_cast<IComponentHandler*> (this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override  { return (uint32) ++refCount; }
    uint32 PLUGIN_API release() override
    {
        const int32 c = --refCount;
        if (c == 0) { delete this; return 0; }
        return (uint32) c;
    }

    tresult PLUGIN_API beginEdit (ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit (ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit (ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent (int32) override { return kResultOk; }

private:
    std::atomic<int32> refCount { 1 };
};

// ---- IHostApplication (+ IPlugInterfaceSupport) ----------------------------
class HostApplication final : public IHostApplication, public IPlugInterfaceSupport
{
public:
    explicit HostApplication (MsgMode mode) : msgMode (mode) { ++g_liveHostObjects; }
    virtual ~HostApplication() { --g_liveHostObjects; }

    tresult PLUGIN_API queryInterface (const TUID queryIid, void** obj) override
    {
        if (tuidEqual (queryIid, IHostApplication_iid) || tuidEqual (queryIid, FUnknown_iid))
        {
            addRef();
            *obj = static_cast<IHostApplication*> (this);
            return kResultOk;
        }
        if (tuidEqual (queryIid, IPlugInterfaceSupport_iid))
        {
            addRef();
            *obj = static_cast<IPlugInterfaceSupport*> (this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override  { return (uint32) ++refCount; }
    uint32 PLUGIN_API release() override
    {
        const int32 c = --refCount;
        if (c == 0) { delete this; return 0; }
        return (uint32) c;
    }

    tresult PLUGIN_API getName (String128 name) override
    {
        // "vst3_probe" in UTF-16.
        static const char16_t kName[] = u"vst3_probe";
        int i = 0;
        for (; kName[i] != 0 && i < 127; ++i) name[i] = (TChar) kName[i];
        name[i] = 0;
        return kResultTrue;
    }

    tresult PLUGIN_API createInstance (TUID cid, TUID _iid, void** obj) override
    {
        const bool wantsMessage = tuidEqual (cid, IMessage_iid);
        const bool wantsAttrs   = tuidEqual (cid, IAttributeList_iid);

        if (! (wantsMessage || wantsAttrs))
        {
            if (obj) *obj = nullptr;
            return kNoInterface;
        }

        switch (msgMode)
        {
            case MsgMode::RefuseFalse:
                if (obj) *obj = nullptr;
                return kResultFalse;
            case MsgMode::RefuseNotImpl:
                if (obj) *obj = nullptr;
                return kNotImplemented;
            case MsgMode::OkButNull:
                if (obj) *obj = nullptr;   // hostile: says OK but hands back null
                return kResultOk;
            case MsgMode::Good:
            default:
                break;
        }

        if (wantsMessage)
        {
            auto* m = new HostMessage();
            return m->queryInterface (_iid, obj);
        }
        auto* a = new HostAttributeList();
        return a->queryInterface (_iid, obj);
    }

    // IPlugInterfaceSupport: advertise nothing beyond the basics.
    tresult PLUGIN_API isPlugInterfaceSupported (const TUID) override { return kResultFalse; }

private:
    std::atomic<int32> refCount { 1 };
    MsgMode msgMode;
};

// ============================================================================
//  Module loading
// ============================================================================

using InitDllProc         = bool (*) ();
using ExitDllProc         = bool (*) ();
using GetPluginFactoryProc = IPluginFactory* (PLUGIN_API*) ();

struct LoadedModule
{
    HMODULE dll { nullptr };
    InitDllProc initDll { nullptr };
    ExitDllProc exitDll { nullptr };
    GetPluginFactoryProc getFactory { nullptr };
    bool ok { false };
    std::string error;
};

// Resolve a user-supplied path (bundle dir OR inner DLL) to the inner DLL path.
static std::wstring resolveModulePath (const std::wstring& in)
{
    DWORD attr = GetFileAttributesW (in.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        // Bundle: <name>.vst3/Contents/x86_64-win/<name>.vst3
        std::wstring name = in;
        while (! name.empty() && (name.back() == L'\\' || name.back() == L'/'))
            name.pop_back();
        size_t slash = name.find_last_of (L"\\/");
        std::wstring base = (slash == std::wstring::npos) ? name : name.substr (slash + 1);
        return name + L"\\Contents\\x86_64-win\\" + base;
    }
    return in;
}

static LoadedModule loadModule (const std::wstring& userPath)
{
    LoadedModule lm;
    std::wstring dllPath = resolveModulePath (userPath);

    // Let the loader find co-located DLLs in the module's own directory.
    lm.dll = LoadLibraryExW (dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (lm.dll == nullptr)
    {
        lm.error = "LoadLibraryEx failed, GetLastError=" + std::to_string (GetLastError());
        return lm;
    }
    lm.initDll    = reinterpret_cast<InitDllProc> (GetProcAddress (lm.dll, "InitDll"));
    lm.exitDll    = reinterpret_cast<ExitDllProc> (GetProcAddress (lm.dll, "ExitDll"));
    lm.getFactory = reinterpret_cast<GetPluginFactoryProc> (GetProcAddress (lm.dll, "GetPluginFactory"));

    if (lm.getFactory == nullptr)
    {
        lm.error = "GetPluginFactory export not found";
        return lm;
    }
    if (lm.initDll != nullptr)
    {
        const bool r = lm.initDll();
        if (! r) { lm.error = "InitDll returned false"; return lm; }
    }
    lm.ok = true;
    return lm;
}

static void unloadModule (LoadedModule& lm)
{
    if (lm.exitDll != nullptr) lm.exitDll();
    if (lm.dll != nullptr) FreeLibrary (lm.dll);
    lm = LoadedModule{};
}

// ============================================================================
//  Class discovery
// ============================================================================

struct ClassIds
{
    bool haveComponent { false };
    bool haveController { false };
    TUID componentCid {};
    TUID controllerCid {};
};

static ClassIds findClasses (IPluginFactory* factory)
{
    ClassIds ids;
    const int32 n = factory->countClasses();
    for (int32 i = 0; i < n; ++i)
    {
        PClassInfo ci;
        if (factory->getClassInfo (i, &ci) != kResultOk) continue;
        if (std::strcmp (ci.category, kVstAudioEffectClass) == 0 && ! ids.haveComponent)
        {
            std::memcpy (ids.componentCid, ci.cid, sizeof (TUID));
            ids.haveComponent = true;
        }
        else if (std::strcmp (ci.category, kVstComponentControllerClass) == 0 && ! ids.haveController)
        {
            std::memcpy (ids.controllerCid, ci.cid, sizeof (TUID));
            ids.haveController = true;
        }
    }
    return ids;
}

// ============================================================================
//  The instance-driving routine (one component + controller life cycle).
//  Every COM call that could touch our code / JUCE globals is SEH-guarded so
//  crashes/exceptions are attributed to a specific step instead of just killing
//  the process silently.
// ============================================================================

struct DriveConfig
{
    IPluginFactory* factory { nullptr };
    FUnknown* hostContext { nullptr };     // may be nullptr for the null-ctx case
    ClassIds ids;
    double sampleRate { 48000.0 };
    int32 maxBlock { 1024 };
    int threadTag { -1 };                  // for concurrency logging
    const std::function<void()>* barrier { nullptr }; // fired right before createInstance
};

// Hammer only createInstance(component)+release across threads. This is the
// tightest stressor for JUCE's non-atomic ScopedJuceInitialiser_GUI refcount
// (numScopedInitInstances) and the unguarded MessageManager singleton, both of
// which are touched on every VST3 createInstance via ScopedRunLoop.
static void churnInstance (DriveConfig cfg, Accumulator& acc, int iters)
{
    const int tag = cfg.threadTag;
    if (cfg.barrier) (*cfg.barrier)();

    for (int i = 0; i < iters; ++i)
    {
        IComponent* component = nullptr;
        DWORD code = 0;
        const bool ran = guardedCall ([&]
        {
            cfg.factory->createInstance ((FIDString) cfg.ids.componentCid,
                                         (FIDString) IComponent_iid,
                                         reinterpret_cast<void**> (&component));
            if (component) { component->release(); component = nullptr; }
        }, code);

        if (! ran)
        {
            const bool isCpp = (code == kCppExceptionCode);
            acc.note (isCpp ? StepOutcome::CppException : StepOutcome::HardFault);
            logf ("    [t%d] churn iter %d => !! %s (code=0x%08lX)\n",
                  tag, i, sehName (code), code);
            return;
        }
    }
    logf ("    [t%d] churn complete (%d iters)\n", tag, iters);
}

// Wrap a COM call, log it, record worst outcome. Returns true if it ran without
// a structured/C++ exception (regardless of the tresult).
static bool step (Accumulator& acc, const char* label, int tag,
                  const std::function<tresult()>& call, tresult* outResult = nullptr)
{
    tresult result = kResultFalse;
    DWORD code = 0;
    const bool ran = guardedCall ([&] { result = call(); }, code);
    if (outResult) *outResult = result;

    if (! ran)
    {
        const bool isCpp = (code == kCppExceptionCode);
        acc.note (isCpp ? StepOutcome::CppException : StepOutcome::HardFault);
        logf ("    [t%d] %-22s => !! %s (code=0x%08lX)\n", tag, label, sehName (code), code);
        return false;
    }
    logf ("    [t%d] %-22s => %s (0x%08X)\n", tag, label, tresultName (result), (unsigned) result);
    return true;
}

// Drives create -> initialize -> connect -> setup -> activate -> teardown.
// Returns when done (or aborts early on a fault). Uses only the factory + host
// context supplied; safe to call from multiple threads concurrently against the
// same factory (that concurrency is exactly what we are testing).
static void driveInstance (DriveConfig cfg, Accumulator& acc)
{
    const int tag = cfg.threadTag;

    IComponent* component = nullptr;
    IEditController* controller = nullptr;
    IAudioProcessor* processor = nullptr;
    IConnectionPoint* compCp = nullptr;
    IConnectionPoint* ctrlCp = nullptr;
    HostComponentHandler* handler = nullptr;

    auto cleanup = [&]
    {
        DWORD dummy = 0;
        if (compCp && ctrlCp)
            guardedCall ([&] { compCp->disconnect (ctrlCp); ctrlCp->disconnect (compCp); }, dummy);
        if (processor) guardedCall ([&] { processor->release(); }, dummy);
        if (compCp)    guardedCall ([&] { compCp->release(); }, dummy);
        if (ctrlCp)    guardedCall ([&] { ctrlCp->release(); }, dummy);
        if (component) guardedCall ([&] { component->terminate(); component->release(); }, dummy);
        if (controller) guardedCall ([&] { controller->terminate(); controller->release(); }, dummy);
        if (handler)   guardedCall ([&] { handler->release(); }, dummy);
    };

    // Fire the barrier so concurrent threads hit createInstance simultaneously.
    if (cfg.barrier) (*cfg.barrier)();

    // 1. Create component (this runs our AudioProcessor ctor + JUCE globals init).
    tresult r;
    if (! step (acc, "createInstance(comp)", tag,
                [&] { return cfg.factory->createInstance ((FIDString) cfg.ids.componentCid,
                                                          (FIDString) IComponent_iid,
                                                          reinterpret_cast<void**> (&component)); }, &r))
    { cleanup(); return; }

    if (component == nullptr || r != kResultOk)
    {
        // This is the LUNA "mInstance = 0" symptom: a clean soft failure.
        acc.note (StepOutcome::SoftFail);
        logf ("    [t%d] component == nullptr after createInstance (LUNA 'mInstance = 0' shape)\n", tag);
        cleanup();
        return;
    }

    // 2. Initialize component with host context.
    if (! step (acc, "component.initialize", tag,
                [&] { return component->initialize (cfg.hostContext); }))
    { cleanup(); return; }

    // 3. Query IAudioProcessor.
    step (acc, "QI(IAudioProcessor)", tag,
          [&] { return component->queryInterface (IAudioProcessor_iid, reinterpret_cast<void**> (&processor)); });

    // 4. Create + initialize controller (JUCE ships a separate controller class).
    if (cfg.ids.haveController)
    {
        if (step (acc, "createInstance(ctrl)", tag,
                  [&] { return cfg.factory->createInstance ((FIDString) cfg.ids.controllerCid,
                                                            (FIDString) IEditController_iid,
                                                            reinterpret_cast<void**> (&controller)); })
            && controller != nullptr)
        {
            step (acc, "controller.initialize", tag,
                  [&] { return controller->initialize (cfg.hostContext); });

            handler = new HostComponentHandler();
            step (acc, "setComponentHandler", tag,
                  [&] { return controller->setComponentHandler (handler); });

            // 5. Connect component <-> controller directly (drives the message
            //    exchange that uses the host context to allocate an IMessage).
            component->queryInterface (IConnectionPoint_iid, reinterpret_cast<void**> (&compCp));
            controller->queryInterface (IConnectionPoint_iid, reinterpret_cast<void**> (&ctrlCp));
            if (compCp && ctrlCp)
            {
                step (acc, "connect(comp->ctrl)", tag, [&] { return compCp->connect (ctrlCp); });
                step (acc, "connect(ctrl->comp)", tag, [&] { return ctrlCp->connect (compCp); });
            }
        }
    }

    // 6. setupProcessing at the requested sample rate / block size.
    if (processor != nullptr)
    {
        ProcessSetup setup {};
        setup.processMode        = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = cfg.maxBlock;
        setup.sampleRate         = cfg.sampleRate;
        step (acc, "setupProcessing", tag, [&] { return processor->setupProcessing (setup); });
    }

    // 7. Activate buses + component (this calls prepareToPlay).
    step (acc, "activateBus(in)", tag,
          [&] { return component->activateBus (kAudio, kInput, 0, (TBool) 1); });
    step (acc, "activateBus(out)", tag,
          [&] { return component->activateBus (kAudio, kOutput, 0, (TBool) 1); });
    step (acc, "component.setActive(1)", tag,
          [&] { return component->setActive ((TBool) 1); });

    if (processor != nullptr)
        step (acc, "setProcessing(1)", tag, [&] { return processor->setProcessing ((TBool) 1); });

    // 8. Deactivate + teardown.
    if (processor != nullptr)
        step (acc, "setProcessing(0)", tag, [&] { return processor->setProcessing ((TBool) 0); });
    step (acc, "component.setActive(0)", tag,
          [&] { return component->setActive ((TBool) 0); });

    cleanup();
    logf ("    [t%d] instance lifecycle complete\n", tag);
}

// ============================================================================
//  Scenario runners (child process, single --case)
// ============================================================================

struct ChildArgs
{
    std::string caseName;
    std::wstring modulePath;
    int threads { 5 };
    int iters { 8 };
    double sampleRate { 48000.0 };
    int maxBlock { 1024 };
};

static MsgMode msgModeForCase (const std::string& c)
{
    if (c == "refuse-msg")   return MsgMode::RefuseFalse;
    if (c == "notimpl-msg")  return MsgMode::RefuseNotImpl;
    if (c == "null-obj-msg") return MsgMode::OkButNull;
    return MsgMode::Good;
}

// Attach the host context to the factory (LUNA calls setHostContext).
static void setFactoryHostContext (IPluginFactory* factory, FUnknown* ctx)
{
    IPluginFactory3* f3 = nullptr;
    if (factory->queryInterface (IPluginFactory3_iid, reinterpret_cast<void**> (&f3)) == kResultOk && f3)
    {
        tresult r = f3->setHostContext (ctx);
        logf ("    setHostContext => %s (0x%08X)\n", tresultName (r), (unsigned) r);
        f3->release();
    }
}

static int runChildCase (const ChildArgs& a)
{
    logf ("=== child case '%s' (module already resolved) ===\n", a.caseName.c_str());

    Accumulator acc;

    // reload-cycle manages its own module load/unload loop.
    if (a.caseName == "reload-cycle")
    {
        for (int k = 0; k < a.iters; ++k)
        {
            logf ("  -- reload iteration %d/%d --\n", k + 1, a.iters);
            LoadedModule lm = loadModule (a.modulePath);
            if (! lm.ok)
            {
                logf ("    load failed: %s\n", lm.error.c_str());
                acc.note (StepOutcome::SoftFail);
                unloadModule (lm);
                continue;
            }
            IPluginFactory* factory = nullptr;
            DWORD code = 0;
            const bool got = guardedCall ([&] { factory = lm.getFactory(); }, code);
            if (! got || factory == nullptr)
            {
                logf ("    GetPluginFactory failed/faulted (code=0x%08lX)\n", code);
                acc.note (got ? StepOutcome::SoftFail : StepOutcome::HardFault);
                unloadModule (lm);
                continue;
            }
            HostApplication* host = new HostApplication (MsgMode::Good);
            setFactoryHostContext (factory, static_cast<IHostApplication*> (host));
            ClassIds ids = findClasses (factory);

            DriveConfig cfg;
            cfg.factory = factory;
            cfg.hostContext = static_cast<IHostApplication*> (host);
            cfg.ids = ids;
            cfg.sampleRate = a.sampleRate;
            cfg.maxBlock = a.maxBlock;
            cfg.threadTag = 0;
            driveInstance (cfg, acc);

            factory->release();
            host->release();
            unloadModule (lm);
        }
        logf ("=== child case '%s' worst outcome=%d ===\n", a.caseName.c_str(), acc.worst.load());
        return acc.worst.load();
    }

    // All other cases share one module load.
    LoadedModule lm = loadModule (a.modulePath);
    if (! lm.ok)
    {
        logf ("  load failed: %s\n", lm.error.c_str());
        return (int) StepOutcome::SoftFail;
    }

    IPluginFactory* factory = nullptr;
    {
        DWORD code = 0;
        const bool got = guardedCall ([&] { factory = lm.getFactory(); }, code);
        if (! got || factory == nullptr)
        {
            logf ("  GetPluginFactory failed/faulted (code=0x%08lX)\n", code);
            unloadModule (lm);
            return got ? (int) StepOutcome::SoftFail : (int) StepOutcome::HardFault;
        }
    }

    const bool nullCtx = (a.caseName == "null-ctx");
    HostApplication* host = nullCtx ? nullptr : new HostApplication (msgModeForCase (a.caseName));
    FUnknown* ctx = host ? static_cast<IHostApplication*> (host) : nullptr;

    setFactoryHostContext (factory, ctx);

    ClassIds ids = findClasses (factory);
    logf ("  discovered classes: component=%d controller=%d\n", ids.haveComponent, ids.haveController);

    DriveConfig base;
    base.factory = factory;
    base.hostContext = ctx;
    base.ids = ids;
    base.sampleRate = a.sampleRate;
    base.maxBlock = a.maxBlock;

    if (a.caseName == "concurrent" || a.caseName == "concurrent-churn")
    {
        const bool churn = (a.caseName == "concurrent-churn");
        const int loops = churn ? a.iters : 1;
        logf ("  launching %d threads (%s), %d loop(s) each...\n",
              a.threads, churn ? "createInstance/release churn" : "full lifecycle", loops);

        // Barrier: block every thread until all have arrived, so the very first
        // createInstance across threads happens as close to simultaneously as
        // possible (maximising the JUCE init-refcount race window).
        std::atomic<int> arrived { 0 };
        std::atomic<bool> release { false };
        const int nthreads = a.threads;
        std::function<void()> barrier = [&]
        {
            arrived.fetch_add (1);
            while (! release.load()) { std::this_thread::yield(); }
        };

        std::vector<std::thread> pool;
        for (int t = 0; t < nthreads; ++t)
        {
            pool.emplace_back ([&, t]
            {
                DriveConfig cfg = base;
                cfg.threadTag = t;
                cfg.barrier = &barrier;
                if (churn) churnInstance (cfg, acc, loops);
                else       driveInstance (cfg, acc);
            });
        }
        while (arrived.load() < nthreads) std::this_thread::yield();
        release.store (true);
        for (auto& th : pool) th.join();
    }
    else if (a.caseName == "worker-thread")
    {
        DriveConfig cfg = base;
        cfg.threadTag = 1;
        std::thread th ([&] { driveInstance (cfg, acc); });
        th.join();
    }
    else
    {
        // good / refuse-msg / notimpl-msg / null-obj-msg / null-ctx / hi-sr
        DriveConfig cfg = base;
        cfg.threadTag = 0;
        driveInstance (cfg, acc);
    }

    factory->release();
    if (host) host->release();
    unloadModule (lm);

    logf ("  live host objects remaining: %d\n", g_liveHostObjects.load());
    logf ("=== child case '%s' worst outcome=%d ===\n", a.caseName.c_str(), acc.worst.load());
    return acc.worst.load();
}

// ============================================================================
//  Orchestrator (parent process): runs each case as an isolated child.
// ============================================================================

static std::wstring selfExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW (nullptr, buf, MAX_PATH * 2);
    return buf;
}

static std::string outcomeLabel (int v)
{
    switch (v)
    {
        case 0:  return "OK";
        case 10: return "SOFT-FAIL (mInstance=0 shape)";
        case 20: return "C++ EXCEPTION escaped COM";
        case 30: return "HARD FAULT (AV/overflow)";
        default: return "outcome=" + std::to_string (v);
    }
}

struct CaseSpec { const char* name; int extraThreads; int timeoutMs; };

static int runOrchestrator (const std::wstring& modulePath)
{
    const std::vector<CaseSpec> cases =
    {
        { "good",          0, 30000 },
        { "hi-sr",         0, 30000 },
        { "refuse-msg",    0, 30000 },
        { "notimpl-msg",   0, 30000 },
        { "null-obj-msg",  0, 30000 },
        { "null-ctx",      0, 30000 },
        { "worker-thread", 0, 30000 },
        { "concurrent",    5, 60000 },
        { "concurrent-churn", 16, 120000 },
        { "reload-cycle",  0, 60000 },
    };

    std::wstring self = selfExePath();

    struct Row { std::string name; std::string result; };
    std::vector<Row> matrix;

    for (const auto& cs : cases)
    {
        logf ("\n############################################################\n");
        logf ("# CASE: %s\n", cs.name);
        logf ("############################################################\n");

        std::wstring cmd = L"\"" + self + L"\" --child --case ";
        // widen case name
        std::wstring wname (cs.name, cs.name + std::strlen (cs.name));
        cmd += wname;
        cmd += L" --module \"" + modulePath + L"\"";
        if (std::strcmp (cs.name, "concurrent") == 0)
            cmd += L" --threads 5";
        if (std::strcmp (cs.name, "concurrent-churn") == 0)
            cmd += L" --threads 16 --iters 200";

        STARTUPINFOW si {}; si.cb = sizeof (si);
        PROCESS_INFORMATION pi {};
        std::vector<wchar_t> cmdMut (cmd.begin(), cmd.end());
        cmdMut.push_back (0);

        std::string resultStr;
        if (! CreateProcessW (nullptr, cmdMut.data(), nullptr, nullptr, TRUE,
                              0, nullptr, nullptr, &si, &pi))
        {
            resultStr = "spawn failed (GetLastError=" + std::to_string (GetLastError()) + ")";
        }
        else
        {
            const DWORD w = WaitForSingleObject (pi.hProcess, cs.timeoutMs);
            if (w == WAIT_TIMEOUT)
            {
                TerminateProcess (pi.hProcess, 0xDEAD);
                WaitForSingleObject (pi.hProcess, 5000);
                resultStr = "HANG / TIMEOUT (killed)";
            }
            else
            {
                DWORD exitCode = 0;
                GetExitCodeProcess (pi.hProcess, &exitCode);
                // A child killed by an unhandled fault reports the NTSTATUS as the
                // exit code. Our own guarded outcomes are the small values 0/10/20/30.
                if (exitCode == 0 || exitCode == 10 || exitCode == 20 || exitCode == 30)
                    resultStr = outcomeLabel ((int) exitCode);
                else
                    resultStr = "CHILD CRASHED (exit=0x" + [&]{
                        char b[16]; std::snprintf (b, sizeof (b), "%08lX", exitCode); return std::string (b);
                    }() + ")";
            }
            CloseHandle (pi.hProcess);
            CloseHandle (pi.hThread);
        }

        logf ("# CASE %s RESULT: %s\n", cs.name, resultStr.c_str());
        matrix.push_back ({ cs.name, resultStr });
    }

    logf ("\n==================== RESULT MATRIX ====================\n");
    for (const auto& row : matrix)
        logf ("  %-16s : %s\n", row.name.c_str(), row.result.c_str());
    logf ("======================================================\n");

    int worst = 0;
    for (const auto& row : matrix)
        if (row.result.rfind ("OK", 0) != 0) worst = 1;
    return worst;
}

// ============================================================================
//  main / argument parsing
// ============================================================================

static std::wstring getArgW (const std::vector<std::wstring>& args, const std::wstring& key,
                             const std::wstring& def = L"")
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1];
    return def;
}
static bool hasFlag (const std::vector<std::wstring>& args, const std::wstring& key)
{
    for (auto& a : args) if (a == key) return true;
    return false;
}
static int getArgI (const std::vector<std::wstring>& args, const std::wstring& key, int def)
{
    std::wstring v = getArgW (args, key);
    return v.empty() ? def : (int) std::wcstol (v.c_str(), nullptr, 10);
}
static double getArgD (const std::vector<std::wstring>& args, const std::wstring& key, double def)
{
    std::wstring v = getArgW (args, key);
    return v.empty() ? def : std::wcstod (v.c_str(), nullptr);
}

int wmain (int argc, wchar_t** argv)
{
    std::vector<std::wstring> args (argv + 1, argv + argc);

    std::wstring modulePath = getArgW (args, L"--module");
    if (modulePath.empty())
    {
        logf ("vst3_probe - minimal VST3 host harness (issue #89)\n");
        logf ("usage: vst3_probe --module \"<path to .vst3>\" [--all]\n");
        logf ("       vst3_probe --child --case <name> --module <path> [--threads N] ...\n");
        return 2;
    }

    if (hasFlag (args, L"--child"))
    {
        ChildArgs a;
        std::wstring wcase = getArgW (args, L"--case", L"good");
        a.caseName.assign (wcase.begin(), wcase.end());
        a.modulePath = modulePath;
        a.threads    = getArgI (args, L"--threads", 5);
        a.iters      = getArgI (args, L"--iters", 8);
        a.maxBlock   = getArgI (args, L"--maxblock", (a.caseName == "hi-sr") ? 8192 : 1024);
        a.sampleRate = getArgD (args, L"--samplerate", (a.caseName == "hi-sr") ? 192000.0 : 48000.0);
        return runChildCase (a);
    }

    // Orchestrate (default).
    logf ("vst3_probe orchestrator\n  module: %ls\n", modulePath.c_str());
    return runOrchestrator (modulePath);
}
