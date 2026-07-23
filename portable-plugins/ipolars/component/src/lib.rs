wit_bindgen::generate!({
    path: "../../../portable-runtime/wit",
    world: "plugin-world",
});

use opencpn::portable::host::{self, LogLevel};
use std::fmt::Write as _;
use std::sync::Mutex;

const SURFACE: &str = "polars.editor";

#[derive(Clone)]
struct Polar {
    name: String,
    tws: Vec<f64>,
    twa: Vec<f64>,
    speeds: Vec<Vec<Option<f64>>>,
}

#[derive(Clone)]
struct BoatPolar {
    file: String,
    crossover: f64,
}

#[derive(Clone)]
enum Document {
    Polar(Polar),
    Boat {
        name: String,
        polars: Vec<BoatPolar>,
    },
}

struct Editor {
    document: Option<Document>,
    status: String,
    diagnostics: String,
    new_angle: String,
    new_wind: String,
    selected: (usize, usize),
    dirty: bool,
    capturing: bool,
    last_twa: Option<f64>,
    last_tws: Option<f64>,
    last_stw: Option<f64>,
    sample_counts: Vec<Vec<u32>>,
    samples: u64,
}

static EDITOR: Mutex<Editor> = Mutex::new(Editor {
    document: None,
    status: String::new(),
    diagnostics: String::new(),
    new_angle: String::new(),
    new_wind: String::new(),
    selected: (0, 0),
    dirty: false,
    capturing: false,
    last_twa: None,
    last_tws: None,
    last_stw: None,
    sample_counts: Vec::new(),
    samples: 0,
});

fn default_polar() -> Polar {
    let tws = vec![6.0, 10.0, 14.0, 20.0];
    let twa: Vec<f64> = vec![
        40.0, 52.0, 60.0, 75.0, 90.0, 110.0, 135.0, 150.0, 165.0, 180.0,
    ];
    let speeds = twa
        .iter()
        .map(|angle| {
            tws.iter()
                .map(|wind| Some((wind * (angle.to_radians().sin().abs() * 0.58 + 0.28)).min(8.2)))
                .collect()
        })
        .collect();
    Polar {
        name: "New cruising polar".into(),
        tws,
        twa,
        speeds,
    }
}

fn capture_polar() -> Polar {
    let tws = (1..=20)
        .map(|value| f64::from(value * 2))
        .collect::<Vec<_>>();
    let twa = (0..=36)
        .map(|value| f64::from(value * 5))
        .collect::<Vec<_>>();
    let speeds = vec![vec![None; tws.len()]; twa.len()];
    Polar {
        name: "Measured 5° TWA / 2 kn TWS polar".into(),
        tws,
        twa,
        speeds,
    }
}

fn fields(line: &str) -> Vec<&str> {
    if line.contains('\t') {
        line.split('\t').map(str::trim).collect()
    } else if line.contains(';') {
        line.split(';').map(str::trim).collect()
    } else if line.contains(',') {
        line.split(',').map(str::trim).collect()
    } else {
        line.split_whitespace().collect()
    }
}

fn number(value: &str, label: &str) -> Result<f64, String> {
    let value = value
        .trim()
        .parse::<f64>()
        .map_err(|_| format!("{label} is not a number: {value}"))?;
    if value.is_finite() {
        Ok(value)
    } else {
        Err(format!("{label} is not finite"))
    }
}

fn validate(p: &Polar) -> Result<String, String> {
    if !(2..=80).contains(&p.tws.len()) || !(2..=181).contains(&p.twa.len()) {
        return Err("A polar requires 2–80 TWS columns and 2–181 TWA rows.".into());
    }
    if !p.tws.windows(2).all(|v| v[0] < v[1]) || p.tws.iter().any(|v| *v <= 0.0 || *v > 200.0) {
        return Err("TWS values must increase strictly and lie in (0, 200] kn.".into());
    }
    if !p.twa.windows(2).all(|v| v[0] < v[1]) || p.twa.iter().any(|v| *v < 0.0 || *v > 180.0) {
        return Err("TWA values must increase strictly and lie in [0, 180]°.".into());
    }
    if p.speeds.len() != p.twa.len() || p.speeds.iter().any(|r| r.len() != p.tws.len()) {
        return Err("Polar matrix dimensions do not match its axes.".into());
    }
    let blanks = p.speeds.iter().flatten().filter(|v| v.is_none()).count();
    let zeros = p
        .speeds
        .iter()
        .flatten()
        .filter(|v| matches!(v, Some(x) if *x == 0.0))
        .count();
    Ok(format!(
        "{} angles × {} wind speeds; {blanks} blank and {zeros} zero unavailable cells.",
        p.twa.len(),
        p.tws.len()
    ))
}

