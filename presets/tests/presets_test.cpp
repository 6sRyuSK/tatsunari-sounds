//
// presets/tests/presets_test.cpp — headless unit tests for the JUCE-free preset
// model successors: StateCodec (wire format), PresetSession (program/preset core),
// and UserPresetStoreFs (on-disk user presets).
//
// Conventions mirror core/tests and params/tests:
//   * links ONLY factory_presets + factory_params (no framework) — this build also
//     proves those headers stay framework-free; a stray framework include would
//     fail to compile here.
//   * accumulate failures in g_failures / fail(), return 1 at the end.
//   * a single case (nothing here is sample-rate dependent).
//
// Oracles are independent of the code under test: hand-built byte layouts for the
// framing, hand-computed expected values for apply/dirty, and a temp-directory
// round-trip for the file store.
//
#include "factory_presets/StateCodec.h"
#include "factory_presets/PresetSession.h"
#include "factory_presets/UserPresetStoreFs.h"

#include "factory_params/ParamDesc.h"
#include "factory_params/ParamStore.h"
#include "factory_params/Range.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace factory_presets;
using factory_params::ParamDesc;
using factory_params::ParamStore;
using factory_params::floatParam;
using factory_params::boolParam;
using factory_params::choiceParam;

namespace
{
int g_failures = 0;

void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("  FAIL: %s\n", msg.c_str());
}
void check (bool cond, const std::string& msg) { if (! cond) fail (msg); }

bool bitEqual (double a, double b) { return std::memcmp (&a, &b, sizeof a) == 0; }
bool bitEqual (float a, float b)   { return std::memcmp (&a, &b, sizeof a) == 0; }

// ===========================================================================
// 1. StateCodec — round-trip, framing, tolerant read, malformed, hook, version
// ===========================================================================
void checkStateCodecRoundTrip()
{
    std::printf ("1. StateCodec exact round-trip (denormalised values re-parse exactly)\n");

    StateModel m;
    m.presetIndex  = 4;
    m.stateVersion = kStateVersionCurrent;
    m.params = {
        { "depth", 65.0 }, { "out", -6.0 }, { "attack", 0.1 }, { "release", 12000.0 },
        { "mix", 1.0 / 3.0 }, { "tiny", 1e-9 }, { "neg", -12.5 }, { "zero", 0.0 },
        { "a&b", 3.14159265358979 }   // id needs XML escaping
    };

    const StateBlob blob = encode (m);
    const auto back = decode (blob.data(), blob.size());
    if (! back) { fail ("decode(encode(m)) returned nullopt"); return; }

    check (back->presetIndex  == m.presetIndex,  "presetIndex round-trips");
    check (back->stateVersion == m.stateVersion, "stateVersion round-trips");
    check (back->params.size() == m.params.size(), "param count round-trips");
    for (std::size_t i = 0; i < m.params.size() && i < back->params.size(); ++i)
    {
        check (back->params[i].first == m.params[i].first,
               "param id round-trips: " + m.params[i].first);
        check (bitEqual (back->params[i].second, m.params[i].second),
               "param value re-parses bit-exactly: " + m.params[i].first);
    }
}

void checkStateCodecFraming()
{
    std::printf ("2. StateCodec framing (magic LE + length LE + UTF-8 XML + null)\n");

    StateModel m;
    m.presetIndex = 2; m.stateVersion = 1;
    m.params = { { "depth", 65.0 } };

    const std::string xml  = writeXml (m);
    const StateBlob    blob = encode (m);

    // Independent little-endian readers (do NOT reuse the codec's helpers).
    auto u32 = [] (const unsigned char* p) -> std::uint32_t
    {
        return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8)
             | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
    };

    check (blob.size() == 9 + xml.size(), "total size == 9 + xml length");
    check (u32 (blob.data()) == kStateMagic, "magic 0x21324356 little-endian");
    check (u32 (blob.data() + 4) == (std::uint32_t) xml.size(), "length field == xml byte length");
    check (blob.back() == 0, "trailing null terminator");
    check (std::memcmp (blob.data() + 8, xml.data(), xml.size()) == 0, "xml bytes follow the header");

    // The XML payload has the documented shape.
    check (xml.find ("<PARAMS presetIndex=\"2\" stateVersion=\"1\">") != std::string::npos,
           "root carries presetIndex + stateVersion");
    check (xml.find ("<PARAM id=\"depth\" value=\"65\"/>") != std::string::npos,
           "PARAM child carries id + denormalised value");
}

