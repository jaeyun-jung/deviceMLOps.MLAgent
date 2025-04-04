/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file    mlops-agent-interface.c
 * @date    5 April 2023
 * @brief   A set of exported ml-agent interfaces for managing pipelines, models, and other service.
 * @see     https://github.com/nnstreamer/deviceMLOps.MLAgent
 * @author  Wook Song <wook16.song@samsung.com>
 * @bug     No known bugs except for NYI items
 */

#include <errno.h>
#include <glib.h>
#include <stdint.h>
#include <json-glib/json-glib.h>

#include "log.h"
#include "mlops-agent-interface.h"
#include "mlops-agent-internal.h"
#include "dbus-interface.h"
#include "model-dbus.h"
#include "pipeline-dbus.h"
#include "resource-dbus.h"

#if defined(__TIZEN__)
#include <app_common.h>

static char *
_resolve_rpk_path_in_json (const char *json_str)
{
  JsonNode *node = NULL;
  JsonArray *array = NULL;
  JsonObject *object = NULL;
  JsonNode *app_info_node = NULL;
  JsonObject *app_info_object = NULL;
  gchar *ret_json_str = NULL;
  const gchar *app_info;

  guint i, n;

  g_autofree gchar *app_id = NULL;
  if (app_get_id (&app_id) == APP_ERROR_INVALID_CONTEXT) {
    ml_logi ("Not an Tizen APP context.");
    return g_strdup (json_str);
  }

  node = json_from_string (json_str, NULL);
  if (!node) {
    ml_loge ("Failed to parse given json string.");
    return NULL;
  }

  if (JSON_NODE_HOLDS_ARRAY (node)) {
    array = json_node_get_array (node);
    n = (array) ? json_array_get_length (array) : 0U;
  } else {
    n = 1;
  }

  if (n == 0U) {
    ml_loge ("No data found in the given json string.");
    return NULL;
  }

  for (i = 0; i < n; ++i) {
    if (array) {
      object = json_array_get_object_element (array, i);
    } else {
      object = json_node_get_object (node);
    }

    if (!object) {
      ml_loge ("Failed to parse given json string.");
      return NULL;
    }

    app_info = json_object_get_string_member (object, "app_info");
    if (!app_info) {
      ml_loge ("Failed to get `app_info` from the given json string.");
      goto done;
    }

    app_info_node = json_from_string (app_info, NULL);
    if (!app_info_node) {
      ml_loge ("Failed to parse `app_info` from the given json string.");
      goto done;
    }

    app_info_object = json_node_get_object (app_info_node);
    if (!app_info_object) {
      ml_loge ("Failed to get `app_info` object.");
      json_node_free (app_info_node);
      goto done;
    }

    if (g_strcmp0 (json_object_get_string_member (app_info_object, "is_rpk"), "T") == 0) {
      gchar *new_path;
      g_autofree gchar *global_resource_path;
      const gchar *res_type =
          json_object_get_string_member (app_info_object, "res_type");

      const gchar *ori_path = json_object_get_string_member (object, "path");

      if (app_get_res_control_global_resource_path (res_type,
          &global_resource_path) != APP_ERROR_NONE) {
            ml_loge ("failed to get global resource path.");
            json_node_free (app_info_node);
            goto done;
      }

      new_path = g_strdup_printf ("%s/%s", global_resource_path, ori_path);
      json_object_set_string_member (object, "path", new_path);
      g_free (new_path);
    }

    json_node_free (app_info_node);
  }

done:
  ret_json_str = json_to_string (node, TRUE);
  json_node_free (node);

  return ret_json_str;
}
#else
static char *
_resolve_rpk_path_in_json (const char *json_str)
{
  return g_strdup (json_str);
}
#endif /* __TIZEN__ */

typedef gpointer ml_agent_proxy_h;

/**
 * @brief An internal helper to get the dbus proxy
 */
