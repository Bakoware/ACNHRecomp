// Animal Crossing: New Horizons (01006F8002326000, 3.0.2): what the shared runtime needs to know about the game.
// The game draws through NVN (docs/upstream/NVN.md); what we know about it is in docs/ACNH.md.
#include <cstring>

#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"acnh", "Animal Crossing: New Horizons", 0x01006F8002326000ull,
                                "io.github.acnhrecomp", "private/acnh",
                                0 /* main.npdm: no system resource */};

namespace {

// The engine splits its memory into fixed budgets, a table in main's read-only data (3.0.2: main+0x404d658):
// cSys, cGfx, cRes, cSnd, cScene, cOthers (the last one is created with size 0: whatever is left).
// cSys runs out here before the title (AppSysHeap, created last in cSys with "the rest", is ~11 MB and a 3.8 MB
// allocation fails, main+1450630): the engine creates task heaps in cSys with "the rest" too and trims them when
// the task is done, and our threads do not run in the console's order, so a heap is created while another is not
// trimmed yet. 64 MiB move from cOthers (1.6 GB, a remainder) to cSys. Only applied when the table holds the
// values of 3.0.2.
constexpr uint64_t kBudgetTable = 0x404d658;
constexpr uint64_t kBudgets[6] = {0xa8df000, 0x9900000, 0x4a271f8, 0x900000, 0x23d00000, 0x65e00000};
constexpr uint64_t kSysExtra = 64ull << 20;

uint64_t main_base() {
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    return m ? m->base : 0;
}

void widen_sys_budget() {
    uint64_t base = main_base();
    if (!base) return;
    uint64_t t = base + kBudgetTable;
    for (int k = 0; k < 6; k++)
        if (ld<uint64_t>(t + 8 * k) != kBudgets[k]) {
            log("acnh: memory budget table not as in 3.0.2: left alone");
            return;
        }
    st<uint64_t>(t, kBudgets[0] + kSysExtra);
    st<uint64_t>(t + 8 * 5, kBudgets[5] - kSysExtra);
    log("acnh: cSys budget %#llx -> %#llx (from cOthers)", (unsigned long long)kBudgets[0],
        (unsigned long long)(kBudgets[0] + kSysExtra));
}

}  // namespace

void game::after_init() { widen_sys_budget(); }
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
