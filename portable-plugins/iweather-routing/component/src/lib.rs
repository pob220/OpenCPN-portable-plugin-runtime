wit_bindgen::generate!({
    path: "../../../portable-runtime/contracts/0.2",
    world: "weather-routing-plugin-world",
});

use exports::opencpn::portable::weather_routing_engine::{
    PolarGrid, RouteEnvironmentPoint, RouteInspectionLine, RoutePoint, RouteRequest, RouteResult,
};
use opencpn::portable::types::{
    ChartCoverageState, ChartSegmentResult, EnvironmentSample, EnvironmentSampleRequest,
    FinalChartSafetyOptions, GeoPoint, GeoSegment, JobEvent, LogLevel, ServiceError,
};
use std::collections::{BTreeMap, BTreeSet, BinaryHeap, HashMap};

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
    pub fn open_weather_routing() -> Result<(), String> {
        opencpn::portable::surfaces::open("routing.workbench").map_err(text)
    }
    pub fn setting_set(key: &str, value: &str) -> Result<(), String> {
        opencpn::portable::settings::set(key, value).map_err(text)
    }
    pub fn environment_sample_batch(
        requests: &[EnvironmentSampleRequest],
    ) -> Result<Vec<EnvironmentSample>, String> {
        opencpn::portable::environment::sample_batch(requests).map_err(text)
    }
    pub fn charts_query_segments(
        segments: &[GeoSegment],
    ) -> Result<Vec<ChartSegmentResult>, String> {
        opencpn::portable::chart_safety::query_segments(segments).map_err(text)
    }
    pub fn charts_query_final_safety(
        segments: &[GeoSegment],
        options: FinalChartSafetyOptions,
    ) -> Result<Vec<ChartSegmentResult>, String> {
        opencpn::portable::chart_safety::query_final_safety(segments, options).map_err(text)
    }
    pub fn routing_progress(percent: u8, message: &str) {
        opencpn::portable::routing_control::progress(percent, message)
    }
    pub fn routing_cancelled() -> bool {
        opencpn::portable::routing_control::cancelled()
    }
}

struct IWeatherRouting;
const ACTION_OPEN: &str = "iweather-routing.open";
const EARTH_NM: f64 = 3440.065;
// Keep portable chart queries comfortably below the runtime's per-call
// resource ceiling. The ordering of both probes and results is significant.
const CHART_SEGMENT_BATCH_LIMIT: usize = 4096;

#[derive(Clone)]
struct Node {
    lat: f64,
    lon: f64,
    time: i64,
    parent: Option<usize>,
    sailed_nm: f64,
    /// Commanded heading through the water for the leg ending at this node.
    /// Keeping it in the tactical state prevents different laylines from
    /// being collapsed merely because tide places them in the same cell.
    incoming_heading: f64,
    tack: i8,
    propulsion_mode: u8,
    motor_seconds: u64,
    propulsion_run_seconds: u64,
    propulsion_transitions: u32,
    consecutive_wait_seconds: u32,
    reached_destination: bool,
}

type SearchCell = (i32, i32, i8, u8, i16);

fn search_cell(request: &RouteRequest, node: &Node) -> SearchCell {
    let cell = request.spatial_cell_nautical_miles;
    let longitude_scale = node.lat.to_radians().cos().abs().max(0.05);
    (
        (node.lat * 60.0 / cell).round() as i32,
        (node.lon * 60.0 * longitude_scale / cell).round() as i32,
        node.tack,
        node.propulsion_mode,
        (node.incoming_heading / f64::from(request.refined_heading_step_degrees.max(5))).floor()
            as i16,
    )
}

fn candidate_score(request: &RouteRequest, node: &Node) -> f64 {
    distance_nm(
        node.lat,
        node.lon,
        request.destination_latitude,
        request.destination_longitude,
    ) + node.sailed_nm * 0.04
}

fn retain_preliminary_candidate(
    request: &RouteRequest,
    alternatives_per_cell: usize,
    sequence: usize,
    candidate: Node,
    preliminary: &mut BTreeMap<SearchCell, Vec<(f64, usize, Node)>>,
    selected: &mut Vec<(usize, Node)>,
) {
    let remaining = distance_nm(
        candidate.lat,
        candidate.lon,
        request.destination_latitude,
        request.destination_longitude,
    );
    if candidate.reached_destination || remaining <= request.destination_tolerance_nm {
        selected.push((sequence, candidate));
        return;
    }
    let labels = preliminary
        .entry(search_cell(request, &candidate))
        .or_default();
    labels.push((candidate_score(request, &candidate), sequence, candidate));
    labels.sort_by(|a, b| a.0.total_cmp(&b.0));
    labels.truncate(alternatives_per_cell);
}

/// Apply the deterministic sector-balanced outer-front reduction used by
/// mature isochrone routers.  A pure closest-to-destination truncation erases
/// the sideways progress required to reach the opposite layline, especially
/// when a tack initially increases range to the destination.
fn sector_balanced_frontier(
    request: &RouteRequest,
    ranked: Vec<(f64, Node)>,
    limit: usize,
) -> Vec<(f64, Node)> {
    let sector_degrees = (f64::from(request.heading_step_degrees) * 0.5).clamp(5.0, 15.0);
    let mut groups: BTreeMap<(i16, i8, u8), Vec<(f64, Node)>> = BTreeMap::new();
    for item in ranked {
        let angle = bearing(
            request.start_latitude,
            request.start_longitude,
            item.1.lat,
            item.1.lon,
        );
        let key = (
            (angle / sector_degrees).floor() as i16,
            item.1.tack,
            item.1.propulsion_mode,
        );
        groups.entry(key).or_default().push(item);
    }
    let mut iterators: Vec<_> = groups
        .into_values()
        .map(|mut group| {
            group.sort_by(|left, right| left.0.total_cmp(&right.0));
            group.into_iter()
        })
        .collect();
    let mut retained = Vec::with_capacity(limit);
    while retained.len() < limit {
        let mut progressed = false;
        for iterator in &mut iterators {
            if let Some(item) = iterator.next() {
                retained.push(item);
                progressed = true;
                if retained.len() == limit {
                    break;
                }
            }
        }
        if !progressed {
            break;
        }
    }
    retained
}

const PROPULSION_SAIL: u8 = 0;
const PROPULSION_MOTOR_SAIL: u8 = 1;
const PROPULSION_MOTOR: u8 = 2;
const MAX_GRAPH_WAIT_SECONDS: u32 = 6 * 3600;
const VALIDATION_INTERVAL_SECONDS: i64 = 15 * 60;
const VALIDATION_SEGMENT_NM: f64 = 1.5;
// The host rejects current vectors whose magnitude reaches 12 m/s before
// exposing them through the portable environment service.  This declared
// physical ceiling therefore remains an admissible, deliberately
// conservative bound for a current-aware A* heuristic.
const MAX_PROVIDER_CURRENT_SPEED_KNOTS: f64 = 12.0 * 1.94384449;

#[derive(Clone, Copy)]
struct ProgressRange {
    start: u8,
    end: u8,
    prefix: &'static str,
}

impl ProgressRange {
    fn report(self, percent: u8, message: &str) {
        let span = u16::from(self.end.saturating_sub(self.start));
        let scaled = u16::from(self.start) + span * u16::from(percent.min(100)) / 100;
        let message = if self.prefix.is_empty() {
            message.to_string()
        } else {
            format!("{} — {message}", self.prefix)
        };
        host::routing_progress(scaled.min(100) as u8, &message);
    }
}

#[derive(Clone)]
struct RouteCorridor {
    points: Vec<(f64, f64)>,
    half_width_nm: f64,
}

impl RouteCorridor {
    fn from_route(points: &[RoutePoint], half_width_nm: f64) -> Self {
        Self {
            points: points
                .iter()
                .map(|point| (point.latitude, point.longitude))
                .collect(),
            half_width_nm,
        }
    }

    fn contains(&self, latitude: f64, longitude: f64) -> bool {
        self.points.windows(2).any(|pair| {
            point_segment_distance_nm(latitude, longitude, pair[0], pair[1]) <= self.half_width_nm
        }) || self.points.first().is_some_and(|point| {
            distance_nm(latitude, longitude, point.0, point.1) <= self.half_width_nm
        })
    }
}

fn point_segment_distance_nm(
    latitude: f64,
    longitude: f64,
    start: (f64, f64),
    end: (f64, f64),
) -> f64 {
    let longitude_scale = latitude.to_radians().cos().abs().max(0.05);
    let relative = |point: (f64, f64)| {
        (
            angular_difference(point.1, longitude) * 60.0 * longitude_scale,
            (point.0 - latitude) * 60.0,
        )
    };
    let (start_x, start_y) = relative(start);
    let (end_x, end_y) = relative(end);
    let segment_x = end_x - start_x;
    let segment_y = end_y - start_y;
    let length_squared = segment_x * segment_x + segment_y * segment_y;
    if length_squared <= 1e-12 {
        return start_x.hypot(start_y);
    }
    let fraction = (-(start_x * segment_x + start_y * segment_y) / length_squared).clamp(0.0, 1.0);
    (start_x + fraction * segment_x).hypot(start_y + fraction * segment_y)
}

fn radians(value: f64) -> f64 {
    value.to_radians()
}

fn distance_nm(a_lat: f64, a_lon: f64, b_lat: f64, b_lon: f64) -> f64 {
    let dlat = radians(b_lat - a_lat);
    let dlon = radians(b_lon - a_lon);
    let a = (dlat / 2.0).sin().powi(2)
        + radians(a_lat).cos() * radians(b_lat).cos() * (dlon / 2.0).sin().powi(2);
    2.0 * EARTH_NM * a.sqrt().atan2((1.0 - a).sqrt())
}

fn bearing(a_lat: f64, a_lon: f64, b_lat: f64, b_lon: f64) -> f64 {
    let y = radians(b_lon - a_lon).sin() * radians(b_lat).cos();
    let x = radians(a_lat).cos() * radians(b_lat).sin()
        - radians(a_lat).sin() * radians(b_lat).cos() * radians(b_lon - a_lon).cos();
    y.atan2(x).to_degrees().rem_euclid(360.0)
}

fn route_chain(nodes: &[Node], endpoint: usize) -> Vec<RoutePoint> {
    let mut chain = Vec::new();
    let mut cursor = Some(endpoint);
    while let Some(index) = cursor {
        let node = &nodes[index];
        chain.push(RoutePoint {
            latitude: node.lat,
            longitude: node.lon,
            unix_time: node.time,
        });
        cursor = node.parent;
    }
    chain.reverse();
    chain
}

fn route_chain_indices(nodes: &[Node], endpoint: usize) -> Vec<usize> {
    let mut chain = Vec::new();
    let mut cursor = Some(endpoint);
    while let Some(index) = cursor {
        chain.push(index);
        cursor = nodes[index].parent;
    }
    chain.reverse();
    chain
}

fn detached_route(nodes: &[Node], endpoint: usize) -> Vec<Node> {
    route_chain_indices(nodes, endpoint)
        .into_iter()
        .enumerate()
        .map(|(position, index)| {
            let mut node = nodes[index].clone();
            node.parent = position.checked_sub(1);
            node
        })
        .collect()
}

fn install_detached_route(nodes: &mut Vec<Node>, route: &[Node]) -> Option<usize> {
    if route.is_empty() {
        return None;
    }
    let base = nodes.len();
    for (position, source) in route.iter().enumerate() {
        let mut node = source.clone();
        node.parent = position.checked_sub(1).map(|parent| base + parent);
        nodes.push(node);
    }
    Some(nodes.len() - 1)
}

fn detached_route_from_points(points: &[RoutePoint]) -> Option<Vec<Node>> {
    if points.len() < 2 {
        return None;
    }
    let mut sailed_nm = 0.0;
    let mut route = Vec::with_capacity(points.len());
    for (index, point) in points.iter().enumerate() {
        let incoming_heading = if index == 0 {
            0.0
        } else {
            let previous = &points[index - 1];
            sailed_nm += distance_nm(
                previous.latitude,
                previous.longitude,
                point.latitude,
                point.longitude,
            );
            bearing(
                previous.latitude,
                previous.longitude,
                point.latitude,
                point.longitude,
            )
        };
        route.push(Node {
            lat: point.latitude,
            lon: point.longitude,
            time: point.unix_time,
            parent: index.checked_sub(1),
            sailed_nm,
            incoming_heading,
            tack: 0,
            propulsion_mode: PROPULSION_SAIL,
            motor_seconds: 0,
            propulsion_run_seconds: 0,
            propulsion_transitions: 0,
            consecutive_wait_seconds: 0,
            reached_destination: index + 1 == points.len(),
        });
    }
    Some(route)
}

fn retain_earliest_route(incumbent: &mut Option<Vec<Node>>, candidate: Vec<Node>) -> bool {
    let replace =
        incumbent
            .as_ref()
            .zip(candidate.last())
            .is_none_or(|(current, candidate_end)| {
                current.last().is_none_or(|current_end| {
                    candidate_end.time < current_end.time
                        || (candidate_end.time == current_end.time
                            && candidate_end.motor_seconds < current_end.motor_seconds)
                })
            });
    if replace {
        *incumbent = Some(candidate);
    }
    replace
}

fn validation_failure_leg(error: &str) -> Option<usize> {
    let marker = "leg ";
    let start = error.find(marker)? + marker.len();
    let digits = error[start..]
        .chars()
        .take_while(|character| character.is_ascii_digit())
        .collect::<String>();
    let one_based = digits.parse::<usize>().ok()?;
    one_based.checked_sub(1)
}

fn remember_invalid_prefix(
    error: &str,
    chain: &[usize],
    persistent_node_count: usize,
    invalid_prefixes: &mut BTreeSet<usize>,
) {
    let Some(leg) = validation_failure_leg(error) else {
        return;
    };
    let endpoint = leg.saturating_add(1);
    if endpoint < chain.len() && chain[endpoint] < persistent_node_count {
        invalid_prefixes.insert(chain[endpoint]);
    }
}

fn ancestry_has_invalid_prefix(
    nodes: &[Node],
    endpoint: usize,
    invalid_prefixes: &BTreeSet<usize>,
) -> bool {
    let mut cursor = Some(endpoint);
    while let Some(index) = cursor {
        if invalid_prefixes.contains(&index) {
            return true;
        }
        cursor = nodes[index].parent;
    }
    false
}

enum ArrivalCandidateDecision {
    Accepted(usize),
    Rejected(String),
    BudgetExhausted,
}

/// Treat destination tolerance as a provisional geometric event. The
/// candidate must pass independent replay before it can terminate a search;
/// a rejected candidate is removed so recovery cannot accidentally seed from
/// the same invalid predecessor chain.
fn consider_arrival_candidate_with<F>(
    nodes: &mut Vec<Node>,
    candidate: Node,
    examined: &mut u32,
    state_limit: u32,
    validate: F,
) -> ArrivalCandidateDecision
where
    F: FnOnce(&[RoutePoint]) -> Result<(), String>,
{
    if *examined >= state_limit {
        return ArrivalCandidateDecision::BudgetExhausted;
    }
    nodes.push(candidate);
    let index = nodes.len() - 1;
    *examined = examined.saturating_add(1);
    let chain = route_chain(nodes, index);
    match validate(&chain) {
        Ok(()) => ArrivalCandidateDecision::Accepted(index),
        Err(error) => {
            nodes.pop();
            ArrivalCandidateDecision::Rejected(error)
        }
    }
}

/// Build bounded, inspection-only geometry from the actual retained search
/// frontier. One outer representative is selected per angular sector. Large
/// gaps split contours instead of implying an untested connection.
fn inspection_geometry(
    request: &RouteRequest,
    nodes: &[Node],
    frontier: &[usize],
) -> (Vec<RouteInspectionLine>, Vec<RouteInspectionLine>) {
    let sector_degrees = f64::from(request.heading_step_degrees).clamp(10.0, 20.0);
    let mut sectors: HashMap<i32, (f64, usize)> = HashMap::new();
    for &index in frontier {
        let node = &nodes[index];
        let angle = bearing(
            request.start_latitude,
            request.start_longitude,
            node.lat,
            node.lon,
        );
        let sector = (angle / sector_degrees).floor() as i32;
        let radius = distance_nm(
            request.start_latitude,
            request.start_longitude,
            node.lat,
            node.lon,
        );
        match sectors.get(&sector) {
            Some((old_radius, _)) if *old_radius >= radius => {}
            _ => {
                sectors.insert(sector, (radius, index));
            }
        }
    }
    let mut representatives: Vec<_> = sectors.into_values().map(|(_, index)| index).collect();
    representatives.sort_by(|left, right| {
        bearing(
            request.start_latitude,
            request.start_longitude,
            nodes[*left].lat,
            nodes[*left].lon,
        )
        .total_cmp(&bearing(
            request.start_latitude,
            request.start_longitude,
            nodes[*right].lat,
            nodes[*right].lon,
        ))
    });
    representatives.truncate(36);

    let mut contours = Vec::new();
    let mut segment: Vec<RoutePoint> = Vec::new();
    let mut previous: Option<usize> = None;
    for &index in &representatives {
        let node = &nodes[index];
        let split = previous.is_some_and(|old| {
            let angular_gap = angular_difference(
                bearing(
                    request.start_latitude,
                    request.start_longitude,
                    nodes[old].lat,
                    nodes[old].lon,
                ),
                bearing(
                    request.start_latitude,
                    request.start_longitude,
                    node.lat,
                    node.lon,
                ),
            )
            .abs();
            angular_gap > sector_degrees * 2.5
                || distance_nm(nodes[old].lat, nodes[old].lon, node.lat, node.lon) > 80.0
        });
        if split && segment.len() >= 2 {
            contours.push(RouteInspectionLine {
                unix_time: segment[0].unix_time,
                points: std::mem::take(&mut segment),
            });
        } else if split {
            segment.clear();
        }
        segment.push(RoutePoint {
            latitude: node.lat,
            longitude: node.lon,
            unix_time: node.time,
        });
        previous = Some(index);
    }
    if segment.len() >= 2 {
        contours.push(RouteInspectionLine {
            unix_time: segment[0].unix_time,
            points: segment,
        });
    }
    let traces = representatives
        .into_iter()
        .map(|index| RouteInspectionLine {
            unix_time: nodes[index].time,
            points: route_chain(nodes, index),
        })
        .collect();
    (contours, traces)
}

/// Keep inspection output bounded independently of the search-state limit.
/// Newer layers are retained because route-to-cursor inspection is most useful
/// near the destination; this also prevents a long (up to 720 hour) search
/// from exhausting the host's transfer buffers.
fn append_bounded_inspection(
    retained: &mut Vec<RouteInspectionLine>,
    mut incoming: Vec<RouteInspectionLine>,
    maximum_lines: usize,
    maximum_points: usize,
) {
    retained.append(&mut incoming);
    let mut points: usize = retained.iter().map(|line| line.points.len()).sum();
    while retained.len() > maximum_lines || points > maximum_points {
        if retained.is_empty() {
            break;
        }
        points = points.saturating_sub(retained[0].points.len());
        retained.remove(0);
    }
}

