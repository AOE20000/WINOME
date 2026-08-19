// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Overview dash, visual port of gnome-shell js/ui/dash.js. See the header
// for the geometry contract. Applications are discovered by enumerating the
// same alt-tab-eligible windows the overview picker shows, then grouped by
// AppUserModelID (UWP apps) or the owner process image (win32). Icons come
// from (in order) WM_GETICON, the window class icon and
// IShellItemImageFactory on the exe path, converted to GdkTexture.

#include "overview-dash.h"
#include "overview-trigger.h"
#include "st-engine.h"
#include "system-status.h"

#include <gdk/gdk.h>
#include <dwmapi.h>

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <vector>

namespace winome {

namespace {

// dash.js baseIconSizes
constexpr int kBaseIconSizes[] = {16, 22, 24, 32, 48, 64};
constexpr int kBaseIconSizesCount = 6;

// Compiled theme values (see _dash.scss / gnome-shell-dark.css).
constexpr double kDashPillRadius = 28.0;
constexpr int kDashPadV = 12;
constexpr int kDashPadH = 10;
constexpr int kDashSidePadding = 6;   // #dash padding-left/right
constexpr int kDashBottomMargin = 12; // .dash-background margin-bottom
constexpr int kTilePad = 6;           // .overview-icon padding
constexpr double kTileRadius = 16.0;
constexpr int kItemGap = 4;           // margin 0 2px on both neighbours
constexpr int kDotSize = 5;
constexpr int kLabelHoverTimeoutMs = 300; // DASH_ITEM_HOVER_TIMEOUT
constexpr int kLabelOffset = 8;           // .dash-label -y-offset

// PKEY_AppUserModel_ID (propsys), defined locally to avoid header churn.
constexpr GUID kPKEY_AppUserModel_ID_fmt = {
    0x9F4C2855, 0x9F79, 0x4B39,
    {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};

// BHID_SFUIObject (shlguid.h) for IShellItem::BindToHandler; MinGW's
// shobjidl.h doesn't pull shlguid.h in, so define the GUID locally.
constexpr GUID kBHID_SFUIObject = {
    0x3981e225, 0xf559, 0x11d3,
    {0x8e, 0x3a, 0x00, 0xc0, 0x4f, 0x68, 0x37, 0xd5}};

struct DashApp {
  std::wstring key;          // AUMID or lowercased exe path
  std::string name;          // hover label text (UTF-8)
  GdkTexture *icon = nullptr;
  std::vector<HWND> windows; // z-order, topmost first
};

struct DashTileRect {
  double x, y, w, h; // pill-relative
};

struct WinomeDash {
  GtkWidget parent_instance;

  GtkWidget *root_fixed;  // overview root (owns the hover label)
  GtkWidget *label;       // .dash-label GtkLabel
  guint label_timeout;    // hover delay source

  std::vector<DashApp> apps;
  int icon_size = 64;      // logical px

  int pill_x = 0, pill_y = 0, pill_w = 0, pill_h = 0; // widget-relative
  std::vector<DashTileRect> tile_rects;
  int hover_index = -1;    // -1 = none

  GdkRGBA pill_color;
  GdkRGBA tile_color;
  GdkRGBA tile_hover_color;
  GdkRGBA fg_color;
  bool colors_resolved = false;
};

struct WinomeDashClass {
  GtkWidgetClass parent_class;
};

#define WINOME_DASH(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_dash_get_type (), WinomeDash))

G_DEFINE_TYPE (WinomeDash, winome_dash, GTK_TYPE_WIDGET)

// --- theme lookups (StEngine, with compiled-CSS fallbacks) -----------------

static void
resolve_colors (WinomeDash *self)
{
  if (self->colors_resolved)
    return;
  self->colors_resolved = true;

  self->pill_color = (GdkRGBA){0.22f, 0.22f, 0.23f, 1.0f};   // #38383b
  self->tile_color = self->pill_color;                        // flat on dash
  self->tile_hover_color = (GdkRGBA){0.25f, 0.25f, 0.26f, 1.0f};
  self->fg_color = (GdkRGBA){0.98f, 0.98f, 0.98f, 1.0f};      // #fafafb

  StEngine *engine = StEngine::get ();
  if (engine == nullptr)
    return;

  // #dash .dash-background { background-color }
  StEngine::Node dash (*engine, nullptr, "dash", "", "");
  StEngine::Node bg (*engine, &dash, "", "dash-background", "");
  std::string color;
  if (bg.lookup_color ("background-color", &color) &&
      gdk_rgba_parse (&self->pill_color, color.c_str ()))
    self->tile_color = self->pill_color;

  // Hover: st-lighten(#38383b, 7%) ~= #414146
  StEngine::Node item (*engine, &dash, "", "dash-item-container", "");
  StEngine::Node tile (*engine, &item, "", "overview-tile", "hover");
  StEngine::Node icon (*engine, &tile, "", "overview-icon", "");
  if (icon.lookup_color ("background-color", &color))
    gdk_rgba_parse (&self->tile_hover_color, color.c_str ());

  StEngine::Node tile_plain (*engine, &item, "", "overview-tile", "");
  StEngine::Node icon_plain (*engine, &tile_plain, "", "overview-icon", "");
  if (icon_plain.lookup_color ("color", &color))
    gdk_rgba_parse (&self->fg_color, color.c_str ());
}

// --- app discovery ----------------------------------------------------------

bool
dash_is_our_process (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  return pid == GetCurrentProcessId ();
}

bool
dash_is_alt_tab_window (HWND hwnd)
{
  if (!IsWindowVisible (hwnd))
    return false;

  HWND try_ = GetAncestor (hwnd, GA_ROOTOWNER);
  HWND walk = nullptr;
  while (try_ != walk) {
    walk = try_;
    try_ = GetLastActivePopup (walk);
    if (IsWindowVisible (try_))
      break;
  }
  if (walk != hwnd)
    return false;

  if (GetWindowLongPtrW (hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    return false;

  return true;
}

bool
dash_is_cloaked (HWND hwnd)
{
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute (hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));
  return cloaked != FALSE;
}

std::wstring
window_aumid (HWND hwnd)
{
  std::wstring aumid;
  IPropertyStore *store = nullptr;
  // Local definition to dodge propsys propagation into every TU.
  struct WinomePropKey {
    GUID fmtid;
    DWORD pid;
  };
  WinomePropKey key{kPKEY_AppUserModel_ID_fmt, 5};

  if (SUCCEEDED (SHGetPropertyStoreForWindow (hwnd, IID_PPV_ARGS (&store)))) {
    PROPVARIANT pv;
    PropVariantInit (&pv);
    if (SUCCEEDED (store->GetValue (
            *reinterpret_cast<PROPERTYKEY *> (&key), &pv)) &&
        (pv.vt == VT_LPWSTR) && pv.pwszVal != nullptr) {
      aumid = pv.pwszVal;
    }
    PropVariantClear (&pv);
    store->Release ();
  }
  return aumid;
}

std::wstring
window_exe_path (HWND hwnd)
{
  DWORD pid = 0;
  GetWindowThreadProcessId (hwnd, &pid);
  if (pid == 0)
    return L"";

  HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return L"";

  wchar_t path[MAX_PATH];
  DWORD size = MAX_PATH;
  std::wstring result;
  if (QueryFullProcessImageNameW (process, 0, path, &size))
    result.assign (path, size);
  CloseHandle (process);
  return result;
}

std::string
utf8 (const std::wstring &w)
{
  if (w.empty ())
    return {};
  int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (),
                               nullptr, 0, nullptr, nullptr);
  std::string out (n, 0);
  WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (), out.data (),
                       n, nullptr, nullptr);
  return out;
}

std::wstring
wide (const char *s)
{
  if (s == nullptr)
    return L"";
  int n = MultiByteToWideChar (CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out (n > 0 ? n - 1 : 0, L'\0');
  if (n > 0)
    MultiByteToWideChar (CP_UTF8, 0, s, -1, out.data (), n);
  return out;
}

std::string
exe_file_description (const std::wstring &exe)
{
  DWORD zero = 0;
  DWORD size = GetFileVersionInfoSizeW (exe.c_str (), &zero);
  if (size == 0)
    return {};
  std::vector<char> buffer (size);
  if (!GetFileVersionInfoW (exe.c_str (), 0, size, buffer.data ()))
    return {};

  struct LangCodePage {
    WORD language;
    WORD code_page;
  } *langs = nullptr;
  UINT count = 0;
  std::string result;
  if (VerQueryValueW (buffer.data (), L"\\VarFileInfo\\Translation",
                      (LPVOID *)&langs, &count) &&
      count > 0) {
    wchar_t query[128];
    swprintf (query, 128, L"\\StringFileInfo\\%04x%04x\\FileDescription",
              langs[0].language, langs[0].code_page);
    wchar_t *desc = nullptr;
    UINT len = 0;
    if (VerQueryValueW (buffer.data (), query, (LPVOID *)&desc, &len) &&
        desc != nullptr)
      result = utf8 (desc);
  }
  return result;
}

GdkTexture *
hicon_to_texture (HICON icon)
{
  if (icon == nullptr)
    return nullptr;

  ICONINFO info = {};
  if (!GetIconInfo (icon, &info))
    return nullptr;

  int w = 0, h = 0;
  bool has_color = info.hbmColor != nullptr;
  if (has_color) {
    BITMAP bm = {};
    GetObjectW (info.hbmColor, sizeof (bm), &bm);
    w = bm.bmWidth;
    h = bm.bmHeight;
  } else if (info.hbmMask != nullptr) {
    BITMAP bm = {};
    GetObjectW (info.hbmMask, sizeof (bm), &bm);
    w = bm.bmWidth;
    h = bm.bmHeight / 2;
  }
  if (w <= 0 || h <= 0) {
    if (info.hbmColor) DeleteObject (info.hbmColor);
    if (info.hbmMask) DeleteObject (info.hbmMask);
    return nullptr;
  }

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC (nullptr);
  void *bits = nullptr;
  HBITMAP dib = CreateDIBSection (screen, &bi, DIB_RGB_COLORS, &bits,
                                  nullptr, 0);
  HDC mem = CreateCompatibleDC (screen);
  HGDIOBJ old = SelectObject (mem, dib);
  BOOL ok = DrawIconEx (mem, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);
  SelectObject (mem, old);
  DeleteDC (mem);
  ReleaseDC (nullptr, screen);

  GdkTexture *texture = nullptr;
  if (ok && bits != nullptr) {
    GBytes *bytes = g_bytes_new (bits, (gsize)w * h * 4);
    texture = gdk_memory_texture_new (w, h,
                                      GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                      bytes, (gsize)w * 4);
    g_bytes_unref (bytes);
  }

  DeleteObject (dib);
  if (info.hbmColor) DeleteObject (info.hbmColor);
  if (info.hbmMask) DeleteObject (info.hbmMask);
  return texture;
}

GdkTexture *
texture_from_hbitmap (HBITMAP bmp)
{
  if (bmp == nullptr)
    return nullptr;

  BITMAP bm = {};
  GetObjectW (bmp, sizeof (bm), &bm);
  if (bm.bmWidth <= 0 || bm.bmHeight <= 0)
    return nullptr;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bm.bmWidth;
  bi.bmiHeader.biHeight = -bm.bmHeight;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC (nullptr);
  void *bits = nullptr;
  HBITMAP dib = CreateDIBSection (screen, &bi, DIB_RGB_COLORS, &bits,
                                  nullptr, 0);
  GdkTexture *texture = nullptr;
  if (dib != nullptr && bits != nullptr) {
    HDC src = CreateCompatibleDC (screen);
    HDC dst = CreateCompatibleDC (screen);
    HGDIOBJ old_src = SelectObject (src, bmp);
    HGDIOBJ old_dst = SelectObject (dst, dib);
    if (BitBlt (dst, 0, 0, bm.bmWidth, bm.bmHeight, src, 0, 0, SRCCOPY)) {
      GBytes *bytes = g_bytes_new (bits,
                                   (gsize)bm.bmWidth * bm.bmHeight * 4);
      texture = gdk_memory_texture_new (bm.bmWidth, bm.bmHeight,
                                        GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                        bytes, (gsize)bm.bmWidth * 4);
      g_bytes_unref (bytes);
    }
    SelectObject (src, old_src);
    SelectObject (dst, old_dst);
    DeleteDC (src);
    DeleteDC (dst);
  }
  if (dib != nullptr)
    DeleteObject (dib);
  ReleaseDC (nullptr, screen);
  return texture;
}

// Best-effort application icon at (at least) @size physical px.
GdkTexture *
app_icon_for (HWND hwnd, const std::wstring &exe, int size)
{
  // 1. Application-provided big icon.
  HICON icon = (HICON)SendMessageTimeoutW (hwnd, WM_GETICON, ICON_BIG, 0,
                                           SMTO_ABORTIFHUNG, 100, nullptr);
  // 2. Class icon.
  if (icon == nullptr)
    icon = (HICON)GetClassLongPtrW (hwnd, GCLP_HICON);

  GdkTexture *texture = hicon_to_texture (icon);
  if (icon != nullptr)
    DestroyIcon (icon);
  if (texture != nullptr &&
      gdk_texture_get_width (texture) >= size)
    return texture;
  g_clear_object (&texture);

  // 3. Shell item image factory on the exe (crisp scaling, alpha).
  if (!exe.empty ()) {
    HRESULT hr = CoInitializeEx (nullptr,
                                 COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool com = SUCCEEDED (hr);
    IShellItem *item = nullptr;
    if (SUCCEEDED (SHCreateItemFromParsingName (exe.c_str (), nullptr,
                                                IID_PPV_ARGS (&item)))) {
      IShellItemImageFactory *factory = nullptr;
      if (SUCCEEDED (item->BindToHandler (nullptr, kBHID_SFUIObject,
                                          IID_PPV_ARGS (&factory)))) {
        HBITMAP bmp = nullptr;
        if (SUCCEEDED (factory->GetImage (SIZE{size, size},
                                          SIIGBF_ICONONLY |
                                              SIIGBF_BIGGERSIZEOK,
                                          &bmp))) {
          texture = texture_from_hbitmap (bmp);
          DeleteObject (bmp);
        }
        factory->Release ();
      }
      item->Release ();
    }
    if (com)
      CoUninitialize ();
    if (texture != nullptr)
      return texture;
  }

  // 4. Classic SHGetFileInfo large icon.
  if (icon == nullptr && !exe.empty ()) {
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW (exe.c_str (), 0, &sfi, sizeof (sfi),
                        SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon != nullptr) {
      texture = hicon_to_texture (sfi.hIcon);
      DestroyIcon (sfi.hIcon);
    }
  }
  return texture;
}

struct EnumCtx {
  HMONITOR monitor;
  std::vector<HWND> *windows;
};

BOOL CALLBACK
dash_collect (HWND hwnd, LPARAM lparam)
{
  EnumCtx *ctx = reinterpret_cast<EnumCtx *> (lparam);
  if (dash_is_our_process (hwnd) || dash_is_cloaked (hwnd) ||
      !dash_is_alt_tab_window (hwnd))
    return TRUE;

  RECT r = {};
  if (IsIconic (hwnd)) {
    WINDOWPLACEMENT wp = {sizeof (wp)};
    if (GetWindowPlacement (hwnd, &wp))
      r = wp.rcNormalPosition;
  } else {
    GetWindowRect (hwnd, &r);
  }
  if (r.right <= r.left || r.bottom <= r.top)
    return TRUE;
  if (MonitorFromRect (&r, MONITOR_DEFAULTTONULL) != ctx->monitor)
    return TRUE;

  ctx->windows->push_back (hwnd);
  return TRUE;
}

void
clear_apps (WinomeDash *self)
{
  for (DashApp &app : self->apps) {
    if (app.icon != nullptr)
      g_object_unref (app.icon);
  }
  self->apps.clear ();
}

void
recompute_layout (WinomeDash *self)
{
  self->tile_rects.clear ();

  int tiles = (int)self->apps.size () + 1; // apps + show-apps
  int tile = self->icon_size + 2 * kTilePad;

  self->pill_w = 2 * kDashPadH + tiles * tile + (tiles - 1) * kItemGap;
  self->pill_h = 2 * kDashPadV + tile;
  self->pill_x = kDashSidePadding;
  self->pill_y = 0;

  double x = self->pill_x + kDashPadH;
  double y = self->pill_y + kDashPadV;
  for (int i = 0; i < tiles; ++i) {
    self->tile_rects.push_back ({x, y, (double)tile, (double)tile});
    x += tile + kItemGap;
  }
}

void
place_label (WinomeDash *self, int index)
{
  if (index < 0 || index >= (int)self->tile_rects.size ())
    return;

  DashApp *app =
      index < (int)self->apps.size () ? &self->apps[index] : nullptr;
  const char *text = app ? app->name.c_str () : "Show Apps";
  if (strcmp (get_system_language (), "zh") == 0 && app == nullptr)
    text = "显示应用";
  gtk_label_set_text (GTK_LABEL (self->label), text);

  GtkRequisition natural;
  gtk_widget_get_preferred_size (self->label, nullptr, &natural);

  double tile_cx = self->tile_rects[index].x + self->tile_rects[index].w / 2;
  double tile_top = self->tile_rects[index].y;

  double origin_x = 0, origin_y = 0;
  if (gtk_widget_translate_coordinates (GTK_WIDGET (self), self->root_fixed, 0,
                                        0, &origin_x, &origin_y)) {
    double cx = origin_x + tile_cx;
    double top = origin_y + tile_top;
    double x = cx - natural.width / 2.0;
    double y = top - natural.height - kLabelOffset;
    gtk_fixed_move (GTK_FIXED (self->root_fixed), self->label,
                    (int)x, (int)y);
  }

  gtk_widget_set_visible (self->label, TRUE);
}

void
hide_label (WinomeDash *self)
{
  if (self->label_timeout != 0) {
    g_source_remove (self->label_timeout);
    self->label_timeout = 0;
  }
  gtk_widget_set_visible (self->label, FALSE);
}

gboolean
label_timeout_cb (gpointer user_data)
{
  WinomeDash *self = WINOME_DASH (user_data);
  self->label_timeout = 0;
  place_label (self, self->hover_index);
  return G_SOURCE_REMOVE;
}

int
tile_at (WinomeDash *self, double x, double y)
{
  for (int i = 0; i < (int)self->tile_rects.size (); ++i) {
    const DashTileRect &r = self->tile_rects[i];
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
      return i;
  }
  return -1;
}

void
activate_app (WinomeDash *self, int index)
{
  if (index < 0 || index >= (int)self->apps.size ())
    return;

  HWND best = nullptr;
  for (HWND hwnd : self->apps[index].windows) {
    if (!IsWindow (hwnd))
      continue;
    best = hwnd;
    break;
  }
  if (best == nullptr)
    return;

  if (IsIconic (best))
    ShowWindow (best, SW_RESTORE);
  SetForegroundWindow (best);
  close_overview ("dash-activate");
}

void
open_start_menu_and_close ()
{
  // The WINOME overview has no app grid yet; the Windows Start menu is the
  // platform's application launcher, matching what Win+Tab is remapped to.
  // open_start_menu() injects a tagged Win press our hook passes through.
  close_overview ("show-apps");
  open_start_menu ();
}

void
on_dash_motion_enter (GtkEventControllerMotion *motion, double x, double y,
                      gpointer user_data)
{
  (void)motion;
  WinomeDash *self = WINOME_DASH (user_data);
  int index = tile_at (self, x, y);
  if (index != self->hover_index) {
    self->hover_index = index;
    gtk_widget_queue_draw (GTK_WIDGET (self));

    if (self->label_timeout != 0) {
      g_source_remove (self->label_timeout);
      self->label_timeout = 0;
    }
    if (index >= 0)
      self->label_timeout =
          g_timeout_add (kLabelHoverTimeoutMs, label_timeout_cb, self);
    else
      gtk_widget_set_visible (self->label, FALSE);
  }
}

void
on_dash_motion (GtkEventControllerMotion *motion, double x, double y,
                gpointer user_data)
{
  (void)motion;
  on_dash_motion_enter (nullptr, x, y, user_data);
}

void
on_dash_motion_leave (GtkEventControllerMotion *motion, gpointer user_data)
{
  (void)motion;
  WinomeDash *self = WINOME_DASH (user_data);
  if (self->hover_index != -1) {
    self->hover_index = -1;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
  hide_label (self);
}

void
on_dash_click (GtkGestureClick *gesture, int n_press, double x, double y,
               gpointer user_data)
{
  (void)n_press;
  if (gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)) !=
      GDK_BUTTON_PRIMARY)
    return;

  WinomeDash *self = WINOME_DASH (user_data);
  int index = tile_at (self, x, y);
  if (index < 0)
    return;

  if (index < (int)self->apps.size ())
    activate_app (self, index);
  else
    open_start_menu_and_close ();
}

// --- widget ------------------------------------------------------------------

static void
winome_dash_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeDash *self = WINOME_DASH (widget);
  resolve_colors (self);

  int w = gtk_widget_get_width (widget);
  int h = gtk_widget_get_height (widget);
  if (w <= 0 || h <= 0)
    return;

  // .dash-background pill.
  graphene_rect_t pill_rect;
  graphene_rect_init (&pill_rect, self->pill_x, self->pill_y, self->pill_w,
                      self->pill_h);
  GskRoundedRect pill;
  gsk_rounded_rect_init_from_rect (&pill, &pill_rect, (float)kDashPillRadius);
  gtk_snapshot_push_rounded_clip (snapshot, &pill);
  gtk_snapshot_append_color (snapshot, &self->pill_color, &pill_rect);
  gtk_snapshot_pop (snapshot);

  // Tiles.
  GdkRGBA *hover = &self->tile_hover_color;
  for (int i = 0; i < (int)self->tile_rects.size (); ++i) {
    const DashTileRect &r = self->tile_rects[i];

    // .overview-icon tile background (hover only, flat otherwise).
    if (i == self->hover_index) {
      graphene_rect_t rect;
      graphene_rect_init (&rect, r.x, r.y, r.w, r.h);
      GskRoundedRect rounded;
      gsk_rounded_rect_init_from_rect (&rounded, &rect, (float)kTileRadius);
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      gtk_snapshot_append_color (snapshot, hover, &rect);
      gtk_snapshot_pop (snapshot);
    }

    // Icon.
    GdkTexture *texture = nullptr;
    if (i < (int)self->apps.size ())
      texture = self->apps[i].icon;

    if (texture != nullptr) {
      graphene_rect_t icon_rect;
      graphene_rect_init (&icon_rect, r.x + kTilePad, r.y + kTilePad,
                          self->icon_size, self->icon_size);
      // Letterbox the texture inside the square.
      double tw = gdk_texture_get_width (texture);
      double th = gdk_texture_get_height (texture);
      double scale = std::min (self->icon_size / tw, self->icon_size / th);
      double dw = tw * scale, dh = th * scale;
      graphene_rect_init (&icon_rect, r.x + kTilePad + (self->icon_size - dw) / 2,
                          r.y + kTilePad + (self->icon_size - dh) / 2, dw, dh);
      gtk_snapshot_append_texture (snapshot, texture, &icon_rect);
    } else if (i == (int)self->apps.size ()) {
      // Show Apps: view-app-grid-symbolic as a crisp 3x3 dot grid.
      double pad = self->icon_size * 0.16;
      double grid = self->icon_size - 2 * pad;
      double dot_r = grid / 8.5;
      double step = grid / 3.0;
      double x0 = r.x + kTilePad + pad, y0 = r.y + kTilePad + pad;
      for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
          graphene_rect_t dot;
          graphene_rect_init (&dot, x0 + dx * step + step / 2 - dot_r,
                              y0 + dy * step + step / 2 - dot_r,
                              2 * dot_r, 2 * dot_r);
          GskRoundedRect rr;
          gsk_rounded_rect_init_from_rect (&rr, &dot, (float)dot_r);
          gtk_snapshot_push_rounded_clip (snapshot, &rr);
          gtk_snapshot_append_color (snapshot, &self->fg_color, &dot);
          gtk_snapshot_pop (snapshot);
        }
      }
    }

