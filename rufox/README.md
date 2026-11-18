# Rufox development

The main download, usage and build guide lives in the [repository README](../README.md).

## Layout

- `src/` — core writer, image analysis, partitioning, formatting and CLI.
- `src/ui/` — GTK4/libadwaita application and asynchronous job engine.
- `tools/rufox-helper.c` — privileged helper used through PolicyKit.
- `tests/` — file-backed unit and integration fixtures; no physical disk is needed.
- `packaging/appimage/` — native x86_64/aarch64 AppImage build.

## Development loop

```bash
meson setup build -Dgui=enabled -Dhotplug=enabled -Dudisks=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

`tests/make_fixture.sh` can create the optional ISO fixture used by the scanner tests. Never point write tests at a real device: use the temporary file targets exercised by `tests/test_core.c`.

## Release packaging

From the repository root:

```bash
bash rufox/packaging/appimage/build-appimage.sh
```

The script builds with `/usr` as the AppDir prefix, runs tests, validates desktop and AppStream metadata, bundles the GTK runtime, creates the AppImage and smoke-tests its packaged CLI. A version tag `v*` starts the GitHub Actions release workflow for both supported architectures.

Windows To Go is intentionally exposed as an unsupported capability until WIM deployment and boot configuration are implemented. Do not advertise it as a working feature.
