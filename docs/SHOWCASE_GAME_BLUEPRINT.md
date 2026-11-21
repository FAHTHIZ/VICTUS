# VICTUS Showcase Game — Research & Production Blueprint

Prepared: 2026-11-21  
Repository: FAHTHIZ/VICTUS, main  
Purpose: select and produce one original, complete game that demonstrates VICTUS through real gameplay—not a renderer demo or disconnected feature tests.

## Executive decision

Build a **third-person cinematic exploration-adventure with physical environmental puzzles, expressive character animation, a compact authored world, and one memorable world-changing set piece**.

### Working title: THE HOLLOW CURRENT

A solitary wayfinder reaches a vast ancient landscape built around a dormant moving structure. Its bridges, water channels, towers, lifts and inhabited chambers are parts of a machine-like ecology that still responds to the world around it. The wayfinder restores isolated mechanisms, learns what the inhabitants were trying to preserve, and decides what should awaken.

The title, story details and visual identity are provisional. Conduct a title/trademark check before release.

- Format: single-player, third-person 3D; keyboard/mouse and gamepad.
- Target: polished 25–40 minute standalone game with a beginning, escalation, climax and ending.
- First production slice: one complete 8–12 minute chapter.
- World: one compact interconnected region, not a seamless continent.
- Visual target: grounded material response and cinematic composition with a distinctive readable art direction. Do not claim photorealism until real-hardware captures support it.

## 1. Research method and findings

Research combined primary developer material, conference presentations, specialist design references, comics/fiction craft and VICTUS's own implementation documentation. A conference abstract is evidence of its stated subject, not proof that a full members-only talk was reviewed. That distinction is preserved below.

### 1.1 Developer and production references

1. **CD PROJEKT RED — “Paths and Possibilities: The Art of Level Design” (AnsweRED Podcast, 2 July 2026).** Level Design Lead Miles Tost and Senior Level Designer Marta Dobińska describe level design as the canvas where gameplay, environment, narrative and quest elements converge. They discuss early location pitches, cross-discipline alignment, player emotional arcs and sandbox opportunities.  
   https://www.cdprojektred.com/en/blog/188/paths-and-possibilities-the-art-of-level-design-transcript-included

2. **Arkane / GDC — “What Happened Here? Environmental Storytelling” (Harvey Smith and Matthias Worch, 2010).** Spaces can imply history through props, scripted events, textures, lighting and composition. Reactive systems can make player agency part of the narrative.  
   https://gdcvault.com/play/1012696/What-Happened-Here-Environmental

3. **Arkane / GDC — “Great Level Design is a Studio-Wide Effort” (Dana Nightingale, 2021).** Great levels depend on communication and contributions across the studio, not only the level designer.  
   https://gdcvault.com/play/1027369/Great-Level-Design-is-a

4. **Naughty Dog / GDC — Uncharted 4 animation workflow and prototyping (Jeremy Yates and Almudena Soria, 2017).** The session overview covers previs, motion capture, transitions, asset tracking and feedback; it also describes how animation prototypes help settle design questions early.  
   https://gdcvault.com/play/1024309/Animation-Bootcamp-Uncharted-4-Naughty

5. **Ubisoft Montreal / GDC — “Motion Matching and the Road to Next-Gen Animation” (Simon Clavet, 2016).** The talk describes searching a motion-capture database at runtime for a pose and future trajectory matching player intent, rather than manually expanding a transition graph. This is an advanced reference, not a VICTUS feature.  
   https://gdcvault.com/play/1023280/Motion-Matching-and-The-Road

6. **Playdead — INSIDE presentations.** The studio links talks on rendering, audio, character sound and immersion. “The Playdead Approach to Audio” describes audio integrated with animation and gameplay; the rendering presentation focuses on a carefully controlled visual system rather than feature accumulation.  
   https://blog.playdead.com/articles/inside_presentations/inside_publications.html  
   Rendering: https://www.youtube.com/watch?v=FMfC47xsImU  
   Audio: https://www.youtube.com/watch?v=Dnd74MQMQ-E

