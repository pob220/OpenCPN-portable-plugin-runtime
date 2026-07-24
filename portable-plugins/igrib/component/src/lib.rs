wit_bindgen::generate!({
    path: "../../../portable-runtime/contracts/0.2",
    world: "plugin-world",
});

use opencpn::portable::types::{
    ChartCoverageState, GeoPoint, GeoSegment, JobEvent, LogLevel, OverlayStyle, ServiceError,
    VesselPosition,
};

fn service_error(message: impl Into<String>) -> ServiceError {
    ServiceError {
        code: "plugin-error".into(),
        message: message.into(),
        retryable: false,
    }
}

mod host {
    use super::*;

    fn text(error: ServiceError) -> String {
        format!("{}: {}", error.code, error.message)
    }

    pub fn register_action(
        action_id: &str,
        label: &str,
        tooltip: &str,
        icon: Option<&str>,
    ) -> Result<u32, String> {
        opencpn::portable::actions::register(action_id, label, tooltip, icon).map_err(text)
    }
    pub fn log(level: LogLevel, message: &str) {
        opencpn::portable::diagnostics::log(level, message)
    }
    pub fn clear_scene(scene_id: &str) -> Result<(), String> {
        opencpn::portable::scenes::clear(scene_id).map_err(text)
    }
    pub fn cancel_job(job_id: &str) -> Result<(), String> {
        opencpn::portable::jobs::cancel(job_id).map_err(text)
    }
    pub fn open_environmental_viewer() -> Result<(), String> {
        opencpn::portable::surfaces::open("environment.viewer").map_err(text)
    }
    pub fn get_vessel_position() -> Result<VesselPosition, String> {
        opencpn::portable::navigation::get_vessel_position().map_err(text)
    }
    pub fn setting_get(key: &str) -> Result<Option<String>, String> {
        opencpn::portable::settings::get(key).map_err(text)
    }
    pub fn setting_set(key: &str, value: &str) -> Result<(), String> {
        opencpn::portable::settings::set(key, value).map_err(text)
    }
    pub fn submit_polyline(
        scene_id: &str,
        points: &[GeoPoint],
        style: OverlayStyle,
    ) -> Result<(), String> {
        opencpn::portable::scenes::submit_polyline(scene_id, points, style).map_err(text)
    }
    pub fn charts_query_segments(
        segments: &[GeoSegment],
    ) -> Result<Vec<opencpn::portable::types::ChartSegmentResult>, String> {
        opencpn::portable::chart_safety::query_segments(segments).map_err(text)
    }
    pub fn start_job(job_id: &str, work_units: u32) -> Result<(), String> {
        opencpn::portable::jobs::start(job_id, work_units).map_err(text)
    }
    pub fn network_get_to_private(
        request_id: &str,
        url: &str,
        private_name: &str,
        max_bytes: u64,
    ) -> Result<(), String> {
        opencpn::portable::network::get_to_private(request_id, url, private_name, max_bytes)
            .map_err(text)
    }
    pub fn storage_private_read(private_name: &str) -> Result<Vec<u8>, String> {
        opencpn::portable::storage::private_read(private_name).map_err(text)
    }
}

struct IGrib;

const ACTION_TOGGLE: &str = "igrib.toggle";
const ACTION_FAILURE_TEST: &str = "igrib.failure-test";
const ACTION_HTTP_TEST: &str = "igrib.http-test";
const SCENE_WEATHER: &str = "igrib.weather-window";
const JOB_PREPARE: &str = "igrib.prepare-weather";
const JOB_HTTP_TEST: &str = "igrib.http-probe";
const HTTP_TEST_FILE: &str = "http-probe.html";

fn fallback_position() -> VesselPosition {
    VesselPosition {
        latitude: 50.35,
        longitude: -4.15,
        course_over_ground: None,
        speed_over_ground: None,
    }
}

fn weather_window(position: &VesselPosition) -> Vec<GeoPoint> {
    let north = position.latitude + 0.35;
    let south = position.latitude - 0.35;
    let east = position.longitude + 0.55;
    let west = position.longitude - 0.55;
    vec![
        GeoPoint {
            latitude: north,
            longitude: west,
        },
        GeoPoint {
            latitude: north,
            longitude: east,
        },
        GeoPoint {
            latitude: south,
            longitude: east,
        },
        GeoPoint {
            latitude: south,
            longitude: west,
        },
        GeoPoint {
            latitude: north,
            longitude: west,
        },
    ]
}

