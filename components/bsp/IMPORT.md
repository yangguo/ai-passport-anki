# BSP vendor record

- Upstream: https://github.com/FoloToy/ai-passport
- Pinned upstream commit: `33d3d1d93a1125b356b47b6d83a7a60121be801e` (the same
  baseline locked by `yangguo/ai-passport-chess`; never follow `main` without review).
- Source of this copy: `components/bsp/` from the actual firmware snapshot in
  `yangguo/ai-passport-xiaoxiaole`, branch
  `cursor/xiaoxiaole-v1-impl-19f9`, commit
  `97924dcaa76bd458a3f2a529230b5a4878e79476` (the snapshot was imported into
  `yangguo/ai-passport-chess` at `339957423c7e2426f467a0098bb02e24ca78bdc6`).
  This snapshot is based on upstream commit
  `33d3d1d93a1125b356b47b6d83a7a60121be801e` plus two additions made by the
  sibling project for deep-sleep support:
  - `src/bsp_power.c`, `include/bsp_power.h` (wake-release wait, deep sleep entry);
  - `BSP_BTN_GPIO` in `include/bsp_pins.h`, and the matching `REQUIRES` in `CMakeLists.txt`.
  Those additions are inherited from the snapshot; the Anki project has not
  modified BSP implementation files.
- License: upstream MIT, (c) 2026 FoloToy; full text in `LICENSE`, kept verbatim.
- Not imported: upstream `main/*` demo UI, assets, docs, CI, skills.
- Audio note (design.md section 8): `bsp_audio_init()` opens full-duplex I2S
  (TX and RX, 6 x 240 stereo frames each). Any change to that is a separate,
  explicitly approved BSP patch and must be recorded here.
- LVGL pin (xiaoxiaole): the BSP's own dependency range is left untouched. This
  project pins `lvgl/lvgl: ~9.5.0` in `main/idf_component.yml`, because 9.6.0
  ignores `CONFIG_LV_MEM_SIZE_KILOBYTES` and defaults to a 64 KiB pool.
