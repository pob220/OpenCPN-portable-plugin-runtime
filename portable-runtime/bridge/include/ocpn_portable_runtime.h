#ifndef OCPN_PORTABLE_RUNTIME_H
#define OCPN_PORTABLE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCPN_PORTABLE_HOST_ABI_VERSION 15u
#define OCPN_PORTABLE_API_V01 1u
#define OCPN_PORTABLE_API_V02 2u
#define OCPN_PORTABLE_API_V03 3u
#define OCPN_PORTABLE_API_V04 4u
#define OCPN_PORTABLE_WORLD_PLUGIN 0u
#define OCPN_PORTABLE_WORLD_WEATHER_ROUTING 1u
#define OCPN_PORTABLE_WORLD_PASSAGE_ROUTING 2u

typedef struct ocpn_portable_runtime ocpn_portable_runtime;

/*
 * OPP API 0.4 action invocation context. location values follow the WIT
 * action-location declaration: 0 toolbar, 1 chart, 2 AIS, 3 route,
 * 4 waypoint and 5 track context menu.
 */
typedef struct ocpn_portable_action_context {
  uint32_t location;
  uint32_t canvas_index;
  uint8_t has_canvas_index;
  double latitude;
  double longitude;
  uint8_t has_position;
  const char* object_kind;
  size_t object_kind_len;
  const char* object_id;
  size_t object_id_len;
} ocpn_portable_action_context;

typedef struct ocpn_portable_geo_point {
  double latitude;
  double longitude;
} ocpn_portable_geo_point;

typedef struct ocpn_portable_overlay_style {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t alpha;
  float width_pixels;
} ocpn_portable_overlay_style;

typedef struct ocpn_portable_geo_segment {
  ocpn_portable_geo_point start;
  ocpn_portable_geo_point end;
} ocpn_portable_geo_segment;

typedef struct ocpn_portable_chart_segment_result {
  /* 0 covered, 1 unsafe, 2 missing coverage, 3 unknown. */
  uint32_t state;
  uint32_t charts_considered;
  /* 0 none/safe, 1 land, 2 drying, 3 shallow, 4 unknown depth,
     5 no chart, 6 provider error. */
  uint32_t reason;
} ocpn_portable_chart_segment_result;

typedef struct ocpn_portable_final_chart_safety_options {
  double safety_margin_nautical_miles;
  double minimum_depth_metres;
  uint8_t require_authoritative;
} ocpn_portable_final_chart_safety_options;

typedef struct ocpn_portable_environment_sample_request {
  double latitude;
  double longitude;
  int64_t unix_time;
} ocpn_portable_environment_sample_request;

typedef struct ocpn_portable_environment_sample {
  double wind_u_knots;
  double wind_v_knots;
  double current_u_knots;
  double current_v_knots;
  double wave_height_metres;
  uint32_t available; /* bit 0 wind, bit 1 current, bit 2 waves */
} ocpn_portable_environment_sample;

typedef struct ocpn_portable_polar_grid {
  const char* identity;
  size_t identity_len;
  const double* true_wind_speeds_knots;
  size_t true_wind_speed_count;
  const double* true_wind_angles_degrees;
  size_t true_wind_angle_count;
  /* Wind-speed-major: wind index * angle count + angle index. */
  const double* boat_speeds_knots;
  size_t boat_speed_count;
} ocpn_portable_polar_grid;

