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
  double h = window.rect.bottom - window.rect.top;
  double ratio = monitor_height > 0 ? h / monitor_height : 1.0;
  return lerp (1.5, 1.0, ratio);
}

// _computeRowSizes
void
compute_row_sizes (Layout *layout)
{
  for (Row &row : layout->rows) {
    row.width = row.fullWidth * layout->scale +
                (row.windows.size () - 1) * layout->columnSpacing;
    row.height = row.fullHeight * layout->scale;
  }
}

// _keepSameRow
bool
keep_same_row (const Row &row, double width, double ideal_row_width)
{
  if (row.fullWidth + width <= ideal_row_width)
    return true;

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
  for (const OvWindowInfo &window : windows)
    total_width += (window.rect.right - window.rect.left) *
                   compute_window_scale (window, monitor_height);
  double ideal_row_width = total_width / num_rows;

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
  for (int i = 0; i < num_rows; ++i) {
    rows.emplace_back ();
    Row &row = rows.back ();

    for (; window_idx < sorted.size (); ++window_idx) {
      const OvWindowInfo *window = sorted[window_idx];
      double s = compute_window_scale (*window, monitor_height);
      double width = (window->rect.right - window->rect.left) * s;
      double height = (window->rect.bottom - window->rect.top) * s;
      row.fullHeight = std::max (row.fullHeight, height);

      if (keep_same_row (row, width, ideal_row_width) || i == num_rows - 1) {
        row.windows.push_back (window);
        row.fullWidth += width;
      } else {
        break;
      }
    }
  }

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

  layout->numRows = num_rows;
  layout->maxColumns = max_row ? max_row->windows.size () : 0;
  layout->gridWidth = max_row ? max_row->fullWidth : 0;
  layout->gridHeight = grid_height;
}

// UnalignedLayoutStrategy.computeScaleAndSpace
void
compute_scale_and_space (Layout *layout, const RECT &area)
{
  double aw = area.right - area.left;
  double ah = area.bottom - area.top;
  double hspacing = (layout->maxColumns - 1) * layout->columnSpacing;
  double vspacing = (layout->numRows - 1) * layout->rowSpacing;

  double spaced_width = aw - hspacing;
  double spaced_height = ah - vspacing;

  double horizontal_scale =
      layout->gridWidth > 0 ? spaced_width / layout->gridWidth : 1.0;
  double vertical_scale =
      layout->gridHeight > 0 ? spaced_height / layout->gridHeight : 1.0;

  double scale = std::min ({horizontal_scale, vertical_scale,
                            kWindowPreviewMaximumScale});

  double scaled_w = layout->gridWidth * scale + hspacing;
  double scaled_h = layout->gridHeight * scale + vspacing;
  double space = (scaled_w * scaled_h) / (aw * ah);

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
  double aw = area.right - area.left;
  double ah = area.bottom - area.top;

  double height_without_spacing = 0;
  for (const Row &row : rows)
    height_without_spacing += row.height;

  double vertical_spacing = (rows.size () - 1) * layout->rowSpacing;
  double additional_vertical_scale =
      std::min (1.0, (ah - vertical_spacing) / height_without_spacing);

  double compensation = 0;
  double y = 0;

  for (Row &row : rows) {
    double horizontal_spacing = (row.windows.size () - 1) * layout->columnSpacing;
    double width_without_spacing = row.width - horizontal_spacing;
    double additional_horizontal_scale =
        std::min (1.0, (aw - horizontal_spacing) / width_without_spacing);

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
      double w = window->rect.right - window->rect.left;
      double h = window->rect.bottom - window->rect.top;

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

}  // namespace winome