fn angular_difference(first: f64, second: f64) -> f64 {
    (first - second + 540.0).rem_euclid(360.0) - 180.0
}

/// Deterministic adaptive heading set derived from the SuperCPN routing
/// strategy.  A coarse fan preserves global exploration while a finer fan
/// around the destination bearing and exact polar laylines avoids quantising
/// away narrow viable passages.  Quantised integer degrees make ordering and
/// de-duplication identical on every supported host architecture.
fn candidate_headings(request: &RouteRequest, target: f64, wind_u: f64, wind_v: f64) -> Vec<f64> {
    const SCALE: f64 = 1_000_000.0;
    let mut values = BTreeSet::new();
    let insert = |set: &mut BTreeSet<i64>, value: f64| {
        set.insert((value.rem_euclid(360.0) * SCALE).round() as i64);
    };
    let coarse = i32::from(request.heading_step_degrees).max(2);
    let search = request.maximum_search_angle_degrees.round() as i32;
    for offset in (-search..=search).step_by(coarse as usize) {
        insert(&mut values, target + f64::from(offset));
    }
    if request.adaptive_headings {
        let fine = i32::from(request.refined_heading_step_degrees).max(1);
        let local = (coarse * 2).min(search);
        for offset in (-local..=local).step_by(fine as usize) {
            insert(&mut values, target + f64::from(offset));
        }
    }

    let wind_to = wind_u.atan2(wind_v).to_degrees().rem_euclid(360.0);
    let wind_from = (wind_to + 180.0).rem_euclid(360.0);
    let wind_speed = wind_u.hypot(wind_v);
    let mut tactical_angles = vec![
        request.min_true_wind_angle_degrees,
        request.max_true_wind_angle_degrees,
    ];
    tactical_angles.extend(optimal_vmg_angles(request, wind_speed));
    for angle in tactical_angles {
        for side in [-1.0, 1.0] {
            let heading = wind_from + side * angle;
            if angular_difference(heading, target).abs()
                <= request.maximum_search_angle_degrees + 1e-9
            {
                insert(&mut values, heading);
            }
        }
    }
    insert(&mut values, target);
    values
        .into_iter()
        .map(|value| value as f64 / SCALE)
        .collect()
}

fn true_wind_angle(wind_u: f64, wind_v: f64, heading: f64) -> (f64, i8) {
    let wind_to = wind_u.atan2(wind_v).to_degrees().rem_euclid(360.0);
    let wind_from = (wind_to + 180.0).rem_euclid(360.0);
    let signed = angular_difference(heading, wind_from);
    (signed.abs(), if signed >= 0.0 { 1 } else { -1 })
}

struct RouteStatistics {
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
}

fn route_environment(points: &[RoutePoint]) -> Result<Vec<RouteEnvironmentPoint>, String> {
    let requests: Vec<_> = points
        .iter()
        .map(|point| EnvironmentSampleRequest {
            latitude: point.latitude,
            longitude: point.longitude,
            unix_time: point.unix_time,
        })
        .collect();
    let samples = host::environment_sample_batch(&requests)?;
    if samples.len() != points.len() {
        return Err("environment provider returned the wrong route-profile batch length".into());
    }
    points
        .iter()
        .zip(samples)
        .map(|(point, sample)| {
            let (Some(wind_u_knots), Some(wind_v_knots)) =
                (sample.wind_u_knots, sample.wind_v_knots)
            else {
                return Err("completed route lost wind coverage in its route profile".into());
            };
            Ok(RouteEnvironmentPoint {
                latitude: point.latitude,
                longitude: point.longitude,
                unix_time: point.unix_time,
                wind_u_knots,
                wind_v_knots,
                current_u_knots: sample.current_u_knots,
                current_v_knots: sample.current_v_knots,
                wave_height_metres: sample.wave_height_metres,
            })
        })
        .collect()
}

fn route_statistics(
    request: &RouteRequest,
    points: &[RoutePoint],
) -> Result<RouteStatistics, String> {
    if points.len() < 2 {
        return Err("completed route has too few points for metrics".into());
    }
    let requests: Vec<_> = points
        .windows(2)
        .map(|pair| {
            let longitude_delta = angular_difference(pair[1].longitude, pair[0].longitude);
            EnvironmentSampleRequest {
                latitude: (pair[0].latitude + pair[1].latitude) * 0.5,
                longitude: (pair[0].longitude + longitude_delta * 0.5 + 540.0).rem_euclid(360.0)
                    - 180.0,
                unix_time: pair[0].unix_time + (pair[1].unix_time - pair[0].unix_time) / 2,
            }
        })
        .collect();
    let samples = host::environment_sample_batch(&requests)?;
    if samples.len() != requests.len() {
        return Err("environment provider returned the wrong route-metric batch length".into());
    }
    let mut speed_total: f64 = 0.0;
    let mut speed_max: f64 = 0.0;
    let mut sog_total: f64 = 0.0;
    let mut sog_max: f64 = 0.0;
    let mut wind_total: f64 = 0.0;
    let mut wind_max: f64 = 0.0;
    let mut current_total: f64 = 0.0;
    let mut current_max: f64 = 0.0;
    let mut current_count = 0usize;
    let mut tacks = 0u32;
    let mut previous_tack = 0i8;
    let mut previous_mode = PROPULSION_SAIL;
    let mut motor_seconds = 0u64;
    let mut propulsion_run_seconds = 0u64;
    let mut propulsion_transitions = 0u32;
    let mut comfort_level = 1u8;
    let mut motion_leg_count = 0usize;
    for (index, pair) in points.windows(2).enumerate() {
        let elapsed_hours = (pair[1].unix_time - pair[0].unix_time) as f64 / 3600.0;
        if elapsed_hours <= 0.0 {
            continue;
        }
        if distance_nm(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        ) <= 0.001
        {
            continue;
        }
        motion_leg_count += 1;
        let sample = &samples[index];
        let (wind_u, wind_v) = match (sample.wind_u_knots, sample.wind_v_knots) {
            (Some(u), Some(v)) => (u, v),
            _ => {
                return Err(format!(
                    "completed route lost wind coverage while calculating metrics for leg {} at {} ({:.5}, {:.5})",
                    index + 1,
                    requests[index].unix_time,
                    requests[index].latitude,
                    requests[index].longitude
                ));
            }
        };
        let wind = wind_u.hypot(wind_v);
        let (current_u, current_v, has_current) =
            match (sample.current_u_knots, sample.current_v_knots) {
                (Some(u), Some(v)) => (u, v, true),
                _ => (0.0, 0.0, false),
            };
        let kinematics = leg_kinematics(&pair[0], &pair[1], current_u, current_v)
            .ok_or_else(|| "completed route has no usable leg motion".to_string())?;
        let (twa, tack) = true_wind_angle(wind_u, wind_v, kinematics.heading_through_water);
        if previous_tack != 0 && tack != previous_tack && twa <= 90.0 {
            tacks += 1;
        }
        let leg_seconds = (pair[1].unix_time - pair[0].unix_time).max(1) as u32;
        let motion = motion_for_heading(
            request,
            wind_u,
            wind_v,
            current_u,
            current_v,
            kinematics.heading_through_water,
            previous_tack,
            previous_mode,
            propulsion_run_seconds,
            leg_seconds,
        )
        .ok_or_else(|| "completed route violates its propulsion policy".to_string())?;
        previous_tack = motion.tack;
        propulsion_run_seconds = next_propulsion_run_seconds(
            previous_mode,
            motion.propulsion_mode,
            propulsion_run_seconds,
            leg_seconds,
        );
        if motion.propulsion_mode != previous_mode {
            propulsion_transitions = propulsion_transitions.saturating_add(1);
        }
        previous_mode = motion.propulsion_mode;
        if motion.propulsion_mode != PROPULSION_SAIL {
            motor_seconds = motor_seconds.saturating_add(u64::from(leg_seconds));
        }
        speed_total += kinematics.effective_speed_through_water;
        speed_max = speed_max.max(kinematics.effective_speed_through_water);
        sog_total += kinematics.speed_over_ground;
        sog_max = sog_max.max(kinematics.speed_over_ground);
        wind_total += wind;
        wind_max = wind_max.max(wind);
        if has_current {
            let current = current_u.hypot(current_v);
            current_total += current;
            current_max = current_max.max(current);
            current_count += 1;
        }

        // This follows the existing Weather Routing comfort categorisation.
        // It is deliberately labelled subjective in the host UI.
        let wave = sample.wave_height_metres.unwrap_or(0.0).max(0.0);
        let wind_effect = (wind / 27.0).powi(3);
        let angle_effect = 20.0 / (30.0 * (2.0 * std::f64::consts::PI).sqrt())
            * (-(twa - 35.0).powi(2) / (2.0 * 30.0f64.powi(2))).exp();
        let wave_effect = (wave / 5.0).powi(2);
        let score = wind_effect * (1.0 + angle_effect) * (1.0 + wave_effect);
        comfort_level = comfort_level.max(if score <= 0.5 {
            1
        } else if score < 1.0 {
            2
        } else {
            3
        });
    }
    let count = motion_leg_count.max(1) as f64;
    Ok(RouteStatistics {
        average_speed_knots: speed_total / count,
        maximum_speed_knots: speed_max,
        average_sog_knots: sog_total / count,
        maximum_sog_knots: sog_max,
        average_wind_knots: wind_total / count,
        maximum_wind_knots: wind_max,
        average_current_knots: (current_count > 0).then_some(current_total / current_count as f64),
        maximum_current_knots: (current_count > 0).then_some(current_max),
        tacks,
        motor_seconds,
        estimated_fuel_litres: request
            .fuel_consumption_litres_per_hour
            .map(|rate| rate * motor_seconds as f64 / 3600.0),
        propulsion_transitions,
        comfort_level,
    })
}

fn bounds(values: &[f64], target: f64) -> (usize, usize) {
    match values.binary_search_by(|value| value.total_cmp(&target)) {
        Ok(index) => (index, index),
        Err(0) => (0, 0),
        Err(index) if index >= values.len() => (values.len() - 1, values.len() - 1),
        Err(index) => (index - 1, index),
    }
}

fn interpolate_polar(grid: &PolarGrid, tws: f64, twa: f64) -> Option<f64> {
    if twa < *grid.true_wind_angles_degrees.first()?
        || twa > *grid.true_wind_angles_degrees.last()?
    {
        return None;
    }
    let (w0, w1) = bounds(&grid.true_wind_speeds_knots, tws);
    let (a0, a1) = bounds(&grid.true_wind_angles_degrees, twa.clamp(0.0, 180.0));
    let angles = grid.true_wind_angles_degrees.len();
    let speed = |wind: usize, angle: usize| grid.boat_speeds_knots[wind * angles + angle];
    let q00 = speed(w0, a0);
    let q01 = speed(w0, a1);
    let q10 = speed(w1, a0);
    let q11 = speed(w1, a1);
    let angle_factor = if a0 == a1 {
        0.0
    } else {
        (twa - grid.true_wind_angles_degrees[a0])
            / (grid.true_wind_angles_degrees[a1] - grid.true_wind_angles_degrees[a0])
    };
    let wind_factor = if w0 == w1 {
        0.0
    } else {
        (tws - grid.true_wind_speeds_knots[w0])
            / (grid.true_wind_speeds_knots[w1] - grid.true_wind_speeds_knots[w0])
    };
    let low = q00 + angle_factor * (q01 - q00);
    let high = q10 + angle_factor * (q11 - q10);
    let value = low + wind_factor * (high - low);
    value.is_finite().then_some(value)
}

fn polar_speed(request: &RouteRequest, tws: f64, twa: f64) -> Option<f64> {
    request
        .polars
        .iter()
        .filter_map(|grid| interpolate_polar(grid, tws, twa))
        .filter(|speed| *speed > 0.05)
        .max_by(f64::total_cmp)
}

/// Return the best attainable upwind and downwind VMG angles for the active
/// polar set.  Boundary laylines alone are rarely optimal: mature routing
/// engines explicitly add these polar-derived headings to their search fan.
fn optimal_vmg_angles(request: &RouteRequest, tws: f64) -> Vec<f64> {
    let start = request.min_true_wind_angle_degrees.ceil() as i32;
    let end = request.max_true_wind_angle_degrees.floor() as i32;
    let mut upwind: Option<(f64, f64)> = None;
    let mut downwind: Option<(f64, f64)> = None;
    for degrees in start..=end {
        let angle = f64::from(degrees);
        let Some(speed) = polar_speed(request, tws, angle) else {
            continue;
        };
        if angle <= 90.0 {
            let vmg = speed * radians(angle).cos();
            if upwind.is_none_or(|(best, _)| vmg > best) {
                upwind = Some((vmg, angle));
            }
        }
        if angle >= 90.0 {
            let vmg = -speed * radians(angle).cos();
            if downwind.is_none_or(|(best, _)| vmg > best) {
                downwind = Some((vmg, angle));
            }
        }
    }
    upwind
        .into_iter()
        .chain(downwind)
        .map(|(_, angle)| angle)
        .collect()
}

#[derive(Clone, Copy)]
struct Motion {
    east_knots: f64,
    north_knots: f64,
    tack: i8,
    propulsion_mode: u8,
}

#[derive(Clone)]
struct ArrivalRefinement {
    candidate_index: usize,
    node_index: usize,
    heading: f64,
    elapsed_seconds: u32,
    motion: Motion,
    fallback: Option<Node>,
}

fn motion_for_heading(
    request: &RouteRequest,
    wind_u: f64,
    wind_v: f64,
    current_u: f64,
    current_v: f64,
    heading: f64,
    previous_tack: i8,
    previous_mode: u8,
    propulsion_run_seconds: u64,
    step_seconds: u32,
) -> Option<Motion> {
    let wind = wind_u.hypot(wind_v);
    let (twa, tack) = true_wind_angle(wind_u, wind_v, heading);
    let sailing_allowed = twa + 1e-9 >= request.min_true_wind_angle_degrees
        && twa - 1e-9 <= request.max_true_wind_angle_degrees;
    let sail_speed = sailing_allowed
        .then(|| {
            let efficiency = if twa <= 90.0 {
                request.upwind_efficiency
            } else {
                request.downwind_efficiency
            };
            polar_speed(request, wind, twa).map(|speed| speed * efficiency)
        })
        .flatten();
    let minimum_run_active = previous_mode != PROPULSION_SAIL
        && propulsion_run_seconds < u64::from(request.minimum_motor_run_seconds);
    let threshold = request.motor_below_sailing_speed_knots
        + if previous_mode == PROPULSION_SAIL {
            0.0
        } else {
            request.motor_crossover_hysteresis_knots
        };
    let engage_motor = sail_speed.is_none_or(|speed| speed < threshold);
    let mut choices = Vec::new();
    if !minimum_run_active {
        if let Some(speed) = sail_speed {
            choices.push((speed, PROPULSION_SAIL));
        }
    }
    if request.allow_motor_sailing && sailing_allowed && (engage_motor || minimum_run_active) {
        if let Some(speed) = sail_speed {
            choices.push((
                speed + request.motor_sailing_boost_knots,
                PROPULSION_MOTOR_SAIL,
            ));
        }
    }
    if request.allow_motor && (engage_motor || minimum_run_active) {
        choices.push((request.motor_speed_knots, PROPULSION_MOTOR));
    }
    let (speed, propulsion_mode) = choices
        .into_iter()
        .filter(|(speed, _)| speed.is_finite() && *speed > 0.05)
        .max_by(|left, right| left.0.total_cmp(&right.0))?;
    let heading_rad = radians(heading);
    let boat_east = speed * heading_rad.sin();
    let boat_north = speed * heading_rad.cos();
    if request
        .max_apparent_wind_knots
        .is_some_and(|limit| (wind_u - boat_east).hypot(wind_v - boat_north) > limit)
    {
        return None;
    }
    let manoeuvre_penalty =
        if propulsion_mode != PROPULSION_MOTOR && previous_tack != 0 && previous_tack != tack {
            if twa <= 90.0 {
                request.tack_penalty_seconds
            } else {
                request.gybe_penalty_seconds
            }
        } else {
            0
        };
    let mode_penalty = if previous_mode != propulsion_mode {
        request.mode_change_penalty_seconds
    } else {
        0
    };
    let penalty = manoeuvre_penalty.saturating_add(mode_penalty);
    if penalty >= step_seconds {
        return None;
    }
    // Manoeuvre time reduces progress through the water while current still
    // acts for the complete forecast step.
    let moving_fraction = (step_seconds - penalty) as f64 / step_seconds as f64;
    Some(Motion {
        east_knots: boat_east * moving_fraction + current_u,
        north_knots: boat_north * moving_fraction + current_v,
        tack,
        propulsion_mode,
    })
}

fn next_propulsion_run_seconds(
    previous_mode: u8,
    next_mode: u8,
    previous_run_seconds: u64,
    elapsed_seconds: u32,
) -> u64 {
    if next_mode == PROPULSION_SAIL {
        0
    } else if previous_mode == PROPULSION_SAIL {
        u64::from(elapsed_seconds)
    } else {
        previous_run_seconds.saturating_add(u64::from(elapsed_seconds))
    }
}

fn advance(lat: f64, lon: f64, east_knots: f64, north_knots: f64, seconds: u32) -> (f64, f64, f64) {
    let hours = seconds as f64 / 3600.0;
    let distance = east_knots.hypot(north_knots) * hours;
    if distance < 1e-9 {
        return (lat, lon, 0.0);
    }
    let brg = east_knots.atan2(north_knots);
    let angular = distance / EARTH_NM;
    let lat1 = radians(lat);
    let lon1 = radians(lon);
    let lat2 = (lat1.sin() * angular.cos() + lat1.cos() * angular.sin() * brg.cos()).asin();
    let lon2 = lon1
        + (brg.sin() * angular.sin() * lat1.cos()).atan2(angular.cos() - lat1.sin() * lat2.sin());
    (
        lat2.to_degrees(),
        ((lon2.to_degrees() + 540.0) % 360.0) - 180.0,
        distance,
    )
}

fn propagated_node(
    request: &RouteRequest,
    node_index: usize,
    node: &Node,
    heading: f64,
    motion: Motion,
    elapsed_seconds: u32,
    target_latitude: f64,
    target_longitude: f64,
) -> Option<Node> {
    let next_motor_seconds =
        node.motor_seconds
            .saturating_add(if motion.propulsion_mode == PROPULSION_SAIL {
                0
            } else {
                u64::from(elapsed_seconds)
            });
    if !motor_budget_allows(request, next_motor_seconds) {
        return None;
    }
    let (lat, lon, sailed) = advance(
        node.lat,
        node.lon,
        motion.east_knots,
        motion.north_knots,
        elapsed_seconds,
    );
    if !lat.is_finite() || !lon.is_finite() || lat.abs() > request.maximum_latitude_degrees {
        return None;
    }
    Some(Node {
        lat,
        lon,
        time: node.time + i64::from(elapsed_seconds),
        parent: Some(node_index),
        sailed_nm: node.sailed_nm + sailed,
        incoming_heading: heading,
        tack: motion.tack,
        propulsion_mode: motion.propulsion_mode,
        motor_seconds: next_motor_seconds,
        propulsion_run_seconds: next_propulsion_run_seconds(
            node.propulsion_mode,
            motion.propulsion_mode,
            node.propulsion_run_seconds,
            elapsed_seconds,
        ),
        propulsion_transitions: node.propulsion_transitions
            + u32::from(motion.propulsion_mode != node.propulsion_mode),
        consecutive_wait_seconds: 0,
        reached_destination: distance_nm(lat, lon, target_latitude, target_longitude)
            <= request.destination_tolerance_nm,
    })
}

