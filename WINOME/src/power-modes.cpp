// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows power mode switching (PowrProf Overlay Schemes + legacy fallback).

#include "power-modes.h"
#include "i18n.h"

#include <windows.h>
#include <powrprof.h>
#include <powersetting.h>

#include <stdio.h>
#include <string.h>

namespace {

// Legacy Windows power plans (well-known GUIDs from PowrProf docs):
//   Power saver          a1841308-3541-4fab-bc81-f71556f20b4a
//   Balanced             381b4222-f694-41f0-9685-ff5bb260df2e
//   Balanced (randomized)b9a9a03a-5b6d-40a4-8656-132d3c462975
//   High performance     8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
//   Ultimate performance e9a42b02-d5df-448d-aa00-03f14749eb61
const GUID kSaverGUID = {
  0xa1841308, 0x3541, 0x4fab,
  { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a }
};
const GUID kBalancedGUID = {
  0x381b4222, 0xf694, 0x41f0,
  { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e }
};
const GUID kBalancedRandomizedGUID = {
  0xb9a9a03a, 0x5b6d, 0x40a4,
  { 0x86, 0x56, 0x13, 0x2d, 0x3c, 0x46, 0x29, 0x75 }
};
const GUID kPerformanceGUID = {
  0x8c5e7fda, 0xe8bf, 0x4a96,
  { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c }
};
const GUID kUltimatePerformanceGUID = {
  0xe9a42b02, 0xd5df, 0x448d,
  { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 }
};

// Modern Windows 10 (1809+) / Windows 11 Power Overlay Schemes:
//   Best power efficiency (Better battery) 961cc777-2547-4f9d-8174-7d86181b8a7a
//   Balanced                               00000000-0000-0000-0000-000000000000
//   Better performance (Win10 4-notch)     3af9b8d9-7c97-431d-ad78-34a85fe439fb
//   Best performance                       ded574b5-45a0-4f42-8737-46345c09c238
const GUID kOverlaySaverGUID = {
  0x961cc777, 0x2547, 0x4f9d,
  { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a }
};
const GUID kOverlayBalancedGUID = {
  0x00000000, 0x0000, 0x0000,
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};
const GUID kOverlayBetterPerformanceGUID = {
  0x3af9b8d9, 0x7c97, 0x431d,
  { 0xad, 0x78, 0x34, 0xa8, 0x5f, 0xe4, 0x39, 0xfb }
};
const GUID kOverlayPerformanceGUID = {
  0xded574b5, 0x45a0, 0x4f42,
  { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 }
};

typedef DWORD (WINAPI *FnPowerGetOverlayScheme) (GUID *overlay_guid);
typedef DWORD (WINAPI *FnPowerSetActiveOverlayScheme) (const GUID *overlay_guid);

struct OverlayApi {
  FnPowerGetOverlayScheme get_effective = nullptr;
  FnPowerGetOverlayScheme get_actual = nullptr;
  FnPowerSetActiveOverlayScheme set_active = nullptr;
  bool supported = false;
};

template <typename Fn>
Fn
load_proc (HMODULE mod, const char *name)
{
  void *sym = reinterpret_cast<void *> (GetProcAddress (mod, name));
  return reinterpret_cast<Fn> (sym);
}

const OverlayApi &
overlay_api (void)
{
  static const OverlayApi api = [] () {
    OverlayApi out;
    HMODULE mod = GetModuleHandleW (L"powrprof.dll");
    if (mod == nullptr)
      mod = LoadLibraryW (L"powrprof.dll");
    if (mod == nullptr)
      return out;

    out.get_effective = load_proc<FnPowerGetOverlayScheme> (
        mod, "PowerGetEffectiveOverlayScheme");
    out.get_actual = load_proc<FnPowerGetOverlayScheme> (
        mod, "PowerGetActualOverlayScheme");
    out.set_active = load_proc<FnPowerSetActiveOverlayScheme> (
        mod, "PowerSetActiveOverlayScheme");

    if (out.set_active != nullptr &&
        (out.get_effective != nullptr || out.get_actual != nullptr)) {
      GUID probe = {};
      if ((out.get_effective != nullptr &&
           out.get_effective (&probe) == ERROR_SUCCESS) ||
          (out.get_actual != nullptr &&
           out.get_actual (&probe) == ERROR_SUCCESS)) {
        out.supported = true;
      }
    }
    return out;
  } ();
  return api;
}

GUID
legacy_guid_for_mode (WinomePowerMode mode)
{
  switch (mode) {
  case WINOME_POWER_SAVER:
    return kSaverGUID;
  case WINOME_POWER_PERFORMANCE:
    return kPerformanceGUID;
  default:
    return kBalancedGUID;
  }
}

GUID
overlay_guid_for_mode (WinomePowerMode mode)
{
  switch (mode) {
  case WINOME_POWER_SAVER:
    return kOverlaySaverGUID;
  case WINOME_POWER_PERFORMANCE:
    return kOverlayPerformanceGUID;
  default:
    return kOverlayBalancedGUID;
  }
}

bool
is_explicit_legacy_non_balanced (const GUID &guid)
{
  return IsEqualGUID (guid, kSaverGUID) ||
         IsEqualGUID (guid, kPerformanceGUID) ||
         IsEqualGUID (guid, kUltimatePerformanceGUID);
}

WinomePowerMode
mode_for_base_and_overlay (const GUID &base, const GUID *overlay)
{
  // Explicit legacy non-Balanced schemes override overlays (Windows Settings
  // disables the Power Mode dropdown while a legacy plan like High Performance
  // is active).
  if (IsEqualGUID (base, kSaverGUID))
    return WINOME_POWER_SAVER;
  if (IsEqualGUID (base, kPerformanceGUID) ||
      IsEqualGUID (base, kUltimatePerformanceGUID))
    return WINOME_POWER_PERFORMANCE;

  if (overlay != nullptr) {
    if (IsEqualGUID (*overlay, kOverlaySaverGUID))
      return WINOME_POWER_SAVER;
    if (IsEqualGUID (*overlay, kOverlayPerformanceGUID) ||
        IsEqualGUID (*overlay, kOverlayBetterPerformanceGUID))
      return WINOME_POWER_PERFORMANCE;
    if (IsEqualGUID (*overlay, kOverlayBalancedGUID) ||
        IsEqualGUID (*overlay, kBalancedGUID) ||
        IsEqualGUID (*overlay, kBalancedRandomizedGUID))
      return WINOME_POWER_BALANCED;
  }

  if (IsEqualGUID (base, kBalancedGUID) ||
      IsEqualGUID (base, kBalancedRandomizedGUID))
    return WINOME_POWER_BALANCED;

  // Unrecognized OEM/custom scheme with no recognized overlay: Balanced is
  // the honest fallback for the checkmark.
  return WINOME_POWER_BALANCED;
}

bool
parse_guid_string (const char *text, GUID *out)
{
  if (text == nullptr || *text == '\0' || out == nullptr)
    return false;
  if (*text == '{')
    ++text;

  unsigned long d1 = 0;
  unsigned int d2 = 0, d3 = 0;
  unsigned int b[8] = {};
  int matched = sscanf (
      text,
      "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
      &d1, &d2, &d3,
      &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
  if (matched != 11)
    return false;

  out->Data1 = static_cast<unsigned long> (d1);
  out->Data2 = static_cast<unsigned short> (d2);
  out->Data3 = static_cast<unsigned short> (d3);
  for (int i = 0; i < 8; ++i)
    out->Data4[i] = static_cast<unsigned char> (b[i]);
  return true;
}

// Read the active legacy base scheme.
gboolean
get_active_base_guid (GUID *out)
{
  GUID *active = NULL;
  if (PowerGetActiveScheme (NULL, &active) != ERROR_SUCCESS)
    return FALSE;
  if (active == NULL)
    return FALSE;
  *out = *active;
  LocalFree (active);
  return TRUE;
}

gboolean
get_active_overlay_guid (GUID *out)
{
  const OverlayApi &api = overlay_api ();
  if (!api.supported)
    return FALSE;
  if (api.get_effective != nullptr &&
      api.get_effective (out) == ERROR_SUCCESS) {
    return TRUE;
  }
  if (api.get_actual != nullptr &&
      api.get_actual (out) == ERROR_SUCCESS) {
    return TRUE;
  }
  return FALSE;
}

gboolean
legacy_scheme_exists (const GUID &target)
{
  DWORD index = 0;
  for (;;) {
    GUID found = {};
    DWORD size = sizeof (found);
    if (PowerEnumerate (NULL, NULL, NULL, ACCESS_SCHEME, index,
                        (UCHAR *) &found, &size) != ERROR_SUCCESS)
      return FALSE;
    if (size == sizeof (GUID) && IsEqualGUID (found, target))
      return TRUE;
    index++;
    if (index > 64)  // safety bound; a normal machine has < 10 schemes
      return FALSE;
  }
}

gboolean
ensure_balanced_base_scheme (void)
{
  GUID base = {};
  if (get_active_base_guid (&base) && !is_explicit_legacy_non_balanced (base))
    return TRUE;

  if (legacy_scheme_exists (kBalancedGUID)) {
    GUID bal = kBalancedGUID;
    return PowerSetActiveScheme (NULL, &bal) == ERROR_SUCCESS;
  }
  if (legacy_scheme_exists (kBalancedRandomizedGUID)) {
    GUID bal = kBalancedRandomizedGUID;
    return PowerSetActiveScheme (NULL, &bal) == ERROR_SUCCESS;
  }
  return FALSE;
}

}  // namespace

WinomePowerMode
winome_power_mode_from_guid_strings (const char *base_guid,
                                     const char *overlay_guid)
{
  GUID base = {};
  if (!parse_guid_string (base_guid, &base))
    return WINOME_POWER_MODE_COUNT;

  GUID overlay = {};
  const bool has_overlay = parse_guid_string (overlay_guid, &overlay);
  return mode_for_base_and_overlay (base, has_overlay ? &overlay : nullptr);
}

WinomePowerMode
winome_power_get_active (void)
{
  GUID base = {};
  const gboolean have_base = get_active_base_guid (&base);
  GUID overlay = {};
  const gboolean have_overlay = get_active_overlay_guid (&overlay);

  if (!have_base && !have_overlay)
    return WINOME_POWER_MODE_COUNT;
  if (!have_base)
    base = kBalancedGUID;

  return mode_for_base_and_overlay (base, have_overlay ? &overlay : nullptr);
}

gboolean
winome_power_set_mode (WinomePowerMode mode)
{
  if (mode < 0 || mode >= WINOME_POWER_MODE_COUNT)
    return FALSE;

  const OverlayApi &api = overlay_api ();
  if (api.supported && api.set_active != nullptr) {
    // If the user was previously on a legacy non-Balanced plan (e.g. Control
    // Panel High Performance), switch the base plan back to Balanced so the
    // Windows 10/11 power mode overlay actually takes effect.
    ensure_balanced_base_scheme ();

    GUID overlay = overlay_guid_for_mode (mode);
    if (api.set_active (&overlay) == ERROR_SUCCESS)
      return TRUE;

    // Some Windows builds expect the Balanced scheme GUID rather than
    // GUID_NULL when resetting the overlay to Balanced.
    if (mode == WINOME_POWER_BALANCED) {
      GUID bal = kBalancedGUID;
      if (api.set_active (&bal) == ERROR_SUCCESS)
        return TRUE;
    }
  }

  // Fallback to legacy power plans on older Windows or OEM-locked plans.
  GUID target = legacy_guid_for_mode (mode);
  if (PowerSetActiveScheme (NULL, &target) == ERROR_SUCCESS)
    return TRUE;
  if (mode == WINOME_POWER_PERFORMANCE &&
      legacy_scheme_exists (kUltimatePerformanceGUID)) {
    GUID ult = kUltimatePerformanceGUID;
    return PowerSetActiveScheme (NULL, &ult) == ERROR_SUCCESS;
  }
  if (mode == WINOME_POWER_BALANCED &&
      legacy_scheme_exists (kBalancedRandomizedGUID)) {
    GUID bal = kBalancedRandomizedGUID;
    return PowerSetActiveScheme (NULL, &bal) == ERROR_SUCCESS;
  }
  return FALSE;
}

gboolean
winome_power_mode_available (WinomePowerMode mode)
{
  if (mode < 0 || mode >= WINOME_POWER_MODE_COUNT)
    return FALSE;

  // Modern Windows with Power Overlay Schemes always supports all 3 modes
  // even when only the Balanced legacy plan is installed.
  if (overlay_api ().supported)
    return TRUE;

  if (legacy_scheme_exists (legacy_guid_for_mode (mode)))
    return TRUE;
  if (mode == WINOME_POWER_PERFORMANCE &&
      legacy_scheme_exists (kUltimatePerformanceGUID))
    return TRUE;
  if (mode == WINOME_POWER_BALANCED &&
      legacy_scheme_exists (kBalancedRandomizedGUID))
    return TRUE;
  return FALSE;
}

const char *
winome_power_mode_label (WinomePowerMode mode)
{
  gboolean zh = winome::is_zh () ? TRUE : FALSE;
  switch (mode) {
  case WINOME_POWER_SAVER:
    return zh ? "节能" : "Power Saver";
  case WINOME_POWER_BALANCED:
    return zh ? "均衡" : "Balanced";
  case WINOME_POWER_PERFORMANCE:
    return zh ? "性能" : "Performance";
  default:
    return "";
  }
}

const char *
winome_power_mode_icon (WinomePowerMode mode)
{
  switch (mode) {
  case WINOME_POWER_SAVER:
    return "power-profile-power-saver-symbolic";
  case WINOME_POWER_BALANCED:
    return "power-profile-balanced-symbolic";
  case WINOME_POWER_PERFORMANCE:
    return "power-profile-performance-symbolic";
  default:
    return "power-profile-balanced-symbolic";
  }
}