#ifndef OCPN_PORTABLE_RUNTIME_H
#define OCPN_PORTABLE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCPN_PORTABLE_HOST_ABI_VERSION 6u

typedef struct ocpn_portable_runtime ocpn_portable_runtime;

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
} ocpn_portable_chart_segment_result;

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

typedef struct ocpn_portable_route_request {
  double start_latitude;
  double start_longitude;
  double destination_latitude;
  double destination_longitude;
  int64_t departure_unix_time;
  double boat_speed_knots;
  uint32_t time_step_seconds;
  uint16_t heading_step_degrees;
  uint32_t max_hours;
  uint32_t max_states;
  uint8_t avoid_unsafe_charts;
  double max_wind_knots;
  double max_wave_metres;
  double min_wind_knots;
  uint32_t limits_available; /* bit 0 max wind, bit 1 waves, bit 2 min wind */
} ocpn_portable_route_request;

typedef struct ocpn_portable_route_point {
  double latitude;
  double longitude;
  int64_t unix_time;
} ocpn_portable_route_point;

typedef struct ocpn_portable_route_result {
  ocpn_portable_route_point* points;
  size_t point_capacity;
  size_t point_count;
  double distance_nautical_miles;
  uint64_t duration_seconds;
  uint32_t states_examined;
  char* diagnostic;
  size_t diagnostic_capacity;
  size_t diagnostic_len;
} ocpn_portable_route_result;

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
  int32_t (*open_environmental_viewer)(void* user_data);
  int32_t (*open_weather_routing)(void* user_data);
  int32_t (*environment_sample_batch)(
      void* user_data, const ocpn_portable_environment_sample_request* requests,
      size_t request_count, ocpn_portable_environment_sample* results,
      size_t result_count);
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
} ocpn_portable_host_callbacks;

ocpn_portable_runtime* ocpn_portable_runtime_create(
    const char* component_path, const ocpn_portable_host_callbacks* callbacks,
    char* error, size_t error_capacity);

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
int32_t ocpn_portable_runtime_on_job_event(
    ocpn_portable_runtime* runtime, const char* job_id, size_t job_id_len,
    uint32_t event_kind, uint8_t progress, const char* message,
    size_t message_len, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_calculate_route(
    ocpn_portable_runtime* runtime, const ocpn_portable_route_request* request,
    ocpn_portable_route_result* result, char* error, size_t error_capacity);
int32_t ocpn_portable_runtime_test_trap(ocpn_portable_runtime* runtime,
                                        char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
