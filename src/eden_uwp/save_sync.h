// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace EdenXbox::SaveSync {

// Cloud save sync that shares saves with SwitchSaveSync, the Switch homebrew: a real Switch and
// NXbox mirror the same Google Drive folder, so both see one save per game.
//
// Every function here BLOCKS (network and disk) and must be called from a worker thread, never from
// the CoreWindow dispatcher; that thread needs a WinRT apartment (winrt::init_apartment), as the
// game worker thread has. Failures come back in the results, nothing is thrown. Calls that touch a
// save are serialized by an internal lock. There is no UI in this file; the caller shows the
// progress and asks the questions.
//
// Typical use, on one worker thread:
//   const std::string name = GameNameFromNames(nacp.GetApplicationNames());
//   SyncResult r = SyncBeforeBoot(title_id, on_progress, name);
//   if (r.status == SyncStatus::Conflict) {
//       // show r.local_modified / r.cloud_modified, ask the player, then:
//       r = ResolveConflict(title_id, choice, on_progress, name);
//   }
//   ... boot the game, play, close it ...
//   SyncAfterExit(title_id, on_progress, name);
//
// OAuth credentials are injected at build time; refresh tokens use the UWP PasswordVault.
// LocalState contains only sync fingerprints, folder mappings, staging and save backups.

// ---------------------------------------------------------------------------------------------
// Sign-in (OAuth device flow: the TV shows a code, the phone signs in)
// ---------------------------------------------------------------------------------------------

/// True when this build has Google credentials or a WebDAV account has been configured.
bool IsConfigured();
void ResetSyncCancellation();
void CancelSync();
bool SelectAccount(std::uint64_t title_id, const std::string& game_name, const std::string& owner,
                   const std::string& profile_uuid = {}, bool device_save = false);
bool ConfigureWebDav(const std::string& url, const std::string& user, const std::string& password);
bool IsWebDav();
std::string ActiveProfileUuid();
// System save IDs are independent of application IDs in Eden. The caller must supply that
// identity rather than guessing which OS save belongs to a game.
struct SystemSave {
    std::uint64_t save_id = 0;
    bool system_space = true;
};

/// True when a refresh token is stored. It is not validated against Google; a revoked token shows
/// up as SyncStatus::NotSignedIn on the next sync (and the stored token is removed).
bool IsSignedIn();

/// Forgets the stored refresh token (local only, like SwitchSaveSync's sign-out) and cancels any
/// sign-in in progress. Fingerprints in savesync_state.json are kept.
void SignOut();

struct DeviceLogin {
    bool ok = false;
    std::string user_code;        // for example "ABCD-EFGH": show it on the TV
    std::string verification_url; // for example "https://www.google.com/device"
    std::string url_with_code;    // verification_url + "?user_code=" + user_code, for a QR code
    int expires_in_seconds = 0;   // how long the code stays valid
    std::string error;            // English text, only when !ok
};

/// Asks Google for a device code. On success PollDeviceLogin() is the next call.
DeviceLogin StartDeviceLogin();

enum class LoginResult {
    SignedIn,  // the refresh token is stored
    Denied,    // the user refused on the phone
    Expired,   // the code ran out (or Google no longer knows it)
    Cancelled, // CancelDeviceLogin() was called
    Failed,    // no sign-in in progress, bad credential, or Google refused (see eden_uwp_diag.txt)
};

/// Polls Google every few seconds until the user signed in, refused, or the code expired. Blocks
/// for up to expires_in_seconds. Network hiccups are retried, not reported.
LoginResult PollDeviceLogin();

/// Makes a PollDeviceLogin() running on another thread return Cancelled within a fraction of a
/// second. Safe to call at any time.
void CancelDeviceLogin();

// ---------------------------------------------------------------------------------------------
// Saves
// ---------------------------------------------------------------------------------------------

enum class SyncStage {
    Connecting,  // signing in to Google and finding the cloud folder
    Comparing,   // reading the Xbox save and the cloud copy
    Downloading, // cloud -> Xbox, one file per step
    Applying,    // writing the downloaded save into the Eden save folder
    Uploading,   // Xbox -> cloud, one file per step
    Cleaning,    // removing cloud files the Xbox save no longer has
    Finished,
};

