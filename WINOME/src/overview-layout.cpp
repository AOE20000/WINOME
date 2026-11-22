// SPDX-License-Identifier: GPL-2.0-only
// Origin: GNOME Shell js/ui/workspace.js (GPL-2.0-or-later)
// UnalignedLayoutStrategy, ported 1:1 for WINOME (2026-08-19). See
// overview-layout.h for the porting notes; the code below mirrors the
// upstream methods in order: computeLayout / computeScaleAndSpace /
// computeWindowSlots, driven by WorkspaceLayout._createBestLayout's
// row-search loop.

#include "overview-layout.h"

#include <algorithm>
#include <cmath>

namespace winome {

namespace {

// upstream constants (workspace.js)
constexpr double kWindowPreviewMaximumScale = 0.95;
constexpr double kLayoutScaleWeight = 1.0;
constexpr double kLayoutSpaceWeight = 0.1;

double
lerp (double start, double end, double t)
{
  return start + (end - start) * t;
}

struct Row {
  double x = 0, y = 0;
  double width = 0, height = 0;      // scaled, spacing included in width
  double fullWidth = 0, fullHeight = 0;
  std::vector<const OvWindowInfo *> windows;
  double additionalScale = 0;
};

struct Layout {
  int numRows = 0;
  std::vector<Row> rows;
  size_t maxColumns = 0;
  double gridWidth = 0;
  double gridHeight = 0;
  double scale = 0;
  double space = 0;
  // Effective spacing (theme 6px + chrome oversize), set by the caller of
  // the strategy entry point.
  double rowSpacing = 0;
  double columnSpacing = 0;
};

// _computeWindowScale: lerp(1.5, 1, bboxHeight / monitor.height)
double
compute_window_scale (const OvWindowInfo &window, double monitor_height)
{
  double h = std::max ((double)(window.rect.bottom - window.rect.top), 0.0);
  double ratio = monitor_height > 0 ? h / monitor_height : 1.0;
  return lerp (1.5, 1.0, ratio);
}

// _computeRowSizes
void
compute_row_sizes (Layout *layout)
{
  for (Row &row : layout->rows) {
    if (row.windows.empty ()) {
      row.width = 0;
      row.height = 0;
      continue;
    }
    row.width = row.fullWidth * layout->scale +
                (row.windows.size () - 1) * layout->columnSpacing;
    row.height = row.fullHeight * layout->scale;
  }
}

// _keepSameRow
bool
keep_same_row (const Row &row, double width, double ideal_row_width)
{
  if (row.windows.empty ())
    return true;

  if (row.fullWidth + width <= ideal_row_width)
    return true;

  if (ideal_row_width <= 0.0)
    return false;

  double old_ratio = row.fullWidth / ideal_row_width;
  double new_ratio = (row.fullWidth + width) / ideal_row_width;
  return std::fabs (1.0 - new_ratio) < std::fabs (1.0 - old_ratio);
}

double
window_center_x (const OvWindowInfo *w)
{
  return (w->rect.right - w->rect.left) / 2.0 + w->rect.left;
}

double
window_center_y (const OvWindowInfo *w)
{
  return (w->rect.bottom - w->rect.top) / 2.0 + w->rect.top;
}

// UnalignedLayoutStrategy.computeLayout
void
compute_layout (const std::vector<OvWindowInfo> &windows, int num_rows,
                double monitor_height, Layout *layout)
{
  std::vector<Row> &rows = layout->rows;

  double total_width = 0;
  for (const OvWindowInfo &window : windows) {
    double w = std::max ((double)(window.rect.right - window.rect.left), 0.0);
    total_width += w * compute_window_scale (window, monitor_height);
  }
  double ideal_row_width = num_rows > 0 ? total_width / num_rows : total_width;

  // Sort windows vertically to minimize travel distance.
  std::vector<const OvWindowInfo *> sorted;
  sorted.reserve (windows.size ());
  for (const OvWindowInfo &w : windows)
    sorted.push_back (&w);
  std::stable_sort (sorted.begin (), sorted.end (),
                    [] (const OvWindowInfo *a, const OvWindowInfo *b) {
                      return window_center_y (a) < window_center_y (b);
                    });

  size_t window_idx = 0;
  for (int i = 0; i < num_rows && window_idx < sorted.size (); ++i) {
    rows.emplace_back ();
    Row &row = rows.back ();

    for (; window_idx < sorted.size (); ++window_idx) {
      const OvWindowInfo *window = sorted[window_idx];
      double s = compute_window_scale (*window, monitor_height);
      double width =
          std::max ((double)(window->rect.right - window->rect.left), 0.0) * s;
      double height =
          std::max ((double)(window->rect.bottom - window->rect.top), 0.0) * s;

      if (keep_same_row (row, width, ideal_row_width) || i == num_rows - 1) {
        row.windows.push_back (window);
        row.fullWidth += width;
        row.fullHeight = std::max (row.fullHeight, height);
      } else {
        break;
      }
    }
  }

  rows.erase (
      std::remove_if (rows.begin (), rows.end (),
                      [] (const Row &r) { return r.windows.empty (); }),
      rows.end ());

  double grid_height = 0;
  const Row *max_row = nullptr;
  for (Row &row : rows) {
    // _sortRow: horizontal travel minimization.
    std::stable_sort (row.windows.begin (), row.windows.end (),
                      [] (const OvWindowInfo *a, const OvWindowInfo *b) {
                        return window_center_x (a) < window_center_x (b);
                      });

    if (max_row == nullptr || row.fullWidth > max_row->fullWidth)
      max_row = &row;
    grid_height += row.fullHeight;
  }

  layout->numRows = (int)rows.size ();
  layout->maxColumns = max_row ? max_row->windows.size () : 0;
  layout->gridWidth = max_row ? max_row->fullWidth : 0;
  layout->gridHeight = grid_height;
}

// UnalignedLayoutStrategy.computeScaleAndSpace
void
compute_scale_and_space (Layout *layout, const RECT &area)
{
  double aw = std::max ((double)(area.right - area.left), 0.0);
  double ah = std::max ((double)(area.bottom - area.top), 0.0);
  double hspacing = layout->maxColumns > 0
                        ? (layout->maxColumns - 1) * layout->columnSpacing
                        : 0.0;
  double vspacing = layout->numRows > 0
                        ? (layout->numRows - 1) * layout->rowSpacing
                        : 0.0;

  double spaced_width = std::max (aw - hspacing, 0.0);
  double spaced_height = std::max (ah - vspacing, 0.0);

  double horizontal_scale =
      layout->gridWidth > 0 ? spaced_width / layout->gridWidth : 1.0;
  double vertical_scale =
      layout->gridHeight > 0 ? spaced_height / layout->gridHeight : 1.0;

  double scale = std::clamp (
      std::min ({horizontal_scale, vertical_scale,
                 kWindowPreviewMaximumScale}),
      0.0, kWindowPreviewMaximumScale);

  double scaled_w = layout->gridWidth * scale + hspacing;
  double scaled_h = layout->gridHeight * scale + vspacing;
  double area_px = aw * ah;
  double space = area_px > 0 ? (scaled_w * scaled_h) / area_px : 0.0;

  layout->scale = scale;
  layout->space = space;
}

// WorkspaceLayout._isBetterScaleAndSpace
bool
is_better_layout (double old_scale, double old_space, double scale,
                  double space)
{
  double space_power = (space - old_space) * kLayoutSpaceWeight;
  double scale_power = (scale - old_scale) * kLayoutScaleWeight;

  if (scale > old_scale && space > old_space)
    return true;
  if (scale > old_scale && space <= old_space)
    return scale_power > space_power;
  if (scale <= old_scale && space > old_space)
    return space_power > scale_power;
  return false;
}

// UnalignedLayoutStrategy.computeWindowSlots
void
compute_window_slots (Layout *layout, const RECT &area, double monitor_height,
                      std::vector<OvSlot> *slots)
{
  compute_row_sizes (layout);

  std::vector<Row> &rows = layout->rows;
  if (rows.empty ())
    return;

  double aw = std::max ((double)(area.right - area.left), 0.0);
  double ah = std::max ((double)(area.bottom - area.top), 0.0);

  double height_without_spacing = 0;
  for (const Row &row : rows)
    height_without_spacing += row.height;

  double vertical_spacing = (rows.size () - 1) * layout->rowSpacing;
  double additional_vertical_scale =
      height_without_spacing > 0
          ? std::clamp ((ah - vertical_spacing) / height_without_spacing,
                        0.0, 1.0)
          : 1.0;

  double compensation = 0;
  double y = 0;

  for (Row &row : rows) {
    if (row.windows.empty ())
      continue;
    double horizontal_spacing = (row.windows.size () - 1) * layout->columnSpacing;
    double width_without_spacing = row.width - horizontal_spacing;
    double additional_horizontal_scale =
        width_without_spacing > 0
            ? std::clamp ((aw - horizontal_spacing) / width_without_spacing,
                          0.0, 1.0)
            : 1.0;

    if (additional_horizontal_scale < additional_vertical_scale) {
      row.additionalScale = additional_horizontal_scale;
      compensation += (additional_vertical_scale - additional_horizontal_scale) *
                      row.height;
    } else {
      row.additionalScale = additional_vertical_scale;
    }

    row.x = area.left +
            std::max (aw - (width_without_spacing * row.additionalScale +
                            horizontal_spacing),
                     0.0) /
                2;
    row.y = area.top +
            std::max (ah - (height_without_spacing + vertical_spacing), 0.0) /
                2 +
            y;
    y += row.height * row.additionalScale + layout->rowSpacing;
  }

  compensation /= 2;

  for (const Row &row : rows) {
    double row_y = row.y + compensation;
    double row_height = row.height * row.additionalScale;
    double x = row.x;

    for (const OvWindowInfo *window : row.windows) {
      double w =
          std::max ((double)(window->rect.right - window->rect.left), 0.0);
      double h =
          std::max ((double)(window->rect.bottom - window->rect.top), 0.0);

      // upstream: scale * windowScale * row.additionalScale, then clamped
      // to WINDOW_PREVIEW_MAXIMUM_SCALE for the clone itself (the cell keeps
      // the unclamped scale, the clone is centered/shrunk inside it).
      double cell_scale = layout->scale *
                          compute_window_scale (*window, monitor_height) *
                          row.additionalScale;
      double cell_w = w * cell_scale;
      double cell_h = h * cell_scale;

      double clone_scale = std::min (cell_scale, kWindowPreviewMaximumScale);
      double clone_w = w * clone_scale;
      double clone_h = h * clone_scale;

      double clone_x = x + (cell_w - clone_w) / 2;
      double clone_y;

      // Single row: vertically centered inside the row; multiple rows:
      // aligned to the bottom edge of the row.
      if (rows.size () == 1)
        clone_y = row_y + (row_height - clone_h) / 2;
      else
        clone_y = row_y + row_height - cell_h;

      OvSlot slot;
      slot.x = std::floor (clone_x);
      slot.y = std::floor (clone_y);
      slot.scale = clone_scale;
      slot.window = *window;
      slots->push_back (slot);

      x += cell_w + layout->columnSpacing;
    }
  }
}

}  // namespace

void
overview_compute_slots (const std::vector<OvWindowInfo> &windows,
                        const RECT &monitor_rect, const RECT &workarea,
                        const RECT &box, double row_spacing,
                        double column_spacing, std::vector<OvSlot> *slots)
{
  slots->clear ();
  if (windows.empty ())
    return;

  double monitor_height = monitor_rect.bottom - monitor_rect.top;

  // WorkspaceLayout._createBestLayout: search row counts, keep the best
  // scale/space trade-off.
  Layout last;
  last.scale = 0;
  last.space = 0;
  last.rowSpacing = row_spacing;
  last.columnSpacing = column_spacing;
  bool have_last = false;
  int last_num_columns = -1;

  for (int num_rows = 1;; ++num_rows) {
    int num_columns =
        (int)std::ceil ((double)windows.size () / num_rows);

    // Adding a row that does not change the column count never helps.
    if (num_columns == last_num_columns)
      break;

    Layout layout;
    layout.rowSpacing = row_spacing;
    layout.columnSpacing = column_spacing;
    compute_layout (windows, num_rows, monitor_height, &layout);
    compute_scale_and_space (&layout, workarea);

    if (have_last &&
        !is_better_layout (last.scale, last.space, layout.scale, layout.space))
      break;

    last = layout;
    last_num_columns = num_columns;
    have_last = true;
  }

  if (!have_last)
    return;

  compute_window_slots (&last, box, monitor_height, slots);
}

MiniThumbClipResult
compute_mini_thumb_clip (const RECT &full_dest, SIZE source_size,
                         const std::vector<RECT> &occluder_boxes)
{
  MiniThumbClipResult out;
  const LONG full_w = full_dest.right - full_dest.left;
  const LONG full_h = full_dest.bottom - full_dest.top;
  if (full_w < 1 || full_h < 1)
    return out;

  RECT vis = full_dest;
  for (const RECT &occ : occluder_boxes) {
    if (occ.right <= occ.left || occ.bottom <= occ.top)
      continue;
    const LONG ix1 = std::max (vis.left, occ.left);
    const LONG iy1 = std::max (vis.top, occ.top);
    const LONG ix2 = std::min (vis.right, occ.right);
    const LONG iy2 = std::min (vis.bottom, occ.bottom);
    if (ix1 >= ix2 || iy1 >= iy2)
      continue;

    // Evaluate the 4 axis-aligned remainder strips completely disjoint from @occ
    // and retain whichever leaves the largest unoccluded preview area.
    const RECT candidates[4] = {
        {vis.left, vis.top, vis.right, iy1},     // top strip above occ
        {vis.left, iy2, vis.right, vis.bottom},  // bottom strip below occ
        {vis.left, vis.top, ix1, vis.bottom},    // left strip outside occ
        {ix2, vis.top, vis.right, vis.bottom},   // right strip outside occ
    };
    long long best_area = 0;
    RECT best_rect{};
    for (const RECT &c : candidates) {
      const long long cw = static_cast<long long> (c.right) - c.left;
      const long long ch = static_cast<long long> (c.bottom) - c.top;
      if (cw > 0 && ch > 0 && cw * ch > best_area) {
        best_area = cw * ch;
        best_rect = c;
      }
    }
    if (best_area <= 0)
      return out;
    vis = best_rect;
  }

  const bool cropped =
      (vis.left != full_dest.left || vis.top != full_dest.top ||
       vis.right != full_dest.right || vis.bottom != full_dest.bottom);
  if (cropped && (vis.right - vis.left < 2 || vis.bottom - vis.top < 2))
    return out;

  if (!cropped) {
    out.visible = true;
    out.dest_rect = vis;
    out.src_rect = RECT{0, 0, source_size.cx, source_size.cy};
    return out;
  }

  if (source_size.cx <= 0 || source_size.cy <= 0)
    return out;

  const double u0 =
      static_cast<double> (vis.left - full_dest.left) / full_w;
  const double v0 =
      static_cast<double> (vis.top - full_dest.top) / full_h;
  const double u1 =
      static_cast<double> (vis.right - full_dest.left) / full_w;
  const double v1 =
      static_cast<double> (vis.bottom - full_dest.top) / full_h;

  LONG sl = std::clamp<LONG> (
      static_cast<LONG> (std::lround (u0 * source_size.cx)), 0,
      source_size.cx - 1);
  LONG st = std::clamp<LONG> (
      static_cast<LONG> (std::lround (v0 * source_size.cy)), 0,
      source_size.cy - 1);
  LONG sr = std::clamp<LONG> (
      static_cast<LONG> (std::lround (u1 * source_size.cx)), sl + 1,
      source_size.cx);
  LONG sb = std::clamp<LONG> (
      static_cast<LONG> (std::lround (v1 * source_size.cy)), st + 1,
      source_size.cy);

  out.visible = true;
  out.dest_rect = vis;
  out.src_rect = RECT{sl, st, sr, sb};
  return out;
}

bool
is_rect_fully_covered_by_boxes (const RECT &target,
                                const std::vector<RECT> &boxes)
{
  if (target.right <= target.left || target.bottom <= target.top)
    return true;
  for (const RECT &b : boxes) {
    if (b.left <= target.left && b.top <= target.top &&
        b.right >= target.right && b.bottom >= target.bottom) {
      return true;
    }
  }
  return false;
}

bool
is_rect_overlapping_any_box (const RECT &target,
                             const std::vector<RECT> &boxes)
{
  if (target.right <= target.left || target.bottom <= target.top)
    return false;
  for (const RECT &b : boxes) {
    if (b.right <= b.left || b.bottom <= b.top)
      continue;
    if (std::max (target.left, b.left) < std::min (target.right, b.right) &&
        std::max (target.top, b.top) < std::min (target.bottom, b.bottom)) {
      return true;
    }
  }
  return false;
}

RECT
transform_rect_around_pivot (const RECT &rect, double pivot_x, double pivot_y,
                             double scale, double translate_y)
{
  if (std::abs (scale - 1.0) < 1e-4 && std::abs (translate_y) < 0.25)
    return rect;
  RECT out{};
  out.left = static_cast<LONG> (
      std::lround (pivot_x + (static_cast<double> (rect.left) - pivot_x) * scale));
  out.right = static_cast<LONG> (
      std::lround (pivot_x + (static_cast<double> (rect.right) - pivot_x) * scale));
  out.top = static_cast<LONG> (
      std::lround (pivot_y + translate_y +
                   (static_cast<double> (rect.top) - pivot_y) * scale));
  out.bottom = static_cast<LONG> (
      std::lround (pivot_y + translate_y +
                   (static_cast<double> (rect.bottom) - pivot_y) * scale));
  return out;
}

WallpaperDrawRect
compute_workarea_wallpaper_draw_rect (double card_x, double card_y,
                                      double card_w, double card_h,
                                      double tex_w, double tex_h,
                                      double panel_top_ratio)
{
  WallpaperDrawRect out{};
  if (card_w <= 0.0 || card_h <= 0.0 || tex_w <= 0.0 || tex_h <= 0.0)
    return out;
  const double ratio = std::max (panel_top_ratio, 0.0);
  const double top_off = card_h * ratio;
  const double mon_x = card_x;
  const double mon_y = card_y - top_off;
  const double mon_w = card_w;
  const double mon_h = card_h + top_off;
  const double s = std::max (mon_w / tex_w, mon_h / tex_h);
  out.w = tex_w * s;
  out.h = tex_h * s;
  out.x = mon_x + (mon_w - out.w) * 0.5;
  out.y = mon_y + (mon_h - out.h) * 0.5;
  return out;
}

}  // namespace winome
