use eframe::egui::{self, Color32, CornerRadius, FontId, Stroke, TextStyle};

pub const ACCENT: Color32 = Color32::from_rgb(61, 125, 255);
pub const GOOD: Color32 = Color32::from_rgb(78, 155, 108);
pub const WARN: Color32 = Color32::from_rgb(170, 125, 46);
pub const BAD: Color32 = Color32::from_rgb(205, 115, 117);
pub const UNKNOWN: Color32 = Color32::from_rgb(125, 125, 125);
pub const BORDER: Color32 = Color32::from_rgba_premultiplied(15, 15, 15, 15);
pub const INK: Color32 = Color32::from_rgb(12, 12, 12);
pub const LIGHT_BG: Color32 = Color32::from_rgb(242, 242, 244);
pub const DARK_CARD: Color32 = Color32::from_rgb(16, 16, 18);
pub const SECONDARY_DARK: Color32 = Color32::from_gray(180);
pub const SECONDARY_LIGHT: Color32 = Color32::from_gray(90);

pub fn reduced_motion() -> bool {
    std::env::var("NXBOX_REDUCED_MOTION").as_deref() == Ok("1")
}
pub fn motion(ctx: &egui::Context, id: egui::Id, target: bool, seconds: f32) -> f32 {
    if reduced_motion() {
        return if target { 1.0 } else { 0.0 };
    }
    ctx.animate_bool_with_time_and_easing(id, target, seconds, egui::emath::easing::cubic_out)
}
pub fn background(ui: &egui::Ui) -> Color32 {
    if ui.visuals().dark_mode {
        Color32::BLACK
    } else {
        LIGHT_BG
    }
}
pub fn surface(ui: &egui::Ui) -> Color32 {
    if ui.visuals().dark_mode {
        DARK_CARD
    } else {
        Color32::WHITE
    }
}
pub fn secondary(ui: &egui::Ui) -> Color32 {
    if ui.visuals().dark_mode {
        SECONDARY_DARK
    } else {
        SECONDARY_LIGHT
    }
}
pub fn configure(ctx: &egui::Context) {
    for theme in [egui::Theme::Dark, egui::Theme::Light] {
        ctx.style_mut_of(theme, |style| {
            style
                .text_styles
                .insert(TextStyle::Heading, FontId::proportional(32.0));
            style
                .text_styles
                .insert(TextStyle::Body, FontId::proportional(17.0));
            style
                .text_styles
                .insert(TextStyle::Button, FontId::proportional(17.0));
            style
                .text_styles
                .insert(TextStyle::Small, FontId::proportional(15.0));
            style
                .text_styles
                .insert(TextStyle::Monospace, FontId::monospace(15.0));
            style.spacing.item_spacing = egui::vec2(10.0, 12.0);
            style.spacing.button_padding = egui::vec2(20.0, 12.0);
            style.spacing.interact_size.y = 44.0;
            style.animation_time = if reduced_motion() { 0.0 } else { 0.15 };
            let v = &mut style.visuals;
            v.window_shadow = egui::epaint::Shadow::NONE;
            v.popup_shadow = egui::epaint::Shadow::NONE;
            v.panel_fill = if theme == egui::Theme::Dark {
                Color32::BLACK
            } else {
                LIGHT_BG
            };
            v.window_fill = if theme == egui::Theme::Dark {
                DARK_CARD
            } else {
                Color32::WHITE
            };
            v.override_text_color = Some(if theme == egui::Theme::Dark {
                Color32::from_gray(238)
            } else {
                INK
            });
            v.widgets.noninteractive.bg_stroke = Stroke::NONE;
            v.selection.bg_fill = ACCENT;
            v.selection.stroke = Stroke::new(1.0, ACCENT);
            v.widgets.inactive.corner_radius = CornerRadius::same(255);
            v.widgets.hovered.corner_radius = CornerRadius::same(255);
            v.widgets.active.corner_radius = CornerRadius::same(255);
            v.widgets.inactive.bg_stroke = Stroke::new(1.0, BORDER);
            v.widgets.hovered.bg_stroke = Stroke::new(1.0, ACCENT);
            v.widgets.active.bg_stroke = Stroke::new(1.0, ACCENT);
        });
    }
}
pub fn primary(text: &str) -> egui::Button<'_> {
    egui::Button::new(egui::RichText::new(text).strong().color(INK))
        .fill(ACCENT)
        .stroke(Stroke::NONE)
        .corner_radius(255)
        .min_size(egui::vec2(120.0, 48.0))
}
pub fn centered(ui: &mut egui::Ui, body: impl FnOnce(&mut egui::Ui)) {
    let mut rect = ui.available_rect_before_wrap();
    let width = rect.width().min(560.0);
    rect.min.x += ((rect.width() - width) / 2.0).max(0.0);
    rect.max.x = rect.min.x + width;
    rect.max.y = rect.min.y;
    ui.scope_builder(
        egui::UiBuilder::new()
            .max_rect(rect)
            .layout(egui::Layout::top_down(egui::Align::Min)),
        body,
    );
}
pub fn drop_zone(ui: &mut egui::Ui, id: &str, selected: bool) -> bool {
    let amount = motion(ui.ctx(), ui.id().with(id), selected, 0.22);
    let height = egui::lerp(172.0..=48.0, amount);
    let (rect, response) = ui.allocate_exact_size(
        egui::vec2(ui.available_width(), height),
        egui::Sense::click(),
    );
    let radius = egui::lerp(16.0..=24.0, amount);
    ui.painter().rect_filled(rect, radius, surface(ui));
    let color = if response.hovered() {
        ACCENT
    } else {
        secondary(ui)
    };
    // Rounded dashed perimeter, sampled as a closed path to keep every side clickable.
    let mut points = Vec::new();
    for (center, start) in [
        (
            rect.left_top() + egui::vec2(radius, radius),
            std::f32::consts::PI,
        ),
        (
            rect.right_top() + egui::vec2(-radius, radius),
            1.5 * std::f32::consts::PI,
        ),
        (rect.right_bottom() + egui::vec2(-radius, -radius), 0.0),
        (
            rect.left_bottom() + egui::vec2(radius, -radius),
            0.5 * std::f32::consts::PI,
        ),
    ] {
        for n in 0..=12 {
            let angle = start + n as f32 / 12.0 * std::f32::consts::FRAC_PI_2;
            points.push(center + radius * egui::vec2(angle.cos(), angle.sin()));
        }
    }
    points.push(points[0]);
    ui.painter().extend(egui::Shape::dashed_line(
        &points,
        Stroke::new(1.0, color),
        6.0,
        5.0,
    ));
    ui.painter().text(
        rect.center(),
        egui::Align2::CENTER_CENTER,
        super::t(
            "Arraste aqui ou clique para escolher",
            "Drag here or click to choose",
        ),
        FontId::proportional(17.0),
        color,
    );
    response.clicked()
}

