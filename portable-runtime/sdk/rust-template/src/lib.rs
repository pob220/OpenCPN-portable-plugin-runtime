wit_bindgen::generate!({
    path: "../../contracts/0.3",
    world: "plugin-world",
});

use opencpn::portable::types::{
    ActionLocation, ActionRegistration, CirclePrimitive, Color, Event, GeoPoint, KeyEvent,
    LogLevel, PointerEvent, RpcRequest, RpcResponse, SceneLayer, ScenePrimitive, SceneStyle,
    SceneUpdate, ServiceError, TimerEvent,
};

const ACTION_ID: &str = "template.hello";

fn error(message: impl Into<String>) -> ServiceError {
    ServiceError {
        code: "template-error".into(),
        message: message.into(),
        retryable: false,
    }
}

struct Template;

impl exports::opencpn::portable::lifecycle::Guest for Template {
    fn initialize() -> Result<exports::opencpn::portable::lifecycle::PluginInfo, ServiceError> {
        opencpn::portable::actions::register(&ActionRegistration {
            action_id: ACTION_ID.into(),
            label: "Portable hello".into(),
            tooltip: "Exercise the API 0.3 author template".into(),
            icon_resource: None,
            locations: vec![ActionLocation::Toolbar, ActionLocation::ChartContextMenu],
        })?;
        Ok(exports::opencpn::portable::lifecycle::PluginInfo {
            id: "org.opencpn.portable-template".into(),
            name: "Portable Plugin Template".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }

    fn enable() -> Result<(), ServiceError> {
        opencpn::portable::diagnostics::log(LogLevel::Info, "template enabled");
        opencpn::portable::scenes::submit(&SceneUpdate {
            scene_id: "template.scene".into(),
            revision: 1,
            replace: true,
            layers: vec![SceneLayer {
                layer_id: "main".into(),
                z_index: 0,
                visible: true,
                primitives: vec![ScenePrimitive::Circle(CirclePrimitive {
                    primitive_id: "hello".into(),
                    centre: GeoPoint {
                        latitude: 0.0,
                        longitude: 0.0,
                    },
                    radius_metres: 1000.0,
                    style: SceneStyle {
                        stroke: Some(Color {
                            red: 25,
                            green: 111,
                            blue: 137,
                            alpha: 255,
                        }),
                        fill: None,
                        width_pixels: 2.0,
                    },
                    interactive: true,
                })],
            }],
        })?;
        opencpn::portable::timers::schedule("template.tick", 1_000, None)?;
        opencpn::portable::plugin_rpc::register_service("template.echo")?;
        Ok(())
    }

    fn disable() {
        let _ = opencpn::portable::scenes::clear("template.scene");
        let _ = opencpn::portable::timers::cancel("template.tick");
        let _ = opencpn::portable::plugin_rpc::unregister_service("template.echo");
    }

    fn on_action(action_id: String) -> Result<(), ServiceError> {
        if action_id != ACTION_ID {
            return Err(error(format!("unknown action {action_id}")));
        }
        opencpn::portable::settings::set("last-action", "hello")?;
        opencpn::portable::private_storage::write_atomic("last-action", b"hello")?;
        opencpn::portable::surfaces::open("template.main")?;
        Ok(())
    }
}

impl exports::opencpn::portable::surface_event_sink::Guest for Template {
    fn on_surface_event(
        surface_id: String,
        control_id: String,
        _value_json: String,
    ) -> Result<String, ServiceError> {
        if surface_id != "template.main" {
            return Err(error(format!("unknown surface {surface_id}")));
        }
        if control_id != "hello" && control_id != "refresh" {
            return Err(error(format!("unknown control {control_id}")));
        }
        Ok(r#"{"template-status":"API 0.3 surface callback is working"}"#.into())
    }
}

impl exports::opencpn::portable::event_sink::Guest for Template {
    fn on_event(_value: Event) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::portable::input_sink::Guest for Template {
    fn on_pointer(_value: PointerEvent) -> Result<bool, ServiceError> {
        Ok(false)
    }

    fn on_key(_value: KeyEvent) -> Result<bool, ServiceError> {
        Ok(false)
    }
}

impl exports::opencpn::portable::timer_sink::Guest for Template {
    fn on_timer(_value: TimerEvent) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::portable::rpc_sink::Guest for Template {
    fn on_request(source_package: String, request: RpcRequest) -> Result<(), ServiceError> {
        opencpn::portable::plugin_rpc::respond(
            &source_package,
            &RpcResponse {
                correlation_id: request.correlation_id,
                status: 200,
                content_type: request.content_type,
                payload: request.payload,
                diagnostic: String::new(),
            },
        )
    }

    fn on_response(_source_package: String, _response: RpcResponse) -> Result<(), ServiceError> {
        Ok(())
    }
}

export!(Template);
