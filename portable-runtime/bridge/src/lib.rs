use std::ffi::{CStr, c_char, c_void};
use std::fs;
use std::path::Path;
use std::ptr;
use std::slice;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::Duration;

use wasmtime::component::ResourceTable;
use wasmtime::component::{Component, HasSelf, Linker};
use wasmtime::{Config, Engine, Store, StoreLimits, StoreLimitsBuilder, Trap};
use wasmtime_wasi::{WasiCtx, WasiCtxView, WasiView};

use exports::opencpn::portable::plugin::JobEvent;

wasmtime::component::bindgen!({
    path: "../wit",
    world: "plugin-world",
});

mod api_v02_imports {
    wasmtime::component::bindgen!({
        path: "../contracts/0.2",
        interfaces: "
            import opencpn:portable/types@0.2.0;
            import opencpn:portable/diagnostics@0.2.0;
            import opencpn:portable/actions@0.2.0;
            import opencpn:portable/navigation@0.2.0;
            import opencpn:portable/settings@0.2.0;
            import opencpn:portable/scenes@0.2.0;
            import opencpn:portable/jobs@0.2.0;
            import opencpn:portable/surfaces@0.2.0;
            import opencpn:portable/environment@0.2.0;
            import opencpn:portable/routing-control@0.2.0;
            import opencpn:portable/chart-safety@0.2.0;
            import opencpn:portable/network@0.2.0;
            import opencpn:portable/storage@0.2.0;
            import opencpn:portable/plugin-messages@0.2.0;
        ",
    });
}

mod api_v02 {
    wasmtime::component::bindgen!({
        path: "../contracts/0.2",
        world: "plugin-world",
        with: {
            "opencpn:portable/types@0.2.0": crate::api_v02_imports::opencpn::portable::types,
            "opencpn:portable/diagnostics@0.2.0": crate::api_v02_imports::opencpn::portable::diagnostics,
            "opencpn:portable/actions@0.2.0": crate::api_v02_imports::opencpn::portable::actions,
            "opencpn:portable/navigation@0.2.0": crate::api_v02_imports::opencpn::portable::navigation,
            "opencpn:portable/settings@0.2.0": crate::api_v02_imports::opencpn::portable::settings,
            "opencpn:portable/scenes@0.2.0": crate::api_v02_imports::opencpn::portable::scenes,
            "opencpn:portable/jobs@0.2.0": crate::api_v02_imports::opencpn::portable::jobs,
            "opencpn:portable/surfaces@0.2.0": crate::api_v02_imports::opencpn::portable::surfaces,
            "opencpn:portable/environment@0.2.0": crate::api_v02_imports::opencpn::portable::environment,
            "opencpn:portable/routing-control@0.2.0": crate::api_v02_imports::opencpn::portable::routing_control,
            "opencpn:portable/chart-safety@0.2.0": crate::api_v02_imports::opencpn::portable::chart_safety,
            "opencpn:portable/network@0.2.0": crate::api_v02_imports::opencpn::portable::network,
            "opencpn:portable/storage@0.2.0": crate::api_v02_imports::opencpn::portable::storage,
            "opencpn:portable/plugin-messages@0.2.0": crate::api_v02_imports::opencpn::portable::plugin_messages,
        },
    });
}

mod api_v02_routing {
    wasmtime::component::bindgen!({
        path: "../contracts/0.2",
        world: "weather-routing-plugin-world",
        with: {
            "opencpn:portable/types@0.2.0": crate::api_v02_imports::opencpn::portable::types,
            "opencpn:portable/diagnostics@0.2.0": crate::api_v02_imports::opencpn::portable::diagnostics,
            "opencpn:portable/actions@0.2.0": crate::api_v02_imports::opencpn::portable::actions,
            "opencpn:portable/navigation@0.2.0": crate::api_v02_imports::opencpn::portable::navigation,
            "opencpn:portable/settings@0.2.0": crate::api_v02_imports::opencpn::portable::settings,
            "opencpn:portable/scenes@0.2.0": crate::api_v02_imports::opencpn::portable::scenes,
            "opencpn:portable/jobs@0.2.0": crate::api_v02_imports::opencpn::portable::jobs,
            "opencpn:portable/surfaces@0.2.0": crate::api_v02_imports::opencpn::portable::surfaces,
            "opencpn:portable/environment@0.2.0": crate::api_v02_imports::opencpn::portable::environment,
            "opencpn:portable/routing-control@0.2.0": crate::api_v02_imports::opencpn::portable::routing_control,
            "opencpn:portable/chart-safety@0.2.0": crate::api_v02_imports::opencpn::portable::chart_safety,
            "opencpn:portable/network@0.2.0": crate::api_v02_imports::opencpn::portable::network,
            "opencpn:portable/storage@0.2.0": crate::api_v02_imports::opencpn::portable::storage,
            "opencpn:portable/plugin-messages@0.2.0": crate::api_v02_imports::opencpn::portable::plugin_messages,
        },
    });
}

const HOST_ABI_VERSION: u32 = 14;
const PORTABLE_API_V01: u32 = 1;
const PORTABLE_API_V02: u32 = 2;
const PORTABLE_WORLD_PLUGIN: u32 = 0;
const PORTABLE_WORLD_WEATHER_ROUTING: u32 = 1;
const ROUTE_POINT_LIMIT: usize = 20_000;
const ROUTE_INSPECTION_POINT_LIMIT: usize = 200_000;
const ROUTE_INSPECTION_LINE_LIMIT: usize = 10_000;
const ERROR_TEXT_LIMIT: usize = 4096;
const SETTINGS_VALUE_LIMIT: usize = 64 * 1024;
const OVERLAY_POINT_LIMIT: usize = 1_000_000;
const CHART_SEGMENT_LIMIT: usize = 10_000;
const ENVIRONMENT_SAMPLE_LIMIT: usize = 100_000;
const POLAR_GRID_LIMIT: usize = 8;
const POLAR_AXIS_LIMIT: usize = 200;
const POLAR_CELL_LIMIT: usize = 200_000;
const PRIVATE_READ_LIMIT: usize = 8 * 1024 * 1024;
const USER_FILE_LIMIT: usize = 8 * 1024 * 1024;
const NAVIGATION_SENTENCE_LIMIT: usize = 1024;
const PLUGIN_MESSAGE_ID_LIMIT: usize = 256;
const PLUGIN_MESSAGE_BODY_LIMIT: usize = 64 * 1024;
const EPOCH_TICK: Duration = Duration::from_millis(100);
const CALL_EPOCH_DEADLINE: u64 = 50;
const ROUTING_BASE_FUEL: u64 = 2_000_000_000;
// Routing may perform an independently bounded coarse pass followed by a
// fine-corridor pass. Current-aware graph recovery uses the environment
// service's conservative physical current ceiling as an admissible A* bound.
// Budget fuel for both while retaining the state and epoch backstops.
const ROUTING_FUEL_PER_RETAINED_STATE: u64 = 1_500_000;
const ROUTING_MAX_FUEL: u64 = 250_000_000_000;
// A routing export can now contain an independently bounded initial pass and
// fine-corridor pass, including current-aware A* recovery. Keep an absolute
// backstop, but do not abort a healthy default 80,000-state route after the
// former seven-minute allowance. The resulting
// deadline scales from 30 to 60 minutes; ordinary routes still return as soon
// as they finish and remain cooperatively cancellable throughout.
const ROUTING_BASE_EPOCH_TICKS: u64 = 18_000;
const ROUTING_EXTRA_EPOCH_TICKS: u64 = 18_000;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GeoPoint {
    latitude: f64,
    longitude: f64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OverlayStyle {
    red: u8,
    green: u8,
    blue: u8,
    alpha: u8,
    width_pixels: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GeoSegment {
    start: GeoPoint,
    end: GeoPoint,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ChartSegmentResult {
    state: u32,
    charts_considered: u32,
    reason: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FinalChartSafetyOptions {
    safety_margin_nautical_miles: f64,
    minimum_depth_metres: f64,
    require_authoritative: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EnvironmentSampleRequest {
    latitude: f64,
    longitude: f64,
    unix_time: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct EnvironmentSample {
    wind_u_knots: f64,
    wind_v_knots: f64,
    current_u_knots: f64,
    current_v_knots: f64,
    wave_height_metres: f64,
    available: u32,
}

#[repr(C)]
pub struct PolarGrid {
    identity: *const c_char,
    identity_len: usize,
    true_wind_speeds_knots: *const f64,
    true_wind_speed_count: usize,
    true_wind_angles_degrees: *const f64,
    true_wind_angle_count: usize,
    boat_speeds_knots: *const f64,
    boat_speed_count: usize,
}

#[repr(C)]
pub struct RouteRequest {
    start_latitude: f64,
    start_longitude: f64,
    destination_latitude: f64,
    destination_longitude: f64,
    departure_unix_time: i64,
    polars: *const PolarGrid,
    polar_count: usize,
    time_step_seconds: u32,
    heading_step_degrees: u16,
    refined_heading_step_degrees: u16,
    adaptive_headings: u8,
    spatial_cell_nautical_miles: f64,
    labels_per_cell: u8,
    max_hours: u32,
    max_states: u32,
    inspection_interval_seconds: u32,
    include_traces: u8,
    avoid_unsafe_charts: u8,
    min_true_wind_angle_degrees: f64,
    max_true_wind_angle_degrees: f64,
    max_wind_knots: f64,
    max_apparent_wind_knots: f64,
    max_wave_metres: f64,
    max_opposing_wind_current_knots_squared: f64,
    land_safety_margin_nautical_miles: f64,
    maximum_latitude_degrees: f64,
    upwind_efficiency: f64,
    downwind_efficiency: f64,
    maximum_search_angle_degrees: f64,
    destination_tolerance_nm: f64,
    tack_penalty_seconds: u32,
    gybe_penalty_seconds: u32,
    allow_motor_sailing: u8,
    allow_motor: u8,
    motor_below_sailing_speed_knots: f64,
    motor_speed_knots: f64,
    motor_sailing_boost_knots: f64,
    motor_crossover_hysteresis_knots: f64,
    minimum_motor_run_seconds: u32,
    mode_change_penalty_seconds: u32,
    maximum_motor_seconds: u32,
    fuel_consumption_litres_per_hour: f64,
    maximum_fuel_litres: f64,
    use_currents: u8,
    require_current_data: u8,
    use_waves: u8,
    require_wave_data: u8,
    limits_available: u32,
    minimum_chart_depth_metres: f64,
    require_authoritative_chart_safety: u8,
}

#[repr(C)]
pub struct RoutePoint {
    latitude: f64,
    longitude: f64,
    unix_time: i64,
}

#[repr(C)]
pub struct RouteLine {
    point_offset: usize,
    point_count: usize,
    unix_time: i64,
}

#[repr(C)]
pub struct RouteEnvironmentPoint {
    latitude: f64,
    longitude: f64,
    unix_time: i64,
    wind_u_knots: f64,
    wind_v_knots: f64,
    current_u_knots: f64,
    current_v_knots: f64,
    wave_height_metres: f64,
    available: u8,
}

#[repr(C)]
pub struct RouteResult {
    points: *mut RoutePoint,
    point_capacity: usize,
    point_count: usize,
    isochrone_points: *mut RoutePoint,
    isochrone_point_capacity: usize,
    isochrone_point_count: usize,
    isochrones: *mut RouteLine,
    isochrone_capacity: usize,
    isochrone_count: usize,
    trace_points: *mut RoutePoint,
    trace_point_capacity: usize,
    trace_point_count: usize,
    traces: *mut RouteLine,
    trace_capacity: usize,
    trace_count: usize,
    route_environment: *mut RouteEnvironmentPoint,
    route_environment_capacity: usize,
    route_environment_count: usize,
    distance_nautical_miles: f64,
    duration_seconds: u64,
    states_examined: u32,
    average_speed_knots: f64,
    maximum_speed_knots: f64,
    average_sog_knots: f64,
    maximum_sog_knots: f64,
    average_wind_knots: f64,
    maximum_wind_knots: f64,
    average_current_knots: f64,
    maximum_current_knots: f64,
    tacks: u32,
    motor_seconds: u64,
    estimated_fuel_litres: f64,
    propulsion_transitions: u32,
    comfort_level: u8,
    metrics_available: u8,
    diagnostic: *mut c_char,
    diagnostic_capacity: usize,
    diagnostic_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostCallbacks {
    abi_version: u32,
    user_data: *mut c_void,
    log: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char, usize)>,
    register_action: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            usize,
            *const c_char,
            usize,
            *const c_char,
            usize,
            *const c_char,
            usize,
            *mut u32,
        ) -> i32,
    >,
    get_vessel_position: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut f64,
            *mut f64,
            *mut f64,
            *mut u8,
            *mut f64,
            *mut u8,
        ) -> i32,
    >,
    setting_get: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            usize,
            *mut c_char,
            usize,
            *mut usize,
            *mut u8,
        ) -> i32,
    >,
    setting_set: Option<
        unsafe extern "C" fn(*mut c_void, *const c_char, usize, *const c_char, usize) -> i32,
    >,
    submit_polyline: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            usize,
            *const GeoPoint,
            usize,
            OverlayStyle,
        ) -> i32,
    >,
    clear_scene: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize) -> i32>,
    start_job: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize, u32) -> i32>,
    cancel_job: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize) -> i32>,
    open_surface: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize) -> i32>,
    open_environmental_viewer: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    open_weather_routing: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    environment_sample_batch: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const EnvironmentSampleRequest,
            usize,
            *mut EnvironmentSample,
            usize,
            *mut c_char,
            usize,
        ) -> i32,
    >,
    routing_progress: Option<unsafe extern "C" fn(*mut c_void, u8, *const c_char, usize)>,
    routing_cancelled: Option<unsafe extern "C" fn(*mut c_void) -> u8>,
    charts_query_segments: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const GeoSegment,
            usize,
            *mut ChartSegmentResult,
            usize,
        ) -> i32,
    >,
    network_get_to_private: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            usize,
            *const c_char,
            usize,
            *const c_char,
            usize,
            u64,
        ) -> i32,
    >,
    storage_private_read: Option<
        unsafe extern "C" fn(*mut c_void, *const c_char, usize, *mut u8, usize, *mut usize) -> i32,
    >,
    user_file_read: Option<
        unsafe extern "C" fn(*mut c_void, *const c_char, usize, *mut u8, usize, *mut usize) -> i32,
    >,
    user_file_write:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize, *const u8, usize) -> i32>,
    send_plugin_message: Option<
        unsafe extern "C" fn(*mut c_void, *const c_char, usize, *const c_char, usize) -> i32,
    >,
    charts_query_final_safety: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const GeoSegment,
            usize,
            *const FinalChartSafetyOptions,
            *mut ChartSegmentResult,
            usize,
        ) -> i32,
    >,
}