typedef struct ocpn_portable_route_request {
  double start_latitude;
  double start_longitude;
  double destination_latitude;
  double destination_longitude;
  int64_t departure_unix_time;
  const ocpn_portable_polar_grid* polars;
  size_t polar_count;
  uint32_t time_step_seconds;
  uint16_t heading_step_degrees;
  uint16_t refined_heading_step_degrees;
  uint8_t adaptive_headings;
  double spatial_cell_nautical_miles;
  uint8_t labels_per_cell;
  uint32_t max_hours;
  uint32_t max_states;
  uint32_t inspection_interval_seconds; /* zero disables isochrone capture */
  uint8_t include_traces;
  uint8_t avoid_unsafe_charts;
  double min_true_wind_angle_degrees;
  double max_true_wind_angle_degrees;
  double max_wind_knots;
  double max_apparent_wind_knots;
  double max_wave_metres;
  double max_opposing_wind_current_knots_squared;
  double land_safety_margin_nautical_miles;
  double maximum_latitude_degrees;
  double upwind_efficiency;
  double downwind_efficiency;
  double maximum_search_angle_degrees;
  double destination_tolerance_nm;
  uint32_t tack_penalty_seconds;
  uint32_t gybe_penalty_seconds;
  uint8_t allow_motor_sailing;
  uint8_t allow_motor;
  double motor_below_sailing_speed_knots;
  double motor_speed_knots;
  double motor_sailing_boost_knots;
  double motor_crossover_hysteresis_knots;
  uint32_t minimum_motor_run_seconds;
  uint32_t mode_change_penalty_seconds;
  uint32_t maximum_motor_seconds;
  double fuel_consumption_litres_per_hour;
  double maximum_fuel_litres;
  uint8_t use_currents;
  uint8_t require_current_data;
  uint8_t use_waves;
  uint8_t require_wave_data;
  uint32_t limits_available; /* bit 0 true wind, bit 1 waves, bit 2 apparent,
                                bit 3 opposing wind/current, bit 4 maximum
                                motor time, bit 5 fuel rate, bit 6 fuel */
  double minimum_chart_depth_metres;
  uint8_t require_authoritative_chart_safety;
} ocpn_portable_route_request;

typedef struct ocpn_portable_route_point {
  double latitude;
  double longitude;
  int64_t unix_time;
} ocpn_portable_route_point;

/* A span into one of the route result point arrays. */
typedef struct ocpn_portable_route_line {
  size_t point_offset;
  size_t point_count;
  int64_t unix_time;
} ocpn_portable_route_line;

typedef struct ocpn_portable_route_environment_point {
  double latitude;
  double longitude;
  int64_t unix_time;
  double wind_u_knots;
  double wind_v_knots;
  double current_u_knots;
  double current_v_knots;
  double wave_height_metres;
  uint8_t available; /* bit 0 current, bit 1 wave */
} ocpn_portable_route_environment_point;

typedef struct ocpn_portable_route_result {
  ocpn_portable_route_point* points;
  size_t point_capacity;
  size_t point_count;
  ocpn_portable_route_point* isochrone_points;
  size_t isochrone_point_capacity;
  size_t isochrone_point_count;
  ocpn_portable_route_line* isochrones;
  size_t isochrone_capacity;
  size_t isochrone_count;
  ocpn_portable_route_point* trace_points;
  size_t trace_point_capacity;
  size_t trace_point_count;
  ocpn_portable_route_line* traces;
  size_t trace_capacity;
  size_t trace_count;
  ocpn_portable_route_environment_point* route_environment;
  size_t route_environment_capacity;
  size_t route_environment_count;
  double distance_nautical_miles;
  uint64_t duration_seconds;
  uint32_t states_examined;
  double average_speed_knots;
  double maximum_speed_knots;
  double average_sog_knots;
  double maximum_sog_knots;
  double average_wind_knots;
  double maximum_wind_knots;
  double average_current_knots;
  double maximum_current_knots;
  uint32_t tacks;
  uint64_t motor_seconds;
  double estimated_fuel_litres;
  uint32_t propulsion_transitions;
  uint8_t comfort_level;
  uint8_t metrics_available; /* bit 0 current metrics, bit 1 fuel */
  char* diagnostic;
  size_t diagnostic_capacity;
  size_t diagnostic_len;
} ocpn_portable_route_result;

typedef struct ocpn_portable_passage_gate {
  const char* id;
  size_t id_len;
  const char* name;
  size_t name_len;
  double latitude;
  double longitude;
} ocpn_portable_passage_gate;

typedef struct ocpn_portable_passage_request {
  ocpn_portable_route_request route;
  const ocpn_portable_passage_gate* gates;
  size_t gate_count;
  int64_t departure_offset_seconds;
} ocpn_portable_passage_request;

typedef struct ocpn_portable_passage_leg {
  uint32_t start_gate_index;
  uint32_t end_gate_index;
  size_t point_offset;
  size_t point_count;
  int64_t departure_unix_time;
  int64_t arrival_unix_time;
  double distance_nautical_miles;
  uint32_t states_examined;
} ocpn_portable_passage_leg;

