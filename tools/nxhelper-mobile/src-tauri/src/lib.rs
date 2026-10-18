mod portal;
mod protocol;

use futures_util::{stream, StreamExt, TryStreamExt};
use portal::{Credentials, Portal, Result};
use protocol::{newest_package, part_plan, private_ipv4, upload_directory, PART_SIZE};
use serde::Serialize;
use std::{
    collections::HashSet,
    io::{Read, Seek, SeekFrom},
    sync::Mutex,
    time::{Duration, Instant},
};
use tauri::{Emitter, Manager};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_fs::{FilePath, FsExt, OpenOptions};
use tokio::io::AsyncReadExt;
use tokio_util::{io::ReaderStream, sync::CancellationToken};

type App = tauri::AppHandle;
struct Source {
    name: String,
    file: std::fs::File,
    size: u64,
    scope: FilePath,
}
#[derive(Default)]
struct Session {
    sources: Vec<Source>,
    connection: Option<(String, Credentials)>,
    active: Option<CancellationToken>,
}
#[derive(Default)]
struct State(Mutex<Session>);
#[derive(Clone, Serialize)]
struct FileInfo {
    name: String,
    size: u64,
}
#[derive(Clone, Serialize)]
struct Progress {
    file: String,
    sent: u64,
    total: u64,
    completed: bool,
}
#[derive(Serialize)]
struct Summary {
    files: usize,
    bytes: u64,
    warnings: Vec<String>,
}
fn error(e: impl std::fmt::Display) -> String {
    e.to_string()
}
fn storage(
    app: &App,
    action: &str,
    host: &str,
    value: Option<String>,
) -> Result<serde_json::Value> {
    app.state::<tauri_plugin_credentials::Credentials<tauri::Wry>>()
        .call(action, serde_json::json!({"host": host, "value": value}))
}

#[tauri::command]
async fn discover() -> Result<Vec<String>> {
    let mut candidates = HashSet::new();
    for interface in if_addrs::get_if_addrs().map_err(error)? {
        if let std::net::IpAddr::V4(ip) = interface.ip() {
            if !private_ipv4(&ip.to_string()) {
                continue;
            }
            let [a, b, c, own] = ip.octets();
            for last in 1..255 {
                if last != own {
                    candidates.insert(format!("{a}.{b}.{c}.{last}"));
                }
            }
        }
    }
    Ok(stream::iter(candidates.into_iter().take(1024))
        .map(|host| async move {
            match tokio::time::timeout(
                Duration::from_millis(500),
                tokio::net::TcpStream::connect((host.as_str(), 11443)),
            )
            .await
            {
                Ok(Ok(_)) => Some(host),
                _ => None,
            }
        })
        .buffer_unordered(24)
        .filter_map(|h| async { h })
        .collect()
        .await)
}
#[tauri::command]
async fn remembered(app: App, host: String) -> Result<Option<Credentials>> {
    if !private_ipv4(&host) {
        return Err("privateIpRequired".into());
    }
    let result = tauri::async_runtime::spawn_blocking(move || storage(&app, "load", &host, None))
        .await
        .map_err(error)??;
    result["value"]
        .as_str()
        .map(|value| serde_json::from_str(value).map_err(error))
        .transpose()
}
#[tauri::command]
async fn connect(app: App, host: String, credentials: Credentials, remember: bool) -> Result<()> {
    let mut portal = Portal::new(&host, credentials.clone())?;
    let packages = portal.packages().await?;
    if newest_package(&packages).is_none() {
        return Err("nxboxMissing".into());
    }
    let saved = remember
        .then(|| serde_json::to_string(&credentials))
        .transpose()
        .map_err(error)?;
    let storage_app = app.clone();
    let storage_host = host.clone();
    // Unchecking remember removes a previously saved credential for this Xbox.
    tauri::async_runtime::spawn_blocking(move || {
        storage(&storage_app, "save", &storage_host, saved)
    })
    .await
    .map_err(error)??;
    let state = app.state::<State>();
    let mut state = state.0.lock().map_err(error)?;
    if state.active.is_some() {
        return Err("busy".into());
    }
    state.connection = Some((host, credentials));
    Ok(())
}
fn open_source(app: &App, path: FilePath, mut name: String) -> Result<Source> {
    if ["prod.keys", "title.keys"].contains(&name.to_ascii_lowercase().as_str()) {
        name.make_ascii_lowercase();
    }
    if !name.to_ascii_lowercase().ends_with(".zip") {
        upload_directory(&name)?;
    }
    let mut options = OpenOptions::new();
    options.read(true);
    let mut file = app
        .fs()
        .open(path.clone(), options)
        .map_err(|_| "fileUnreadable".to_string())?;
    // Seek to end also obtains the real size for Android descriptors whose metadata length is zero.
    let size = file
        .seek(SeekFrom::End(0))
        .map_err(|_| "fileNotSeekable".to_string())?;
    file.seek(SeekFrom::Start(0)).map_err(error)?;
    if size == 0 {
        return Err("fileInvalid".into());
    }
    Ok(Source {
        name,
        file,
        size,
        scope: path,
    })
}
fn release_sources(app: &App, sources: Vec<Source>) {
    for source in sources {
        drop(source.file);
        #[cfg(target_os = "ios")]
        let _ = app
            .fs()
            .stop_accessing_security_scoped_resource(source.scope);
        #[cfg(not(target_os = "ios"))]
        let _ = (&app, source.scope);
    }
}
#[tauri::command]
fn can_pick_folder() -> bool {
    cfg!(desktop)
}