fn arrival_trial_seconds(
    request: &RouteRequest,
    node: &Node,
    motion: Motion,
    maximum_seconds: u32,
) -> Option<u32> {
    let target = bearing(
        node.lat,
        node.lon,
        request.destination_latitude,
        request.destination_longitude,
    );
    let target_radians = radians(target);
    let direct_distance = distance_nm(
        node.lat,
        node.lon,
        request.destination_latitude,
        request.destination_longitude,
    );
    let ground_speed_squared = motion.east_knots.powi(2) + motion.north_knots.powi(2);
    let along_target =
        motion.east_knots * target_radians.sin() + motion.north_knots * target_radians.cos();
    if ground_speed_squared <= 0.0025 || along_target <= 0.05 {
        return None;
    }
    let closest_seconds = (direct_distance * along_target / ground_speed_squared * 3600.0).ceil();
    if !closest_seconds.is_finite()
        || closest_seconds < 1.0
        || closest_seconds > f64::from(maximum_seconds)
    {
        return None;
    }
    let seconds = closest_seconds as u32;
    let (latitude, longitude, _) = advance(
        node.lat,
        node.lon,
        motion.east_knots,
        motion.north_knots,
        seconds,
    );
    (distance_nm(
        latitude,
        longitude,
        request.destination_latitude,
        request.destination_longitude,
    ) <= request.destination_tolerance_nm)
        .then_some(seconds)
}

fn arrival_midpoint_request(
    node: &Node,
    motion: Motion,
    elapsed_seconds: u32,
) -> EnvironmentSampleRequest {
    let (latitude, longitude, _) = advance(
        node.lat,
        node.lon,
        motion.east_knots,
        motion.north_knots,
        elapsed_seconds / 2,
    );
    EnvironmentSampleRequest {
        latitude,
        longitude,
        unix_time: node.time + i64::from(elapsed_seconds / 2),
    }
}

/// Re-evaluate provisionally shortened arrival legs at their real midpoint.
///
/// The ordinary predictor/corrector samples the midpoint of a complete
/// routing step. An arrival may use only a fraction of that step, so reusing
/// the complete-step sample can admit a leg which the independent replay
/// correctly rejects. Two batched corrections keep the host-call cost
/// bounded while converging the elapsed time and environmental midpoint.
fn refine_shortened_arrivals_with<F>(
    request: &RouteRequest,
    nodes: &[Node],
    candidates: &mut Vec<Node>,
    mut refinements: Vec<ArrivalRefinement>,
    mut sample_batch: F,
) -> Result<(), String>
where
    F: FnMut(&[EnvironmentSampleRequest]) -> Result<Vec<EnvironmentSample>, String>,
{
    let mut replacements = vec![None; candidates.len()];
    for refinement in &refinements {
        replacements[refinement.candidate_index] = Some(refinement.fallback.clone());
    }
    for _ in 0..2 {
        if refinements.is_empty() {
            break;
        }
        let requests: Vec<_> = refinements
            .iter()
            .map(|refinement| {
                arrival_midpoint_request(
                    &nodes[refinement.node_index],
                    refinement.motion,
                    refinement.elapsed_seconds,
                )
            })
            .collect();
        let samples = sample_batch(&requests)?;
        if samples.len() != refinements.len() {
            return Err(
                "environment provider returned the wrong shortened-arrival batch length".into(),
            );
        }
        let mut corrected = Vec::with_capacity(refinements.len());
        for (mut refinement, sample) in refinements.into_iter().zip(samples) {
            let node = &nodes[refinement.node_index];
            let Some((wind_u, wind_v, current_u, current_v)) = usable_environment(
                request,
                sample.wind_u_knots,
                sample.wind_v_knots,
                sample.current_u_knots,
                sample.current_v_knots,
                sample.wave_height_metres,
            ) else {
                continue;
            };
            let Some(midpoint_motion) = motion_for_heading(
                request,
                wind_u,
                wind_v,
                current_u,
                current_v,
                refinement.heading,
                node.tack,
                node.propulsion_mode,
                node.propulsion_run_seconds,
                refinement.elapsed_seconds,
            ) else {
                continue;
            };
            let Some(elapsed_seconds) =
                arrival_trial_seconds(request, node, midpoint_motion, request.time_step_seconds)
            else {
                continue;
            };
            let Some(motion) = motion_for_heading(
                request,
                wind_u,
                wind_v,
                current_u,
                current_v,
                refinement.heading,
                node.tack,
                node.propulsion_mode,
                node.propulsion_run_seconds,
                elapsed_seconds,
            ) else {
                continue;
            };
            refinement.elapsed_seconds = elapsed_seconds;
            refinement.motion = motion;
            corrected.push(refinement);
        }
        refinements = corrected;
    }
    for refinement in refinements {
        let Some(candidate) = propagated_node(
            request,
            refinement.node_index,
            &nodes[refinement.node_index],
            refinement.heading,
            refinement.motion,
            refinement.elapsed_seconds,
            request.destination_latitude,
            request.destination_longitude,
        ) else {
            continue;
        };
        if candidate.reached_destination {
            replacements[refinement.candidate_index] = Some(Some(candidate));
        }
    }
    let original = std::mem::take(candidates);
    *candidates = original
        .into_iter()
        .enumerate()
        .filter_map(|(index, candidate)| match replacements[index].take() {
            None => Some(candidate),
            Some(replacement) => replacement,
        })
        .collect();
    Ok(())
}

fn refine_shortened_arrivals(
    request: &RouteRequest,
    nodes: &[Node],
    candidates: &mut Vec<Node>,
    refinements: Vec<ArrivalRefinement>,
) -> Result<(), String> {
    refine_shortened_arrivals_with(
        request,
        nodes,
        candidates,
        refinements,
        host::environment_sample_batch,
    )
}

struct LegKinematics {
    course_over_ground: f64,
    speed_over_ground: f64,
    heading_through_water: f64,
    effective_speed_through_water: f64,
}

/// Recover the vessel's heading and effective speed through the water from a
/// delivered ground track.  Route geometry records position and time, so the
/// current vector must be removed before applying polar or true-wind-angle
/// policy.  Treating COG as heading and then adding current again can reject a
/// perfectly valid crabbed course in a tidal stream.
fn leg_kinematics(
    start: &RoutePoint,
    end: &RoutePoint,
    current_u: f64,
    current_v: f64,
) -> Option<LegKinematics> {
    let elapsed_hours = (end.unix_time - start.unix_time) as f64 / 3600.0;
    if !elapsed_hours.is_finite() || elapsed_hours <= 0.0 {
        return None;
    }
    let course_over_ground = bearing(start.latitude, start.longitude, end.latitude, end.longitude);
    let speed_over_ground =
        distance_nm(start.latitude, start.longitude, end.latitude, end.longitude) / elapsed_hours;
    if !speed_over_ground.is_finite() || speed_over_ground <= 0.0 {
        return None;
    }
    let course_radians = radians(course_over_ground);
    let water_east = speed_over_ground * course_radians.sin() - current_u;
    let water_north = speed_over_ground * course_radians.cos() - current_v;
    let effective_speed_through_water = water_east.hypot(water_north);
    if !effective_speed_through_water.is_finite() || effective_speed_through_water <= 0.01 {
        return None;
    }
    Some(LegKinematics {
        course_over_ground,
        speed_over_ground,
        heading_through_water: water_east.atan2(water_north).to_degrees().rem_euclid(360.0),
        effective_speed_through_water,
    })
}

fn opposing_wind_current(wind_u: f64, wind_v: f64, current_u: f64, current_v: f64) -> f64 {
    -(wind_u * current_u + wind_v * current_v)
}

/// Return the centre segment and conservative probes at the configured
/// clearance.  The host owns land/chart data; the portable engine only sends
/// value geometry.  Search propagation uses the centre and parallel sides.
/// Dense final validation also crosses the opposite corners so an obstruction
/// inside the swept corridor cannot hide between the three longitudinal
/// probes.
fn clearance_segments(
    segment: &GeoSegment,
    margin_nm: f64,
    close_final_corridor: bool,
) -> Vec<GeoSegment> {
    let mut result = vec![segment.clone()];
    if margin_nm <= 1e-9 {
        return result;
    }
    let course = bearing(
        segment.start.latitude,
        segment.start.longitude,
        segment.end.latitude,
        segment.end.longitude,
    );
    let course_radians = radians(course);
    let perpendicular_east = course_radians.cos();
    let perpendicular_north = -course_radians.sin();
    let average_latitude = (segment.start.latitude + segment.end.latitude) * 0.5;
    let longitude_scale = average_latitude.to_radians().cos().abs().max(0.05);
    let mut parallel = Vec::with_capacity(2);
    for side in [-1.0, 1.0] {
        let latitude_offset = side * perpendicular_north * margin_nm / 60.0;
        let longitude_offset = side * perpendicular_east * margin_nm / (60.0 * longitude_scale);
        parallel.push(GeoSegment {
            start: GeoPoint {
                latitude: (segment.start.latitude + latitude_offset).clamp(-90.0, 90.0),
                longitude: (segment.start.longitude + longitude_offset + 540.0).rem_euclid(360.0)
                    - 180.0,
            },
            end: GeoPoint {
                latitude: (segment.end.latitude + latitude_offset).clamp(-90.0, 90.0),
                longitude: (segment.end.longitude + longitude_offset + 540.0).rem_euclid(360.0)
                    - 180.0,
            },
        });
    }
    result.extend(parallel.iter().cloned());
    if close_final_corridor {
        // The centre and parallel probes alone can miss a small obstruction
        // inside the swept rectangle.  Crossing the opposite offset corners
        // matches the conservative final-route geometry used by the working
        // OpenCPN weather router while keeping the cheaper three-line test for
        // the much larger propagation fan.
        result.push(GeoSegment {
            start: parallel[0].start.clone(),
            end: parallel[1].end.clone(),
        });
        result.push(GeoSegment {
            start: parallel[1].start.clone(),
            end: parallel[0].end.clone(),
        });
    }
    result
}

fn query_chart_segments_with<F>(
    segments: &[GeoSegment],
    mut query: F,
) -> Result<Vec<ChartSegmentResult>, String>
where
    F: FnMut(&[GeoSegment]) -> Result<Vec<ChartSegmentResult>, String>,
{
    let mut results = Vec::with_capacity(segments.len());
    for batch in segments.chunks(CHART_SEGMENT_BATCH_LIMIT) {
        let mut batch_results = query(batch)?;
        if batch_results.len() != batch.len() {
            return Err("chart service returned the wrong batch length".into());
        }
        results.append(&mut batch_results);
    }
    Ok(results)
}

fn query_chart_segments(segments: &[GeoSegment]) -> Result<Vec<ChartSegmentResult>, String> {
    query_chart_segments_with(segments, host::charts_query_segments)
}

fn query_final_chart_safety(
    segments: &[GeoSegment],
    request: &RouteRequest,
) -> Result<Vec<ChartSegmentResult>, String> {
    // The portable engine has already expanded every dense route segment into
    // the full five-line clearance corridor.  Passing zero here prevents the
    // host provider from applying the horizontal margin a second time while
    // still requesting chart-object and depth semantics for every probe.
    let options = FinalChartSafetyOptions {
        safety_margin_nautical_miles: 0.0,
        minimum_depth_metres: request.minimum_chart_depth_metres,
        require_authoritative: request.require_authoritative_chart_safety,
    };
    query_chart_segments_with(segments, |batch| {
        host::charts_query_final_safety(batch, options.clone())
    })
}

fn chart_corridor_is_covered(results: &[ChartSegmentResult]) -> bool {
    !results.is_empty()
        && results
            .iter()
            .all(|result| result.state == ChartCoverageState::Covered)
}

fn validate(request: &RouteRequest) -> Result<(), String> {
    for value in [request.start_latitude, request.destination_latitude] {
        if !value.is_finite() || !(-90.0..=90.0).contains(&value) {
            return Err("route latitude is invalid".into());
        }
    }
    for value in [request.start_longitude, request.destination_longitude] {
        if !value.is_finite() || !(-180.0..=180.0).contains(&value) {
            return Err("route longitude is invalid".into());
        }
    }
    if !(60..=21600).contains(&request.time_step_seconds)
        || !(2..=90).contains(&request.heading_step_degrees)
        || !(1..=90).contains(&request.refined_heading_step_degrees)
        || request.refined_heading_step_degrees > request.heading_step_degrees
        || !request.spatial_cell_nautical_miles.is_finite()
        || !(0.25..=30.0).contains(&request.spatial_cell_nautical_miles)
        || !(1..=8).contains(&request.labels_per_cell)
        || !(1..=720).contains(&request.max_hours)
        || !(100..=1_000_000).contains(&request.max_states)
        || !request.minimum_chart_depth_metres.is_finite()
        || !(0.0..=100.0).contains(&request.minimum_chart_depth_metres)
    {
        return Err("route calculation limits are outside the supported range".into());
    }
    if request.polars.is_empty() || request.polars.len() > 8 {
        return Err("route request needs 1-8 polar grids".into());
    }
    let mut total_cells = 0usize;
    for polar in &request.polars {
        if polar.identity.is_empty()
            || polar.true_wind_speeds_knots.len() < 2
            || polar.true_wind_speeds_knots.len() > 200
            || polar.true_wind_angles_degrees.len() < 2
            || polar.true_wind_angles_degrees.len() > 200
        {
            return Err("polar identity or axes are invalid".into());
        }
        if !polar
            .true_wind_speeds_knots
            .windows(2)
            .all(|pair| pair[0].is_finite() && pair[0] >= 0.0 && pair[0] < pair[1])
            || !polar
                .true_wind_speeds_knots
                .last()
                .is_some_and(|value| value.is_finite() && *value <= 200.0)
            || !polar.true_wind_angles_degrees.windows(2).all(|pair| {
                pair[0].is_finite() && pair[0] >= 0.0 && pair[0] < pair[1] && pair[1] <= 180.0
            })
        {
            return Err("polar axes must be finite and strictly increasing".into());
        }
        let expected = polar
            .true_wind_speeds_knots
            .len()
            .checked_mul(polar.true_wind_angles_degrees.len())
            .ok_or_else(|| "polar dimensions overflow".to_string())?;
        if polar.boat_speeds_knots.len() != expected
            || polar
                .boat_speeds_knots
                .iter()
                .any(|speed| !speed.is_finite() || !(0.0..=100.0).contains(speed))
        {
            return Err("polar dimensions or boat speeds are invalid".into());
        }
        total_cells = total_cells
            .checked_add(expected)
            .ok_or_else(|| "polar cell count overflow".to_string())?;
    }
    if total_cells > 200_000 {
        return Err("route polar data exceeds the cell limit".into());
    }
    if !request.min_true_wind_angle_degrees.is_finite()
        || !request.max_true_wind_angle_degrees.is_finite()
        || !(0.0..=180.0).contains(&request.min_true_wind_angle_degrees)
        || !(0.0..=180.0).contains(&request.max_true_wind_angle_degrees)
        || request.min_true_wind_angle_degrees > request.max_true_wind_angle_degrees
    {
        return Err("true-wind-angle bounds must satisfy 0 <= minimum <= maximum <= 180".into());
    }
    if !(0.1..=1.5).contains(&request.upwind_efficiency)
        || !(0.1..=1.5).contains(&request.downwind_efficiency)
        || !(30.0..=180.0).contains(&request.maximum_search_angle_degrees)
        || !(0.05..=20.0).contains(&request.destination_tolerance_nm)
        || !(1.0..=90.0).contains(&request.maximum_latitude_degrees)
        || !request.land_safety_margin_nautical_miles.is_finite()
        || !(0.0..=20.0).contains(&request.land_safety_margin_nautical_miles)
        || request.require_current_data && !request.use_currents
        || request.require_wave_data && !request.use_waves
    {
        return Err("route vessel, data-policy or search settings are invalid".into());
    }
    if !request.motor_below_sailing_speed_knots.is_finite()
        || !(0.0..=30.0).contains(&request.motor_below_sailing_speed_knots)
        || !request.motor_crossover_hysteresis_knots.is_finite()
        || !(0.0..=10.0).contains(&request.motor_crossover_hysteresis_knots)
        || request.minimum_motor_run_seconds > 86_400
        || request.mode_change_penalty_seconds > 21_600
        || (request.allow_motor
            && (!request.motor_speed_knots.is_finite()
                || !(0.1..=50.0).contains(&request.motor_speed_knots)))
        || (request.allow_motor_sailing
            && (!request.motor_sailing_boost_knots.is_finite()
                || !(0.0..=30.0).contains(&request.motor_sailing_boost_knots)))
        || request
            .maximum_motor_seconds
            .is_some_and(|limit| limit == 0)
        || request
            .fuel_consumption_litres_per_hour
            .is_some_and(|rate| !rate.is_finite() || rate <= 0.0 || rate > 1_000.0)
        || request
            .maximum_fuel_litres
            .is_some_and(|fuel| !fuel.is_finite() || fuel <= 0.0 || fuel > 1_000_000.0)
        || request.maximum_fuel_litres.is_some()
            && request.fuel_consumption_litres_per_hour.is_none()
    {
        return Err("route propulsion policy is invalid".into());
    }
    for limit in [
        request.max_wind_knots,
        request.max_apparent_wind_knots,
        request.max_wave_metres,
        request.max_opposing_wind_current_knots_squared,
    ]
    .into_iter()
    .flatten()
    {
        if !limit.is_finite() || limit < 0.0 {
            return Err("route environmental limit is invalid".into());
        }
    }
    if request.start_latitude.abs() > request.maximum_latitude_degrees
        || request.destination_latitude.abs() > request.maximum_latitude_degrees
    {
        return Err("route endpoint exceeds the maximum latitude".into());
    }
    Ok(())
}

fn motor_budget_allows(request: &RouteRequest, seconds: u64) -> bool {
    if request
        .maximum_motor_seconds
        .is_some_and(|limit| seconds > u64::from(limit))
    {
        return false;
    }
    if let (Some(rate), Some(limit)) = (
        request.fuel_consumption_litres_per_hour,
        request.maximum_fuel_litres,
    ) {
        if rate * seconds as f64 / 3600.0 > limit + 1e-9 {
            return false;
        }
    }
    true
}

