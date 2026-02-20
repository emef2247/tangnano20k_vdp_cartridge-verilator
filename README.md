# tangnano20k_vdp_cartridge-verilator

This repository provides a Verilator-based harness for the tangnano20k VDP cartridge project with:
- raw VDP pixel export (screen_pos_x/screen_pos_y + vdp_r/g/b)
- a C++ wrapper test harness that can sample raw VDP pixels and write PPM frames
- runtime VCD control (open/close and dump ON/OFF) from CSV scenarios
- helpers to limit or post-filter VCD output

This README explains how to build, run, and use the harness and the CSV VCD-control commands.

---

## Requirements

- Linux / WSL (Linux recommended for fastest I/O)
- Verilator (recommended >= 4.0, tested with recent stable)
- g++ (C++11 or newer)
- Python 3 (optional, for VCD post-filtering helper)
- make

Install on Debian/Ubuntu:
```bash
sudo apt update
sudo apt install verilator g++ make python3
```

---

## Quick build

From the repository root:

1. (Optional) Clean previous build artifacts:
```bash
make clean || rm -rf obj_dir
```

2. Build with Verilator:
```bash
make msx2logo
```
or, use the default target:
```bash
make
```

Notes:
- The project's Makefile invokes `verilator --cc --exe --build --trace ...` by default for debug builds.
- If your build system requires the `VM_TRACE` macro to enable the trace code path in C++ (some setups), add `-DVM_TRACE` to the Verilator flags. Example:
```bash
# add VM_TRACE to the Verilator compile defines
make VERILATOR_FLAGS+=" -DVM_TRACE"
```
(You can also edit the Makefile to include `-DVM_TRACE` in `VERILATOR_FLAGS`.)

---

## Running

After a successful build the executable is under `obj_dir/` (or at the top-level `Vwrapper_top` depending on your Makefile). Example run:

```bash
./obj_dir/Vwrapper_top --vcd=trace.vcd --csv=tests/csv/msx2_logo.csv --dump-screen
```

Command-line options:
- `--vcd=PATH`  : enable VCD tracing to `PATH`.
- `--csv=PATH`  : run CSV scenario script at `PATH`.
- `--dump-screen` or `--dump-screen=1` : enable PPM dumping of frames captured from raw VDP pixels.
- `--vramtest`  : run VRAM test scenario (existing test harness scenario).
- `--debug`     : enable verbose debug logging in the wrapper.

Note: The harness sets a default VCD trace depth in code. If you want to modify the trace depth you can change the call to `vdp_cartridge_set_vcd_depth(...)` in `main.cpp` before enabling the VCD.

---

## CSV commands for runtime VCD control

During a CSV-driven scenario you can open/close the trace file and enable/disable dumping (without closing) using these CSV commands:

- `VCD_OPEN,<path?>`  
  Open trace file. Path optional; defaults to `dump.vcd`.
  Example CSV line:
  ```
  VCD_OPEN,dump.vcd
  ```

- `VCD_CLOSE`  
  Close trace file (physically close).

- `VCD_ON,<0|1?>`  
  Enable VCD dumping while the trace file is open. If no argument, defaults to `1`.
  Example:
  ```
  VCD_ON,1
  ```

- `VCD_OFF`  
  Disable VCD dumping while keeping the trace file open. This suppresses `g_tfp->dump()` calls but does not close the file.

Usage pattern:
- `VCD_OPEN` (file is opened, dump enabled by default)  
- `VCD_OFF` (pause dumping to reduce file I/O)  
- ... run cycles ...  
- `VCD_ON` (resume dumping)  
- `VCD_CLOSE` (close file when finished)

This allows dynamic control of trace volume without the cost of file reopen.

---

## Raw VDP pixel capture (PPM)

The wrapper samples the raw VDP outputs (not post-processed HDMI/display signals) and produces PPM images on frame boundaries.

- Enable dump by:
  - passing `--dump-screen` on the command line, or
  - calling `VCD_OPEN`/`VCD_ON` are unrelated to PPM; PPM is controlled by `--dump-screen` / `vdp_cartridge_set_dump_screen(1)`.