#[tauri::command]
async fn pick_files(app: App, folder: bool) -> Result<Vec<FileInfo>> {
    tauri::async_runtime::spawn_blocking(move || {
        {
            if app
                .state::<State>()
                .0
                .lock()
                .map_err(error)?
                .active
                .is_some()
            {
                return Err("busy".into());
            }
        }
        let picker = app.dialog().file();
        let paths = if folder {
            #[cfg(desktop)]
            {
                picker.blocking_pick_folder().map(|p| vec![p])
            }
            #[cfg(mobile)]
            {
                return Err("folderUnsupported".into());
            }
        } else {
            picker
                .add_filter("NXbox", &["nsp", "nsz", "xci", "xcz", "keys", "zip", "nca"])
                .blocking_pick_files()
        };
        let Some(paths) = paths else {
            return Ok(Vec::new());
        };
        let mut added = Vec::new();
        let result = (|| -> Result<()> {
            for path in paths {
                #[cfg(target_os = "ios")]
                let folder_access = if folder {
                    let mut options = OpenOptions::new();
                    options.read(true);
                    Some(app.fs().open(path.clone(), options).map_err(error)?)
                } else {
                    None
                };
                #[cfg(target_os = "android")]
                let entries: Vec<(FilePath, String)> = {
                    if matches!(&path, FilePath::Url(u) if u.scheme() == "content") {
                        let data = storage(&app, "documents", &path.to_string(), None)?;
                        data["files"]
                            .as_array()
                            .ok_or("fileUnreadable")?
                            .iter()
                            .map(|item| {
                                let path =
                                    serde_json::from_value(item["path"].clone()).map_err(error)?;
                                Ok((
                                    path,
                                    item["name"].as_str().ok_or("fileInvalid")?.to_string(),
                                ))
                            })
                            .collect::<Result<_>>()?
                    } else {
                        local_entries(&path, folder)?
                    }
                };
                #[cfg(not(target_os = "android"))]
                let entries = local_entries(&path, folder)?;
                for (path, name) in entries {
                    added.push(open_source(&app, path, name)?);
                }
                #[cfg(target_os = "ios")]
                if folder_access.is_some() {
                    drop(folder_access);
                    let _ = app.fs().stop_accessing_security_scoped_resource(path);
                }
            }
            Ok(())
        })();
        if let Err(e) = result {
            release_sources(&app, added);
            return Err(e);
        }
        let state = app.state::<State>();
        let mut state = state.0.lock().map_err(error)?;
        if state.active.is_some() {
            release_sources(&app, added);
            return Err("busy".into());
        }
        let mut names: HashSet<_> = state
            .sources
            .iter()
            .map(|s| s.name.to_ascii_lowercase())
            .collect();
        if added
            .iter()
            .any(|s| !names.insert(s.name.to_ascii_lowercase()))
        {
            release_sources(&app, added);
            return Err("fileConflict".into());
        }
        state.sources.extend(added);
        Ok(state
            .sources
            .iter()
            .map(|s| FileInfo {
                name: s.name.clone(),
                size: s.size,
            })
            .collect())
    })
    .await
    .map_err(error)?
}
fn local_entries(path: &FilePath, folder: bool) -> Result<Vec<(FilePath, String)>> {
    let local = path.clone().into_path().map_err(error)?;
    if folder {
        std::fs::read_dir(local)
            .map_err(error)?
            .filter_map(|entry| match entry {
                Ok(entry)
                    if entry
                        .path()
                        .extension()
                        .is_some_and(|e| e.eq_ignore_ascii_case("nca")) =>
                {
                    Some(Ok((
                        FilePath::Path(entry.path()),
                        entry.file_name().to_string_lossy().to_string(),
                    )))
                }
                Ok(_) => None,
                Err(e) => Some(Err(error(e))),
            })
            .collect()
    } else {
        Ok(vec![(
            path.clone(),
            local
                .file_name()
                .ok_or("fileInvalid")?
                .to_string_lossy()
                .to_string(),
        )])
    }
}
#[tauri::command]
fn clear_files(app: App) -> Result<()> {
    let state = app.state::<State>();
    let mut state = state.0.lock().map_err(error)?;
    if state.active.is_some() {
        return Err("busy".into());
    }
    release_sources(&app, std::mem::take(&mut state.sources));
    Ok(())
}
#[tauri::command]
fn cancel(app: App) {
    if let Ok(state) = app.state::<State>().0.lock() {
        if let Some(token) = &state.active {
            token.cancel();
        }
    }
}