struct ValidationSubsegment {
    route_leg: usize,
    start: RoutePoint,
    end: RoutePoint,
}

fn route_point_at_fraction(start: &RoutePoint, end: &RoutePoint, fraction: f64) -> RoutePoint {
    if fraction >= 1.0 {
        return RoutePoint {
            latitude: end.latitude,
            longitude: end.longitude,
            unix_time: end.unix_time,
        };
    }
    let distance = distance_nm(start.latitude, start.longitude, end.latitude, end.longitude);
    let course = bearing(start.latitude, start.longitude, end.latitude, end.longitude);
    let course_radians = radians(course);
    let (latitude, longitude, _) = advance(
        start.latitude,
        start.longitude,
        distance * fraction * course_radians.sin(),
        distance * fraction * course_radians.cos(),
        3600,
    );
    RoutePoint {
        latitude,
        longitude,
        unix_time: start.unix_time
            + ((end.unix_time - start.unix_time) as f64 * fraction).round() as i64,
    }
}

fn validation_subsegments(
    points: &[RoutePoint],
) -> Result<(Vec<ValidationSubsegment>, Vec<std::ops::Range<usize>>), String> {
    let mut subsegments = Vec::new();
    let mut leg_ranges = Vec::with_capacity(points.len().saturating_sub(1));
    for (route_leg, pair) in points.windows(2).enumerate() {
        let seconds = pair[1].unix_time - pair[0].unix_time;
        if seconds <= 0 {
            return Err("independent validation found non-increasing timestamps".into());
        }
        let distance = distance_nm(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        );
        let time_parts = ((seconds + VALIDATION_INTERVAL_SECONDS - 1) / VALIDATION_INTERVAL_SECONDS)
            .max(1) as usize;
        let distance_parts = (distance / VALIDATION_SEGMENT_NM).ceil().max(1.0) as usize;
        let required_parts = time_parts.max(distance_parts);
        if required_parts > 255 {
            return Err(format!(
                "independent validation rejected leg {} because it requires more than 255 bounded probes",
                route_leg + 1
            ));
        }
        // Keep an odd number of probes so the centre probe is the exact
        // original-leg midpoint used by propulsion metrics. This prevents a
        // route from passing probes either side of a narrow coverage gap and
        // then failing only after solver acceptance.
        let mut parts = required_parts;
        if parts % 2 == 0 {
            parts += 1;
        }
        let first = subsegments.len();
        for part in 0..parts {
            subsegments.push(ValidationSubsegment {
                route_leg,
                start: route_point_at_fraction(&pair[0], &pair[1], part as f64 / parts as f64),
                end: route_point_at_fraction(&pair[0], &pair[1], (part + 1) as f64 / parts as f64),
            });
        }
        leg_ranges.push(first..subsegments.len());
    }
    Ok((subsegments, leg_ranges))
}

/// Independently replay exact route geometry.  Delivered routes require the
/// configured destination; prefix checks use the same chronological dynamics
/// without pretending an intermediate search state is an arrival.
fn validate_route_geometry(
    request: &RouteRequest,
    points: &[RoutePoint],
    require_destination: bool,
) -> Result<u64, String> {
    if !require_destination && points.len() == 1 {
        return if distance_nm(
            points[0].latitude,
            points[0].longitude,
            request.start_latitude,
            request.start_longitude,
        ) <= 0.002
        {
            Ok(0)
        } else {
            Err("independent validation found the wrong route start".into())
        };
    }
    if points.len() < 2 {
        return Err("independent validation rejected an empty route".into());
    }
    if distance_nm(
        points[0].latitude,
        points[0].longitude,
        request.start_latitude,
        request.start_longitude,
    ) > 0.002
    {
        return Err("independent validation found the wrong route start".into());
    }
    if require_destination {
        let end = points.last().expect("route was checked non-empty");
        if distance_nm(
            end.latitude,
            end.longitude,
            request.destination_latitude,
            request.destination_longitude,
        ) > request.destination_tolerance_nm + 1e-9
        {
            return Err(
                "independent validation found the route outside destination tolerance".into(),
            );
        }
    }

    let (validation_segments, leg_ranges) = validation_subsegments(points)?;
    let mut samples = Vec::with_capacity(validation_segments.len());
    let mut segments = Vec::with_capacity(validation_segments.len());
    for probe in &validation_segments {
        let longitude_delta = angular_difference(probe.end.longitude, probe.start.longitude);
        samples.push(EnvironmentSampleRequest {
            latitude: (probe.start.latitude + probe.end.latitude) * 0.5,
            longitude: (probe.start.longitude + longitude_delta * 0.5 + 540.0).rem_euclid(360.0)
                - 180.0,
            unix_time: probe.start.unix_time + (probe.end.unix_time - probe.start.unix_time) / 2,
        });
        let segment = GeoSegment {
            start: GeoPoint {
                latitude: probe.start.latitude,
                longitude: probe.start.longitude,
            },
            end: GeoPoint {
                latitude: probe.end.latitude,
                longitude: probe.end.longitude,
            },
        };
        segments.extend(clearance_segments(
            &segment,
            request.land_safety_margin_nautical_miles,
            true,
        ));
    }
    let environments = host::environment_sample_batch(&samples)?;
    if environments.len() != samples.len() {
        return Err("independent validation received the wrong environmental batch length".into());
    }
    // Search-time avoidance may be disabled for diagnosis, but a delivered
    // route must always pass the host's dense final safety service.
    let chart_results = query_final_chart_safety(&segments, request)?;
    if chart_results.len() != segments.len() {
        return Err("independent validation received the wrong chart batch length".into());
    }
    let probes_per_leg = if request.land_safety_margin_nautical_miles > 1e-9 {
        5
    } else {
        1
    };
    if let Some((result_index, rejected)) = chart_results
        .iter()
        .enumerate()
        .find(|(_, result)| result.state != ChartCoverageState::Covered)
    {
        let probe_index = result_index / probes_per_leg;
        let route_leg = validation_segments
            .get(probe_index)
            .map_or(probe_index + 1, |probe| probe.route_leg + 1);
        return Err(format!(
            "independent final chart-safety validation rejected leg {route_leg}: {}",
            rejected.diagnostic
        ));
    }

    // Environmental limits and coverage are checked at every dense probe,
    // not only once at the midpoint of an hour-long routing leg.
    for (probe_index, environment) in environments.iter().enumerate() {
        let route_leg = validation_segments[probe_index].route_leg + 1;
        let (Some(wind_u), Some(wind_v)) = (environment.wind_u_knots, environment.wind_v_knots)
        else {
            return Err(format!(
                "independent validation lost wind coverage on leg {route_leg}"
            ));
        };
        if request
            .max_wind_knots
            .is_some_and(|limit| wind_u.hypot(wind_v) > limit + 1e-9)
        {
            return Err(format!(
                "independent validation exceeded the true-wind limit on leg {route_leg}"
            ));
        }
        if request.use_waves {
            if request.require_wave_data && environment.wave_height_metres.is_none() {
                return Err(format!(
                    "independent validation lost required wave coverage on leg {route_leg}"
                ));
            }
            if request.max_wave_metres.is_some_and(|limit| {
                environment
                    .wave_height_metres
                    .is_some_and(|height| height > limit + 1e-9)
            }) {
                return Err(format!(
                    "independent validation exceeded the wave limit on leg {route_leg}"
                ));
            }
        }
        let current_available =
            environment.current_u_knots.is_some() && environment.current_v_knots.is_some();
        if request.use_currents && request.require_current_data && !current_available {
            return Err(format!(
                "independent validation lost required current coverage on leg {route_leg}"
            ));
        }
        if request
            .max_opposing_wind_current_knots_squared
            .is_some_and(|limit| {
                current_available
                    && opposing_wind_current(
                        wind_u,
                        wind_v,
                        environment.current_u_knots.unwrap_or(0.0),
                        environment.current_v_knots.unwrap_or(0.0),
                    ) > limit + 1e-9
            })
        {
            return Err(format!(
                "independent validation exceeded the wind-against-current limit on leg {route_leg}"
            ));
        }
    }

    let mut previous_tack = 0i8;
    let mut previous_mode = PROPULSION_SAIL;
    let mut motor_seconds = 0u64;
    let mut propulsion_run_seconds = 0u64;
    for (index, pair) in points.windows(2).enumerate() {
        let probe_range = &leg_ranges[index];
        let environment = &environments[probe_range.start + probe_range.len() / 2];
        let (Some(wind_u), Some(wind_v)) = (environment.wind_u_knots, environment.wind_v_knots)
        else {
            return Err(format!(
                "independent validation lost wind coverage on leg {}",
                index + 1
            ));
        };
        let wind = wind_u.hypot(wind_v);
        if request
            .max_wind_knots
            .is_some_and(|limit| wind > limit + 1e-9)
        {
            return Err("independent validation exceeded the true-wind limit".into());
        }
        if request.use_waves {
            if request.require_wave_data && environment.wave_height_metres.is_none() {
                return Err("independent validation lost required wave coverage".into());
            }
            if request.max_wave_metres.is_some_and(|limit| {
                environment
                    .wave_height_metres
                    .is_some_and(|height| height > limit + 1e-9)
            }) {
                return Err("independent validation exceeded the wave limit".into());
            }
        }
        let current_available =
            environment.current_u_knots.is_some() && environment.current_v_knots.is_some();
        if request.use_currents && request.require_current_data && !current_available {
            return Err("independent validation lost required current coverage".into());
        }
        let current_u = if request.use_currents {
            environment.current_u_knots.unwrap_or(0.0)
        } else {
            0.0
        };
        let current_v = if request.use_currents {
            environment.current_v_knots.unwrap_or(0.0)
        } else {
            0.0
        };
        if request
            .max_opposing_wind_current_knots_squared
            .is_some_and(|limit| {
                current_available
                    && opposing_wind_current(wind_u, wind_v, current_u, current_v) > limit + 1e-9
            })
        {
            return Err("independent validation exceeded the wind-against-current limit".into());
        }
        let leg_seconds = (pair[1].unix_time - pair[0].unix_time) as u32;
        if distance_nm(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        ) <= 0.001
        {
            // The graph fallback may explicitly wait for a bounded forecast
            // or tidal gate. Environmental and chart constraints for this
            // stationary interval were checked by the dense probes above.
            if request
                .max_apparent_wind_knots
                .is_some_and(|limit| wind > limit + 1e-9)
            {
                return Err(
                    "independent validation exceeded the apparent-wind limit while waiting".into(),
                );
            }
            continue;
        }
        let kinematics =
            leg_kinematics(&pair[0], &pair[1], current_u, current_v).ok_or_else(|| {
                format!(
                    "independent validation found no usable motion on leg {}",
                    index + 1
                )
            })?;
        let motion = motion_for_heading(
            request,
            wind_u,
            wind_v,
            current_u,
            current_v,
            kinematics.heading_through_water,
            previous_tack,
            previous_mode,
            propulsion_run_seconds,
            leg_seconds,
        )
        .ok_or_else(|| {
            let (twa, _) = true_wind_angle(
                wind_u,
                wind_v,
                kinematics.heading_through_water,
            );
            format!(
                "independent validation rejected leg {}: heading through water {:.1}° (COG {:.1}°) gives TWA {:.1}°, outside the vessel propulsion or wind-angle policy",
                index + 1,
                kinematics.heading_through_water,
                kinematics.course_over_ground,
                twa
            )
        })?;
        previous_tack = motion.tack;
        propulsion_run_seconds = next_propulsion_run_seconds(
            previous_mode,
            motion.propulsion_mode,
            propulsion_run_seconds,
            leg_seconds,
        );
        previous_mode = motion.propulsion_mode;
        if motion.propulsion_mode != PROPULSION_SAIL {
            motor_seconds = motor_seconds.saturating_add(u64::from(leg_seconds));
            if !motor_budget_allows(request, motor_seconds) {
                return Err("independent validation exceeded the motor-time or fuel limit".into());
            }
        }
        // motion_for_heading includes manoeuvre time in its effective water
        // vector.  The delivered effective STW may be lower (reefing or
        // conservative helming), but it must not materially exceed the polar
        // result reconstructed from the independently sampled environment.
        let available_effective_stw =
            (motion.east_knots - current_u).hypot(motion.north_knots - current_v);
        let elapsed_hours = (pair[1].unix_time - pair[0].unix_time) as f64 / 3600.0;
        if !available_effective_stw.is_finite()
            || available_effective_stw <= 0.05
            || kinematics.effective_speed_through_water
                > available_effective_stw * 1.30 + 0.25 / elapsed_hours
        {
            return Err(format!(
                "independent dynamics replay rejected leg {} (COG {:.1}°, heading through water {:.1}°)",
                index + 1,
                kinematics.course_over_ground,
                kinematics.heading_through_water
            ));
        }
    }
    Ok((environments.len()
        + if request.avoid_unsafe_charts {
            segments.len()
        } else {
            0
        }) as u64)
}

/// The final acceptance boundary always performs a fresh dense replay of the
/// exact delivered route, independent of any prefix certification used to
/// avoid repeating known-invalid recovery work.
fn validate_delivered_route(request: &RouteRequest, points: &[RoutePoint]) -> Result<u64, String> {
    validate_route_geometry(request, points, true)
}

fn validate_route_prefix(request: &RouteRequest, points: &[RoutePoint]) -> Result<u64, String> {
    validate_route_geometry(request, points, false)
}

fn usable_environment(
    request: &RouteRequest,
    wind_u: Option<f64>,
    wind_v: Option<f64>,
    current_u: Option<f64>,
    current_v: Option<f64>,
    wave_height: Option<f64>,
) -> Option<(f64, f64, f64, f64)> {
    let (wind_u, wind_v) = (wind_u?, wind_v?);
    if request
        .max_wind_knots
        .is_some_and(|limit| wind_u.hypot(wind_v) > limit)
        || (request.use_waves && request.require_wave_data && wave_height.is_none())
        || (request.use_waves
            && request
                .max_wave_metres
                .is_some_and(|limit| wave_height.is_some_and(|height| height > limit)))
    {
        return None;
    }
    let current_available = current_u.is_some() && current_v.is_some();
    if request.use_currents && request.require_current_data && !current_available {
        return None;
    }
    let (current_u, current_v) = if request.use_currents {
        (current_u.unwrap_or(0.0), current_v.unwrap_or(0.0))
    } else {
        (0.0, 0.0)
    };
    if request
        .max_opposing_wind_current_knots_squared
        .is_some_and(|limit| {
            current_available && opposing_wind_current(wind_u, wind_v, current_u, current_v) > limit
        })
    {
        return None;
    }
    Some((wind_u, wind_v, current_u, current_v))
}

/// Expand one tactical state using the same midpoint predictor/corrector and
/// chart corridor checks as the forward isochrone.  Recovery solvers use this
/// bounded primitive rather than bypassing the portable host's data policy.
fn expand_recovery_node(
    request: &RouteRequest,
    nodes: &[Node],
    node_index: usize,
    target_latitude: f64,
    target_longitude: f64,
    step_seconds: u32,
    corridor: Option<&RouteCorridor>,
) -> Result<Vec<Node>, String> {
    let node = &nodes[node_index];
    let start_samples = host::environment_sample_batch(&[EnvironmentSampleRequest {
        latitude: node.lat,
        longitude: node.lon,
        unix_time: node.time,
    }])?;
    let Some(start) = start_samples.first() else {
        return Err("environment provider returned an empty recovery sample".into());
    };
    let Some((wind_u, wind_v, current_u, current_v)) = usable_environment(
        request,
        start.wind_u_knots,
        start.wind_v_knots,
        start.current_u_knots,
        start.current_v_knots,
        start.wave_height_metres,
    ) else {
        return Ok(Vec::new());
    };
    let target = bearing(node.lat, node.lon, target_latitude, target_longitude);
    let mut drafts = Vec::new();
    for heading in candidate_headings(request, target, wind_u, wind_v) {
        let Some(motion) = motion_for_heading(
            request,
            wind_u,
            wind_v,
            current_u,
            current_v,
            heading,
            node.tack,
            node.propulsion_mode,
            node.propulsion_run_seconds,
            step_seconds,
        ) else {
            continue;
        };
        let (latitude, longitude, _) = advance(
            node.lat,
            node.lon,
            motion.east_knots,
            motion.north_knots,
            step_seconds / 2,
        );
        drafts.push((heading, latitude, longitude));
    }
    let requests: Vec<_> = drafts
        .iter()
        .map(|(_, latitude, longitude)| EnvironmentSampleRequest {
            latitude: *latitude,
            longitude: *longitude,
            unix_time: node.time + i64::from(step_seconds / 2),
        })
        .collect();
    let samples = host::environment_sample_batch(&requests)?;
    if samples.len() != drafts.len() {
        return Err(
            "environment provider returned the wrong recovery midpoint batch length".into(),
        );
    }
    let mut candidates = Vec::new();
    let mut segments = Vec::new();
    let mut ranges = Vec::new();
    for ((heading, _, _), sample) in drafts.into_iter().zip(samples) {
        let Some((wind_u, wind_v, current_u, current_v)) = usable_environment(
            request,
            sample.wind_u_knots,
            sample.wind_v_knots,
            sample.current_u_knots,
            sample.current_v_knots,
            sample.wave_height_metres,
        ) else {
            continue;
        };
        let Some(motion) = motion_for_heading(
            request,
            wind_u,
            wind_v,
            current_u,
            current_v,
            heading,
            node.tack,
            node.propulsion_mode,
            node.propulsion_run_seconds,
            step_seconds,
        ) else {
            continue;
        };
        let next_motor_seconds =
            node.motor_seconds
                .saturating_add(if motion.propulsion_mode == PROPULSION_SAIL {
                    0
                } else {
                    u64::from(step_seconds)
                });
        if !motor_budget_allows(request, next_motor_seconds) {
            continue;
        }
        let (lat, lon, sailed) = advance(
            node.lat,
            node.lon,
            motion.east_knots,
            motion.north_knots,
            step_seconds,
        );
        if !lat.is_finite() || !lon.is_finite() || lat.abs() > request.maximum_latitude_degrees {
            continue;
        }
        if corridor.is_some_and(|route| !route.contains(lat, lon)) {
            continue;
        }
        candidates.push(Node {
            lat,
            lon,
            time: node.time + i64::from(step_seconds),
            parent: Some(node_index),
            sailed_nm: node.sailed_nm + sailed,
            incoming_heading: heading,
            tack: motion.tack,
            propulsion_mode: motion.propulsion_mode,
            motor_seconds: next_motor_seconds,
            propulsion_run_seconds: next_propulsion_run_seconds(
                node.propulsion_mode,
                motion.propulsion_mode,
                node.propulsion_run_seconds,
                step_seconds,
            ),
            propulsion_transitions: node.propulsion_transitions
                + u32::from(motion.propulsion_mode != node.propulsion_mode),
            consecutive_wait_seconds: 0,
            reached_destination: distance_nm(lat, lon, target_latitude, target_longitude)
                <= request.destination_tolerance_nm,
        });
        let segment = GeoSegment {
            start: GeoPoint {
                latitude: node.lat,
                longitude: node.lon,
            },
            end: GeoPoint {
                latitude: lat,
                longitude: lon,
            },
        };
        let first = segments.len();
        segments.extend(clearance_segments(
            &segment,
            request.land_safety_margin_nautical_miles,
            false,
        ));
        ranges.push(first..segments.len());
    }
    if !request.avoid_unsafe_charts {
        return Ok(candidates);
    }
    let chart_results = query_chart_segments(&segments)?;
    Ok(candidates
        .into_iter()
        .enumerate()
        .filter_map(|(index, candidate)| {
            chart_corridor_is_covered(&chart_results[ranges[index].clone()]).then_some(candidate)
        })
        .collect())
}