typedef struct ocpn_portable_passage_result {
  ocpn_portable_route_result route;
  ocpn_portable_passage_leg* legs;
  size_t leg_capacity;
  size_t leg_count;
  uint64_t validation_samples;
} ocpn_portable_passage_result;

typedef struct ocpn_portable_host_callbacks {
  uint32_t abi_version;
  void* user_data;

  void (*log)(void* user_data, uint32_t level, const char* message,
              size_t message_len);
  int32_t (*register_action)(void* user_data, const char* action_id,
                             size_t action_id_len, const char* label,
                             size_t label_len, const char* tooltip,
                             size_t tooltip_len, const char* icon_resource,
                             size_t icon_resource_len,
                             uint32_t* host_action_id);
  int32_t (*get_vessel_position)(void* user_data, double* latitude,
                                 double* longitude, double* cog,
                                 uint8_t* has_cog, double* sog,
                                 uint8_t* has_sog);
  int32_t (*setting_get)(void* user_data, const char* key, size_t key_len,
                         char* value, size_t value_capacity, size_t* value_len,
                         uint8_t* found);
  int32_t (*setting_set)(void* user_data, const char* key, size_t key_len,
                         const char* value, size_t value_len);
  int32_t (*submit_polyline)(void* user_data, const char* scene_id,
                             size_t scene_id_len,
                             const ocpn_portable_geo_point* points,
                             size_t point_count,
                             ocpn_portable_overlay_style style);
  int32_t (*clear_scene)(void* user_data, const char* scene_id,
                         size_t scene_id_len);
  int32_t (*start_job)(void* user_data, const char* job_id, size_t job_id_len,
                       uint32_t work_units);
  int32_t (*cancel_job)(void* user_data, const char* job_id, size_t job_id_len);
  int32_t (*open_surface)(void* user_data, const char* surface_id,
                          size_t surface_id_len);
  int32_t (*open_environmental_viewer)(void* user_data);
  int32_t (*open_weather_routing)(void* user_data);
  int32_t (*environment_sample_batch)(
      void* user_data, const ocpn_portable_environment_sample_request* requests,
      size_t request_count, ocpn_portable_environment_sample* results,
      size_t result_count, char* error, size_t error_capacity);
  void (*routing_progress)(void* user_data, uint8_t percent,
                           const char* message, size_t message_len);
  uint8_t (*routing_cancelled)(void* user_data);
  int32_t (*charts_query_segments)(void* user_data,
                                   const ocpn_portable_geo_segment* segments,
                                   size_t segment_count,
                                   ocpn_portable_chart_segment_result* results,
                                   size_t result_count);
  int32_t (*network_get_to_private)(void* user_data, const char* request_id,
                                    size_t request_id_len, const char* url,
                                    size_t url_len, const char* private_name,
                                    size_t private_name_len,
                                    uint64_t max_bytes);
  int32_t (*storage_private_read)(void* user_data, const char* private_name,
                                  size_t private_name_len, uint8_t* value,
                                  size_t value_capacity, size_t* value_len);
  int32_t (*user_file_read)(void* user_data, const char* grant_token,
                            size_t grant_token_len, uint8_t* value,
                            size_t value_capacity, size_t* value_len);
  int32_t (*user_file_write)(void* user_data, const char* grant_token,
                             size_t grant_token_len, const uint8_t* value,
                             size_t value_len);
  int32_t (*send_plugin_message)(void* user_data, const char* message_id,
                                 size_t message_id_len,
                                 const char* message_body,
                                 size_t message_body_len);
  int32_t (*charts_query_final_safety)(
      void* user_data, const ocpn_portable_geo_segment* segments,
      size_t segment_count,
      const ocpn_portable_final_chart_safety_options* options,
      ocpn_portable_chart_segment_result* results, size_t result_count);
  /*
   * Private bounded transport for API 0.3's typed author services. WIT is
   * the public contract. The bridge supplies a policy-bounded output buffer
   * and the host reports the actual response length.
   */
  int32_t (*author_service_call)(void* user_data, const char* operation,
                                 size_t operation_len, const char* request_json,
                                 size_t request_json_len, char* response_json,
                                 size_t response_capacity,
                                 size_t* response_len);
} ocpn_portable_host_callbacks;

