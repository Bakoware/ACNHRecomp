// Animal Crossing: New Horizons (01006F8002326000, 3.0.2): what the shared runtime needs to know about the game.
// The game draws through NVN (docs/upstream/NVN.md); what we know about it is in docs/ACNH.md.
#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"acnh", "Animal Crossing: New Horizons", 0x01006F8002326000ull,
                                "io.github.acnhrecomp", "private/acnh",
                                0 /* main.npdm: no system resource */};

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