fn append_greedy_connection(
    request: &RouteRequest,
    nodes: &mut Vec<Node>,
    start: usize,
    target_latitude: f64,
    target_longitude: f64,
    horizon_seconds: u32,
    examined: &mut u32,
    state_limit: u32,
    corridor: Option<&RouteCorridor>,
) -> Result<Option<usize>, String> {
    let step = request.time_step_seconds.min(1800).max(300);
    let mut current = start;
    let deadline = nodes[start].time + i64::from(horizon_seconds);
    let mut stalled = 0u8;
    while nodes[current].time < deadline && *examined < state_limit {
        if host::routing_cancelled() {
            return Err("route calculation cancelled".into());
        }
        let remaining = distance_nm(
            nodes[current].lat,
            nodes[current].lon,
            target_latitude,
            target_longitude,
        );
        if remaining <= request.destination_tolerance_nm {
            return Ok(Some(current));
        }
        let elapsed = (deadline - nodes[current].time).min(i64::from(step)) as u32;
        let mut candidates = expand_recovery_node(
            request,
            nodes,
            current,
            target_latitude,
            target_longitude,
            elapsed,
            corridor,
        )?;
        candidates.sort_by(|left, right| {
            distance_nm(left.lat, left.lon, target_latitude, target_longitude).total_cmp(
                &distance_nm(right.lat, right.lon, target_latitude, target_longitude),
            )
        });
        let Some(mut best) = candidates.into_iter().next() else {
            return Ok(None);
        };
        let next_remaining = distance_nm(best.lat, best.lon, target_latitude, target_longitude);
        stalled = if next_remaining >= remaining - 0.05 {
            stalled.saturating_add(1)
        } else {
            0
        };
        if stalled >= 4 {
            return Ok(None);
        }
        best.reached_destination = next_remaining <= request.destination_tolerance_nm;
        nodes.push(best);
        current = nodes.len() - 1;
        *examined = examined.saturating_add(1);
        if nodes[current].reached_destination {
            return Ok(Some(current));
        }
    }
    Ok(None)
}

fn destination_point(
    latitude: f64,
    longitude: f64,
    bearing_degrees: f64,
    radius_nm: f64,
) -> (f64, f64) {
    let angle = radians(bearing_degrees);
    let (lat, lon, _) = advance(
        latitude,
        longitude,
        radius_nm * angle.sin(),
        radius_nm * angle.cos(),
        3600,
    );
    (lat, lon)
}

/// SuperCPN-style bounded reverse reachability: rank historical forward
/// states from the destination side, then try reproducible forward bridges
/// directly and through destination-centred approach rings.  Every bridge is
/// still sailed forward in time and remains subject to the same host checks.
struct ReverseRecoveryOutcome {
    winner: Option<usize>,
    rejected_candidates: u32,
    last_rejection: Option<String>,
}

fn reverse_isochrone_recovery(
    request: &RouteRequest,
    nodes: &mut Vec<Node>,
    examined: &mut u32,
    state_limit: u32,
    corridor: Option<&RouteCorridor>,
    invalid_prefixes: &mut BTreeSet<usize>,
    progress: ProgressRange,
    progress_base: u8,
    progress_span: u8,
) -> Result<ReverseRecoveryOutcome, String> {
    let mut outcome = ReverseRecoveryOutcome {
        winner: None,
        rejected_candidates: 0,
        last_rejection: None,
    };
    let reverse_state_limit = state_limit.min(request.max_states);
    let mut seeds: Vec<_> = (0..nodes.len()).collect();
    seeds.sort_by(|left, right| {
        distance_nm(
            nodes[*left].lat,
            nodes[*left].lon,
            request.destination_latitude,
            request.destination_longitude,
        )
        .total_cmp(&distance_nm(
            nodes[*right].lat,
            nodes[*right].lon,
            request.destination_latitude,
            request.destination_longitude,
        ))
        .then_with(|| nodes[*right].time.cmp(&nodes[*left].time))
    });
    let mut approaches = Vec::new();
    for radius in [request.destination_tolerance_nm.max(2.0), 5.0, 10.0] {
        for direction in (0..360).step_by(15) {
            approaches.push(destination_point(
                request.destination_latitude,
                request.destination_longitude,
                f64::from(direction),
                radius,
            ));
        }
    }
    const MAX_REVERSE_SEEDS: usize = 32;
    let mut seed_number = 0usize;
    for seed in seeds {
        if ancestry_has_invalid_prefix(nodes, seed, invalid_prefixes) {
            continue;
        }
        if seed_number >= MAX_REVERSE_SEEDS {
            break;
        }
        let seed_indices = route_chain_indices(nodes, seed);
        let seed_chain = route_chain(nodes, seed);
        if let Err(error) = validate_route_prefix(request, &seed_chain) {
            remember_invalid_prefix(&error, &seed_indices, nodes.len(), invalid_prefixes);
            outcome.rejected_candidates = outcome.rejected_candidates.saturating_add(1);
            outcome.last_rejection = Some(error);
            continue;
        }
        seed_number += 1;
        let stage_progress = (seed_number * 4 / MAX_REVERSE_SEEDS).min(4) as u8;
        progress.report(
            progress_base + stage_progress.saturating_mul(progress_span) / 4,
            &format!(
                "Reverse-isocrone recovery: testing frontier bridge {}/{} ({} retained states examined)",
                seed_number,
                MAX_REVERSE_SEEDS,
                examined
            ),
        );
        let checkpoint = nodes.len();
        if let Some(index) = append_greedy_connection(
            request,
            nodes,
            seed,
            request.destination_latitude,
            request.destination_longitude,
            24 * 3600,
            examined,
            reverse_state_limit,
            corridor,
        )? {
            let chain = route_chain(nodes, index);
            match validate_delivered_route(request, &chain) {
                Ok(_) => {
                    outcome.winner = Some(index);
                    return Ok(outcome);
                }
                Err(error) => {
                    let chain_indices = route_chain_indices(nodes, index);
                    remember_invalid_prefix(&error, &chain_indices, checkpoint, invalid_prefixes);
                    outcome.rejected_candidates = outcome.rejected_candidates.saturating_add(1);
                    outcome.last_rejection = Some(error);
                    if outcome.rejected_candidates == 1 || outcome.rejected_candidates % 16 == 0 {
                        progress.report(
                            progress_base
                                + stage_progress.saturating_mul(progress_span) / 4,
                            &format!(
                                "Reverse-isocrone recovery: provisional bridge failed independent replay; continuing with alternative approaches ({} rejected)",
                                outcome.rejected_candidates
                            ),
                        );
                    }
                }
            }
        }
        nodes.truncate(checkpoint);
        approaches.sort_by(|left, right| {
            (distance_nm(nodes[seed].lat, nodes[seed].lon, left.0, left.1)
                + distance_nm(
                    left.0,
                    left.1,
                    request.destination_latitude,
                    request.destination_longitude,
                ))
            .total_cmp(
                &(distance_nm(nodes[seed].lat, nodes[seed].lon, right.0, right.1)
                    + distance_nm(
                        right.0,
                        right.1,
                        request.destination_latitude,
                        request.destination_longitude,
                    )),
            )
        });
        for &(latitude, longitude) in approaches.iter().take(12) {
            let checkpoint = nodes.len();
            let Some(approach) = append_greedy_connection(
                request,
                nodes,
                seed,
                latitude,
                longitude,
                18 * 3600,
                examined,
                reverse_state_limit,
                corridor,
            )?
            else {
                nodes.truncate(checkpoint);
                continue;
            };
            if let Some(index) = append_greedy_connection(
                request,
                nodes,
                approach,
                request.destination_latitude,
                request.destination_longitude,
                12 * 3600,
                examined,
                reverse_state_limit,
                corridor,
            )? {
                let chain = route_chain(nodes, index);
                match validate_delivered_route(request, &chain) {
                    Ok(_) => {
                        outcome.winner = Some(index);
                        return Ok(outcome);
                    }
                    Err(error) => {
                        let chain_indices = route_chain_indices(nodes, index);
                        remember_invalid_prefix(
                            &error,
                            &chain_indices,
                            checkpoint,
                            invalid_prefixes,
                        );
                        outcome.rejected_candidates = outcome.rejected_candidates.saturating_add(1);
                        outcome.last_rejection = Some(error);
                        if outcome.rejected_candidates == 1 || outcome.rejected_candidates % 16 == 0
                        {
                            progress.report(
                                progress_base
                                    + stage_progress.saturating_mul(progress_span) / 4,
                                &format!(
                                    "Reverse-isocrone recovery: provisional approach failed independent replay; continuing ({} rejected)",
                                    outcome.rejected_candidates
                                ),
                            );
                        }
                    }
                }
            }
            nodes.truncate(checkpoint);
        }
    }
    Ok(outcome)
}

