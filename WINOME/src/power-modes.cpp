// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Windows power scheme switching (PowrProf).

#include "power-modes.h"
#include "system-status.h"

#include <windows.h>
#include <powrprof.h>
#include <powersetting.h>
#include <string.h>

namespace {

// The stock Windows power plans (well-known GUIDs from the PowrProf docs):
//   Power saver     a1841308-3541-4fab-bc81-f71556f20b4a
//   Balanced        381b4222-f694-41f0-9685-ff5bb260df2e
//   High performance 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
//
// Windows 11 ships Balanced under an additional randomized GUID
// (b9a9a03a-...) when the "use randomized" policy is on, so a plan we cannot
// name is reported as Balanced rather than as an unknown mode.
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

GUID
guid_for_mode (WinomePowerMode mode)
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

WinomePowerMode
mode_for_guid (const GUID &guid)
{
  if (IsEqualGUID (guid, kSaverGUID))
    return WINOME_POWER_SAVER;
  if (IsEqualGUID (guid, kPerformanceGUID))
    return WINOME_POWER_PERFORMANCE;
  if (IsEqualGUID (guid, kBalancedGUID) ||
      IsEqualGUID (guid, kBalancedRandomizedGUID))
    return WINOME_POWER_BALANCED;
  // An unrecognized scheme is most likely a vendor/OEM plan; Balanced is the
  // honest fallback for the checkmark.
  return WINOME_POWER_BALANCED;
}

// Read the active scheme. PowerGetActiveScheme takes a GUID** and fills the
// pointer for us, so query the size first and copy out of it.
gboolean
get_active_guid (GUID *out)
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

}  // namespace

WinomePowerMode
winome_power_get_active (void)
{
  GUID active;
  if (!get_active_guid (&active))
    return WINOME_POWER_MODE_COUNT;
  return mode_for_guid (active);
}

gboolean
winome_power_set_mode (WinomePowerMode mode)
{
  if (mode < 0 || mode >= WINOME_POWER_MODE_COUNT)
    return FALSE;
  GUID target = guid_for_mode (mode);
  return PowerSetActiveScheme (NULL, &target) == ERROR_SUCCESS;
}

gboolean
winome_power_mode_available (WinomePowerMode mode)
{
  if (mode < 0 || mode >= WINOME_POWER_MODE_COUNT)
    return FALSE;
  GUID target = guid_for_mode (mode);

  // Enumerate the installed schemes; a GUID that is not present cannot be
  // activated (PowerSetActiveScheme would fail silently).
  DWORD index = 0;
  for (;;) {
    DWORD size = 0;
    if (PowerEnumerate (NULL, &target, NULL, ACCESS_SCHEME, index,
                        NULL, &size) != ERROR_SUCCESS)
      return FALSE;
    if (size != sizeof (GUID))
      return FALSE;
    GUID found;
    if (PowerEnumerate (NULL, &target, NULL, ACCESS_SCHEME, index,
                        (UCHAR *) &found, &size) != ERROR_SUCCESS)
      return FALSE;
    if (IsEqualGUID (found, target))
      return TRUE;
    index++;
    if (index > 64)  // safety bound; a normal machine has < 10 schemes
      return FALSE;
  }
}

const char *
winome_power_mode_label (WinomePowerMode mode)
{
  const char *lang = winome::get_system_language ();
  gboolean zh = (strcmp (lang, "zh") == 0);
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
    return "power-profile-saver-symbolic";
  case WINOME_POWER_BALANCED:
    return "power-profile-balanced-symbolic";
  case WINOME_POWER_PERFORMANCE:
    return "power-profile-performance-symbolic";
  default:
    return "power-profile-balanced-symbolic";
  }
}