7. **Valve / Half-Life 2.** Developer commentary and analysis are useful references for guiding player attention with architecture, sightlines, characters and environmental motion rather than constant waypoint markers.  
   https://www.youtube.com/watch?v=IjpRG98kRF4  
   https://www.gamedeveloper.com/design/half-life-2-s-animated-architecture

8. **Outer Wilds — interview with creative director Alex Beachum.** Discovery changes the player's understanding of the world; fragments can be found in different orders if they enrich rather than invalidate one another.  
   https://medium.com/@cordialkobold/interview-with-alex-beachum-creative-director-of-outer-wilds-a01bb9631e20

### 1.2 Books and visual storytelling

These are craft references, not works to imitate.

- **Will Eisner, Comics and Sequential Art:** staging, gesture, visual emphasis, spatial continuity and communicating action without explanatory dialogue.
- **Scott McCloud, Understanding Comics: The Invisible Art:** closure, time, motion and the audience's active role in connecting moments. In games, what players infer between events can matter as much as what a cutscene states.  
  https://scottmccloud.com/2-print/1-uc/
- **Jessica Abel and Matt Madden, Drawing Words and Writing Pictures:** visual sequencing, thumbnailing, page rhythm and deliberate composition.
- **The Level Design Book — Worldbuilding:** avoid encyclopedic lore before proving the playable experience. Start with three functional questions: who made this place, who inhabits it now, and how can the player affect it?  
  https://book.leveldesignbook.com/process/preproduction/worldbuilding
- **Emily Short — “Books on Worldbuilding”:** curated entry point for fiction and game-design worldbuilding references.  
  https://emshort.blog/2018/03/06/books-on-worldbuilding/

### 1.3 Principles derived from the research

- Design spaces around player actions, not merely decorate them around a story.
- Every major location needs a past, a present and a player-shaped future, communicated through layout, objects, wear, light, sound and inhabitants.
- Choreograph emotional rhythm: arrival, curiosity, orientation, discovery, rising uncertainty, skill test, payoff and aftermath.
- Use cinematic framing sparingly. A camera move should reveal something; it should not constantly take control away.
- Prototype character animation with gameplay early. Sound must react to space, distance, movement and events.
- Reward observation and inference; avoid lore-document overload.
- A coherent art direction is more valuable than enabling every rendering feature.

## 2. VICTUS capability audit

Based on repository documentation inspected 2026-11-21. This is not a physical-device benchmark; the engine's maturity labels remain authoritative.

### 2.1 Systems the game can demonstrate

| System | Documented capability | Planned use |
|---|---|---|
| 3D rendering | Forward+ clustered PBR, cascaded/local shadows, SSAO, IBL, physical sky/atmosphere, probes, bloom, TAA/upscaling, fog, clouds, decals, LOD and occlusion | Vistas, interiors, material readability, controlled lighting and cinematic composition |
| Assets/materials | glTF/GLB import, PBR materials, cooked cache, texture compression, LOD generation | Original character, architecture, props and materials |
| Animation | Skeletal clips, GPU skinning, cross-fades, 1D/2D blend spaces, state machines, IK, retargeting and root-motion support | Locomotion, interactions, inhabitant performances and sequences |
| Physics | Jolt bodies, character controllers, joints, triggers, queries, ragdolls and convex-hull destruction | Push/pull mechanisms, moving platforms, falling objects and controlled breakage |
| AI/navigation | Recast/Detour, behaviour trees, blackboards, perception and Luau tasks | A small number of purposeful inhabitants with readable routines |
| Sequencer | Transform, animation, audio, camera and event tracks | Opening, one major transformation and ending |
| Audio | miniaudio, spatial audio, buses, streaming and script control | Environmental sound, positional cues and mechanical feedback |
| Gameplay/runtime | ECS, Luau, prefabs, scenes, UI, input, checkpoints/save support, validation and export | Complete loop, progression, menus, feedback and standalone build |
| Particles | CPU emitters, bursts, trails, flipbooks, depth fade and instanced billboards | Dust, debris, mist accents and sparks |