struct SyncProgress {
    SyncStage stage = SyncStage::Connecting;
    std::uint64_t done = 0;  // bytes transferred in this stage
    std::uint64_t total = 0; // bytes expected in this stage, 0 when not known yet
    std::string item;        // file being transferred, may be empty
};

/// Called on the calling thread. May be empty.
using ProgressCallback = std::function<void(const SyncProgress&)>;

enum class SyncStatus {
    AccountRequired, // choose a cloud account folder before the first sync
    Failed,        // something went wrong; the Xbox save is unchanged or was put back (see message)
    NotConfigured, // the build has no Google credentials or configured WebDAV account
    NotSignedIn,   // no refresh token, or Google revoked it: sign in again
    NothingToSync, // no save on this console and none in the cloud
    UpToDate,      // both sides already identical; nothing was written
    Uploaded,      // the Xbox save went to the cloud
    Downloaded,    // the cloud save replaced the Xbox save (the old one is in savesync_backup)
    Conflict,      // both sides changed since the last sync: NOTHING was written, the UI must ask
};

struct SyncResult {
    SyncStatus status = SyncStatus::Failed;
    std::vector<std::string> account_folders;
    std::string message;      // short English text for logs and as a fallback
    std::string cloud_folder; // "<game folder>/<account folder>" when known

    // Filled whenever the side was read; the UI shows them next to a Conflict. Times are UTC
    // seconds since 1970-01-01, 0 when unknown: the newest file time on the Xbox, and the newest
    // Drive modifiedTime in the cloud folder.
    std::int64_t local_modified = 0;
    std::int64_t cloud_modified = 0;
    std::uint32_t local_files = 0;
    std::uint32_t cloud_files = 0;
    std::uint64_t local_bytes = 0;
    std::uint64_t cloud_bytes = 0;
};

/// Mirrors one title's save for the ACTIVE Eden user:
/// nand\user\save\0000000000000000\<user>\<TITLEID>.
/// Call it before the game boots; it downloads a newer cloud save (and also uploads when only the
/// Xbox changed). A Conflict result means nothing was written: ask, then call ResolveConflict.
///
/// The active user is Settings::values.current_user (through Eden's ProfileManager), so call this
/// after the settings of the session (eden_settings.txt) were applied.
///
/// title_id is the id Eden names the save folder with (the application's program id).
/// game_name is the game's name exactly as SwitchSaveSync names it: GameNameFromNames() over the
/// NACP names. It may be empty only after one successful sync of this save, because the cloud
/// folder is then remembered in savesync_state.json.
SyncResult SyncBeforeBoot(std::uint64_t title_id, const ProgressCallback& progress = {},
                          const std::string& game_name = {},
                          std::optional<SystemSave> system_save = {},
                          const std::string& profile_uuid = {}, bool device_save = false);

/// The same reconciliation, meant for after the game closed and Eden released the save files.
SyncResult SyncAfterExit(std::uint64_t title_id, const ProgressCallback& progress = {},
                         const std::string& game_name = {},
                         std::optional<SystemSave> system_save = {},
                         const std::string& profile_uuid = {}, bool device_save = false);

enum class ConflictChoice {
    KeepXbox,  // the Xbox save overwrites the cloud copy
    KeepBoth,  // back up Xbox locally, then continue with the cloud save
    KeepCloud, // the cloud copy overwrites the Xbox save (the old one goes to savesync_backup)
};

/// Applies the user's answer to a Conflict. It refuses (Failed) to replace a side with nothing.
SyncResult ResolveConflict(std::uint64_t title_id, ConflictChoice choice,
                           const ProgressCallback& progress = {}, const std::string& game_name = {},
                           std::optional<SystemSave> system_save = {},
                           const std::string& profile_uuid = {}, bool device_save = false);

/// The game name SwitchSaveSync uses for a title: the first non-empty application name among the
/// first 16 NACP language entries (FileSys::NACP::GetApplicationNames()), independent of the
/// console language. With no name, uses SwitchSaveSync's unknown-title folder when an ID is given.
std::string GameNameFromNames(const std::vector<std::string>& nacp_application_names,
                              std::uint64_t title_id = 0);

} // namespace EdenXbox::SaveSync
