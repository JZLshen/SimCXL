# DMA Experiments

This directory packages four bandwidth experiments into one place:

1. Single `CopyEngine` with `N` channels, measure aggregate bandwidth as `N = 1, 2, 4, 8, 16`.
2. `N` `CopyEngine`s with one channel per engine, measure aggregate bandwidth as `N = 1, 2, 4, 8, 16`.
3. Single `CopyEngine` with one channel, sweep `XFERCAP` and measure bandwidth.
4. CPU `memmove` core sweep in gem5, measure aggregate bandwidth as `N = 1, 2, 4, 8` and probe `16`.

## Layout

- `src/`
  - `run_dma_matrix.sh`: reproduce the matrix run in gem5.
  - `run_dma_xfercap_sweep.sh`: reproduce the `XFERCAP` sweep in gem5.
  - `run_cpu_memmove_sweep.sh`: reproduce the CPU `memmove` core sweep in gem5.
  - `extract_and_plot_dma.py`: extract CSV data from the serial log and generate SVG figures.
  - `extract_and_plot_dma_xfercap.py`: extract the `XFERCAP` sweep and generate the SVG figure.
  - `extract_and_plot_cpu_memmove.py`: extract the CPU `memmove` sweep and generate the SVG figure.
  - `cxl_copyengine_bw.c`: symlink to the benchmark source used by the experiments.
  - `cpu_memmove_bw.c`: symlink to the guest CPU `memmove` benchmark source.
  - `x86-cxl-rpc-test.py`: symlink to the gem5 launcher used by the experiments.
- `data/`
  - `engine_sweep.csv`: final data for `N CopyEngine x 1 channel`.
  - `channel_sweep.csv`: final data for `1 CopyEngine x N channels`.
  - `xfercap_sweep.csv`: final data for `1 CopyEngine x 1 channel` with varying `XFERCAP`.
  - `cpu_memmove_sweep.csv`: final data for the CPU `memmove` sweep.
  - `raw/`: copied raw logs from the matrix run, the `XFERCAP` sweep, and the CPU `memmove` sweep.
- `images/`
  - `engine_sweep.svg`
  - `channel_sweep.svg`
  - `xfercap_sweep.svg`
  - `cpu_memmove_sweep.svg`

## Current dataset

The current results were extracted from:

- `output/ce_matrix_16e16c_20260310_010640/board.pc.com_1.device`
- `output/ce_matrix_16e16c_20260310_010640/host.log`

The matrix run used a single guest boot with the board configured as:

- `16 CopyEngine`
- `16 channels` per engine
- KVM boot, then switch to `TIMING`
- `4 MiB` per lane
- `warmup=1`, `loops=3`
- `mode=parallel`

The `XFERCAP` sweep used a separate raw directory:

- `exp/DMA/data/raw/xfercap_20260310_xfercap_1m_256plus/`

The `XFERCAP` sweep used:

- `1 CopyEngine`
- `1 channel`
- KVM boot, then switch to `TIMING`
- `1 MiB` per lane
- `warmup=1`, `loops=3`
- `mode=parallel`
- `XFERCAP = 256B, 512B, 1KiB, 2KiB, 4KiB`

The sweep stops at `4KiB` because the current benchmark requires
`page_size % chunk_bytes == 0`, and the guest page size in this setup is
`4KiB`. Larger `XFERCAP` values would need a different benchmark allocation
strategy or larger guest pages.

`64B` and `128B` were not kept in the final dataset because the guest-side
descriptor construction cost becomes disproportionately high in TIMING mode for
very small chunks, which makes the sweep far less tractable without changing
the benchmark setup path.

The CPU `memmove` sweep used:

- Raw directory: `exp/DMA/data/raw/cpu_memmove_20260310_cpu_memmove_2m/`
- KVM boot, then switch to `TIMING`
- Guest benchmark: `tests/test-progs/cxl-rpc/src/cpu_memmove_bw.c`
- Synchronization: `pthread_barrier`
- `2 MiB` per thread
- `warmup=1`, `loops=1`
- `N = 1, 2, 4, 8` completed and plotted

`2 MiB` per thread was chosen as the reporting point because:

- `1c @ 4 MiB/thread` measured `0.730 GiB/s`
- `1c @ 2 MiB/thread` measured `0.719 GiB/s`
- `1c @ 1 MiB/thread` measured `1.512 GiB/s`, which is clearly inflated by cache effects

So `2 MiB/thread` is much closer to the sustained single-core path than
`1 MiB/thread`, while still keeping the multi-core TIMING runs tractable.

`16c` was started with the same `2 MiB/thread` setup, but it did not reach the
first `cpu_memmove_bw_loop` line within a practical host-time window. The raw
console log stops at the benchmark configuration line in:

- `output/cpu_memmove_16c_20260310_cpu_memmove_2m/board.pc.com_1.device`

Earlier CPU memmove debug directories such as
`cpu_memmove_20260310_cpu_memmove_l1` and `cpu_memmove_20260310_cpu_memmove_pbar`
are kept for traceability but are not part of the reported dataset below.

## Summary

### Part 1: `N CopyEngine x 1 channel`

| N | Avg total bandwidth (GiB/s) |
|---|---:|
| 1 | 4.601 |
| 2 | 8.812 |
| 4 | 12.340 |
| 8 | 14.286 |
| 16 | 13.419 |

Current peak is at `8 CopyEngine`.

### Part 2: `1 CopyEngine x N channels`

| N | Avg total bandwidth (GiB/s) |
|---|---:|
| 1 | 4.786 |
| 2 | 8.812 |
| 4 | 12.444 |
| 8 | 13.951 |
| 16 | 14.152 |

Multiple channels on a single engine do increase aggregate bandwidth, and
the curve approaches the same platform ceiling as the multi-engine case.

### Part 3: `1 CopyEngine x 1 channel`, sweep `XFERCAP`

| XFERCAP | Avg total bandwidth (GiB/s) |
|---|---:|
| 256B | 0.682 |
| 512B | 1.130 |
| 1KiB | 2.059 |
| 2KiB | 3.509 |
| 4KiB | 4.728 |

For a single channel, larger `XFERCAP` substantially improves bandwidth. The
curve is close to linear at small sizes because increasing `XFERCAP` reduces
descriptor traffic and amortizes the serialized `descriptor fetch -> DMA read
-> DMA write` state-machine overhead across more bytes per descriptor.

### Part 4: CPU `memmove` core sweep

| N | Avg total bandwidth (GiB/s) |
|---|---:|
| 1 | 0.723 |
| 2 | 1.415 |
| 4 | 2.787 |
| 8 | 5.675 |
| 16 | not completed |

The `1 -> 8` curve is still close to linear, and the per-core sustained
bandwidth stays around `0.70 GiB/s`. This is far below the DMA-side platform
ceiling measured above, so the current CPU `memmove` experiment has not yet
reached the shared-bus saturation point.
