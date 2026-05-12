/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct bContext;
namespace blender::ui {
struct Layout;
}

namespace blender {

void usd_stage_prim_tree_draw(const bContext *C, blender::ui::Layout &layout);

}  // namespace blender