### 2.2 Constraints that shape the design

The repository explicitly states that no subsystem is production-ready yet.

- Rendering performance has not been measured on a real discrete GPU in the documented validation. Local GPU tests use software Vulkan; macOS CI uses a virtualized Apple GPU. GPU timestamp queries are unavailable.
- Linux desktop is the strongest verified target. Windows/macOS builds exist, but interactive editor/player use is not documented as verified. Android APK packaging builds in CI, but no physical Android device has been validated.
- Vulkan is verified; Metal is experimental/CI-tested; D3D12 is unusable because DXIL is not generated.
- Animation does not include motion matching or foot-contact IK. Retargeting lacks humanoid pose correction; use authored, well-matched clips and restrained IK.
- Ragdoll get-up support was added on 2026-11-21, but VICTUS does not ship get-up animation clips; the caller must provide matching clips. Destruction fractures convex hulls, not hierarchical/concave structures.
- Navigation is single-tile, rebuilds as a whole and has no authored off-mesh links or environment queries. Keep the region compact and routes authored.
- Weather, water, fog and clouds have documented visual/functional limitations. Use them as controlled atmosphere, not as simulated fluid or weather gameplay.
- World streaming has constraints including no general cross-cell references or Studio world view. Prefer one compact connected region and explicit scene transitions if needed.
- Runtime asset streaming has limits; editor imports and environment-map baking can be synchronous. Organize and optimize assets early.
- Audio has spatialization but no HRTF, convolution reverb, per-source effects or audio profiling. Real listening tests are mandatory.
- UI has no rich text, sliders or text input. Avoid a UI-heavy design.

Engine references:
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/KNOWN_LIMITATIONS.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/STATUS.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/PLATFORM_SUPPORT.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/ANIMATION.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/SEQUENCER.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/RENDERING_SPEC.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/GAMEPLAY_SPEC.md
- https://github.com/FAHTHIZ/VICTUS/blob/main/docs/PRODUCTION_PIPELINE_SPEC.md

## 3. Recommended creative direction: THE HOLLOW CURRENT

**Genre:** third-person exploration-adventure with physical puzzles and light, readable encounters.  
**Tone:** wondrous, mysterious, melancholic, ultimately hopeful.  
**Player fantasy:** become a wayfinder who understands and awakens a vast place by observing it, moving through it and physically changing it.  
**Core promise:** the environment is not a backdrop; it is the central character and the main gameplay instrument.

### Premise

A wayfinder arrives at a remote structure that is part landscape, part machine and part habitat. Its inhabitants have disappeared from the outer districts, but their routines remain embedded in the architecture: a bridge still tries to complete its old cycle; water still follows a channel to a dry basin; a lift waits for a counterweight; a signal repeats from a tower whose audience is gone.

The player discovers that the structure was designed to preserve a living system through a long period of change. Its mechanisms have become isolated. Restoring them reveals not a simple catastrophe, but a difficult choice about what kind of future the place should have.

The story must be legible without collectible lore. Players should understand the emotional stakes from actions, spaces and character behaviour.

### Design pillars

1. **A world that communicates:** each major space has a readable history, current purpose and visible consequence when changed.
2. **Physical curiosity:** push, pull, turn, carry, redirect, activate and observe, with satisfying animation, audio and physics feedback.
3. **Expressive movement:** starts, stops, turns, climbs, balances, reaches, braces and reacts. No sliding mannequin or idle-only placeholder.
4. **Cinematic moments earned by play:** exploration flows into authored sequences without confusing camera cuts or long exposition.
5. **One coherent art direction:** strong silhouettes, scale, material contrast, composition and intentional color language.
6. **A complete game:** beginning, progression, climax, resolution, checkpoint/save behaviour and distributable build.

### Core loop