impl IGrib {
    fn initialize() -> Result<exports::opencpn::portable::lifecycle::PluginInfo, String> {
        host::register_action(
            ACTION_TOGGLE,
            "iGRIB",
            "Open portable iGRIB",
            Some("resources/igrib.svg"),
        )?;
        host::log(LogLevel::Info, "iGRIB portable component initialised");
        Ok(exports::opencpn::portable::lifecycle::PluginInfo {
            id: "org.opencpn.igrib".into(),
            name: "iGRIB".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }

    fn enable() -> Result<(), String> {
        host::log(LogLevel::Info, "iGRIB enabled");
        Ok(())
    }

    fn disable() {
        let _ = host::clear_scene(SCENE_WEATHER);
        let _ = host::cancel_job(JOB_PREPARE);
        host::log(LogLevel::Info, "iGRIB disabled");
    }

    fn on_action(action_id: String) -> Result<(), String> {
        match action_id.as_str() {
            ACTION_TOGGLE => {
                if let Err(message) = host::open_environmental_viewer() {
                    host::log(
                        LogLevel::Warning,
                        &format!("Host environmental service unavailable: {message}"),
                    );
                }
                let position = host::get_vessel_position().unwrap_or_else(|message| {
                    host::log(
                        LogLevel::Warning,
                        &format!("No valid vessel position ({message}); using the test area"),
                    );
                    fallback_position()
                });
                let count = host::setting_get("activation-count")?
                    .and_then(|value| value.parse::<u64>().ok())
                    .unwrap_or(0)
                    + 1;
                host::setting_set("activation-count", &count.to_string())?;
                host::submit_polyline(
                    SCENE_WEATHER,
                    &weather_window(&position),
                    OverlayStyle {
                        red: 30,
                        green: 170,
                        blue: 245,
                        alpha: 225,
                        width_pixels: 4.0,
                    },
                )?;
                let window = weather_window(&position);
                let segments: Vec<GeoSegment> = window
                    .windows(2)
                    .map(|pair| GeoSegment {
                        start: pair[0],
                        end: pair[1],
                    })
                    .collect();
                let coverage = host::charts_query_segments(&segments)?;
                let missing = coverage
                    .iter()
                    .filter(|result| result.state != ChartCoverageState::Covered)
                    .count();
                host::log(
                    LogLevel::Info,
                    &format!(
                        "batched chart coverage query: {} segments, {} not covered",
                        coverage.len(),
                        missing
                    ),
                );
                host::start_job(JOB_PREPARE, 40)?;
                host::log(
                    LogLevel::Info,
                    &format!("iGRIB weather preparation started (activation {count})"),
                );
                Ok(())
            }
            ACTION_FAILURE_TEST => panic!("intentional iGRIB component trap"),
            ACTION_HTTP_TEST => {
                let url = host::setting_get("http-test-url")?
                    .unwrap_or_else(|| "https://opencpn.org/".into());
                host::network_get_to_private(JOB_HTTP_TEST, &url, HTTP_TEST_FILE, 1024 * 1024)?;
                host::log(LogLevel::Info, "iGRIB host HTTP request started");
                Ok(())
            }
            _ => Err(format!("unknown iGRIB action: {action_id}")),
        }
    }

    fn on_surface_event(
        surface_id: String,
        control_id: String,
        value_json: String,
    ) -> Result<String, String> {
        if surface_id != "environment.viewer" {
            return Err(format!("unknown iGRIB surface: {surface_id}"));
        }
        if control_id.is_empty()
            || control_id.len() > 96
            || !control_id
                .bytes()
                .all(|value| value.is_ascii_alphanumeric() || b"-._".contains(&value))
        {
            return Err("invalid portable surface control id".into());
        }
        if value_json.len() > 64 * 1024 {
            return Err("portable surface state exceeds 64 KiB".into());
        }
        // iGRIB owns its controller state. OpenCPN renders the declared
        // controls and provides services, but state persistence and policy
        // remain in the portable component.  The restore request is a typed
        // controller operation rather than a host-side knowledge of iGRIB's
        // preference keys.
        let key = format!("surface.{control_id}");
        if control_id == "display-settings" && value_json == "{\"request\":\"restore\"}" {
            return Ok(host::setting_get(&key)?.unwrap_or_else(|| "{}".into()));
        }
        host::setting_set(&key, &value_json)?;
        host::log(
            LogLevel::Debug,
            &format!("environment.viewer state updated: {control_id}"),
        );
        Ok(value_json)
    }

    fn on_job_event(job_id: String, event: JobEvent) {
        match event {
            JobEvent::Progress(percent) => {
                if percent % 25 == 0 {
                    host::log(LogLevel::Debug, &format!("{job_id}: {percent}%"));
                }
            }
            JobEvent::Completed if job_id == JOB_HTTP_TEST => {
                match host::storage_private_read(HTTP_TEST_FILE) {
                    Ok(bytes) => host::log(
                        LogLevel::Info,
                        &format!(
                            "{job_id} completed; {} bytes read from private storage",
                            bytes.len()
                        ),
                    ),
                    Err(message) => host::log(
                        LogLevel::Error,
                        &format!("{job_id} completed but private storage read failed: {message}"),
                    ),
                }
            }
            JobEvent::Completed => host::log(LogLevel::Info, &format!("{job_id} completed")),
            JobEvent::Cancelled => host::log(LogLevel::Info, &format!("{job_id} cancelled")),
            JobEvent::Failed(message) => {
                host::log(LogLevel::Error, &format!("{job_id} failed: {message}"))
            }
        }
    }
}

impl exports::opencpn::portable::lifecycle::Guest for IGrib {
    fn initialize() -> Result<exports::opencpn::portable::lifecycle::PluginInfo, ServiceError> {
        IGrib::initialize().map_err(service_error)
    }
    fn enable() -> Result<(), ServiceError> {
        IGrib::enable().map_err(service_error)
    }
    fn disable() {
        IGrib::disable()
    }
    fn on_action(action_id: String) -> Result<(), ServiceError> {
        IGrib::on_action(action_id).map_err(service_error)
    }
}

impl exports::opencpn::portable::surface_event_sink::Guest for IGrib {
    fn on_surface_event(
        surface_id: String,
        control_id: String,
        value_json: String,
    ) -> Result<String, ServiceError> {
        IGrib::on_surface_event(surface_id, control_id, value_json).map_err(service_error)
    }
}

impl exports::opencpn::portable::job_event_sink::Guest for IGrib {
    fn on_job_event(job_id: String, event: JobEvent) {
        IGrib::on_job_event(job_id, event)
    }
}

impl exports::opencpn::portable::event_sink::Guest for IGrib {
    fn on_event(_event: opencpn::portable::types::Event) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::portable::plugin_message_sink::Guest for IGrib {
    fn on_plugin_message(_message_id: String, _message_body: String) -> Result<(), ServiceError> {
        Ok(())
    }
}

export!(IGrib);
