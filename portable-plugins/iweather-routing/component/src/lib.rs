wit_bindgen::generate!({
    path: "../../../portable-runtime/wit",
    world: "plugin-world",
});

use exports::opencpn::portable::plugin::{
    PolarGrid, RouteEnvironmentPoint, RouteInspectionLine, RoutePoint, RouteRequest, RouteResult,
};
use opencpn::portable::host::{
    self, ChartCoverageState, EnvironmentSampleRequest, GeoPoint, GeoSegment, LogLevel,
};
use std::collections::{BTreeSet, HashMap};

struct IWeatherRouting;
const ACTION_OPEN: &str = "iweather-routing.open";
const EARTH_NM: f64 = 3440.065;

#[derive(Clone)]
struct Node {
    lat: f64,
    lon: f64,
    time: i64,
    parent: Option<usize>,
    sailed_nm: f64,
    tack: i8,
    propulsion_mode: u8,
    motor_seconds: u64,
    propulsion_run_seconds: u64,
    propulsion_transitions: u32,
    reached_destination: bool,
}

const PROPULSION_SAIL: u8 = 0;
const PROPULSION_MOTOR_SAIL: u8 = 1;
const PROPULSION_MOTOR: u8 = 2;

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
    for angle in [
        request.min_true_wind_angle_degrees,
        request.max_true_wind_angle_degrees,
    ] {
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
    let requests: Vec<_> = points[..points.len() - 1]
        .iter()
        .map(|point| EnvironmentSampleRequest {
            latitude: point.latitude,
            longitude: point.longitude,
            unix_time: point.unix_time,
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
    for (index, pair) in points.windows(2).enumerate() {
        let elapsed_hours = (pair[1].unix_time - pair[0].unix_time) as f64 / 3600.0;
        if elapsed_hours <= 0.0 {
            continue;
        }
        let heading = bearing(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        );
        let sog = distance_nm(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        ) / elapsed_hours;
        let sample = &samples[index];
        let (wind_u, wind_v) = match (sample.wind_u_knots, sample.wind_v_knots) {
            (Some(u), Some(v)) => (u, v),
            _ => return Err("completed route lost wind coverage while calculating metrics".into()),
        };
        let wind = wind_u.hypot(wind_v);
        let (twa, tack) = true_wind_angle(wind_u, wind_v, heading);
        if previous_tack != 0 && tack != previous_tack && twa <= 90.0 {
            tacks += 1;
        }
        let (current_u, current_v, has_current) =
            match (sample.current_u_knots, sample.current_v_knots) {
                (Some(u), Some(v)) => (u, v, true),
                _ => (0.0, 0.0, false),
            };
        let heading_radians = radians(heading);
        let leg_seconds = (pair[1].unix_time - pair[0].unix_time).max(1) as u32;
        let motion = motion_for_heading(
            request,
            wind_u,
            wind_v,
            current_u,
            current_v,
            heading,
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
        let speed = (sog * heading_radians.sin() - current_u)
            .hypot(sog * heading_radians.cos() - current_v);
        speed_total += speed;
        speed_max = speed_max.max(speed);
        sog_total += sog;
        sog_max = sog_max.max(sog);
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
    let count = (points.len() - 1) as f64;
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

struct Motion {
    east_knots: f64,
    north_knots: f64,
    speed_through_water: f64,
    tack: i8,
    propulsion_mode: u8,
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
        speed_through_water: speed,
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

fn opposing_wind_current(wind_u: f64, wind_v: f64, current_u: f64, current_v: f64) -> f64 {
    -(wind_u * current_u + wind_v * current_v)
}

/// Return the centre segment and conservative parallel probes at the
/// configured clearance.  The host owns land/chart data; the portable engine
/// only sends value geometry.  Requiring every probe to be covered turns the
/// nominal line into a bounded safety corridor without exposing chart
/// objects across the runtime boundary.
fn clearance_segments(segment: &GeoSegment, margin_nm: f64) -> Vec<GeoSegment> {
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
    for side in [-1.0, 1.0] {
        let latitude_offset = side * perpendicular_north * margin_nm / 60.0;
        let longitude_offset = side * perpendicular_east * margin_nm / (60.0 * longitude_scale);
        result.push(GeoSegment {
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
    result
}

fn chart_corridor_is_covered(results: &[opencpn::portable::host::ChartSegmentResult]) -> bool {
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

/// Independently replay the exact delivered geometry.  This deliberately
/// re-samples the immutable environmental dataset and chart service instead
/// of trusting search-state decisions.  A route cannot be returned as a
/// success unless every chronological leg passes this boundary.
fn validate_delivered_route(request: &RouteRequest, points: &[RoutePoint]) -> Result<u64, String> {
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
    let end = points.last().expect("route was checked non-empty");
    if distance_nm(
        end.latitude,
        end.longitude,
        request.destination_latitude,
        request.destination_longitude,
    ) > request.destination_tolerance_nm + 1e-9
    {
        return Err("independent validation found the route outside destination tolerance".into());
    }

    let mut samples = Vec::with_capacity(points.len() - 1);
    let mut segments = Vec::with_capacity(points.len() - 1);
    for pair in points.windows(2) {
        if pair[1].unix_time <= pair[0].unix_time {
            return Err("independent validation found non-increasing timestamps".into());
        }
        let longitude_delta = angular_difference(pair[1].longitude, pair[0].longitude);
        samples.push(EnvironmentSampleRequest {
            latitude: (pair[0].latitude + pair[1].latitude) * 0.5,
            longitude: (pair[0].longitude + longitude_delta * 0.5 + 540.0).rem_euclid(360.0)
                - 180.0,
            unix_time: pair[0].unix_time + (pair[1].unix_time - pair[0].unix_time) / 2,
        });
        let segment = GeoSegment {
            start: GeoPoint {
                latitude: pair[0].latitude,
                longitude: pair[0].longitude,
            },
            end: GeoPoint {
                latitude: pair[1].latitude,
                longitude: pair[1].longitude,
            },
        };
        segments.extend(clearance_segments(
            &segment,
            request.land_safety_margin_nautical_miles,
        ));
    }
    let environments = host::environment_sample_batch(&samples)?;
    if environments.len() != samples.len() {
        return Err("independent validation received the wrong environmental batch length".into());
    }
    if request.avoid_unsafe_charts {
        let chart_results = host::charts_query_segments(&segments)?;
        if chart_results.len() != segments.len() {
            return Err("independent validation received the wrong chart batch length".into());
        }
        let probes_per_leg = if request.land_safety_margin_nautical_miles > 1e-9 {
            3
        } else {
            1
        };
        if chart_results
            .chunks(probes_per_leg)
            .any(|results| !chart_corridor_is_covered(results))
        {
            return Err(
                "independent validation rejected unsafe or uncovered route geometry".into(),
            );
        }
    }

    let mut previous_tack = 0i8;
    let mut previous_mode = PROPULSION_SAIL;
    let mut motor_seconds = 0u64;
    let mut propulsion_run_seconds = 0u64;
    for (index, pair) in points.windows(2).enumerate() {
        let environment = &environments[index];
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
        let heading = bearing(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        );
        let leg_seconds = (pair[1].unix_time - pair[0].unix_time) as u32;
        let motion = motion_for_heading(
            request,
            wind_u,
            wind_v,
            current_u,
            current_v,
            heading,
            previous_tack,
            previous_mode,
            propulsion_run_seconds,
            leg_seconds,
        )
        .ok_or_else(|| {
            "independent validation rejected the vessel propulsion or wind-angle policy".to_string()
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
        let speed_through_water = motion.speed_through_water;
        let heading_radians = radians(heading);
        let boat_east = speed_through_water * heading_radians.sin();
        let boat_north = speed_through_water * heading_radians.cos();
        // Current is added exactly once.  Compare the delivered leg against
        // independently reconstructed along-track progress with a small
        // allowance for spherical interpolation and manoeuvre time.
        let ground_east = boat_east + current_u;
        let ground_north = boat_north + current_v;
        let along_track =
            ground_east * heading_radians.sin() + ground_north * heading_radians.cos();
        let elapsed_hours = (pair[1].unix_time - pair[0].unix_time) as f64 / 3600.0;
        let delivered_distance = distance_nm(
            pair[0].latitude,
            pair[0].longitude,
            pair[1].latitude,
            pair[1].longitude,
        );
        if !along_track.is_finite()
            || along_track <= 0.05
            || delivered_distance > along_track * elapsed_hours * 1.30 + 0.25
        {
            return Err(format!(
                "independent dynamics replay rejected leg {}",
                index + 1
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

fn calculate(request: RouteRequest) -> Result<RouteResult, String> {
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
        reached_destination: false,
    }];
    let mut frontier = vec![0usize];
    let max_layers = request.max_hours.saturating_mul(3600) / request.time_step_seconds;
    let mut examined = 0u32;
    let mut winner = None;
    let mut isochrones = Vec::new();
    let mut traces = Vec::new();
    let mut last_inspection_time = request.departure_unix_time;

    for layer in 0..max_layers.max(1) {
        if host::routing_cancelled() {
            return Err("route calculation cancelled".into());
        }
        let sample_requests: Vec<_> = frontier
            .iter()
            .map(|&index| {
                let node = &nodes[index];
                EnvironmentSampleRequest {
                    latitude: node.lat,
                    longitude: node.lon,
                    unix_time: node.time,
                }
            })
            .collect();
        let samples = host::environment_sample_batch(&sample_requests)?;
        if samples.len() != frontier.len() {
            return Err("environment provider returned the wrong batch length".into());
        }
        if !samples
            .iter()
            .any(|sample| sample.wind_u_knots.is_some() && sample.wind_v_knots.is_some())
        {
            return Err(format!(
                "iGRIB has no wind coverage for forecast step {} at the requested time",
                layer + 1
            ));
        }
        let mut candidates: Vec<Node> = Vec::new();
        let mut chart_segments: Vec<GeoSegment> = Vec::new();
        let mut chart_ranges: Vec<std::ops::Range<usize>> = Vec::new();
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
            let direct_distance = distance_nm(
                node.lat,
                node.lon,
                request.destination_latitude,
                request.destination_longitude,
            );
            if let Some(direct_motion) = motion_for_heading(
                &request,
                wind_u,
                wind_v,
                current_u,
                current_v,
                target,
                node.tack,
                node.propulsion_mode,
                node.propulsion_run_seconds,
                request.time_step_seconds,
            ) {
                let target_rad = radians(target);
                let progress = direct_motion.east_knots * target_rad.sin()
                    + direct_motion.north_knots * target_rad.cos();
                let seconds = if progress > 0.05 {
                    (direct_distance / progress * 3600.0).ceil() as u32
                } else {
                    u32::MAX
                };
                let next_motor_seconds = node.motor_seconds.saturating_add(
                    if direct_motion.propulsion_mode == PROPULSION_SAIL {
                        0
                    } else {
                        u64::from(seconds)
                    },
                );
                let next_run_seconds = next_propulsion_run_seconds(
                    node.propulsion_mode,
                    direct_motion.propulsion_mode,
                    node.propulsion_run_seconds,
                    seconds,
                );
                if seconds > 0
                    && seconds <= request.time_step_seconds
                    && motor_budget_allows(&request, next_motor_seconds)
                {
                    candidates.push(Node {
                        lat: request.destination_latitude,
                        lon: request.destination_longitude,
                        time: node.time + seconds as i64,
                        parent: Some(node_index),
                        sailed_nm: node.sailed_nm + direct_distance,
                        tack: direct_motion.tack,
                        propulsion_mode: direct_motion.propulsion_mode,
                        motor_seconds: next_motor_seconds,
                        propulsion_run_seconds: next_run_seconds,
                        propulsion_transitions: node.propulsion_transitions
                            + u32::from(direct_motion.propulsion_mode != node.propulsion_mode),
                        reached_destination: true,
                    });
                    let segment = GeoSegment {
                        start: GeoPoint {
                            latitude: node.lat,
                            longitude: node.lon,
                        },
                        end: GeoPoint {
                            latitude: request.destination_latitude,
                            longitude: request.destination_longitude,
                        },
                    };
                    let first = chart_segments.len();
                    chart_segments.extend(clearance_segments(
                        &segment,
                        request.land_safety_margin_nautical_miles,
                    ));
                    chart_ranges.push(first..chart_segments.len());
                }
            }
            for heading in candidate_headings(&request, target, wind_u, wind_v) {
                let Some(motion) = motion_for_heading(
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
                let next_motor_seconds = node.motor_seconds.saturating_add(
                    if motion.propulsion_mode == PROPULSION_SAIL {
                        0
                    } else {
                        u64::from(request.time_step_seconds)
                    },
                );
                let next_run_seconds = next_propulsion_run_seconds(
                    node.propulsion_mode,
                    motion.propulsion_mode,
                    node.propulsion_run_seconds,
                    request.time_step_seconds,
                );
                if !motor_budget_allows(&request, next_motor_seconds) {
                    continue;
                }
                let (lat, lon, sailed) = advance(
                    node.lat,
                    node.lon,
                    motion.east_knots,
                    motion.north_knots,
                    request.time_step_seconds,
                );
                if !lat.is_finite()
                    || !lon.is_finite()
                    || lat.abs() > request.maximum_latitude_degrees
                {
                    continue;
                }
                candidates.push(Node {
                    lat,
                    lon,
                    time: node.time + request.time_step_seconds as i64,
                    parent: Some(node_index),
                    sailed_nm: node.sailed_nm + sailed,
                    tack: motion.tack,
                    propulsion_mode: motion.propulsion_mode,
                    motor_seconds: next_motor_seconds,
                    propulsion_run_seconds: next_run_seconds,
                    propulsion_transitions: node.propulsion_transitions
                        + u32::from(motion.propulsion_mode != node.propulsion_mode),
                    reached_destination: false,
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
                let first = chart_segments.len();
                chart_segments.extend(clearance_segments(
                    &segment,
                    request.land_safety_margin_nautical_miles,
                ));
                chart_ranges.push(first..chart_segments.len());
            }
        }
        if candidates.is_empty() {
            return Err("no viable states remain after environmental limits".into());
        }
        let chart_results = if request.avoid_unsafe_charts {
            host::charts_query_segments(&chart_segments)?
        } else {
            Vec::new()
        };
        if request.avoid_unsafe_charts && chart_results.len() != chart_segments.len() {
            return Err("chart service returned the wrong batch length".into());
        }
        let mut bucketed: HashMap<(i32, i32, i8, u8), Vec<(f64, usize)>> = HashMap::new();
        for (candidate_index, candidate) in candidates.into_iter().enumerate() {
            examined = examined.saturating_add(1);
            if examined >= request.max_states {
                break;
            }
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
            nodes.push(candidate);
            let index = nodes.len() - 1;
            if nodes[index].reached_destination || remaining <= request.destination_tolerance_nm {
                winner = Some(index);
                break;
            }
            // Quantise in approximate nautical-mile space.  Keeping several
            // labels per cell preserves materially different arrivals while
            // bounding memory and avoiding a fixed latitude/longitude grid.
            let cell = request.spatial_cell_nautical_miles;
            let longitude_scale = nodes[index].lat.to_radians().cos().abs().max(0.05);
            let key = (
                (nodes[index].lat * 60.0 / cell).round() as i32,
                (nodes[index].lon * 60.0 * longitude_scale / cell).round() as i32,
                nodes[index].tack,
                nodes[index].propulsion_mode,
            );
            let score = remaining + nodes[index].sailed_nm * 0.04;
            let labels = bucketed.entry(key).or_default();
            labels.push((score, index));
            labels.sort_by(|a, b| a.0.total_cmp(&b.0));
            labels.truncate(usize::from(request.labels_per_cell));
        }
        if winner.is_some() {
            break;
        }
        let mut ranked: Vec<_> = bucketed.into_values().flatten().collect();
        ranked.sort_by(|a, b| a.0.total_cmp(&b.0));
        frontier = ranked
            .into_iter()
            .take(
                320usize
                    .saturating_mul(usize::from(request.labels_per_cell))
                    .min(1280),
            )
            .map(|(_, index)| index)
            .collect();
        if frontier.is_empty() || examined >= request.max_states {
            break;
        }
        const INSPECTION_INTERVAL_SECONDS: i64 = 2 * 3600;
        let frontier_time = nodes[frontier[0]].time;
        if frontier_time - last_inspection_time >= INSPECTION_INTERVAL_SECONDS {
            let (layer_contours, layer_traces) = inspection_geometry(&request, &nodes, &frontier);
            append_bounded_inspection(&mut isochrones, layer_contours, 8_000, 160_000);
            append_bounded_inspection(&mut traces, layer_traces, 8_000, 160_000);
            last_inspection_time = frontier_time;
        }
        let percent = (((layer + 1) * 100) / max_layers.max(1)).min(99) as u8;
        host::routing_progress(
            percent,
            &format!(
                "Forecast step {}: {} route states examined",
                layer + 1,
                examined
            ),
        );
    }
    let winner = winner.ok_or_else(|| {
        format!(
            "no route reached the destination within {} hours and {} states",
            request.max_hours, examined
        )
    })?;
    // Re-sample and independently check the exact delivered final approach.
    // A search-state constraint must not be treated as proof that the direct
    // segment subsequently appended to the result is also feasible.
    let winner_node = &nodes[winner];
    if winner_node.reached_destination {
        let chain = route_chain(&nodes, winner);
        let validation_samples = validate_delivered_route(&request, &chain)?;
        let statistics = route_statistics(&request, &chain)?;
        let route_environment = route_environment(&chain)?;
        host::routing_progress(100, "Route complete and independently validated");
        return Ok(RouteResult {
            distance_nautical_miles: winner_node.sailed_nm,
            duration_seconds: (winner_node.time - request.departure_unix_time).max(0) as u64,
            states_examined: examined,
            diagnostic: format!(
                "SuperCPN-derived adaptive time-layer routing completed using typed iGRIB samples and batched host chart checks. The exact delivered geometry passed an independent chronological replay using {validation_samples} fresh environmental/chart samples."
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
    let final_samples = host::environment_sample_batch(&[EnvironmentSampleRequest {
        latitude: winner_node.lat,
        longitude: winner_node.lon,
        unix_time: winner_node.time,
    }])?;
    let final_environment = final_samples
        .first()
        .ok_or_else(|| "environment provider omitted the final-approach sample".to_string())?;
    let (Some(final_wind_u), Some(final_wind_v)) = (
        final_environment.wind_u_knots,
        final_environment.wind_v_knots,
    ) else {
        return Err("final approach has no wind coverage".into());
    };
    if request
        .max_wind_knots
        .is_some_and(|limit| final_wind_u.hypot(final_wind_v) > limit)
    {
        return Err("final approach exceeds the maximum true-wind speed".into());
    }
    if request.use_waves {
        if request.require_wave_data && final_environment.wave_height_metres.is_none() {
            return Err("final approach has no required wave coverage".into());
        }
        if request.max_wave_metres.is_some_and(|limit| {
            final_environment
                .wave_height_metres
                .is_some_and(|height| height > limit)
        }) {
            return Err("final approach exceeds the maximum wave height".into());
        }
    }
    let final_current_available =
        final_environment.current_u_knots.is_some() && final_environment.current_v_knots.is_some();
    if request.use_currents && request.require_current_data && !final_current_available {
        return Err("final approach has no required current coverage".into());
    }
    let final_current_u = if request.use_currents {
        final_environment.current_u_knots.unwrap_or(0.0)
    } else {
        0.0
    };
    let final_current_v = if request.use_currents {
        final_environment.current_v_knots.unwrap_or(0.0)
    } else {
        0.0
    };
    if request
        .max_opposing_wind_current_knots_squared
        .is_some_and(|limit| {
            final_current_available
                && opposing_wind_current(
                    final_wind_u,
                    final_wind_v,
                    final_current_u,
                    final_current_v,
                ) > limit
        })
    {
        return Err("final approach exceeds the wind-against-current limit".into());
    }
    let final_heading = bearing(
        winner_node.lat,
        winner_node.lon,
        request.destination_latitude,
        request.destination_longitude,
    );
    let final_motion = motion_for_heading(
        &request,
        final_wind_u,
        final_wind_v,
        final_current_u,
        final_current_v,
        final_heading,
        winner_node.tack,
        winner_node.propulsion_mode,
        winner_node.propulsion_run_seconds,
        request.time_step_seconds,
    )
    .ok_or_else(|| {
        "final approach violates true-wind-angle, apparent-wind or manoeuvre limits".to_string()
    })?;
    let mut chain = route_chain(&nodes, winner);
    let final_distance = distance_nm(
        chain.last().unwrap().latitude,
        chain.last().unwrap().longitude,
        request.destination_latitude,
        request.destination_longitude,
    );
    let final_heading_rad = radians(final_heading);
    let final_progress = final_motion.east_knots * final_heading_rad.sin()
        + final_motion.north_knots * final_heading_rad.cos();
    if final_progress < 0.05 || !final_motion.speed_through_water.is_finite() {
        return Err("final approach has no usable vessel progress".into());
    }
    let final_seconds = ((final_distance / final_progress * 3600.0).ceil() as i64).max(1);
    if final_seconds > request.time_step_seconds as i64 {
        return Err("final approach cannot reach the destination in one routing step".into());
    }
    let final_motor_seconds = winner_node.motor_seconds.saturating_add(
        if final_motion.propulsion_mode == PROPULSION_SAIL {
            0
        } else {
            final_seconds as u64
        },
    );
    if !motor_budget_allows(&request, final_motor_seconds) {
        return Err("final approach exceeds the motor-time or fuel limit".into());
    }
    let arrival_time = chain
        .last()
        .map(|p| p.unix_time)
        .unwrap_or(request.departure_unix_time);
    chain.push(RoutePoint {
        latitude: request.destination_latitude,
        longitude: request.destination_longitude,
        unix_time: arrival_time + final_seconds,
    });
    let final_segment = GeoSegment {
        start: GeoPoint {
            latitude: chain[chain.len() - 2].latitude,
            longitude: chain[chain.len() - 2].longitude,
        },
        end: GeoPoint {
            latitude: request.destination_latitude,
            longitude: request.destination_longitude,
        },
    };
    if request.avoid_unsafe_charts {
        let final_probes =
            clearance_segments(&final_segment, request.land_safety_margin_nautical_miles);
        let final_chart_result = host::charts_query_segments(&final_probes)?;
        if final_chart_result.len() != final_probes.len() {
            return Err("chart service returned the wrong final-approach batch length".into());
        }
        if !chart_corridor_is_covered(&final_chart_result) {
            return Err("final approach failed the independent chart-safety check".into());
        }
    }
    let validation_samples = validate_delivered_route(&request, &chain)?;
    let statistics = route_statistics(&request, &chain)?;
    let route_environment = route_environment(&chain)?;
    host::routing_progress(100, "Route complete and independently validated");
    Ok(RouteResult {
        distance_nautical_miles: nodes[winner].sailed_nm + final_distance,
        duration_seconds: (chain.last().unwrap().unix_time - request.departure_unix_time).max(0)
            as u64,
        states_examined: examined,
        diagnostic: format!(
            "SuperCPN-derived adaptive time-layer routing completed using typed iGRIB samples and batched host chart checks. The exact delivered geometry passed an independent chronological replay using {validation_samples} fresh environmental/chart samples."
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
    })
}

impl exports::opencpn::portable::plugin::Guest for IWeatherRouting {
    fn initialize() -> Result<exports::opencpn::portable::plugin::PluginInfo, String> {
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
        Ok(exports::opencpn::portable::plugin::PluginInfo {
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
    fn on_job_event(_: String, _: exports::opencpn::portable::plugin::JobEvent) {}
    fn calculate_route(request: RouteRequest) -> Result<RouteResult, String> {
        calculate(request)
    }
    fn test_trap() {
        panic!("intentional iWeatherRouting component trap");
    }
}

export!(IWeatherRouting);
