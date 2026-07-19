#ifndef OCPN_PORTABLE_RUNTIME_H
#define OCPN_PORTABLE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCPN_PORTABLE_HOST_ABI_VERSION 2u

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
} ocpn_portable_host_callbacks;

ocpn_portable_runtime* ocpn_portable_runtime_create(
    const char* component_path, const ocpn_portable_host_callbacks* callbacks,
    char* error, size_t error_capacity);

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
int32_t ocpn_portable_runtime_test_trap(ocpn_portable_runtime* runtime,
                                        char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