fn parse_polar(text: &str) -> Result<Polar, String> {
    let mut lines: Vec<&str> = text
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .collect();
    if lines.is_empty() {
        return Err("The polar file is empty.".into());
    }
    let mut name = "Imported polar".to_string();
    if !fields(lines[0])
        .first()
        .is_some_and(|v| v.eq_ignore_ascii_case("twa/tws") || v.eq_ignore_ascii_case("twa"))
    {
        name = lines.remove(0).to_string();
    }
    if lines.is_empty() {
        return Err("The TWA/TWS header is missing.".into());
    }
    let header = fields(lines.remove(0));
    if header.len() < 3
        || !(header[0].eq_ignore_ascii_case("twa/tws") || header[0].eq_ignore_ascii_case("twa"))
    {
        return Err("Expected TWA/TWS followed by wind-speed columns.".into());
    }
    let tws = header[1..]
        .iter()
        .map(|v| number(v, "TWS"))
        .collect::<Result<Vec<_>, _>>()?;
    let mut twa = Vec::new();
    let mut speeds = Vec::new();
    for (index, line) in lines.iter().enumerate() {
        let row = fields(line);
        if row.is_empty() || row.len() > tws.len() + 1 {
            return Err(format!("Invalid row {}.", index + 2));
        }
        twa.push(number(row[0], "TWA")?);
        let mut values = Vec::new();
        for column in 0..tws.len() {
            let cell = row.get(column + 1).copied().unwrap_or("");
            values.push(if cell.is_empty() {
                None
            } else {
                let speed = number(cell, "boat speed")?;
                if !(0.0..=100.0).contains(&speed) {
                    return Err("Boat speed must lie in [0, 100] kn.".into());
                }
                Some(speed)
            });
        }
        speeds.push(values);
    }
    let polar = Polar {
        name,
        tws,
        twa,
        speeds,
    };
    validate(&polar)?;
    Ok(polar)
}

fn attr(tag: &str, name: &str) -> Option<String> {
    let needle = format!("{name}=\"");
    let tail = &tag[tag.find(&needle)? + needle.len()..];
    Some(
        tail[..tail.find('"')?]
            .replace("&quot;", "\"")
            .replace("&amp;", "&"),
    )
}

fn parse_boat(text: &str) -> Result<Document, String> {
    let root_at = text
        .find("<OpenCPNWeatherRoutingBoat")
        .ok_or("XML is not an OpenCPN Weather Routing boat document.")?;
    let root =
        &text[root_at..=root_at + text[root_at..].find('>').ok_or("Incomplete boat root.")?];
    let name = attr(root, "Name").unwrap_or_else(|| "OpenCPN boat".into());
    let mut polars = Vec::new();
    let mut rest = text;
    while let Some(at) = rest.find("<Polar ") {
        rest = &rest[at..];
        let end = rest.find('>').ok_or("Incomplete Polar element.")?;
        let tag = &rest[..=end];
        if let Some(file) = attr(tag, "FileName") {
            polars.push(BoatPolar {
                file,
                crossover: attr(tag, "CrossOverPercentage")
                    .and_then(|v| v.parse().ok())
                    .unwrap_or(0.0),
            });
        }
        rest = &rest[end + 1..];
    }
    if polars.is_empty() {
        return Err("Boat XML contains no Polar references.".into());
    }
    Ok(Document::Boat { name, polars })
}

fn fmt(value: f64) -> String {
    let mut out = format!("{value:.3}");
    while out.contains('.') && out.ends_with('0') {
        out.pop();
    }
    if out.ends_with('.') {
        out.pop();
    }
    out
}

