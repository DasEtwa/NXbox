# Save synchronization with SwitchSaveSync

NXbox uses `Nintendo Switch Saves/<game>/<account>/`, with loose, unchanged save
files. The game name is the first nonempty NACP language entry; invalid filename
characters become `_`, and trailing spaces/dots are removed, as in SwitchSaveSync.
The first sync asks which Switch account folder to use. That choice survives
nickname changes. Device saves use `console`.

Before boot, the full-screen message is “Syncing save with the cloud, please wait”.
The percentage counts completed files in the displayed transfer stage; connecting
and scanning have no invented percentage. B / Skip cancels HTTP and waits for the
worker to stop before the emulator opens any save. An interrupted sync keeps its
pending marker. Both sides changing, or different saves without a prior marker,
requires a choice: keep Xbox, use cloud, or back up Xbox and use cloud. Previous
Xbox saves remain in versioned LocalState backups before replacement. Failed
writes restore the backup. Eden's `.yuzu_save_size` is excluded from cloud saves.

Google refresh tokens and WebDAV configuration use the app's UWP PasswordVault.
Legacy plaintext refresh tokens are removed and require a fresh login. Never
commit credentials, tokens, keys, generated credential headers or save data.

## Google OAuth setup

1. Open https://console.cloud.google.com/ and select the **same project/client used
   to build your SwitchSaveSync**. With `drive.file`, an unrelated OAuth application
   cannot automatically discover saves created by another application.
2. Under APIs & Services / Library, enable **Google Drive API**.
3. Open **Google Auth Platform**. Configure Branding (name, support email, developer
   contact), Audience (External for personal Google accounts), and Data Access.
   Add only `https://www.googleapis.com/auth/drive.file`.
4. Under Clients, create an OAuth client with application type **TVs and Limited
   Input devices**, if you do not already have SwitchSaveSync's TV client. Use that
   client for both builds. Do not choose Desktop or Web application.
5. During testing, add your Google account as a test user. For continuing use,
   publish the consent app to production; testing refresh tokens expire after
   seven days. Complete any information Google requests in the console.
6. In https://github.com/NspxMiguel/NXbox/settings/secrets/actions choose **New
   repository secret** twice: `NXBOX_GDRIVE_CLIENT_ID` and
   `NXBOX_GDRIVE_CLIENT_SECRET`, using the TV client's values.
7. Run **Build NXbox for Xbox Series X** on `team/t15-savesync`. CI injects the
   values through the environment during CMake configuration. Without both values,
   Google sign-in explains that this build is not configured; WebDAV is available.

Local builds use the same environment variables. `NXBOX_ENABLE_SAVE_SYNC=OFF`
disables Google credentials. CMake does not put environment credentials into its
cache; do not share the generated `save_sync_credentials.h`. OAuth client secrets
embedded in a limited-input application are not a substitute for server secrets.

## WebDAV

Choose WebDAV on the sign-in screen, then enter the HTTPS collection URL, username
and a revocable app password with the on-screen keyboard. Use the same collection
URL as SwitchSaveSync. Folder listing uses PROPFIND Depth 1; creation uses MKCOL;
file transfers use GET/PUT. Extra remote files are removed with DELETE, matching
SwitchSaveSync. Use a provider with version history or a recycle bin. Redirects
are rejected so credentials cannot follow a server to a different origin.

## Validation

`cd tests/port && python3 -m unittest discover -s . -p 'test_*.py'`
compiles and exercises the production C++ name, mapping, fingerprint, conflict and
resumable upload policies against synthetic data. MSVC validation runs through
`.github/workflows/build-nxbox.yml`. Device OAuth approval and a real Switch/Xbox
round trip require the owner's configured client and consoles.
