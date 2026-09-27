# Contributing

Thanks for your interest! DeuxDisplay is early-stage, so open an issue before starting large
changes to make sure they fit.

## Dev setup
1. Install Visual Studio 2022 / Build Tools with the C++ desktop workload.
2. Run `scripts\bootstrap-dev.ps1` for JDK 17, the Android SDK and adb.
3. Build as described in the [README](README.md).

## Pull requests
- Keep PRs focused, one logical change each.
- CI must pass (host build + tests, Android build + lint + unit tests, driver compile).
- If you touch the protocol, update `docs/wire-protocol.md` and **both** implementations in the
  same PR.
- If you touch the frame path (capture, encode, transport, decode, render), include before/after
  latency numbers or explain why they're unaffected. Measurements go in `docs/latency-notes.md`.
- Driver, capture and on-device behaviour can't be tested in CI. State which Windows build, GPU
  and Android device you tested on.

## Style
- C++: C++20, warnings as errors, format with `.clang-format`.
- Kotlin: official Kotlin style (`kotlin.code.style=official`).
- Line endings are managed by `.gitattributes`. Don't fight them.

## Releasing
Releases are built by CI (`.github/workflows/release.yml`), not on a dev machine. Push a tag:

```powershell
git tag v0.2.0; git push origin v0.2.0      # v0.2.0-rc1 publishes a pre-release
```

The workflow builds the host, agent, driver package and APK, runs the unit tests, and publishes
`DeuxDisplay-<version>-win-x64.zip`, `DeuxDisplay-<version>.apk` and `SHA256SUMS.txt` to a GitHub
Release with generated notes. The APK's `versionName`/`versionCode` come from the tag.

The APK is release-signed when these repository secrets are set (otherwise it's debug-signed, and
users have to uninstall the app before installing the next release):
`ANDROID_KEYSTORE_BASE64` (the `.jks`, base64), `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`,
`ANDROID_KEY_PASSWORD`. Keep a backup of the keystore: losing it means users can't update in place.

## Reporting bugs
Include: Windows build (`winver`), GPU + driver version, Android device + version, host log
output, and `adb logcat -s DeuxDisplay` output.