    // Running dot (every item here is a running app; the show-apps tile has
    // no dot, exactly like the GNOME dash).
    if (i < (int)self->apps.size ()) {
      double cx = r.x + r.w / 2.0;
      double dy = r.y + r.h - kTilePad - kDotSize / 2.0 - 0.5;
      graphene_rect_t dot;
      graphene_rect_init (&dot, cx - kDotSize / 2.0, dy, kDotSize, kDotSize);
      GskRoundedRect rr;
      gsk_rounded_rect_init_from_rect (&rr, &dot, (float)(kDotSize / 2.0));
      gtk_snapshot_push_rounded_clip (snapshot, &rr);
      gtk_snapshot_append_color (snapshot, &self->fg_color, &dot);
      gtk_snapshot_pop (snapshot);
    }
  }
}

static void
winome_dash_measure (GtkWidget *widget, GtkOrientation orientation,
                     int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  (void)for_size;
  WinomeDash *self = WINOME_DASH (widget);
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *natural = self->pill_w + 2 * kDashSidePadding;
  else
    *natural = self->pill_h + kDashBottomMargin;
  *minimum = *natural;
  *minimum_baseline = *natural_baseline = -1;
}

static void
winome_dash_init (WinomeDash *self)
{
  self->hover_index = -1;
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "default");
}