ocpn_portable_runtime* ocpn_portable_runtime_create(
    const char* component_path, const ocpn_portable_host_callbacks* callbacks,
    uint32_t portable_api, uint32_t portable_world, char* error,
    size_t error_capacity);

/* Create a fresh, isolated Store for a compute export. The compiled component,
 * epoch clock and explicitly granted host callbacks are shared; plugin
 * lifecycle methods are intentionally not invoked on the replica. */
ocpn_portable_runtime* ocpn_portable_runtime_clone_compute(
    const ocpn_portable_runtime* runtime, char* error, size_t error_capacity);

void ocpn_portable_runtime_destroy(ocpn_portable_runtime* runtime);

int32_t ocpn_portable_runtime_initialize(
    ocpn_portable_runtime* runtime, const char* expected_id,
    size_t expected_id_len, const char* expected_name, size_t expected_name_len,
    const char* expected_version, size_t expected_version_len, char* error,
    size_t error_capacity);
int32_t ocpn_portable_runtime_enable(ocpn_portable_runtime* runtime,
                                     char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_disable(ocpn_portable_runtime* runtime,
                                      char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_action(ocpn_portable_runtime* runtime,
                                        const char* action_id,
                                        size_t action_id_len, char* error,
                                        size_t error_capacity);
int32_t ocpn_portable_runtime_on_action_v04(
    ocpn_portable_runtime* runtime, const char* action_id, size_t action_id_len,
    const ocpn_portable_action_context* context, char* error,
    size_t error_capacity);
int32_t ocpn_portable_runtime_on_surface_event(
    ocpn_portable_runtime* runtime, const char* surface_id,
    size_t surface_id_len, const char* control_id, size_t control_id_len,
    const char* value_json, size_t value_json_len, char* state_json,
    size_t state_json_capacity, size_t* state_json_len, char* error,
    size_t error_capacity);
int32_t ocpn_portable_runtime_on_job_event(
    ocpn_portable_runtime* runtime, const char* job_id, size_t job_id_len,
    uint32_t event_kind, uint8_t progress, const char* message,
    size_t message_len, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_event(ocpn_portable_runtime* runtime,
                                       uint32_t event_kind, const char* topic,
                                       size_t topic_len, const char* payload,
                                       size_t payload_len, uint64_t sequence,
                                       char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_navigation_sentence(
    ocpn_portable_runtime* runtime, const char* sentence, size_t sentence_len,
    char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_plugin_message(
    ocpn_portable_runtime* runtime, const char* message_id,
    size_t message_id_len, const char* message_body, size_t message_body_len,
    char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_pointer_event(
    ocpn_portable_runtime* runtime, uint32_t kind, uint32_t button,
    uint32_t canvas_index, int32_t x_pixels, int32_t y_pixels, double latitude,
    double longitude, uint8_t has_position, int32_t wheel_rotation,
    uint32_t modifiers, const char* hit_scene_id, size_t hit_scene_id_len,
    const char* hit_primitive_id, size_t hit_primitive_id_len, uint8_t* handled,
    char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_key_event(ocpn_portable_runtime* runtime,
                                           uint32_t key_code, uint32_t unicode,
                                           uint8_t has_unicode, uint8_t pressed,
                                           uint8_t repeat, uint32_t modifiers,
                                           uint8_t* handled, char* error,
                                           size_t error_capacity);
int32_t ocpn_portable_runtime_on_timer(ocpn_portable_runtime* runtime,
                                       const char* timer_id,
                                       size_t timer_id_len,
                                       int64_t scheduled_unix_milliseconds,
                                       int64_t fired_unix_milliseconds,
                                       char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_rpc_request(
    ocpn_portable_runtime* runtime, const char* source_package,
    size_t source_package_len, const char* request_json,
    size_t request_json_len, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_on_rpc_response(
    ocpn_portable_runtime* runtime, const char* source_package,
    size_t source_package_len, const char* response_json,
    size_t response_json_len, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_calculate_route(
    ocpn_portable_runtime* runtime, const ocpn_portable_route_request* request,
    ocpn_portable_route_result* result, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_calculate_passage(
    ocpn_portable_runtime* runtime,
    const ocpn_portable_passage_request* request,
    ocpn_portable_passage_result* result, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_test_trap(ocpn_portable_runtime* runtime,
                                        char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
