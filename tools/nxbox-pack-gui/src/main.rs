// NxbHelper GUI: a step-by-step assistant over nxbhelper-core (`nxbhelper-core --api`, JSON lines).
mod design;
use base64::Engine;
use eframe::egui;
use serde_json::{Value, json};
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{Receiver, channel};

fn main() -> eframe::Result {
    let icon = image::load_from_memory(include_bytes!("../assets/app.png"))
        .expect("Embedded app icon")
        .to_rgba8();
    let icon = egui::IconData {
        width: icon.width(),
        height: icon.height(),
        rgba: icon.into_raw(),
    };
    eframe::run_native(
        "NxbHelper",
        eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default()
                .with_inner_size([640.0, 800.0])
                .with_min_inner_size([560.0, 560.0])
                .with_icon(icon),
            ..Default::default()
        },
        Box::new(|cc| {
            cc.egui_ctx.set_theme(egui::ThemePreference::System);
            design::configure(&cc.egui_ctx);
            design::configure_fonts(&cc.egui_ctx);
            Ok(Box::<App>::default())
        }),
    )
}

fn pt() -> bool {
    static LANGUAGE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *LANGUAGE.get_or_init(|| {
        let forced = std::env::var("NXBHELPER_LANG").or_else(|_| std::env::var("NXHELPER_LANG"))
            .or_else(|_| std::env::var("NXBOX_PACK_LANG"))
            .ok()
            .filter(|s| s == "pt" || s == "en");
        forced
            .or_else(sys_locale::get_locale)
            .is_some_and(|s| s.to_lowercase().starts_with("pt"))
    })
}
fn t(pt_text: &'static str, en_text: &'static str) -> &'static str {
    if pt() { pt_text } else { en_text }
}

/// Runs one core request on a worker thread; every JSON line it prints arrives on the channel.
fn core(request: Value) -> Receiver<Value> {
    let (tx, rx) = channel();
    std::thread::spawn(move || {
        // NXBOX_PACK_CORE="bun /path/to/cli.ts" in development; the bundled binary otherwise.
        let mut argv: Vec<String> = match std::env::var("NXBOX_PACK_CORE") {
            Ok(cmd) => cmd.split_whitespace().map(String::from).collect(),
            Err(_) => {
                let exe = std::env::current_exe().unwrap_or_default();
                let name = if cfg!(windows) {
                    "nxbhelper-core.exe"
                } else {
                    "nxbhelper-core"
                };
                vec![exe.with_file_name(name).to_string_lossy().into_owned()]
            }
        };
        argv.push("--api".into());
        let child = Command::new(&argv[0])
            .args(&argv[1..])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn();
        let mut child = match child {
            Ok(c) => c,
            Err(e) => {
                let _ = tx.send(json!({ "error": format!("core: {e}") }));
                return;
            }
        };
        let _ = child
            .stdin
            .take()
            .unwrap()
            .write_all(request.to_string().as_bytes());
        for line in BufReader::new(child.stdout.take().unwrap())
            .lines()
            .map_while(Result::ok)
        {
            if let Ok(v) = serde_json::from_str(&line) {
                let _ = tx.send(v);
            }
        }
        let _ = child.wait();
    });
    rx
}

#[derive(PartialEq, Clone, Copy)]
enum Mode {
    Game,
    Full,
    Drive,
    Xbox,
    Check,
}

#[derive(PartialEq, Clone, Copy, Debug)]
enum Page {
    Welcome,
    Check,
    Game,
    Keys,
    Firmware,
    Games,
    Extras,
    Install,
    Progress,
}

fn flow(mode: Option<Mode>) -> &'static [Page] {
    match mode {
        Some(Mode::Game) => &[
            Page::Welcome,
            Page::Game,
            Page::Keys,
            Page::Extras,
            Page::Install,
            Page::Progress,
        ],
        Some(Mode::Full) => &[
            Page::Welcome,
            Page::Keys,
            Page::Firmware,
            Page::Games,
            Page::Install,
            Page::Progress,
        ],
        Some(Mode::Drive | Mode::Xbox) => &[
            Page::Welcome,
            Page::Keys,
            Page::Firmware,
            Page::Games,
            Page::Install,
            Page::Progress,
        ],
        Some(Mode::Check) => &[Page::Welcome, Page::Check],
        None => &[Page::Welcome],
    }
}

#[derive(Default)]
struct App {
    page_index: usize,
    mode: Option<Mode>,
    games: Vec<PathBuf>,
    info: HashMap<PathBuf, Value>,
    icons: HashMap<PathBuf, egui::TextureHandle>,
    inspecting: Vec<(PathBuf, Receiver<Value>)>,
    keys: Option<PathBuf>,
    title_keys: Option<PathBuf>,
    drive: Option<PathBuf>,
    report: Option<Value>,
    warnings: Vec<String>,
    firmware: Option<PathBuf>,
    official_ui: bool,
    saves: Option<PathBuf>,
    mods: Vec<PathBuf>,
    manual: bool,
    host: String,
    user: String,
    password: String,
    remember: bool,
    out: Option<PathBuf>,
    discover_rx: Option<Receiver<Value>>,
    discovery_started: bool,
    run_rx: Option<Receiver<Value>>,
    status: String,
    progress: Option<f32>,
    install_rect: Option<egui::Rect>,
    done: Option<String>,
    error: Option<String>,
}

fn is_game(p: &Path) -> bool {
    matches!(
        p.extension()
            .and_then(|e| e.to_str())
            .map(str::to_lowercase)
            .as_deref(),
        Some("nsp" | "xci" | "nsz" | "xcz")
    )
}

