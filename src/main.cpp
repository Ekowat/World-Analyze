#include "core/Runtime.hpp"
#include <pl/Mod.hpp>

class WorldAnalysisMod {
public:
    static WorldAnalysisMod& instance() {
        static WorldAnalysisMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) { return worldanalysis::core::Runtime::get().load(context); }
    bool enable(pl::mod::ModContext& context) { return worldanalysis::core::Runtime::get().enable(context); }
    bool disable(pl::mod::ModContext& context) { return worldanalysis::core::Runtime::get().disable(context); }
    bool unload(pl::mod::ModContext& context) { return worldanalysis::core::Runtime::get().unload(context); }
};

PL_REGISTER_MOD(WorldAnalysisMod, WorldAnalysisMod::instance())