void checkStateCodecTolerant()
{
    std::printf ("3. StateCodec tolerant read (unknown ignored, missing -> absent)\n");

    // Unknown root attr, unknown PARAM attr, unknown self-closed + text elements,
    // a valueless PARAM (skipped), pretty-printed whitespace.
    const char* xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<PARAMS presetIndex=\"3\" stateVersion=\"7\" future=\"ignore-me\">\n"
        "  <PARAM id=\"depth\" value=\"65.0\" hint=\"unused\"/>\n"
        "  <WIDGET x=\"1\"/>\n"
        "  <NOTE>free text <b>bold</b></NOTE>\n"
        "  <PARAM id=\"broken\"/>\n"
        "  <PARAM id=\"mix\" value=\"50\"/>\n"
        "</PARAMS>";

    const auto m = readXml (xml);
    if (! m) { fail ("tolerant XML failed to parse"); return; }

    check (m->presetIndex == 3,  "presetIndex parsed past an unknown root attr");
    check (m->stateVersion == 7, "stateVersion parsed");
    check (m->params.size() == 2, "only the two well-formed PARAMs kept (broken/unknown dropped)");
    check (m->has ("depth") && bitEqual (m->get ("depth", -1.0), 65.0), "depth read");
    check (m->has ("mix")   && bitEqual (m->get ("mix", -1.0), 50.0), "mix read");
    check (! m->has ("broken"), "valueless PARAM dropped");
    check (! m->has ("nope") && bitEqual (m->get ("nope", 42.0), 42.0), "missing key -> caller default");
}

void checkStateCodecMalformed()
{
    std::printf ("4. StateCodec malformed -> clean failure (no throw)\n");

    // Valid baseline to corrupt.
    StateModel m; m.params = { { "depth", 1.0 } };
    StateBlob good = encode (m);

    check (! decode (nullptr, 0).has_value(), "null/empty -> nullopt");
    check (! decode (good.data(), 8).has_value(), "size <= 8 -> nullopt");

    StateBlob badMagic = good; badMagic[0] ^= 0xff;
    check (! decode (badMagic.data(), badMagic.size()).has_value(), "wrong magic -> nullopt");

    // Framed but non-XML / non-PARAMS / unterminated payloads.
    check (! decode (frame ("not xml at all").data(),   frame ("not xml at all").size()).has_value(),   "garbage payload -> nullopt");
    check (! decode (frame ("<FOO/>").data(),           frame ("<FOO/>").size()).has_value(),           "non-PARAMS root -> nullopt");
    check (! decode (frame ("<PARAMS>").data(),         frame ("<PARAMS>").size()).has_value(),         "unterminated root -> nullopt");
    check (! readXml ("<PARAMS><PARAM id=\"x\" value=\"1\"").has_value(),                                "unterminated tag -> nullopt");

    // A truncated buffer (fewer bytes than the framed XML) is detected as malformed
    // rather than read past the end: the reader clamps to the bytes provided, so the
    // partial XML has no </PARAMS> and fails to parse.
    check (! decode (good.data(), good.size() - 15).has_value(), "truncated buffer -> nullopt (clamped read, no OOB)");

    // A length field claiming more than the buffer holds, but whose payload is
    // nonetheless complete, still parses (tolerant): the claimed length is clamped
    // to the buffer and the trailing null after </PARAMS> is ignored.
    StateBlob longLen = encode (m);
    longLen[4] = 0xff; longLen[5] = 0xff; // claim ~64 KiB of XML
    check (decode (longLen.data(), longLen.size()).has_value(), "over-long length but complete payload -> still parses (clamped, not OOB)");
}

