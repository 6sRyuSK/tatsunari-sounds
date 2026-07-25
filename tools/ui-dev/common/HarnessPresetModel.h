#pragma once

#include "factory_presets/PresetSession.h"

#include <string>
#include <vector>

// Adapts the shared PresetSession to any plugin editor model with the common
// names/currentIndex/load contract (PfPresetModel, DeqPresetModel, ...).
template <class EditorPresetModel>
class HarnessPresetModel final : public EditorPresetModel
{
public:
    HarnessPresetModel (factory_params::ParamStore& store,
                        const factory_presets::PresetBank& bank,
                        std::vector<std::string> excluded = {})
        : session_ (store, bank, excluded)
    {
    }

    std::vector<std::string> names() const override
    {
        std::vector<std::string> result;
        result.reserve ((std::size_t) session_.numPrograms());
        for (int i = 0; i < session_.numPrograms(); ++i)
            result.push_back (session_.programName (i));
        return result;
    }

    int currentIndex() const override { return session_.currentProgram(); }

    bool load (int index) override
    {
        if (index < 0 || index >= session_.numPrograms()) return false;
        session_.applyProgram (index);
        return true;
    }

private:
    factory_presets::PresetSession session_;
};
