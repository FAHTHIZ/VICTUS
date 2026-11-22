# Audio: VICTUS against Unity 6.6 and Unreal 5.8 (2026-11-22)

This note feeds the audio rows of [COMPETITOR_BENCHMARK.md](COMPETITOR_BENCHMARK.md) and [KNOWN_LIMITATIONS.md](../KNOWN_LIMITATIONS.md). It compares documented features only. No competitor was run.

## What VICTUS has (`engine/audio/include/victus/audio/audio.hpp`, c5b6238)

- miniaudio mixing with six fixed buses (Master, Music, Sfx, Ui, Ambient, Voice), each with its own volume and mute.
- Per bus, an optional Butterworth low-pass and a send into one shared reverb (four Freeverb comb filters).
- Spatial sources with Inverse, Linear or Exponential attenuation.
- Streaming for long clips, and a voice budget ranked by priority and then audibility. Losing sources become virtual and resume in place; stolen one-shots fade out over 10 ms.
- One-shots with per-clip instance limits.
- Acoustics (ADR-0096):
  - Geometry occlusion using Steam Audio material transmission bands, plus optional five-ray volumetric occlusion.
  - Reverb zones that blend with a fade margin.
  - An underwater muffle filter.
- Hot reload of clips, and a null device for tests and servers.
- The script API (1.1) exposes `audio_playing` and `audio_occlusion`.
- KNOWN_LIMITATIONS lists the gaps: no per-source effects, HRTF, ducking or audio profiling, and a simple comb reverb.

## Unreal Engine 5.8

- MetaSounds is a DSP graph per sound source with sample-accurate timing and audio-rate parameter modulation. Each MetaSound renders as its own graph, in parallel, and can use its own sample rate, buffer size and channel count. Presets and composition allow graphs to be reused ([MetaSounds overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/metasounds-the-next-generation-sound-sources-in-unreal-engine)).
- Audio Modulation replaces Sound Classes and Mixes with generic parameter buses that any source can drive. Quartz schedules audio on time or musical values, independent of buffer size and game-thread timing ([Audio in UE5](https://docs.unrealengine.com/5.0/en-US/audio-in-unreal-engine-5/)).
- New in 5.8 ([5.8 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes)):
  - Expanded MetaSound Node Configuration.
  - MetaSound Templates (Experimental), with a Randomizer reference template.
  - Experimental channel-agnostic types and a Wave Player node.

  A press summary also reports Audio Insights loudness metering and a signal-flow view, a Beta Waveform Editor, Beta subtitles, and WASAPI as the Windows default ([CDM](https://cdm.link/unreal-engine-5-8-and-audio)). Those items were not cross-checked against Epic's text.

## Unity 6.x

- Unity 6.6 imports second- and third-order ambisonic clips, with 9 and 16 channels ([New in Unity 6.6](https://docs.unity.com/en-us/engine/6000.7/manual/whats-new/unity66)).
- The Audio Random Container (since 2023.2, in Unity 6.0) handles clip variation without scripting ([reference](https://docs.unity3d.com/6/Documentation/Manual/AudioRandomContainer-UI.html)):
  - Sequential, shuffle and random playlists, with "avoid repeating last".
  - Randomised volume, pitch and timing.
  - Pulse and offset triggers.
- The Scriptable Audio Pipeline (6.3) uses Burst-compiled generators, effects and root outputs ([manual](https://docs.unity3d.com/6000.7/Documentation/Manual/audio-scriptable-processors.html)). Audio clips became scriptable generators in 6.5/6.6.
- The Enhanced Audio Foundation backend is a substitute on Windows, Mac, iOS, Android and Xbox from 6.5. It is enabled with the `-showAudioFoundationUI` flag ([Audio status update Q2 2026](https://discussions.unity.com/t/audio-status-update-q2-2026/1723396)).

## Gaps and suggested order

| Gap | Unity | Unreal | Suggested VICTUS step | Size |
|---|---|---|---|---|
| Clip variation (footsteps, impacts) | Audio Random Container | MetaSound Randomizer template, Sound Cue random | A `.vsound` container asset with a clip list, shuffle, random or sequential order, no-repeat count, and volume and pitch ranges. Played by AudioSource and `play_one_shot` like a clip. | Small. The highest value for designers. |
| Ducking and state mixes | Audio Mixer snapshots and ducking | Audio Modulation buses and mixes | Bus gain envelopes driven by another bus's level (sidechain), plus named bus "mixes" scripts can push and pop. | Medium |
| Audio profiling | Audio Profiler module | Audio Insights (production-ready in 5.8 per the release summary) | Export `AudioStats` (real, virtual and stolen voices, occlusion rays) as FrameReport counters, so the existing Profiler and Chrome traces show them. | Small |
| Music timing | Not reviewed | Quartz | A sample-clock scheduler for `play_at(beat)`. Keep it out of scope until a template needs it. | Medium |
| Procedural DSP graph | Scriptable Audio Pipeline (code) | MetaSounds (graph) | Not recommended now. The `graph` module could host it later, but it would need a real-time-safe executor. | Large |
| HRTF and ambisonics | Ambisonic import up to third order; spatializer plug-ins | Built-in HRTF options (not reviewed in detail) | Steam Audio is already the material source. An HRTF backend would be a separate ADR with licence review. | Large |

## Not reviewed

- Unity Audio Mixer ducking details.
- Unreal HRTF and spatialization plug-ins.
- Wwise and FMOD integration costs.
- Mobile audio latency on either engine.