async fn upload_source(
    app: &App,
    portal: &mut Portal,
    package: &str,
    source: Source,
) -> Result<()> {
    let directory = upload_directory(&source.name)?;
    let mut file = tokio::fs::File::from_std(source.file);
    use tokio::io::AsyncSeekExt;
    file.seek(SeekFrom::Start(0)).await.map_err(error)?;
    let plan = part_plan(&source.name, source.size);
    for (index, (name, size)) in plan.iter().enumerate() {
        let offset = index as u64 * PART_SIZE;
        let file = file.try_clone().await.map_err(error)?;
        let progress_app = app.clone();
        let progress_name = source.name.clone();
        let total = source.size;
        let mut sent = offset;
        let mut last = Instant::now();
        let reader =
            ReaderStream::with_capacity(file.take(*size), 256 * 1024).inspect_ok(move |chunk| {
                sent += chunk.len() as u64;
                if last.elapsed() >= Duration::from_millis(150) || sent == total {
                    let _ = progress_app.emit(
                        "upload-progress",
                        Progress {
                            file: progress_name.clone(),
                            sent,
                            total,
                            completed: false,
                        },
                    );
                    last = Instant::now();
                }
            });
        app.emit(
            "upload-progress",
            Progress {
                file: source.name.clone(),
                sent: offset,
                total: source.size,
                completed: false,
            },
        )
        .map_err(error)?;
        portal
            .upload(
                package,
                directory,
                name,
                *size,
                reqwest::Body::wrap_stream(reader),
            )
            .await?;
    }
    if plan.len() > 1 {
        let manifest = format!("parts={}\nsize={}\n", plan.len(), source.size);
        portal
            .upload(
                package,
                directory,
                &format!("{}.nxparts", source.name),
                manifest.len() as u64,
                manifest.into(),
            )
            .await?;
    }
    app.emit(
        "upload-progress",
        Progress {
            file: source.name,
            sent: source.size,
            total: source.size,
            completed: true,
        },
    )
    .map_err(error)?;
    Ok(())
}
struct Temporary(std::path::PathBuf);
impl Drop for Temporary {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
async fn prepare(
    app: &App,
    sources: Vec<Source>,
    temporary: &Temporary,
    cancel: CancellationToken,
) -> Result<Vec<Source>> {
    let app = app.clone();
    let directory = temporary.0.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut output = Vec::new();
        let mut names = HashSet::new();
        for mut source in sources {
            if !source.name.to_ascii_lowercase().ends_with(".zip") {
                output.push(source);
                continue;
            }
            source.file.seek(SeekFrom::Start(0)).map_err(error)?;
            let mut archive =
                zip::ZipArchive::new(source.file).map_err(|_| "firmwareZipInvalid")?;
            for index in 0..archive.len() {
                if cancel.is_cancelled() {
                    return Err("cancelled".into());
                }
                let mut entry = archive.by_index(index).map_err(error)?;
                if entry.is_dir() || !entry.name().to_ascii_lowercase().ends_with(".nca") {
                    continue;
                }
                let name = entry
                    .name()
                    .rsplit('/')
                    .next()
                    .ok_or("fileInvalid")?
                    .to_string();
                upload_directory(&name)?;
                if !names.insert(name.to_ascii_lowercase()) {
                    return Err("fileConflict".into());
                }
                if entry.size() > 2 * PART_SIZE || output.len() > 4096 {
                    return Err("firmwareZipInvalid".into());
                }
                let path = directory.join(&name);
                let mut file = std::fs::OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&path)
                    .map_err(error)?;
                let mut buffer = [0u8; 256 * 1024];
                let mut written = 0u64;
                loop {
                    if cancel.is_cancelled() {
                        return Err("cancelled".into());
                    }
                    let count = entry.read(&mut buffer).map_err(error)?;
                    if count == 0 {
                        break;
                    }
                    written += count as u64;
                    if written > 2 * PART_SIZE {
                        return Err("firmwareZipInvalid".into());
                    }
                    std::io::Write::write_all(&mut file, &buffer[..count])
                        .map_err(|_| "phoneSpace".to_string())?;
                }
                output.push(open_source(&app, FilePath::Path(path), name)?);
            }
        }
        let mut names = HashSet::new();
        if output
            .iter()
            .any(|s| !names.insert(s.name.to_ascii_lowercase()))
        {
            return Err("fileConflict".into());
        }
        output.sort_by_key(|s| match upload_directory(&s.name).unwrap_or_default() {
            "LocalState/games" => 0,
            "LocalState/eden/keys" => 1,
            _ => 2,
        });
        if output.is_empty() {
            return Err("fileInvalid".into());
        }
        Ok(output)
    })
    .await
    .map_err(error)?
}
#[tauri::command]
async fn send(app: App) -> Result<Summary> {
    let (host, credentials, sources, token) = {
        let state = app.state::<State>();
        let mut state = state.0.lock().map_err(error)?;
        if state.active.is_some() {
            return Err("busy".into());
        }
        let (host, credentials) = state.connection.clone().ok_or("connectionRequired")?;
        if state.sources.is_empty() {
            return Err("fileInvalid".into());
        }
        let sources = state
            .sources
            .iter()
            .map(|s| {
                Ok(Source {
                    name: s.name.clone(),
                    file: s.file.try_clone().map_err(error)?,
                    size: s.size,
                    scope: s.scope.clone(),
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let token = CancellationToken::new();
        state.active = Some(token.clone());
        (host, credentials, sources, token)
    };
    let result = async {
        let path = app.path().app_cache_dir().map_err(error)?.join(format!("firmware-{}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_err(error)?.as_nanos()));
        std::fs::create_dir_all(&path).map_err(error)?;
        let temporary = Temporary(path);
        // Await blocking extraction on cancellation so its temp files cannot outlive cleanup.
        let sources = prepare(&app, sources, &temporary, token.clone()).await?;
        let transfer = async {
            let mut portal = Portal::new(&host, credentials)?;
            let packages = portal.packages().await?;
            let package = newest_package(&packages).ok_or("nxboxMissing")?;
            let bytes = sources.iter().try_fold(0u64, |total, s| total.checked_add(s.size).ok_or("fileInvalid"))?;
            let free = portal.free_space().await;
            if free.is_some_and(|free| free < bytes) { return Err("driveSpace".into()); }
            let warnings = if free.is_none() { vec!["xboxSpaceWarning".into()] } else { Vec::new() };
            if !warnings.is_empty() { app.emit("upload-warning", "xboxSpaceWarning").map_err(error)?; }
            let count = sources.len(); let mut made = HashSet::new();
            for source in sources {
                let mut parent = String::new();
                for name in upload_directory(&source.name)?.split('/') {
                    let full = if parent.is_empty() { name.to_string() } else { format!("{parent}/{name}") };
                    if made.insert(full.clone()) { portal.folder(package, &parent, name).await?; }
                    parent = full;
                }
                upload_source(&app, &mut portal, package, source).await?;
            }
            Ok(Summary { files: count, bytes, warnings })
        };
        tokio::select! { biased; _ = token.cancelled() => Err("cancelled".into()), result = transfer => result }
    }.await;
    app.state::<State>().0.lock().map_err(error)?.active = None;
    result
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_credentials::init())
        .manage(State::default())
        .invoke_handler(tauri::generate_handler![
            discover,
            remembered,
            connect,
            pick_files,
            can_pick_folder,
            clear_files,
            send,
            cancel
        ])
        .run(tauri::generate_context!())
        .expect("NxHelper failed to start");
}
