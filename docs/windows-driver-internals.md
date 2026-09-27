# Windows driver internals: how DeuxDisplayIdd creates a monitor

Companion to [driver/README.md](../driver/README.md) (build and install) and
[architecture.md](architecture.md). This doc explains *where the driver sits in Windows' display
stack* and *what happens, callback by callback*, when a tablet connects.

---

## Part 1: Intuition

### What a display driver normally does

On a normal PC the GPU driver (WDDM) tells Windows which monitors are connected on its ports,
reads their EDID (a 128-byte "business card": name, size, supported modes), and then scans the
composed desktop out of video memory onto the cable.

### What an *indirect* display driver does

An **Indirect Display Driver (IDD)** is for monitors that aren't wired to a GPU port: USB docks,
wireless displays, or, here, a monitor that doesn't physically exist. It does two things:

1. **Reports monitors** as if they were plugged into a port, with an EDID and a list of modes.
2. **Receives the composed frames** for those monitors through a *swap chain*, and ships them
   wherever it wants (over USB, the network...).

Windows still does all the real rendering on a real GPU. The IDD is just told "here's the next
finished frame for your monitor".

### What DeuxDisplayIdd chooses to do

- It does job 1 fully: EDIDs and modes are **generated on the fly** from what the tablet reports.
- It does job 2 **minimally**: it takes each frame and immediately hands it back. The host
  reads the pixels a different way, through **Desktop Duplication**, from its own process.

Why not read frames inside the driver? The driver lives in a locked-down `WUDFHost.exe` process.
Getting frames out would mean cross-process texture sharing and building the encoder into the
driver. Desktop Duplication already gives the host a GPU texture of that monitor for about
0.5–2 ms. The cost is one extra GPU copy. See "Alternatives considered" in
[architecture.md](architecture.md).

So, in one line: **the driver makes the monitor exist; the host watches it.**

---

## Part 2: The layer diagram

```
 ════════════════════════════════════ USER MODE ═══════════════════════════════════════════

  ┌──────────────────────┐   ┌───────────────────────────────┐  ┌───────────────────────────┐
  │ Applications         │   │ DeuxDisplayHost.exe (user)     │  │ Settings > Display, Win+P │
  │ (draw with D3D/GDI)  │   │                               │  │ (arrange, scale, rotate)  │
  └──────────┬───────────┘   │  control plane:               │  └─────────────┬─────────────┘
             │               │   CreateFile(device iface)    │                │
             ▼               │   DeviceIoControl(PLUG/UNPLUG)│                │ SetDisplayConfig /
  ┌──────────────────────┐   │                               │                │ ChangeDisplaySettingsEx
  │ DWM (dwm.exe)        │   │  data plane:                  │                │
  │ desktop compositor   │   │   IDXGIOutputDuplication      │                │
  │  - composes each     │◄──┼── AcquireNextFrame ◄──────────┼─┐              │
  │    monitor's image   │   │   (GPU texture of DXD000n)    │ │              │
  │  - feeds Desktop     │   └───────────────┬───────────────┘ │              │
  │    Duplication       │                   │ IOCTL           │              │
  └──────────┬───────────┘                   │                 │              │
             │ present to the IDD's          │                 │              │
             │ swap chain                    │                 │              │
             ▼                               ▼                 │              │
  ┌────────────────────────────────────────────────────────────┴──┐           │
  │ WUDFHost.exe  (device group "DeuxDisplayIddGroup")            │           │
  │ ┌───────────────────────────────────────────────────────────┐ │           │
  │ │ DeuxDisplayIdd.dll      ← THIS REPO (driver/)             │ │           │
  │ │  • EvtIddCxDeviceIoControl: PLUG / UNPLUG                  │ │           │
  │ │  • Plug(): EDID (Edid.h) + modes → IddCxMonitorCreate/     │ │           │
  │ │            IddCxMonitorArrival                             │ │           │
  │ │  • EvtParseMonitorDescription / QueryTargetModes: modes    │ │           │
  │ │  • EvtAssignSwapChain → SwapChainProcessor thread:         │ │           │
  │ │      ReleaseAndAcquireBuffer → drop → FinishedProcessing   │ │           │
  │ │  • EvtFileCleanup: handle closed → unplug that monitor     │ │           │
  │ ├───────────────────────────────────────────────────────────┤ │           │
  │ │ IddCx class extension (Microsoft): monitor objects,        │ │           │
  │ │ swap chains, mode negotiation with the OS                  │ │           │
  │ ├───────────────────────────────────────────────────────────┤ │           │
  │ │ UMDF 2 / WDF runtime (Microsoft): PnP, power, I/O queues,  │ │           │
  │ │ file objects                                               │ │           │
  │ └───────────────────────────────────────────────────────────┘ │           │
  └───────────────────────────────┬───────────────────────────────┘           │
                                  │ UMDF reflector (IRPs ⇄ user mode)          │
 ════════════════════════════════════ KERNEL MODE ═════════════════════════════╪═════════════
                                  │                                           │
  ┌───────────────────────────────▼───────────────┐    ┌──────────────────────▼────────────┐
  │ WUDFRd.sys (reflector)                         │    │ Win32k / display config (CCD)     │
  ├────────────────────────────────────────────────┤    └──────────────────────┬────────────┘
  │ IndirectKmd.sys (Microsoft, INF UpperFilter)   │◄───────────────────────────┤
  │  kernel shim that makes the IDD look like a    │                            │
  │  WDDM display adapter to the graphics kernel   │                            ▼
  └───────────────────────┬────────────────────────┘    ┌───────────────────────────────────┐
                          └────────────────────────────►│ Dxgkrnl.sys (DirectX graphics      │
                                                        │ kernel): adapters, monitors, VidPN,│
                                                        │ scheduling                         │
                                                        └──────────────────┬────────────────┘
                                                                           │
                                                        ┌──────────────────▼────────────────┐
                                                        │ Real GPU's WDDM driver (render     │
                                                        │ adapter: Intel/NVIDIA/AMD). DWM    │
                                                        │ renders the virtual monitor here,  │
                                                        │ and the host's encoder runs here.  │
                                                        └───────────────────────────────────┘

 ══════════════════════════════════ DEVICE TREE (PnP) ═════════════════════════════════════

  HTREE\ROOT\0
   └─ SWD\DeuxDisplayIdd\DeuxDisplayIdd     persistent software device (install-driver.ps1)
       │  class: Display adapter, hardware ID "DeuxDisplayIdd", driver: deuxdisplayidd.inf
       │  device interface {9D4C6A2E-…-6F2A1B3C4D5E}  ← what the host opens
       ├─ MONITOR\DXD0001   connector 0  (plugged for session 1, "DeuxDisplay")
       ├─ MONITOR\DXD0002   connector 1  ("DeuxDisplay 2")
       ├─ MONITOR\DXD0003   connector 2
       └─ MONITOR\DXD0004   connector 3
```

