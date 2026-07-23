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
struct Measurement {
    tws: f64,
    twa: f64,
    aws: f64,
    awa: f64,
    stw: f64,
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
    selected_boat: usize,
    new_polar_file: String,
    new_crossover: String,
    measurement_apparent: bool,
    measurement_wind_speed: String,
    measurement_wind_angle: String,
    measurement_stw: String,
    selected_measurement: usize,
    measurements: Vec<Measurement>,
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
    selected_boat: 0,
    new_polar_file: String::new(),
    new_crossover: String::new(),
    measurement_apparent: false,
    measurement_wind_speed: String::new(),
    measurement_wind_angle: String::new(),
    measurement_stw: String::new(),
    selected_measurement: 0,
    measurements: Vec::new(),
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

fn is_matrix_header(value: &str) -> bool {
    let normalized = value.trim().replace('\\', "/").to_ascii_lowercase();
    normalized == "twa/tws" || normalized == "twa"
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

fn parse_expedition(lines: &[&str]) -> Result<Polar, String> {
    let mut samples = Vec::new();
    let mut winds = Vec::new();
    let mut angles = Vec::new();
    for (line_index, line) in lines.iter().enumerate() {
        let row = fields(line);
        if row.len() < 5 || row.len() % 2 == 0 {
            return Err(format!(
                "Invalid Expedition row {}; expected TWS followed by TWA/STW pairs.",
                line_index + 1
            ));
        }
        let wind = number(row[0], "TWS")?;
        if !(0.0..=200.0).contains(&wind) || wind == 0.0 {
            return Err("Expedition TWS values must lie in (0, 200] kn.".into());
        }
        winds.push(wind);
        for pair in row[1..].chunks_exact(2) {
            let angle = number(pair[0], "TWA")?;
            let speed = number(pair[1], "boat speed")?;
            if !(0.0..=180.0).contains(&angle) || !(0.0..=100.0).contains(&speed) {
                return Err(
                    "Expedition TWA/STW values must lie in [0, 180]° and [0, 100] kn.".into(),
                );
            }
            angles.push(angle);
            samples.push((wind, angle, speed));
        }
    }
    winds.sort_by(f64::total_cmp);
    winds.dedup_by(|left, right| *left == *right);
    angles.sort_by(f64::total_cmp);
    angles.dedup_by(|left, right| *left == *right);
    let mut speeds = vec![vec![None; winds.len()]; angles.len()];
    for (wind, angle, speed) in samples {
        let column = winds
            .iter()
            .position(|value| *value == wind)
            .ok_or("Internal Expedition TWS indexing failure.")?;
        let row = angles
            .iter()
            .position(|value| *value == angle)
            .ok_or("Internal Expedition TWA indexing failure.")?;
        speeds[row][column] = Some(speed);
    }
    let polar = Polar {
        name: "Imported Expedition polar".into(),
        tws: winds,
        twa: angles,
        speeds,
    };
    validate(&polar)?;
    Ok(polar)
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
    const EXPEDITION_MINIMUM_FIELDS: usize = 5;
    if lines.iter().all(|line| {
        let row = fields(line);
        row.len() >= EXPEDITION_MINIMUM_FIELDS
            && row.len() % 2 == 1
            && row.iter().all(|value| value.parse::<f64>().is_ok())
    }) {
        return parse_expedition(&lines);
    }
    let mut name = "Imported polar".to_string();
    if !fields(lines[0])
        .first()
        .is_some_and(|value| is_matrix_header(value))
    {
        name = lines.remove(0).to_string();
    }
    if lines.is_empty() {
        return Err("The TWA/TWS header is missing.".into());
    }
    let header = fields(lines.remove(0));
    if header.len() < 3 || !is_matrix_header(header[0]) {
        return Err("Expected TWA/TWS followed by wind-speed columns.".into());
    }
    let raw_tws = header[1..]
        .iter()
        .map(|v| number(v, "TWS"))
        .collect::<Result<Vec<_>, _>>()?;
    let mut twa = Vec::new();
    let mut speeds = Vec::new();
    for (index, line) in lines.iter().enumerate() {
        let row = fields(line);
        if row.is_empty() || row.len() > raw_tws.len() + 1 {
            return Err(format!("Invalid row {}.", index + 2));
        }
        twa.push(number(row[0], "TWA")?);
        let mut values = Vec::new();
        for column in 0..raw_tws.len() {
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
    // polar_pi's OCPN/QTVlm and MaxSea exports add zero-wind and 60-knot
    // boundary columns populated entirely by zero placeholders.  They are
    // formatting sentinels rather than measured performance columns.
    let retained_columns = raw_tws
        .iter()
        .enumerate()
        .filter_map(|(column, wind)| {
            let has_performance = speeds
                .iter()
                .any(|row| row[column].is_some_and(|speed| speed > 0.0));
            (*wind > 0.0 && has_performance).then_some(column)
        })
        .collect::<Vec<_>>();
    let tws = retained_columns
        .iter()
        .map(|column| raw_tws[*column])
        .collect::<Vec<_>>();
    for row in &mut speeds {
        *row = retained_columns.iter().map(|column| row[*column]).collect();
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
            let crossover = attr(tag, "CrossOverPercentage")
                .map(|value| number(&value, "CrossOverPercentage"))
                .transpose()?
                .unwrap_or(0.0);
            if !(0.0..=100.0).contains(&crossover) {
                return Err("CrossOverPercentage must lie in [0, 100].".into());
            }
            polars.push(BoatPolar { file, crossover });
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

fn json_string_array(value: &str) -> Result<Vec<String>, String> {
    let value = value.trim();
    if !value.starts_with('[') || !value.ends_with(']') {
        return Err("Expected a bounded array of opaque file grants.".into());
    }
    let body = &value[1..value.len() - 1];
    let mut values = Vec::new();
    let mut start = None;
    let mut escaped = false;
    for (index, character) in body.char_indices() {
        if let Some(quote) = start {
            if escaped {
                escaped = false;
            } else if character == '\\' {
                escaped = true;
            } else if character == '"' {
                values.push(json_string(&body[quote..=index])?);
                start = None;
                if values.len() > 64 {
                    return Err("At most 64 logbook files may be imported at once.".into());
                }
            }
        } else if character == '"' {
            start = Some(index);
        } else if character != ',' && !character.is_whitespace() {
            return Err("Malformed opaque file-grant array.".into());
        }
    }
    if start.is_some() || escaped || values.is_empty() {
        return Err("Malformed or empty opaque file-grant array.".into());
    }
    Ok(values)
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
            let mut table = String::from("{\"columns\":[\"TWA / TWS\"");
            // Build one immutable value for both the editable matrix and the
            // host-rendered diagram, keeping the portable UI state canonical.
            for wind in &p.tws {
                table.push(',');
                table.push_str(&json(&format!("{} kn", fmt(*wind))));
            }
            table.push_str("],\"rows\":[");
            for (r, angle) in p.twa.iter().enumerate() {
                if r > 0 { table.push(','); }
                table.push('[');
                table.push_str(&json(&fmt(*angle)));
                for speed in &p.speeds[r] {
                    table.push(',');
                    table.push_str(&json(&speed.map(fmt).unwrap_or_default()));
                }
                table.push(']');
            }
            table.push_str("]}");
            let _ = write!(
                controls,
                "\"polar-grid\":{table},\"polar-plot\":{table},\"boat-name\":\"\",\"boat-grid\":{{\"columns\":[\"Polar file\",\"Crossover %\"],\"rows\":[]}},"
            );
        }
        Some(Document::Boat { name, polars }) => {
            controls.push_str("\"document-type\":\"OpenCPN Weather Routing boat XML\",\"polar-grid\":{\"columns\":[\"TWA / TWS\"],\"rows\":[]},\"polar-plot\":{\"columns\":[\"TWA / TWS\"],\"rows\":[]},");
            let _ = write!(controls, "\"boat-name\":{},", json(name));
            controls.push_str("\"boat-grid\":{\"columns\":[\"Polar file\",\"Crossover %\"],\"rows\":[");
            for (i, p) in polars.iter().enumerate() {
                if i > 0 { controls.push(','); }
                let _ = write!(controls, "[{},{}]", json(&p.file), json(&fmt(p.crossover)));
            }
            controls.push_str("]},");
        }
        None => controls.push_str("\"document-type\":\"No document\",\"polar-grid\":{\"columns\":[\"TWA / TWS\"],\"rows\":[]},\"polar-plot\":{\"columns\":[\"TWA / TWS\"],\"rows\":[]},\"boat-name\":\"\",\"boat-grid\":{\"columns\":[\"Polar file\",\"Crossover %\"],\"rows\":[]},"),
    }
    controls.push_str("\"measurement-grid\":{\"rows\":[");
    for (index, measurement) in editor.measurements.iter().enumerate() {
        if index > 0 {
            controls.push(',');
        }
        let _ = write!(
            controls,
            "[{},{},{},{},{}]",
            json(&fmt(measurement.tws)),
            json(&fmt(measurement.twa)),
            json(&fmt(measurement.aws)),
            json(&fmt(measurement.awa)),
            json(&fmt(measurement.stw))
        );
    }
    controls.push_str("]},");
    let _ = write!(
        controls,
        "\"diagnostics\":{},\"new-angle\":{},\"new-wind\":{},\"new-polar-file\":{},\"new-crossover\":{},\"measurement-apparent\":{},\"measurement-wind-speed\":{},\"measurement-wind-angle\":{},\"measurement-stw\":{},\"capture\":{},\"capture-status\":{},\"sample-count\":{}",
        json(&editor.diagnostics),
        json(&editor.new_angle),
        json(&editor.new_wind),
        json(&editor.new_polar_file),
        json(&editor.new_crossover),
        if editor.measurement_apparent {
            "true"
        } else {
            "false"
        },
        json(&editor.measurement_wind_speed),
        json(&editor.measurement_wind_angle),
        json(&editor.measurement_stw),
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

fn begin_capture(editor: &mut Editor) {
    let polar = capture_polar();
    editor.sample_counts = vec![vec![0; polar.tws.len()]; polar.twa.len()];
    editor.document = Some(Document::Polar(polar));
    editor.samples = 0;
    editor.last_twa = None;
    editor.last_tws = None;
    editor.last_stw = None;
}

fn retain_sample(editor: &mut Editor, twa: f64, tws: f64, stw: f64) -> bool {
    if !(0.0..=180.0).contains(&twa)
        || !(0.0..=200.0).contains(&tws)
        || !(0.0..=100.0).contains(&stw)
    {
        return false;
    }
    let Some(Document::Polar(polar)) = &mut editor.document else {
        return false;
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
    true
}

fn true_from_apparent(aws: f64, awa: f64, stw: f64) -> Option<(f64, f64)> {
    if !aws.is_finite() || !awa.is_finite() || !stw.is_finite() || aws < 0.0 || stw < 0.0 {
        return None;
    }
    let angle = awa.abs().to_radians();
    let true_forward = -aws * angle.cos() + stw;
    let true_starboard = -aws * angle.sin();
    let tws = true_forward.hypot(true_starboard);
    if tws < 1e-9 {
        return Some((0.0, 0.0));
    }
    let twa = (-true_starboard)
        .atan2(-true_forward)
        .to_degrees()
        .abs()
        .clamp(0.0, 180.0);
    Some((tws, twa))
}

fn apparent_from_true(tws: f64, twa: f64, stw: f64) -> Option<(f64, f64)> {
    if !tws.is_finite()
        || !twa.is_finite()
        || !stw.is_finite()
        || tws < 0.0
        || stw < 0.0
        || !(0.0..=180.0).contains(&twa)
    {
        return None;
    }
    let angle = twa.to_radians();
    let apparent_forward = -tws * angle.cos() - stw;
    let apparent_starboard = -tws * angle.sin();
    let aws = apparent_forward.hypot(apparent_starboard);
    if aws < 1e-9 {
        return Some((0.0, 0.0));
    }
    let awa = (-apparent_starboard)
        .atan2(-apparent_forward)
        .to_degrees()
        .abs()
        .clamp(0.0, 180.0);
    Some((aws, awa))
}

fn nmea_speed(value: f64, unit: &str) -> Option<f64> {
    match unit {
        "N" => Some(value),
        "M" => Some(value * 1.943_844_492_440_6),
        "K" => Some(value / 1.852),
        _ => None,
    }
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
            if let (Ok(angle), Ok(raw)) = (fields[1].parse::<f64>(), fields[3].parse::<f64>()) {
                if let Some(speed) = nmea_speed(raw, fields[4]) {
                    editor.last_twa = Some(angle.abs().min(180.0));
                    editor.last_tws = Some(speed);
                }
            }
        }
        Some("MWV") if fields.len() >= 6 && fields[5] == "A" => {
            if let (Ok(angle), Ok(raw)) = (fields[1].parse::<f64>(), fields[3].parse::<f64>()) {
                if let Some(speed) = nmea_speed(raw, fields[4]) {
                    if fields[2] == "T" {
                        editor.last_twa = Some(angle.min(360.0 - angle).abs().clamp(0.0, 180.0));
                        editor.last_tws = Some(speed);
                    } else if fields[2] == "R" {
                        if let Some(stw) = editor.last_stw {
                            if let Some((tws, twa)) = true_from_apparent(speed, angle, stw) {
                                editor.last_twa = Some(twa);
                                editor.last_tws = Some(tws);
                            }
                        }
                    }
                }
            }
        }
        Some("VHW") if fields.len() >= 7 => {
            if let Ok(raw) = fields[5].parse::<f64>() {
                editor.last_stw = nmea_speed(raw, fields[6]);
            }
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
    retain_sample(editor, twa, tws, stw);
}

fn append_nmea_log(editor: &mut Editor, text: &str) -> (usize, usize) {
    editor.capturing = true;
    let mut valid = 0;
    let mut rejected = 0;
    for raw_line in text.lines().take(1_000_000) {
        let Some(start) = raw_line.find('$') else {
            rejected += 1;
            continue;
        };
        let sentence = raw_line[start..].trim();
        if valid_nmea(sentence).is_some() {
            valid += 1;
            navigation_sample(editor, sentence);
        } else {
            rejected += 1;
        }
    }
    editor.capturing = false;
    (valid, rejected)
}

fn import_nmea_log(editor: &mut Editor, text: &str) -> (usize, usize) {
    begin_capture(editor);
    append_nmea_log(editor, text)
}

fn csv_fields(line: &str) -> Vec<&str> {
    if line.contains('\t') {
        line.split('\t').map(str::trim).collect()
    } else if line.contains(';') {
        line.split(';').map(str::trim).collect()
    } else {
        line.split(',').map(str::trim).collect()
    }
}

fn append_csv(editor: &mut Editor, text: &str) -> Result<(usize, usize), String> {
    let mut lines = text.lines().filter(|line| !line.trim().is_empty());
    let header = csv_fields(lines.next().ok_or("Observation CSV is empty.")?);
    let names = header
        .iter()
        .map(|value| value.trim().to_ascii_lowercase())
        .collect::<Vec<_>>();
    let column = |name: &str| names.iter().position(|value| value == name);
    let tws_column = column("tws");
    let twa_column = column("twa");
    let aws_column = column("aws");
    let awa_column = column("awa");
    let stw_column = column("stw").ok_or(
        "Observation CSV requires STW; SOG is not substituted because current changes it.",
    )?;
    if (tws_column.is_none() || twa_column.is_none())
        && (aws_column.is_none() || awa_column.is_none())
    {
        return Err("Observation CSV requires TWS/TWA or AWS/AWA columns.".into());
    }
    let engine_column = column("engineon");
    let manoeuvre_column = column("manoeuvring");
    let steady_column = column("steady");
    let truthy = |value: &str| {
        matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "1" | "true" | "yes" | "on"
        )
    };
    let mut accepted = 0;
    let mut rejected = 0;
    for line in lines.take(1_000_000) {
        let values = csv_fields(line);
        let value = |index: Option<usize>| {
            index
                .and_then(|position| values.get(position))
                .and_then(|text| text.parse::<f64>().ok())
        };
        if engine_column.is_some_and(|index| values.get(index).is_some_and(|entry| truthy(entry)))
            || manoeuvre_column
                .is_some_and(|index| values.get(index).is_some_and(|entry| truthy(entry)))
            || steady_column
                .is_some_and(|index| values.get(index).is_some_and(|entry| !truthy(entry)))
        {
            rejected += 1;
            continue;
        }
        let Some(stw) = value(Some(stw_column)) else {
            rejected += 1;
            continue;
        };
        let wind = match (value(tws_column), value(twa_column)) {
            (Some(tws), Some(twa)) => Some((tws, twa.abs().min(180.0))),
            _ => match (value(aws_column), value(awa_column)) {
                (Some(aws), Some(awa)) => true_from_apparent(aws, awa, stw),
                _ => None,
            },
        };
        if let Some((tws, twa)) = wind {
            if retain_sample(editor, twa, tws, stw) {
                accepted += 1;
            } else {
                rejected += 1;
            }
        } else {
            rejected += 1;
        }
    }
    Ok((accepted, rejected))
}

fn import_csv(editor: &mut Editor, text: &str) -> Result<(usize, usize), String> {
    begin_capture(editor);
    append_csv(editor, text)
}

fn logbook_number(value: &str) -> Option<f64> {
    let normalized = value.trim().replace(',', ".");
    let token = normalized
        .split_whitespace()
        .next()
        .unwrap_or("")
        .trim_matches(|character: char| {
            !(character.is_ascii_digit()
                || character == '.'
                || character == '-'
                || character == '+'
                || character == 'e'
                || character == 'E')
        });
    token
        .parse::<f64>()
        .ok()
        .filter(|number| number.is_finite())
}

fn append_logbook_konni(editor: &mut Editor, text: &str) -> (usize, usize) {
    let mut accepted = 0;
    let mut rejected = 0;
    for line in text.lines().take(1_000_000) {
        let fields = line.split('\t').map(str::trim).collect::<Vec<_>>();
        if fields.len() <= 20 || fields.get(7).copied() != Some("S") {
            rejected += 1;
            continue;
        }
        let engine_running = [28_usize, 40_usize].iter().any(|index| {
            fields
                .get(*index)
                .is_some_and(|value| !value.is_empty() && !value.contains("00:00"))
        });
        if engine_running {
            rejected += 1;
            continue;
        }
        let Some(stw) = fields.get(15).and_then(|value| logbook_number(value)) else {
            rejected += 1;
            continue;
        };
        let Some(direction_field) = fields.get(19) else {
            rejected += 1;
            continue;
        };
        let Some(mut angle) = logbook_number(direction_field) else {
            rejected += 1;
            continue;
        };
        let Some(speed_field) = fields.get(20) else {
            rejected += 1;
            continue;
        };
        let Some(raw_wind) = logbook_number(speed_field) else {
            rejected += 1;
            continue;
        };
        let lower = speed_field.to_ascii_lowercase();
        let wind = if lower.contains("m/s") {
            raw_wind * 1.943_844_492_440_6
        } else if lower.contains("km/h") || lower.contains("kph") {
            raw_wind / 1.852
        } else {
            raw_wind
        };
        angle = angle.rem_euclid(360.0);
        if angle > 180.0 {
            angle = 360.0 - angle;
        }
        let true_wind = if direction_field.to_ascii_uppercase().contains('R') {
            true_from_apparent(wind, angle, stw)
        } else {
            Some((wind, angle))
        };
        if let Some((tws, twa)) = true_wind {
            if retain_sample(editor, twa, tws, stw) {
                accepted += 1;
                continue;
            }
        }
        rejected += 1;
    }
    (accepted, rejected)
}

fn looks_like_nmea(text: &str) -> bool {
    text.lines().take(200).any(|line| {
        line.find('$')
            .is_some_and(|start| valid_nmea(line[start..].trim()).is_some())
    })
}

fn looks_like_logbook_konni(text: &str) -> bool {
    text.lines()
        .take(50)
        .any(|line| line.split('\t').count() >= 21)
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
        e.new_polar_file = "boat.pol".into();
        e.new_crossover = "0".into();
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
            "boat-name" => {
                let name = json_string(&value)?;
                if name.trim().is_empty() || name.len() > 256 {
                    return Err("Boat name must contain 1–256 characters.".into());
                }
                let Some(Document::Boat {
                    name: document_name,
                    ..
                }) = &mut e.document
                else {
                    return Err("Open a boat XML document first.".into());
                };
                *document_name = name;
                e.dirty = true;
                e.status = "Boat name edited.".into();
            }
            "new-polar-file" => e.new_polar_file = json_string(&value)?,
            "new-crossover" => e.new_crossover = json_string(&value)?,
            "add-polar-reference" => {
                let file = e.new_polar_file.trim().to_string();
                if file.is_empty() || file.len() > 4096 {
                    return Err("Enter a bounded polar file reference.".into());
                }
                let crossover = number(&e.new_crossover, "crossover")?;
                if !(0.0..=100.0).contains(&crossover) {
                    return Err("Crossover must lie in [0, 100] percent.".into());
                }
                let Some(Document::Boat { polars, .. }) = &mut e.document else {
                    return Err("Open a boat XML document first.".into());
                };
                polars.push(BoatPolar { file, crossover });
                e.selected_boat = polars.len() - 1;
                e.dirty = true;
                e.status = "Added a polar reference.".into();
            }
            "remove-polar-reference" => {
                let selected = e.selected_boat;
                let Some(Document::Boat { polars, .. }) = &mut e.document else {
                    return Err("Open a boat XML document first.".into());
                };
                if polars.len() <= 1 {
                    return Err("A routable boat XML must retain at least one polar.".into());
                }
                if selected >= polars.len() {
                    return Err("Select a polar reference first.".into());
                }
                polars.remove(selected);
                e.selected_boat = selected.min(polars.len() - 1);
                e.dirty = true;
                e.status = "Removed the selected polar reference.".into();
            }
            "move-polar-up" | "move-polar-down" => {
                let selected = e.selected_boat;
                let Some(Document::Boat { polars, .. }) = &mut e.document else {
                    return Err("Open a boat XML document first.".into());
                };
                if selected >= polars.len() {
                    return Err("Select a polar reference first.".into());
                }
                let destination = if control == "move-polar-up" {
                    selected.saturating_sub(1)
                } else {
                    (selected + 1).min(polars.len() - 1)
                };
                if destination != selected {
                    polars.swap(selected, destination);
                    e.selected_boat = destination;
                    e.dirty = true;
                }
                e.status = "Reordered the boat polar references.".into();
            }
            "measurement-apparent" => {
                e.measurement_apparent = value.trim() == "true";
            }
            "measurement-wind-speed" => {
                e.measurement_wind_speed = json_string(&value)?;
            }
            "measurement-wind-angle" => {
                e.measurement_wind_angle = json_string(&value)?;
            }
            "measurement-stw" => e.measurement_stw = json_string(&value)?,
            "add-measurement" => {
                let wind_speed = number(&e.measurement_wind_speed, "wind speed")?;
                let wind_angle = number(&e.measurement_wind_angle, "wind angle")?;
                let stw = number(&e.measurement_stw, "speed through water")?;
                if !(0.0..=200.0).contains(&wind_speed)
                    || !(0.0..=180.0).contains(&wind_angle)
                    || !(0.0..=100.0).contains(&stw)
                {
                    return Err(
                        "Wind speed, wind angle and STW are outside physical bounds.".into(),
                    );
                }
                let measurement = if e.measurement_apparent {
                    let (tws, twa) = true_from_apparent(wind_speed, wind_angle, stw)
                        .ok_or("Could not convert the apparent-wind observation.")?;
                    Measurement {
                        tws,
                        twa,
                        aws: wind_speed,
                        awa: wind_angle,
                        stw,
                    }
                } else {
                    let (aws, awa) = apparent_from_true(wind_speed, wind_angle, stw)
                        .ok_or("Could not convert the true-wind observation.")?;
                    Measurement {
                        tws: wind_speed,
                        twa: wind_angle,
                        aws,
                        awa,
                        stw,
                    }
                };
                e.measurements.push(measurement);
                e.selected_measurement = e.measurements.len() - 1;
                e.status = format!("Added manual observation {}.", e.measurements.len());
            }
            "measurement-grid" => {
                e.selected_measurement = integer(&value, "row")?;
            }
            "remove-measurement" => {
                let selected = e.selected_measurement;
                if selected >= e.measurements.len() {
                    return Err("Select a manual observation first.".into());
                }
                e.measurements.remove(selected);
                e.selected_measurement = e
                    .selected_measurement
                    .min(e.measurements.len().saturating_sub(1));
                e.status = "Removed the selected manual observation.".into();
            }
            "clear-measurements" => {
                e.measurements.clear();
                e.selected_measurement = 0;
                e.status = "Cleared all manual observations.".into();
            }
            "generate-from-measurements" => {
                let measurements = e.measurements.clone();
                if measurements.is_empty() {
                    return Err("Add at least one manual observation first.".into());
                }
                begin_capture(&mut e);
                for measurement in &measurements {
                    retain_sample(&mut e, measurement.twa, measurement.tws, measurement.stw);
                }
                e.capturing = false;
                e.status = format!(
                    "Generated a measured polar from {} manual observation(s).",
                    measurements.len()
                );
            }
            "capture" => {
                let start = value.trim() == "true";
                if start && !e.capturing {
                    begin_capture(&mut e);
                    e.status = "Live polar capture started.".into();
                } else if !start {
                    e.status = "Live capture stopped; review and interpolate before saving.".into();
                }
                e.capturing = start;
            }
            "import-vdr" => {
                let bytes = host::user_file_read(&json_string(&value)?)?;
                let text =
                    String::from_utf8(bytes).map_err(|_| "NMEA/VDR log is not UTF-8 text.")?;
                let (valid, rejected) = import_nmea_log(&mut e, &text);
                e.status = format!(
                    "Imported NMEA/VDR log: {valid} valid sentences, {rejected} rejected lines, {} complete samples retained.",
                    e.samples
                );
            }
            "import-csv" => {
                let bytes = host::user_file_read(&json_string(&value)?)?;
                let text =
                    String::from_utf8(bytes).map_err(|_| "Observation CSV is not UTF-8 text.")?;
                let (accepted, rejected) = import_csv(&mut e, &text)?;
                e.status = format!(
                    "Imported observations: {accepted} accepted, {rejected} rejected. Motoring, manoeuvring, unstable, incomplete, and SOG-only rows are excluded."
                );
            }
            "import-logbooks" => {
                let grants = json_string_array(&value)?;
                begin_capture(&mut e);
                let mut files = 0_usize;
                let mut accepted = 0_usize;
                let mut rejected = 0_usize;
                let mut formats = Vec::new();
                for grant in grants {
                    let bytes = host::user_file_read(&grant)?;
                    let text = String::from_utf8(bytes)
                        .map_err(|_| "A selected logbook is not UTF-8 text.")?;
                    let (file_accepted, file_rejected, format) = if looks_like_nmea(&text) {
                        let before = e.samples;
                        let (valid, invalid) = append_nmea_log(&mut e, &text);
                        let retained =
                            usize::try_from(e.samples.saturating_sub(before)).unwrap_or(usize::MAX);
                        (
                            retained,
                            invalid + valid.saturating_sub(retained),
                            "NMEA/VDR",
                        )
                    } else if looks_like_logbook_konni(&text) {
                        let (valid, invalid) = append_logbook_konni(&mut e, &text);
                        (valid, invalid, "LogbookKonni")
                    } else {
                        let (valid, invalid) = append_csv(&mut e, &text)?;
                        (valid, invalid, "CSV/TSV")
                    };
                    accepted += file_accepted;
                    rejected += file_rejected;
                    files += 1;
                    if !formats.contains(&format) {
                        formats.push(format);
                    }
                }
                e.capturing = false;
                e.status = format!(
                    "Imported {files} logbook file(s) ({formats}): {accepted} complete STW/wind observations accepted, {rejected} rows or sentences rejected.",
                    formats = formats.join(", ")
                );
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
                e.selected_boat = r;
                if value.contains("\"value\":") {
                    let text = string_field(&value, "value")?;
                    let Some(Document::Boat { polars, .. }) = &mut e.document else {
                        return Err("Boat grid inactive.".into());
                    };
                    let p = polars.get_mut(r).ok_or("Boat grid edit out of range.")?;
                    if c == 0 {
                        if text.trim().is_empty() || text.len() > 4096 {
                            return Err("Polar file reference is empty or oversized.".into());
                        }
                        p.file = text;
                    } else if c == 1 {
                        let crossover = number(&text, "crossover")?;
                        if !(0.0..=100.0).contains(&crossover) {
                            return Err("Crossover must lie in [0, 100] percent.".into());
                        }
                        p.crossover = crossover;
                    } else {
                        return Err("Boat grid column invalid.".into());
                    }
                    e.dirty = true;
                    e.status = "Boat reference edited.".into();
                }
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