pub fn heading_font(ctx: &egui::Context, size: f32) -> FontId {
    let available = ctx.fonts(|fonts| {
        fonts
            .families()
            .contains(&egui::FontFamily::Name("system-bold".into()))
    });
    if available {
        FontId::new(size, egui::FontFamily::Name("system-bold".into()))
    } else {
        FontId::proportional(size)
    }
}
pub fn configure_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    for path in [
        "C:/Windows/Fonts/segoeuib.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ] {
        if let Ok(bytes) = std::fs::read(path) {
            fonts.font_data.insert(
                "system-bold".into(),
                egui::FontData::from_owned(bytes).into(),
            );
            fonts.families.insert(
                egui::FontFamily::Name("system-bold".into()),
                vec!["system-bold".into()],
            );
            ctx.data_mut(|data| data.insert_temp(egui::Id::new("system-bold"), true));
            break;
        }
    }
    ctx.set_fonts(fonts);
}
pub fn label(ui: &mut egui::Ui, text: &str) {
    let mut job = egui::text::LayoutJob::default();
    job.append(
        &text.to_uppercase(),
        0.0,
        egui::TextFormat {
            font_id: heading_font(ui.ctx(), 12.0),
            color: secondary(ui),
            extra_letter_spacing: 1.2,
            ..Default::default()
        },
    );
    ui.label(job);
}
pub fn file_chip(ui: &mut egui::Ui, id: &str, name: &str) -> bool {
    let amount = motion(ui.ctx(), ui.id().with(id), true, 0.22);
    egui::Frame::new()
        .fill(surface(ui))
        .stroke(Stroke::new(1.0, BORDER))
        .corner_radius(egui::lerp(16.0..=24.0, amount) as u8)
        .inner_margin(16)
        .show(ui, |ui| {
            ui.set_min_size(egui::vec2(
                ui.available_width(),
                egui::lerp(140.0..=24.0, amount),
            ));
            ui.label(name);
        })
        .response
        .interact(egui::Sense::click())
        .clicked()
}

pub fn compat_ink(color: Color32, dark: bool) -> Color32 {
    if dark {
        color
    } else {
        Color32::from_rgb(
            (color.r() as f32 * 0.72) as u8,
            (color.g() as f32 * 0.72) as u8,
            (color.b() as f32 * 0.72) as u8,
        )
    }
}
pub fn check(ui: &mut egui::Ui, size: f32) {
    let (rect, _) = ui.allocate_exact_size(egui::vec2(size, size), egui::Sense::hover());
    ui.painter()
        .circle_stroke(rect.center(), size * 0.43, Stroke::new(2.0, ACCENT));
    let point = |x, y| rect.min + egui::vec2(size * x, size * y);
    ui.painter().add(egui::Shape::line(
        vec![point(0.27, 0.50), point(0.44, 0.66), point(0.73, 0.34)],
        Stroke::new(3.0, ACCENT),
    ));
}
