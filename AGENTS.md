# AGENTS.md — RK3588 YOLOv8-SAM Inspection System

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

- **Target**: aarch64 Linux (Rockchip RK3588). Build is cross-compiled on x86.
- **Output**: `yolov8_sam_demo_qt` (main, Qt Widgets UI) and `yolov8_sam_demo` (headless CLI).
- **Qt**: Uses system Qt5 Widgets (`qtbase5-dev`). UI code lives in `qtui/`; inference pipeline is UI-independent in `core/pipeline.cpp`.
- **Private libs**: `lib/librknnrt.so` and `lib/librga.so` are prebuilt Rockchip libraries — do not attempt to rebuild them.

## Project structure

| Dir | Purpose |
|-----|---------|
| `include/` | RKNN API headers, YOLOv8 inference, common types |
| `utils/` | Runtime components: YOLOv8, MobileSAM, RKNN pool, image/video/camera I/O. Compiled into `yolov8-utils` static lib. |
| `core/` | Application-layer modules: config (JSON hot-reload), CAN bus, GPIO (TCA6424), camera capture, judgment logic, frame bus, logging |
| `qtui/` | Qt Widgets UI: toolbar + video widget (draggable detection lines) + stats dock + log tabs. Bridges to `core/pipeline` via QTimers. |
| `config/` | Runtime config directory (gitignored). Holds `parameters.json` — auto-generated with defaults on first run. |
| `model/` | RKNN model files (not tracked). Expected at runtime in CWD. |

## Entry points

Two modes, selected at build time by `WITH_UI` define:

- **Qt UI** (default target `yolov8_sam_demo_qt`): fullscreen on launch, Esc toggles windowed. Headless CLI still available via `yolov8_sam_demo`.
- **`WITH_UI=0`** (build without UI): CLI-only. Takes input path as first argument.

## Configuration system (`core/config.h`, `core/config.cpp`)

- Custom flat JSON (not standard JSON library). Each line: `"key": value`.
- **Hot-reload**: `poll_hot_reload()` detects external edits via mtime and re-reads the file.
- **Auto-save**: UI changes are written back to disk with 1s debounce via `mark_dirty()` / `poll_save_due()`.
- Config is initialized with: `config::init("/path/to/config/dir", "parameters.json")`.
- `config::g` is the global singleton — thread-safe for reads, caller must serialize writes.
- **Recipes (配方)**: product-param snapshots (model, label, material/box class, target count, line fracs) saved/loaded via `config::save_recipe(name)` / `load_recipe(name)`, stored in `<config_dir>/recipes/<name>.json`. UI toolbar "配方" menu provides save/load.

## Key conventions

- **Paths** in `parameters.json` point to `/home/forlinx/lvgl/...`. Update when deploying elsewhere.
- **Chinese font**: Qt uses system Noto CJK automatically.
- **Qt platform**: xcb on desktop; eglfs/linuxfb available for production kiosk (set `QT_QPA_PLATFORM`).
- **No package manager**: All deps (OpenCV, SDL2, FreeType, spdlog) must be pre-installed on the build machine.
- **RKNN models** (`*.rknn`) are pre-converted Rockchip NPU models. Not committed to git. Placed in `model/` at runtime.
- **Optional components**: SAM, CAN bus, GPIO, and camera each have enable flags in config — code guards them appropriately.

## Naming patterns

- `frame_bus` is a thread-safe mailbox (producer-consumer) for passing inference frames from worker thread to UI thread.
- `judgment` handles multi-box qualification logic (material count vs target, reveal state).
- `camera_capture` wraps a HikVision (Hikrobot) industrial camera via the MVS SDK (`libMvCameraControl`, installed under `/userdata/MVS`, symlinked from `/opt/MVS`).
