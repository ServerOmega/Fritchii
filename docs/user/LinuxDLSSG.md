# DLSS Frame Generation (DLSS-G) on Linux — test build

Fritchii v5.8.4 with native NGX DLSS super resolution (PR #21) plus an experimental native
NGX DLSS Frame Generation path (no Streamline). Untested on hardware at build time.

## Requirements

* NVIDIA RTX GPU, proprietary driver new enough for DLSS-G (RTX 40 for 2x, RTX 50 for 3x/4x).
* The DLSS super resolution library as before (`libnvidia-ngx-dlss.so[.310.9.16]` in
  `~/.local/share/fritchii/ngx` or `~/.local/share/eden/ngx`, or `$EDEN_NGX_DIR`).
* The DLSS-G library `libnvidia-ngx-dlssg.so.310.9.16`. This AppImage bundles NVIDIA's
  release copy in `usr/bin/ngx/` (used last). A copy you put in the same folder as the DLSS
  library wins over the bundled one.
* Frame generation only runs on frames DLSS super resolution evaluated (the games with DLSS
  inputs); everything else is shown as it is.

## Turning it on

Emulation > Configure > Graphics: enable NVIDIA DLSS, then
"DLSS Frame Generation (experimental)": Off (default) / 2x / 3x / 4x.
Or set `EDEN_DLSSG=2|3|4` (0 or 1 forces it off). Restart the game after changing it
(the extensions and the swapchain frame count are chosen at start).

Keep "asynchronous presentation" on (default): the present thread spaces the generated frames.
VSync off (Immediate) or Mailbox gives the most room; FIFO caps the output at the refresh rate.

## Logs

```
grep -E 'DLSS-G|NVIDIA DLSS \(native NGX\)' ~/.local/share/fritchii/log/eden_log.txt
```

In order, expect:

* `DLSS-G: runtime found: .../libnvidia-ngx-dlssg.so.310.9.16`
* `DLSS-G: instance extensions: ...`, `DLSS-G: feature requirements query 0x1: support bits 0x0`
* `DLSS-G: capability: available 1 ... -> up to 4x`
* `DLSS-G: device ready, 2x asked for, running at 2x`
* `DLSS-G: frame generation on at 2x ...`
* in game: `DLSS-G: first frame with DLSS inputs ...`, `DLSS-G: feature created (0x1) ...`,
  `DLSS-G: evaluate ok (0x1) ...`, then every 600 frames `DLSS-G pacing: ...`

Failures say why: `libnvidia-ngx-dlssg.so is missing from ...`, `device lacks ...`,
`NGX reports DLSS-G not available`, `NVIDIA driver too old`, `evaluate failed (0x...)`.
After a failure the session continues without frame generation.

## Debug switches

* `EDEN_NGX_LOG=1` (or `2`, verbose): NGX's own log lines in Fritchii's log.
* `__NGX_LOG_LEVEL=2`, `__NGX_ENABLE_OVERRIDE_LOG_PATH=1 __NGX_LOG_PATH_OVERRIDE=/tmp/ngx`:
  NVIDIA's NGX log files.
* `__NGX_SHOW_INDICATOR=1024`: NVIDIA's on-screen DLSS indicator (release libraries).
* `EDEN_DLSSG_MVEC_SCALE=pixels`: pass motion vector scale in pixels instead of normalized
  (try this if generated frames smear or move the wrong distance).
* `EDEN_DLSSG_SUBRECT=0`: generate over the whole window instead of only the game image.
