wit_bindgen::generate!({
    path: "../../../portable-runtime/contracts/0.2",
    world: "plugin-world",
});

use opencpn::portable::types::{Event, EventKind, JobEvent, LogLevel, ServiceError};
use std::sync::Mutex;

const ACTION_PROBE: &str = "capability-lab.probe";
const ACTION_ECHO_MESSAGE: &str = "capability-lab.echo-message";
const ACTION_ECHO_NMEA: &str = "capability-lab.echo-nmea";

fn service_error(message: impl Into<String>) -> ServiceError {
    ServiceError {
        code: "plugin-error".into(),
        message: message.into(),
        retryable: false,
    }
}

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

impl exports::opencpn::portable::lifecycle::Guest for CapabilityLab {
    fn initialize() -> Result<exports::opencpn::portable::lifecycle::PluginInfo, ServiceError> {
        opencpn::portable::actions::register(
            ACTION_PROBE,
            "Capability probe",
            "Send a bounded portable-to-native plugin message",
            None,
        )?;
        opencpn::portable::actions::register(
            ACTION_ECHO_MESSAGE,
            "Echo plugin message",
            "Echo the last filtered native-to-portable plugin message",
            None,
        )?;
        opencpn::portable::actions::register(
            ACTION_ECHO_NMEA,
            "Echo NMEA",
            "Echo the last filtered NMEA 0183 sentence",
            None,
        )?;
        Ok(exports::opencpn::portable::lifecycle::PluginInfo {
            id: "org.opencpn.capability-lab".into(),
            name: "Portable Capability Lab".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }

    fn enable() -> Result<(), ServiceError> {
        opencpn::portable::diagnostics::log(LogLevel::Info, "portable capability lab enabled");
        Ok(())
    }

    fn disable() {}

    fn on_action(action_id: String) -> Result<(), ServiceError> {
        let result = match action_id.as_str() {
            ACTION_PROBE => opencpn::portable::plugin_messages::send(
                "CAPABILITY_OUT",
                "{\"source\":\"portable\",\"probe\":\"messaging\"}",
            ),
            ACTION_ECHO_MESSAGE => {
                let message = STATE
                    .lock()
                    .map_err(|_| service_error("capability probe state lock failed"))?
                    .message
                    .clone();
                opencpn::portable::plugin_messages::send("CAPABILITY_ECHO", &message)
            }
            ACTION_ECHO_NMEA => {
                let sentence = STATE
                    .lock()
                    .map_err(|_| service_error("capability probe state lock failed"))?
                    .sentence
                    .clone();
                opencpn::portable::plugin_messages::send("CAPABILITY_NMEA", &sentence)
            }
            _ => Err(service_error(format!(
                "unknown capability probe action: {action_id}"
            ))),
        };
        result
    }
}

impl exports::opencpn::portable::surface_event_sink::Guest for CapabilityLab {
    fn on_surface_event(
        surface_id: String,
        _control_id: String,
        _value: String,
    ) -> Result<String, ServiceError> {
        Err(service_error(format!(
            "unknown capability probe surface: {surface_id}"
        )))
    }
}

impl exports::opencpn::portable::job_event_sink::Guest for CapabilityLab {
    fn on_job_event(_job_id: String, _event: JobEvent) {}
}

impl exports::opencpn::portable::event_sink::Guest for CapabilityLab {
    fn on_event(event: Event) -> Result<(), ServiceError> {
        if event.kind == EventKind::Nmea0183 {
            STATE
                .lock()
                .map_err(|_| service_error("capability probe state lock failed"))?
                .sentence = event.payload;
        }
        Ok(())
    }
}

impl exports::opencpn::portable::plugin_message_sink::Guest for CapabilityLab {
    fn on_plugin_message(_message_id: String, message_body: String) -> Result<(), ServiceError> {
        STATE
            .lock()
            .map_err(|_| service_error("capability probe state lock failed"))?
            .message = message_body;
        Ok(())
    }
}

export!(CapabilityLab);
