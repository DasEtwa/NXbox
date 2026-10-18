# NxHelper mobile

© 2026 NSPX · GPL-3.0-or-later · Android / iOS · `com.nxhelper.app`

## Português

Baixe os artefatos do workflow **Build NxHelper mobile** no GitHub Actions e descompacte-os.
No Android, abra `NxHelper-android.apk` e permita a instalação por essa fonte.
No iPhone, importe `NxHelper-ios-unsigned.ipa` no AltStore ou SideStore; ele assina
com sua conta durante o sideload. Renove a assinatura conforme o aplicativo pedir.
Não é uma distribuição pela App Store.

Use o mesmo Wi-Fi do Xbox. Ative o Modo de Desenvolvedor e o Device Portal no
console. Encontre o Xbox, entre com o usuário/senha do Portal, escolha os arquivos
e envie. A senha só é guardada se marcar **Lembrar neste celular**; desmarcar e
conectar apaga a senha salva desse Xbox. Mantenha o app aberto durante o envio.
Depois abra o NXbox no console para juntar jogos grandes e instalar firmware.
ZIPs de firmware são extraídos no cache do celular e precisam de espaço local;
jogos são lidos diretamente, em trechos, inclusive quando têm mais de 10 GiB.
As pastas de firmware devem conter arquivos `.nca` soltos; também é possível
selecionar vários `.nca`. Não há preparação de USB nem extração de NAND/emuMMC.

## English

Download and unzip the artifacts from **Build NxHelper mobile** in GitHub Actions.
On Android, open `NxHelper-android.apk` and allow installation from that source.
On iPhone, import `NxHelper-ios-unsigned.ipa` into AltStore or SideStore; it signs
the app with your account during sideloading. Renew signing when prompted.
This app is not distributed through the App Store.

Use the same Wi-Fi as the Xbox. Enable Dev Mode and Device Portal on the console.
Find the Xbox, enter Portal credentials, pick files and send. Credentials are saved
only with **Remember on this phone** checked; uncheck and connect to remove a saved
credential for that Xbox. Keep the app open while transferring. Open NXbox afterwards
to join large games and install firmware. Firmware ZIPs need temporary phone disk
space; games stream directly in bounded chunks, including files above 10 GiB.
Firmware folders must contain loose `.nca` files; selecting multiple `.nca` files
also works. USB preparation and NAND/emuMMC extraction require the desktop version.

## Development

Requires Rust stable, Node 22+, and the Tauri 2 platform prerequisites. No frontend
build or external assets are needed. Networking runs exclusively in Rust and only
targets literal private IPv4 addresses on port 11443. TLS handshake signatures are
verified; the Xbox's private certificate does not need a public trust chain.
Redirects and proxy discovery are disabled. There are no analytics or background
network calls. Installing/updating NXbox remains a desktop task.

```sh
npm ci
cargo test --locked --manifest-path src-tauri/Cargo.toml
cargo clippy --locked --manifest-path src-tauri/Cargo.toml -- -D warnings
npm run tauri -- android init --ci
npm run prepare:mobile
npm run tauri -- android build --apk --target aarch64
# On macOS with Xcode, xcodegen and libimobiledevice:
npm run tauri -- ios init --ci
npm run prepare:mobile
(cd src-tauri/gen/apple && xcodegen generate)
npm run tauri -- ios build --no-sign --archive-only --target aarch64
```

Native generated projects are ignored and rebuilt by CI. `prepare:mobile` applies
Android permissions/backup settings and iOS local-network prompt translations.
CI signs a release APK with the four `NXHELPER_ANDROID_*` secrets described in the
workflow; without a complete set it generates a debug key and logs the fallback.
The APK supports ARM64 phones. Debug fallback keys are ephemeral: switching keys
requires uninstalling the previous build. The iOS artifact is an unsigned device
archive repackaged as an IPA for the sideloading tool to sign.

Uploads follow the desktop paths and order: `LocalState/games`,
`LocalState/eden/keys`, then `LocalState/firmware`. Files above 1 GiB use
`<name>.nxpart000`, `001`, etc., followed by `<name>.nxparts` with
`parts=<count>\nsize=<bytes>\n`. Each remote part and manifest is size-checked.
Cancellation stops requests and extraction; partial remote files are retained and
replaced on retry. A final manifest is sent only after all parts are verified.
The free-space check matches desktop behavior; old Portal versions show a warning
when drive capacity is unavailable. File validation is limited to names, readable
ranges and transfer sizes; it does not perform the desktop cryptographic checks or
XCZ conversion. XCZ playback requires conversion with the desktop helper first.

Host Rust tests cover private IPv4 policy, split planning for 2.5 GiB, numerical
package versions and destination mapping. Device Portal interoperability, mobile
picker grants, native credential stores and large-file transfers require real
Android/iOS and Xbox validation. CI is the mobile build of record.

On mobile, choose the firmware ZIP or select its loose `.nca` files. Folder picking is available only on desktop because the Tauri mobile dialog does not expose it.