fn file_name(p: &Path) -> String {
    p.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn heading(ui: &mut egui::Ui, title: &str, body: &str) {
    ui.label(egui::RichText::new(title).font(design::heading_font(ui.ctx(), 32.0)));
    ui.add_space(6.0);
    ui.label(body);
    ui.add_space(22.0);
}

fn choose_file(ui: &mut egui::Ui, label: &str, folder: bool) -> Option<PathBuf> {
    if ui.button(label).clicked() {
        let dialog = rfd::FileDialog::new();
        return if folder {
            dialog.pick_folder()
        } else {
            dialog.pick_file()
        };
    }
    None
}

fn compat_text(status: &str) -> (egui::Color32, &'static str, &'static str) {
    match status {
        "playable" => (
            design::GOOD,
            t("Jogável", "Playable"),
            t(
                "Funciona do início ao fim no Xbox.",
                "Works from start to finish on Xbox.",
            ),
        ),
        "ingame" => (
            design::WARN,
            t("Funciona com problemas", "Works with issues"),
            t(
                "Chega ao jogo, mas há falhas gráficas ou de desempenho.",
                "Reaches gameplay, but has graphics or performance issues.",
            ),
        ),
        "boots" => (
            design::WARN,
            t("Apenas abre", "Boots only"),
            t(
                "Abre, mas ainda não chega à jogabilidade.",
                "Opens, but does not reach gameplay yet.",
            ),
        ),
        "broken" => (
            design::BAD,
            t("Ainda não funciona", "Does not work yet"),
            t(
                "Não é possível jogar no momento.",
                "Cannot be played at the moment.",
            ),
        ),
        _ => (
            design::UNKNOWN,
            t("Ainda não testado", "Not tested yet"),
            t(
                "Ninguém mediu este jogo no Xbox; pode ou não funcionar.",
                "This game has not been measured on Xbox; it may or may not work.",
            ),
        ),
    }
}

impl App {
    fn page(&self) -> Page {
        flow(self.mode)[self.page_index.min(flow(self.mode).len() - 1)]
    }

    fn add_game(&mut self, path: PathBuf) {
        if self.mode == Some(Mode::Game) {
            self.games.clear();
        }
        if !self.games.contains(&path) {
            self.inspecting.push((path.clone(), core(json!({ "cmd": "inspect", "game": path, "keys": self.keys, "language": if pt() { "pt-BR" } else { "en" } }))));
            self.games.push(path);
        }
    }

    fn set_keys(&mut self, path: PathBuf) {
        self.keys = Some(path);
        self.inspecting.clear();
        self.icons.clear();
        for game in &self.games {
            self.inspecting.push((game.clone(), core(json!({ "cmd": "inspect", "game": game, "keys": self.keys, "language": if pt() { "pt-BR" } else { "en" } }))));
        }
    }

    fn take_drops(&mut self, ctx: &egui::Context) {
        for file in ctx.input(|i| i.raw.dropped_files.clone()) {
            let path = file.path().to_path_buf();
            if path.as_os_str().is_empty() {
                continue;
            }
            if is_game(&path) {
                self.add_game(path);
            } else if path
                .file_name()
                .is_some_and(|n| n.eq_ignore_ascii_case("prod.keys"))
            {
                self.set_keys(path);
            } else if self.page() == Page::Firmware
                || (self.page() == Page::Extras && self.firmware.is_none())
            {
                self.firmware = Some(path);
            }
        }
    }

    fn poll(&mut self, ctx: &egui::Context) {
        let mut finished = Vec::new();
        for (i, (path, rx)) in self.inspecting.iter().enumerate() {
            if let Ok(v) = rx.try_recv() {
                if let Some(encoded) = v["iconBase64"].as_str()
                    && let Ok(bytes) = base64::engine::general_purpose::STANDARD.decode(encoded)
                    && let Ok(image) = image::load_from_memory(&bytes)
                {
                    let rgba = image.to_rgba8();
                    let pixels = egui::ColorImage::from_rgba_unmultiplied(
                        [rgba.width() as usize, rgba.height() as usize],
                        rgba.as_raw(),
                    );
                    self.icons.insert(
                        path.clone(),
                        ctx.load_texture("game-icon", pixels, egui::TextureOptions::LINEAR),
                    );
                }
                self.info.insert(path.clone(), v);
                finished.push(i);
            }
        }
        for i in finished.into_iter().rev() {
            self.inspecting.remove(i);
        }
        if let Some(v) = self.discover_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            if self.host.is_empty()
                && let Some(h) = v["hosts"].get(0).and_then(Value::as_str)
            {
                self.host = h.into();
            }
            self.discover_rx = None;
        }
        while let Some(v) = self.run_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            if let Some(warning) = v["warning"].as_str() {
                self.warnings.push(warning.into());
            }
            if v["items"].is_array() {
                self.report = Some(v.clone());
                self.done = Some(v["root"].as_str().unwrap_or("").into());
                self.run_rx = None;
            }
            if v["report"].is_object() {
                self.report = Some(v["report"].clone());
            }
            if let Some(e) = v["error"].as_str() {
                self.error = Some(v["message"].as_str().unwrap_or(e).into());
            } else if v["done"].as_bool() == Some(true) {
                self.done = Some(
                    v["path"]
                        .as_str()
                        .or(v["name"].as_str())
                        .unwrap_or("")
                        .into(),
                );
            } else if let Some(step) = v["step"].as_str() {
                let step = match step {
                    "firmware" => t("Verificando o firmware", "Verifying the firmware"),
                    "runtime" => t("Baixando o NXbox", "Downloading NXbox"),
                    "prepare" => t("Preparando o jogo", "Preparing the game"),
                    "connect" => t("Conectando ao Xbox", "Connecting to the Xbox"),
                    "upload" if self.mode == Some(Mode::Drive) => {
                        t("Copiando para o pendrive", "Copying to the USB drive")
                    }
                    "upload" => t("Enviando ao Xbox", "Sending to the Xbox"),
                    other => {
                        if other.is_empty() {
                            ""
                        } else {
                            t("Trabalhando", "Working")
                        }
                    }
                };
                self.status = match v["file"].as_str() {
                    Some(f) => format!("{step}: {}", file_name(Path::new(f))),
                    None => step.into(),
                };
                self.progress = match (v["sent"].as_f64(), v["total"].as_f64()) {
                    (Some(s), Some(t)) if t > 0.0 => Some((s / t) as f32),
                    _ => None,
                };
            }
        }
        if self.done.is_some() || self.error.is_some() {
            self.run_rx = None;
        }
    }

    fn can_continue(&self) -> bool {
        match self.page() {
            Page::Welcome => self.mode.is_some(),
            Page::Check => false,
            Page::Game => {
                self.games.len() == 1
                    && self
                        .info
                        .get(&self.games[0])
                        .is_some_and(|i| i.get("error").is_none())
            }
            Page::Keys => {
                self.keys
                    .as_ref()
                    .is_some_and(|p| file_name(p).eq_ignore_ascii_case("prod.keys"))
                    && self.inspecting.is_empty()
            }
            Page::Games if matches!(self.mode, Some(Mode::Drive | Mode::Xbox)) => {
                !self.games.is_empty()
            }
            Page::Firmware | Page::Games | Page::Extras => true,
            Page::Install => {
                if self.mode == Some(Mode::Drive) {
                    self.drive.is_some()
                } else if self.manual && self.mode != Some(Mode::Xbox) {
                    self.out.is_some()
                } else {
                    !self.host.trim().is_empty()
                }
            }
            Page::Progress => false,
        }
    }

    fn start(&mut self) {
        let full = self.mode == Some(Mode::Full);
        let target = if self.mode == Some(Mode::Drive) {
            json!({ "kind": "export", "out": self.drive })
        } else if self.manual && self.mode != Some(Mode::Xbox) {
            json!({ "kind": "export", "out": self.out })
        } else {
            json!({
                "kind": "install", "host": self.host.trim(), "user": self.user,
                "password": self.password, "remember": self.remember,
            })
        };
        let job = json!({
            "mode": match self.mode { Some(Mode::Drive) => "drive", Some(Mode::Xbox) => "xbox", Some(Mode::Full) => "full", _ => "game" },
            "titleKeys": self.title_keys, "language": if pt() { "pt" } else { "en" },
            "games": self.games, "keys": self.keys, "firmware": self.firmware,
            "officialUi": full && self.official_ui && self.firmware.is_some(),
            "name": self.games.first().and_then(|p| self.info.get(p)).and_then(|i| i["name"].as_str()),
            "saves": self.saves, "mods": self.mods, "target": target,
        });
        self.error = None;
        self.report = None;
        self.warnings.clear();
        self.done = None;
        self.progress = None;
        self.status = t("Iniciando a instalação", "Starting the installation").into();
        self.run_rx = Some(core(json!({ "cmd": "run", "job": job })));
    }

    fn show_report(&mut self, ui: &mut egui::Ui) {
        let Some(report) = self.report.clone() else {
            return;
        };
        if report["ready"].as_bool() == Some(true) {
            ui.heading(t(
                "Pronto pra plugar no Xbox",
                "Ready to plug into the Xbox",
            ));
        }
        let mut fixable = false;
        if let Some(items) = report["items"].as_array() {
            for item in items {
                let (symbol, color) = match item["status"].as_str() {
                    Some("ok") => ("✓", design::GOOD),
                    Some("warn") => ("!", design::WARN),
                    _ => ("✗", design::BAD),
                };
                ui.colored_label(
                    design::compat_ink(color, ui.visuals().dark_mode),
                    format!("{} {}", symbol, item["title"].as_str().unwrap_or("")),
                );
                if let Some(detail) = item["detail"].as_str() {
                    ui.label(detail);
                }
                if let Some(fix) = item["fix"].as_str() {
                    ui.label(fix);
                }
                ui.add_space(10.0);
                fixable |= item["id"] == "firmware"
                    && item["fix"].as_str()
                        == Some(t(
                            "NxbHelper pode descompactar. Escolha Consertar.",
                            "NxbHelper can unpack it. Choose Fix it.",
                        ));
            }
        }
        if fixable
            && self.run_rx.is_none()
            && ui.add(design::primary(t("Consertar", "Fix it"))).clicked()
        {
            self.error = None;
            self.done = None;
            self.run_rx = Some(core(
                json!({ "cmd": "fixDrive", "path": report["root"], "language": if pt() { "pt" } else { "en" } }),
            ));
        }
    }

    fn game_card(&self, ui: &mut egui::Ui, path: &Path) -> bool {
        let amount = design::motion(ui.ctx(), ui.id().with("game-drop"), true, 0.22);
        egui::Frame::new().fill(design::surface(ui)).corner_radius(egui::lerp(16.0..=22.0, amount) as u8)
            .stroke(egui::Stroke::new(1.0, design::BORDER)).inner_margin(18).show(ui, |ui| {
            ui.set_width((ui.available_width()).max(0.0));
            ui.set_min_height(egui::lerp(172.0..=120.0, amount));
            ui.horizontal_top(|ui| {
                if let Some(icon) = self.icons.get(path) {
                    ui.add(egui::Image::new(icon).fit_to_exact_size(egui::vec2(64.0, 64.0)).corner_radius(16));
                }
                ui.vertical(|ui| {
                    match self.info.get(path) {
                        None => { ui.strong(file_name(path)); ui.spinner(); ui.label(t("Identificando o jogo…", "Identifying the game…")); }
                        Some(info) if info.get("error").is_some() => {
                            ui.label(t("Este arquivo não pôde ser lido como um jogo de Switch. Confira se é um .nsp, .xci, .nsz ou .xcz completo.", "This file could not be read as a Switch game. Check that it is a complete .nsp, .xci, .nsz or .xcz."));
                        }
                        Some(info) => {
                            ui.label(egui::RichText::new(info["name"].as_str().unwrap_or("?")).font(design::heading_font(ui.ctx(), 22.0)));
                            ui.monospace(format!("ID {}", info["titleId"].as_str().unwrap_or(t("desconhecido", "unknown"))));
                            ui.horizontal_wrapped(|ui| {
                                if let Some(version) = info["version"].as_str() { ui.monospace(version); }
                                if let Some(size) = info["size"].as_u64() { ui.monospace(format!("{:.2} GiB", size as f64 / 1073741824.0)); }
                                if let Some(publisher) = info["publisher"].as_str() { ui.small(publisher); }
                            });
                            let (color, label, meaning) = compat_text(info["compat"]["status"].as_str().unwrap_or(""));
                            let color = design::compat_ink(color, ui.visuals().dark_mode);
                            egui::Frame::new().fill(color.linear_multiply(0.13)).corner_radius(255).inner_margin(egui::Margin::symmetric(12, 5)).show(ui, |ui| { ui.label(egui::RichText::new(label).size(15.0).color(color)); });
                            ui.small(meaning);
                            if let Some(note) = info["compat"]["note"].as_str() { ui.small(note); }
                        }
                    }
                });
            });
        }).response.interact(egui::Sense::click()).clicked()
    }

    fn progress_bar(&self, ui: &mut egui::Ui) {
        let amount = design::motion(
            ui.ctx(),
            egui::Id::new("install-morph"),
            self.page() == Page::Progress,
            0.32,
        );
        let width = ui.available_width();
        let height = 16.0;
        let progress = self.progress.unwrap_or(0.08).clamp(0.0, 1.0);
        let (rect, _) = ui.allocate_exact_size(egui::vec2(width, height), egui::Sense::hover());
        let origin = self.install_rect.unwrap_or(rect);
        let morph = egui::Rect::from_min_max(
            origin.min.lerp(rect.min, amount),
            origin.max.lerp(rect.max, amount),
        );
        let painter = ui.ctx().layer_painter(egui::LayerId::new(
            egui::Order::Foreground,
            egui::Id::new("install-bar"),
        ));
        painter.rect_filled(morph, 255, design::surface(ui));
        let fill = egui::lerp(1.0..=progress, amount);
        let filled =
            egui::Rect::from_min_size(morph.min, egui::vec2(morph.width() * fill, morph.height()));
        painter.rect_filled(filled, 255, design::ACCENT);
        if self.progress.is_some() {
            ui.monospace(format!("{:.0}%", progress * 100.0));
        }
    }

    fn render(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        ui.painter()
            .rect_filled(ctx.content_rect(), 0, design::background(ui));
        let steps = flow(self.mode).len();
        let page = self.page();
        egui::Panel::top("steps")
            .frame(
                egui::Frame::new()
                    .fill(design::background(ui))
                    .inner_margin(20),
            )
            .show(ui, |ui| {
                design::centered(ui, |ui| {
                    ui.spacing_mut().interact_size.y = 3.0;
                    ui.horizontal(|ui| {
                        for i in 0..steps.max(6) {
                            let (rect, _) = ui.allocate_exact_size(
                                egui::vec2(
                                    (ui.available_width() / (steps.max(6) - i) as f32 - 10.0)
                                        .max(12.0),
                                    3.0,
                                ),
                                egui::Sense::hover(),
                            );
                            ui.painter().rect_filled(
                                rect,
                                255,
                                if i <= self.page_index {
                                    design::ACCENT
                                } else {
                                    design::secondary(ui).gamma_multiply(0.35)
                                },
                            );
                        }
                    });
                });
            });
        egui::Panel::bottom("nav")
            .frame(
                egui::Frame::new()
                    .fill(design::background(ui))
                    .inner_margin(20),
            )
            .show(ui, |ui| {
                design::centered(ui, |ui| {
                    ui.set_height(48.0);
                    ui.horizontal(|ui| {
                        let running =
                            page == Page::Progress && self.done.is_none() && self.error.is_none();
                        if self.page_index > 0
                            && !running
                            && ui
                                .add(egui::Button::new(t("Voltar", "Back")).frame(false))
                                .clicked()
                        {
                            self.page_index -= 1;
                            if page == Page::Progress {
                                self.run_rx = None;
                            }
                        }
                        if self.mode.is_some() && page != Page::Welcome {
                            ui.small(format!(
                                "{} {} {} {}",
                                t("Etapa", "Step"),
                                self.page_index,
                                t("de", "of"),
                                steps - 1
                            ));
                        }
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if page == Page::Progress {
                                if self.done.is_some()
                                    && ui.add(design::primary(t("Concluir", "Finish"))).clicked()
                                {
                                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                                }
                            } else {
                                let next = if page == Page::Install {
                                    if matches!(self.mode, Some(Mode::Drive | Mode::Xbox)) {
                                        t("Enviar arquivos", "Send files")
                                    } else if self.manual {
                                        t("Gerar pasta", "Create folder")
                                    } else {
                                        t("Instalar", "Install")
                                    }
                                } else {
                                    t("Próximo", "Next")
                                };
                                design::motion(&ctx, egui::Id::new("install-morph"), false, 0.32);
                                let response =
                                    ui.add_enabled(self.can_continue(), design::primary(next));
                                if page == Page::Install {
                                    self.install_rect = Some(response.rect);
                                }
                                if response.clicked() {
                                    if page == Page::Install {
                                        self.start();
                                    }
                                    self.page_index += 1;
                                }
                            }
                        });
                    });
                });
            });
        egui::CentralPanel::default()
            .frame(
                egui::Frame::new()
                    .fill(design::background(ui))
                    .inner_margin(24),
            )
            .show(ui, |ui| {
                egui::ScrollArea::vertical().show(ui, |ui| {
                    design::centered(ui, |ui| self.show_page(ui));
                });
            });
    }

    fn show_page(&mut self, ui: &mut egui::Ui) {
        match self.page() {
            Page::Welcome => {
                heading(
                    ui,
                    t("Bem-vindo ao NxbHelper", "Welcome to NxbHelper"),
                    t(
                        "Este assistente prepara e instala o NXbox no seu Xbox Series X|S. Cada etapa explica o que é necessário; nada é enviado a lugar algum além do seu próprio Xbox.",
                        "This assistant prepares and installs NXbox on your Xbox Series X|S. Each step explains what is needed; nothing is sent anywhere except your own Xbox.",
                    ),
                );
                ui.label(t(
                    "Como deseja enviar seus jogos?",
                    "How would you like to send your games?",
                ));
                ui.add_space(6.0);
                for (mode, label) in [
                    (
                        Mode::Drive,
                        t(
                            "Pelo pendrive (recomendado)",
                            "With a USB drive (recommended)",
                        ),
                    ),
                    (Mode::Xbox, t("Pelo Device Portal", "Over Device Portal")),
                ] {
                    if ui.add(design::primary(label)).clicked() {
                        self.mode = Some(mode);
                        self.page_index = 1;
                    }
                    ui.add_space(8.0);
                }
                if ui
                    .small_button(t(
                        "Instalar um jogo como app próprio",
                        "Install a game as its own app",
                    ))
                    .clicked()
                {
                    self.mode = Some(Mode::Game);
                    self.page_index = 1;
                }
                if ui
                    .small_button(t("Conferir pendrive", "Check USB drive"))
                    .clicked()
                {
                    self.mode = Some(Mode::Check);
                    self.page_index = 1;
                }
                ui.add_space(8.0);
                if ui
                    .small_button(t("NXbox completo / exportar", "Full NXbox / export"))
                    .clicked()
                {
                    self.mode = Some(Mode::Full);
                    self.page_index = 1;
                }
                ui.add_space(16.0);
                ui.small(t(
                    "Requisitos: Xbox em Modo Desenvolvedor com o Device Portal ativado (Dev Home > Remote Access) e arquivos do seu próprio Nintendo Switch.",
                    "Requirements: an Xbox in Developer Mode with Device Portal enabled (Dev Home > Remote Access) and files from your own Nintendo Switch.",
                ));
            }
            Page::Check => {
                heading(
                    ui,
                    t("Conferir pendrive", "Check USB drive"),
                    t(
                        "Escolha a raiz do pendrive preparado para o Xbox.",
                        "Choose the root of the USB drive prepared for Xbox.",
                    ),
                );
                if self.run_rx.is_none() {
                    if let Some(path) =
                        choose_file(ui, t("Escolher pendrive…", "Choose USB drive…"), true)
                    {
                        self.drive = Some(path.clone());
                        self.report = None;
                        self.error = None;
                        self.done = None;
                        self.run_rx = Some(core(
                            json!({ "cmd": "checkDrive", "path": path, "language": if pt() { "pt" } else { "en" } }),
                        ));
                    }
                } else {
                    ui.spinner();
                }
                if let Some(error) = &self.error {
                    ui.label(error);
                }
                self.show_report(ui);
            }
            Page::Game => {
                heading(
                    ui,
                    t("Selecione o jogo", "Select the game"),
                    t(
                        "Arraste o arquivo do jogo para esta janela ou clique em Escolher arquivo. São aceitos arquivos .nsp, .xci, .nsz e .xcz. O NXbox identifica o título e informa o estado de compatibilidade medido no Xbox.",
                        "Drag the game file onto this window or click Choose file. .nsp, .xci, .nsz and .xcz files are accepted. NXbox identifies the title and shows its compatibility status as measured on Xbox.",
                    ),
                );
                let clicked = if let Some(game) = self.games.first().cloned() {
                    self.game_card(ui, &game)
                } else {
                    design::drop_zone(ui, "game-drop", false)
                };
                if clicked && let Some(p) = rfd::FileDialog::new().pick_file() {
                    self.add_game(p);
                }
            }
            Page::Keys => {
                heading(
                    ui,
                    t("Chaves do console (prod.keys)", "Console keys (prod.keys)"),
                    t(
                        "O NXbox precisa das chaves extraídas do seu próprio Nintendo Switch para ler jogos e firmware. O arquivo é copiado somente para o seu Xbox. Para obtê-lo, use o Lockpick_RCM no seu console.",
                        "NXbox needs the keys dumped from your own Nintendo Switch to read games and firmware. The file is copied only to your Xbox. To obtain it, run Lockpick_RCM on your console.",
                    ),
                );
                let clicked = if let Some(keys) = &self.keys {
                    design::file_chip(ui, "keys-drop", &file_name(keys))
                } else {
                    design::drop_zone(ui, "keys-drop", false)
                };
                if clicked && let Some(p) = rfd::FileDialog::new().pick_file() {
                    self.set_keys(p);
                }
                ui.add_space(8.0);
                if !self.inspecting.is_empty() {
                    ui.spinner();
                }
                match &self.keys {
                    Some(k) if file_name(k).eq_ignore_ascii_case("prod.keys") => {
                        ui.horizontal(|ui| {
                            design::check(ui, 20.0);
                            ui.label(file_name(k));
                        });
                    }
                    Some(_) => {
                        ui.label(t(
                            "O arquivo precisa se chamar prod.keys.",
                            "The file must be named prod.keys.",
                        ));
                    }
                    None => {
                        ui.label(t("Nenhum arquivo selecionado.", "No file selected."));
                    }
                }
                ui.add_space(12.0);
                if let Some(path) = choose_file(
                    ui,
                    t("title.keys (opcional)…", "title.keys (optional)…"),
                    false,
                ) {
                    self.title_keys = Some(path);
                }
                if let Some(path) = &self.title_keys {
                    ui.label(path.display().to_string());
                }
            }
            Page::Firmware => {
                heading(
                    ui,
                    t(
                        "Firmware do Switch (opcional)",
                        "Switch firmware (optional)",
                    ),
                    t(
                        "Sem firmware, os jogos funcionam normalmente: o NXbox substitui os serviços do sistema. Com o firmware do seu console, recursos do sistema passam a funcionar como no Switch original, como o editor de Mii e o menu HOME. Aceita a pasta de firmware, um arquivo .zip ou um backup da NAND.",
                        "Without firmware, games work normally: NXbox replaces the system services. With your console's firmware, system features behave as on the original Switch, such as the Mii editor and the HOME menu. Accepts the firmware folder, a .zip file or a NAND backup.",
                    ),
                );
                ui.horizontal(|ui| {
                    if let Some(p) = choose_file(ui, t("Escolher pasta…", "Choose folder…"), true)
                    {
                        self.firmware = Some(p);
                    }
                    if let Some(p) = choose_file(ui, t("Escolher arquivo…", "Choose file…"), false)
                    {
                        self.firmware = Some(p);
                    }
                    if self.firmware.is_some() && ui.button(t("Remover", "Remove")).clicked() {
                        self.firmware = None;
                    }
                });
                if design::drop_zone(ui, "firmware-drop", self.firmware.is_some())
                    && let Some(p) = rfd::FileDialog::new().pick_file()
                {
                    self.firmware = Some(p);
                }
                ui.label(match &self.firmware {
                    Some(f) => f.display().to_string(),
                    None => t("Nenhum firmware selecionado.", "No firmware selected.").into(),
                });
                ui.add_space(14.0);
                if self.mode == Some(Mode::Full) {
                    ui.add_enabled_ui(self.firmware.is_some(), |ui| {
                    ui.checkbox(&mut self.official_ui, t("Usar a interface oficial do Switch", "Use the official Switch interface"));
                    ui.indent("ui", |ui| {
                        ui.label(t(
                            "Ao abrir o NXbox, o menu HOME do Switch aparece no lugar da biblioteca. As opções do NXbox continuam disponíveis pressionando Start + Select duas vezes. Pode ser alterado depois nas configurações do NXbox.",
                            "When NXbox opens, the Switch HOME menu is shown instead of the library. NXbox options remain available by pressing Start + Select twice. This can be changed later in the NXbox settings.",
                        ));
                    });
                });
                    if self.firmware.is_none() {
                        ui.small(t("Requer firmware.", "Requires firmware."));
                    }
                }
            }
            Page::Games => {
                heading(
                    ui,
                    t("Jogos (opcional)", "Games (optional)"),
                    t(
                        "Adicione os jogos que deseja copiar para a biblioteca agora. Arraste vários arquivos de uma vez ou clique em Adicionar. Também é possível adicioná-los depois, pelo próprio NXbox no Xbox.",
                        "Add the games you want to copy to the library now. Drag several files at once or click Add. You can also add them later from NXbox on the Xbox.",
                    ),
                );
                if design::drop_zone(ui, "games-drop", !self.games.is_empty())
                    && let Some(paths) = rfd::FileDialog::new().pick_files()
                {
                    for path in paths {
                        self.add_game(path);
                    }
                }
                ui.add_space(8.0);
                let mut remove = None;
                for (i, game) in self.games.clone().iter().enumerate() {
                    ui.vertical(|ui| {
                        self.game_card(ui, game);
                        if ui.small_button(t("Remover", "Remove")).clicked() {
                            remove = Some(i);
                        }
                    });
                }
                if let Some(i) = remove {
                    self.games.remove(i);
                }
                if self.games.is_empty() {
                    ui.label(t(
                        "Nenhum jogo adicionado. A biblioteca começará vazia.",
                        "No games added. The library will start empty.",
                    ));
                }
            }
            Page::Extras => {
                heading(
                    ui,
                    t(
                        "Conteúdo adicional (opcional)",
                        "Additional content (optional)",
                    ),
                    t(
                        "Itens que acompanham o jogo. Todos são opcionais; pode avançar sem nenhum deles.",
                        "Items that come with the game. All are optional; you can continue without any of them.",
                    ),
                );
                for (label, help, folder, slot) in [
                    (
                        t("Firmware", "Firmware"),
                        t(
                            "Necessário apenas para jogos que usam recursos do sistema, como alguns títulos online locais.",
                            "Only needed by games that use system features.",
                        ),
                        true,
                        0,
                    ),
                    (
                        t("Saves", "Saves"),
                        t(
                            "Pasta de saves exportada do seu Switch, por exemplo com o JKSV.",
                            "Save folder exported from your Switch, for example with JKSV.",
                        ),
                        true,
                        1,
                    ),
                ] {
                    ui.strong(label);
                    ui.small(help);
                    let current = if slot == 0 {
                        &self.firmware
                    } else {
                        &self.saves
                    };
                    ui.label(
                        current
                            .as_ref()
                            .map(|p| p.display().to_string())
                            .unwrap_or_else(|| t("Nada selecionado.", "Nothing selected.").into()),
                    );
                    if let Some(p) = choose_file(ui, t("Escolher…", "Choose…"), folder) {
                        if slot == 0 {
                            self.firmware = Some(p)
                        } else {
                            self.saves = Some(p)
                        }
                    }
                    ui.add_space(10.0);
                }
                ui.strong("Mods");
                ui.small(t(
                    "Pastas no formato LayeredFS (com romfs ou exefs dentro).",
                    "Folders in LayeredFS layout (with romfs or exefs inside).",
                ));
                for m in &self.mods {
                    ui.label(m.display().to_string());
                }
                if let Some(p) = choose_file(ui, t("Adicionar mod…", "Add mod…"), true) {
                    self.mods.push(p);
                }
            }
            Page::Install => {
                if self.mode == Some(Mode::Drive) {
                    heading(
                        ui,
                        t("Preparar pendrive", "Prepare USB drive"),
                        t(
                            "Escolha a raiz do pendrive. Os arquivos serão conferidos antes da cópia.",
                            "Choose the USB drive root. Files will be validated before copying.",
                        ),
                    );
                    if let Some(path) =
                        choose_file(ui, t("Escolher pendrive…", "Choose USB drive…"), true)
                    {
                        self.drive = Some(path);
                    }
                    if let Some(path) = &self.drive {
                        ui.label(path.display().to_string());
                    }
                    return;
                }
                if self.mode == Some(Mode::Xbox) {
                    self.manual = false;
                    ui.label(t(
                        "O SSD interno enche rápido; recomendamos um pendrive.",
                        "The internal SSD fills fast; a USB drive is recommended.",
                    ));
                }
                heading(
                    ui,
                    t("Instalação", "Installation"),
                    t(
                        "Escolha como o NXbox chegará ao seu Xbox.",
                        "Choose how NXbox will reach your Xbox.",
                    ),
                );
                if self.mode != Some(Mode::Xbox) {
                    ui.radio_value(
                        &mut self.manual,
                        false,
                        t(
                            "Instalar automaticamente no Xbox",
                            "Install automatically on the Xbox",
                        ),
                    );
                    ui.indent("auto", |ui| {
                    ui.label(t(
                        "O NxbHelper se conecta ao Device Portal do Xbox pela rede local e instala tudo sozinho. O Xbox e este computador precisam estar na mesma rede.",
                        "NxbHelper connects to the Xbox Device Portal over the local network and installs everything by itself. The Xbox and this computer must be on the same network.",
                    ));
                });
                    ui.radio_value(
                        &mut self.manual,
                        true,
                        t(
                            "Prefiro instalar manualmente",
                            "I prefer to install manually",
                        ),
                    );
                    ui.indent("manual", |ui| {
                    ui.label(t(
                        "Gera a pasta do aplicativo (UWP) com todo o conteúdo já dentro. Depois, registre-a você mesmo pelo Device Portal do Xbox.",
                        "Builds the app (UWP) folder with all content already inside. You then register it yourself through the Xbox Device Portal.",
                    ));
                });
                }
                ui.add_space(14.0);
                if self.manual {
                    if let Some(p) = choose_file(
                        ui,
                        t("Escolher onde salvar…", "Choose where to save…"),
                        true,
                    ) {
                        self.out = Some(p);
                    }
                    ui.label(
                        self.out
                            .as_ref()
                            .map(|p| p.display().to_string())
                            .unwrap_or_else(|| {
                                t("Nenhuma pasta escolhida.", "No folder chosen.").into()
                            }),
                    );
                } else {
                    egui::Grid::new("portal")
                        .num_columns(2)
                        .spacing([12.0, 8.0])
                        .show(ui, |ui| {
                            design::label(ui, t("Endereço IP do Xbox", "Xbox IP address"));
                            ui.scope(|ui| {
                                ui.visuals_mut().widgets.inactive.corner_radius = 16.into();
                                ui.add_sized(
                                    [220.0, 44.0],
                                    egui::TextEdit::singleline(&mut self.host),
                                );
                            });
                            ui.end_row();
                            design::label(ui, t("Usuário", "User name"));
                            ui.scope(|ui| {
                                ui.visuals_mut().widgets.inactive.corner_radius = 16.into();
                                ui.add_sized(
                                    [220.0, 44.0],
                                    egui::TextEdit::singleline(&mut self.user),
                                );
                            });
                            ui.end_row();
                            design::label(ui, t("Senha", "Password"));
                            ui.scope(|ui| {
                                ui.visuals_mut().widgets.inactive.corner_radius = 16.into();
                                ui.add_sized(
                                    [220.0, 44.0],
                                    egui::TextEdit::singleline(&mut self.password).password(true),
                                );
                            });
                            ui.end_row();
                        });
                    ui.checkbox(
                        &mut self.remember,
                        t(
                            "Lembrar o usuário e a senha neste computador",
                            "Remember the user name and password on this computer",
                        ),
                    );
                    ui.small(t(
                        "O endereço aparece sozinho quando o Xbox é encontrado na rede; também é mostrado em Dev Home. Usuário e senha são os definidos em Dev Home > Remote Access; deixe em branco se o Device Portal estiver sem senha ou se já foram salvos antes.",
                        "The address fills in when the Xbox is found on the network; it is also shown in Dev Home. User name and password are the ones set in Dev Home > Remote Access; leave them empty if Device Portal has no password or they were saved before.",
                    ));
                }
            }
            Page::Progress => {
                for warning in &self.warnings {
                    ui.label(warning);
                }
                if self.report.is_some() {
                    if let Some(error) = &self.error {
                        ui.label(error);
                    }
                    self.show_report(ui);
                    return;
                }
                if let Some(e) = self.error.clone() {
                    if matches!(self.mode, Some(Mode::Drive | Mode::Xbox)) {
                        heading(
                            ui,
                            t("O envio não foi concluído", "The transfer did not finish"),
                            t(
                                "Confira o problema abaixo e tente novamente. Os arquivos já copiados foram mantidos.",
                                "Check the problem below and try again. Files already copied were kept.",
                            ),
                        );
                        ui.label(e);
                        return;
                    }
                    heading(
                        ui,
                        t(
                            "A instalação não foi concluída",
                            "The installation did not finish",
                        ),
                        t(
                            "Nada foi alterado no Xbox além do que já tinha sido enviado. Volte, confira os dados e tente novamente.",
                            "Nothing was changed on the Xbox beyond what had already been sent. Go back, check the details and try again.",
                        ),
                    );
                    ui.monospace(e);
                } else if self.mode == Some(Mode::Xbox) && self.done.is_some() {
                    heading(
                        ui,
                        t("Arquivos enviados", "Files sent"),
                        t(
                            "Abra o NXbox no Xbox para importar o firmware e jogar.",
                            "Open NXbox on Xbox to import firmware and play.",
                        ),
                    );
                } else if let Some(d) = self.done.clone() {
                    let (title, body) = if self.manual {
                        (
                            t("Pasta gerada", "Folder created"),
                            t(
                                "A pasta do aplicativo está pronta para ser registrada pelo Device Portal:",
                                "The app folder is ready to be registered through Device Portal:",
                            ),
                        )
                    } else {
                        (
                            t("Instalação concluída", "Installation complete"),
                            t(
                                "O aplicativo já aparece em Jogos e aplicativos no Xbox:",
                                "The app now appears in Games & apps on the Xbox:",
                            ),
                        )
                    };
                    design::check(ui, 48.0);
                    heading(ui, title, body);
                    ui.monospace(d);
                } else {
                    heading(
                        ui,
                        t("Instalando", "Installing"),
                        t(
                            "Mantenha esta janela aberta até o fim.",
                            "Keep this window open until it finishes.",
                        ),
                    );
                    ui.label(&self.status);
                    self.progress_bar(ui);
                }
            }
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.take_drops(&ctx);
        if self.page() == Page::Install
            && self.mode != Some(Mode::Drive)
            && !self.manual
            && !self.discovery_started
        {
            self.discovery_started = true;
            self.discover_rx = Some(core(json!({ "cmd": "discover" })));
        }
        self.poll(&ctx);
        if !self.inspecting.is_empty() || self.run_rx.is_some() || self.discover_rx.is_some() {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }

        self.render(ui);
    }
}

