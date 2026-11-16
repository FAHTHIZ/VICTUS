// SPDX-License-Identifier: Apache-2.0
// Luau scripting benchmarks: per-entity update cost for N scripted entities (one `update` call
// each, reading and writing the transform), plus a pure-VM arithmetic loop for reference.
#include "bench_harness.hpp"

#include "victus/core/fs.hpp"
#include "victus/script/script.hpp"

#include <filesystem>

using namespace victus;

namespace {
constexpr const char* kMover = R"lua(
local M = {}
M.speed = 1
function M:start() self.t = 0 end
function M:update(dt)
    self.t += dt
    local p = self.entity.position
    self.entity.position = vector.create(p.x + self.speed * dt, math.sin(self.t), p.z)
end
return M
)lua";
} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    const auto dir = std::filesystem::temp_directory_path() / "victus_script_bench";
    std::filesystem::create_directories(dir);
    if (!fs::write_text_atomic(dir / "mover.luau", kMover)) return 1;
    script::register_components();

    for (const std::size_t n : {1'000u, 10'000u}) {
        scene::Scene s;
        for (std::size_t i = 0; i < n; ++i) {
            const auto e = s.create("e");
            s.world().add<script::ScriptComponent>(e).script = "mover.luau";
        }
        script::HostOptions o;
        o.assets_root = dir;
        auto host = script::ScriptHost::create(o);
        if (!host) return 1;
        gameplay::FrameTime t{0, 1.0f / 60.0f, 0};
        (*host)->update(s, t, nullptr); // instantiate + start
        h.run("script/update_" + std::to_string(n / 1000) + "k_entities", n, [&] {
            ++t.frame;
            (*host)->update(s, t, nullptr);
        });
        if ((*host)->stats().errors) return 1;
    }
    {
        auto host = script::ScriptHost::create({dir});
        if (!host) return 1;
        h.run("script/vm_loop_1M", 1'000'000, [&] {
            auto r = (*host)->eval("local s = 0 for i = 1, 1000000 do s += i * 0.5 end return s");
            bench::do_not_optimize(r);
        });
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return h.finish();
}