unsafe impl Send for HostCallbacks {}

struct HostState {
    callbacks: HostCallbacks,
    limits: StoreLimits,
    wasi: WasiCtx,
    table: ResourceTable,
}

impl WasiView for HostState {
    fn ctx(&mut self) -> WasiCtxView<'_> {
        WasiCtxView {
            ctx: &mut self.wasi,
            table: &mut self.table,
        }
    }
}

struct EpochTicker {
    stop: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
}

impl Drop for EpochTicker {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(ticker) = self.thread.take() {
            let _ = ticker.join();
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ApiKind {
    V01,
    V02,
    V02WeatherRouting,
}

enum RuntimeBindings {
    V01(PluginWorld),
    V02(api_v02::PluginWorld),
    V02WeatherRouting(api_v02_routing::WeatherRoutingPluginWorld),
}

pub struct Runtime {
    engine: Engine,
    component: Component,
    callbacks: HostCallbacks,
    store: Store<HostState>,
    bindings: RuntimeBindings,
    api_kind: ApiKind,
    _epoch_ticker: Arc<EpochTicker>,
}

fn instantiate_runtime(
    engine: Engine,
    component: Component,
    callbacks: HostCallbacks,
    api_kind: ApiKind,
    epoch_ticker: Arc<EpochTicker>,
) -> anyhow::Result<Runtime> {
    let mut linker = Linker::new(&engine);
    // Supply only WASI's inert defaults. In particular, do not inherit
    // environment variables, arguments, stdio, filesystem preopens or
    // network access. OpenCPN capabilities are the only authority source.
    wasmtime_wasi::p2::add_to_linker_sync(&mut linker)?;
    match api_kind {
        ApiKind::V01 => {
            PluginWorld::add_to_linker::<_, HasSelf<_>>(&mut linker, |state| state)?;
        }
        ApiKind::V02 => {
            api_v02::PluginWorld::add_to_linker::<_, HasSelf<_>>(&mut linker, |state| state)?;
        }
        ApiKind::V02WeatherRouting => {
            api_v02_routing::WeatherRoutingPluginWorld::add_to_linker::<_, HasSelf<_>>(
                &mut linker,
                |state| state,
            )?;
        }
    }

    let limits = StoreLimitsBuilder::new()
        .memory_size(256 * 1024 * 1024)
        .table_elements(100_000)
        .instances(4)
        .memories(4)
        .tables(8)
        .build();
    let mut store = Store::new(
        &engine,
        HostState {
            callbacks,
            limits,
            wasi: WasiCtx::builder().build(),
            table: ResourceTable::new(),
        },
    );
    store.limiter(|state| &mut state.limits);
    store.set_fuel(100_000_000)?;
    store.set_epoch_deadline(CALL_EPOCH_DEADLINE);
    let bindings = match api_kind {
        ApiKind::V01 => {
            RuntimeBindings::V01(PluginWorld::instantiate(&mut store, &component, &linker)?)
        }
        ApiKind::V02 => RuntimeBindings::V02(api_v02::PluginWorld::instantiate(
            &mut store, &component, &linker,
        )?),
        ApiKind::V02WeatherRouting => RuntimeBindings::V02WeatherRouting(
            api_v02_routing::WeatherRoutingPluginWorld::instantiate(
                &mut store, &component, &linker,
            )?,
        ),
    };
    Ok(Runtime {
        engine,
        component,
        callbacks,
        store,
        bindings,
        api_kind,
        _epoch_ticker: epoch_ticker,
    })
}

fn v02_guest_error(
    error: api_v02_imports::opencpn::portable::types::ServiceError,
) -> anyhow::Error {
    anyhow::anyhow!("{}: {}", error.code, error.message)
}

fn prepare_call(runtime: &mut Runtime) -> anyhow::Result<()> {
    // A fresh finite budget on every guest entry prevents one component from
    // monopolising the UI thread while avoiding lifetime fuel depletion.
    runtime.store.set_fuel(100_000_000)?;
    // The engine ticker advances every 100 ms.  This is a five-second
    // wall-clock ceiling even if compiled guest code does not consume fuel as
    // expected; host services apply their own, shorter operation deadlines.
    runtime.store.set_epoch_deadline(CALL_EPOCH_DEADLINE);
    Ok(())
}

fn routing_epoch_deadline_ticks(requested_states: u32) -> u64 {
    let bounded_states = u64::from(requested_states.clamp(100, 1_000_000));
    ROUTING_BASE_EPOCH_TICKS
        .saturating_add(bounded_states.saturating_mul(ROUTING_EXTRA_EPOCH_TICKS) / 1_000_000)
}

fn routing_epoch_deadline_minutes(requested_states: u32) -> u64 {
    let milliseconds = routing_epoch_deadline_ticks(requested_states)
        .saturating_mul(EPOCH_TICK.as_millis() as u64);
    milliseconds.saturating_add(60_000 - 1) / 60_000
}

fn prepare_routing_call(runtime: &mut Runtime, requested_states: u32) -> anyhow::Result<()> {
    // Route calculation is a declared bounded workload, unlike a toolbar or
    // lifecycle callback.  Scale deterministic execution fuel with the
    // retained-state budget exposed by the route request, while clamping to
    // the component's public limit.  Scale the independent wall-clock
    // backstop from thirty to at most sixty minutes as well: two-pass recovery
    // searches remain bounded, but a healthy difficult route should not be
    // aborted at the former seven-minute default. Cancellation remains
    // cooperative throughout all solver stages.
    let bounded_states = u64::from(requested_states.clamp(100, 1_000_000));
    let fuel = ROUTING_BASE_FUEL
        .saturating_add(bounded_states.saturating_mul(ROUTING_FUEL_PER_RETAINED_STATE))
        .min(ROUTING_MAX_FUEL);
    runtime.store.set_fuel(fuel)?;
    runtime
        .store
        .set_epoch_deadline(routing_epoch_deadline_ticks(requested_states));
    Ok(())
}

fn callback_error(operation: &str, code: i32) -> String {
    format!("host service {operation} failed with code {code}")
}

fn chart_segment_results(
    output: Vec<ChartSegmentResult>,
) -> Result<Vec<opencpn::portable::host::ChartSegmentResult>, String> {
    output
        .into_iter()
        .map(|result| {
            let (state, diagnostic) = match result.state {
                0 => (
                    opencpn::portable::host::ChartCoverageState::Covered,
                    if result.charts_considered == 0 {
                        "advisory coastline fallback found no intersection"
                    } else {
                        "authoritative chart safety accepted the segment"
                    },
                ),
                1 => (
                    opencpn::portable::host::ChartCoverageState::Unsafe,
                    match result.reason {
                        1 => "segment intersects chart LNDARE land",
                        2 => "segment intersects chart DRGARE/ITDARE drying area",
                        3 => "segment enters a DEPARE shallower than the vessel requirement",
                        _ => "segment intersects a host chart-safety exclusion",
                    },
                ),
                2 => (
                    opencpn::portable::host::ChartCoverageState::MissingCoverage,
                    if result.reason == 4 {
                        "chart coverage does not establish the required minimum depth"
                    } else {
                        "authoritative chart safety data is unavailable or incomplete"
                    },
                ),
                _ => (
                    opencpn::portable::host::ChartCoverageState::Unknown,
                    "chart safety could not be determined",
                ),
            };
            Ok(opencpn::portable::host::ChartSegmentResult {
                state,
                charts_considered: result.charts_considered,
                diagnostic: diagnostic.to_string(),
            })
        })
        .collect()
}

impl opencpn::portable::host::Host for HostState {
    fn log(&mut self, level: opencpn::portable::host::LogLevel, message: String) {
        let numeric_level = match level {
            opencpn::portable::host::LogLevel::Debug => 0,
            opencpn::portable::host::LogLevel::Info => 1,
            opencpn::portable::host::LogLevel::Warning => 2,
            opencpn::portable::host::LogLevel::Error => 3,
        };
        if let Some(callback) = self.callbacks.log {
            unsafe {
                callback(
                    self.callbacks.user_data,
                    numeric_level,
                    message.as_ptr().cast(),
                    message.len(),
                );
            }
        }
    }

