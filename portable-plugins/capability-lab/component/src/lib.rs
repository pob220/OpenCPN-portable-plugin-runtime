wit_bindgen::generate!({
    path: "../../../portable-runtime/wit",
    world: "plugin-world",
});

use opencpn::portable::host::{self, LogLevel};
use std::sync::Mutex;

const ACTION_PROBE: &str = "capability-lab.probe";
const ACTION_ECHO_MESSAGE: &str = "capability-lab.echo-message";
const ACTION_ECHO_NMEA: &str = "capability-lab.echo-nmea";

#[derive(Default)]
struct ProbeState {
    message: String,
    sentence: String,
}

static STATE: Mutex<ProbeState> = Mutex::new(ProbeState {
    message: String::new(),
    sentence: String::new(),
});

struct CapabilityLab;

impl exports::opencpn::portable::plugin::Guest for CapabilityLab {
    fn initialize() -> Result<exports::opencpn::portable::plugin::PluginInfo, String> {
        host::register_action(
            ACTION_PROBE,
            "Capability probe",
            "Send a bounded portable-to-native plugin message",
            None,
        )?;
        host::register_action(
            ACTION_ECHO_MESSAGE,
            "Echo plugin message",
            "Echo the last filtered native-to-portable plugin message",
            None,
        )?;
        host::register_action(
            ACTION_ECHO_NMEA,
            "Echo NMEA",
            "Echo the last filtered NMEA 0183 sentence",
            None,
        )?;
        Ok(exports::opencpn::portable::plugin::PluginInfo {
            id: "org.opencpn.capability-lab".into(),
            name: "Portable Capability Lab".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }

    fn enable() -> Result<(), String> {
        host::log(LogLevel::Info, "portable capability lab enabled");
        Ok(())
    }

    fn disable() {}

    fn on_action(action_id: String) -> Result<(), String> {
        match action_id.as_str() {
            ACTION_PROBE => host::send_plugin_message(
                "CAPABILITY_OUT",
                "{\"source\":\"portable\",\"probe\":\"messaging\"}",
            ),
            ACTION_ECHO_MESSAGE => {
                let message = STATE
                    .lock()
                    .map_err(|_| "capability probe state lock failed")?
                    .message
                    .clone();
                host::send_plugin_message("CAPABILITY_ECHO", &message)
            }
            ACTION_ECHO_NMEA => {
                let sentence = STATE
                    .lock()
                    .map_err(|_| "capability probe state lock failed")?
                    .sentence
                    .clone();
                host::send_plugin_message("CAPABILITY_NMEA", &sentence)
            }
            _ => Err(format!("unknown capability probe action: {action_id}")),
        }
    }

    fn on_surface_event(
        surface_id: String,
        _control_id: String,
        value: String,
    ) -> Result<String, String> {
        if surface_id != "host.plugin-message" {
            return Err(format!("unknown capability probe surface: {surface_id}"));
        }
        STATE
            .lock()
            .map_err(|_| "capability probe state lock failed")?
            .message = value;
        Ok("{}".into())
    }

    fn on_job_event(_job_id: String, _event: exports::opencpn::portable::plugin::JobEvent) {}

    fn on_navigation_sentence(sentence: String) {
        if let Ok(mut state) = STATE.lock() {
            state.sentence = sentence;
        }
    }

    fn calculate_route(
        _request: exports::opencpn::portable::plugin::RouteRequest,
    ) -> Result<exports::opencpn::portable::plugin::RouteResult, String> {
        Err("the capability lab is not a weather-routing engine".into())
    }

    fn test_trap() {
        panic!("intentional capability-lab trap");
    }
}

export!(CapabilityLab);