void checkStateCodecMigration()
{
    std::printf ("5. StateCodec legacy-migration hook + stateVersion semantics\n");

    // RS-style hook: derive "detail" from a legacy sharpness/selectivity pair when
    // absent (mean), leave an existing "detail" alone.
    auto migrate = [] (StateModel& s)
    {
        if (! s.has ("detail"))
        {
            const double sharp = s.get ("sharpness", 50.0);
            const double sel   = s.get ("selectivity", 50.0);
            s.set ("detail", (sharp + sel) / 2.0);
        }
    };

    // Legacy blob: no stateVersion attr in the model -> writes 0 -> reads back 0.
    StateModel legacy;
    legacy.stateVersion = 0;
    legacy.params = { { "sharpness", 80.0 }, { "selectivity", 60.0 }, { "depth", 50.0 } };
    const StateBlob lb = encode (legacy);
    check (writeXml (legacy).find ("stateVersion=\"0\"") != std::string::npos, "legacy writes stateVersion=0");

    const auto mig = decode (lb.data(), lb.size(), migrate);
    if (! mig) { fail ("legacy decode+migrate returned nullopt"); return; }
    check (mig->stateVersion == 0, "absent-in-model stateVersion reads as 0 (legacy)");
    check (mig->has ("detail") && bitEqual (mig->get ("detail", -1.0), 70.0), "hook injected detail == mean(80,60)");

    // Modern blob already carrying detail: hook must not overwrite it.
    StateModel modern;
    modern.stateVersion = kStateVersionCurrent;
    modern.params = { { "detail", 33.0 }, { "sharpness", 80.0 } };
    const StateBlob mb = encode (modern);
    const auto m2 = decode (mb.data(), mb.size(), migrate);
    if (! m2) { fail ("modern decode+migrate returned nullopt"); return; }
    check (m2->stateVersion == kStateVersionCurrent, "modern stateVersion round-trips");
    check (bitEqual (m2->get ("detail", -1.0), 33.0), "existing detail left untouched by hook");
}

// ===========================================================================
// 2. PresetSession — program list, apply (real units, no residue, exclusions),
//    index round-trip, dirty tracking, suppressNextAutoSync
// ===========================================================================
std::vector<ParamDesc> sessionDescs()
{
    return {
        floatParam  ("depth",   "Depth",   0.0f, 100.0f,   0.1f, 30.0f, " %",  1),
        floatParam  ("out",     "Output", -24.0f,  24.0f,  0.1f,  0.0f, " dB", 1),
        floatParam  ("b0_freq", "B0 Freq", 20.0f, 20000.0f, 0.0f, 650.0f, " Hz", 1, 650.0f),
        boolParam   ("bypass",  "Bypass",  false, 1, factory_params::kFlagBypass),
        choiceParam ("quality", "Quality", { "Fast", "Normal", "High" }, 1, 1)
    };
}

// Static preset bank (raw pointers into constexpr arrays, as PresetBank expects).
constexpr PresetParam kP1[] = { { "depth", 80.0f }, { "out", -6.0f } };
constexpr PresetParam kP2[] = { { "b0_freq", 1000.0f }, { "quality", 2.0f } }; // quality is EXCLUDED
constexpr Preset      kPresets[] = {
    { "Preset One", kP1, 2 },
    { "Preset Two", kP2, 2 },
};
constexpr PresetBank  kBank { kPresets, 2 };

// Independent expected real value: descriptor default, snapped to its legal grid.
float snappedDefault (const ParamStore& store, const char* id)
{
    const int idx = store.indexOf (id);
    const auto& d = store.desc (idx);
    return factory_params::snapToLegalValue (factory_params::makeRange (d), d.defaultValue);
}
float snappedValue (const ParamStore& store, const char* id, float real)
{
    const int idx = store.indexOf (id);
    return factory_params::snapToLegalValue (factory_params::makeRange (store.desc (idx)), real);
}
float readVal (const ParamStore& store, const char* id) { return store.value (store.indexOf (id)); }

