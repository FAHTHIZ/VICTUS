# In-game UI: VICTUS `gameui` against Unity UI Toolkit 6.6/6.7 and Unreal UMG/Common UI 5.8 (2026-11-22)

This note feeds the UI row of [COMPETITOR_BENCHMARK.md](COMPETITOR_BENCHMARK.md) and GAP-29 in the [gap register](ENGINE_LANDSCAPE_AND_GAP_AUDIT.md). It compares documented features only. No competitor was run.

## What VICTUS has (`engine/gameui/include/victus/gameui/gameui.hpp`, c5b6238)

- A UI tree built from scene entities.
  - The root is a `UiCanvas`, scaled from a reference resolution with a width/height match.
  - Elements are `UiRect`s with anchors, pivot, offset and size, like a RectTransform.
  - Visuals: panels, images and text (with optional rich-text markup).
  - Widgets: buttons, sliders, toggles, single-line text input, progress bars and scroll views (wheel, drag and focus scrolling).
  - Vertical and horizontal `UiLayout`.
- `keyboard_navigation` (Up/Down/Enter) and a `blocks_input` modal flag.
- Localized text via `.vstrings` keys. The built-in fonts cover Latin, Cyrillic and Greek, with no shaping or right-to-left support (KNOWN_LIMITATIONS).
- Headless layout and interaction, so it is testable without a GPU. `build_draw_list` produces renderer-independent draw items.
- A visual UI Editor canvas in Studio (ADR-0193).
- Missing (source and KNOWN_LIMITATIONS):
  - World-space UI panels.
  - Declarative data binding (UI is driven from Luau scripts).
  - Style sheets or themes shared across elements.
  - Transition animations.
  - Screen stacks with back navigation.
  - Gamepad glyphs.
  - Text shaping and right-to-left text.

## Unity UI Toolkit

- Runtime data binding (Unity 6) binds any plain C# object to control properties. It has binding modes, update triggers and type converters, and can be authored in UI Builder or C# ([runtime data binding](https://docs.unity.com/en-us/engine/6000.6/manual/uitoolkits/uielements/uie-data-binding/uie-runtime-binding)).
- World-space panels arrived in 6.2: Panel Settings Render Mode set to World Space, with input through Panel Input Configuration ([world-space UI](https://docs.unity3d.com/6000.2/Documentation/Manual/ui-systems/world-space-ui.html)).
- Changes in 6.6 and 6.7, according to Unity's status post ([State of UI Toolkit in 6.7](https://discussions.unity.com/t/state-of-ui-toolkit-in-unity-6-7/1736756)):
  - 6.6: `backdrop-filter` and `drop-shadow()`, UI Shader Graph vertex stages and mesh modifiers (experimental), zero-allocation `SetText` overloads, and UI Toolkit Profiler modules.
  - In-scene UI authoring with a UI Viewport.
  - 6.7: `[VisualElementComponent]` components.
- Unity's 6.7 comparison table lists these for UI Toolkit, and notes that it lacks custom navigation order, lit world-space UI and 3D objects inside the hierarchy ([UI systems comparison](https://docs.unity.com/en-us/engine/7000.0/manual/uitoolkits/ui-system-compare)):
  - Data binding and global style management.
  - UI anti-aliasing and SVG.
  - Right-to-left text and emoji.
  - A built-in UI test framework.
  - Transition animations.

## Unreal UMG, Common UI and MVVM (5.8)

- Common UI left beta and is production-ready in 5.8. Enhanced Input and Common Input/UI were unified, removing duplicate data assets ([5.8 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes)).
- Common UI provides ([Common UI API](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/CommonUI?lang=en-US)):
  - Activatable widget stacks and queues, with input routed to the topmost active widget.
  - Platform-specific action icons.
  - An analog-cursor preprocessor for controller navigation.
  - Load guards.
  - Style assets for buttons, text and borders.
- The UMG Viewmodel (MVVM) plugin is still Beta. Viewmodels push updates only when a FieldNotify variable changes, which Epic calls more efficient than raw attribute bindings ([UMG Viewmodel](https://dev.epicgames.com/documentation/unreal-engine/umg-viewmodel-for-unreal-engine)). 5.8 added a Context resolver that pulls viewmodels from parent widgets (release notes above).

## Gaps and suggested order

| Gap | Unity | Unreal | Suggested VICTUS step | Size |
|---|---|---|---|---|
| Data binding | Runtime binding (6.0) | MVVM (Beta) | A `UiBind` component mapping an element property (text, value, visible, progress) to a script table path, with one-way and two-way modes, evaluated only when the source changes. This replaces per-frame HUD scripts. | Medium. The highest value. |
| Screen stack and back navigation | Not built in (custom) | Common UI activatable stacks | A `UiScreen` with push/pop by name and Back/Escape handling, and focus restored on pop. Fits `blocks_input`. | Small |
| Gamepad navigation and glyphs | Input System support; no custom navigation order | Common UI analog cursor and action icons | Extend `keyboard_navigation` to 2D spatial navigation with gamepad D-pad and stick, plus an action-glyph image from the input map. | Medium |
| World-space UI | World-space panels (6.2) | `UWidgetComponent` | Render a `UiCanvas` into a texture on a quad entity, with ray-picked input. | Medium |
| Styles | USS | Common UI style assets | A `.vuistyle` asset referenced by elements, with state colours, fonts and radius. | Small |
| Text shaping and right-to-left | Supported | Supported | HarfBuzz plus font fallback. Needs a dependency and licence review. | Large |

## Not reviewed

- Unity uGUI-only features (lit world-space UI).
- Slate performance tooling.
- UI accessibility (screen readers) on either engine.
- Performance numbers.