### How to read the diagram

- **Two planes.** The *control plane* (host → IOCTL → driver) decides whether a monitor exists
  and what it looks like. The *data plane* (DWM → Desktop Duplication → host) carries pixels.
  The driver sits on the control plane plus a stub of the data plane (draining the swap chain).
- **The only code we own is the `DeuxDisplayIdd.dll` box.** Everything else is Windows.
  `IndirectKmd.sys` is listed in the INF as an `UpperFilters` entry, which is how an IDD gets
  a kernel presence without writing kernel code.
- **Rendering happens on the real GPU** (the "render adapter"). IddCx passes its LUID to
  `EvtIddCxMonitorAssignSwapChain`, and the host's encoder runs on that same GPU, so frames
  never touch system RAM.

---

## Part 3: Lifecycle, callback by callback

### A. Install (admin, once)

```
 install-driver.ps1
   ├─ make a self-signed cert (non-exportable key), trust it on this machine
   ├─ sign the catalog; pnputil /add-driver deuxdisplayidd.inf
   └─ DeuxDisplayHost --install-device
        └─ SwDeviceCreate("DeuxDisplayIdd", HTREE\ROOT\0) + SwDeviceSetLifetime(ParentPresent)
             → PnP matches hardware ID → loads DeuxDisplayIdd.dll in WUDFHost
```

The INF also sets the device's security descriptor
`D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`: SYSTEM and Administrators get full access,
**Interactive Users get read/write**. That's the single line that lets a non-admin host send
`PLUG`.

### B. Driver load (every boot)

| Order | Callback (Driver.cpp)                  | What it does |
|------:|----------------------------------------|--------------|
| 1 | `DriverEntry`                                | `WdfDriverCreate` with `DeuxDisplayDeviceAdd` |
| 2 | `DeuxDisplayDeviceAdd`                       | Register IddCx callbacks (`IddCxDeviceInitConfig`), the file-cleanup callback, create the WDF device, `IddCxDeviceInitialize`, **create the device interface** the host looks for |
| 3 | `DeuxDisplayDeviceD0Entry`                   | `InitAdapter()`: fill `IDDCX_ADAPTER_CAPS` (`MaxMonitorsSupported = 4`, names), `IddCxAdapterInitAsync` |
| 4 | `DeuxDisplayAdapterInitFinished`             | Mark the adapter ready. **No monitor yet**: the adapter shows in Device Manager, but Display Settings shows nothing new |

### C. A tablet connects (host sends `PLUG`)