void checkPresetSession()
{
    std::printf ("6. PresetSession program list / apply / dirty / suppress\n");

    ParamStore store (sessionDescs());
    const std::vector<std::string> exclude { "bypass", "quality" };
    PresetSession session (store, kBank, exclude);

    // Program list + names.
    check (session.numPrograms() == 3, "numPrograms == 1 (Init) + 2 presets");
    check (session.programName (0) == "Init", "program 0 == Init");
    check (session.programName (1) == "Preset One", "program 1 name");
    check (session.programName (2) == "Preset Two", "program 2 name");
    check (session.programName (3).empty(), "out-of-range program name empty");
    check (session.isExcluded ("bypass") && session.isExcluded ("quality"), "exclusions reported");
    check (! session.isExcluded ("depth"), "managed param not excluded");

    // Pre-set the excluded params to non-defaults so "untouched" is observable.
    store.setFromHost (store.indexOf ("bypass"), 1.0f);
    store.setFromHost (store.indexOf ("quality"), 2.0f);

    // --- Init (program 0): managed -> defaults, excluded untouched.
    session.applyProgram (0);
    check (session.currentProgram() == 0, "currentProgram == 0 after applyProgram(0)");
    check (bitEqual (readVal (store, "depth"),   snappedDefault (store, "depth")),   "Init: depth default");
    check (bitEqual (readVal (store, "out"),     snappedDefault (store, "out")),     "Init: out default");
    check (bitEqual (readVal (store, "b0_freq"), snappedDefault (store, "b0_freq")), "Init: b0_freq default");
    check (bitEqual (readVal (store, "bypass"), 1.0f),  "Init: excluded bypass untouched");
    check (bitEqual (readVal (store, "quality"), 2.0f), "Init: excluded quality untouched");
    check (! session.isDirty(), "clean immediately after applyProgram(0)");

    // --- Preset One: listed values applied, unlisted managed -> default.
    session.applyProgram (1);
    check (session.currentProgram() == 1, "currentProgram == 1");
    check (bitEqual (readVal (store, "depth"), snappedValue (store, "depth", 80.0f)), "P1: depth == 80");
    check (bitEqual (readVal (store, "out"),   snappedValue (store, "out",  -6.0f)),  "P1: out == -6");
    check (bitEqual (readVal (store, "b0_freq"), snappedDefault (store, "b0_freq")),  "P1: unlisted b0_freq -> default (no residue)");
    check (bitEqual (readVal (store, "bypass"), 1.0f),  "P1: excluded bypass untouched");

    // --- Preset Two: b0_freq set; quality listed but EXCLUDED -> untouched.
    session.applyProgram (2);
    check (bitEqual (readVal (store, "b0_freq"), snappedValue (store, "b0_freq", 1000.0f)), "P2: b0_freq == 1000");
    check (bitEqual (readVal (store, "quality"), 2.0f), "P2: excluded quality untouched even though preset lists it");
    check (bitEqual (readVal (store, "depth"), snappedDefault (store, "depth")), "P2: unlisted depth -> default");

    // --- Index round-trip + out-of-range no-op.
    session.applyProgram (1);
    const auto none = session.applyProgram (99);
    check (none.empty() && session.currentProgram() == 1, "out-of-range applyProgram is a no-op");

    // --- Dirty tracking.
    check (! session.isDirty(), "clean after re-applying Preset One");
    store.setFromUi (store.indexOf ("depth"), 55.0f);
    check (session.isDirty(), "editing a managed param dirties the session");
    session.applyProgram (1);
    check (! session.isDirty(), "re-applying the program clears dirty");
    store.setFromUi (store.indexOf ("bypass"), 0.0f);
    check (! session.isDirty(), "editing an excluded param does NOT dirty the session");

    // --- suppressNextAutoSync one-shot latch.
    check (! session.consumeNextAutoSync(), "suppress latch starts clear");
    session.suppressNextAutoSync();
    check (session.consumeNextAutoSync(), "latch consumed true once");
    check (! session.consumeNextAutoSync(), "latch clears after one consume");
}

// ===========================================================================
// 3. UserPresetStoreFs — temp-dir round-trip, sanitisation, list/remove
// ===========================================================================
std::filesystem::path testRoot()
{
    std::error_code ec;
    return std::filesystem::temp_directory_path (ec) / "factory_presets_test";
}