- Output files:
  - `display_000000.ppm`, `display_000001.ppm`, ... (one PPM per captured frame)

- Sampling details:
  - Sampling occurs at a stable clock phase chosen for the DUT; if pixels look wrong, adjust sampling phase/edge in wrapper as needed (the code documents where to tune).

---

## Controlling VCD scope (trace depth) and size

`g_top->trace(g_tfp, depth)` registers signals to be traced up to `depth` levels. However:
- If `wrapper_top` exposes many DUT signals at the top level, `depth` alone may still result in many recorded signals.
- Verilator versions or generated code may affect depth behavior.

Options to reduce VCD size:
1. Set trace depth to `0` or `1` before opening the VCD:
   - Modify `main.cpp` or add a CLI parameter to call `vdp_cartridge_set_vcd_depth(0)` prior to `vdp_cartridge_set_vcd_enabled(1, path)`.

2. Use CSV `VCD_OFF` / `VCD_ON` to suspend/resume dumping during long uninteresting runs.

3. Post-filter a generated VCD to only keep `wrapper_top` top-level signals (a small Python helper is provided in the branch). Example:
```bash
python3 tools/filter_vcd.py dump.vcd wrapper_top trimmed.vcd
```
This keeps only top-level vars under `wrapper_top` and prunes submodule signals.

---

## Troubleshooting

- Link error `cannot open output file Vwrapper_top: No such file or directory`  
  - Common causes: `obj_dir/` permissions, stale file locks, Windows Defender/antivirus locking on WSL, or file-system sync issues. Try:
    ```bash
    rm -rf obj_dir
    make msx2logo
    ```
    If `rm` fails, check file attributes (`lsattr -R obj_dir`) and ownership; if under WSL, restarting WSL sometimes clears locks.

- Huge VCD even with depth 0:
  - Confirm `wrapper_top` does not re-export many DUT signals as top ports.
  - Use `VCD_OFF` to pause dumping during long sequences.
  - Use the provided VCD filter script to extract only the wrapper_top signals.

- Wrong-looking captures (single-line stripe):
  - Sampling phase is likely wrong. The wrapper samples at a specific clock edge; adjust sampling (posedge/negedge or sample phase divisor) in `vdp_cartridge_wrapper.cpp` where the capture is performed.

---

## Developer notes

- Key APIs are declared in `src/verilator/vdp_cartridge_wrapper.h`:
  - `vdp_cartridge_set_vcd_depth(int depth)`
  - `int vdp_cartridge_set_vcd_enabled(int enable, const char* path)`
  - `void vdp_cartridge_set_vcd_dump(int enable)` — enable/disable `g_tfp->dump()` while file is open
  - `void vdp_cartridge_set_dump_screen(int enable)` — enable/disable PPM dump of raw VDP frames
  - `void vdp_render_frame_rgb(uint8_t* dst, int pitch)` — render one frame from `display_*` outputs

- VCD dump control:
  - The wrapper calls `g_tfp->dump()` at controlled points (posedge by default) only when `g_tfp` is open and `g_vcd_dump_enabled` is set. This reduces dump frequency and allows CSV-controlled ON/OFF without closing the file.

---

## Examples

Build and run (top-level trace disabled by default; enable with `--vcd`):
```bash
make msx2logo
./obj_dir/Vwrapper_top --csv=tests/csv/msx2_logo.csv --vcd=trace.vcd --dump-screen
```

Example CSV (snippet):
```
# Open trace
VCD_OPEN,trace.vcd
# run cycles...
CYCLE,1000
# Pause dumping
VCD_OFF
CYCLE,50000
# Resume dumping
VCD_ON,1
# Close file later
VCD_CLOSE
```

Filter a large VCD to wrapper_top top-level signals:
```bash
python3 tools/filter_vcd.py dump.vcd wrapper_top trimmed.vcd
```

---

## License

MIT — see project LICENSE.

---

If you want, I can:
- Add a `--vcd-depth=N` CLI option to `main.cpp` and include it in this README.
- Commit the VCD filter helper into `tools/filter_vcd.py` and a short test CSV demonstrating VCD_ON/OFF.