static void
winome_dash_dispose (GObject *object)
{
  WinomeDash *self = WINOME_DASH (object);
  hide_label (self);
  if (self->label != nullptr) {
    gtk_fixed_remove (GTK_FIXED (self->root_fixed), self->label);
    self->label = nullptr;
  }
  clear_apps (self);
  G_OBJECT_CLASS (winome_dash_parent_class)->dispose (object);
}

static void
winome_dash_class_init (WinomeDashClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->snapshot = winome_dash_snapshot;
  widget_class->measure = winome_dash_measure;

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = winome_dash_dispose;
}

}  // namespace

// Exported for the overview window-preview hover chrome (windowPreview.js
// shows the window's application icon at ICON_SIZE 64 logical px on hover).
GdkTexture *
winome_window_app_icon (HWND hwnd, int size)
{
  return app_icon_for (hwnd, window_exe_path (hwnd), size);
}

GtkWidget *
overview_dash_new (GtkWidget *root_fixed)
{
  WinomeDash *self =
      (WinomeDash *)g_object_new (winome_dash_get_type (), nullptr);
  self->root_fixed = root_fixed;

  // Hover label (.dash-label pill), floating above the dash.
  self->label = gtk_label_new ("");
  gtk_widget_add_css_class (self->label, "dash-label");
  gtk_widget_set_visible (self->label, FALSE);
  gtk_fixed_put (GTK_FIXED (root_fixed), self->label, 0, 0);

  // Controllers: motion for hover + label, click for activation.
  GtkEventController *motion =
      gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_dash_motion_enter), self);
  g_signal_connect (motion, "motion", G_CALLBACK (on_dash_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_dash_motion_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_dash_click), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  return GTK_WIDGET (self);
}

