wit_bindgen::generate!({
    path: "../../wit",
    world: "plugin-world",
});

struct LegacyFixture;

impl exports::opencpn::portable::plugin::Guest for LegacyFixture {
    fn initialize() -> Result<exports::opencpn::portable::plugin::PluginInfo, String> {
        Ok(exports::opencpn::portable::plugin::PluginInfo {
            id: "org.opencpn.igrib".into(),
            name: "iGRIB".into(),
            version: "0.1.0".into(),
        })
    }

    fn enable() -> Result<(), String> {
        Ok(())
    }

    fn disable() {}

    fn on_action(_action_id: String) -> Result<(), String> {
        Ok(())
    }

    fn on_surface_event(
        _surface_id: String,
        _control_id: String,
        _value: String,
    ) -> Result<String, String> {
        Ok("{}".into())
    }

    fn on_job_event(_job_id: String, _event: exports::opencpn::portable::plugin::JobEvent) {}

    fn on_navigation_sentence(_sentence: String) {}

    fn calculate_route(
        _request: exports::opencpn::portable::plugin::RouteRequest,
    ) -> Result<exports::opencpn::portable::plugin::RouteResult, String> {
        Err("legacy compatibility fixture does not route".into())
    }

    fn test_trap() {
        panic!("intentional portable API 0.1 compatibility trap")
    }
}

export!(LegacyFixture);
