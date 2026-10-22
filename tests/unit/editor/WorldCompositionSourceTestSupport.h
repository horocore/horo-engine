#pragma once

#include <string>

namespace Horo::Editor::WorldCompositionTests {
    inline std::string WorldSource() {
        return R"({"version":1,"world":"00000000-0000-0000-0000-000000000001",
                "bounds":{"minimum":[-1000,-1000,-1000],"maximum":[1999,1999,1999]},
                "grid":{"origin":[0,0,0],"cellMillimeters":1000,"bounds":[-1,1,-1,1,-1,1],"lodLevels":1},
                "layers":[{"id":0,"name":"Terrain","ownership":0,"flags":0,"priority":1}],
                "pages":[{"asset":"00000000-0000-0000-0000-000000000002","revision":1,"objects":[
                    {"id":1,"source":"00000000-0000-0000-0000-000000000003",
                     "bounds":{"minimum":[-1,0,0],"maximum":[1000,0,0]},"placement":0,"layer":0}]}]})";
    }
}  // namespace Horo::Editor::WorldCompositionTests