void
overview_dash_repopulate (GtkWidget *dash, HMONITOR monitor, int max_height)
{
  WinomeDash *self = WINOME_DASH (dash);
  hide_label (self);
  self->hover_index = -1;
  clear_apps (self);

  // Icon size: largest base size that fits the available dash height
  // (dash.js _adjustIconSize, simplified to the height constraint).
  double scale = gtk_widget_get_scale_factor (dash);
  int avail = max_height - 2 * kDashPadV - 2 * kTilePad - kDashBottomMargin;
  self->icon_size = kBaseIconSizes[0];
  for (int i = 0; i < kBaseIconSizesCount; ++i) {
    if (kBaseIconSizes[i] <= avail)
      self->icon_size = kBaseIconSizes[i];
  }
  int icon_px = (int)(self->icon_size * scale + 0.5);

  // Enumerate windows top-down and group into applications.
  std::vector<HWND> windows;
  EnumCtx ctx{monitor, &windows};
  EnumWindows (dash_collect, reinterpret_cast<LPARAM> (&ctx));

  struct KeyOrder {
    std::wstring key;
    size_t index;
  };
  std::vector<KeyOrder> order;

  for (HWND hwnd : windows) {
    std::wstring key = window_aumid (hwnd);
    std::wstring exe = window_exe_path (hwnd);
    if (key.empty ()) {
      key = exe;
      std::transform (key.begin (), key.end (), key.begin (), towlower);
    }

    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < self->apps.size (); ++i) {
      if (self->apps[i].key == key) {
        idx = i;
        break;
      }
    }
    if (idx == SIZE_MAX) {
      DashApp app;
      app.key = key;
      self->apps.push_back (std::move (app));
      idx = self->apps.size () - 1;
      order.push_back ({key, idx});
    }
    self->apps[idx].windows.push_back (hwnd);

    // Name + icon resolved lazily on first window of the group.
    if (self->apps[idx].windows.size () == 1) {
      wchar_t title[256] = L"";
      GetWindowTextW (hwnd, title, 256);

      std::wstring exe_lower = exe;
      std::transform (exe_lower.begin (), exe_lower.end (), exe_lower.begin (),
                      towlower);
      bool is_frame_host =
          exe_lower.find (L"applicationframehost.exe") != std::wstring::npos;

      std::string name;
      if (is_frame_host || exe.empty ())
        name = utf8 (title);
      else {
        name = exe_file_description (exe);
        if (name.empty ())
          name = utf8 (title);
      }
      if (name.empty ())
        name = "Application";
      self->apps[idx].name = name;

      self->apps[idx].icon = app_icon_for (hwnd, exe, icon_px);
    }
  }

  recompute_layout (self);
  gtk_widget_queue_resize (dash);
}

int
overview_dash_get_width (GtkWidget *dash)
{
  WinomeDash *self = WINOME_DASH (dash);
  return self->pill_w + 2 * kDashSidePadding;
}

int
overview_dash_get_height (GtkWidget *dash)
{
  WinomeDash *self = WINOME_DASH (dash);
  return self->pill_h + kDashBottomMargin;
}

}  // namespace winome
