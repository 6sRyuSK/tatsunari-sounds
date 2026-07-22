#include "factory_shell/ClapParamBridge.h"

#include "factory_params/Text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace factory_shell
{
    using factory_params::ParamDesc;
    using factory_params::ParamType;

    ParamBridge::ParamBridge (const std::vector<ParamDesc>& descs, ExposePredicate isExposed)
        : table (&descs)
    {
        exposed.reserve (descs.size());
        byUid.reserve (descs.size());
        for (int i = 0; i < static_cast<int> (descs.size()); ++i)
        {
            const ParamDesc& d = descs[static_cast<std::size_t> (i)];
            if (isExposed != nullptr && ! isExposed (d))
                continue; // the plugin keeps this one off the CLAP surface
            exposed.push_back (i);
            byUid.emplace_back (d.uid, i);
        }
        std::sort (byUid.begin(), byUid.end(),
                   [] (const auto& a, const auto& b) { return a.first < b.first; });
    }

    int ParamBridge::decimalsFor (const ParamDesc& d) noexcept
    {
        if (d.type != ParamType::Float)
            return 0; // Bool/Choice render a label, not decimals
        if (d.interval <= 0.0f)
            return 2; // continuous (no grid): a sensible fixed precision
        int dec = 0;
        float x = d.interval;
        while (x < 1.0f && dec < 6) { x *= 10.0f; ++dec; }
        return dec;
    }

    bool ParamBridge::getInfo (std::uint32_t paramIndex, clap_param_info_t* info) const noexcept
    {
        if (info == nullptr || paramIndex >= exposed.size())
            return false;

        const ParamDesc& d = (*table)[static_cast<std::size_t> (exposed[paramIndex])];

        info->id     = d.uid; // clap_id == fnv1a32(id); see header (VST3-tag cutover TODO)
        info->cookie = nullptr;
        info->module[0] = '\0';
        std::snprintf (info->name, sizeof (info->name), "%s", d.name.c_str());

        std::uint32_t flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (d.type == ParamType::Bool || d.type == ParamType::Choice)
            flags |= CLAP_PARAM_IS_STEPPED;
        if (d.type == ParamType::Choice)
            flags |= CLAP_PARAM_IS_ENUM; // requires IS_STEPPED, set above
        if (d.flags & factory_params::kFlagBypass)
            flags |= CLAP_PARAM_IS_BYPASS; // merges plugin + host bypass; implies stepped

        info->flags         = flags;
        info->min_value     = static_cast<double> (d.minValue);
        info->max_value     = static_cast<double> (d.maxValue);
        info->default_value = static_cast<double> (d.defaultValue);
        return true;
    }

    int ParamBridge::storeIndexForParamIndex (std::uint32_t paramIndex) const noexcept
    {
        if (paramIndex >= exposed.size())
            return -1;
        return exposed[paramIndex];
    }

    bool ParamBridge::clapIdForStoreIndex (int storeIndex, clap_id& outId) const noexcept
    {
        if (table == nullptr || storeIndex < 0 || storeIndex >= static_cast<int> (table->size()))
            return false;
        // Only exposed parameters carry a CLAP id / output lane. `exposed` is small
        // (~40 params) and this is called once per drained GUI edit, not per sample.
        for (int full : exposed)
        {
            if (full == storeIndex)
            {
                outId = (*table)[static_cast<std::size_t> (storeIndex)].uid;
                return true;
            }
        }
        return false;
    }

    int ParamBridge::storeIndexForId (clap_id id) const noexcept
    {
        // Binary search the sorted (uid -> FULL index) table. RT-safe.
        int lo = 0, hi = static_cast<int> (byUid.size()) - 1;
        while (lo <= hi)
        {
            const int mid = lo + ((hi - lo) >> 1);
            const std::uint32_t midUid = byUid[static_cast<std::size_t> (mid)].first;
            if (midUid == id)   return byUid[static_cast<std::size_t> (mid)].second;
            if (midUid <  id)   lo = mid + 1;
            else                hi = mid - 1;
        }
        return -1;
    }

    void emitParamEventsToHost (factory_params::ParamStore& store,
                                const ParamBridge&          bridge,
                                const clap_output_events_t* out) noexcept
    {
        if (out == nullptr)
            return;

        store.drainHostWrites ([&bridge, out] (const factory_params::HostWrite& w) noexcept
        {
            clap_id id;
            if (! bridge.clapIdForStoreIndex (w.index, id))
                return; // legacy / non-exposed parameter: no CLAP output lane

            if (w.kind == factory_params::HostWrite::Kind::Value)
            {
                clap_event_param_value_t ev {};
                ev.header.size     = sizeof (ev);
                ev.header.time     = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type     = CLAP_EVENT_PARAM_VALUE;
                ev.header.flags    = 0;
                ev.param_id   = id;
                ev.cookie     = nullptr;
                ev.note_id    = -1;
                ev.port_index = -1;
                ev.channel    = -1;
                ev.key        = -1;
                ev.value      = static_cast<double> (w.value);
                out->try_push (out, &ev.header);
            }
            else
            {
                clap_event_param_gesture_t ev {};
                ev.header.size     = sizeof (ev);
                ev.header.time     = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type     = (w.kind == factory_params::HostWrite::Kind::GestureBegin)
                                         ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                         : CLAP_EVENT_PARAM_GESTURE_END;
                ev.header.flags = 0;
                ev.param_id     = id;
                out->try_push (out, &ev.header);
            }
        });
    }

    void ParamBridge::valueToText (int storeIndex, double value, char* out, std::uint32_t cap) const
    {
        if (out == nullptr || cap == 0)
            return;
        if (storeIndex < 0 || storeIndex >= static_cast<int> (table->size()))
        {
            out[0] = '\0';
            return;
        }
        const ParamDesc& d = (*table)[static_cast<std::size_t> (storeIndex)];
        const std::string s = factory_params::formatValue (d, static_cast<float> (value), decimalsFor (d));
        std::snprintf (out, cap, "%s", s.c_str());
    }

    bool ParamBridge::textToValue (int storeIndex, const char* text, double* outValue) const
    {
        if (text == nullptr || outValue == nullptr)
            return false;
        if (storeIndex < 0 || storeIndex >= static_cast<int> (table->size()))
            return false;
        const ParamDesc& d = (*table)[static_cast<std::size_t> (storeIndex)];
        float real = 0.0f;
        if (! factory_params::parseValue (d, text, real))
            return false;
        *outValue = static_cast<double> (real);
        return true;
    }
} // namespace factory_shell
