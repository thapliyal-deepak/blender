/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender::ui {
struct Layout;
}

namespace blender {

struct ARegionType;
struct bContext;

void usd_stage_prim_tree_draw(const bContext *C, blender::ui::Layout &layout);
void usd_stage_prim_tree_panel_register(ARegionType *art);

}  // namespace blender
