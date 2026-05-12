#pragma once

namespace blender::io::usd {

/* Bitmask for tracking what has changed to avoid redundant sync. */
enum UsdDirtyBits {
  USD_DIRTY_CLEAN = 0,
  USD_DIRTY_TRANSFORM = (1 << 0),
  USD_DIRTY_POINTS = (1 << 1),
  USD_DIRTY_TOPOLOGY = (1 << 2),
  USD_DIRTY_LIGHT = (1 << 3),
  USD_DIRTY_CAMERA = (1 << 4),
  USD_DIRTY_ALL = -1,
};

}  // namespace blender::io::usd
