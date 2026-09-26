# Third-party notices: `driver/`

The code in this directory is derived from the **Indirect Display Driver sample**
(`video/IndirectDisplay`) in [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples),
commit `2dc3fd3a0cc84a2933f2194e7ec0871584979071`.

- Copyright (c) Microsoft Corporation
- License: **Microsoft Public License (MS-PL)**. See [LICENSE](LICENSE).

MS-PL requires that source distributions of derived code stay under MS-PL with the license
included. Because of that, the sample-derived files under `driver/` (`Driver.cpp`, `Driver.h`,
`DeuxDisplayIdd.inf`, the project files) are licensed under MS-PL, not the MIT license used
elsewhere in this repository. `Edid.h` is original code and is MIT-licensed, which lets the MIT
host tests include it. The host service and Android app are separate programs that don't include
sample code; they remain MIT.

## Changes from the sample
- Renamed to `DeuxDisplayIdd`; new hardware IDs, INF strings and UMDF device group.
- One monitor, with an EDID generated at compile time (`Edid.h`) from the target device's
  resolution, refresh rates and physical size instead of hard-coded sample EDIDs.
- Mode lists match the target device.
- Stable monitor container ID so Windows remembers the display arrangement.
- WPP tracing removed. Spectre-mitigated libraries not required. WDK consumed via NuGet.
- Driver files are copied to `DIRID 13` (run from the driver store).