struct GraphQueueEntry {
    priority: f64,
    cost: i64,
    serial: u64,
    node: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct GraphLabel {
    cost: i64,
    motor_seconds: u64,
    node: usize,
}

fn graph_label_dominates(left: &GraphLabel, right: &GraphLabel) -> bool {
    left.cost <= right.cost
        && left.motor_seconds <= right.motor_seconds
        && (left.cost < right.cost || left.motor_seconds < right.motor_seconds)
}

/// Retain an evenly distributed bounded sample of the non-dominated
/// time/motor-resource frontier. The fastest and least-motor labels are
/// always retained when at least two labels are allowed.
fn insert_graph_label(labels: &mut Vec<GraphLabel>, candidate: GraphLabel, limit: usize) -> bool {
    if labels.iter().any(|label| {
        graph_label_dominates(label, &candidate)
            || (label.cost == candidate.cost && label.motor_seconds == candidate.motor_seconds)
    }) {
        return false;
    }
    labels.retain(|label| !graph_label_dominates(&candidate, label));
    labels.push(candidate);
    labels.sort_by(|left, right| {
        left.cost
            .cmp(&right.cost)
            .then_with(|| right.motor_seconds.cmp(&left.motor_seconds))
            .then_with(|| left.node.cmp(&right.node))
    });
    if labels.len() <= limit.max(1) {
        return true;
    }
    let source = labels.clone();
    let keep = limit.max(1);
    let mut retained = Vec::with_capacity(keep);
    for index in 0..keep {
        let source_index = if keep == 1 {
            0
        } else {
            index * (source.len() - 1) / (keep - 1)
        };
        let selected = source[source_index];
        if !retained.contains(&selected) {
            retained.push(selected);
        }
    }
    *labels = retained;
    labels.contains(&candidate)
}

fn minimum_run_remaining_bin(request: &RouteRequest, node: &Node, step: u32) -> u32 {
    if node.propulsion_mode == PROPULSION_SAIL {
        return 0;
    }
    let remaining =
        u64::from(request.minimum_motor_run_seconds).saturating_sub(node.propulsion_run_seconds);
    let divisor = u64::from(step.max(1));
    ((remaining + divisor - 1) / divisor) as u32
}

fn maximum_vessel_speed_knots(request: &RouteRequest) -> f64 {
    let polar_max = request
        .polars
        .iter()
        .flat_map(|polar| polar.boat_speeds_knots.iter().copied())
        .filter(|speed| speed.is_finite())
        .fold(0.0, f64::max)
        * request.upwind_efficiency.max(request.downwind_efficiency);
    let mut maximum = polar_max;
    if request.allow_motor_sailing {
        maximum = maximum.max(polar_max + request.motor_sailing_boost_knots);
    }
    if request.allow_motor {
        maximum = maximum.max(request.motor_speed_knots);
    }
    maximum.max(0.0)
}

/// A* may only use a lower bound on remaining time.  The maximum polar/motor
/// speed bounds through-water progress and the portable environment contract's
/// physical current ceiling bounds favourable drift.
fn remaining_time_lower_bound_seconds(request: &RouteRequest, node: &Node) -> f64 {
    let maximum_speed = maximum_vessel_speed_knots(request)
        + if request.use_currents {
            MAX_PROVIDER_CURRENT_SPEED_KNOTS
        } else {
            0.0
        };
    if maximum_speed <= 0.05 {
        return 0.0;
    }
    distance_nm(
        node.lat,
        node.lon,
        request.destination_latitude,
        request.destination_longitude,
    ) / maximum_speed
        * 3600.0
}

/// A bounded stationary loiter is a planning abstraction for waiting out a
/// tide gate or forecast no-go wind. It is deliberately available only while
/// sailing, never consumes motor/fuel, and is independently revalidated.
fn graph_wait_candidate(
    request: &RouteRequest,
    nodes: &[Node],
    node_index: usize,
    step: u32,
) -> Result<Option<Node>, String> {
    let node = &nodes[node_index];
    if node.propulsion_mode != PROPULSION_SAIL
        || node.consecutive_wait_seconds.saturating_add(step) > MAX_GRAPH_WAIT_SECONDS
        || node.time - request.departure_unix_time + i64::from(step)
            > i64::from(request.max_hours) * 3600
    {
        return Ok(None);
    }
    let samples = host::environment_sample_batch(&[EnvironmentSampleRequest {
        latitude: node.lat,
        longitude: node.lon,
        unix_time: node.time + i64::from(step / 2),
    }])?;
    let Some(sample) = samples.first() else {
        return Err("environment provider returned an empty graph-wait sample".into());
    };
    if usable_environment(
        request,
        sample.wind_u_knots,
        sample.wind_v_knots,
        sample.current_u_knots,
        sample.current_v_knots,
        sample.wave_height_metres,
    )
    .is_none()
    {
        return Ok(None);
    }
    if request.max_apparent_wind_knots.is_some_and(|limit| {
        sample
            .wind_u_knots
            .zip(sample.wind_v_knots)
            .is_some_and(|(wind_u, wind_v)| wind_u.hypot(wind_v) > limit)
    }) {
        return Ok(None);
    }
    Ok(Some(Node {
        lat: node.lat,
        lon: node.lon,
        time: node.time + i64::from(step),
        parent: Some(node_index),
        sailed_nm: node.sailed_nm,
        incoming_heading: node.incoming_heading,
        tack: node.tack,
        propulsion_mode: PROPULSION_SAIL,
        motor_seconds: node.motor_seconds,
        propulsion_run_seconds: 0,
        propulsion_transitions: node.propulsion_transitions,
        consecutive_wait_seconds: node.consecutive_wait_seconds.saturating_add(step),
        reached_destination: false,
    }))
}

impl PartialEq for GraphQueueEntry {
    fn eq(&self, other: &Self) -> bool {
        self.priority.total_cmp(&other.priority).is_eq()
            && self.cost == other.cost
            && self.serial == other.serial
    }
}
impl Eq for GraphQueueEntry {}
impl PartialOrd for GraphQueueEntry {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for GraphQueueEntry {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        other
            .priority
            .total_cmp(&self.priority)
            .then_with(|| other.cost.cmp(&self.cost))
            .then_with(|| other.serial.cmp(&self.serial))
    }
}

fn cross_track_nm(request: &RouteRequest, node: &Node) -> f64 {
    let start_to_node = distance_nm(
        request.start_latitude,
        request.start_longitude,
        node.lat,
        node.lon,
    );
    let route_bearing = bearing(
        request.start_latitude,
        request.start_longitude,
        request.destination_latitude,
        request.destination_longitude,
    );
    let node_bearing = bearing(
        request.start_latitude,
        request.start_longitude,
        node.lat,
        node.lon,
    );
    start_to_node * radians(angular_difference(node_bearing, route_bearing)).sin()
}

/// Time-dependent A* fallback over tactical position/time/heading/tack cells.
/// It is deliberately bounded to the remaining portable state budget and a
/// 120 NM passage corridor, matching the reference solver's default domain.
fn time_dependent_graph_fallback(
    request: &RouteRequest,
    nodes: &mut Vec<Node>,
    examined: &mut u32,
    state_limit: u32,
    corridor: Option<&RouteCorridor>,
    invalid_prefixes: &mut BTreeSet<usize>,
    progress: ProgressRange,
    progress_base: u8,
    progress_span: u8,
) -> Result<Option<usize>, String> {
    let step = request.time_step_seconds.min(1800).max(300);
    let mut seeds: Vec<_> = (0..nodes.len()).collect();
    seeds.sort_by(|left, right| {
        candidate_score(request, &nodes[*left]).total_cmp(&candidate_score(request, &nodes[*right]))
    });
    let mut open = BinaryHeap::new();
    let mut labels: BTreeMap<(SearchCell, i64, u32), Vec<GraphLabel>> = BTreeMap::new();
    let mut serial = 0u64;
    let mut accepted_seeds = 0usize;
    for seed in seeds {
        if ancestry_has_invalid_prefix(nodes, seed, invalid_prefixes) {
            continue;
        }
        if accepted_seeds >= 128 {
            break;
        }
        let seed_indices = route_chain_indices(nodes, seed);
        let seed_chain = route_chain(nodes, seed);
        if let Err(error) = validate_route_prefix(request, &seed_chain) {
            remember_invalid_prefix(&error, &seed_indices, nodes.len(), invalid_prefixes);
            continue;
        }
        accepted_seeds += 1;
        let cost = nodes[seed].time - request.departure_unix_time;
        let heuristic = remaining_time_lower_bound_seconds(request, &nodes[seed]);
        open.push(GraphQueueEntry {
            priority: cost as f64 + heuristic,
            cost,
            serial,
            node: seed,
        });
        serial += 1;
        labels
            .entry((
                search_cell(request, &nodes[seed]),
                cost / i64::from(step),
                minimum_run_remaining_bin(request, &nodes[seed], step),
            ))
            .or_default()
            .push(GraphLabel {
                cost,
                motor_seconds: nodes[seed].motor_seconds,
                node: seed,
            });
    }
    let state_limit = state_limit.min(request.max_states);
    let graph_limit = state_limit
        .saturating_sub(*examined)
        .min((request.max_states / 4).max(2_000));
    let mut graph_labels = 0u32;
    let mut direct_attempts = 0u8;
    while let Some(entry) = open.pop() {
        if host::routing_cancelled() {
            return Err("route calculation cancelled".into());
        }
        if graph_labels >= graph_limit || *examined >= state_limit {
            break;
        }
        let entry_key = (
            search_cell(request, &nodes[entry.node]),
            entry.cost / i64::from(step),
            minimum_run_remaining_bin(request, &nodes[entry.node], step),
        );
        if !labels
            .get(&entry_key)
            .is_some_and(|cell| cell.iter().any(|label| label.node == entry.node))
        {
            continue;
        }
        let remaining = distance_nm(
            nodes[entry.node].lat,
            nodes[entry.node].lon,
            request.destination_latitude,
            request.destination_longitude,
        );
        if remaining <= request.destination_tolerance_nm {
            let chain = route_chain(nodes, entry.node);
            match validate_delivered_route(request, &chain) {
                Ok(_) => return Ok(Some(entry.node)),
                Err(error) => {
                    let chain_indices = route_chain_indices(nodes, entry.node);
                    remember_invalid_prefix(&error, &chain_indices, nodes.len(), invalid_prefixes);
                }
            }
        }
        if remaining <= 60.0 && direct_attempts < 8 {
            direct_attempts += 1;
            let checkpoint = nodes.len();
            if let Some(index) = append_greedy_connection(
                request,
                nodes,
                entry.node,
                request.destination_latitude,
                request.destination_longitude,
                24 * 3600,
                examined,
                state_limit,
                corridor,
            )? {
                let chain = route_chain(nodes, index);
                match validate_delivered_route(request, &chain) {
                    Ok(_) => return Ok(Some(index)),
                    Err(error) => {
                        let chain_indices = route_chain_indices(nodes, index);
                        remember_invalid_prefix(
                            &error,
                            &chain_indices,
                            checkpoint,
                            invalid_prefixes,
                        );
                    }
                }
            }
            nodes.truncate(checkpoint);
        }
        if nodes[entry.node].time - request.departure_unix_time
            >= i64::from(request.max_hours) * 3600
        {
            continue;
        }
        let mut candidates = expand_recovery_node(
            request,
            nodes,
            entry.node,
            request.destination_latitude,
            request.destination_longitude,
            step,
            corridor,
        )?;
        // A* needs a bounded local branching factor: retain the best tactical
        // headings by admissible remaining distance while keeping both tack
        // signs represented. The broad fan belongs to the isochrone stage;
        // carrying all of it into every graph vertex exhausts labels before
        // the queue can gain useful depth.
        candidates.sort_by(|left, right| {
            candidate_score(request, left).total_cmp(&candidate_score(request, right))
        });
        let mut graph_candidates = Vec::new();
        for tack in [-1, 1] {
            graph_candidates.extend(
                candidates
                    .iter()
                    .filter(|candidate| candidate.tack == tack)
                    .take(4)
                    .cloned(),
            );
        }
        graph_candidates.sort_by(|left, right| {
            candidate_score(request, left).total_cmp(&candidate_score(request, right))
        });
        graph_candidates.truncate(8);
        if let Some(wait) = graph_wait_candidate(request, nodes, entry.node, step)? {
            graph_candidates.push(wait);
        }
        for candidate in graph_candidates {
            if cross_track_nm(request, &candidate).abs() > 120.0 {
                continue;
            }
            let cost = candidate.time - request.departure_unix_time;
            let key = (
                search_cell(request, &candidate),
                cost / i64::from(step),
                minimum_run_remaining_bin(request, &candidate, step),
            );
            let cell = labels.entry(key).or_default();
            let pending = GraphLabel {
                cost,
                motor_seconds: candidate.motor_seconds,
                node: usize::MAX,
            };
            if !insert_graph_label(cell, pending, usize::from(request.labels_per_cell)) {
                continue;
            }
            nodes.push(candidate);
            let index = nodes.len() - 1;
            cell.iter_mut()
                .find(|label| label.node == usize::MAX)
                .expect("accepted graph label should retain its pending marker")
                .node = index;
            graph_labels = graph_labels.saturating_add(1);
            *examined = examined.saturating_add(1);
            if graph_labels % 256 == 0 {
                let stage_percent = if graph_limit == 0 {
                    0
                } else {
                    (graph_labels.saturating_mul(4) / graph_limit).min(4)
                };
                progress.report(
                    progress_base
                        + (stage_percent as u8).saturating_mul(progress_span) / 4,
                    &format!(
                        "Time-dependent graph fallback: {} graph labels accepted, {} retained states examined, {} queued",
                        graph_labels,
                        examined,
                        open.len()
                    ),
                );
            }
            let heuristic = remaining_time_lower_bound_seconds(request, &nodes[index]);
            open.push(GraphQueueEntry {
                priority: cost as f64 + heuristic,
                cost,
                serial,
                node: index,
            });
            serial += 1;
        }
    }
    Ok(None)
}

fn calculate_pass(
    request: RouteRequest,
    corridor: Option<&RouteCorridor>,
    warm_start: Option<&[RoutePoint]>,
    progress: ProgressRange,
) -> Result<RouteResult, String> {
    validate(&request)?;
    let direct = distance_nm(
        request.start_latitude,
        request.start_longitude,
        request.destination_latitude,
        request.destination_longitude,
    );
    if direct < 0.05 {
        return Err("start and destination are the same".into());
    }
    let mut nodes = vec![Node {
        lat: request.start_latitude,
        lon: request.start_longitude,
        time: request.departure_unix_time,
        parent: None,
        sailed_nm: 0.0,
        tack: 0,
        propulsion_mode: PROPULSION_SAIL,
        motor_seconds: 0,
        propulsion_run_seconds: 0,
        propulsion_transitions: 0,
        consecutive_wait_seconds: 0,
        incoming_heading: 0.0,
        reached_destination: false,
    }];
    let mut invalid_prefixes = BTreeSet::new();
    let mut frontier = vec![0usize];
    let max_layers = request.max_hours.saturating_mul(3600) / request.time_step_seconds;
    // `max_states` is a bound on the feasible labels retained by the search,
    // not on the much larger transient heading fan.  Charging raw candidates
    // here used to exhaust a nominal 80,000-state search after only a few
    // forecast layers, before a cruising yacht could cover even a short
    // coastal passage.
    let mut examined = 1u32;
    let mut generated = 0u64;
    let mut completed_layers = 0u32;
    let mut budget_exhausted = false;
    let mut winner = None;
    let mut solver_path = "forward adaptive isochrone";
    let mut forward_failure = String::new();
    let mut forward_rejected_arrivals = 0u32;
    let mut last_forward_rejection = None;
    // Preserve bounded reverse and graph portions of the declared state budget.
    // Otherwise a difficult forward search can consume every label before the
    // explicitly requested reverse and graph stages begin.
    let forward_state_limit = request.max_states.saturating_mul(3) / 5;
    let interleave_interval = (forward_state_limit / 4).clamp(4_000, 12_000);
    let recovery_slice = (request.max_states / 20).clamp(1_000, 4_000);
    let mut next_interleaved_recovery = interleave_interval;
    let mut recovery_incumbent = warm_start.and_then(detached_route_from_points);
    let mut incumbent_solver_path = if recovery_incumbent.is_some() {
        "validated coarse-route warm start"
    } else {
        "interleaved reverse-isocrone recovery"
    };
    let mut interleaved_recovery_attempts = 0u32;
    let mut progress_floor = 0u8;
    let mut isochrones = Vec::new();
    let mut traces = Vec::new();
    let mut last_inspection_time = request.departure_unix_time;
    let mut sample_requests: Vec<EnvironmentSampleRequest> = Vec::new();
    let mut drafts: Vec<(usize, f64, f64, f64)> = Vec::new();
    let mut midpoint_requests: Vec<EnvironmentSampleRequest> = Vec::new();
    let mut chart_segments: Vec<GeoSegment> = Vec::new();
    let mut chart_ranges: Vec<std::ops::Range<usize>> = Vec::new();

    'forward: for layer in 0..max_layers.max(1) {
        if host::routing_cancelled() {
            return Err("route calculation cancelled".into());
        }
        sample_requests.clear();
        sample_requests.extend(frontier.iter().map(|&index| {
            let node = &nodes[index];
            EnvironmentSampleRequest {
                latitude: node.lat,
                longitude: node.lon,
                unix_time: node.time,
            }
        }));
        let samples = host::environment_sample_batch(&sample_requests)?;
        if samples.len() != frontier.len() {
            return Err("environment provider returned the wrong batch length".into());
        }
        if !samples
            .iter()
            .any(|sample| sample.wind_u_knots.is_some() && sample.wind_v_knots.is_some())
        {
            forward_failure = format!(
                "iGRIB has no wind coverage for forecast step {} at the requested time",
                layer + 1
            );
            break 'forward;
        }
        // First predict each candidate's midpoint from the environment at the
        // start of the step.  The final propagation is evaluated against a
        // fresh sample at that predicted midpoint.  This predictor/corrector
        // scheme makes the search use the same chronological weather point as
        // independent replay; previously a one-hour leg could be admitted by
        // start-of-leg wind and then (correctly) rejected by midpoint wind.
        drafts.clear();
        for (frontier_index, &node_index) in frontier.iter().enumerate() {
            let node = &nodes[node_index];
            let env = &samples[frontier_index];
            let (Some(wind_u), Some(wind_v)) = (env.wind_u_knots, env.wind_v_knots) else {
                continue;
            };
            let wind = wind_u.hypot(wind_v);
            if request.max_wind_knots.is_some_and(|limit| wind > limit) {
                continue;
            }
            if request.use_waves {
                if request.require_wave_data && env.wave_height_metres.is_none() {
                    continue;
                }
                if request
                    .max_wave_metres
                    .is_some_and(|limit| env.wave_height_metres.is_some_and(|h| h > limit))
                {
                    continue;
                }
            }
            let current_available = env.current_u_knots.is_some() && env.current_v_knots.is_some();
            if request.use_currents && request.require_current_data && !current_available {
                continue;
            }
            let current_u = if request.use_currents {
                env.current_u_knots.unwrap_or(0.0)
            } else {
                0.0
            };
            let current_v = if request.use_currents {
                env.current_v_knots.unwrap_or(0.0)
            } else {
                0.0
            };
            if request
                .max_opposing_wind_current_knots_squared
                .is_some_and(|limit| {
                    current_available
                        && opposing_wind_current(wind_u, wind_v, current_u, current_v) > limit
                })
            {
                continue;
            }
            let target = bearing(
                node.lat,
                node.lon,
                request.destination_latitude,
                request.destination_longitude,
            );
            for heading in candidate_headings(&request, target, wind_u, wind_v) {
                let Some(predictor_motion) = motion_for_heading(
                    &request,
                    wind_u,
                    wind_v,
                    current_u,
                    current_v,
                    heading,
                    node.tack,
                    node.propulsion_mode,
                    node.propulsion_run_seconds,
                    request.time_step_seconds,
                ) else {
                    continue;
                };
                let (mid_lat, mid_lon, _) = advance(
                    node.lat,
                    node.lon,
                    predictor_motion.east_knots,
                    predictor_motion.north_knots,
                    request.time_step_seconds / 2,
                );
                drafts.push((node_index, heading, mid_lat, mid_lon));
            }
        }
        if drafts.is_empty() {
            forward_failure =
                "no viable states remain after start-of-step environmental limits".into();
            break 'forward;
        }
        generated = generated.saturating_add(drafts.len() as u64);
        midpoint_requests.clear();
        midpoint_requests.extend(drafts.iter().map(|(node_index, _, latitude, longitude)| {
            EnvironmentSampleRequest {
                latitude: *latitude,
                longitude: *longitude,
                unix_time: nodes[*node_index].time + i64::from(request.time_step_seconds / 2),
            }
        }));
        let midpoint_samples = host::environment_sample_batch(&midpoint_requests)?;
        if midpoint_samples.len() != drafts.len() {
            return Err("environment provider returned the wrong midpoint batch length".into());
        }

        let alternatives_per_cell = usize::from(request.labels_per_cell)
            .saturating_mul(4)
            .max(4);
        let mut preliminary: BTreeMap<SearchCell, Vec<(f64, usize, Node)>> = BTreeMap::new();
        let mut selected_candidates = Vec::new();
        let mut provisional_arrivals: Vec<Node> = Vec::new();
        let mut provisional_sequences = Vec::new();
        let mut arrival_refinements = Vec::new();
        let mut candidate_sequence = 0usize;
        for ((node_index, heading, _, _), env) in
            drafts.iter().copied().zip(midpoint_samples.into_iter())
        {
            let node = &nodes[node_index];
            let (Some(wind_u), Some(wind_v)) = (env.wind_u_knots, env.wind_v_knots) else {
                continue;
            };
            let wind = wind_u.hypot(wind_v);
            if request.max_wind_knots.is_some_and(|limit| wind > limit) {
                continue;
            }
            if request.use_waves {
                if request.require_wave_data && env.wave_height_metres.is_none() {
                    continue;
                }
                if request
                    .max_wave_metres
                    .is_some_and(|limit| env.wave_height_metres.is_some_and(|h| h > limit))
                {
                    continue;
                }
            }
            let current_available = env.current_u_knots.is_some() && env.current_v_knots.is_some();
            if request.use_currents && request.require_current_data && !current_available {
                continue;
            }
            let current_u = if request.use_currents {
                env.current_u_knots.unwrap_or(0.0)
            } else {
                0.0
            };
            let current_v = if request.use_currents {
                env.current_v_knots.unwrap_or(0.0)
            } else {
                0.0
            };
            if request
                .max_opposing_wind_current_knots_squared
                .is_some_and(|limit| {
                    current_available
                        && opposing_wind_current(wind_u, wind_v, current_u, current_v) > limit
                })
            {
                continue;
            }
            let Some(full_step_motion) = motion_for_heading(
                &request,
                wind_u,
                wind_v,
                current_u,
                current_v,
                heading,
                node.tack,
                node.propulsion_mode,
                node.propulsion_run_seconds,
                request.time_step_seconds,
            ) else {
                continue;
            };
            let full_step_candidate = propagated_node(
                &request,
                node_index,
                node,
                heading,
                full_step_motion,
                request.time_step_seconds,
                request.destination_latitude,
                request.destination_longitude,
            );
            // Do not snap a current-displaced ground track onto the
            // destination. A provisionally shortened leg is retained only
            // after two fresh samples at its own changing midpoint.
            let provisional =
                arrival_trial_seconds(&request, node, full_step_motion, request.time_step_seconds)
                    .and_then(|elapsed_seconds| {
                        motion_for_heading(
                            &request,
                            wind_u,
                            wind_v,
                            current_u,
                            current_v,
                            heading,
                            node.tack,
                            node.propulsion_mode,
                            node.propulsion_run_seconds,
                            elapsed_seconds,
                        )
                        .map(|motion| (elapsed_seconds, motion))
                    })
                    .and_then(|(elapsed_seconds, motion)| {
                        propagated_node(
                            &request,
                            node_index,
                            node,
                            heading,
                            motion,
                            elapsed_seconds,
                            request.destination_latitude,
                            request.destination_longitude,
                        )
                        .filter(|candidate| candidate.reached_destination)
                        .map(|candidate| (candidate, elapsed_seconds, motion))
                    });
            let (candidate, refinement) =
                if let Some((candidate, elapsed_seconds, motion)) = provisional {
                    (
                        candidate,
                        Some((
                            node_index,
                            heading,
                            elapsed_seconds,
                            motion,
                            full_step_candidate,
                        )),
                    )
                } else {
                    let Some(candidate) = full_step_candidate else {
                        continue;
                    };
                    (candidate, None)
                };
            if corridor.is_some_and(|route| !route.contains(candidate.lat, candidate.lon)) {
                continue;
            }
            let sequence = candidate_sequence;
            candidate_sequence = candidate_sequence.saturating_add(1);
            if let Some((node_index, heading, elapsed_seconds, motion, fallback)) = refinement {
                let candidate_index = provisional_arrivals.len();
                provisional_arrivals.push(candidate);
                provisional_sequences.push(sequence);
                arrival_refinements.push(ArrivalRefinement {
                    candidate_index,
                    node_index,
                    heading,
                    elapsed_seconds,
                    motion,
                    fallback,
                });
            } else if candidate.lat.is_finite()
                && candidate.lon.is_finite()
                && candidate.lat.abs() <= request.maximum_latitude_degrees
            {
                retain_preliminary_candidate(
                    &request,
                    alternatives_per_cell,
                    sequence,
                    candidate,
                    &mut preliminary,
                    &mut selected_candidates,
                );
            }
        }
        refine_shortened_arrivals(
            &request,
            &nodes,
            &mut provisional_arrivals,
            arrival_refinements,
        )?;
        for (sequence, candidate) in provisional_sequences
            .into_iter()
            .zip(provisional_arrivals.into_iter())
        {
            if candidate.lat.is_finite()
                && candidate.lon.is_finite()
                && candidate.lat.abs() <= request.maximum_latitude_degrees
                && corridor.is_none_or(|route| route.contains(candidate.lat, candidate.lon))
            {
                retain_preliminary_candidate(
                    &request,
                    alternatives_per_cell,
                    sequence,
                    candidate,
                    &mut preliminary,
                    &mut selected_candidates,
                );
            }
        }

        // Chart safety is an expensive host boundary and each route segment
        // expands into several clearance probes.  Reduce geometrically
        // equivalent raw headings first, retaining four times the requested
        // labels per cell so chart rejection still has local alternatives.
        // Destination-reaching candidates are never pre-pruned.
        selected_candidates.extend(
            preliminary
                .into_values()
                .flatten()
                .map(|(_, index, candidate)| (index, candidate)),
        );
        // Restore generation order after the per-cell reduction. This keeps
        // all score ties and later stable sorts deterministic while allowing
        // discarded raw candidates to be released before clearance geometry
        // is allocated.
        selected_candidates.sort_by_key(|(index, _)| *index);
        let candidates: Vec<Node> = selected_candidates
            .into_iter()
            .map(|(_, candidate)| candidate)
            .collect();
        if candidates.is_empty() {
            forward_failure = "no viable states remain after midpoint environmental limits".into();
            break 'forward;
        }

        // Clearance corridors are several probes per propagated segment. Do
        // not construct them for headings which the exact preliminary
        // per-cell reduction has already discarded.
        chart_segments.clear();
        chart_ranges.clear();
        chart_ranges.reserve(candidates.len());
        for candidate in &candidates {
            let parent = &nodes[candidate
                .parent
                .expect("a propagated candidate must have a parent")];
            let segment = GeoSegment {
                start: GeoPoint {
                    latitude: parent.lat,
                    longitude: parent.lon,
                },
                end: GeoPoint {
                    latitude: candidate.lat,
                    longitude: candidate.lon,
                },
            };
            let first = chart_segments.len();
            chart_segments.extend(clearance_segments(
                &segment,
                request.land_safety_margin_nautical_miles,
                false,
            ));
            chart_ranges.push(first..chart_segments.len());
        }

        let chart_results = if request.avoid_unsafe_charts {
            query_chart_segments(&chart_segments)?
        } else {
            Vec::new()
        };
        if request.avoid_unsafe_charts && chart_results.len() != chart_segments.len() {
            return Err("chart service returned the wrong batch length".into());
        }
        // Reduce the feasible fan to a bounded set of spatial/tack/propulsion
        // labels before appending anything to the persistent predecessor
        // arena.  This is the portable analogue of reducing an isochrone
        // frontier: discarded candidates consume neither the retained-state
        // budget nor long-lived Wasm memory.
        let mut bucketed: BTreeMap<SearchCell, Vec<(f64, Node)>> = BTreeMap::new();
        for (candidate_index, candidate) in candidates.into_iter().enumerate() {
            if request.avoid_unsafe_charts
                && !chart_corridor_is_covered(&chart_results[chart_ranges[candidate_index].clone()])
            {
                continue;
            }
            let remaining = distance_nm(
                candidate.lat,
                candidate.lon,
                request.destination_latitude,
                request.destination_longitude,
            );
            if candidate.reached_destination || remaining <= request.destination_tolerance_nm {
                let parent = candidate
                    .parent
                    .expect("a propagated arrival candidate must have a parent");
                if ancestry_has_invalid_prefix(&nodes, parent, &invalid_prefixes) {
                    forward_rejected_arrivals = forward_rejected_arrivals.saturating_add(1);
                    last_forward_rejection = Some(
                        "independent validation already rejected the shared route prefix".into(),
                    );
                    continue;
                }
                let parent_chain = route_chain_indices(&nodes, parent);
                match consider_arrival_candidate_with(
                    &mut nodes,
                    candidate,
                    &mut examined,
                    forward_state_limit,
                    |chain| validate_delivered_route(&request, chain).map(|_| ()),
                ) {
                    ArrivalCandidateDecision::Accepted(index) => {
                        winner = Some(index);
                        break;
                    }
                    ArrivalCandidateDecision::Rejected(error) => {
                        remember_invalid_prefix(
                            &error,
                            &parent_chain,
                            nodes.len(),
                            &mut invalid_prefixes,
                        );
                        forward_rejected_arrivals = forward_rejected_arrivals.saturating_add(1);
                        last_forward_rejection = Some(error);
                        continue;
                    }
                    ArrivalCandidateDecision::BudgetExhausted => {
                        budget_exhausted = true;
                        break;
                    }
                }
            }
            // Quantise in approximate nautical-mile space.  Keeping several
            // labels per cell preserves materially different arrivals while
            // bounding memory and avoiding a fixed latitude/longitude grid.
            let score = remaining + candidate.sailed_nm * 0.04;
            let labels = bucketed
                .entry(search_cell(&request, &candidate))
                .or_default();
            labels.push((score, candidate));
            labels.sort_by(|a, b| a.0.total_cmp(&b.0));
            labels.truncate(usize::from(request.labels_per_cell));
        }
        if winner.is_some() {
            break;
        }
        let mut ranked: Vec<_> = bucketed.into_values().flatten().collect();
        ranked.sort_by(|a, b| a.0.total_cmp(&b.0));
        if ranked.is_empty() {
            forward_failure = format!(
                "no chart-safe route states remain after forecast step {}",
                layer + 1
            );
            break 'forward;
        }
        let frontier_limit = 320usize
            .saturating_mul(usize::from(request.labels_per_cell))
            .min(1280);
        let retained_limit = frontier_limit.min(ranked.len());
        let ranked = sector_balanced_frontier(&request, ranked, retained_limit);
        let remaining_budget = forward_state_limit.saturating_sub(examined) as usize;
        let retain_count = retained_limit.min(remaining_budget);
        if retain_count == 0 {
            budget_exhausted = true;
            break;
        }
        frontier.clear();
        frontier.reserve(retain_count);
        for (_, candidate) in ranked.into_iter().take(retain_count) {
            nodes.push(candidate);
            frontier.push(nodes.len() - 1);
        }
        examined = examined.saturating_add(retain_count as u32);
        completed_layers = layer + 1;
        if retain_count < retained_limit || examined >= forward_state_limit {
            budget_exhausted = true;
        }
        let frontier_time = nodes[frontier[0]].time;
        if request.inspection_interval_seconds.is_some_and(|interval| {
            interval > 0 && frontier_time - last_inspection_time >= i64::from(interval)
        }) {
            let (layer_contours, layer_traces) = inspection_geometry(&request, &nodes, &frontier);
            append_bounded_inspection(&mut isochrones, layer_contours, 8_000, 160_000);
            if request.include_traces {
                append_bounded_inspection(&mut traces, layer_traces, 8_000, 160_000);
            }
            last_inspection_time = frontier_time;
        }
        let percent = ((((layer + 1) * 90) / max_layers.max(1)).min(89) as u8).max(progress_floor);
        progress.report(
            percent,
            &format!(
                "Forward isochrone — forecast step {}: {} feasible states retained ({} raw candidates generated)",
                layer + 1,
                examined,
                generated
            ),
        );
        if recovery_incumbent
            .as_ref()
            .and_then(|route| route.last())
            .is_some_and(|incumbent| frontier_time >= incumbent.time)
        {
            winner = install_detached_route(
                &mut nodes,
                recovery_incumbent
                    .as_ref()
                    .expect("checked recovery incumbent"),
            );
            solver_path = incumbent_solver_path;
            break;
        }
        if winner.is_none()
            && examined >= next_interleaved_recovery
            && examined < forward_state_limit
        {
            interleaved_recovery_attempts = interleaved_recovery_attempts.saturating_add(1);
            progress.report(
                percent,
                &format!(
                    "Forward isochrone checkpoint: trying bounded reverse recovery before resuming ({examined}/{forward_state_limit} forward-stage states)"
                ),
            );
            let checkpoint = nodes.len();
            let reverse_limit = examined
                .saturating_add(recovery_slice)
                .min(forward_state_limit);
            let reverse = reverse_isochrone_recovery(
                &request,
                &mut nodes,
                &mut examined,
                reverse_limit,
                corridor,
                &mut invalid_prefixes,
                progress,
                percent,
                0,
            )?;
            if let Some(index) = reverse.winner {
                if retain_earliest_route(&mut recovery_incumbent, detached_route(&nodes, index)) {
                    incumbent_solver_path = "interleaved reverse-isocrone recovery";
                }
            }
            nodes.truncate(checkpoint);

            if recovery_incumbent.is_none() && examined < forward_state_limit {
                progress.report(
                    percent,
                    "Time-dependent graph fallback: reverse checkpoint was incomplete; trying a bounded graph tranche",
                );
                let graph_limit = examined
                    .saturating_add(recovery_slice)
                    .min(forward_state_limit);
                if let Some(index) = time_dependent_graph_fallback(
                    &request,
                    &mut nodes,
                    &mut examined,
                    graph_limit,
                    corridor,
                    &mut invalid_prefixes,
                    progress,
                    percent,
                    0,
                )? {
                    if retain_earliest_route(&mut recovery_incumbent, detached_route(&nodes, index))
                    {
                        incumbent_solver_path = "interleaved time-dependent graph fallback";
                    }
                }
                nodes.truncate(checkpoint);
            }
            progress_floor = percent;
            next_interleaved_recovery = examined
                .saturating_add(interleave_interval)
                .min(forward_state_limit);
            if recovery_incumbent
                .as_ref()
                .and_then(|route| route.last())
                .is_some_and(|incumbent| frontier_time >= incumbent.time)
            {
                winner = install_detached_route(
                    &mut nodes,
                    recovery_incumbent
                        .as_ref()
                        .expect("checked recovery incumbent"),
                );
                solver_path = incumbent_solver_path;
                break;
            }
        }
        if budget_exhausted {
            break;
        }
    }
    if winner.is_none() {
        if let Some(incumbent) = recovery_incumbent.as_ref() {
            winner = install_detached_route(&mut nodes, incumbent);
            solver_path = incumbent_solver_path;
        }
    }
    if forward_failure.is_empty() && winner.is_none() {
        let reason = if budget_exhausted {
            format!(
                "forward isochrone used its reserved {}-state budget after {} forecast steps",
                forward_state_limit, completed_layers
            )
        } else {
            format!(
                "forward isochrone did not arrive within {} hours ({} layers, {} retained states, {} raw candidates)",
                request.max_hours, completed_layers, examined, generated
            )
        };
        forward_failure = reason;
    }
    if winner.is_none() && forward_rejected_arrivals > 0 {
        forward_failure.push_str(&format!(
            "; independently replayed and rejected {forward_rejected_arrivals} provisional arrival candidate(s){}",
            last_forward_rejection
                .as_ref()
                .map(|error| format!(" (last rejection: {error})"))
                .unwrap_or_default()
        ));
    }

