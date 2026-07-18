#include "factory_shell/ClapStateBridge.h"

#include <cstdint>
#include <vector>

namespace factory_shell
{
    bool saveState (const factory_params::ParamStore& store,
                    int presetIndex,
                    const clap_ostream_t* stream)
    {
        if (stream == nullptr)
            return false;

        factory_presets::StateModel model;
        model.presetIndex  = presetIndex;
        model.stateVersion = factory_presets::kStateVersionCurrent;
        model.params.reserve (static_cast<std::size_t> (store.size()));
        for (int i = 0; i < store.size(); ++i)
            model.params.emplace_back (store.desc (i).id,
                                       static_cast<double> (store.value (i)));

        const factory_presets::StateBlob blob = factory_presets::encode (model);

        // CLAP streams may accept fewer bytes than requested; loop to completion.
        std::uint64_t written = 0;
        while (written < blob.size())
        {
            const std::int64_t w = stream->write (stream,
                                                  blob.data() + written,
                                                  static_cast<std::uint64_t> (blob.size()) - written);
            if (w <= 0)
                return false;
            written += static_cast<std::uint64_t> (w);
        }
        return true;
    }

    bool loadState (factory_params::ParamStore& store,
                    const clap_istream_t* stream,
                    const std::function<void (factory_presets::StateModel&)>& migrate,
                    int& outPresetIndex)
    {
        if (stream == nullptr)
            return false;

        // Read the whole blob (state.load is [main-thread], so this may allocate).
        std::vector<unsigned char> buffer;
        unsigned char chunk[4096];
        for (;;)
        {
            const std::int64_t r = stream->read (stream, chunk, sizeof (chunk));
            if (r < 0)
                return false;      // read error
            if (r == 0)
                break;             // eof
            buffer.insert (buffer.end(), chunk, chunk + r);
        }

        auto model = migrate
            ? factory_presets::decode (buffer.data(), buffer.size(), migrate)
            : factory_presets::decode (buffer.data(), buffer.size());
        if (! model)
            return false;

        // Apply: model value where present, descriptor default otherwise. setFromHost
        // snaps/clamps to the legal grid and bumps the change epoch (identical landing
        // to a host-driven edit), so absent keys leave no residue from prior state.
        for (int i = 0; i < store.size(); ++i)
        {
            const auto& d = store.desc (i);
            const double v = model->get (d.id, static_cast<double> (d.defaultValue));
            store.setFromHost (i, static_cast<float> (v));
        }

        outPresetIndex = model->presetIndex;
        return true;
    }
} // namespace factory_shell
