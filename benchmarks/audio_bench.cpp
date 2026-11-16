// SPDX-License-Identifier: Apache-2.0
// Audio mixing benchmark: cost of mixing one 1024-frame stereo block (21.3 ms of audio at 48 kHz)
// with N looping voices, non-spatial and spatial (attenuation + panning against a listener),
// through the real miniaudio node graph without an output device.
#include "bench_harness.hpp"

#include "victus/audio/audio.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace victus;

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    const auto dir = std::filesystem::temp_directory_path() / "victus_audio_bench";
    std::filesystem::create_directories(dir);
    std::vector<float> tone(48000);
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = 0.2f * std::sin(static_cast<float>(i) * 0.0576f);
    if (!audio::write_wav(dir / "tone.wav", tone.data(), tone.size(), 1, 48000)) return 1;
    audio::register_components();
    constexpr std::size_t kBlock = 1024;
    std::vector<float> out(kBlock * 2);
    for (const bool spatial : {false, true}) {
        for (const int voices : {8, 32, 128}) {
            audio::EngineOptions o;
            o.assets_root = dir;
            o.null_device = true;
            auto a = audio::AudioEngine::create(o);
            if (!a) return 1;
            scene::Scene s;
            s.world().add<audio::AudioListenerComponent>(s.create("listener"));
            for (int i = 0; i < voices; ++i) {
                const auto e = s.create("src");
                s.world().get<scene::Transform>(e)->position = {std::cos(static_cast<float>(i)) * 5, 0, std::sin(static_cast<float>(i)) * 5};
                auto& c = s.world().add<audio::AudioSourceComponent>(e);
                c.clip = "tone.wav";
                c.loop = true;
                c.spatial = spatial;
            }
            (*a)->update(s);
            if ((*a)->stats().playing != static_cast<std::uint32_t>(voices)) return 2;
            h.run(std::string("audio/mix_1024_frames_") + std::to_string(voices) + (spatial ? "_spatial" : "_flat"),
                  static_cast<std::uint64_t>(voices), [&] {
                      (*a)->render(out.data(), kBlock);
                      bench::do_not_optimize(out);
                  });
            h.run(std::string("audio/update_") + std::to_string(voices) + (spatial ? "_sources_spatial" : "_sources_flat"), static_cast<std::uint64_t>(voices),
                  [&] { (*a)->update(s); });
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return 0;
}