    fn register_action(
        &mut self,
        action_id: String,
        label: String,
        tooltip: String,
        icon_resource: Option<String>,
    ) -> Result<u32, String> {
        let callback = self
            .callbacks
            .register_action
            .ok_or_else(|| "register-action service unavailable".to_string())?;
        let mut host_id = 0_u32;
        let (icon_ptr, icon_len) = icon_resource.as_ref().map_or((ptr::null(), 0), |value| {
            (value.as_ptr().cast(), value.len())
        });
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                action_id.as_ptr().cast(),
                action_id.len(),
                label.as_ptr().cast(),
                label.len(),
                tooltip.as_ptr().cast(),
                tooltip.len(),
                icon_ptr,
                icon_len,
                &mut host_id,
            )
        };
        (code == 0)
            .then_some(host_id)
            .ok_or_else(|| callback_error("register-action", code))
    }

    fn get_vessel_position(&mut self) -> Result<opencpn::portable::host::VesselPosition, String> {
        let callback = self
            .callbacks
            .get_vessel_position
            .ok_or_else(|| "get-vessel-position service unavailable".to_string())?;
        let mut latitude = 0.0;
        let mut longitude = 0.0;
        let mut cog = 0.0;
        let mut sog = 0.0;
        let mut has_cog = 0;
        let mut has_sog = 0;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                &mut latitude,
                &mut longitude,
                &mut cog,
                &mut has_cog,
                &mut sog,
                &mut has_sog,
            )
        };
        if code != 0 {
            return Err(callback_error("get-vessel-position", code));
        }
        Ok(opencpn::portable::host::VesselPosition {
            latitude,
            longitude,
            course_over_ground: (has_cog != 0).then_some(cog),
            speed_over_ground: (has_sog != 0).then_some(sog),
        })
    }

    fn setting_get(&mut self, key: String) -> Result<Option<String>, String> {
        let callback = self
            .callbacks
            .setting_get
            .ok_or_else(|| "setting-get service unavailable".to_string())?;
        let mut value = vec![0_u8; SETTINGS_VALUE_LIMIT];
        let mut value_len = 0_usize;
        let mut found = 0_u8;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                key.as_ptr().cast(),
                key.len(),
                value.as_mut_ptr().cast(),
                value.len(),
                &mut value_len,
                &mut found,
            )
        };
        if code != 0 {
            return Err(callback_error("setting-get", code));
        }
        if found == 0 {
            return Ok(None);
        }
        if value_len > value.len() {
            return Err("setting value exceeded host limit".to_string());
        }
        value.truncate(value_len);
        String::from_utf8(value)
            .map(Some)
            .map_err(|_| "host returned a non-UTF-8 setting".to_string())
    }

    fn setting_set(&mut self, key: String, value: String) -> Result<(), String> {
        let callback = self
            .callbacks
            .setting_set
            .ok_or_else(|| "setting-set service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                key.as_ptr().cast(),
                key.len(),
                value.as_ptr().cast(),
                value.len(),
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("setting-set", code))
    }

    fn submit_polyline(
        &mut self,
        scene_id: String,
        points: Vec<opencpn::portable::host::GeoPoint>,
        style: opencpn::portable::host::OverlayStyle,
    ) -> Result<(), String> {
        if points.len() > OVERLAY_POINT_LIMIT {
            return Err("overlay point limit exceeded".to_string());
        }
        let callback = self
            .callbacks
            .submit_polyline
            .ok_or_else(|| "submit-polyline service unavailable".to_string())?;
        let c_points: Vec<GeoPoint> = points
            .into_iter()
            .map(|point| GeoPoint {
                latitude: point.latitude,
                longitude: point.longitude,
            })
            .collect();
        let c_style = OverlayStyle {
            red: style.red,
            green: style.green,
            blue: style.blue,
            alpha: style.alpha,
            width_pixels: style.width_pixels,
        };
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                scene_id.as_ptr().cast(),
                scene_id.len(),
                c_points.as_ptr(),
                c_points.len(),
                c_style,
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("submit-polyline", code))
    }

    fn clear_scene(&mut self, scene_id: String) -> Result<(), String> {
        let callback = self
            .callbacks
            .clear_scene
            .ok_or_else(|| "clear-scene service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                scene_id.as_ptr().cast(),
                scene_id.len(),
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("clear-scene", code))
    }

    fn start_job(&mut self, job_id: String, work_units: u32) -> Result<(), String> {
        let callback = self
            .callbacks
            .start_job
            .ok_or_else(|| "start-job service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                job_id.as_ptr().cast(),
                job_id.len(),
                work_units,
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("start-job", code))
    }

    fn cancel_job(&mut self, job_id: String) -> Result<(), String> {
        let callback = self
            .callbacks
            .cancel_job
            .ok_or_else(|| "cancel-job service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                job_id.as_ptr().cast(),
                job_id.len(),
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("cancel-job", code))
    }

    fn open_surface(&mut self, surface_id: String) -> Result<(), String> {
        let callback = self
            .callbacks
            .open_surface
            .ok_or_else(|| "open-surface service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                surface_id.as_ptr().cast(),
                surface_id.len(),
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("open-surface", code))
    }

    fn open_environmental_viewer(&mut self) -> Result<(), String> {
        let callback = self
            .callbacks
            .open_environmental_viewer
            .ok_or_else(|| "open-environmental-viewer service unavailable".to_string())?;
        let code = unsafe { callback(self.callbacks.user_data) };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("open-environmental-viewer", code))
    }

    fn open_weather_routing(&mut self) -> Result<(), String> {
        let callback = self
            .callbacks
            .open_weather_routing
            .ok_or_else(|| "open-weather-routing service unavailable".to_string())?;
        let code = unsafe { callback(self.callbacks.user_data) };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("open-weather-routing", code))
    }

    fn environment_sample_batch(
        &mut self,
        requests: Vec<opencpn::portable::host::EnvironmentSampleRequest>,
    ) -> Result<Vec<opencpn::portable::host::EnvironmentSample>, String> {
        if requests.len() > ENVIRONMENT_SAMPLE_LIMIT {
            return Err("environment sample batch limit exceeded".to_string());
        }
        let callback = self
            .callbacks
            .environment_sample_batch
            .ok_or_else(|| "environment-sample-batch service unavailable".to_string())?;
        let input: Vec<EnvironmentSampleRequest> = requests
            .into_iter()
            .map(|request| EnvironmentSampleRequest {
                latitude: request.latitude,
                longitude: request.longitude,
                unix_time: request.unix_time,
            })
            .collect();
        let mut output = vec![EnvironmentSample::default(); input.len()];
        let mut error = vec![0_u8; ERROR_TEXT_LIMIT];
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                input.as_ptr(),
                input.len(),
                output.as_mut_ptr(),
                output.len(),
                error.as_mut_ptr().cast(),
                error.len(),
            )
        };
        if code != 0 {
            let error_len = error
                .iter()
                .position(|byte| *byte == 0)
                .unwrap_or(error.len());
            let detail = String::from_utf8_lossy(&error[..error_len]);
            return Err(if detail.is_empty() {
                callback_error("environment-sample-batch", code)
            } else {
                format!("environment provider failed: {detail}")
            });
        }
        Ok(output
            .into_iter()
            .map(|sample| opencpn::portable::host::EnvironmentSample {
                wind_u_knots: (sample.available & 1 != 0).then_some(sample.wind_u_knots),
                wind_v_knots: (sample.available & 1 != 0).then_some(sample.wind_v_knots),
                current_u_knots: (sample.available & 2 != 0).then_some(sample.current_u_knots),
                current_v_knots: (sample.available & 2 != 0).then_some(sample.current_v_knots),
                wave_height_metres: (sample.available & 4 != 0)
                    .then_some(sample.wave_height_metres),
            })
            .collect())
    }

    fn routing_progress(&mut self, percent: u8, message: String) {
        if let Some(callback) = self.callbacks.routing_progress {
            unsafe {
                callback(
                    self.callbacks.user_data,
                    percent,
                    message.as_ptr().cast(),
                    message.len(),
                )
            }
        }
    }

    fn routing_cancelled(&mut self) -> bool {
        self.callbacks
            .routing_cancelled
            .map(|callback| unsafe { callback(self.callbacks.user_data) != 0 })
            .unwrap_or(false)
    }

    fn charts_query_segments(
        &mut self,
        segments: Vec<opencpn::portable::host::GeoSegment>,
    ) -> Result<Vec<opencpn::portable::host::ChartSegmentResult>, String> {
        if segments.len() > CHART_SEGMENT_LIMIT {
            return Err("chart segment batch limit exceeded".to_string());
        }
        let callback = self
            .callbacks
            .charts_query_segments
            .ok_or_else(|| "charts-query-segments service unavailable".to_string())?;
        let input: Vec<GeoSegment> = segments
            .into_iter()
            .map(|segment| GeoSegment {
                start: GeoPoint {
                    latitude: segment.start.latitude,
                    longitude: segment.start.longitude,
                },
                end: GeoPoint {
                    latitude: segment.end.latitude,
                    longitude: segment.end.longitude,
                },
            })
            .collect();
        let mut output = vec![ChartSegmentResult::default(); input.len()];
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                input.as_ptr(),
                input.len(),
                output.as_mut_ptr(),
                output.len(),
            )
        };
        if code != 0 {
            return Err(callback_error("charts-query-segments", code));
        }
        chart_segment_results(output)
    }

    fn charts_query_final_safety(
        &mut self,
        segments: Vec<opencpn::portable::host::GeoSegment>,
        options: opencpn::portable::host::FinalChartSafetyOptions,
    ) -> Result<Vec<opencpn::portable::host::ChartSegmentResult>, String> {
        if segments.len() > CHART_SEGMENT_LIMIT {
            return Err("final chart-safety segment batch limit exceeded".to_string());
        }
        let callback = self
            .callbacks
            .charts_query_final_safety
            .ok_or_else(|| "final chart-safety service unavailable".to_string())?;
        let input: Vec<GeoSegment> = segments
            .into_iter()
            .map(|segment| GeoSegment {
                start: GeoPoint {
                    latitude: segment.start.latitude,
                    longitude: segment.start.longitude,
                },
                end: GeoPoint {
                    latitude: segment.end.latitude,
                    longitude: segment.end.longitude,
                },
            })
            .collect();
        let options = FinalChartSafetyOptions {
            safety_margin_nautical_miles: options.safety_margin_nautical_miles,
            minimum_depth_metres: options.minimum_depth_metres,
            require_authoritative: u8::from(options.require_authoritative),
        };
        let mut output = vec![ChartSegmentResult::default(); input.len()];
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                input.as_ptr(),
                input.len(),
                &options,
                output.as_mut_ptr(),
                output.len(),
            )
        };
        if code != 0 {
            return Err(callback_error("charts-query-final-safety", code));
        }
        chart_segment_results(output)
    }

    fn network_get_to_private(
        &mut self,
        request_id: String,
        url: String,
        private_name: String,
        max_bytes: u64,
    ) -> Result<(), String> {
        let callback = self
            .callbacks
            .network_get_to_private
            .ok_or_else(|| "network-get-to-private service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                request_id.as_ptr().cast(),
                request_id.len(),
                url.as_ptr().cast(),
                url.len(),
                private_name.as_ptr().cast(),
                private_name.len(),
                max_bytes,
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("network-get-to-private", code))
    }

    fn storage_private_read(&mut self, private_name: String) -> Result<Vec<u8>, String> {
        let callback = self
            .callbacks
            .storage_private_read
            .ok_or_else(|| "storage-private-read service unavailable".to_string())?;
        let mut value = vec![0_u8; PRIVATE_READ_LIMIT];
        let mut value_len = 0_usize;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                private_name.as_ptr().cast(),
                private_name.len(),
                value.as_mut_ptr(),
                value.len(),
                &mut value_len,
            )
        };
        if code != 0 || value_len > value.len() {
            return Err(callback_error("storage-private-read", code));
        }
        value.truncate(value_len);
        Ok(value)
    }

    fn user_file_read(&mut self, grant_token: String) -> Result<Vec<u8>, String> {
        let callback = self
            .callbacks
            .user_file_read
            .ok_or_else(|| "user-file-read service unavailable".to_string())?;
        let mut value = vec![0_u8; USER_FILE_LIMIT];
        let mut value_len = 0_usize;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                grant_token.as_ptr().cast(),
                grant_token.len(),
                value.as_mut_ptr(),
                value.len(),
                &mut value_len,
            )
        };
        if code != 0 || value_len > value.len() {
            return Err(callback_error("user-file-read", code));
        }
        value.truncate(value_len);
        Ok(value)
    }

    fn user_file_write(&mut self, grant_token: String, value: Vec<u8>) -> Result<(), String> {
        if value.len() > USER_FILE_LIMIT {
            return Err("user file size limit exceeded".to_string());
        }
        let callback = self
            .callbacks
            .user_file_write
            .ok_or_else(|| "user-file-write service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                grant_token.as_ptr().cast(),
                grant_token.len(),
                value.as_ptr(),
                value.len(),
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("user-file-write", code))
    }

    fn send_plugin_message(
        &mut self,
        message_id: String,
        message_body: String,
    ) -> Result<(), String> {
        if message_id.is_empty() || message_id.len() > PLUGIN_MESSAGE_ID_LIMIT {
            return Err("plugin message id is empty or exceeds policy".to_string());
        }
        if message_body.len() > PLUGIN_MESSAGE_BODY_LIMIT {
            return Err("plugin message body exceeds policy".to_string());
        }
        let callback = self
            .callbacks
            .send_plugin_message
            .ok_or_else(|| "send-plugin-message service unavailable".to_string())?;
        let code = unsafe {
            callback(
                self.callbacks.user_data,
                message_id.as_ptr().cast(),
                message_id.len(),
                message_body.as_ptr().cast(),
                message_body.len(),
            )
        };
        (code == 0)
            .then_some(())
            .ok_or_else(|| callback_error("send-plugin-message", code))
    }
}

