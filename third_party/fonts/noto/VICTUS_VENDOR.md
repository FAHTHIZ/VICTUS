# Noto Sans Bengali and Noto Sans Arabic (vendored fonts)

- Upstream: The Noto Project Authors, https://github.com/notofonts/bengali and https://github.com/notofonts/arabic
- Releases (fetched 2026-11-22, unmodified unhinted TTFs from the release zips):
  - `NotoSansBengali-v3.011` (2026-01-09): https://github.com/notofonts/bengali/releases/download/NotoSansBengali-v3.011/NotoSansBengali-v3.011.zip (zip SHA-256 2aca24bf71665e66c32e14862ee58857dd8b8b07b6321ec7c96f3121ad6683b1)
  - `NotoSansArabic-v2.013`: https://github.com/notofonts/arabic/releases/download/NotoSansArabic-v2.013/NotoSansArabic-v2.013.zip (zip SHA-256 1301aceaea84c501cf2e6dcfb3182e2328c8eae5725817fcb239672bda7154f1)
- Files (path inside each zip: `<Family>/unhinted/ttf/`), SHA-256:
  - NotoSansBengali-Medium.ttf 9539cb08dd3acc287ab6422041cce3bf722f1637f44098750998014f3add681d
  - NotoSansBengali-Bold.ttf 2fe8d779e0f19576d1fc11e39f9154b4326a17b693215017dd1f5a8898c3f2c3
  - NotoSansArabic-Medium.ttf a66dfdfd2fd345d500a8f169d07297eb29fc683938c61d6bdd5717cafca86d51
  - NotoSansArabic-Bold.ttf f4cb79f842a07d1bcf597879574358fb7010f73761396d1d0f2445dfedb4d2ca
- License: SIL Open Font License 1.1 (`OFL-Bengali.txt`, `OFL-Arabic.txt`, copied from each zip). Embedded unmodified in the runtime as fallback faces after Inter (ADR-0224); the OFL permits bundling with software as long as the fonts are not sold on their own and the license ships with them.
- Weights: Medium (500) and Bold (700) match the built-in Inter Medium and Inter Bold.
- Size: about 490 KB for the four files.