fn serialize(document: &Document) -> Result<String, String> {
    match document {
        Document::Polar(p) => {
            validate(p)?;
            let mut out = format!("{}\nTWA/TWS", p.name);
            for v in &p.tws {
                let _ = write!(out, "\t{}", fmt(*v));
            }
            out.push('\n');
            for (r, angle) in p.twa.iter().enumerate() {
                out.push_str(&fmt(*angle));
                for speed in &p.speeds[r] {
                    out.push('\t');
                    if let Some(v) = speed {
                        out.push_str(&fmt(*v));
                    }
                }
                out.push('\n');
            }
            Ok(out)
        }
        Document::Boat { name, polars } => {
            let escape = |v: &str| {
                v.replace('&', "&amp;")
                    .replace('"', "&quot;")
                    .replace('<', "&lt;")
                    .replace('>', "&gt;")
            };
            let mut out = format!(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<OpenCPNWeatherRoutingBoat version=\"1.10\" Name=\"{}\">\n",
                escape(name)
            );
            for p in polars {
                let _ = writeln!(
                    out,
                    "  <Polar FileName=\"{}\" CrossOverPercentage=\"{}\"/>",
                    escape(&p.file),
                    fmt(p.crossover)
                );
            }
            out.push_str("</OpenCPNWeatherRoutingBoat>\n");
            Ok(out)
        }
    }
}