use api_v02_imports::opencpn::portable::types as v02_types;

fn v02_error(code: &str, message: impl Into<String>, retryable: bool) -> v02_types::ServiceError {
    v02_types::ServiceError {
        code: code.to_string(),
        message: message.into(),
        retryable,
    }
}

fn v02_host_error(message: String) -> v02_types::ServiceError {
    v02_error("host-service-failed", message, false)
}

impl api_v02_imports::opencpn::portable::types::Host for HostState {}

impl api_v02_imports::opencpn::portable::diagnostics::Host for HostState {
    fn log(&mut self, level: v02_types::LogLevel, message: String) {
        let level = match level {
            v02_types::LogLevel::Debug => opencpn::portable::host::LogLevel::Debug,
            v02_types::LogLevel::Info => opencpn::portable::host::LogLevel::Info,
            v02_types::LogLevel::Warning => opencpn::portable::host::LogLevel::Warning,
            v02_types::LogLevel::Error => opencpn::portable::host::LogLevel::Error,
        };
        <Self as opencpn::portable::host::Host>::log(self, level, message);
    }
}

impl api_v02_imports::opencpn::portable::actions::Host for HostState {
    fn register(
        &mut self,
        action_id: String,
        label: String,
        tooltip: String,
        icon_resource: Option<String>,
    ) -> Result<u32, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::register_action(
            self,
            action_id,
            label,
            tooltip,
            icon_resource,
        )
        .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::navigation::Host for HostState {
    fn get_vessel_position(
        &mut self,
    ) -> Result<v02_types::VesselPosition, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::get_vessel_position(self)
            .map(|position| v02_types::VesselPosition {
                latitude: position.latitude,
                longitude: position.longitude,
                course_over_ground: position.course_over_ground,
                speed_over_ground: position.speed_over_ground,
            })
            .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::settings::Host for HostState {
    fn get(&mut self, key: String) -> Result<Option<String>, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::setting_get(self, key).map_err(v02_host_error)
    }

    fn set(&mut self, key: String, value: String) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::setting_set(self, key, value)
            .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::scenes::Host for HostState {
    fn submit_polyline(
        &mut self,
        scene_id: String,
        points: Vec<v02_types::GeoPoint>,
        style: v02_types::OverlayStyle,
    ) -> Result<(), v02_types::ServiceError> {
        let points = points
            .into_iter()
            .map(|point| opencpn::portable::host::GeoPoint {
                latitude: point.latitude,
                longitude: point.longitude,
            })
            .collect();
        let style = opencpn::portable::host::OverlayStyle {
            red: style.red,
            green: style.green,
            blue: style.blue,
            alpha: style.alpha,
            width_pixels: style.width_pixels,
        };
        <Self as opencpn::portable::host::Host>::submit_polyline(self, scene_id, points, style)
            .map_err(v02_host_error)
    }

    fn clear(&mut self, scene_id: String) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::clear_scene(self, scene_id).map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::jobs::Host for HostState {
    fn start(&mut self, job_id: String, work_units: u32) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::start_job(self, job_id, work_units)
            .map_err(v02_host_error)
    }

    fn cancel(&mut self, job_id: String) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::cancel_job(self, job_id).map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::surfaces::Host for HostState {
    fn open(&mut self, surface_id: String) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::open_surface(self, surface_id)
            .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::environment::Host for HostState {
    fn sample_batch(
        &mut self,
        requests: Vec<v02_types::EnvironmentSampleRequest>,
    ) -> Result<Vec<v02_types::EnvironmentSample>, v02_types::ServiceError> {
        let requests = requests
            .into_iter()
            .map(
                |request| opencpn::portable::host::EnvironmentSampleRequest {
                    latitude: request.latitude,
                    longitude: request.longitude,
                    unix_time: request.unix_time,
                },
            )
            .collect();
        <Self as opencpn::portable::host::Host>::environment_sample_batch(self, requests)
            .map(|samples| {
                samples
                    .into_iter()
                    .map(|sample| v02_types::EnvironmentSample {
                        wind_u_knots: sample.wind_u_knots,
                        wind_v_knots: sample.wind_v_knots,
                        current_u_knots: sample.current_u_knots,
                        current_v_knots: sample.current_v_knots,
                        wave_height_metres: sample.wave_height_metres,
                    })
                    .collect()
            })
            .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::routing_control::Host for HostState {
    fn progress(&mut self, percent: u8, message: String) {
        <Self as opencpn::portable::host::Host>::routing_progress(self, percent, message);
    }

    fn cancelled(&mut self) -> bool {
        <Self as opencpn::portable::host::Host>::routing_cancelled(self)
    }
}

fn v02_geo_segments(
    segments: Vec<v02_types::GeoSegment>,
) -> Vec<opencpn::portable::host::GeoSegment> {
    segments
        .into_iter()
        .map(|segment| opencpn::portable::host::GeoSegment {
            start: opencpn::portable::host::GeoPoint {
                latitude: segment.start.latitude,
                longitude: segment.start.longitude,
            },
            end: opencpn::portable::host::GeoPoint {
                latitude: segment.end.latitude,
                longitude: segment.end.longitude,
            },
        })
        .collect()
}

fn v02_chart_results(
    results: Vec<opencpn::portable::host::ChartSegmentResult>,
) -> Vec<v02_types::ChartSegmentResult> {
    results
        .into_iter()
        .map(|result| v02_types::ChartSegmentResult {
            state: match result.state {
                opencpn::portable::host::ChartCoverageState::Covered => {
                    v02_types::ChartCoverageState::Covered
                }
                opencpn::portable::host::ChartCoverageState::Unsafe => {
                    v02_types::ChartCoverageState::Unsafe
                }
                opencpn::portable::host::ChartCoverageState::MissingCoverage => {
                    v02_types::ChartCoverageState::MissingCoverage
                }
                opencpn::portable::host::ChartCoverageState::Unknown => {
                    v02_types::ChartCoverageState::Unknown
                }
            },
            charts_considered: result.charts_considered,
            diagnostic: result.diagnostic,
        })
        .collect()
}

impl api_v02_imports::opencpn::portable::chart_safety::Host for HostState {
    fn query_segments(
        &mut self,
        segments: Vec<v02_types::GeoSegment>,
    ) -> Result<Vec<v02_types::ChartSegmentResult>, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::charts_query_segments(
            self,
            v02_geo_segments(segments),
        )
        .map(v02_chart_results)
        .map_err(v02_host_error)
    }

    fn query_final_safety(
        &mut self,
        segments: Vec<v02_types::GeoSegment>,
        options: v02_types::FinalChartSafetyOptions,
    ) -> Result<Vec<v02_types::ChartSegmentResult>, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::charts_query_final_safety(
            self,
            v02_geo_segments(segments),
            opencpn::portable::host::FinalChartSafetyOptions {
                safety_margin_nautical_miles: options.safety_margin_nautical_miles,
                minimum_depth_metres: options.minimum_depth_metres,
                require_authoritative: options.require_authoritative,
            },
        )
        .map(v02_chart_results)
        .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::network::Host for HostState {
    fn get_to_private(
        &mut self,
        request_id: String,
        url: String,
        private_name: String,
        max_bytes: u64,
    ) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::network_get_to_private(
            self,
            request_id,
            url,
            private_name,
            max_bytes,
        )
        .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::storage::Host for HostState {
    fn private_read(&mut self, private_name: String) -> Result<Vec<u8>, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::storage_private_read(self, private_name)
            .map_err(v02_host_error)
    }

    fn user_file_read(&mut self, grant_token: String) -> Result<Vec<u8>, v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::user_file_read(self, grant_token)
            .map_err(v02_host_error)
    }

    fn user_file_write(
        &mut self,
        grant_token: String,
        value: Vec<u8>,
    ) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::user_file_write(self, grant_token, value)
            .map_err(v02_host_error)
    }
}

impl api_v02_imports::opencpn::portable::plugin_messages::Host for HostState {
    fn send(
        &mut self,
        message_id: String,
        message_body: String,
    ) -> Result<(), v02_types::ServiceError> {
        <Self as opencpn::portable::host::Host>::send_plugin_message(self, message_id, message_body)
            .map_err(v02_host_error)
    }
}

fn write_error(error: *mut c_char, capacity: usize, message: &str) {
    if error.is_null() || capacity == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let length = bytes.len().min(capacity.saturating_sub(1));
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), error.cast(), length);
        *error.add(length) = 0;
    }
}

fn ffi_result<T>(result: anyhow::Result<T>, error: *mut c_char, error_capacity: usize) -> i32 {
    match result {
        Ok(_) => 0,
        Err(err) => {
            let message = format!("{err:#}");
            write_error(error, error_capacity.min(ERROR_TEXT_LIMIT), &message);
            -1
        }
    }
}

fn routing_error_message(error: &anyhow::Error, requested_states: u32) -> String {
    match error.downcast_ref::<Trap>() {
        Some(Trap::Interrupt) => format!(
            "weather routing exceeded its bounded {}-minute runtime deadline while exploring a difficult route; the calculation was safely interrupted. Retry with fewer departure alternatives or workers, or reduce the search limits",
            routing_epoch_deadline_minutes(requested_states)
        ),
        Some(Trap::OutOfFuel) => format!(
            "weather routing exhausted its bounded computation budget for {} retained states; the calculation was safely interrupted. Retry with fewer departure alternatives or a smaller search, or increase the state limit",
            requested_states.clamp(100, 1_000_000)
        ),
        _ => format!("{error:#}"),
    }
}

fn ffi_routing_result<T>(
    result: anyhow::Result<T>,
    requested_states: u32,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    match result {
        Ok(_) => 0,
        Err(err) => {
            let message = routing_error_message(&err, requested_states);
            write_error(error, error_capacity.min(ERROR_TEXT_LIMIT), &message);
            -1
        }
    }
}

fn input_string(ptr: *const c_char, len: usize) -> anyhow::Result<String> {
    if ptr.is_null() && len != 0 {
        anyhow::bail!("null string pointer with non-zero length");
    }
    let bytes = if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(ptr.cast::<u8>(), len) }
    };
    Ok(std::str::from_utf8(bytes)?.to_string())
}

