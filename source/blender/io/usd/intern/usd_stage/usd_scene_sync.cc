/* source/blender/io/usd/intern/usd_scene_sync.cc */
#include "usd_scene_sync.hh"
#include "usd_types_sync.hh"

#include "DNA_light_types.h"
#include "DNA_camera_types.h"

#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdLux/lightAPI.h>

namespace blender::io::usd {

/** Light Bidirectional Sync */
void sync_light(pxr::UsdPrim prim, Light *bl_light, bool is_push)
{
  pxr::UsdLuxLightAPI usd_light(prim);
  if (is_push) {
    usd_light.GetIntensityAttr().Set(bl_light->energy);
  }
  else {
    float intensity;
    usd_light.GetIntensityAttr().Get(&intensity);
    bl_light->energy = intensity;
  }
}

/** Camera Bidirectional Sync */
void sync_camera(pxr::UsdPrim prim, Camera *bl_cam, bool is_push)
{
  pxr::UsdGeomCamera usd_cam(prim);
  if (is_push) {
    usd_cam.GetFocalLengthAttr().Set(bl_cam->lens);
  }
  else {
    float focal;
    usd_cam.GetFocalLengthAttr().Get(&focal);
    bl_cam->lens = focal;
  }
}

}  // namespace blender::io::usd