static ml_agent_proxy_h
_get_proxy_new_for_bus_sync (ml_agent_service_type_e type)
{
  static const GBusType bus_types[] = { G_BUS_TYPE_SYSTEM, G_BUS_TYPE_SESSION };
  static const size_t num_bus_types =
      sizeof (bus_types) / sizeof (bus_types[0]);
  ml_agent_proxy_h proxy = NULL;
  size_t i;

  switch (type) {
    case ML_AGENT_SERVICE_PIPELINE:
    {
      MachinelearningServicePipeline *mlsp;

      for (i = 0; i < num_bus_types; ++i) {
        mlsp = machinelearning_service_pipeline_proxy_new_for_bus_sync
            (bus_types[i], G_DBUS_PROXY_FLAGS_NONE, DBUS_ML_BUS_NAME,
            DBUS_PIPELINE_PATH, NULL, NULL);
        if (mlsp) {
          break;
        }
      }
      proxy = (ml_agent_proxy_h) mlsp;
      break;
    }
    case ML_AGENT_SERVICE_MODEL:
    {
      MachinelearningServiceModel *mlsm;

      for (i = 0; i < num_bus_types; ++i) {
        mlsm = machinelearning_service_model_proxy_new_for_bus_sync
            (bus_types[i], G_DBUS_PROXY_FLAGS_NONE, DBUS_ML_BUS_NAME,
            DBUS_MODEL_PATH, NULL, NULL);
        if (mlsm)
          break;
      }
      proxy = (ml_agent_proxy_h) mlsm;
      break;
    }
    case ML_AGENT_SERVICE_RESOURCE:
    {
      MachinelearningServiceResource *mlsr;

      for (i = 0; i < num_bus_types; ++i) {
        mlsr = machinelearning_service_resource_proxy_new_for_bus_sync
            (bus_types[i], G_DBUS_PROXY_FLAGS_NONE, DBUS_ML_BUS_NAME,
            DBUS_RESOURCE_PATH, NULL, NULL);
        if (mlsr)
          break;
      }
      proxy = (ml_agent_proxy_h) mlsr;
      break;
    }
    default:
      break;
  }

  return proxy;
}

/**
 * @brief An interface exported for setting the description of a pipeline.
 */