fn input_doubles(ptr: *const f64, len: usize) -> anyhow::Result<Vec<f64>> {
    if ptr.is_null() && len != 0 {
        anyhow::bail!("null numeric pointer with non-zero length");
    }
    Ok(if len == 0 {
        Vec::new()
    } else {
        unsafe { slice::from_raw_parts(ptr, len) }.to_vec()
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_create(
    component_path: *const c_char,
    callbacks: *const HostCallbacks,
    portable_api: u32,
    portable_world: u32,
    error: *mut c_char,
    error_capacity: usize,
) -> *mut Runtime {
    let result = (|| -> anyhow::Result<Runtime> {
        if component_path.is_null() || callbacks.is_null() {
            anyhow::bail!("component path and callbacks are required");
        }
        let callbacks = unsafe { *callbacks };
        if callbacks.abi_version != HOST_ABI_VERSION {
            anyhow::bail!(
                "unsupported host callback ABI {}, expected {}",
                callbacks.abi_version,
                HOST_ABI_VERSION
            );
        }
        let api_kind = match (portable_api, portable_world) {
            (PORTABLE_API_V01, PORTABLE_WORLD_PLUGIN) => ApiKind::V01,
            (PORTABLE_API_V02, PORTABLE_WORLD_PLUGIN) => ApiKind::V02,
            (PORTABLE_API_V02, PORTABLE_WORLD_WEATHER_ROUTING) => ApiKind::V02WeatherRouting,
            _ => anyhow::bail!(
                "unsupported portable API/world combination {portable_api}/{portable_world}"
            ),
        };
        let path = unsafe { CStr::from_ptr(component_path) }.to_str()?;
        let bytes = fs::read(Path::new(path))?;

        let mut config = Config::new();
        config.wasm_component_model(true);
        config.consume_fuel(true);
        config.epoch_interruption(true);
        config.cranelift_nan_canonicalization(true);
        let engine = Engine::new(&config)?;
        let component = Component::new(&engine, bytes)?;
        let epoch_ticker_stop = Arc::new(AtomicBool::new(false));
        let ticker_stop = Arc::clone(&epoch_ticker_stop);
        let ticker_engine = engine.clone();
        let epoch_ticker = thread::Builder::new()
            .name("ocpn-portable-epoch".to_string())
            .spawn(move || {
                while !ticker_stop.load(Ordering::Acquire) {
                    thread::sleep(EPOCH_TICK);
                    ticker_engine.increment_epoch();
                }
            })?;
        let epoch_ticker = Arc::new(EpochTicker {
            stop: epoch_ticker_stop,
            thread: Some(epoch_ticker),
        });
        instantiate_runtime(engine, component, callbacks, api_kind, epoch_ticker)
    })();

    match result {
        Ok(runtime) => Box::into_raw(Box::new(runtime)),
        Err(err) => {
            write_error(error, error_capacity, &format!("{err:#}"));
            ptr::null_mut()
        }
    }
}

/// Create a fresh, isolated Store for a long-running compute export while
/// sharing the compiled Component, Engine epoch clock and host capabilities.
/// The replica deliberately does not execute plugin lifecycle methods: those
/// methods can register UI and must remain confined to the primary instance.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_clone_compute(
    runtime: *const Runtime,
    error: *mut c_char,
    error_capacity: usize,
) -> *mut Runtime {
    let Some(runtime) = (unsafe { runtime.as_ref() }) else {
        write_error(error, error_capacity, "runtime is null");
        return ptr::null_mut();
    };
    match instantiate_runtime(
        runtime.engine.clone(),
        runtime.component.clone(),
        runtime.callbacks,
        runtime.api_kind,
        Arc::clone(&runtime._epoch_ticker),
    ) {
        Ok(replica) => Box::into_raw(Box::new(replica)),
        Err(err) => {
            write_error(error, error_capacity, &format!("{err:#}"));
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_destroy(runtime: *mut Runtime) {
    if !runtime.is_null() {
        drop(unsafe { Box::from_raw(runtime) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_initialize(
    runtime: *mut Runtime,
    expected_id: *const c_char,
    expected_id_len: usize,
    expected_name: *const c_char,
    expected_name_len: usize,
    expected_version: *const c_char,
    expected_version_len: usize,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        let expected_id = input_string(expected_id, expected_id_len)?;
        let expected_name = input_string(expected_name, expected_name_len)?;
        let expected_version = input_string(expected_version, expected_version_len)?;
        let (id, name, version) = match &runtime.bindings {
            RuntimeBindings::V01(bindings) => {
                let info = bindings
                    .opencpn_portable_plugin()
                    .call_initialize(&mut runtime.store)?
                    .map_err(anyhow::Error::msg)?;
                (info.id, info.name, info.version)
            }
            RuntimeBindings::V02(bindings) => {
                let info = bindings
                    .opencpn_portable_lifecycle()
                    .call_initialize(&mut runtime.store)?
                    .map_err(v02_guest_error)?;
                (info.id, info.name, info.version)
            }
            RuntimeBindings::V02WeatherRouting(bindings) => {
                let info = bindings
                    .opencpn_portable_lifecycle()
                    .call_initialize(&mut runtime.store)?
                    .map_err(v02_guest_error)?;
                (info.id, info.name, info.version)
            }
        };
        if id != expected_id || name != expected_name || version != expected_version {
            anyhow::bail!(
                "component identity mismatch: manifest ({expected_id}, {expected_name}, {expected_version}), component ({}, {}, {})",
                id,
                name,
                version
            );
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_enable(
    runtime: *mut Runtime,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => bindings
                .opencpn_portable_plugin()
                .call_enable(&mut runtime.store)?
                .map_err(anyhow::Error::msg)?,
            RuntimeBindings::V02(bindings) => bindings
                .opencpn_portable_lifecycle()
                .call_enable(&mut runtime.store)?
                .map_err(v02_guest_error)?,
            RuntimeBindings::V02WeatherRouting(bindings) => bindings
                .opencpn_portable_lifecycle()
                .call_enable(&mut runtime.store)?
                .map_err(v02_guest_error)?,
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_disable(
    runtime: *mut Runtime,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => bindings
                .opencpn_portable_plugin()
                .call_disable(&mut runtime.store)?,
            RuntimeBindings::V02(bindings) => bindings
                .opencpn_portable_lifecycle()
                .call_disable(&mut runtime.store)?,
            RuntimeBindings::V02WeatherRouting(bindings) => bindings
                .opencpn_portable_lifecycle()
                .call_disable(&mut runtime.store)?,
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_on_action(
    runtime: *mut Runtime,
    action_id: *const c_char,
    action_id_len: usize,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        let action_id = input_string(action_id, action_id_len)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => bindings
                .opencpn_portable_plugin()
                .call_on_action(&mut runtime.store, &action_id)?
                .map_err(anyhow::Error::msg)?,
            RuntimeBindings::V02(bindings) => bindings
                .opencpn_portable_lifecycle()
                .call_on_action(&mut runtime.store, &action_id)?
                .map_err(v02_guest_error)?,
            RuntimeBindings::V02WeatherRouting(bindings) => bindings
                .opencpn_portable_lifecycle()
                .call_on_action(&mut runtime.store, &action_id)?
                .map_err(v02_guest_error)?,
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_on_surface_event(
    runtime: *mut Runtime,
    surface_id: *const c_char,
    surface_id_len: usize,
    control_id: *const c_char,
    control_id_len: usize,
    value_json: *const c_char,
    value_json_len: usize,
    state_json: *mut c_char,
    state_json_capacity: usize,
    state_json_len: *mut usize,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        let surface_id = input_string(surface_id, surface_id_len)?;
        let control_id = input_string(control_id, control_id_len)?;
        let value_json = input_string(value_json, value_json_len)?;
        if value_json.len() > SETTINGS_VALUE_LIMIT {
            anyhow::bail!("surface event value exceeds the 64 KiB limit");
        }
        let state = match &runtime.bindings {
            RuntimeBindings::V01(bindings) => bindings
                .opencpn_portable_plugin()
                .call_on_surface_event(&mut runtime.store, &surface_id, &control_id, &value_json)?
                .map_err(anyhow::Error::msg)?,
            RuntimeBindings::V02(bindings) => bindings
                .opencpn_portable_surface_event_sink()
                .call_on_surface_event(&mut runtime.store, &surface_id, &control_id, &value_json)?
                .map_err(v02_guest_error)?,
            RuntimeBindings::V02WeatherRouting(bindings) => bindings
                .opencpn_portable_surface_event_sink()
                .call_on_surface_event(&mut runtime.store, &surface_id, &control_id, &value_json)?
                .map_err(v02_guest_error)?,
        };
        if state.len() > SETTINGS_VALUE_LIMIT {
            anyhow::bail!("surface event state exceeds the 64 KiB limit");
        }
        if state_json_len.is_null() {
            anyhow::bail!("surface state length output is null");
        }
        unsafe { *state_json_len = state.len() };
        if state.len() > state_json_capacity || (!state.is_empty() && state_json.is_null()) {
            anyhow::bail!("surface state output capacity is too small");
        }
        if !state.is_empty() {
            unsafe {
                ptr::copy_nonoverlapping(state.as_ptr(), state_json.cast(), state.len());
            }
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_on_job_event(
    runtime: *mut Runtime,
    job_id: *const c_char,
    job_id_len: usize,
    event_kind: u32,
    progress: u8,
    message: *const c_char,
    message_len: usize,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        let job_id = input_string(job_id, job_id_len)?;
        let failed_message = (event_kind == 3)
            .then(|| input_string(message, message_len))
            .transpose()?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => {
                let event = match event_kind {
                    0 => JobEvent::Progress(progress),
                    1 => JobEvent::Completed,
                    2 => JobEvent::Cancelled,
                    3 => JobEvent::Failed(failed_message.unwrap_or_default()),
                    _ => anyhow::bail!("invalid job event kind {event_kind}"),
                };
                bindings.opencpn_portable_plugin().call_on_job_event(
                    &mut runtime.store,
                    &job_id,
                    &event,
                )?;
            }
            RuntimeBindings::V02(bindings) => {
                let event = match event_kind {
                    0 => v02_types::JobEvent::Progress(progress),
                    1 => v02_types::JobEvent::Completed,
                    2 => v02_types::JobEvent::Cancelled,
                    3 => v02_types::JobEvent::Failed(failed_message.unwrap_or_default()),
                    _ => anyhow::bail!("invalid job event kind {event_kind}"),
                };
                bindings
                    .opencpn_portable_job_event_sink()
                    .call_on_job_event(&mut runtime.store, &job_id, &event)?;
            }
            RuntimeBindings::V02WeatherRouting(bindings) => {
                let event = match event_kind {
                    0 => v02_types::JobEvent::Progress(progress),
                    1 => v02_types::JobEvent::Completed,
                    2 => v02_types::JobEvent::Cancelled,
                    3 => v02_types::JobEvent::Failed(failed_message.unwrap_or_default()),
                    _ => anyhow::bail!("invalid job event kind {event_kind}"),
                };
                bindings
                    .opencpn_portable_job_event_sink()
                    .call_on_job_event(&mut runtime.store, &job_id, &event)?;
            }
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_on_event(
    runtime: *mut Runtime,
    event_kind: u32,
    topic: *const c_char,
    topic_len: usize,
    payload: *const c_char,
    payload_len: usize,
    sequence: u64,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        if topic_len > PLUGIN_MESSAGE_ID_LIMIT {
            anyhow::bail!("event topic exceeds policy");
        }
        if payload_len > PLUGIN_MESSAGE_BODY_LIMIT {
            anyhow::bail!("event payload exceeds policy");
        }
        let topic = input_string(topic, topic_len)?;
        let payload = input_string(payload, payload_len)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => match event_kind {
                0 => {
                    if payload.is_empty() || payload.len() > NAVIGATION_SENTENCE_LIMIT {
                        anyhow::bail!("navigation sentence is empty or exceeds 1024 bytes");
                    }
                    bindings
                        .opencpn_portable_plugin()
                        .call_on_navigation_sentence(&mut runtime.store, &payload)?;
                }
                8 => {
                    bindings
                        .opencpn_portable_plugin()
                        .call_on_surface_event(
                            &mut runtime.store,
                            "host.plugin-message",
                            &topic,
                            &payload,
                        )?
                        .map_err(anyhow::Error::msg)?;
                }
                _ => {}
            },
            RuntimeBindings::V02(bindings) => {
                if event_kind == 8 {
                    bindings
                        .opencpn_portable_plugin_message_sink()
                        .call_on_plugin_message(&mut runtime.store, &topic, &payload)?
                        .map_err(v02_guest_error)?;
                    return Ok(());
                }
                let kind = match event_kind {
                    0 => v02_types::EventKind::Nmea0183,
                    1 => v02_types::EventKind::Nmea2000,
                    2 => v02_types::EventKind::SignalK,
                    3 => v02_types::EventKind::NavigationPosition,
                    4 => v02_types::EventKind::AisTarget,
                    5 => v02_types::EventKind::ActiveLeg,
                    6 => v02_types::EventKind::Cursor,
                    7 => v02_types::EventKind::Viewport,
                    _ => anyhow::bail!("invalid event kind {event_kind}"),
                };
                bindings
                    .opencpn_portable_event_sink()
                    .call_on_event(
                        &mut runtime.store,
                        &v02_types::Event {
                            kind,
                            topic,
                            payload,
                            sequence,
                        },
                    )?
                    .map_err(v02_guest_error)?;
            }
            RuntimeBindings::V02WeatherRouting(bindings) => {
                if event_kind == 8 {
                    bindings
                        .opencpn_portable_plugin_message_sink()
                        .call_on_plugin_message(&mut runtime.store, &topic, &payload)?
                        .map_err(v02_guest_error)?;
                    return Ok(());
                }
                let kind = match event_kind {
                    0 => v02_types::EventKind::Nmea0183,
                    1 => v02_types::EventKind::Nmea2000,
                    2 => v02_types::EventKind::SignalK,
                    3 => v02_types::EventKind::NavigationPosition,
                    4 => v02_types::EventKind::AisTarget,
                    5 => v02_types::EventKind::ActiveLeg,
                    6 => v02_types::EventKind::Cursor,
                    7 => v02_types::EventKind::Viewport,
                    _ => anyhow::bail!("invalid event kind {event_kind}"),
                };
                bindings
                    .opencpn_portable_event_sink()
                    .call_on_event(
                        &mut runtime.store,
                        &v02_types::Event {
                            kind,
                            topic,
                            payload,
                            sequence,
                        },
                    )?
                    .map_err(v02_guest_error)?;
            }
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_on_navigation_sentence(
    runtime: *mut Runtime,
    sentence: *const c_char,
    sentence_len: usize,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        if sentence_len == 0 || sentence_len > NAVIGATION_SENTENCE_LIMIT {
            anyhow::bail!("navigation sentence is empty or exceeds 1024 bytes");
        }
        let sentence = input_string(sentence, sentence_len)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => bindings
                .opencpn_portable_plugin()
                .call_on_navigation_sentence(&mut runtime.store, &sentence)?,
            RuntimeBindings::V02(bindings) => {
                let event = v02_types::Event {
                    kind: v02_types::EventKind::Nmea0183,
                    topic: String::new(),
                    payload: sentence,
                    sequence: 0,
                };
                bindings
                    .opencpn_portable_event_sink()
                    .call_on_event(&mut runtime.store, &event)?
                    .map_err(v02_guest_error)?;
            }
            RuntimeBindings::V02WeatherRouting(bindings) => {
                let event = v02_types::Event {
                    kind: v02_types::EventKind::Nmea0183,
                    topic: String::new(),
                    payload: sentence,
                    sequence: 0,
                };
                bindings
                    .opencpn_portable_event_sink()
                    .call_on_event(&mut runtime.store, &event)?
                    .map_err(v02_guest_error)?;
            }
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_on_plugin_message(
    runtime: *mut Runtime,
    message_id: *const c_char,
    message_id_len: usize,
    message_body: *const c_char,
    message_body_len: usize,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        if message_id_len == 0 || message_id_len > PLUGIN_MESSAGE_ID_LIMIT {
            anyhow::bail!("plugin message id is empty or exceeds policy");
        }
        if message_body_len > PLUGIN_MESSAGE_BODY_LIMIT {
            anyhow::bail!("plugin message body exceeds policy");
        }
        let message_id = input_string(message_id, message_id_len)?;
        let message_body = input_string(message_body, message_body_len)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => {
                bindings
                    .opencpn_portable_plugin()
                    .call_on_surface_event(
                        &mut runtime.store,
                        "host.plugin-message",
                        &message_id,
                        &message_body,
                    )?
                    .map_err(anyhow::Error::msg)?;
            }
            RuntimeBindings::V02(bindings) => bindings
                .opencpn_portable_plugin_message_sink()
                .call_on_plugin_message(&mut runtime.store, &message_id, &message_body)?
                .map_err(v02_guest_error)?,
            RuntimeBindings::V02WeatherRouting(bindings) => bindings
                .opencpn_portable_plugin_message_sink()
                .call_on_plugin_message(&mut runtime.store, &message_id, &message_body)?
                .map_err(v02_guest_error)?,
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

struct NormalizedPolarGrid {
    identity: String,
    true_wind_speeds_knots: Vec<f64>,
    true_wind_angles_degrees: Vec<f64>,
    boat_speeds_knots: Vec<f64>,
}

struct NormalizedRoutePoint {
    latitude: f64,
    longitude: f64,
    unix_time: i64,
}

struct NormalizedRouteLine {
    unix_time: i64,
    points: Vec<NormalizedRoutePoint>,
}

struct NormalizedRouteEnvironmentPoint {
    latitude: f64,
    longitude: f64,
    unix_time: i64,
    wind_u_knots: f64,
    wind_v_knots: f64,
    current_u_knots: Option<f64>,
    current_v_knots: Option<f64>,
    wave_height_metres: Option<f64>,
}

struct NormalizedRoute {
    points: Vec<NormalizedRoutePoint>,
    isochrones: Vec<NormalizedRouteLine>,
    traces: Vec<NormalizedRouteLine>,
    route_environment: Vec<NormalizedRouteEnvironmentPoint>,
    distance_nautical_miles: f64,
    duration_seconds: u64,
    states_examined: u32,
    average_speed_knots: f64,
    maximum_speed_knots: f64,
    average_sog_knots: f64,
    maximum_sog_knots: f64,
    average_wind_knots: f64,
    maximum_wind_knots: f64,
    average_current_knots: Option<f64>,
    maximum_current_knots: Option<f64>,
    tacks: u32,
    motor_seconds: u64,
    estimated_fuel_litres: Option<f64>,
    propulsion_transitions: u32,
    comfort_level: u8,
    diagnostic: String,
}

macro_rules! normalize_route {
    ($route:expr) => {{
        let route = $route;
        NormalizedRoute {
            points: route
                .points
                .into_iter()
                .map(|point| NormalizedRoutePoint {
                    latitude: point.latitude,
                    longitude: point.longitude,
                    unix_time: point.unix_time,
                })
                .collect(),
            isochrones: route
                .isochrones
                .into_iter()
                .map(|line| NormalizedRouteLine {
                    unix_time: line.unix_time,
                    points: line
                        .points
                        .into_iter()
                        .map(|point| NormalizedRoutePoint {
                            latitude: point.latitude,
                            longitude: point.longitude,
                            unix_time: point.unix_time,
                        })
                        .collect(),
                })
                .collect(),
            traces: route
                .traces
                .into_iter()
                .map(|line| NormalizedRouteLine {
                    unix_time: line.unix_time,
                    points: line
                        .points
                        .into_iter()
                        .map(|point| NormalizedRoutePoint {
                            latitude: point.latitude,
                            longitude: point.longitude,
                            unix_time: point.unix_time,
                        })
                        .collect(),
                })
                .collect(),
            route_environment: route
                .route_environment
                .into_iter()
                .map(|point| NormalizedRouteEnvironmentPoint {
                    latitude: point.latitude,
                    longitude: point.longitude,
                    unix_time: point.unix_time,
                    wind_u_knots: point.wind_u_knots,
                    wind_v_knots: point.wind_v_knots,
                    current_u_knots: point.current_u_knots,
                    current_v_knots: point.current_v_knots,
                    wave_height_metres: point.wave_height_metres,
                })
                .collect(),
            distance_nautical_miles: route.distance_nautical_miles,
            duration_seconds: route.duration_seconds,
            states_examined: route.states_examined,
            average_speed_knots: route.average_speed_knots,
            maximum_speed_knots: route.maximum_speed_knots,
            average_sog_knots: route.average_sog_knots,
            maximum_sog_knots: route.maximum_sog_knots,
            average_wind_knots: route.average_wind_knots,
            maximum_wind_knots: route.maximum_wind_knots,
            average_current_knots: route.average_current_knots,
            maximum_current_knots: route.maximum_current_knots,
            tacks: route.tacks,
            motor_seconds: route.motor_seconds,
            estimated_fuel_litres: route.estimated_fuel_litres,
            propulsion_transitions: route.propulsion_transitions,
            comfort_level: route.comfort_level,
            diagnostic: route.diagnostic,
        }
    }};
}

macro_rules! build_route_request {
    ($request_type:ident, $request:expr, $polars:expr $(, $extra_name:ident : $extra_value:expr)*) => {
        $request_type {
            start_latitude: $request.start_latitude,
            start_longitude: $request.start_longitude,
            destination_latitude: $request.destination_latitude,
            destination_longitude: $request.destination_longitude,
            departure_unix_time: $request.departure_unix_time,
            polars: $polars,
            time_step_seconds: $request.time_step_seconds,
            heading_step_degrees: $request.heading_step_degrees,
            refined_heading_step_degrees: $request.refined_heading_step_degrees,
            adaptive_headings: $request.adaptive_headings != 0,
            spatial_cell_nautical_miles: $request.spatial_cell_nautical_miles,
            labels_per_cell: $request.labels_per_cell,
            max_hours: $request.max_hours,
            max_states: $request.max_states,
            avoid_unsafe_charts: $request.avoid_unsafe_charts != 0,
            min_true_wind_angle_degrees: $request.min_true_wind_angle_degrees,
            max_true_wind_angle_degrees: $request.max_true_wind_angle_degrees,
            max_wind_knots: ($request.limits_available & 1 != 0).then_some($request.max_wind_knots),
            max_apparent_wind_knots: ($request.limits_available & 4 != 0)
                .then_some($request.max_apparent_wind_knots),
            max_wave_metres: ($request.limits_available & 2 != 0)
                .then_some($request.max_wave_metres),
            max_opposing_wind_current_knots_squared: ($request.limits_available & 8 != 0)
                .then_some($request.max_opposing_wind_current_knots_squared),
            land_safety_margin_nautical_miles: $request.land_safety_margin_nautical_miles,
            minimum_chart_depth_metres: $request.minimum_chart_depth_metres,
            require_authoritative_chart_safety: $request.require_authoritative_chart_safety != 0,
            use_currents: $request.use_currents != 0,
            require_current_data: $request.require_current_data != 0,
            use_waves: $request.use_waves != 0,
            require_wave_data: $request.require_wave_data != 0,
            maximum_latitude_degrees: $request.maximum_latitude_degrees,
            upwind_efficiency: $request.upwind_efficiency,
            downwind_efficiency: $request.downwind_efficiency,
            tack_penalty_seconds: $request.tack_penalty_seconds,
            gybe_penalty_seconds: $request.gybe_penalty_seconds,
            allow_motor_sailing: $request.allow_motor_sailing != 0,
            allow_motor: $request.allow_motor != 0,
            motor_below_sailing_speed_knots: $request.motor_below_sailing_speed_knots,
            motor_speed_knots: $request.motor_speed_knots,
            motor_sailing_boost_knots: $request.motor_sailing_boost_knots,
            motor_crossover_hysteresis_knots: $request.motor_crossover_hysteresis_knots,
            minimum_motor_run_seconds: $request.minimum_motor_run_seconds,
            mode_change_penalty_seconds: $request.mode_change_penalty_seconds,
            maximum_motor_seconds: ($request.limits_available & 16 != 0)
                .then_some($request.maximum_motor_seconds),
            fuel_consumption_litres_per_hour: ($request.limits_available & 32 != 0)
                .then_some($request.fuel_consumption_litres_per_hour),
            maximum_fuel_litres: ($request.limits_available & 64 != 0)
                .then_some($request.maximum_fuel_litres),
            maximum_search_angle_degrees: $request.maximum_search_angle_degrees,
            destination_tolerance_nm: $request.destination_tolerance_nm,
            $($extra_name: $extra_value,)*
        }
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_calculate_route(
    runtime: *mut Runtime,
    request: *const RouteRequest,
    result: *mut RouteResult,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let Some(request) = (unsafe { request.as_ref() }) else {
        write_error(error, error_capacity, "route request is null");
        return -1;
    };
    let Some(output) = (unsafe { result.as_mut() }) else {
        write_error(error, error_capacity, "route result is null");
        return -1;
    };
    let calculated = (|| -> anyhow::Result<()> {
        prepare_routing_call(runtime, request.max_states)?;
        if request.polar_count == 0 || request.polar_count > POLAR_GRID_LIMIT {
            anyhow::bail!("route request needs 1-{POLAR_GRID_LIMIT} polar grids");
        }
        if request.polars.is_null() {
            anyhow::bail!("route polar pointer is null");
        }
        let raw_polars = unsafe { slice::from_raw_parts(request.polars, request.polar_count) };
        let mut total_cells = 0usize;
        let mut polars = Vec::with_capacity(raw_polars.len());
        for raw in raw_polars {
            if raw.true_wind_speed_count < 2
                || raw.true_wind_speed_count > POLAR_AXIS_LIMIT
                || raw.true_wind_angle_count < 2
                || raw.true_wind_angle_count > POLAR_AXIS_LIMIT
            {
                anyhow::bail!("polar axes are outside the supported range");
            }
            let expected = raw
                .true_wind_speed_count
                .checked_mul(raw.true_wind_angle_count)
                .ok_or_else(|| anyhow::anyhow!("polar dimensions overflow"))?;
            if raw.boat_speed_count != expected {
                anyhow::bail!("polar grid dimensions do not match boat speeds");
            }
            total_cells = total_cells
                .checked_add(expected)
                .ok_or_else(|| anyhow::anyhow!("polar cell count overflow"))?;
            if total_cells > POLAR_CELL_LIMIT {
                anyhow::bail!("route polar data exceeds the cell limit");
            }
            polars.push(NormalizedPolarGrid {
                identity: input_string(raw.identity, raw.identity_len)?,
                true_wind_speeds_knots: input_doubles(
                    raw.true_wind_speeds_knots,
                    raw.true_wind_speed_count,
                )?,
                true_wind_angles_degrees: input_doubles(
                    raw.true_wind_angles_degrees,
                    raw.true_wind_angle_count,
                )?,
                boat_speeds_knots: input_doubles(raw.boat_speeds_knots, raw.boat_speed_count)?,
            });
        }
        let route = match &runtime.bindings {
            RuntimeBindings::V01(bindings) => {
                use exports::opencpn::portable::plugin::RouteRequest as GuestRouteRequest;
                let polars = polars
                    .into_iter()
                    .map(|polar| exports::opencpn::portable::plugin::PolarGrid {
                        identity: polar.identity,
                        true_wind_speeds_knots: polar.true_wind_speeds_knots,
                        true_wind_angles_degrees: polar.true_wind_angles_degrees,
                        boat_speeds_knots: polar.boat_speeds_knots,
                    })
                    .collect();
                let guest_request = build_route_request!(GuestRouteRequest, request, polars);
                let route = bindings
                    .opencpn_portable_plugin()
                    .call_calculate_route(&mut runtime.store, &guest_request)?
                    .map_err(anyhow::Error::msg)?;
                normalize_route!(route)
            }
            RuntimeBindings::V02WeatherRouting(bindings) => {
                use api_v02_routing::exports::opencpn::portable::weather_routing_engine as routing;
                use routing::RouteRequest as GuestRouteRequest;
                let polars = polars
                    .into_iter()
                    .map(|polar| routing::PolarGrid {
                        identity: polar.identity,
                        true_wind_speeds_knots: polar.true_wind_speeds_knots,
                        true_wind_angles_degrees: polar.true_wind_angles_degrees,
                        boat_speeds_knots: polar.boat_speeds_knots,
                    })
                    .collect();
                let guest_request = build_route_request!(
                    GuestRouteRequest,
                    request,
                    polars,
                    inspection_interval_seconds:
                        (request.inspection_interval_seconds != 0)
                            .then_some(request.inspection_interval_seconds),
                    include_traces: request.include_traces != 0
                );
                let route = bindings
                    .opencpn_portable_weather_routing_engine()
                    .call_calculate_route(&mut runtime.store, &guest_request)?
                    .map_err(v02_guest_error)?;
                normalize_route!(route)
            }
            RuntimeBindings::V02(_) => {
                anyhow::bail!("portable API 0.2 base world does not export weather routing")
            }
        };
        output.point_count = route.points.len();
        if route.points.len() > ROUTE_POINT_LIMIT
            || route.points.len() > output.point_capacity
            || (!route.points.is_empty() && output.points.is_null())
        {
            anyhow::bail!(
                "route result requires {} points, capacity is {}",
                route.points.len(),
                output.point_capacity
            );
        }
        for (index, point) in route.points.into_iter().enumerate() {
            unsafe {
                *output.points.add(index) = RoutePoint {
                    latitude: point.latitude,
                    longitude: point.longitude,
                    unix_time: point.unix_time,
                };
            }
        }
        let copy_lines = |lines: Vec<NormalizedRouteLine>,
                          point_output: *mut RoutePoint,
                          point_capacity: usize,
                          line_output: *mut RouteLine,
                          line_capacity: usize|
         -> anyhow::Result<(usize, usize)> {
            if lines.len() > ROUTE_INSPECTION_LINE_LIMIT || lines.len() > line_capacity {
                anyhow::bail!(
                    "route inspection requires {} lines, capacity is {}",
                    lines.len(),
                    line_capacity
                );
            }
            if !lines.is_empty() && line_output.is_null() {
                anyhow::bail!("route inspection line output is null");
            }
            let line_count = lines.len();
            let point_count = lines.iter().try_fold(0usize, |total, line| {
                total
                    .checked_add(line.points.len())
                    .ok_or_else(|| anyhow::anyhow!("route inspection point count overflow"))
            })?;
            if point_count > ROUTE_INSPECTION_POINT_LIMIT || point_count > point_capacity {
                anyhow::bail!(
                    "route inspection requires {} points, capacity is {}",
                    point_count,
                    point_capacity
                );
            }
            if point_count != 0 && point_output.is_null() {
                anyhow::bail!("route inspection point output is null");
            }
            let mut offset = 0usize;
            for (line_index, line) in lines.into_iter().enumerate() {
                let count = line.points.len();
                unsafe {
                    *line_output.add(line_index) = RouteLine {
                        point_offset: offset,
                        point_count: count,
                        unix_time: line.unix_time,
                    };
                }
                for point in line.points {
                    unsafe {
                        *point_output.add(offset) = RoutePoint {
                            latitude: point.latitude,
                            longitude: point.longitude,
                            unix_time: point.unix_time,
                        };
                    }
                    offset += 1;
                }
            }
            Ok((offset, line_count))
        };
        let (isochrone_point_count, isochrone_count) = copy_lines(
            route.isochrones,
            output.isochrone_points,
            output.isochrone_point_capacity,
            output.isochrones,
            output.isochrone_capacity,
        )?;
        output.isochrone_point_count = isochrone_point_count;
        output.isochrone_count = isochrone_count;
        let (trace_point_count, trace_count) = copy_lines(
            route.traces,
            output.trace_points,
            output.trace_point_capacity,
            output.traces,
            output.trace_capacity,
        )?;
        output.trace_point_count = trace_point_count;
        output.trace_count = trace_count;
        output.route_environment_count = route.route_environment.len();
        if route.route_environment.len() > ROUTE_POINT_LIMIT
            || route.route_environment.len() != output.point_count
            || route.route_environment.len() > output.route_environment_capacity
            || (!route.route_environment.is_empty() && output.route_environment.is_null())
        {
            anyhow::bail!(
                "route environment requires {} points, capacity is {}",
                route.route_environment.len(),
                output.route_environment_capacity
            );
        }
        for (index, point) in route.route_environment.into_iter().enumerate() {
            let mut available = 0u8;
            if point.current_u_knots.is_some() && point.current_v_knots.is_some() {
                available |= 1;
            }
            if point.wave_height_metres.is_some() {
                available |= 2;
            }
            unsafe {
                *output.route_environment.add(index) = RouteEnvironmentPoint {
                    latitude: point.latitude,
                    longitude: point.longitude,
                    unix_time: point.unix_time,
                    wind_u_knots: point.wind_u_knots,
                    wind_v_knots: point.wind_v_knots,
                    current_u_knots: point.current_u_knots.unwrap_or_default(),
                    current_v_knots: point.current_v_knots.unwrap_or_default(),
                    wave_height_metres: point.wave_height_metres.unwrap_or_default(),
                    available,
                };
            }
        }
        output.distance_nautical_miles = route.distance_nautical_miles;
        output.duration_seconds = route.duration_seconds;
        output.states_examined = route.states_examined;
        output.average_speed_knots = route.average_speed_knots;
        output.maximum_speed_knots = route.maximum_speed_knots;
        output.average_sog_knots = route.average_sog_knots;
        output.maximum_sog_knots = route.maximum_sog_knots;
        output.average_wind_knots = route.average_wind_knots;
        output.maximum_wind_knots = route.maximum_wind_knots;
        output.metrics_available = 0;
        if let (Some(average), Some(maximum)) =
            (route.average_current_knots, route.maximum_current_knots)
        {
            output.average_current_knots = average;
            output.maximum_current_knots = maximum;
            output.metrics_available |= 1;
        }
        output.tacks = route.tacks;
        output.motor_seconds = route.motor_seconds;
        output.propulsion_transitions = route.propulsion_transitions;
        if let Some(fuel) = route.estimated_fuel_litres {
            output.estimated_fuel_litres = fuel;
            output.metrics_available |= 2;
        }
        output.comfort_level = route.comfort_level;
        output.diagnostic_len = route.diagnostic.len();
        if route.diagnostic.len() >= output.diagnostic_capacity || output.diagnostic.is_null() {
            anyhow::bail!("route diagnostic exceeded output capacity");
        }
        unsafe {
            ptr::copy_nonoverlapping(
                route.diagnostic.as_ptr(),
                output.diagnostic.cast(),
                route.diagnostic.len(),
            );
            *output.diagnostic.add(route.diagnostic.len()) = 0;
        }
        Ok(())
    })();
    ffi_routing_result(calculated, request.max_states, error, error_capacity)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocpn_portable_runtime_test_trap(
    runtime: *mut Runtime,
    error: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let Some(runtime) = (unsafe { runtime.as_mut() }) else {
        write_error(error, error_capacity, "runtime is null");
        return -1;
    };
    let result = (|| -> anyhow::Result<()> {
        prepare_call(runtime)?;
        match &runtime.bindings {
            RuntimeBindings::V01(bindings) => bindings
                .opencpn_portable_plugin()
                .call_test_trap(&mut runtime.store)?,
            RuntimeBindings::V02(_) | RuntimeBindings::V02WeatherRouting(_) => {
                anyhow::bail!("test-trap is available only to portable API 0.1 fixtures")
            }
        }
        Ok(())
    })();
    ffi_result(result, error, error_capacity)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn routing_deadline_accounts_for_default_two_pass_search() {
        let default_ticks = routing_epoch_deadline_ticks(80_000);
        assert!(default_ticks >= 30 * 60 * 10);
        assert!(default_ticks <= 60 * 60 * 10);
        assert_eq!(routing_epoch_deadline_minutes(80_000), 33);
        assert_eq!(routing_epoch_deadline_ticks(1_000_000), 60 * 60 * 10);
    }

    #[test]
    fn routing_interrupt_is_reported_without_a_wasm_backtrace() {
        let error =
            anyhow::Error::new(Trap::Interrupt).context("error while executing at wasm backtrace");
        let message = routing_error_message(&error, 80_000);
        assert!(message.contains("33-minute runtime deadline"));
        assert!(message.contains("safely interrupted"));
        assert!(!message.contains("wasm backtrace"));
        assert!(!message.contains("wasm function"));
    }

    #[test]
    fn routing_fuel_exhaustion_has_an_actionable_diagnostic() {
        let error = anyhow::Error::new(Trap::OutOfFuel);
        let message = routing_error_message(&error, 80_000);
        assert!(message.contains("computation budget"));
        assert!(message.contains("80000 retained states"));
        assert!(!message.contains("wasm backtrace"));
    }
}
