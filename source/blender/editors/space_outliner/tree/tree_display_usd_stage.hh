/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#ifdef WITH_USD

#  include "tree_display.hh"

#  include <string>
#  include <vector>

namespace blender::ed::outliner {

class TreeDisplayUsdStage final : public AbstractTreeDisplay {
  std::vector<std::string> prim_labels_;

 public:
  TreeDisplayUsdStage(SpaceOutliner &space_outliner);

  ListBaseT<TreeElement> build_tree(const TreeSourceData &source_data) override;

 private:
  void build_prim_subtree(ListBaseT<TreeElement> &tree,
                          TreeElement *parent,
                          void *stage_raw,
                          void *prim_raw);
};

}  // namespace blender::ed::outliner

#endif /* WITH_USD */