```
 host (session thread)                driver (WUDFHost)                         Windows
 ──────────────────────               ─────────────────                         ───────
 CM_Get_Device_Interface_List
 CreateFile(interface path)  ───────► (new WDFFILEOBJECT = "owner")
 DeviceIoControl(kIoctlPlug,
   PlugRequest{w,h,mm,Hz[4]}) ──────► DeuxDisplayDeviceIoControl
                                       └─ Plug(request, owner):
                                          • IsValidPlugRequest + FitsInDtd
                                          • pick slot: caller's own, else lowest free (0..3)
                                          • BuildEdid(): vendor DXD, product index+1,
                                            preferred + secondary timing, size in mm
                                          • BuildModes(): each Hz at native size,
                                            + half size @ 60 Hz
                                          • stash EDID+modes in g_Current[index]
                                          • IddCxMonitorCreate(ConnectorIndex=index,
                                            EDID, ContainerId = fixed GUID + index)
                                          • IddCxMonitorArrival ───────────────► "monitor plugged"
                                                                                  │
                                       DeuxDisplayParseMonitorDescription ◄──────┤ read EDID
                                         (match EDID bytes in g_Current → modes)  │
                                       DeuxDisplayMonitorQueryModes ◄────────────┤ target modes
                                                                                  │ pick mode, extend
                                       DeuxDisplayAdapterCommitModes ◄───────────┤ (no-op)
                                       DeuxDisplayMonitorAssignSwapChain ◄───────┘ render GPU LUID,
                                         └─ new SwapChainProcessor thread            frame event
 ◄──── PlugResult{index} ─────────────
 WaitForExtendedDisplay: find
   MONITOR\DXD000(index+1) as its own
   output; SetDisplayConfig(EXTEND)
   if Windows chose duplicate
 DuplicateOutput1 on that output ────────────────────────────────────────────► frames start
```

Details worth noticing:

- **Why `g_Current`?** `EvtIddCxParseMonitorDescription` receives only EDID bytes, not the
  monitor object. The driver finds the mode list by comparing EDID bytes. The product code
  (index + 1) makes each monitor's EDID unique.
- **Why a fixed container ID per index?** Windows keys remembered layout (position, scale) on
  it, so "the tablet's monitor" stays where you put it last time.
- **`FitsInDtd`.** An EDID detailed timing stores the pixel clock in a 16-bit field of 10 kHz
  units, so the maximum is 655.35 MHz. Very large or very fast modes are rejected here and the host
  falls back to its default mode.
- **Modes = monitor modes ∩ target modes.** The driver returns the same list for both, so Windows
  offers exactly what the tablet asked for.

### D. Steady state: the swap chain loop

```
 SwapChainProcessor::RunCore  (thread, MMCSS "Distribution")
   IddCxSwapChainSetDevice(D3D11 device on the render adapter)
   loop:
     IddCxSwapChainReleaseAndAcquireBuffer
       E_PENDING → wait (new-frame event | terminate | 16 ms)
       success   → drop the surface; IddCxSwapChainFinishedProcessingFrame
       failure   → break (swap chain abandoned; OS will assign a new one)
   WdfObjectDelete(swapchain)
```

This loop must keep draining. If it stops, DWM's presents for that monitor back up. Meanwhile
DWM also feeds Desktop Duplication, which is what the host actually reads.

### E. Disconnect

| Trigger                        | Path |
|--------------------------------|------|
| Session ends normally          | Host `DeviceIoControl(kIoctlUnplug)` → `Unplug(owner)` → `IddCxMonitorDeparture` |
| Host closes its handle         | `VirtualDisplay` destructor → `CloseHandle` → `DeuxDisplayFileCleanup` → `Unplug(owner)` |
| Host crashes / is killed       | Kernel closes the handle → same `EvtFileCleanup` path. **No phantom monitors** |
| Monitor departs                | IddCx calls `EvtIddCxMonitorUnassignSwapChain` → processor thread stops |

Ownership is per **file object** (per handle), which gives the multi-tablet behaviour: each
session opens its own handle and can only unplug its own monitor.

---

## Part 4: File map

| File | License | Contents |
|------|---------|----------|
| `driver/DeuxDisplayIdd/Public.h`  | MIT    | The host↔driver contract: interface GUID, IOCTL codes, `PlugRequest`/`PlugResult`, shared validation |
| `driver/DeuxDisplayIdd/Edid.h`    | MIT    | EDID 1.4 builder (detailed timings, name, physical size, checksum) and parser |
| `driver/DeuxDisplayIdd/Driver.h`  | MS-PL  | `Direct3DDevice`, `SwapChainProcessor`, device and monitor contexts |
| `driver/DeuxDisplayIdd/Driver.cpp`| MS-PL  | All callbacks above |
| `driver/DeuxDisplayIdd/DeuxDisplayIdd.inf` | MS-PL | Class Display, `UpperFilters=IndirectKmd`, security SDDL, UMDF device group |
| `host/src/display/VirtualDisplay.cpp` | MIT | Host side: install/remove the software device, open the interface, PLUG/UNPLUG, force extend |
| `host/tests/EdidTests.cpp`        | MIT    | EDID round-trip tests (run in CI without the driver) |

## Part 5: Debugging checklist

- Device Manager → *Display adapters* → *DeuxDisplay Virtual Monitor*: status/code.
- `DeuxDisplayHost --create-display`: plugs a test monitor without a tablet.
- `DeuxDisplayHost --list-outputs`: are `MONITOR\DXD000n` outputs visible to DXGI?
- Attach a debugger to the `WUDFHost.exe` hosting `DeuxDisplayIddGroup`.
- Event Viewer → *Microsoft → Windows → DriverFrameworks-UserMode*.
- A monitor that appears **duplicated** instead of extended is a topology issue (host handles
  it with `SetDisplayConfig(SDC_TOPOLOGY_EXTEND)`), not a driver bug.