1. Read the space: notice a sound, moving object, light, route, inhabitant or broken mechanism.
2. Form a hypothesis about how the mechanism works.
3. Interact: traverse, manipulate objects, redirect a mechanism or respond to an inhabitant.
4. Observe the consequence: the environment changes visibly, audibly and persistently.
5. Use the new state to reach a route, reveal a memory, help an inhabitant or unlock the next space.
6. Reflect and continue, with a quiet beat that communicates what changed and raises the next question.

Avoid repetitive key hunts and arbitrary switches. Each puzzle teaches a rule, then combines it with a new spatial or physical constraint.

## 4. Game structure

### Chapter 0 — Arrival (3–5 minutes)
- Playable opening, not a long movie.
- Establish scale with a staged vista and immediately controllable character.
- Teach movement, camera, interaction and one simple physical manipulation.
- One short sequencer event shows the world reacting.
- End with the first mechanism awakening.

### Chapter 1 — The Dryworks (8–12 minutes)
- Compact exterior-to-interior route with a landmark visible from several locations.
- Water-routing puzzle using channels, gates and counterweights. Water is visual/environmental presentation, not fluid simulation.
- One inhabitant/guardian with a readable routine and behaviour-tree reaction.
- Traversal challenge with authored animation.
- Environmental reveal through props, wear, sound and composition.
- A major mechanism moves, changes the skyline and opens the next route.

### Chapter 2 — The Suspended Quarter (8–12 minutes)
- Vertically layered space around lifts, bridges and counterweights.
- Combine learned mechanics rather than introducing unrelated systems.
- Controlled set piece: a support fails, objects move, the player escapes and a route changes. Use supported convex objects and authored replacement geometry.
- One character interaction changes the player's understanding of the inhabitants.
- Lighting and spatial audio make the location feel different before and after activation.

### Chapter 3 — The Heartway (6–10 minutes)
- Bring earlier visual and mechanical motifs together.
- Final multi-step interaction using observation, traversal and physical manipulation.
- One cinematic transformation using sequencer, animation, camera and audio.
- Final player decision expressed through action, not a dialogue wheel.
- Concise ending showing the world state the player created.

Target length: 25–40 minutes. If evidence says scope is too large, reduce chapter count or encounter complexity; do not reduce playability, animation quality, polish or testing.

## 5. Signature set piece: The Turning Causeway

The player reaches a broken causeway spanning a deep chamber. A distant structure is visible through mist. The player discovers that the bridge is a counterweighted mechanism: a movable mass, a locking arm and a power linkage must be placed in sequence. Each action changes the mechanism physically and audibly. As the final lock releases, the causeway rotates into position, chamber lights wake in sequence, an inhabitant reacts, and the camera briefly reframes the new route before control returns.

This proves third-person controller/camera, interaction, physics, an articulated mechanism, materials, shadows, atmosphere, particles, spatial audio, AI reaction, animation, sequencer camera/event tracks, checkpoint and a meaningful gameplay payoff.

The event must work in normal gameplay if the cinematic camera is disabled. The sequencer enhances presentation; it must not be the only thing making the puzzle function.

## 6. Visual, character and audio direction

### Visual language
- Limited palette: natural stone/ceramic/metal for the old structure, a restrained living accent for active systems, and a separate readable color for interactables.
- Establish scale with foreground silhouettes, midground traversal routes and distant landmarks.
- Use PBR materials with real roughness/metalness variation; avoid uniform gloss.
- Lighting directs attention; it does not simply brighten every corner.
- Strong silhouettes for protagonist and interactables.
- Compose vistas like deliberate comic panels: one dominant read, supporting details and a controlled reveal.
- Fog, clouds, reflections, bloom and depth effects are used only where they improve legibility and mood.
- Use original or properly licensed assets; keep an asset provenance and license ledger.

### Minimum character performance set
Idle with weight shifts; walk; jog/run; start; stop; directional turns; jump; fall; land; reach/interact; push/pull; brace; operate mechanism; balance/step-over where required; hit/react/recover if encounters remain; one or two expressive story gestures.

Check transitions at real movement speeds. Reject foot sliding, abrupt pose pops, mismatched hand placement and animation that ignores collision.

