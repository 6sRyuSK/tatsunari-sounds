#pragma once
//
// factory_presets/StateCodec.h — the framework-free successor to the state save/load
// path that today rides the reference framework (ProgramAdapter.h's stateToXml /
// applyStateXml + the framework's copyXmlToBinary / getXmlFromBinary). It is the
// single, headless-testable definition of the plugin state wire format, so the
// codec can be unit-tested without linking the framework and later reused by the
// CLAP shell.
//
// WIRE FORMAT
//   Framing (byte-compatible with the reference framework's copyXmlToBinary, so a
//   blob written here decodes with the framework's getXmlFromBinary and vice
//   versa):
//     [0..3]   magic 0x21324356, little-endian
//     [4..7]   uint32 little-endian = UTF-8 XML byte length (EXCLUDING the null)
//     [8..8+L) the UTF-8 XML text
//     [8+L]    a single 0x00 terminator byte      (total size == 9 + L)
//   getXmlFromBinary reads min(size-8, L) XML bytes, which excludes that null; the
//   decoder here mirrors that exactly.
//
//   VERIFY-AGAINST-A-REAL-BLOB (TODO): there is no captured golden framework blob
//   checked into plugins/*/tests/fixtures yet, so the framing above is asserted
//   only against its own spec (hand-computed bytes in presets/tests). When the RS
//   shell lands, add a fixture: a blob produced by the framework's copyXmlToBinary
//   for a known <PARAMS> tree, and assert decode() reads it byte-for-byte. Until
//   then this parity is SPEC-verified, not blob-verified.
//
//   Payload XML (this codec's own small dialect — a clean break from the framework
//   ValueTree serialisation, which is why the migration is a MAJOR bump):
//     <PARAMS presetIndex="N" stateVersion="V"><PARAM id="…" value="…"/>…</PARAMS>
//   * value is the DENORMALISED (real-unit) value, written via std::to_chars in
//     its shortest round-trippable form, so from_chars re-parses it to the exact
//     same double (asserted in presets/tests).
//   * stateVersion is NEW (the framework blob carried no version); an absent
//     attribute reads back as 0 == "legacy / pre-versioned", which a migration
//     hook can act on.
//
// TOLERANT READ (never throws): unknown attributes and unknown child elements are
// ignored; a PARAM missing its id or with an unparseable value is skipped (the
// caller then applies that parameter's default); a wrong magic, truncation, a
// non-PARAMS root, or malformed markup returns std::nullopt. The model carries
// only what was present, so "missing key -> default" is the caller's job (it holds
// the descriptor defaults) — matching the framework applyStateXml contract.
//
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace factory_presets
{
    // --- the parsed / to-be-written state ------------------------------------
    struct StateModel
    {
        std::vector<std::pair<std::string, double>> params; // (id, real value), in order
        int presetIndex  = 0;
        int stateVersion = 0;

        const double* find (std::string_view id) const noexcept
        {
            for (auto& kv : params)
                if (kv.first == id)
                    return &kv.second;
            return nullptr;
        }
        bool   has (std::string_view id) const noexcept { return find (id) != nullptr; }
        double get (std::string_view id, double fallback) const noexcept
        {
            const double* p = find (id);
            return p != nullptr ? *p : fallback;
        }
        // Upsert: overwrite an existing id in place, else append. Used by migration
        // hooks (e.g. injecting a derived "detail" from legacy sharpness/selectivity).
        void set (std::string_view id, double value)
        {
            for (auto& kv : params)
                if (kv.first == id) { kv.second = value; return; }
            params.emplace_back (std::string (id), value);
        }
    };

    inline constexpr std::uint32_t kStateMagic          = 0x21324356u;
    inline constexpr int           kStateVersionCurrent = 1;
    inline constexpr std::string_view kStateRootTag     = "PARAMS";

    using StateBlob = std::vector<unsigned char>;

    // === encode ==============================================================
    namespace detail
    {
        inline void putU32le (StateBlob& out, std::uint32_t v)
        {
            out.push_back (static_cast<unsigned char> ( v        & 0xffu));
            out.push_back (static_cast<unsigned char> ((v >> 8)  & 0xffu));
            out.push_back (static_cast<unsigned char> ((v >> 16) & 0xffu));
            out.push_back (static_cast<unsigned char> ((v >> 24) & 0xffu));
        }

        inline std::uint32_t getU32le (const unsigned char* p) noexcept
        {
            return  static_cast<std::uint32_t> (p[0])
                 | (static_cast<std::uint32_t> (p[1]) << 8)
                 | (static_cast<std::uint32_t> (p[2]) << 16)
                 | (static_cast<std::uint32_t> (p[3]) << 24);
        }

        inline void appendEscaped (std::string& out, std::string_view s)
        {
            for (char c : s)
            {
                switch (c)
                {
                    case '&': out += "&amp;";  break;
                    case '<': out += "&lt;";   break;
                    case '>': out += "&gt;";   break;
                    case '"': out += "&quot;"; break;
                    default:  out += c;        break;
                }
            }
        }

        inline std::string intToString (long long v)
        {
            char buf[32];
            auto res = std::to_chars (buf, buf + sizeof buf, v);
            return std::string (buf, res.ptr);
        }

        // Shortest representation that round-trips: from_chars(to_chars(x)) == x.
        inline std::string doubleShortest (double v)
        {
            char buf[64];
            auto res = std::to_chars (buf, buf + sizeof buf, v);
            return std::string (buf, res.ptr);
        }

        inline bool isSpace (char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }
        inline bool isNameEnd (char c) noexcept
        {
            return isSpace (c) || c == '>' || c == '/' || c == '=';
        }

        inline std::string unescape (std::string_view s)
        {
            std::string out;
            out.reserve (s.size());
            for (std::size_t i = 0; i < s.size();)
            {
                if (s[i] != '&') { out += s[i]; ++i; continue; }

                const std::size_t semi = s.find (';', i);
                if (semi == std::string_view::npos) { out += '&'; ++i; continue; }

                const std::string_view ent = s.substr (i + 1, semi - i - 1);
                if      (ent == "amp")  out += '&';
                else if (ent == "lt")   out += '<';
                else if (ent == "gt")   out += '>';
                else if (ent == "quot") out += '"';
                else if (ent == "apos") out += '\'';
                else if (! ent.empty() && ent[0] == '#')
                {
                    const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                    const std::string_view digits = ent.substr (hex ? 2 : 1);
                    int code = 0;
                    const auto* first = digits.data();
                    const auto* last  = digits.data() + digits.size();
                    const auto r = std::from_chars (first, last, code, hex ? 16 : 10);
                    if (r.ec == std::errc{} && code >= 0 && code < 0x80)
                        out += static_cast<char> (code);
                    // Non-ASCII code points are dropped (tolerant): factory ids /
                    // numeric values never need them.
                }
                else { out += '&'; ++i; continue; } // unknown entity: keep the '&'

                i = semi + 1;
            }
            return out;
        }

        inline int parseIntOr (std::string_view s, int fallback) noexcept
        {
            std::size_t b = 0, e = s.size();
            while (b < e && isSpace (s[b])) ++b;
            while (e > b && isSpace (s[e - 1])) --e;
            if (b < e && s[b] == '+') ++b; // from_chars rejects a leading '+'
            int v = 0;
            const auto r = std::from_chars (s.data() + b, s.data() + e, v);
            return r.ec == std::errc{} ? v : fallback;
        }

        inline bool parseDouble (std::string_view s, double& out) noexcept
        {
            std::size_t b = 0, e = s.size();
            while (b < e && isSpace (s[b])) ++b;
            while (e > b && isSpace (s[e - 1])) --e;
            if (b < e && s[b] == '+') ++b;
            const auto r = std::from_chars (s.data() + b, s.data() + e, out);
            return r.ec == std::errc{} && r.ptr == s.data() + e;
        }

        // Parse a tag body (positioned just after the element name) into its
        // attributes, reporting whether it self-closed and where parsing resumed.
        struct TagInfo
        {
            std::vector<std::pair<std::string, std::string>> attrs;
            bool        selfClose = false;
            std::size_t next      = 0;
            bool        ok        = false;
        };

        inline TagInfo readAttrs (std::string_view s, std::size_t i)
        {
            TagInfo t;
            const std::size_t n = s.size();
            for (;;)
            {
                while (i < n && isSpace (s[i])) ++i;
                if (i >= n) return t;                       // unterminated

                if (s[i] == '>') { t.next = i + 1; t.ok = true; return t; }
                if (s[i] == '/')
                {
                    ++i;
                    while (i < n && isSpace (s[i])) ++i;
                    if (i < n && s[i] == '>') { t.selfClose = true; t.next = i + 1; t.ok = true; }
                    return t;
                }

                const std::size_t ns = i;
                while (i < n && ! isNameEnd (s[i])) ++i;
                const std::string_view name = s.substr (ns, i - ns);

                while (i < n && isSpace (s[i])) ++i;
                if (i >= n || s[i] != '=')                  // valueless attr: tolerate + skip
                {
                    if (name.empty()) return t;             // stuck on junk -> malformed
                    continue;
                }
                ++i;
                while (i < n && isSpace (s[i])) ++i;
                if (i >= n) return t;
                const char q = s[i];
                if (q != '"' && q != '\'') return t;        // unquoted value -> malformed
                ++i;
                const std::size_t vs = i;
                while (i < n && s[i] != q) ++i;
                if (i >= n) return t;                        // unterminated value
                const std::string_view raw = s.substr (vs, i - vs);
                ++i;                                         // past the closing quote
                t.attrs.emplace_back (std::string (name), unescape (raw));
            }
        }
    } // namespace detail

    // Serialise the model to its XML payload text (no framing).
    inline std::string writeXml (const StateModel& m)
    {
        std::string s;
        s.reserve (64 + m.params.size() * 40);
        s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
        s += '<'; s += kStateRootTag;
        s += " presetIndex=\"";  s += detail::intToString (m.presetIndex);  s += '"';
        s += " stateVersion=\""; s += detail::intToString (m.stateVersion); s += "\">";
        for (auto& kv : m.params)
        {
            s += "<PARAM id=\"";
            detail::appendEscaped (s, kv.first);
            s += "\" value=\"";
            s += detail::doubleShortest (kv.second);
            s += "\"/>";
        }
        s += "</"; s += kStateRootTag; s += '>';
        return s;
    }

    // Wrap XML text in the framework-compatible framing (magic + LE length + null).
    inline StateBlob frame (std::string_view xml)
    {
        StateBlob out;
        out.reserve (9 + xml.size());
        detail::putU32le (out, kStateMagic);
        detail::putU32le (out, static_cast<std::uint32_t> (xml.size()));
        out.insert (out.end(), xml.begin(), xml.end());
        out.push_back (0); // terminator byte (framework parity; excluded from length)
        return out;
    }

    // Full encode: model -> framed binary blob ready to hand to the host.
    inline StateBlob encode (const StateModel& m) { return frame (writeXml (m)); }

    // === decode ==============================================================

    // Strip the framing, returning the XML text (or nullopt on a bad/truncated
    // frame). Mirrors getXmlFromBinary: min(size-8, storedLength) bytes.
    inline std::optional<std::string> unframe (const void* data, std::size_t size)
    {
        if (data == nullptr || size <= 8) return std::nullopt;
        const auto* p = static_cast<const unsigned char*> (data);
        if (detail::getU32le (p) != kStateMagic) return std::nullopt;

        const std::uint32_t len = detail::getU32le (p + 4);
        if (len == 0) return std::nullopt;

        const std::size_t avail = size - 8;
        const std::size_t n = static_cast<std::size_t> (len) < avail ? static_cast<std::size_t> (len)
                                                                     : avail;
        return std::string (reinterpret_cast<const char*> (p + 8), n);
    }

    // Tolerant XML -> model. nullopt only on a structurally broken document (no
    // PARAMS root, unterminated markup); individual bad PARAMs are skipped.
    inline std::optional<StateModel> readXml (std::string_view xml)
    {
        using namespace detail;
        StateModel m;
        std::size_t i = 0;
        const std::size_t n = xml.size();

        // Prolog: XML declaration, comments, doctype — all skipped.
        for (;;)
        {
            while (i < n && isSpace (xml[i])) ++i;
            if (i + 1 < n && xml[i] == '<' && xml[i + 1] == '?')
            {
                const auto e = xml.find ("?>", i + 2);
                if (e == std::string_view::npos) return std::nullopt;
                i = e + 2; continue;
            }
            if (i + 3 < n && xml.compare (i, 4, "<!--") == 0)
            {
                const auto e = xml.find ("-->", i + 4);
                if (e == std::string_view::npos) return std::nullopt;
                i = e + 3; continue;
            }
            if (i + 1 < n && xml[i] == '<' && xml[i + 1] == '!')
            {
                const auto e = xml.find ('>', i);
                if (e == std::string_view::npos) return std::nullopt;
                i = e + 1; continue;
            }
            break;
        }

        if (i >= n || xml[i] != '<') return std::nullopt;
        ++i;
        const std::size_t ns = i;
        while (i < n && ! isNameEnd (xml[i])) ++i;
        if (xml.substr (ns, i - ns) != kStateRootTag) return std::nullopt; // root must be PARAMS

        TagInfo root = readAttrs (xml, i);
        if (! root.ok) return std::nullopt;
        for (auto& a : root.attrs)
        {
            if      (a.first == "presetIndex")  m.presetIndex  = parseIntOr (a.second, 0);
            else if (a.first == "stateVersion") m.stateVersion = parseIntOr (a.second, 0);
        }
        if (root.selfClose) return m; // <PARAMS .../> — a valid empty state
        i = root.next;

        for (;;)
        {
            while (i < n && isSpace (xml[i])) ++i;
            if (i >= n) return std::nullopt;                 // no </PARAMS>

            if (xml[i] != '<')                               // stray text: skip to next tag
            {
                const auto lt = xml.find ('<', i);
                if (lt == std::string_view::npos) return std::nullopt;
                i = lt; continue;
            }
            if (i + 1 < n && xml[i + 1] == '/')              // </PARAMS> closes the root
            {
                const auto e = xml.find ('>', i);
                if (e == std::string_view::npos) return std::nullopt;
                return m;
            }
            if (i + 3 < n && xml.compare (i, 4, "<!--") == 0)
            {
                const auto e = xml.find ("-->", i + 4);
                if (e == std::string_view::npos) return std::nullopt;
                i = e + 3; continue;
            }

            ++i;
            const std::size_t es = i;
            while (i < n && ! isNameEnd (xml[i])) ++i;
            const std::string_view tag = xml.substr (es, i - es);

            TagInfo el = readAttrs (xml, i);
            if (! el.ok) return std::nullopt;

            if (tag == "PARAM")
            {
                std::string id;
                bool   haveId  = false;
                double val     = 0.0;
                bool   haveVal = false;
                for (auto& a : el.attrs)
                {
                    if      (a.first == "id")    { id = a.second; haveId = true; }
                    else if (a.first == "value") { haveVal = parseDouble (a.second, val); }
                }
                if (haveId && haveVal)
                    m.params.emplace_back (std::move (id), val);
                // else: dropped (tolerant) -> caller applies the parameter default.
            }

            if (el.selfClose)
            {
                i = el.next;
            }
            else
            {
                // Unknown non-self-closed element: skip to its matching close tag.
                std::string close = "</";
                close += tag;
                close += '>';
                const auto e = xml.find (close, el.next);
                if (e == std::string_view::npos) return std::nullopt;
                i = e + close.size();
            }
        }
    }

    // Full decode with a legacy-migration hook. `migrate` is invoked with the
    // parsed model (carrying stateVersion) so it can inject or transform params
    // before they reach the caller — e.g. RS derives "detail" from a pre-versioned
    // sharpness/selectivity pair when "detail" is absent. nullopt propagates a
    // bad frame / malformed document; the hook is not run in that case.
    template <class Migrate>
    inline std::optional<StateModel> decode (const void* data, std::size_t size, Migrate&& migrate)
    {
        auto xml = unframe (data, size);
        if (! xml) return std::nullopt;
        auto model = readXml (*xml);
        if (! model) return std::nullopt;
        migrate (*model);
        return model;
    }

    inline std::optional<StateModel> decode (const void* data, std::size_t size)
    {
        return decode (data, size, [] (StateModel&) {});
    }
}
