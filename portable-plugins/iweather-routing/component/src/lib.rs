wit_bindgen::generate!({
    path: "../../../portable-runtime/wit",
    world: "plugin-world",
});

use exports::opencpn::portable::plugin::{
    PolarGrid, RouteInspectionLine, RoutePoint, RouteRequest, RouteResult,
};
use opencpn::portable::host::{
    self, ChartCoverageState, EnvironmentSampleRequest, GeoPoint, GeoSegment, LogLevel,
};
use std::collections::HashMap;

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
    reached_destination: bool,
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
    comfort_level: u8,
}

fn route_statistics(points: &[RoutePoint]) -> Result<RouteStatistics, String> {
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
        previous_tack = tack;
        let (current_u, current_v, has_current) =
            match (sample.current_u_knots, sample.current_v_knots) {
                (Some(u), Some(v)) => (u, v, true),
                _ => (0.0, 0.0, false),
            };
        let heading_radians = radians(heading);
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
}

fn motion_for_heading(
    request: &RouteRequest,
    wind_u: f64,
    wind_v: f64,
    current_u: f64,
    current_v: f64,
    heading: f64,
    previous_tack: i8,
) -> Option<Motion> {
    let wind = wind_u.hypot(wind_v);
    let (twa, tack) = true_wind_angle(wind_u, wind_v, heading);
    if twa + 1e-9 < request.min_true_wind_angle_degrees
        || twa - 1e-9 > request.max_true_wind_angle_degrees
    {
        return None;
    }
    let efficiency = if twa <= 90.0 {
        request.upwind_efficiency
    } else {
        request.downwind_efficiency
    };
    let speed = polar_speed(request, wind, twa)? * efficiency;
    let heading_rad = radians(heading);
    let boat_east = speed * heading_rad.sin();
    let boat_north = speed * heading_rad.cos();
    if request
        .max_apparent_wind_knots
        .is_some_and(|limit| (wind_u - boat_east).hypot(wind_v - boat_north) > limit)
    {
        return None;
    }
    let penalty = if previous_tack != 0 && previous_tack != tack {
        if twa <= 90.0 {
            request.tack_penalty_seconds
        } else {
            request.gybe_penalty_seconds
        }
    } else {
        0
    };
    if penalty >= request.time_step_seconds {
        return None;
    }
    // Manoeuvre time reduces progress through the water while current still
    // acts for the complete forecast step.
    let moving_fraction =
        (request.time_step_seconds - penalty) as f64 / request.time_step_seconds as f64;
    Some(Motion {
        east_knots: boat_east * moving_fraction + current_u,
        north_knots: boat_north * moving_fraction + current_v,
        speed_through_water: speed,
        tack,
    })
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
        || request.require_current_data && !request.use_currents
        || request.require_wave_data && !request.use_waves
    {
        return Err("route vessel, data-policy or search settings are invalid".into());
    }
    for limit in [
        request.max_wind_knots,
        request.max_apparent_wind_knots,
        request.max_wave_metres,
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
        let mut segments: Vec<GeoSegment> = Vec::new();
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
                &request, wind_u, wind_v, current_u, current_v, target, node.tack,
            ) {
                let target_rad = radians(target);
                let progress = direct_motion.east_knots * target_rad.sin()
                    + direct_motion.north_knots * target_rad.cos();
                let seconds = if progress > 0.05 {
                    (direct_distance / progress * 3600.0).ceil() as u32
                } else {
                    u32::MAX
                };
                if seconds > 0 && seconds <= request.time_step_seconds {
                    candidates.push(Node {
                        lat: request.destination_latitude,
                        lon: request.destination_longitude,
                        time: node.time + seconds as i64,
                        parent: Some(node_index),
                        sailed_nm: node.sailed_nm + direct_distance,
                        tack: direct_motion.tack,
                        reached_destination: true,
                    });
                    segments.push(GeoSegment {
                        start: GeoPoint {
                            latitude: node.lat,
                            longitude: node.lon,
                        },
                        end: GeoPoint {
                            latitude: request.destination_latitude,
                            longitude: request.destination_longitude,
                        },
                    });
                }
            }
            let step = request.heading_step_degrees as i32;
            let search = request.maximum_search_angle_degrees.round() as i32;
            for offset in (-search..=search).step_by(step as usize) {
                let heading = (target + offset as f64).rem_euclid(360.0);
                let Some(motion) = motion_for_heading(
                    &request, wind_u, wind_v, current_u, current_v, heading, node.tack,
                ) else {
                    continue;
                };
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
                    reached_destination: false,
                });
                segments.push(GeoSegment {
                    start: GeoPoint {
                        latitude: node.lat,
                        longitude: node.lon,
                    },
                    end: GeoPoint {
                        latitude: lat,
                        longitude: lon,
                    },
                });
            }
        }
        if candidates.is_empty() {
            return Err("no viable states remain after environmental limits".into());
        }
        let chart_results = if request.avoid_unsafe_charts {
            host::charts_query_segments(&segments)?
        } else {
            Vec::new()
        };
        if request.avoid_unsafe_charts && chart_results.len() != segments.len() {
            return Err("chart service returned the wrong batch length".into());
        }
        let mut bucketed: HashMap<(i32, i32, i8), (f64, usize)> = HashMap::new();
        for (candidate_index, candidate) in candidates.into_iter().enumerate() {
            examined = examined.saturating_add(1);
            if examined >= request.max_states {
                break;
            }
            if request.avoid_unsafe_charts
                && chart_results[candidate_index].state != ChartCoverageState::Covered
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
            let key = (
                (nodes[index].lat * 20.0).round() as i32,
                (nodes[index].lon * 20.0).round() as i32,
                nodes[index].tack,
            );
            let score = remaining + nodes[index].sailed_nm * 0.04;
            match bucketed.get(&key) {
                Some((old, _)) if *old <= score => {}
                _ => {
                    bucketed.insert(key, (score, index));
                }
            }
        }
        if winner.is_some() {
            break;
        }
        let mut ranked: Vec<_> = bucketed.into_values().collect();
        ranked.sort_by(|a, b| a.0.total_cmp(&b.0));
        frontier = ranked
            .into_iter()
            .take(320)
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
        let statistics = route_statistics(&chain)?;
        host::routing_progress(100, "Route complete and final approach validated");
        return Ok(RouteResult {
            distance_nautical_miles: winner_node.sailed_nm,
            duration_seconds: (winner_node.time - request.departure_unix_time).max(0) as u64,
            states_examined: examined,
            diagnostic: "Adaptive time-layer routing completed using typed iGRIB samples and batched host chart checks. The destination leg was generated and checked inside the search. Model and chart results are advisory, not navigation-authoritative.".into(),
            points: chain,
            isochrones,
            traces,
            average_speed_knots: statistics.average_speed_knots,
            maximum_speed_knots: statistics.maximum_speed_knots,
            average_sog_knots: statistics.average_sog_knots,
            maximum_sog_knots: statistics.maximum_sog_knots,
            average_wind_knots: statistics.average_wind_knots,
            maximum_wind_knots: statistics.maximum_wind_knots,
            average_current_knots: statistics.average_current_knots,
            maximum_current_knots: statistics.maximum_current_knots,
            tacks: statistics.tacks,
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
    let arrival_time = chain
        .last()
        .map(|p| p.unix_time)
        .unwrap_or(request.departure_unix_time);
    chain.push(RoutePoint {
        latitude: request.destination_latitude,
        longitude: request.destination_longitude,
        unix_time: arrival_time + final_seconds,
    });
    let final_segment = [GeoSegment {
        start: GeoPoint {
            latitude: chain[chain.len() - 2].latitude,
            longitude: chain[chain.len() - 2].longitude,
        },
        end: GeoPoint {
            latitude: request.destination_latitude,
            longitude: request.destination_longitude,
        },
    }];
    if request.avoid_unsafe_charts {
        let final_chart_result = host::charts_query_segments(&final_segment)?;
        if final_chart_result.len() != 1 {
            return Err("chart service returned the wrong final-approach batch length".into());
        }
        if final_chart_result[0].state != ChartCoverageState::Covered {
            return Err("final approach failed the independent chart-safety check".into());
        }
    }
    let statistics = route_statistics(&chain)?;
    host::routing_progress(100, "Route complete and final approach validated");
    Ok(RouteResult {
        distance_nautical_miles: nodes[winner].sailed_nm + final_distance,
        duration_seconds: (chain.last().unwrap().unix_time - request.departure_unix_time).max(0) as u64,
        states_examined: examined,
        diagnostic: "Adaptive time-layer routing completed using typed iGRIB samples and batched host chart checks. Model and chart results are advisory, not navigation-authoritative.".into(),
        points: chain,
        isochrones,
        traces,
        average_speed_knots: statistics.average_speed_knots,
        maximum_speed_knots: statistics.maximum_speed_knots,
        average_sog_knots: statistics.average_sog_knots,
        maximum_sog_knots: statistics.maximum_sog_knots,
        average_wind_knots: statistics.average_wind_knots,
        maximum_wind_knots: statistics.maximum_wind_knots,
        average_current_knots: statistics.average_current_knots,
        maximum_current_knots: statistics.maximum_current_knots,
        tacks: statistics.tacks,
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
        if surface_id != "weather-routing.main" {
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