### Audio
- Layer environmental bed, localized mechanisms, distant life and close player movement.
- Important interactions need distinct start, loop and completion cues.
- Spatial placement hints at unseen spaces and directs attention.
- Ambience changes after the player changes the world.
- Integrate animation timing and sound events.
- Listen on actual speakers/headphones and in the packaged build; automated mix tests do not replace listening.

## 7. Vertical slice: exact scope and exit criteria

Build one complete, polished 8–12 minute slice before the rest of the game:

1. Start screen and new-game flow.
2. Player spawns at the arrival overlook.
3. Functional third-person camera and movement.
4. Short traversal route with jump/landing and an interaction.
5. Physical puzzle with two or three linked mechanism states.
6. One animated inhabitant/guardian with a simple routine and reactive behaviour.
7. Environment transformation that changes traversal.
8. Authored cinematic beat with camera, animation, audio and event tracks.
9. Checkpoint/save and reload.
10. Pause, restart, credits/ending using supported UI.
11. Standalone export with all assets/audio included.
12. Repeatable automated play-through or deterministic gameplay smoke test.

**All exit criteria are mandatory:**
- A new player can finish without developer instructions.
- No stuck states caused by collision, camera or navigation.
- Critical characters have complete correctly bound clips; no placeholders.
- No missing textures, broken materials, audio gaps or obvious loading stalls.
- Puzzle is understandable through in-world cues and has reliable state transitions.
- Cinematic begins/ends cleanly and restores correct control/camera state.
- Save/reload preserves progression without duplicating or losing objects.
- Export runs from a clean output directory without editor dependencies.
- Test report records machine, GPU, driver, build, settings, frame-time distribution, memory and known issues.
- A human playtest finds no blocker; critical feedback is fixed before expansion.

## 8. Production gates

### Gate A — Technical preflight
- Build current main from a clean checkout; run full CTest and sample tests.
- Run victus check on a minimal project.
- Verify exact public APIs/editor workflows for controller, interaction, AI, sequencer, save/load and export.
- Confirm target platform and real test hardware before art production.
- Record API gaps as engine issues, not project-only shortcuts.

Exit: no content production until the core loop is proven with graybox assets.

### Gate B — Graybox gameplay
- Build the arrival overlook, causeway room and connected route with primitive geometry.
- Implement camera, movement, interaction, puzzle state, mechanism animation, one AI reaction and checkpoint.
- Test the complete loop with placeholder visuals.
- Measure CPU frame times, memory, loading and input response.

Exit: the slice is fun and understandable without final art.

### Gate C — Art and animation target
- Lock protagonist model, skeleton, scale, materials and import settings.
- Produce the minimum complete animation set.
- Create one representative exterior, interior and causeway set piece.
- Validate lighting at multiple camera positions and real target hardware.
- Profile the target GPU and revise art budgets.

Exit: art direction and character performance are achievable on target hardware.

### Gate D — Complete vertical slice
- Add audio, particles, interaction feedback, sequencer beat, UI, save/load, failure recovery and export.
- Playtest with people who did not build the level.
- Fix causes of confusion, not merely add instructions.
- Capture real gameplay footage from the packaged build.

Exit: all slice criteria pass.

### Gate E — Full game
Only after Gate D: expand the same visual language and mechanic grammar; add story only where it deepens understanding or changes play; keep new features reusable through public engine systems; add automated smoke coverage as chapters are added.

### Gate F — Release candidate
Clean clone/build/export; full automated tests and manual playthrough; platform claims only for validated platforms; asset licenses and notices; controls, accessibility basics, credits, known issues and README; gameplay capture with exact hardware/settings.

## 9. Non-negotiable engineering rules