fn json(value: &str) -> String {
    let mut out = "\"".to_string();
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c.is_control() => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn json_string(value: &str) -> Result<String, String> {
    let value = value.trim();
    if value.len() < 2 || !value.starts_with('"') || !value.ends_with('"') {
        return Err("Expected an opaque file grant.".into());
    }
    let mut out = String::new();
    let mut escaped = false;
    for c in value[1..value.len() - 1].chars() {
        if escaped {
            out.push(match c {
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                c => c,
            });
            escaped = false;
        } else if c == '\\' {
            escaped = true;
        } else {
            out.push(c);
        }
    }
    if escaped {
        Err("Incomplete JSON escape.".into())
    } else {
        Ok(out)
    }
}

fn integer(value: &str, name: &str) -> Result<usize, String> {
    let needle = format!("\"{name}\":");
    let start = value
        .find(&needle)
        .ok_or_else(|| format!("Missing {name}."))?
        + needle.len();
    value[start..]
        .trim_start()
        .chars()
        .take_while(char::is_ascii_digit)
        .collect::<String>()
        .parse()
        .map_err(|_| format!("Invalid {name}."))
}

fn string_field(value: &str, name: &str) -> Result<String, String> {
    let needle = format!("\"{name}\":");
    let start = value
        .find(&needle)
        .ok_or_else(|| format!("Missing {name}."))?
        + needle.len();
    json_string(value[start..].trim_end_matches('}').trim())
}

fn response(editor: &Editor) -> String {
    let mut controls = String::new();
    match editor.document.as_ref() {
        Some(Document::Polar(p)) => {
            controls.push_str("\"document-type\":\"Polar performance table\",");
            controls.push_str("\"polar-grid\":{\"columns\":[\"TWA / TWS\"");
            for wind in &p.tws { controls.push(','); controls.push_str(&json(&format!("{} kn", fmt(*wind)))); }
            controls.push_str("],\"rows\":[");
            for (r, angle) in p.twa.iter().enumerate() {
                if r > 0 { controls.push(','); } controls.push('['); controls.push_str(&json(&fmt(*angle)));
                for speed in &p.speeds[r] { controls.push(','); controls.push_str(&json(&speed.map(fmt).unwrap_or_default())); }
                controls.push(']');
            }
            controls.push_str("]},\"boat-grid\":{\"columns\":[\"Polar file\",\"Crossover %\"],\"rows\":[]},");
        }
        Some(Document::Boat { polars, .. }) => {
            controls.push_str("\"document-type\":\"OpenCPN Weather Routing boat XML\",\"polar-grid\":{\"columns\":[\"TWA / TWS\"],\"rows\":[]},");
            controls.push_str("\"boat-grid\":{\"columns\":[\"Polar file\",\"Crossover %\"],\"rows\":[");
            for (i, p) in polars.iter().enumerate() {
                if i > 0 { controls.push(','); }
                let _ = write!(controls, "[{},{}]", json(&p.file), json(&fmt(p.crossover)));
            }
            controls.push_str("]},");
        }
        None => controls.push_str("\"document-type\":\"No document\",\"polar-grid\":{\"columns\":[\"TWA / TWS\"],\"rows\":[]},\"boat-grid\":{\"columns\":[\"Polar file\",\"Crossover %\"],\"rows\":[]},"),
    }
    let _ = write!(
        controls,
        "\"diagnostics\":{},\"new-angle\":{},\"new-wind\":{},\"capture\":{},\"capture-status\":{},\"sample-count\":{}",
        json(&editor.diagnostics),
        json(&editor.new_angle),
        json(&editor.new_wind),
        if editor.capturing { "true" } else { "false" },
        json(&if editor.capturing {
            format!(
                "Recording valid true-wind and speed-through-water sentences — {} samples retained.",
                editor.samples
            )
        } else {
            format!("Stopped — {} samples retained.", editor.samples)
        }),
        json(&editor.samples.to_string())
    );
    format!(
        "{{\"status\":{},\"controls\":{{{controls}}}}}",
        json(&editor.status)
    )
}

fn sort(p: &mut Polar) {
    let mut rows: Vec<_> = p.twa.iter().copied().zip(p.speeds.clone()).collect();
    rows.sort_by(|a, b| a.0.total_cmp(&b.0));
    rows.dedup_by(|a, b| a.0 == b.0);
    p.twa = rows.iter().map(|r| r.0).collect();
    p.speeds = rows.into_iter().map(|r| r.1).collect();
    let mut columns: Vec<_> = p
        .tws
        .iter()
        .copied()
        .enumerate()
        .map(|(i, v)| (v, i))
        .collect();
    columns.sort_by(|a, b| a.0.total_cmp(&b.0));
    columns.dedup_by(|a, b| a.0 == b.0);
    p.tws = columns.iter().map(|c| c.0).collect();
    for row in &mut p.speeds {
        *row = columns
            .iter()
            .map(|c| row.get(c.1).copied().flatten())
            .collect();
    }
}

fn refresh(editor: &mut Editor) {
    editor.diagnostics = match editor.document.as_ref() {
        Some(Document::Polar(p)) => {
            validate(p).unwrap_or_else(|v| format!("Validation error: {v}"))
        }
        Some(Document::Boat { name, polars }) => format!(
            "Boat “{name}” references {} polar file(s). Each referenced file still requires explicit user selection.",
            polars.len()
        ),
        None => "Open or create a document.".into(),
    };
}

fn valid_nmea(sentence: &str) -> Option<Vec<&str>> {
    let sentence = sentence.trim();
    let star = sentence.rfind('*')?;
    if !sentence.starts_with('$') || star + 3 != sentence.len() {
        return None;
    }
    let expected = u8::from_str_radix(&sentence[star + 1..], 16).ok()?;
    let actual = sentence.as_bytes()[1..star]
        .iter()
        .fold(0_u8, |checksum, value| checksum ^ value);
    (actual == expected).then(|| sentence[..star].split(',').collect())
}

fn navigation_sample(editor: &mut Editor, sentence: &str) {
    let Some(fields) = valid_nmea(sentence) else {
        return;
    };
    let kind = fields
        .first()
        .and_then(|value| value.get(value.len().saturating_sub(3)..));
    match kind {
        Some("VWT") if fields.len() >= 5 => {
            if let (Ok(angle), Ok(speed)) = (fields[1].parse::<f64>(), fields[3].parse::<f64>()) {
                editor.last_twa = Some(angle.clamp(0.0, 180.0));
                editor.last_tws = Some(speed);
            }
        }
        Some("MWV")
            if fields.len() >= 6 && fields[2] == "T" && fields[4] == "N" && fields[5] == "A" =>
        {
            if let (Ok(angle), Ok(speed)) = (fields[1].parse::<f64>(), fields[3].parse::<f64>()) {
                editor.last_twa = Some(angle.min(360.0 - angle).clamp(0.0, 180.0));
                editor.last_tws = Some(speed);
            }
        }
        Some("VHW") if fields.len() >= 7 && fields[6] == "N" => {
            editor.last_stw = fields[5].parse::<f64>().ok();
        }
        _ => return,
    }
    if !editor.capturing {
        return;
    }
    let (Some(twa), Some(tws), Some(stw)) = (editor.last_twa, editor.last_tws, editor.last_stw)
    else {
        return;
    };
    if !(0.0..=180.0).contains(&twa)
        || !(0.0..=200.0).contains(&tws)
        || !(0.0..=100.0).contains(&stw)
    {
        return;
    }
    let Some(Document::Polar(polar)) = &mut editor.document else {
        return;
    };
    let row = ((twa / 5.0).round() as usize).min(polar.twa.len() - 1);
    let column = ((tws / 2.0).round() as usize)
        .saturating_sub(1)
        .min(polar.tws.len() - 1);
    if editor.sample_counts.len() != polar.twa.len() {
        editor.sample_counts = vec![vec![0; polar.tws.len()]; polar.twa.len()];
    }
    let count = editor.sample_counts[row][column];
    let previous = polar.speeds[row][column].unwrap_or(stw);
    polar.speeds[row][column] = Some((previous * f64::from(count) + stw) / f64::from(count + 1));
    editor.sample_counts[row][column] = count + 1;
    editor.samples += 1;
    editor.dirty = true;
}

struct IPolars;
impl exports::opencpn::portable::plugin::Guest for IPolars {
    fn initialize() -> Result<exports::opencpn::portable::plugin::PluginInfo, String> {
        host::register_action(
            "ipolars.open",
            "iPolars",
            "Open the portable polar and boat editor",
            Some("resources/ipolars.svg"),
        )?;
        let mut e = EDITOR.lock().map_err(|_| "iPolars state lock failed")?;
        e.document = Some(Document::Polar(default_polar()));
        e.status = "New cruising polar — open a .pol or boat .xml file, or begin editing.".into();
        refresh(&mut e);
        Ok(exports::opencpn::portable::plugin::PluginInfo {
            id: "org.opencpn.ipolars".into(),
            name: "iPolars".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }
    fn enable() -> Result<(), String> {
        host::log(LogLevel::Info, "iPolars enabled");
        Ok(())
    }
    fn disable() {
        host::log(LogLevel::Info, "iPolars disabled");
    }
    fn on_action(id: String) -> Result<(), String> {
        if id == "ipolars.open" {
            host::open_surface(SURFACE)
        } else {
            Err(format!("Unknown action: {id}"))
        }
    }
    fn on_surface_event(surface: String, control: String, value: String) -> Result<String, String> {
        if surface != SURFACE {
            return Err(format!("Unknown surface: {surface}"));
        }
        let mut e = EDITOR.lock().map_err(|_| "iPolars state lock failed")?;
        match control.as_str() {
            "surface-opened" | "refresh" => {}
            "open" => {
                let bytes = host::user_file_read(&json_string(&value)?)?;
                let text = String::from_utf8(bytes).map_err(|_| "File is not UTF-8 text.")?;
                e.document = Some(if text.trim_start().starts_with('<') {
                    parse_boat(&text)?
                } else {
                    Document::Polar(parse_polar(&text)?)
                });
                e.dirty = false;
                e.status = "Loaded and validated the selected document.".into();
            }
            "save-as" => {
                let document = e.document.as_ref().ok_or("There is no document to save.")?;
                host::user_file_write(&json_string(&value)?, serialize(document)?.as_bytes())?;
                e.dirty = false;
                e.status = "Saved atomically through a one-shot host grant.".into();
            }
            "new-polar" => {
                e.document = Some(Document::Polar(default_polar()));
                e.dirty = false;
                e.status = "Created a new cruising polar.".into();
            }
            "new-boat" => {
                e.document = Some(Document::Boat {
                    name: "New OpenCPN boat".into(),
                    polars: vec![BoatPolar {
                        file: "boat.pol".into(),
                        crossover: 0.0,
                    }],
                });
                e.dirty = false;
                e.status = "Created a new interoperable boat XML.".into();
            }
            "capture" => {
                let start = value.trim() == "true";
                if start && !e.capturing {
                    let polar = capture_polar();
                    e.sample_counts = vec![vec![0; polar.tws.len()]; polar.twa.len()];
                    e.document = Some(Document::Polar(polar));
                    e.samples = 0;
                    e.last_twa = None;
                    e.last_tws = None;
                    e.last_stw = None;
                    e.status = "Live polar capture started.".into();
                } else if !start {
                    e.status = "Live capture stopped; review and interpolate before saving.".into();
                }
                e.capturing = start;
            }
            "new-angle" => e.new_angle = json_string(&value)?,
            "new-wind" => e.new_wind = json_string(&value)?,
            "add-angle" => {
                let angle = number(&e.new_angle, "TWA")?;
                if !(0.0..=180.0).contains(&angle) {
                    return Err("TWA must be in [0,180]°.".into());
                }
                let Some(Document::Polar(p)) = &mut e.document else {
                    return Err("Open a polar first.".into());
                };
                p.twa.push(angle);
                p.speeds.push(vec![None; p.tws.len()]);
                sort(p);
                e.dirty = true;
                e.status = format!("Added {angle}° TWA.");
            }
            "add-wind" => {
                let wind = number(&e.new_wind, "TWS")?;
                if wind <= 0.0 || wind > 200.0 {
                    return Err("TWS must be in (0,200] kn.".into());
                }
                let Some(Document::Polar(p)) = &mut e.document else {
                    return Err("Open a polar first.".into());
                };
                p.tws.push(wind);
                for row in &mut p.speeds {
                    row.push(None);
                }
                sort(p);
                e.dirty = true;
                e.status = format!("Added {wind} kn TWS.");
            }
            "remove-angle" => {
                let row = e.selected.0;
                let Some(Document::Polar(p)) = &mut e.document else {
                    return Err("Open a polar first.".into());
                };
                if p.twa.len() <= 2 || row >= p.twa.len() {
                    return Err("Select a row; two must remain.".into());
                }
                p.twa.remove(row);
                p.speeds.remove(row);
                e.dirty = true;
                e.status = "Removed selected angle.".into();
            }
            "remove-wind" => {
                let col = e.selected.1;
                let Some(Document::Polar(p)) = &mut e.document else {
                    return Err("Open a polar first.".into());
                };
                if col == 0 || p.tws.len() <= 2 || col > p.tws.len() {
                    return Err("Select a TWS cell; two columns must remain.".into());
                }
                p.tws.remove(col - 1);
                for row in &mut p.speeds {
                    row.remove(col - 1);
                }
                e.dirty = true;
                e.status = "Removed selected wind speed.".into();
            }
            "normalize" => {
                let Some(Document::Polar(p)) = &mut e.document else {
                    return Err("Open a polar first.".into());
                };
                sort(p);
                e.dirty = true;
                e.status = "Sorted and de-duplicated both axes.".into();
            }
            "interpolate" => {
                let Some(Document::Polar(p)) = &mut e.document else {
                    return Err("Open a polar first.".into());
                };
                for c in 0..p.tws.len() {
                    for r in 0..p.twa.len() {
                        if p.speeds[r][c].is_none() {
                            let before = (0..r).rev().find_map(|i| p.speeds[i][c].map(|v| (i, v)));
                            let after =
                                (r + 1..p.twa.len()).find_map(|i| p.speeds[i][c].map(|v| (i, v)));
                            if let (Some((a, av)), Some((b, bv))) = (before, after) {
                                p.speeds[r][c] = Some(
                                    av + (bv - av) * (p.twa[r] - p.twa[a]) / (p.twa[b] - p.twa[a]),
                                );
                            }
                        }
                    }
                }
                e.dirty = true;
                e.status = "Interpolated bounded gaps.".into();
            }
            "polar-grid" => {
                let r = integer(&value, "row")?;
                let c = integer(&value, "column")?;
                e.selected = (r, c);
                if value.contains("\"value\":") {
                    let text = string_field(&value, "value")?;
                    let Some(Document::Polar(p)) = &mut e.document else {
                        return Err("Polar grid inactive.".into());
                    };
                    if r >= p.twa.len() || c > p.tws.len() {
                        return Err("Grid edit out of range.".into());
                    }
                    if c == 0 {
                        p.twa[r] = number(&text, "TWA")?;
                    } else {
                        p.speeds[r][c - 1] = if text.trim().is_empty() {
                            None
                        } else {
                            Some(number(&text, "boat speed")?)
                        };
                    }
                    e.dirty = true;
                    e.status = "Cell edited; validate before saving.".into();
                }
            }
            "boat-grid" => {
                let r = integer(&value, "row")?;
                let c = integer(&value, "column")?;
                let text = string_field(&value, "value")?;
                let Some(Document::Boat { polars, .. }) = &mut e.document else {
                    return Err("Boat grid inactive.".into());
                };
                let p = polars.get_mut(r).ok_or("Boat grid edit out of range.")?;
                if c == 0 {
                    p.file = text;
                } else if c == 1 {
                    p.crossover = number(&text, "crossover")?;
                } else {
                    return Err("Boat grid column invalid.".into());
                }
                e.dirty = true;
                e.status = "Boat reference edited.".into();
            }
            _ => return Err(format!("Unknown control: {control}")),
        }
        refresh(&mut e);
        Ok(response(&e))
    }
    fn on_job_event(_: String, _: exports::opencpn::portable::plugin::JobEvent) {}
    fn on_navigation_sentence(sentence: String) {
        if let Ok(mut editor) = EDITOR.lock() {
            navigation_sample(&mut editor, &sentence);
        }
    }
    fn calculate_route(
        _: exports::opencpn::portable::plugin::RouteRequest,
    ) -> Result<exports::opencpn::portable::plugin::RouteResult, String> {
        Err("iPolars edits vessel performance; it does not route.".into())
    }
    fn test_trap() {
        panic!("intentional iPolars conformance trap");
    }
}
export!(IPolars);
