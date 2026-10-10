# Android package qualification

HORO-2201 / #2262 implements the PLT-001.6 assembly boundary. The checked-in
`android/toolchain-lock.json` and `android/package-profiles.json` are its sources
of truth. Linux is the initial qualified build host. SDK paths and signing
capabilities are supplied explicitly; they never enter portable provenance.

The debug profile admits arm64-v8a and the x86_64 development ABI; release admits
arm64-v8a. Each native CMake configure selects exactly one ABI. The API tuple is
minSdk 29 / targetSdk 36 / compileSdk 36, with NDK 28.2.13676358, CMake 3.31.6,
Ninja 1.11.1, JDK 17, Gradle 8.11.1, AGP 8.10.1 and GameActivity 3.0.5. The AAR and Gradle
distribution have checked-in SHA-256 identities. Java dependency versions and
artifact hashes are captured inside each package for provenance comparison.

Install the locked platform, build-tools and NDK through SDK manager, provide
CMake and Ninja on PATH, and obtain the locked Gradle distribution and GameActivity
AAR. The hosted `android-package.yml` workflow demonstrates these prerequisites
and checks download hashes before execution. Assemble in a new output directory:

```bash
python3 scripts/android_package.py --profile qualification-debug \
  --abi arm64-v8a --abi x86_64 --sdk "$ANDROID_HOME" \
  --ndk "$ANDROID_NDK_ROOT" --gradle "$GRADLE_HOME/bin/gradle" \
  --game-activity "$GAME_ACTIVITY_AAR" --output "$PACKAGE_OUTPUT" --unsigned
```

Without `--unsigned`, supply `HORO_ANDROID_KEYSTORE`, `HORO_ANDROID_KEY_ALIAS`,
`HORO_ANDROID_STORE_PASSWORD` and `HORO_ANDROID_KEY_PASSWORD` externally. Signing
availability, alias and store password are checked before compilation. Private
key password validity is checked by APK signer. Passwords are passed through
its environment mechanism, never interpolated into commands or evidence.
The CI identity is disposable qualification data, not a distribution key.

`--native-root` admits an explicit directory containing one subdirectory per
selected ABI. Each must contain exactly the profile's declared native libraries,
including one shared C++ runtime. ELF admission checks actual machine type,
Android API notes, 16 KiB load alignment, forbidden RPATH/RUNPATH and complete
DT_NEEDED closure before Gradle runs. Assets reject traversal, links and oversized
inputs. Final inspection verifies native and asset hashes, manifest identity/API,
permissions and required hardware features against admitted inputs.

Outputs are `signed.apk` or `unsigned.apk`, `provenance.json`, `inspection.json`
and separate command logs. Provenance records source revision, profile, exact
tool versions, ABI/native dependency closure and content hashes; inspection lists
actual APK members and their hashes. Output directories are never overwritten.
ZIP ordering/timestamps, native debug-prefix mappings and 16 KiB alignment are
explicit. Hosted qualification assembles twice in distinct directories and
compares the signed APK, provenance and inspection byte for byte.

## Composition and qualification limits

The opt-in CMake presets `android-debug` and `android-release` build an actual
GameActivity native entry and packaged asset probe without desktop dependencies.
`HORO_ANDROID_PACKAGE_QUALIFICATION` selects this host composition before the
normal engine graph. It does not change the default desktop presets or public
header ownership. The qualification manifest has no required interactive
hardware feature; its only admitted permission is AndroidX's application-local
signature permission for receiver protection.

These packages prove toolchain and assembly behavior. They deliberately record
`renderer: none` and `distributionQualified: false`; they do not qualify an
interactive engine host, renderer, emulator/device lifecycle, or product signing.
Those capabilities require their own host composition and profile acceptance.
No physical Android execution is implied by APK construction. The existing
Android storage NDK syntax qualification remains an independent gate.

## Migration

Android packaging previously had aspirational documentation without executable
presets or an assembly contract. Callers now select a declared profile and explicit
inputs instead of supplying undocumented CMake cache switches. To admit another
ABI, native dependency, asset, permission or feature, change the checked-in profile,
update its compatibility tests and hosted package evidence together. New runtime
hosts must supply their own explicit composition rather than extending this probe
into backend discovery.
