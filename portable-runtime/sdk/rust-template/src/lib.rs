wit_bindgen::generate!({
    path: "../../contracts/0.4",
    world: "plugin-world",
});

use opencpn::opp::types::{
    ActionLocation, ActionRegistration, CirclePrimitive, Color, Event, GeoPoint, KeyEvent,
    LogLevel, PointerEvent, RpcRequest, RpcResponse, SceneCanvasTarget, SceneLayer, ScenePrimitive,
    SceneRenderPhase, SceneStyle, SceneUpdate, ServiceError, SurfaceRole, TimerEvent,
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

impl exports::opencpn::opp::lifecycle::Guest for Template {
    fn initialize() -> Result<exports::opencpn::opp::lifecycle::PluginInfo, ServiceError> {
        opencpn::opp::actions::register(&ActionRegistration {
            action_id: ACTION_ID.into(),
            label: "Portable hello".into(),
            tooltip: "Exercise the OPP API 0.4 author template".into(),
            icon_resource: None,
            locations: vec![ActionLocation::Toolbar, ActionLocation::ChartContextMenu],
        })?;
        Ok(exports::opencpn::opp::lifecycle::PluginInfo {
            id: "org.opencpn.portable-template".into(),
            name: "Portable Plugin Template".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }

    fn enable() -> Result<(), ServiceError> {
        opencpn::opp::diagnostics::log(LogLevel::Info, "template enabled");
        opencpn::opp::scenes::submit(&SceneUpdate {
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
                        dash_pattern: vec![],
                    },
                    interactive: true,
                })],
            }],
            canvas_target: SceneCanvasTarget::All,
            selected_canvases: vec![],
            render_phase: SceneRenderPhase::AboveVessels,
        })?;
        opencpn::opp::timers::schedule("template.tick", 1_000, None)?;
        opencpn::opp::plugin_rpc::register_service("template.echo")?;
        Ok(())
    }

    fn disable() {
        let _ = opencpn::opp::scenes::clear("template.scene");
        let _ = opencpn::opp::timers::cancel("template.tick");
        let _ = opencpn::opp::plugin_rpc::unregister_service("template.echo");
    }

    fn on_action(invocation: opencpn::opp::types::ActionInvocation) -> Result<(), ServiceError> {
        if invocation.action_id != ACTION_ID {
            return Err(error(format!("unknown action {}", invocation.action_id)));
        }
        opencpn::opp::settings::set("last-action", "hello")?;
        opencpn::opp::private_storage::write_atomic("last-action", b"hello")?;
        opencpn::opp::surfaces::open("template.main", SurfaceRole::ToolWindow)?;
        Ok(())
    }
}

impl exports::opencpn::opp::surface_event_sink::Guest for Template {
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
        Ok(r#"{"template-status":"OPP API 0.4 surface callback is working"}"#.into())
    }
}

impl exports::opencpn::opp::event_sink::Guest for Template {
    fn on_event(_value: Event) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::opp::input_sink::Guest for Template {
    fn on_pointer(_value: PointerEvent) -> Result<bool, ServiceError> {
        Ok(false)
    }

    fn on_key(_value: KeyEvent) -> Result<bool, ServiceError> {
        Ok(false)
    }
}

impl exports::opencpn::opp::timer_sink::Guest for Template {
    fn on_timer(_value: TimerEvent) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::opp::rpc_sink::Guest for Template {
    fn on_request(source_package: String, request: RpcRequest) -> Result<(), ServiceError> {
        opencpn::opp::plugin_rpc::respond(
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
