# Experimental Linux NGX DLSS SR (private local build)

Base: Fritchii v5.6 / 696a456641ae83c9f8b6564c57ac311c7ef56799.
This is NOT an upstream release and has not been validated in a game or on an NVIDIA GPU.
Keep the working non-NGX AppImage.

## Runtime installation

NVIDIA's SDK/API declarations were checked at commit
`9981c6350dc58c0ce21f024fd65e31e851c31689` of https://github.com/NVIDIA/DLSS.
The SDK license conditionally permits application redistribution, but requires protective
terms and prohibits imposing copyleft obligations on the SDK (sections 2 and 4.e).
This is not a conclusion that bundling it with GPL Fritchii is permitted. Conservatively,
no NVIDIA SDK object code, driver, or DLSS feature library is linked into or shipped in
this AppImage. Do not treat this as a public redistribution/legal clearance.

Read https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt.
For personal local use, the optional `tools/setup-linux-ngx.sh` script can download the
pinned official SDK archive and release DLSS-SR feature library. Run it with
`--accept-nvidia-license` only after reviewing/accepting NVIDIA's terms. It needs curl,
sha256sum and a C++ compiler; it does not install a driver or need sudo.

The script builds `libnvsdk_ngx.so` from NVIDIA's official `libnvsdk_ngx.a` locally.
This is the public SDK dispatch shim, not the driver NGX library: those APIs must not be
assumed ABI-compatible. The shim and `libnvidia-ngx-dlss.so.310.9.16` go in
`${XDG_DATA_HOME:-~/.local/share}/eden/ngx`. The installed NVIDIA driver must separately
provide `libnvidia-ngx.so.*`. No Windows DLLs are used.

Overrides:
- `EDEN_NGX_SDK_LIBRARY=/absolute/path/libnvsdk_ngx.so`
- `EDEN_NGX_DLSS_DIR=/absolute/path/to/DLSS-feature-libraries`
The feature's driver/default search can also be used; system `/usr/lib` and
`/usr/lib/x86_64-linux-gnu` are considered for feature discovery.

## Enabling / checking

Emulation > Configure > Graphics > "Use NVIDIA DLSS in place of a game's DLSS".
Use Vulkan and initially resolution scale 1x, then restart the game. The Linux panel
shows the loader/model paths, NOT a claim of GPU compatibility. The existing OoT
camera-motion, jitter, depth and motion-vector inputs are retained.

The renderer picks DLAA / Quality / Balanced / Performance / Ultra Performance from
the input-to-output resolution ratio with the same thresholds as the Windows backend:
0.99 / 0.65 / 0.57 / 0.49 / lower. These are not additional independent UI quality
settings. `EDEN_NX2_DLSS_PRESET=J|K|L|M|default` selects the model hint; K is default.
Tonemapped OoT uses unit exposure. HDR uses auto exposure unless explicitly disabled
with `EDEN_NX2_DLSS_AUTO_EXPOSURE=0`; `EDEN_NX2_DLSS_HDR=0` also retains the Windows
backend's override. The current renderer interface supplies no separate exposure texture.
This is Super Resolution/DLAA only, NOT DLSS frame generation or ray reconstruction.

Log: normally `~/.local/share/eden/log/eden_log.txt` (or the configured/portable data
folder). Search for `Linux NGX`, `NVIDIA DLSS`, `NGX:` and `using FSR 3.1`.
Success checkpoints are separate:
- `SDK loaded ... awaiting NVIDIA Vulkan device` only means loader/extension discovery.
- `NVIDIA DLSS available through Linux NGX` means init/capabilities succeeded.
- `Linux NGX DLSS SR feature created` means CreateFeature succeeded.
- `Linux NGX DLSS SR first evaluation recorded successfully` means recording succeeded,
  NOT proof of correct pixels, successful GPU execution, or improved performance.
Any loader/init/feature/evaluation failure leaves or restores the existing FSR path.
An asynchronous evaluation failure may clear one output frame before FSR takes over.

`EDEN_NGX_PROBE=1 <AppImage>` performs a loader/instance-extension probe after GUI
initialization without starting a game or changing the saved DLSS setting. On a GPU-less
host, this is not a render/evaluation test; the GUI also logs its normal Vulkan error.

## Vulkan integration / lifetime

Queries RequiredExtensions before instance/device creation; current SDKs additionally use
the DLSS-specific GetFeatureInstanceExtensionRequirements and
GetFeatureDeviceExtensionRequirements APIs. Missing mandatory requirements disable NGX,
not Vulkan globally. Deprecated EXT buffer-device-address requirements are normalized
to the renderer's KHR feature path; both extensions must not be enabled simultaneously.
The renderer preserves bufferDeviceAddress for NGX and fails over if it is unsupported.

Uses NVSDK_NGX_VULKAN_Init_with_ProjectID with the existing Windows backend's custom
project GUID `8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64`,
NVSDK_NGX_ENGINE_TYPE_CUSTOM, engine version `Fritchii-5.6-linux-ngx`.
No NVIDIA-issued application ID or private key is needed for this documented path.

Feature resources are retired only after GPU completion when changing input/output size
or creation flags. Inputs transition GENERAL -> shader-read-only -> GENERAL; output
remains GENERAL, with read/write dependencies. SDK command recording invalidates the
renderer state cache. NGX is shut down before the Vulkan device is destroyed.

## Build

Use the existing clang/ThinLTO x86-64-v3 settings, Discord enabled and tests disabled:

    cmake -S . -B build -DENABLE_NVIDIA_DLSS=ON \
      -DEGLANTINE_NGX_SDK=/path/to/official/NVIDIA-DLSS-SDK \
      -DUSE_DISCORD_PRESENCE=ON -DYUZU_TESTS=OFF
    cmake --build build --target yuzu -j8

Only SDK declarations are compiled. The optional SDK loader is dlopen'd through its
public exports at runtime. Windows retains its original Streamline backend and panel.
The local Xlib Status/Success/BadValue collision fixes are also needed for this Linux build.

## Unverified risks

No NVIDIA GPU/game was available on the build box. Driver 615 compatibility, extension
and feature negotiation on the user's actual GPU, model loading, resource layouts under
validation, device loss, resize/reset handling, motion-vector direction/scale, jitter sign,
exposure/ghosting/trails and actual FPS need in-game testing. Source compilation and a
successful API return do not establish that the image is correct or that DLSS helps a
CPU-bound scene. This build does not promise a locked 60 FPS.
