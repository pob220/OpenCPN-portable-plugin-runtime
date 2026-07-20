wit_bindgen::generate!({
    path: "../../../portable-runtime/wit",
    world: "plugin-world",
});

use opencpn::portable::host::{
    self, ChartCoverageState, GeoPoint, GeoSegment, LogLevel, OverlayStyle, VesselPosition,
};

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

impl exports::opencpn::portable::plugin::Guest for IGrib {
    fn initialize() -> Result<exports::opencpn::portable::plugin::PluginInfo, String> {
        host::register_action(
            ACTION_TOGGLE,
            "iGRIB",
            "Open the portable iGRIB proof of concept",
            Some("resources/igrib.svg"),
        )?;
        host::register_action(
            ACTION_FAILURE_TEST,
            "iGRIB fault test",
            "Deliberately trap the portable component (developer test)",
            Some("resources/fault-test.svg"),
        )?;
        host::register_action(
            ACTION_HTTP_TEST,
            "iGRIB host HTTP test",
            "Download a small OpenCPN page through the capability-controlled host client",
            Some("resources/http-download.svg"),
        )?;
        host::log(LogLevel::Info, "iGRIB portable component initialised");
        Ok(exports::opencpn::portable::plugin::PluginInfo {
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

    fn on_job_event(job_id: String, event: exports::opencpn::portable::plugin::JobEvent) {
        use exports::opencpn::portable::plugin::JobEvent;
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

    fn calculate_route(
        _request: exports::opencpn::portable::plugin::RouteRequest,
    ) -> Result<exports::opencpn::portable::plugin::RouteResult, String> {
        Err("iGRIB is an environmental-data provider, not a routing engine".into())
    }

    fn test_trap() {
        panic!("intentional portable component conformance trap");
    }
}

export!(IGrib);