    // A forward endpoint is only provisional.  If independent replay rejects
    // it, continue into recovery instead of returning the validator error as
    // the final answer (the failure mode reported for this Irish Sea route).
    if let Some(index) = winner {
        let chain = route_chain(&nodes, index);
        if let Err(error) = validate_delivered_route(&request, &chain) {
            forward_failure = format!("forward candidate rejected: {error}");
            nodes.truncate(index);
            winner = None;
        }
    }

    if winner.is_none() && nodes.len() > 1 && examined < request.max_states {
        progress.report(
            90,
            &format!(
                "Reverse-isocrone recovery: forward routing was incomplete ({forward_failure}); searching destination-side approach bridges"
            ),
        );
        let checkpoint = nodes.len();
        let graph_reserve = (request.max_states / 5)
            .max(100)
            .min(request.max_states / 3);
        let reverse_limit = request.max_states.saturating_sub(graph_reserve);
        let reverse = reverse_isochrone_recovery(
            &request,
            &mut nodes,
            &mut examined,
            reverse_limit,
            corridor,
            &mut invalid_prefixes,
            progress,
            90,
            4,
        )?;
        if let Some(index) = reverse.winner {
            winner = Some(index);
            solver_path = "reverse-isocrone recovery";
        } else {
            nodes.truncate(checkpoint);
            if reverse.rejected_candidates > 0 {
                forward_failure.push_str(&format!(
                    "; reverse-isocrone recovery rejected {} provisional bridge(s) and found no independently reproducible alternative{}",
                    reverse.rejected_candidates,
                    reverse
                        .last_rejection
                        .as_ref()
                        .map(|error| format!(" (last rejection: {error})"))
                        .unwrap_or_default()
                ));
            } else {
                forward_failure
                    .push_str("; reverse-isocrone recovery found no reproducible bridge");
            }
        }
    }

    if winner.is_none() && examined < request.max_states {
        let graph_mode = if request.use_currents {
            "resource-aware A* with polar/motor and provider current bounds"
        } else {
            "resource-aware A* with a polar/motor speed bound"
        };
        progress.report(
            95,
            &format!(
                "Time-dependent graph fallback: reverse recovery was incomplete; exploring bounded position/time/heading/tack labels using {graph_mode} ({examined}/{} states used)",
                request.max_states
            ),
        );
        if let Some(index) = time_dependent_graph_fallback(
            &request,
            &mut nodes,
            &mut examined,
            request.max_states,
            corridor,
            &mut invalid_prefixes,
            progress,
            95,
            4,
        )? {
            winner = Some(index);
            solver_path = "time-dependent graph fallback";
        } else {
            forward_failure.push_str("; time-dependent graph corridor exhausted without a route");
        }
    }

    let winner = winner.ok_or_else(|| {
        format!(
            "all routing stages failed: {forward_failure} ({} retained states examined)",
            examined
        )
    })?;
    // Re-sample and independently check the exact delivered arrival geometry.
    // Search decisions remain provisional until every chronological leg has
    // passed fresh environmental, vessel-policy and chart checks.
    let winner_node = &nodes[winner];
    if winner_node.reached_destination {
        let chain = route_chain(&nodes, winner);
        progress.report(99, "Authoritative final chart/depth corridor validation");
        let validation_samples = validate_delivered_route(&request, &chain)?;
        let statistics = route_statistics(&request, &chain)?;
        let route_environment = route_environment(&chain)?;
        progress.report(100, "Route complete and independently validated");
        return Ok(RouteResult {
            distance_nautical_miles: winner_node.sailed_nm,
            duration_seconds: (winner_node.time - request.departure_unix_time).max(0) as u64,
            states_examined: examined,
            diagnostic: format!(
                "The {solver_path} stage completed using typed iGRIB samples, polar-optimal tack/gybe headings, and batched host chart checks. The exact delivered geometry passed an independent chronological replay using {validation_samples} fresh environmental/chart samples. Solver cascade used {interleaved_recovery_attempts} bounded recovery checkpoint(s) before final forward/reverse/graph completion."
            ),
            points: chain,
            isochrones,
            traces,
            route_environment,
            average_speed_knots: statistics.average_speed_knots,
            maximum_speed_knots: statistics.maximum_speed_knots,
            average_sog_knots: statistics.average_sog_knots,
            maximum_sog_knots: statistics.maximum_sog_knots,
            average_wind_knots: statistics.average_wind_knots,
            maximum_wind_knots: statistics.maximum_wind_knots,
            average_current_knots: statistics.average_current_knots,
            maximum_current_knots: statistics.maximum_current_knots,
            tacks: statistics.tacks,
            motor_seconds: statistics.motor_seconds,
            estimated_fuel_litres: statistics.estimated_fuel_litres,
            propulsion_transitions: statistics.propulsion_transitions,
            comfort_level: statistics.comfort_level,
        });
    }
    Err("internal routing error: selected arrival is outside destination tolerance".into())
}

fn needs_corridor_refinement(request: &RouteRequest) -> bool {
    request.time_step_seconds > 1800
        || request.spatial_cell_nautical_miles > 1.5
        || request.heading_step_degrees > 5
        || request.refined_heading_step_degrees > 5
}

fn refined_request(mut request: RouteRequest) -> RouteRequest {
    request.time_step_seconds = request.time_step_seconds.min(1800);
    request.spatial_cell_nautical_miles = request.spatial_cell_nautical_miles.min(1.5);
    request.heading_step_degrees = request.heading_step_degrees.min(5);
    request.refined_heading_step_degrees = request
        .refined_heading_step_degrees
        .min(request.heading_step_degrees)
        .min(5);
    request.adaptive_headings = true;
    request
}

fn calculate(request: RouteRequest) -> Result<RouteResult, String> {
    validate(&request)?;
    if !needs_corridor_refinement(&request) {
        return calculate_pass(
            request,
            None,
            None,
            ProgressRange {
                start: 0,
                end: 100,
                prefix: "",
            },
        );
    }

    let coarse_request = request.clone();
    let mut coarse = calculate_pass(
        coarse_request,
        None,
        None,
        ProgressRange {
            start: 0,
            end: 70,
            prefix: "Initial route",
        },
    )?;
    if host::routing_cancelled() {
        return Err("route calculation cancelled".into());
    }
    let half_width_nm = (request.spatial_cell_nautical_miles * 4.0).max(12.0);
    let corridor = RouteCorridor::from_route(&coarse.points, half_width_nm);
    let fine_request = refined_request(request);
    host::routing_progress(
        70,
        &format!(
            "Corridor refinement: rerunning within {:.0} NM at {}-minute / {:.1} NM / {}° resolution",
            half_width_nm,
            fine_request.time_step_seconds / 60,
            fine_request.spatial_cell_nautical_miles,
            fine_request.heading_step_degrees
        ),
    );
    match calculate_pass(
        fine_request,
        Some(&corridor),
        Some(&coarse.points),
        ProgressRange {
            start: 70,
            end: 100,
            prefix: "Corridor refinement",
        },
    ) {
        Ok(mut refined) => {
            let coarse_states = coarse.states_examined;
            let refined_states = refined.states_examined;
            let coarse_diagnostic = coarse.diagnostic.clone();
            let eta_delta = refined.duration_seconds as i128 - coarse.duration_seconds as i128;
            let absolute_delta = eta_delta.unsigned_abs();
            let stability = if absolute_delta <= 5 * 60 {
                "stable within five minutes"
            } else {
                "materially resolution-sensitive"
            };
            refined.diagnostic.push_str(&format!(
                " Automatic corridor refinement completed; fine-resolution ETA changed by {eta_delta:+} seconds versus the initial route ({stability}). The initial pass examined {coarse_states} retained states and the reported fine pass examined {refined_states}; each remained within the configured per-pass bound. Initial-pass diagnostic: {coarse_diagnostic}"
            ));
            host::routing_progress(
                100,
                &format!("Refined route complete — ETA change {eta_delta:+} seconds; {stability}"),
            );
            Ok(refined)
        }
        Err(error) if error == "route calculation cancelled" => Err(error),
        Err(error) => {
            coarse.diagnostic.push_str(&format!(
                " Automatic corridor refinement did not produce a replacement route ({error}); the independently validated initial route was retained."
            ));
            host::routing_progress(
                100,
                "Fine corridor search was incomplete; retained the independently validated initial route",
            );
            Ok(coarse)
        }
    }
}

impl IWeatherRouting {
    fn initialize() -> Result<exports::opencpn::portable::lifecycle::PluginInfo, String> {
        host::register_action(
            ACTION_OPEN,
            "iWeatherRouting",
            "Open portable weather routing",
            Some("resources/iweather-routing.svg"),
        )?;
        host::log(
            LogLevel::Info,
            "iWeatherRouting portable component initialised",
        );
        Ok(exports::opencpn::portable::lifecycle::PluginInfo {
            id: "org.opencpn.iweather-routing".into(),
            name: "iWeatherRouting".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        })
    }
    fn enable() -> Result<(), String> {
        Ok(())
    }
    fn disable() {
        host::log(LogLevel::Info, "iWeatherRouting disabled");
    }
    fn on_action(action_id: String) -> Result<(), String> {
        if action_id == ACTION_OPEN {
            host::open_weather_routing()
        } else {
            Err(format!("unknown iWeatherRouting action: {action_id}"))
        }
    }
    fn on_surface_event(
        surface_id: String,
        control_id: String,
        value_json: String,
    ) -> Result<String, String> {
        if surface_id != "routing-workbench" && surface_id != "weather-routing.main" {
            return Err(format!("unknown iWeatherRouting surface: {surface_id}"));
        }
        if control_id.is_empty() || value_json.len() > 64 * 1024 {
            return Err("invalid weather-routing surface event".into());
        }
        host::setting_set(&format!("surface.{control_id}"), &value_json)?;
        Ok(value_json)
    }
}

impl exports::opencpn::portable::lifecycle::Guest for IWeatherRouting {
    fn initialize() -> Result<exports::opencpn::portable::lifecycle::PluginInfo, ServiceError> {
        IWeatherRouting::initialize().map_err(service_error)
    }
    fn enable() -> Result<(), ServiceError> {
        IWeatherRouting::enable().map_err(service_error)
    }
    fn disable() {
        IWeatherRouting::disable()
    }
    fn on_action(action_id: String) -> Result<(), ServiceError> {
        IWeatherRouting::on_action(action_id).map_err(service_error)
    }
}

impl exports::opencpn::portable::surface_event_sink::Guest for IWeatherRouting {
    fn on_surface_event(
        surface_id: String,
        control_id: String,
        value_json: String,
    ) -> Result<String, ServiceError> {
        IWeatherRouting::on_surface_event(surface_id, control_id, value_json).map_err(service_error)
    }
}

