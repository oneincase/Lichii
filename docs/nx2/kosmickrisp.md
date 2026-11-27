# KosmicKrisp (Vulkan through Mesa) on macOS

macOS always presents Vulkan through Metal. The build ships MoltenVK for that, and can also run
[Mesa's KosmicKrisp](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/drivers/kosmickrisp.rst)
driver, which is a separate Vulkan implementation for the same GPUs.

## Choosing it

Graphics → API lists the implementation as part of the API on macOS:

- `Vulkan (MoltenVK)` — the shipped default; `VulkanDriver::Auto` resolves to it.
- `Vulkan (KosmicKrisp)` — listed and selectable only when an install is found.
- `Null`

The choice is per game like the other renderer settings. It takes effect at the next boot of a
game, not while one runs; nothing needs restarting.

## Requirements

| | |
|---|---|
| Hardware | Apple silicon. KosmicKrisp does not support Intel Macs. |
| macOS | 26 or newer. KosmicKrisp is built on Metal 4. |
| Driver | Mesa 26.3 or newer (`brew install mesa`, `brew upgrade mesa`, or a Vulkan SDK that ships KosmicKrisp). Older builds miss features and presentation fixes this build relies on. |

ICD discovery order:

1. `EDEN_NX2_KK_ICD=<path>` (explicit override).
2. Lichii's own directories: `$XDG_DATA_HOME/lichii/vulkan/icd.d` (default
   `~/.local/share/lichii/vulkan/icd.d`) and `$XDG_CONFIG_HOME/lichii/vulkan/icd.d`. An
   implementation placed here is the one the app runs, ahead of any package manager's.
3. `$VULKAN_SDK/share/vulkan/icd.d`.
4. Homebrew (`share/vulkan/icd.d`, `etc/vulkan/icd.d`, and versioned `Cellar/mesa/*` directories),
   `/usr/local`, `~/.local/share/vulkan/icd.d`, `/etc/vulkan/icd.d`.

The development machine ships a locally built Mesa `main` (currently `Mesa 27.0.0-devel`,
Vulkan 1.4.368) as `~/.local/share/lichii/vulkan/icd.d/kosmickrisp_icd.json`, pointing at
`/Users/oneincase/Documents/workspace/mesa-preview/build-kk/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib`.
That makes it the default KosmicKrisp. Rebuilding that checkout's `build-kk` updates the default in
place; if the manifest's library disappears, discovery falls through to Homebrew's Mesa. To go back
to a release build, delete the manifest or point `EDEN_NX2_KK_ICD` at another ICD.

When KosmicKrisp is not found the entry is hidden and the tooltip says why.

## How it is loaded

KosmicKrisp is a Vulkan ICD: it exports only `vk_icdGetInstanceProcAddr`, so it needs the Khronos
loader. The macOS `OpenLibrary()` therefore prefers the loader (bundle, `$VULKAN_SDK`,
`/opt/homebrew/lib`, `/usr/local/lib`) and only falls back to loading the bundled MoltenVK
directly, which leaves KosmicKrisp unavailable.

`Vulkan::PrepareDriverEnvironment()` runs once, before the loader is first used, and points
`VK_DRIVER_FILES` at exactly the manifests this build wants:

- the bundled MoltenVK manifest (`Contents/Resources/MoltenVK_icd.json` → `../Frameworks/libMoltenVK.dylib`),
- the discovered KosmicKrisp manifest.

This is deliberate: the loader's default search only found KosmicKrisp (plus llvmpipe) on a stock
Homebrew host and missed Homebrew's MoltenVK. An existing `VK_DRIVER_FILES` or `VK_ICD_FILENAMES`
in the environment is respected instead of overwritten.

Both ICDs stay enumerated for the process; the per-game choice happens when the device is created
by matching `VkPhysicalDeviceDriverProperties::driverID` (`VK_DRIVER_ID_MOLTENVK` or
`VK_DRIVER_ID_MESA_KOSMICKRISP`) and never by device index, whose order is not stable.

## Logs and overrides

Boot logs include:

```
Render.Vulkan <Info> Vulkan ICD: MoltenVK (Vulkan 1.4.0) at .../libMoltenVK.dylib
Render.Vulkan <Info> Vulkan ICD: KosmicKrisp (Vulkan 1.4.354) at .../libvulkan_kosmickrisp.dylib
Render.Vulkan <Info> VK_DRIVER_FILES=.../MoltenVK_icd.json:.../kosmickrisp_mesa_icd.aarch64.json
Render.Vulkan <Info> Using Vulkan library /opt/homebrew/lib/libvulkan.1.dylib
Render.Vulkan <Info> Vulkan driver KosmicKrisp (Mesa 26.3.0), Vulkan 1.4.360: device 0 of 2
```

| Variable | Meaning |
|---|---|
| `EDEN_NX2_VULKAN_DRIVER=mvk\|kk` | Picks the implementation for A/B runs and headless tools, before any setting is read. |
| `EDEN_NX2_KK_ICD=<path>` | Uses that ICD manifest instead of the discovered one. |
| `VK_DRIVER_FILES` / `VK_ICD_FILENAMES` | Left alone when set by the user. |

## Memory

Host memory grows while a game loads and compiles pipelines (guest environments, driver pipeline
data, and cache reads), and the process footprint can sit well above the emulator's own Vulkan
device memory; the texture and buffer caches themselves stay flat. `EDEN_NX2_SWAPCHAIN_TRACE=1`
prints both figures every 10 s, so a real leak shows up as device memory or footprint growing
while the game idles.

## Known differences

- `eden-cli --replay` cannot use KosmicKrisp: the driver has no `VK_EXT_headless_surface`, so it
  has no present queue without a window. Replays stay on MoltenVK.
- KosmicKrisp has no `VK_EXT_transform_feedback` (Mesa 26.2) and no `VK_EXT_custom_border_color`
  unless `MESA_KK_EXPERIMENTAL=custom_border` is set; both degrade the way they do on other
  drivers that lack them.
- The macOS required-feature fallbacks (geometry shaders, wide lines, non-solid fill, logic op,
  cull distance) are shared by both Metal implementations through `IsAppleVulkan()`.
- Extended Dynamic State stays off on macOS for both drivers (MoltenVK renders black screens with
  it); it is not turned on for KosmicKrisp before it is validated.

## Packaging

The macOS app bundle is self-contained for KosmicKrisp:

- `Contents/Frameworks/libvulkan.1.dylib` — the Khronos loader (Apache-2.0).
- `Contents/Frameworks/libvulkan_kosmickrisp.dylib` — the bundled Mesa build (currently
  `27.0.0-devel`), with its `libSPIRV-Tools.dylib` beside it and its install name rewritten to
  `@loader_path/libSPIRV-Tools.dylib` by a post-build step.
- `Contents/Resources/vulkan/icd.d/Kosmickrisp_icd.json` — points at `../../../Frameworks/...`.

A CMake configure picks the driver up with:

```sh
cmake -S . -B build -G Ninja \
  -DLICHII_BUNDLED_KOSMICKRISP_DYLIB=.../build-kk/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib \
  -DLICHII_BUNDLED_SPIRV_TOOLS_DYLIB=/opt/homebrew/opt/spirv-tools/lib/libSPIRV-Tools.dylib
```

Without those options the loader is still bundled but KosmicKrisp is not, and the app falls back to
whatever the host has.

The bundled driver is only offered to the loader on Apple silicon with macOS 26 or newer
(`MeetsKosmickrispOsRequirement()`); older systems keep MoltenVK only. A KosmicKrisp the user
installs themselves still wins over the bundled one (see the discovery order above).
