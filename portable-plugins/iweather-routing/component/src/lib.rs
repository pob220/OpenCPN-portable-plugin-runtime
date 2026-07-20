wit_bindgen::generate!({
    path: "../../../portable-runtime/wit",
    world: "plugin-world",
});

use exports::opencpn::portable::plugin::{RoutePoint, RouteRequest, RouteResult};
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
    if !(0.2..=80.0).contains(&request.boat_speed_knots)
        || !(60..=21600).contains(&request.time_step_seconds)
        || !(2..=90).contains(&request.heading_step_degrees)
        || !(1..=720).contains(&request.max_hours)
        || !(100..=1_000_000).contains(&request.max_states)
    {
        return Err("route calculation limits are outside the supported range".into());
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
    }];
    let mut frontier = vec![0usize];
    let max_layers = request.max_hours.saturating_mul(3600) / request.time_step_seconds;
    let reach =
        (request.boat_speed_knots * request.time_step_seconds as f64 / 3600.0).max(0.5) * 1.25;
    let mut examined = 0u32;
    let mut winner = None;

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
            let wind = env
                .wind_u_knots
                .unwrap_or(0.0)
                .hypot(env.wind_v_knots.unwrap_or(0.0));
            if request.min_wind_knots.is_some_and(|limit| wind < limit) {
                continue;
            }
            if request.max_wind_knots.is_some_and(|limit| wind > limit) {
                continue;
            }
            if request
                .max_wave_metres
                .is_some_and(|limit| env.wave_height_metres.is_some_and(|h| h > limit))
            {
                continue;
            }
            let target = bearing(
                node.lat,
                node.lon,
                request.destination_latitude,
                request.destination_longitude,
            );
            let step = request.heading_step_degrees as i32;
            for offset in (-90..=90).step_by(step as usize) {
                let heading = (target + offset as f64).rem_euclid(360.0);
                let heading_rad = radians(heading);
                let relative = if wind > 0.1 {
                    let wind_to = env
                        .wind_u_knots
                        .unwrap_or(0.0)
                        .atan2(env.wind_v_knots.unwrap_or(0.0))
                        .to_degrees()
                        .rem_euclid(360.0);
                    radians((heading - wind_to).rem_euclid(360.0)).sin().abs()
                } else {
                    1.0
                };
                // A conservative estimated polar: the user supplies reference
                // speed at 15 kt TWS. Both wind strength and true-wind angle
                // affect speed; close-hauled/dead-downwind performance is
                // deliberately reduced.
                let wind_factor = (wind / 15.0).sqrt().clamp(0.2, 1.15);
                let boat = request.boat_speed_knots * wind_factor * (0.32 + 0.68 * relative);
                let east = boat * heading_rad.sin() + env.current_u_knots.unwrap_or(0.0);
                let north = boat * heading_rad.cos() + env.current_v_knots.unwrap_or(0.0);
                let (lat, lon, sailed) =
                    advance(node.lat, node.lon, east, north, request.time_step_seconds);
                if !lat.is_finite() || !lon.is_finite() {
                    continue;
                }
                candidates.push(Node {
                    lat,
                    lon,
                    time: node.time + request.time_step_seconds as i64,
                    parent: Some(node_index),
                    sailed_nm: node.sailed_nm + sailed,
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
        let mut bucketed: HashMap<(i32, i32), (f64, usize)> = HashMap::new();
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
            if remaining <= reach {
                winner = Some(index);
                break;
            }
            let key = (
                (nodes[index].lat * 20.0).round() as i32,
                (nodes[index].lon * 20.0).round() as i32,
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
    let mut chain = Vec::new();
    let mut cursor = Some(winner);
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
    let final_distance = distance_nm(
        chain.last().unwrap().latitude,
        chain.last().unwrap().longitude,
        request.destination_latitude,
        request.destination_longitude,
    );
    let final_seconds = ((request.time_step_seconds as f64
        * (final_distance / reach).clamp(0.0, 1.0))
    .ceil() as i64)
        .max(1);
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
    host::routing_progress(100, "Route complete and final approach validated");
    Ok(RouteResult {
        distance_nautical_miles: nodes[winner].sailed_nm + final_distance,
        duration_seconds: (chain.last().unwrap().unix_time - request.departure_unix_time).max(0) as u64,
        states_examined: examined,
        diagnostic: "Adaptive time-layer routing completed using typed iGRIB samples and batched host chart checks. Model and chart results are advisory, not navigation-authoritative.".into(),
        points: chain,
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
    fn on_job_event(_: String, _: exports::opencpn::portable::plugin::JobEvent) {}
    fn calculate_route(request: RouteRequest) -> Result<RouteResult, String> {
        calculate(request)
    }
    fn test_trap() {
        panic!("intentional iWeatherRouting component trap");
    }
}

export!(IWeatherRouting);