1. No fake proof: screenshots, video and performance claims come from a real VICTUS build.
2. No external engine substitution. External content tools are acceptable; runtime renderer substitution is not.
3. No hardcoded demo-only bypasses. Use public APIs; add missing reusable capability to VICTUS with tests/docs.
4. Graybox geometry is temporary and Gate B only; no placeholder characters in milestone builds.
5. No feature-count race. Every feature serves player experience and is demonstrable.
6. No giant open world until streaming and real-hardware performance justify it.
7. No motion-matching promise. Use existing state machines/blend spaces and authored clips.
8. No fake water simulation. Use channels, gates, animated surfaces and visual flow cues.
9. No building-scale collapse dependency. Use supported convex destruction and authored set pieces.
10. No untested platform promises.
11. Every major interaction gets tests for start, state transition, interruption, reload and completion.
12. Work directly in main as requested, with small tested reversible commits. Never force-push or overwrite unrelated work.

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Visual ambition exceeds target GPU | Benchmark representative scene early; budget geometry, textures, shadows, transparency and effects |
| Character feels like a prototype | Secure final-quality model and complete clips before expanding content |
| Puzzle state is brittle | Explicit state/events, serialization, tests for transitions and reload |
| AI looks artificial | Few agents, each with one clear goal and tested reaction |
| Cinematics interrupt play | Keep short, skippable where appropriate, and restore state |
| Lore expands endlessly | Functional worldbuilding; each detail supports a space, mechanic, character or reveal |
| Scope grows | Freeze slice scope; require exit criteria before chapter expansion |
| Engine gaps are hidden | Publish capability/limitation matrix and track reusable engine work separately |

## 11. Research-to-design traceability

| Research insight | Design decision |
|---|---|
| CDPR: level design connects environment, gameplay, quest and emotional arc | Plan each chapter as player emotions and playable beats |
| Arkane: spaces imply history and react to agency | Environment tells the story and changes after player action |
| Naughty Dog: animation prototyping informs design early | Prototype interactions in graybox before final art |
| Ubisoft motion-matching research | Treat locomotion as a risk; do not claim motion matching exists in VICTUS |
| Playdead: controlled visual language and sound/gameplay integration | Build one art-directed scene and reactive audio before scaling |
| Valve: architecture and characters guide attention | Use landmarks, light, silhouettes and staging instead of constant waypoints |
| Outer Wilds: discovery changes understanding | Connected discoveries; avoid lore that only works in one order |
| Eisner/McCloud: framing, gesture, closure and rhythm | Compose spaces and cinematic beats as visual sequences |
| Level Design Book: avoid premature lore | Answer who made the place, who lives there and how the player changes it |

## 12. Immediate next actions

1. Treat this document as the research baseline, not proof the game is implemented.
2. Audit current main against the slice, including exact script API and editor support.
3. Build the causeway graybox and temporary controller in the intended sample/project structure.
4. Add automated puzzle and checkpoint tests before final assets.
5. Choose the protagonist/asset pipeline only after a test character imports, animates, interacts and exports correctly.
6. Capture the packaged VICTUS player and evaluate it against the visual target.
7. Greenlight the remaining chapters only after the vertical slice passes.

## Further study list

- Dana Nightingale, “Great Level Design is a Studio-Wide Effort,” GDC 2021.
- Harvey Smith and Matthias Worch, “What Happened Here? Environmental Storytelling,” GDC 2010.
- Simon Clavet, “Motion Matching and the Road to Next-Gen Animation,” GDC 2016.
- Jeremy Yates and Almudena Soria, Uncharted 4 animation workflow/prototyping, GDC 2017.
- Playdead, INSIDE rendering and audio presentations.
- Valve, Half-Life 2 developer commentary.
- Alex Beachum, Outer Wilds creative-director interview.
- Will Eisner, Comics and Sequential Art.
- Scott McCloud, Understanding Comics.
- Jessica Abel and Matt Madden, Drawing Words and Writing Pictures.
- The Level Design Book, worldbuilding and preproduction.
- Emily Short, “Books on Worldbuilding.”

## Scope exclusions

The first release excludes open-world streaming, multiplayer, procedural narrative, motion matching, full-body contact IK, building-scale destruction, simulated fluid dynamics, photorealistic facial capture, a large cast, branching dialogue trees and unvalidated Android performance.

These exclusions protect the goal: one finished game that makes VICTUS's real capabilities visible and its limitations honestly measurable.