int
ml_agent_pipeline_set_description (const char *name, const char *pipeline_desc)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || !STR_IS_VALID (pipeline_desc)) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_set_pipeline_sync (mlsp,
      name, pipeline_desc, &ret, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for getting the pipeline's description corresponding to the given @a name.
 */
int
ml_agent_pipeline_get_description (const char *name, char **pipeline_desc)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || !pipeline_desc) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_get_pipeline_sync (mlsp,
      name, &ret, pipeline_desc, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for deletion of the pipeline's description corresponding to the given @a name.
 */
int
ml_agent_pipeline_delete (const char *name)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name)) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_delete_pipeline_sync (mlsp,
      name, &ret, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for launching the pipeline's description corresponding to the given @a name.
 */
int
ml_agent_pipeline_launch (const char *name, int64_t * id)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || !id) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_launch_pipeline_sync (mlsp,
      name, &ret, id, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for changing the pipeline's state of the given @a id to start.
 */
int
ml_agent_pipeline_start (const int64_t id)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_start_pipeline_sync (mlsp,
      id, &ret, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for changing the pipeline's state of the given @a id to stop.
 */
int
ml_agent_pipeline_stop (const int64_t id)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_stop_pipeline_sync (mlsp,
      id, &ret, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for destroying a launched pipeline corresponding to the given @a id.
 */
int
ml_agent_pipeline_destroy (const int64_t id)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_destroy_pipeline_sync (mlsp,
      id, &ret, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for getting the pipeline's state of the given @a id.
 */
int
ml_agent_pipeline_get_state (const int64_t id, int *state)
{
  MachinelearningServicePipeline *mlsp;
  gboolean result;
  gint ret;

  if (!state) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsp = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_PIPELINE);
  if (!mlsp) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_pipeline_call_get_state_sync (mlsp,
      id, &ret, state, NULL, NULL);
  g_object_unref (mlsp);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for registering a model.
 */
int
ml_agent_model_register (const char *name, const char *path,
    const int activate, const char *description, const char *app_info,
    uint32_t * version)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || !STR_IS_VALID (path) || !version) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_register_sync (mlsm, name, path,
      activate, description ? description : "", app_info ? app_info : "",
      version, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for updating the description of the model with @a name and @a version.
 */
int
ml_agent_model_update_description (const char *name,
    const uint32_t version, const char *description)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || !STR_IS_VALID (description) || version == 0U) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_update_description_sync (mlsm,
      name, version, description, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for activating the model with @a name and @a version.
 */
int
ml_agent_model_activate (const char *name, const uint32_t version)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || version == 0U) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_activate_sync (mlsm,
      name, version, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for getting the information of the model with @a name and @a version.
 */
int
ml_agent_model_get (const char *name, const uint32_t version, char **model_info)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;
  gchar *ret_json;

  if (!STR_IS_VALID (name) || !model_info || version == 0U) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_get_sync (mlsm,
      name, version, &ret_json, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);

  *model_info = _resolve_rpk_path_in_json (ret_json);
  g_free (ret_json);

  return 0;
}

/**
 * @brief An interface exported for getting the information of the activated model with @a name.
 */
int
ml_agent_model_get_activated (const char *name, char **model_info)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;
  gchar *ret_json;

  if (!STR_IS_VALID (name) || !model_info) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_get_activated_sync (mlsm,
      name, &ret_json, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);

  *model_info = _resolve_rpk_path_in_json (ret_json);
  g_free (ret_json);

  return 0;
}

/**
 * @brief An interface exported for getting the information of all the models corresponding to the given @a name.
 */
int
ml_agent_model_get_all (const char *name, char **model_info)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;
  gchar *ret_json;

  if (!STR_IS_VALID (name) || !model_info) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_get_all_sync (mlsm,
      name, &ret_json, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);

  *model_info = _resolve_rpk_path_in_json (ret_json);
  g_free (ret_json);

  return 0;
}

/**
 * @brief An interface exported for removing the model of @a name and @a version.
 * @details If @a force is true, this will delete the model even if it is activated.
 */
int
ml_agent_model_delete (const char *name, const uint32_t version,
    const int force)
{
  MachinelearningServiceModel *mlsm;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name)) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsm = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_MODEL);
  if (!mlsm) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_model_call_delete_sync (mlsm,
      name, version, force, &ret, NULL, NULL);
  g_object_unref (mlsm);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for adding the resource.
 */
int
ml_agent_resource_add (const char *name, const char *path,
    const char *description, const char *app_info)
{
  MachinelearningServiceResource *mlsr;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name) || !STR_IS_VALID (path)) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsr = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_RESOURCE);
  if (!mlsr) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_resource_call_add_sync (mlsr, name, path,
      description ? description : "", app_info ? app_info : "",
      &ret, NULL, NULL);
  g_object_unref (mlsr);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for removing the resource with @a name.
 */
int
ml_agent_resource_delete (const char *name)
{
  MachinelearningServiceResource *mlsr;
  gboolean result;
  gint ret;

  if (!STR_IS_VALID (name)) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsr = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_RESOURCE);
  if (!mlsr) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_resource_call_delete_sync (mlsr,
      name, &ret, NULL, NULL);
  g_object_unref (mlsr);

  g_return_val_if_fail (ret == 0 && result, ret);
  return 0;
}

/**
 * @brief An interface exported for getting the description of the resource with @a name.
 */
int
ml_agent_resource_get (const char *name, char **res_info)
{
  MachinelearningServiceResource *mlsr;
  gboolean result;
  gint ret;
  gchar *ret_json;

  if (!STR_IS_VALID (name) || !res_info) {
    g_return_val_if_reached (-EINVAL);
  }

  mlsr = _get_proxy_new_for_bus_sync (ML_AGENT_SERVICE_RESOURCE);
  if (!mlsr) {
    g_return_val_if_reached (-EIO);
  }

  result = machinelearning_service_resource_call_get_sync (mlsr,
      name, &ret_json, &ret, NULL, NULL);
  g_object_unref (mlsr);

  g_return_val_if_fail (ret == 0 && result, ret);

  *res_info = _resolve_rpk_path_in_json (ret_json);
  g_free (ret_json);

  return 0;
}
