#pragma once

#include <pxr/usd/usd/prim.h>

/* Forward declarations. */
struct Light;
struct Camera;

namespace blender::io::usd {

/** Sync scene element properties. */
void sync_light(pxr::UsdPrim prim, Light *bl_light, bool is_push);
void sync_camera(pxr::UsdPrim prim, Camera *bl_cam, bool is_push);

}  // namespace blender::io::usd
