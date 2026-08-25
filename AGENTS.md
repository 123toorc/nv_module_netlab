# AGENTS.md

## Cursor Cloud specific instructions

This repo is the **NVIDIA open GPU kernel modules** (v610.43.02, `version.mk`), patched ("netlab") to
export GPU P2P symbols consumed by the `urma_driver` `udma.ko` module.

### Build / run (NOT possible on this VM)
Build is `make modules` (see top `Makefile` / `README.md`), which requires a matching kernel build tree
at `/lib/modules/$(uname -r)/build`. The Cursor Cloud VM runs a custom kernel **`6.12.58+`** for which
no matching kernel headers/source are available (apt only offers `6.8`-series generic headers), so the
kbuild step fails with `/lib/modules/6.12.58+/build: No such file or directory`.

Notes:
- The OS-agnostic C portion under `src/nvidia` compiles with the host gcc, but the final `.ko` link in
  `kernel-open/` needs the kernel tree above.
- Loading `nvidia.ko` additionally requires a physical NVIDIA GPU (absent here).
- Building this repo first produces `kernel-open/nvidia/{nv-p2p.h,Module.symvers}` that `urma_driver`
  depends on.

There is no lint tooling and no in-repo test suite (`CONTRIBUTING.md`); validation is runtime module
load on real hardware. No build dependencies for this repo are installed by the environment update
script.