void checkUserPresetStoreFs()
{
    std::printf ("7. UserPresetStoreFs round-trip / sanitisation / list-remove\n");

    namespace fs = std::filesystem;
    const fs::path root = testRoot();
    std::error_code ec;
    fs::remove_all (root, ec); // fresh start

    const UserPresetStoreFs::Blob blob { 1, 2, 3, 4, 5, 0, 0xff, 0x21 };

    // --- Round-trip in a temp dir.
    {
        UserPresetStoreFs store (root / "roundtrip");
        check (! store.exists ("My Preset"), "absent before save");
        check (store.save ("My Preset", blob), "save succeeds");
        check (store.exists ("My Preset"), "exists after save");
        const auto loaded = store.load ("My Preset");
        check (loaded.has_value() && *loaded == blob, "load returns the exact bytes");
        const auto names = store.list();
        check (names.size() == 1 && names[0] == "My Preset", "list shows the one preset");
        check (store.remove ("My Preset"), "remove succeeds");
        check (! store.exists ("My Preset"), "gone after remove");
        check (store.list().empty(), "list empty after remove");
        check (store.remove ("ghost"), "remove of a non-existent preset is a no-op success");

        // Empty blob round-trips as an empty blob (not a failure).
        check (store.save ("Empty", {}), "save empty blob");
        const auto e = store.load ("Empty");
        check (e.has_value() && e->empty(), "load empty blob");
    }

    // --- Sanitisation: illegal chars removed; original name resolves same file.
    {
        UserPresetStoreFs store (root / "sanitise");
        check (store.save ("a/b:c*?d", blob), "save name with illegal chars");
        const auto names = store.list();
        check (std::find (names.begin(), names.end(), "abcd") != names.end(),
               "illegal chars stripped -> 'abcd' on disk");
        check (store.load ("a/b:c*?d").has_value(), "original name resolves the sanitised file");
        check (store.load ("abcd").has_value(), "sanitised name resolves the same file");

        // Leading/trailing spaces + dots trimmed (divergence from the framework).
        check (store.save ("  .Trimmed.  ", blob), "save name with edge spaces/dots");
        const auto names2 = store.list();
        check (std::find (names2.begin(), names2.end(), "Trimmed") != names2.end(),
               "edge spaces/dots trimmed -> 'Trimmed'");

        // A name that sanitises to empty fails soft on every op.
        check (! store.save ("///", blob), "empty-after-sanitise save -> false");
        check (! store.load ("::").has_value(), "empty-after-sanitise load -> nullopt");
        check (! store.exists ("***"), "empty-after-sanitise exists -> false");
        check (! store.remove ("???"), "empty-after-sanitise remove -> false");
    }

    // --- list() sorted (byte-wise) + remove reflected.
    {
        UserPresetStoreFs store (root / "listing");
        check (store.save ("Alpha", blob) && store.save ("beta", blob) && store.save ("Gamma", blob),
               "save three presets");
        const auto names = store.list();
        // ASCII sort: uppercase before lowercase -> Alpha, Gamma, beta.
        check (names.size() == 3 && names[0] == "Alpha" && names[1] == "Gamma" && names[2] == "beta",
               "list is byte-wise sorted (documented divergence from case-insensitive)");
        check (store.remove ("beta"), "remove one");
        const auto after = store.list();
        check (after.size() == 2 && std::find (after.begin(), after.end(), "beta") == after.end(),
               "removed preset gone from list");
    }

    // --- list() on a never-created directory is empty, not an error.
    {
        UserPresetStoreFs store (root / "never_created");
        check (store.list().empty() && ! store.exists ("x"), "missing dir -> empty list, fail-soft");
    }

    fs::remove_all (root, ec); // clean up
}
} // namespace

int main()
{
    std::printf ("factory_presets unit tests\n");

    checkStateCodecRoundTrip();
    checkStateCodecFraming();
    checkStateCodecTolerant();
    checkStateCodecMalformed();
    checkStateCodecMigration();
    checkPresetSession();
    checkUserPresetStoreFs();

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
