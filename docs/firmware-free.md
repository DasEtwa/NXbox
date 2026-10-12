# Firmware-free system data

T20 baseline: `main ea3244c9c`. This inventory covers every registered descriptor
in `system_archive.cpp` and every system-data title in the public
[Switchbrew title list](https://switchbrew.org/wiki/Title_list#System_Data_Archives).
Unlisted IDs in `0x0100000000000800–0x01000000000008FF` remain **missing** and
return null; no speculative supplier is assigned. Existing imported firmware
can still take precedence in other tracks' loaders.

**Synthesized** means a supplier exists; it does not prove all titles accept it.
**Missing** means there is no clean-room supplier. **Firmware-only/out of scope**
means executable firmware or official console credentials must not be replaced
with copied Nintendo content. No Nintendo firmware, keys, games or SDK binaries
were opened or used to generate the new resources in this track.

## Archive inventory

| Title ID | Archive | Status | Consumer / limitation |
| --- | --- | --- | --- |
| `0x0100000000000800` | CertStore | Synthesized | Original sslT, zero entries; no credentials or trust anchors. SSL HLE fallback patch required (T16). |
| `0x0100000000000801` | ErrorMessage | Missing | Error paths and DatabaseInfo name are public; DatabaseInfo schema is not sufficiently documented. HLE error applet is T17. |
| `0x0100000000000802` | MiiModel | Synthesized | Original NFTR/NFSR resources; all documented part indices. Guest SDK rendering requires console validation. |
| `0x0100000000000803` | BrowserDll | Missing | Browser data/NROs are not replaced; HLE browser is T17. |
| `0x0100000000000804` | Help | Missing | No HTML/license/tutorial substitute. |
| `0x0100000000000805` | SharedFont | Missing | Legacy shared-font dictionaries; actual fonts are 0810–0814. |
| `0x0100000000000806` | NgWord | Synthesized | 16 language files + common.txt + version.dat; fixed zero-iteration loop. |
| `0x0100000000000807` | SsidList | Missing | No hotspot list substitute; HLE networking does not load it. |
| `0x0100000000000808` | Dictionary | Missing | No keyboard dictionary substitute; keyboard HLE is T17. |
| `0x0100000000000809` | SystemVersion | Synthesized | Existing 0x100 version structure from HLE API constants, not a firmware file. |
| `0x010000000000080A` | AvatarImage | Missing | No avatar image substitute. |
| `0x010000000000080B` | LocalNews | Synthesized | Original empty RomFS, no offline tutorial/news articles. Native News applet behavior unverified. |
| `0x010000000000080C` | Eula | Missing | No EULA substitute; no automatic acceptance of legal terms. |
| `0x010000000000080D` | UrlBlackList | Missing | No browser URL policy substitute. |
| `0x010000000000080E` | TimeZoneBinary | Synthesized | Existing nx_tzdb generated IANA timezone resources. |
| `0x010000000000080F` | CertStoreCruiser | Missing | No browser trust store substitute; not a retail Switch archive. |
| `0x0100000000000810` | FontNintendoExtension | Synthesized | Existing YuzuOSSExtension font (TTF name table). |
| `0x0100000000000811` | FontStandard | Synthesized | Existing Open Sans (Apache 2.0 name table), BFTTF wrapper. |
| `0x0100000000000812` | FontKorean | Synthesized | Existing Open Sans substitute; Korean glyph coverage is not established. |
| `0x0100000000000813` | FontChineseTraditional | Synthesized | Existing Open Sans substitute; Chinese glyph coverage is not established. |
| `0x0100000000000814` | FontChineseSimple | Synthesized | Existing Open Sans + Source Sans Pro (OFL 1.1 name table); Chinese glyph coverage is not established. |
| `0x0100000000000815` | FontBfcpx | Missing | No Bfcpx substitute; not a retail Switch archive. |
| `0x0100000000000816` | SystemUpdate | Missing | System-update metadata, not needed for HLE boot. |
| `0x0100000000000817` | 0100000000000817 | Missing | Unidentified inherited descriptor; no format or reader. |
| `0x0100000000000818` | FirmwareDebugSettings | Missing | HLE settings implement configuration without reading this archive. |
| `0x0100000000000819` | BootImagePackage | Firmware-only / out of scope | Firmware executable package; outside clean-room data substitution. |
| `0x010000000000081A` | BootImagePackageSafe | Firmware-only / out of scope | Safe-mode firmware executable package; outside scope. |
| `0x010000000000081B` | BootImagePackageExFat | Firmware-only / out of scope | exFAT firmware executable package; outside scope. |
| `0x010000000000081C` | BootImagePackageExFatSafe | Firmware-only / out of scope | Safe-mode exFAT firmware executable package; outside scope. |
| `0x010000000000081D` | FatalMessage | Missing | No fatal-message database substitute; HLE error handling is T17. |
| `0x010000000000081E` | ControllerIcon | Missing | No controller icon substitute; HLE controller UI is T17. |
| `0x010000000000081F` | PlatformConfigIcosa | Missing | Hardware platform configuration, not read by HLE. |
| `0x0100000000000820` | PlatformConfigCopper | Missing | Hardware platform configuration, not read by HLE. |
| `0x0100000000000821` | PlatformConfigHoag | Missing | Hardware platform configuration, not read by HLE. |
| `0x0100000000000822` | ControllerFirmware | Firmware-only / out of scope | Peripheral executable firmware; outside scope. |
| `0x0100000000000823` | NgWord2 | Synthesized | Existing 51 empty gzip automata + version.dat (52 files); original provenance of the inherited constant needs audit. |
| `0x0100000000000824` | PlatformConfigIcosaMariko | Missing | Hardware platform configuration, not read by HLE. |
| `0x0100000000000825` | ApplicationBlackList | Missing | Installation/update restriction data, not read by HLE. |
| `0x0100000000000826` | RebootlessSystemUpdateVersion | Missing | No rebootless-update version substitute. |
| `0x0100000000000827` | ContentActionTable | Missing | Content-action policy, not read by HLE. |
| `0x0100000000000828` | FunctionBlackList | Missing | Feature restriction policy, not read by HLE. |
| `0x0100000000000829` | PlatformConfigCalcio | Missing | Hardware platform configuration, not read by HLE. |
| `0x0100000000000830` | NgWordT | Missing | China-region NgWordT has no supplier; do not assume NgWord2 binary compatibility. |
| `0x0100000000000831` | PlatformConfigAula | Missing | Hardware platform configuration, not read by HLE. |
| `0x0100000000000832` | CradleFirmware | Firmware-only / out of scope | Dock executable firmware; outside scope. |
| `0x0100000000000835` | ErrorMessageUtf8 | Missing | Alternate UTF-8 error database; no supplier. |
| `0x0100000000000859` | ClientCertData | Firmware-only / out of scope | Official service client credentials: intentionally never synthesized. |
| `0x010000000000085C` | GameCardConfigurationData | Missing | Game-card configuration; no documented minimal substitute. |

## Firmware content read paths

| Code / entry point | Content read | Firmware-free behavior / owner |
| --- | --- | --- |
| `src/core/hle/service/filesystem/fsp/fsp_srv.cpp`, `OpenDataStorageByDataId` | Any system-data ID requested by a guest, via RomFSController | Calls `SynthesizeSystemArchive` if storage is absent (T16). This is the route used by the guest Mii SDK. |
| `src/core/file_sys/romfs_factory.cpp`, `RomFSFactory::GetEntry` and `src/core/hle/service/filesystem/romfs_controller.cpp` | Generic registered Data NCA, requested title and storage | Loader route for the FSP path; no title-specific patch in T20. |
| `src/core/hle/service/ns/platform_service_manager.cpp` | 0810–0814, named BFTTF files | NAND first, synthesized fonts second; decrypts BFTTF into shared memory (T16). |
| `src/core/hle/service/set/system_settings_server.cpp`, `GetFirmwareVersionImpl` | 0809 `/file` | NAND first, synthesized SystemVersion second (T16). |
| `src/core/hle/service/glue/time/time_zone_binary.cpp` | 080E `/binaryList.txt`, `/zoneinfo`, version | NAND first, nx_tzdb fallback; timezone rule parser is in `src/core/hle/service/psc/time` (T16). |
| `src/core/hle/service/ssl/cert_store.cpp`, `CertStore` | 0800 `/ssl_TrustedCerts.bdf` | NAND only. New original empty archive is reachable by FSP but needs the T16 patch below for the SSL loader. Absence already produces an empty HLE trust store. |
| `src/core/hle/service/am/frontend/applet_web_browser.cpp` | Web system Data archives and font 0811 | NAND first, synthesized fallback; BrowserDll/Help and other native resources remain missing (T17). |
| `src/core/hle/service/am/process_creation.cpp`, `CreateProcess` | Registered system applet Program NCA via `GetEntryRaw` | Native applets require their executable content; HLE applet selection/completion belongs to T17. |
| `src/core/hle/service/am/service/library_applet_creator.cpp`, `application_creator.cpp` | Native applet/application executables | HLE bypass policy belongs to T17. |
| `src/frontend_common/firmware_manager.cpp`, `GetFirmwareVersion` | SystemVersion through settings service; installed content enumeration | Existing fallback; used to select ES, PCIe, CAPS, NVDRV and NS service command tables (T16). |
| `src/frontend_common/firmware_files.h` and frontend firmware installers | User-provided NCA files/version | Import/discovery only, not a firmware-free resource supplier; outside T20. |
| `src/core/file_sys/registered_cache.cpp`, `content_archive.cpp`, `bis_factory.cpp` | Registered NAND NCAs, keys for decryption | Generic content plumbing; no embedded system-data substitute. |
| `src/core/hle/service/mii/mii_database_manager.cpp` | Emulated NAND `system/save/8000000000000030/MiiDatabase.dat` | Save data, not firmware. Seed six original defaults on empty successful initialization; preserve nonempty and corrupt databases. |

The same generic loaders read application/update/DLC content through
`patch_manager.cpp` and `src/core/loader/*`; those are game assets, not firmware
archive dependencies. Guest requests for unknown IDs cannot be inventoried by
static service reads alone: record FSP title IDs on console for those cases.

## Original Mii resources

Public binary layout reference:
[Vee ResourceShape](https://tangled.org/j0.lol/vee/blob/main/crates/vee_resources/src/shape.rs),
[Vee ResourceTexture](https://tangled.org/j0.lol/vee/blob/main/crates/vee_resources/src/tex.rs),
and its [NFTR format pattern](https://tangled.org/j0.lol/vee/blob/main/testbed/nx_texture_mid_srgb.hexpat),
pinned to commit `4ecb8ec3b49ec4f4f72a513a9a6bf052ed82deaf`.
Only published layouts were consulted; no Vee code or Nintendo assets are copied.
The restricted shape-viewer demo and downloadable proprietary resources were not used.

`data/generate_mii_resources.py` procedurally generates an ellipsoid head,
front mask surface, upper hair cap, pyramidal nose, accessory plane and simple
eye/eyebrow/mouth/mole masks. All 857 shape slots and 327 texture slots have
bounded descriptors. Absent hat/beard geometry uses a valid compressed empty
payload. Repeated styles deliberately share original payloads. Headwear and
face transforms are zero because these original meshes share an origin.

The NFSR data uses float16 positions/UVs, packed signed normals, tangent/color
attributes, uint16 triangles and bounds. NFTR uses 32×32 RGBA8, linear tiling,
one mip, 16-byte alignment. Both use zlib payloads and version 1 headers.
High/mid shapes and low/mid linear/sRGB textures intentionally share the same
small artwork; the monochrome masks do not require a different transfer curve.
This is a minimal fallback, not a recreation of Nintendo's artwork/style set.

Eden does **not** contain a NFTR/NFSR renderer or reader: the guest SDK reads
these files. The T20 tests run Eden's real `CreateRomFS`/`ExtractRomFS` readers,
compare extracted bytes to the generator, and independently validate all
resource descriptors, inflated sizes, attribute bounds and triangle indices.
This establishes structural validity, not guest SDK acceptance or appearance.
No claim that every title renders a default Mii is made before console tests.

The six `StoreData::BuildDefault` profiles are original Player 1–6 profiles,
with valid field ranges, unique creator IDs and Eden's own CRC calculation.
The existing six-default source is always available. Empty successful database
initialization seeds all six in one save, on every platform; existing databases
are not expanded or overwritten. Explicit Format/Delete remain effective for
the current session; a later initialization seeds an empty database again.
Corruption and mount/save failures still return errors.

## Other minimal archives and remaining gaps

CertStore follows the public
[sslT header format](https://switchbrew.org/wiki/SSL_services#CertStore): magic
and zero entries, with both modern `.bdf` and legacy `.tcf` paths. It deliberately
contains no trusted roots or private credentials. It does not bypass TLS
verification. CRL and fingerprint tables are not synthesized without a schema.

LocalNews is an empty RomFS directory. The HLE news service does not load this
archive; native News applet acceptance of an empty directory is unverified.
ErrorMessage/FatalMessage/UTF-8 error databases remain missing: public filenames
alone do not establish a valid `DatabaseInfo`/message format. T17 should use
original host error text in HLE, and return failure for unsupported native data.
NgWordT, keyboard dictionaries and several policy/config archives also remain
missing; see the inventory. Firmware-free boot of all titles is **not complete**.

Existing embedded font name tables identify Open Sans/Source Sans Pro and
YuzuOSSExtension, not Nintendo firmware fonts. This is a provenance observation,
not proof of complete Korean/Chinese character coverage. Existing NgWord2 bytes
were not regenerated and their upstream provenance is not established here.

## Cross-track patch proposals (not applied)

**T16 — SSL archive fallback.** In `ssl/cert_store.cpp`, include
`core/file_sys/system_archive/system_archive.h`, replace the early no-NCA return
and direct `nca->GetRomFS()` with:

```cpp
const auto romfs = nca ? nca->GetRomFS()
                      : FileSys::SystemArchive::SynthesizeSystemArchive(CertStoreDataId);
```

Also fall back when an existing NCA has no readable RomFS. Keep the existing
sslT parser and zero-entry behavior. Do not add console client keys/certificates,
Nintendo endpoint authentication, or disabled TLS checks. Test zero entries
with `GetCertificates(All)` and its invalid terminator.

**T17 — Mii selector/appet completion.** Enumerate `SourceFlag::Database` after
successful `MiiManager::Initialize`; six profiles now exist on fresh NAND.
Return the selected database index/CharInfo, signal the completion event, and
handle cancellation/initialization errors. Do not rely on a native firmware
Mii editor process. `AppendMiiImage`/`UpdateMiiImage` currently return success
without generating images; any original host preview/image output belongs to
T17. Error applet text and native browser/news resource requests also belong there.

## Measured coverage and acceptance

These are structural coverage counts, not execution-line coverage:

| Measurement | Baseline ea3244c9c | T20 |
| --- | ---: | ---: |
| Registered system-data descriptors | 48 | 48 |
| Suppliers (including the former Mii stub) | 10/48 (20.83%) | 12/48 (25.00%) |
| Documented descriptors | 0/48 in this report | 48/48 (100%) |
| Mii shape descriptors with validated payload bounds | 0/857 | 857/857 (100%) |
| Mii texture descriptors with validated payload bounds | 0/327 | 327/327 (100%) |
| NgWord numbered language files | 0/16 | 16/16 |
| Fresh UWP database Miis | 1 | 6 |
| Built-in default profiles | 6 inherited profiles | 6 original validated profiles |

Run `cd tests/port && python3 -m unittest discover -s . -p "test_*.py"`.
T20 coverage: `python3 -m unittest discover -s tests/port -p "test_t20_*.py"`.
The executable harness uses actual VFS/RomFS, StoreData, CharInfo, database and
MiiManager code. Only host infrastructure and unrelated suppliers are stubbed;
assertions remain enabled. Temporary compilation/output is under `/tmp` and
is removed automatically. The reproducibility test compares committed data
with freshly generated data.

Console checks still required, with no firmware directory installed: fresh-NAND
Mii enumeration and selection/completion; guest SDK NFTR/NFSR initialization;
default Mii rendering in multiple titles; eyes/mouth mask positioning and alpha;
all six defaults; reload and selection of saved profiles; malformed/small IPC
buffers; NgWord startup; timezone and font startup; zero-trust SSL error behavior.
Capture FSP title IDs and guest resource errors before adding any further supplier.
MSVC workflow and full port-suite evidence are recorded in the delivery report.
