/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spusdstage
 *
 * USD Stage Editor — a dedicated space that shows the USD prim hierarchy
 * directly from an open UsdStage, keeping USD prims out of Blender's Outliner.
 */

#include <memory>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "space_usd_stage.hh"

/* -------------------------------------------------------------------- */
/** \name Space callbacks
 * \{ */

static SpaceLink *usd_stage_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  SpaceUsdStage *suss = MEM_new<SpaceUsdStage>("SpaceUsdStage");
  suss->spacetype = SPACE_USD_STAGE;

  /* Header region. */
  ARegion *region = MEM_new<ARegion>("usd stage header region");
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;
  BLI_addtail(&suss->regionbase, region);

  /* Main region. */
  region = MEM_new<ARegion>("usd stage main region");
  region->regiontype = RGN_TYPE_WINDOW;
  BLI_addtail(&suss->regionbase, region);

  return reinterpret_cast<SpaceLink *>(suss);
}

static void usd_stage_free(SpaceLink *sl)
{
  (void)sl;
}

static void usd_stage_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *usd_stage_duplicate(SpaceLink *sl)
{
  SpaceUsdStage *suss_old = reinterpret_cast<SpaceUsdStage *>(sl);
  SpaceUsdStage *suss_new = MEM_new<SpaceUsdStage>("SpaceUsdStage duplicate");
  *suss_new = *suss_old;
  return reinterpret_cast<SpaceLink *>(suss_new);
}

static void usd_stage_operatortypes() {}

static void usd_stage_keymap(wmKeyConfig * /*keyconf*/) {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main region callbacks
 * \{ */

static void usd_stage_main_region_init(wmWindowManager *wm, ARegion *region)
{
  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_LIST, region->winx, region->winy);
  WM_keymap_ensure(wm->defaultconf, "USD Stage", SPACE_USD_STAGE, RGN_TYPE_WINDOW);
}

static void usd_stage_main_region_draw(const bContext *C, ARegion *region)
{
  View2D *v2d = &region->v2d;

  /* Clear background. */
  UI_ThemeClearColor(TH_BACK);

  /* Draw placeholder text until live-sync is wired. */
  UI_view2d_view_ortho(v2d);

  const SpaceUsdStage *suss = reinterpret_cast<const SpaceUsdStage *>(
      CTX_wm_space_data(C));

  const char *msg = (suss->filepath[0] != '\0') ? suss->filepath :
                                                   "No USD stage open.\nUse File > Open USD…";

  /* Simple text in top-left of region. */
  const int x = 8;
  const int y = region->winy - 24;
  UI_FontThemeColor(BLF_default(), TH_TEXT);
  BLF_position(BLF_default(), x, y, 0.0f);
  BLF_draw(BLF_default(), msg, strlen(msg));

  UI_view2d_view_restore(C);
}

static void usd_stage_main_region_listener(const wmRegionListenerParams * /*params*/) {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Header region callbacks
 * \{ */

static void usd_stage_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void usd_stage_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void ED_spacetype_usd_stage()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();

  st->spaceid = SPACE_USD_STAGE;
  STRNCPY(st->name, "USD Stage");

  st->create = usd_stage_create;
  st->free = usd_stage_free;
  st->init = usd_stage_init;
  st->duplicate = usd_stage_duplicate;
  st->operatortypes = usd_stage_operatortypes;
  st->keymap = usd_stage_keymap;

  /* Main region. */
  ARegionType *art = MEM_new<ARegionType>("spacetype usd stage main region");
  art->regionid = RGN_TYPE_WINDOW;
  art->init = usd_stage_main_region_init;
  art->draw = usd_stage_main_region_draw;
  art->listener = usd_stage_main_region_listener;
  BLI_addhead(&st->regiontypes, art);

  /* Header region. */
  art = MEM_new<ARegionType>("spacetype usd stage header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->init = usd_stage_header_region_init;
  art->draw = usd_stage_header_region_draw;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}

/** \} */