#[cfg(test)]
mod screenshots {
    use super::*;
    use egui_kittest::Harness;

    // Synthetic UI state only: no backend, credentials, keys, game files or network access.
    #[test]
    #[ignore = "writes headless page previews to /tmp/t28_shots"]
    fn render_all_pages() {
        let output = std::env::var("NXBOX_PACK_SHOTS").unwrap_or_else(|_| "/tmp/t28_shots".into());
        std::fs::create_dir_all(&output).unwrap();
        for theme in [egui::Theme::Dark, egui::Theme::Light] {
            for page in [
                Page::Welcome,
                Page::Check,
                Page::Game,
                Page::Keys,
                Page::Firmware,
                Page::Games,
                Page::Extras,
                Page::Install,
                Page::Progress,
            ] {
                let mode = if page == Page::Check {
                    Mode::Check
                } else if matches!(page, Page::Firmware | Page::Games) {
                    Mode::Full
                } else {
                    Mode::Game
                };
                let mut app = App {
                    mode: Some(mode),
                    ..App::default()
                };
                app.page_index = flow(app.mode).iter().position(|p| *p == page).unwrap();
                if page == Page::Progress {
                    app.progress = Some(0.62);
                    app.status = t("Enviando ao Xbox", "Sending to the Xbox").into();
                }
                let mut harness = Harness::builder()
                    .with_theme(theme)
                    .with_size(egui::vec2(640.0, 800.0))
                    .build_ui_state(
                        |ui, app: &mut App| {
                            ui.ctx().set_theme(theme);
                            design::configure(ui.ctx());
                            if ui.ctx().cumulative_pass_nr() == 0 {
                                design::configure_fonts(ui.ctx());
                            }
                            app.render(ui);
                        },
                        app,
                    );
                harness.run_steps(30);
                let name = format!("{output}/{theme:?}-{page:?}.png").to_lowercase();
                harness.render().unwrap().save(name).unwrap();
            }
            for (name, page, done) in [
                ("game-selected", Page::Game, false),
                ("keys-selected", Page::Keys, false),
                ("success", Page::Progress, true),
                ("drive-report", Page::Progress, true),
                ("drive-fixable", Page::Progress, true),
                ("usb-destination", Page::Install, false),
                ("xbox-destination", Page::Install, false),
            ] {
                let path = PathBuf::from("Example.nsp");
                let mut app = App {
                    mode: Some(Mode::Game),
                    keys: Some(PathBuf::from("prod.keys")),
                    ..App::default()
                };
                app.page_index = flow(app.mode).iter().position(|p| *p == page).unwrap();
                app.games.push(path.clone());
                app.info.insert(path, json!({"name":"Example adventure", "titleId":"0100123456780000", "version":"2.4.1", "publisher":"Example studio", "size":2345678901_u64,"compat":{"status":"playable"}}));
                if done {
                    app.done = Some("Example adventure".into());
                }
                if name.starts_with("drive-") {
                    app.mode = Some(Mode::Drive);
                    app.page_index = flow(app.mode).len() - 1;
                    let ready = name == "drive-report";
                    app.report = Some(json!({ "root": "/example/USB", "ready": ready, "items": [
                        { "id": "prodKeys", "status": "ok", "title": t("Chaves do console", "Console keys"), "detail": "prod.keys" },
                        { "id": "titleKeys", "status": "warn", "title": t("Chaves de títulos", "Title keys"), "fix": t("Opcional; necessário apenas para alguns jogos.", "Optional; only needed for some games.") },
                        { "id": "firmware", "status": if ready { "ok" } else { "fail" }, "title": "Firmware", "detail": if ready { "20.0.0 — 230 files" } else { "firmware.zip" }, "fix": if ready { "" } else { t("NxbHelper pode descompactar. Escolha Consertar.", "NxbHelper can unpack it. Choose Fix it.") } },
                        { "id": "games", "status": "ok", "title": t("2 jogos encontrados", "2 games found") }
                    ] }));
                }
                if name.ends_with("destination") {
                    app.mode = Some(if name.starts_with("usb") {
                        Mode::Drive
                    } else {
                        Mode::Xbox
                    });
                    app.page_index = flow(app.mode)
                        .iter()
                        .position(|p| *p == Page::Install)
                        .unwrap();
                }
                let mut harness = Harness::builder()
                    .with_theme(theme)
                    .with_size(egui::vec2(560.0, 560.0))
                    .build_ui_state(
                        |ui, app: &mut App| {
                            ui.ctx().set_theme(theme);
                            design::configure(ui.ctx());
                            if ui.ctx().cumulative_pass_nr() == 0 {
                                design::configure_fonts(ui.ctx());
                            }
                            if app.icons.is_empty()
                                && let Some(path) = app.games.first()
                            {
                                let icon = egui::ColorImage::filled([32, 32], design::ACCENT);
                                app.icons.insert(
                                    path.clone(),
                                    ui.ctx().load_texture(
                                        "synthetic-icon",
                                        icon,
                                        egui::TextureOptions::LINEAR,
                                    ),
                                );
                            }
                            app.render(ui);
                        },
                        app,
                    );
                harness.run_steps(30);
                harness
                    .render()
                    .unwrap()
                    .save(format!("{output}/{theme:?}-{name}.png").to_lowercase())
                    .unwrap();
            }
        }
    }
}