impl exports::opencpn::portable::job_event_sink::Guest for IWeatherRouting {
    fn on_job_event(_: String, _: JobEvent) {}
}

impl exports::opencpn::portable::event_sink::Guest for IWeatherRouting {
    fn on_event(_event: opencpn::portable::types::Event) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::portable::plugin_message_sink::Guest for IWeatherRouting {
    fn on_plugin_message(_message_id: String, _message_body: String) -> Result<(), ServiceError> {
        Ok(())
    }
}

impl exports::opencpn::portable::weather_routing_engine::Guest for IWeatherRouting {
    fn calculate_route(request: RouteRequest) -> Result<RouteResult, ServiceError> {
        calculate(request).map_err(service_error)
    }
}

export!(IWeatherRouting);

#[cfg(test)]
mod tests {
    use super::*;

    fn test_request(use_currents: bool) -> RouteRequest {
        RouteRequest {
            start_latitude: 53.0,
            start_longitude: -5.0,
            destination_latitude: 53.5,
            destination_longitude: -4.0,
            departure_unix_time: 0,
            polars: vec![PolarGrid {
                identity: "test".into(),
                true_wind_speeds_knots: vec![5.0, 20.0],
                true_wind_angles_degrees: vec![40.0, 160.0],
                boat_speeds_knots: vec![4.0, 5.0, 10.0, 12.0],
            }],
            time_step_seconds: 3600,
            heading_step_degrees: 15,
            refined_heading_step_degrees: 5,
            adaptive_headings: true,
            spatial_cell_nautical_miles: 3.0,
            labels_per_cell: 2,
            max_hours: 120,
            max_states: 80_000,
            inspection_interval_seconds: Some(7200),
            include_traces: true,
            avoid_unsafe_charts: true,
            min_true_wind_angle_degrees: 40.0,
            max_true_wind_angle_degrees: 160.0,
            max_wind_knots: Some(50.0),
            max_apparent_wind_knots: Some(50.0),
            max_wave_metres: Some(8.0),
            max_opposing_wind_current_knots_squared: None,
            land_safety_margin_nautical_miles: 0.4,
            minimum_chart_depth_metres: 2.0,
            require_authoritative_chart_safety: true,
            use_currents,
            require_current_data: false,
            use_waves: true,
            require_wave_data: true,
            maximum_latitude_degrees: 89.0,
            upwind_efficiency: 1.0,
            downwind_efficiency: 1.0,
            tack_penalty_seconds: 300,
            gybe_penalty_seconds: 300,
            allow_motor_sailing: false,
            allow_motor: false,
            motor_below_sailing_speed_knots: 3.0,
            motor_speed_knots: 5.5,
            motor_sailing_boost_knots: 1.5,
            motor_crossover_hysteresis_knots: 0.2,
            minimum_motor_run_seconds: 1800,
            mode_change_penalty_seconds: 120,
            maximum_motor_seconds: None,
            fuel_consumption_litres_per_hour: None,
            maximum_fuel_litres: None,
            maximum_search_angle_degrees: 120.0,
            destination_tolerance_nm: 1.0,
        }
    }

    fn test_node() -> Node {
        Node {
            lat: 53.0,
            lon: -5.0,
            time: 0,
            parent: None,
            sailed_nm: 0.0,
            incoming_heading: 0.0,
            tack: 0,
            propulsion_mode: PROPULSION_SAIL,
            motor_seconds: 0,
            propulsion_run_seconds: 0,
            propulsion_transitions: 0,
            consecutive_wait_seconds: 0,
            reached_destination: false,
        }
    }

    #[test]
    fn validated_coarse_route_becomes_a_detached_fine_incumbent() {
        let points = vec![
            RoutePoint {
                latitude: 53.0,
                longitude: -5.0,
                unix_time: 100,
            },
            RoutePoint {
                latitude: 53.1,
                longitude: -5.2,
                unix_time: 3700,
            },
            RoutePoint {
                latitude: 53.2,
                longitude: -5.4,
                unix_time: 7300,
            },
        ];
        let route = detached_route_from_points(&points).expect("valid warm route");
        assert_eq!(route.len(), points.len());
        assert_eq!(route[0].parent, None);
        assert_eq!(route[1].parent, Some(0));
        assert_eq!(route[2].parent, Some(1));
        assert!(route[2].reached_destination);
        assert!(route[2].sailed_nm > route[1].sailed_nm);

        let mut arena = vec![test_node()];
        let winner = install_detached_route(&mut arena, &route).expect("installed warm route");
        let rebuilt = route_chain(&arena, winner);
        assert_eq!(rebuilt.len(), points.len());
        assert_eq!(rebuilt[0].unix_time, 100);
        assert_eq!(rebuilt[2].unix_time, 7300);
    }

    #[test]
    fn interleaved_incumbent_keeps_the_earliest_arrival() {
        let mut slower = vec![test_node()];
        slower[0].time = 500;
        let mut incumbent = Some(slower);
        let mut later = vec![test_node()];
        later[0].time = 600;
        assert!(!retain_earliest_route(&mut incumbent, later));
        let mut earlier = vec![test_node()];
        earlier[0].time = 400;
        assert!(retain_earliest_route(&mut incumbent, earlier));
        assert_eq!(incumbent.unwrap()[0].time, 400);
    }

    fn test_segment() -> GeoSegment {
        GeoSegment {
            start: GeoPoint {
                latitude: 53.0,
                longitude: -5.0,
            },
            end: GeoPoint {
                latitude: 53.1,
                longitude: -4.9,
            },
        }
    }

    #[test]
    fn final_clearance_corridor_adds_crossing_diagonals_only_at_delivery() {
        let segment = test_segment();
        let propagation = clearance_segments(&segment, 0.4, false);
        let final_validation = clearance_segments(&segment, 0.4, true);

        assert_eq!(propagation.len(), 3);
        assert_eq!(final_validation.len(), 5);
        let same_point = |left: &GeoPoint, right: &GeoPoint| {
            (left.latitude - right.latitude).abs() < 1e-12
                && (left.longitude - right.longitude).abs() < 1e-12
        };
        assert!(same_point(
            &final_validation[3].start,
            &propagation[1].start
        ));
        assert!(same_point(&final_validation[3].end, &propagation[2].end));
        assert!(same_point(
            &final_validation[4].start,
            &propagation[2].start
        ));
        assert!(same_point(&final_validation[4].end, &propagation[1].end));
    }

    #[test]
    fn final_clearance_corridor_collapses_to_the_centre_at_zero_margin() {
        assert_eq!(clearance_segments(&test_segment(), 0.0, true).len(), 1);
    }

    #[test]
    fn chart_queries_are_split_without_reordering_results() {
        let segment_count = CHART_SEGMENT_BATCH_LIMIT * 2 + 5;
        let segments = vec![test_segment(); segment_count];
        let mut batch_sizes = Vec::new();
        let mut next_result = 0u32;

        let results = query_chart_segments_with(&segments, |batch| {
            batch_sizes.push(batch.len());
            Ok(batch
                .iter()
                .map(|_| {
                    let result = ChartSegmentResult {
                        state: ChartCoverageState::Covered,
                        charts_considered: next_result,
                        diagnostic: String::new(),
                    };
                    next_result += 1;
                    result
                })
                .collect())
        })
        .expect("large chart query should be split into valid batches");

        assert_eq!(
            batch_sizes,
            vec![CHART_SEGMENT_BATCH_LIMIT, CHART_SEGMENT_BATCH_LIMIT, 5]
        );
        assert_eq!(results.len(), segment_count);
        assert!(
            results
                .iter()
                .enumerate()
                .all(|(index, result)| result.charts_considered == index as u32)
        );
    }

    #[test]
    fn chart_query_batch_length_mismatch_fails_closed() {
        let error = query_chart_segments_with(&[test_segment()], |_| Ok(Vec::new()))
            .expect_err("missing chart results must reject the route");
        assert_eq!(error, "chart service returned the wrong batch length");
    }

    #[test]
    fn delivered_ground_track_is_corrected_for_current_before_wind_policy() {
        let start = RoutePoint {
            latitude: 0.0,
            longitude: 0.0,
            unix_time: 0,
        };
        let end = RoutePoint {
            latitude: 0.1,
            longitude: 0.0,
            unix_time: 3600,
        };
        let kinematics = leg_kinematics(&start, &end, 6.0, 0.0)
            .expect("a northbound ground track in an east-going current is valid");
        assert!(kinematics.course_over_ground < 0.01);
        assert!((kinematics.heading_through_water - 315.0).abs() < 0.2);

        // A northerly makes COG appear to be in the no-go sector, while the
        // actual crabbed heading through water is a valid close-hauled course.
        let (ground_twa, _) = true_wind_angle(0.0, -10.0, kinematics.course_over_ground);
        let (water_twa, _) = true_wind_angle(0.0, -10.0, kinematics.heading_through_water);
        assert!(ground_twa < 0.01);
        assert!(water_twa > 40.0);
    }

    #[test]
    fn graph_heuristic_uses_polar_bound_above_eight_knots() {
        let request = test_request(false);
        let node = test_node();
        assert_eq!(maximum_vessel_speed_knots(&request), 12.0);
        let expected = distance_nm(
            node.lat,
            node.lon,
            request.destination_latitude,
            request.destination_longitude,
        ) / 12.0
            * 3600.0;
        assert!((remaining_time_lower_bound_seconds(&request, &node) - expected).abs() < 1e-6);
    }

    #[test]
    fn graph_heuristic_uses_the_provider_current_ceiling() {
        let request = test_request(true);
        let node = test_node();
        let expected = distance_nm(
            node.lat,
            node.lon,
            request.destination_latitude,
            request.destination_longitude,
        ) / (12.0 + MAX_PROVIDER_CURRENT_SPEED_KNOTS)
            * 3600.0;
        assert!((remaining_time_lower_bound_seconds(&request, &node) - expected).abs() < 1e-6);
    }

    #[test]
    fn independently_rejected_prefix_invalidates_only_its_descendants() {
        let mut nodes = vec![test_node(), test_node(), test_node(), test_node()];
        nodes[0].parent = None;
        nodes[1].parent = Some(0);
        nodes[2].parent = Some(1);
        nodes[3].parent = Some(0);
        let chain = route_chain_indices(&nodes, 2);
        let mut invalid = BTreeSet::new();
        remember_invalid_prefix(
            "independent validation rejected leg 2: wind-angle policy",
            &chain,
            nodes.len(),
            &mut invalid,
        );
        assert!(invalid.contains(&2));
        assert!(ancestry_has_invalid_prefix(&nodes, 2, &invalid));
        assert!(!ancestry_has_invalid_prefix(&nodes, 1, &invalid));
        assert!(!ancestry_has_invalid_prefix(&nodes, 3, &invalid));
    }

    #[test]
    fn validation_subdivides_long_legs_by_time_and_distance() {
        let points = vec![
            RoutePoint {
                latitude: 53.0,
                longitude: -5.0,
                unix_time: 0,
            },
            RoutePoint {
                latitude: 53.1,
                longitude: -5.0,
                unix_time: 3600,
            },
        ];
        let (segments, ranges) =
            validation_subsegments(&points).expect("a valid leg should subdivide");
        assert_eq!(ranges.len(), 1);
        assert!(segments.len() >= 4);
        assert!(segments.iter().all(|segment| {
            segment.end.unix_time - segment.start.unix_time <= VALIDATION_INTERVAL_SECONDS
                && distance_nm(
                    segment.start.latitude,
                    segment.start.longitude,
                    segment.end.latitude,
                    segment.end.longitude,
                ) <= VALIDATION_SEGMENT_NM + 0.01
        }));
        assert!(segments.iter().any(|segment| {
            segment.start.unix_time + (segment.end.unix_time - segment.start.unix_time) / 2 == 1800
        }));

        let mut unbounded = points;
        unbounded[1].unix_time = VALIDATION_INTERVAL_SECONDS * 256;
        assert!(matches!(
            validation_subsegments(&unbounded),
            Err(error) if error.contains("more than 255")
        ));
    }

    #[test]
    fn rejected_arrival_is_removed_before_trying_the_next_candidate() {
        let mut nodes = vec![test_node()];
        let mut examined = 1;
        let mut first = test_node();
        first.parent = Some(0);
        first.time = 1800;
        first.lon = -4.95;
        first.reached_destination = true;

        let rejected =
            consider_arrival_candidate_with(&mut nodes, first, &mut examined, 10, |_| {
                Err("provisional final leg is outside the TWA policy".into())
            });
        assert!(matches!(
            rejected,
            ArrivalCandidateDecision::Rejected(ref error)
                if error.contains("outside the TWA policy")
        ));
        assert_eq!(nodes.len(), 1);
        assert_eq!(examined, 2);

        let mut second = test_node();
        second.parent = Some(0);
        second.time = 1800;
        second.lon = -4.96;
        second.reached_destination = true;
        let accepted =
            consider_arrival_candidate_with(&mut nodes, second, &mut examined, 10, |_| Ok(()));
        assert!(matches!(accepted, ArrivalCandidateDecision::Accepted(1)));
        assert_eq!(nodes.len(), 2);
        assert_eq!(examined, 3);
    }

    #[test]
    fn shortened_arrival_is_resampled_at_its_actual_midpoint() {
        let mut request = test_request(false);
        request.start_latitude = 0.0;
        request.start_longitude = 0.0;
        request.destination_latitude = 0.0;
        request.destination_longitude = 0.05;
        request.avoid_unsafe_charts = false;
        request.use_waves = false;
        request.require_wave_data = false;
        let mut root = test_node();
        root.lat = request.start_latitude;
        root.lon = request.start_longitude;
        let nodes = vec![root];
        let provisional_motion = Motion {
            east_knots: 6.0,
            north_knots: 0.0,
            tack: -1,
            propulsion_mode: PROPULSION_SAIL,
        };
        let provisional_seconds = arrival_trial_seconds(
            &request,
            &nodes[0],
            provisional_motion,
            request.time_step_seconds,
        )
        .expect("the provisional motion should cross destination tolerance");
        let provisional = propagated_node(
            &request,
            0,
            &nodes[0],
            90.0,
            provisional_motion,
            provisional_seconds,
            request.destination_latitude,
            request.destination_longitude,
        )
        .expect("the provisional arrival should be usable");
        assert!(provisional.reached_destination);
        let fallback = propagated_node(
            &request,
            0,
            &nodes[0],
            90.0,
            provisional_motion,
            request.time_step_seconds,
            request.destination_latitude,
            request.destination_longitude,
        );
        let mut candidates = vec![provisional];
        let refinement = ArrivalRefinement {
            candidate_index: 0,
            node_index: 0,
            heading: 90.0,
            elapsed_seconds: provisional_seconds,
            motion: provisional_motion,
            fallback,
        };
        let mut sampled_midpoints = Vec::new();
        refine_shortened_arrivals_with(
            &request,
            &nodes,
            &mut candidates,
            vec![refinement],
            |requests| {
                sampled_midpoints.extend_from_slice(requests);
                Ok(requests
                    .iter()
                    .map(|_| EnvironmentSample {
                        wind_u_knots: Some(0.0),
                        wind_v_knots: Some(10.0),
                        current_u_knots: Some(0.0),
                        current_v_knots: Some(0.0),
                        wave_height_metres: None,
                    })
                    .collect())
            },
        )
        .expect("shortened arrival refinement should succeed");

        assert_eq!(sampled_midpoints.len(), 2);
        assert!(
            sampled_midpoints
                .iter()
                .all(|sample| sample.unix_time > 0 && sample.unix_time < 1800)
        );
        assert_eq!(candidates.len(), 1);
        assert!(candidates[0].reached_destination);
        assert!(candidates[0].time < i64::from(request.time_step_seconds));
    }

    #[test]
    fn invalid_shortened_arrival_keeps_the_valid_full_step_candidate() {
        let mut request = test_request(false);
        request.start_latitude = 0.0;
        request.start_longitude = 0.0;
        request.destination_latitude = 0.0;
        request.destination_longitude = 0.05;
        request.avoid_unsafe_charts = false;
        request.use_waves = false;
        request.require_wave_data = false;
        let mut root = test_node();
        root.lat = request.start_latitude;
        root.lon = request.start_longitude;
        let nodes = vec![root];
        let motion = Motion {
            east_knots: 6.0,
            north_knots: 0.0,
            tack: -1,
            propulsion_mode: PROPULSION_SAIL,
        };
        let elapsed_seconds =
            arrival_trial_seconds(&request, &nodes[0], motion, request.time_step_seconds)
                .expect("the provisional motion should cross destination tolerance");
        let provisional = propagated_node(
            &request,
            0,
            &nodes[0],
            90.0,
            motion,
            elapsed_seconds,
            request.destination_latitude,
            request.destination_longitude,
        )
        .expect("the provisional arrival should be usable");
        let fallback = propagated_node(
            &request,
            0,
            &nodes[0],
            90.0,
            motion,
            request.time_step_seconds,
            request.destination_latitude,
            request.destination_longitude,
        )
        .expect("the complete routing step should remain usable");
        let fallback_time = fallback.time;
        let mut candidates = vec![provisional];

        refine_shortened_arrivals_with(
            &request,
            &nodes,
            &mut candidates,
            vec![ArrivalRefinement {
                candidate_index: 0,
                node_index: 0,
                heading: 90.0,
                elapsed_seconds,
                motion,
                fallback: Some(fallback),
            }],
            |requests| {
                Ok(requests
                    .iter()
                    .map(|_| EnvironmentSample {
                        wind_u_knots: None,
                        wind_v_knots: None,
                        current_u_knots: None,
                        current_v_knots: None,
                        wave_height_metres: None,
                    })
                    .collect())
            },
        )
        .expect("an unavailable shortened midpoint should use the complete step");

        assert_eq!(candidates.len(), 1);
        assert_eq!(candidates[0].time, fallback_time);
    }

    #[test]
    fn graph_labels_preserve_fast_and_low_motor_tradeoffs() {
        let mut labels = Vec::new();
        assert!(insert_graph_label(
            &mut labels,
            GraphLabel {
                cost: 100,
                motor_seconds: 1000,
                node: 1,
            },
            2,
        ));
        assert!(insert_graph_label(
            &mut labels,
            GraphLabel {
                cost: 110,
                motor_seconds: 500,
                node: 2,
            },
            2,
        ));
        assert!(insert_graph_label(
            &mut labels,
            GraphLabel {
                cost: 120,
                motor_seconds: 0,
                node: 3,
            },
            2,
        ));
        assert_eq!(
            labels.iter().map(|label| label.node).collect::<Vec<_>>(),
            vec![1, 3]
        );
    }

    #[test]
    fn fine_request_uses_thirty_minute_five_degree_resolution() {
        let refined = refined_request(test_request(true));
        assert_eq!(refined.time_step_seconds, 1800);
        assert_eq!(refined.heading_step_degrees, 5);
        assert_eq!(refined.refined_heading_step_degrees, 5);
        assert_eq!(refined.spatial_cell_nautical_miles, 1.5);
    }
}
