# AMD ROCDXG Library

This fork is a patched `librocdxg` build for getting ROCm to work on an AMD Radeon RX 6700S laptop GPU under WSL 2. 

1. WSL can sometimes see the GPU without being able to actually use it
2. This fork adds the fixes that made real GPU execution work on an RX 6700S
3. You still need the right WSL and bash environment variables when you run ROCm or PyTorch

## Who This Is For

1. Use WSL 2 on Windows
2. Have a gfx1032 GPU.
3. Can get ROCm to partially detect the GPU, but real tensor copies or inference hang

## Tested Device And Software

This is the setup used while testing this fork:

1. GPU: AMD Radeon RX 6700S
2. OS: Windows 11
3. WSL distro: Ubuntu 24.04
4. ROCm: 7.2.x
5. Python: 3.12
6. PyTorch: ROCm 7.2 wheels for Ubuntu 24.04

What worked in testing:

1. `rocminfo` detected the GPU
2. `torch.cuda.is_available()` returned `True`
3. `torch.cuda.device_count()` returned `1`
4. `torch.cuda.get_device_name(0)` returned `AMD Radeon RX 6700S`
5. Repeated host to GPU tensor copies succeeded


## What Changed In This Fork

### 1. The GPU can be forced to run as `gfx1030`

The RX 6700S needed:

```bash
export HSA_OVERRIDE_GFX_VERSION=10.3.0
```

This fork makes that override apply more consistently in the `librocdxg` device and topology paths.

### 2. Software queue allocation was changed

The default software queue path could make the GPU appear alive without actually running work correctly. This fork adds a user controlled queue allocation path through:

```bash
export LIBROCDXG_ALLOC_USER_QUEUE_FROM_UMD=1
```

In plain language, this changes how the GPU command inbox is created so the RX 6700S WSL stack actually accepts it.

### 3. SDMA copy engines are disabled for this setup

The RX 6700S could detect the GPU, but real CPU to GPU tensor copies would hang until timeout unless these were disabled:

```bash
export HSA_ENABLE_SDMA=0
export HSA_ENABLE_PEER_SDMA=0
```

### 4. Dispatch and scratch handling were improved

This fork also includes:

1. missing kernel launch values were added
2. temporary GPU memory settings were copied more reliably

### 5. Queue cleanup was fixed

The UMD allocated software queue path now properly frees its queue memory on teardown.

## Prerequisites

Before building this repo, make sure you have:

1. Windows 11 with WSL 2 installed
2. Ubuntu 24.04 or Ubuntu 22.04 inside WSL
3. A recent Windows AMD driver
4. ROCm installed in WSL
5. Windows SDK installed on Windows
6. `cmake` and `g++` installed in WSL

Useful references:

1. [AMD Drivers](https://www.amd.com/en/support/download/drivers.html)
2. [Install WSL](https://learn.microsoft.com/en-us/windows/wsl/install)
3. [ROCm Installation Quick Start](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/install/quick-start.html)
4. [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/)

## Build And Install

Run these commands inside WSL.

### 1. Clone the repo

```bash
git clone https://github.com/joshEng1/librocdxg.git
cd librocdxg
```

### 2. Confirm ROCm is installed

```bash
ls /opt
```

You should see a ROCm installation, usually something like `rocm` or `rocm-7.2.0`.

### 3. Build the library

The Windows SDK path may vary. The example below uses the SDK version that was used during testing.

```bash
mkdir -p build
cd build
cmake .. -DWIN_SDK='/mnt/c/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared'
make -j$(nproc)
sudo make install
```

If you have a different SDK version, adjust the path under:

```text
C:\Program Files (x86)\Windows Kits\10\Include\
```

## WSL Environment Variables To Use

These are the important runtime variables for the RX 6700S setup tested here:

```bash
export HSA_ENABLE_DXG_DETECTION=1
export HSA_OVERRIDE_GFX_VERSION=10.3.0
export HSA_ENABLE_SDMA=0
export HSA_ENABLE_PEER_SDMA=0
export LIBROCDXG_ALLOC_USER_QUEUE_FROM_UMD=1
```

What they mean:

1. `HSA_ENABLE_DXG_DETECTION=1`
   Tells ROCm to look for the GPU through WSL DXG

2. `HSA_OVERRIDE_GFX_VERSION=10.3.0`
   Makes the 6700S run as `gfx1030`

3. `HSA_ENABLE_SDMA=0`
   Disables the SDMA copy engine path that was hanging during tensor copies

4. `HSA_ENABLE_PEER_SDMA=0`
   Disables the related peer SDMA path too

5. `LIBROCDXG_ALLOC_USER_QUEUE_FROM_UMD=1`
   Enables the queue allocation path that worked on this machine

### Recommended Persistent Bash Setup

Add this block to your `~/.bashrc` and `~/.profile`:

```bash
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
    export HSA_ENABLE_DXG_DETECTION=1
    export HSA_OVERRIDE_GFX_VERSION=10.3.0
    export HSA_ENABLE_SDMA=0
    export HSA_ENABLE_PEER_SDMA=0
    export LIBROCDXG_ALLOC_USER_QUEUE_FROM_UMD=1
fi
```

That way, every new WSL shell gets the working settings automatically.

## How To Run And Test

Here is the simplest way to verify whether the stack is working.

### 1. Check that ROCm sees the GPU

```bash
rocminfo
```

You want to see the GPU listed as an agent.

### 2. Run a basic PyTorch check

```bash
python - <<'PY'
import torch
print("available", torch.cuda.is_available())
print("count", torch.cuda.device_count())
print("name", torch.cuda.get_device_name(0))
x = torch.tensor([1.0, 2.0, 3.0], device="cuda")
print("copy_ok", x.cpu().tolist())
PY
```

Expected output should look roughly like this:

```text
available True
count 1
name AMD Radeon RX 6700S
copy_ok [1.0, 2.0, 3.0]
```

### 3. Run a repeated copy test

This is closer to what exposed the original failure:

```bash
python - <<'PY'
import torch
device = "cuda"
cpu = torch.arange(1024 * 1024, dtype=torch.float32)
for i in range(3):
    gpu = cpu.to(device)
    y = (gpu + 1).sum()
    print(i, float(y.cpu()))
PY
```

If this finishes instead of hanging, your runtime path is in much better shape.

## How I Tested This Fork

The validation path for this repo was:

1. Build `librocdxg.so` in WSL with CMake
2. Install or preload the built library
3. Export the RX 6700S WSL environment variables listed above
4. Run `rocminfo`
5. Run a PyTorch probe that:
   1. checks CUDA availability
   2. reads the device name
   3. allocates a small GPU tensor
   4. copies data from CPU to GPU
   5. copies results back to CPU
6. Repeat the test across multiple fresh Python processes

This fork also passed repeated runtime tests using the freshly built library instead of only the installed copy.

## Common Problems

### Problem: `rocminfo` fails at `hsa_init`

Check:

1. WSL is installed correctly
2. `/dev/dxg` exists inside WSL
3. `/usr/lib/wsl/lib/libdxcore.so` exists
4. `HSA_ENABLE_DXG_DETECTION=1` is set

### Problem: PyTorch sees the GPU, but hangs at tensor copy

This was the main RX 6700S problem during testing.

Make sure these are set:

```bash
export HSA_ENABLE_SDMA=0
export HSA_ENABLE_PEER_SDMA=0
export LIBROCDXG_ALLOC_USER_QUEUE_FROM_UMD=1
```

### Problem: PyTorch reports `available True`, but real work still fails

That usually means the stack is only at the “device visible” stage, not the “device actually usable” stage.

Run the repeated copy test above, not just `torch.cuda.is_available()`.

### Problem: You see `Warning: Windows driver is old, please update it.`

That warning appeared during testing even when the runtime tests worked. It is still a good idea to update the Windows AMD driver if you can.

## Optional Environment Variables For Debugging

Most users do not need these, but they were useful while debugging:

1. `LIBROCDXG_FORCE_HWS=1`
2. `LIBROCDXG_DISABLE_HWS=1`
3. `LIBROCDXG_DISABLE_PLATFORM_ATOMIC=1`
4. `LIBROCDXG_KEEP_SCRATCH_INDEX_STRIDE=1`
5. `LIBROCDXG_DEBUG_WRITE_MARKER=1`

These are for troubleshooting. They are not part of the normal RX 6700S runtime setup.

## Container Notes

If you use containers under WSL, you still need the WSL specific DXG device and libraries:

| Flag | Purpose |
| ---- | ------- |
| `--device /dev/dxg` | Exposes the WSL GPU device to the container |
| `-v /usr/lib/wsl/lib/libdxcore.so:/usr/lib/libdxcore.so` | Makes DXCore available inside the container |
| `-v /opt/rocm/lib/librocdxg.so:/usr/lib/librocdxg.so` | Makes ROCDXG available inside the container |
| `-e HSA_ENABLE_DXG_DETECTION=1` | Enables WSL GPU detection inside the container |

Example:

```bash
docker run -it \
    -v /usr/lib/wsl/lib/libdxcore.so:/usr/lib/libdxcore.so \
    -v /opt/rocm/lib/librocdxg.so:/usr/lib/librocdxg.so \
    -e HSA_ENABLE_DXG_DETECTION=1 \
    --device=/dev/dxg \
    --cap-add=SYS_PTRACE \
    --security-opt seccomp=unconfined \
    --ipc=host \
    --shm-size 8G \
    rocm/pytorch:latest
```
## Documentation

For general ROCm documentation, see:

[Use ROCm on Radeon and Ryzen](https://rocm.docs.amd.com/projects/radeon-ryzen/en/latest/index.html#)

## Sources

This gave me a good outline on what variables needed to be changed: https://github.com/ROCm/ROCm/issues/1756
