/*
 * Copyright (C) 2010 Frank Morgner
 *
 * This file is part of ifdnfc.
 *
 * ifdnfc is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * ifdnfc is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ifdnfc.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ifd-nfc.h"
#include "atr.h"

#ifdef HAVE_DEBUGLOG_H
#include <debuglog.h>
#else

#define LogXxd(priority, fmt, data1, data2) do { } while(0)

enum {
	PCSC_LOG_DEBUG = 0,
	PCSC_LOG_INFO,
	PCSC_LOG_ERROR,
	PCSC_LOG_CRITICAL
};

#ifdef HAVE_SYSLOG_H

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

void log_msg(const int priority, const char *fmt, ...)
{
	char debug_buffer[160]; /* up to 2 lines of 80 characters */
	va_list argptr;
	int syslog_level;

	switch(priority) {
		case PCSC_LOG_CRITICAL:
			syslog_level = LOG_CRIT;
			break;
		case PCSC_LOG_ERROR:
			syslog_level = LOG_ERR;
			break;
		case PCSC_LOG_INFO:
			syslog_level = LOG_INFO;
			break;
		default:
			syslog_level = LOG_DEBUG;
	}

	va_start(argptr, fmt);
	(void)vsnprintf(debug_buffer, sizeof debug_buffer, fmt, argptr);
	va_end(argptr);

	syslog(syslog_level, "%s", debug_buffer);
}
#define Log0(priority) log_msg(priority, "%s:%d:%s()", __FILE__, __LINE__, __FUNCTION__)
#define Log1(priority, fmt) log_msg(priority, "%s:%d:%s() " fmt, __FILE__, __LINE__, __FUNCTION__)
#define Log2(priority, fmt, data) log_msg(priority, "%s:%d:%s() " fmt, __FILE__, __LINE__, __FUNCTION__, data)
#define Log3(priority, fmt, data1, data2) log_msg(priority, "%s:%d:%s() " fmt, __FILE__, __LINE__, __FUNCTION__, data1, data2)
#define Log4(priority, fmt, data1, data2, data3) log_msg(priority, "%s:%d:%s() " fmt, __FILE__, __LINE__, __FUNCTION__, data1, data2, data3)
#define Log5(priority, fmt, data1, data2, data3, data4) log_msg(priority, "%s:%d:%s() " fmt, __FILE__, __LINE__, __FUNCTION__, data1, data2, data3, data4)
#define Log9(priority, fmt, data1, data2, data3, data4, data5, data6, data7, data8) log_msg(priority, "%s:%d:%s() " fmt, __FILE__, __LINE__, __FUNCTION__, data1, data2, data3, data4, data5, data6, data7, data8)

#else

#define Log0(priority) do { } while(0)
#define Log1(priority, fmt) do { } while(0)
#define Log2(priority, fmt, data) do { } while(0)
#define Log3(priority, fmt, data1, data2) do { } while(0)
#define Log4(priority, fmt, data1, data2, data3) do { } while(0)
#define Log5(priority, fmt, data1, data2, data3, data4) do { } while(0)
#define Log9(priority, fmt, data1, data2, data3, data4, data5, data6, data7, data8) do { } while(0)

#endif
#endif

#ifdef HAVE_IFDHANDLER_H
#include <ifdhandler.h>
#else
#include "my_ifdhandler.h"
#endif

#include <nfc/nfc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/*
 * This implementation was written based on information provided by the
 * following documents:
 *
 * From PC/SC specifications:
 * http://www.pcscworkgroup.com/specifications/specdownload.php
 *
 *  Interoperability Specification for ICCs and Personal Computer Systems
 *   Part 3. Requirements for PC-Connected Interface Devices
 *   PCSC_Part3 - v2.01.09
 *   http://www.pcscworkgroup.com/specifications/files/pcsc3_v2.01.09.pdf
 */

struct ifd_slot {
  bool present;
  bool initiated;
  nfc_target target;
  unsigned char atr[MAX_ATR_SIZE];
  size_t atr_len;
};

struct ifd_device {
  nfc_device *device;
  struct ifd_slot slot;
  bool connected;
  bool secure_element_as_card;
  int Lun;
};

nfc_context *context = NULL;

#define IFDNFC_MAX_DEVICES 10
static struct ifd_device ifd_devices[IFDNFC_MAX_DEVICES];
static bool ifdnfc_initialized = false;

static nfc_connstring ifd_connstring;

static const nfc_modulation supported_modulations[] = {
  { NMT_ISO14443A, NBR_106 },
};

static int lun2device_index(DWORD Lun)
{
  size_t i;
  // Find slot containing Lun
  for (i = 0; i < sizeof(ifd_devices); i++)
    if (ifd_devices[i].Lun == Lun) {
      return i;
    }
  // Slot not found
  return -1;
}

static void ifdnfc_disconnect(struct ifd_device *ifdnfc)
{
  if (ifdnfc->connected) {
    if (ifdnfc->slot.present) {
      if (nfc_initiator_deselect_target(ifdnfc->device) < 0) {
        Log3(PCSC_LOG_ERROR, "Could not disconnect from %s (%s).", str_nfc_modulation_type(ifdnfc->slot.target.nm.nmt), nfc_strerror(ifdnfc->device));
      } else {
        ifdnfc->slot.present = false;
      }
    }
    nfc_close(ifdnfc->device);
    ifdnfc->connected = false;
    ifdnfc->device = NULL;
    ifdnfc->Lun = -1;
  }
}

static bool ifdnfc_target_to_atr(struct ifd_device *ifdnfc)
{
  unsigned char atqb[12];
  ifdnfc->slot.atr_len = sizeof(ifdnfc->slot.atr);

  switch (ifdnfc->slot.target.nm.nmt) {
    case NMT_ISO14443A:
      /* libnfc already strips TL and CRC1/CRC2 */
      if (!get_atr(ATR_ISO14443A_106,
                   ifdnfc->slot.target.nti.nai.abtAts, ifdnfc->slot.target.nti.nai.szAtsLen,
                   (unsigned char *) ifdnfc->slot.atr, &(ifdnfc->slot.atr_len))) {
        Log1(PCSC_LOG_DEBUG, "get_atr: FAIL");
        ifdnfc->slot.atr_len = 0;
        return false;
      }
      Log1(PCSC_LOG_DEBUG, "get_atr: OK");
      break;
    case NMT_ISO14443B:
      // First ATQB byte always equal to 0x50
      atqb[0] = 0x50;

      // Store the PUPI (Pseudo-Unique PICC Identifier)
      memcpy(&atqb[1], ifdnfc->slot.target.nti.nbi.abtPupi, 4);

      // Store the Application Data
      memcpy(&atqb[5], ifdnfc->slot.target.nti.nbi.abtApplicationData, 4);

      // Store the Protocol Info
      memcpy(&atqb[9], ifdnfc->slot.target.nti.nbi.abtProtocolInfo, 3);
      if (!get_atr(ATR_ISO14443B_106, atqb, sizeof(atqb),
                   (unsigned char *) ifdnfc->slot.atr, &(ifdnfc->slot.atr_len)))
        ifdnfc->slot.atr_len = 0;
      return false;
      break;
    case NMT_ISO14443BI:
    case NMT_ISO14443B2CT:
    case NMT_ISO14443B2SR:
    case NMT_JEWEL:
    case NMT_FELICA:
    case NMT_DEP:
      /* for all other types: Empty ATR */
      Log1(PCSC_LOG_INFO, "Returning empty ATR for card without APDU support.");
      ifdnfc->slot.atr_len = 0;
      return true;
  }

  return true;
}

static bool ifdnfc_reselect_target(struct ifd_device *ifdnfc, bool warm)
{
  switch (ifdnfc->slot.target.nm.nmt) {
    case NMT_ISO14443A:
      if (nfc_device_set_property_bool(ifdnfc->device, NP_INFINITE_SELECT, false) < 0) {
        Log2(PCSC_LOG_ERROR, "Could not set infinite-select property (%s)", nfc_strerror(ifdnfc->device));
        ifdnfc->slot.present = false;
        return false;
      }
      nfc_target nt;
      // the UID might change when the field was lost. We don't reuse it for a cold reselection
      if (nfc_initiator_select_passive_target(ifdnfc->device, ifdnfc->slot.target.nm, warm ? ifdnfc->slot.target.nti.nai.abtUid : NULL, warm ? ifdnfc->slot.target.nti.nai.szUidLen : 0, &nt) < 1) {
        Log3(PCSC_LOG_DEBUG, "Could not select target %s. (%s)", str_nfc_modulation_type(ifdnfc->slot.target.nm.nmt), nfc_strerror(ifdnfc->device));
        ifdnfc->slot.present = false;
        return false;
      } else {
        if (!warm) {
          // for a warm reset compare the ATS
          if (ifdnfc->slot.target.nti.nai.szAtsLen == nt.nti.nai.szAtsLen
              && 0 == memcmp(ifdnfc->slot.target.nti.nai.abtAts, nt.nti.nai.abtAts, nt.nti.nai.szAtsLen)) {
            return true;
          } else {
            return false;
          }
        }
        return true;
      }
      break;
    case NMT_DEP:
    case NMT_FELICA:
    case NMT_ISO14443B2CT:
    case NMT_ISO14443B2SR:
    case NMT_ISO14443B:
    case NMT_ISO14443BI:
    case NMT_JEWEL:
    default:
      // TODO Implement me :)
      break;
  }
  return false;
}

static bool ifdnfc_se_is_available(struct ifd_device *ifdnfc)
{
  if (!ifdnfc->connected)
    return false;

  if (ifdnfc->slot.present && ifdnfc->slot.initiated)
    return true; // SE is considered as wired, so it is always available once detected as present

  if (nfc_initiator_init_secure_element(ifdnfc->device) < 0) {
    Log2(PCSC_LOG_ERROR, "Could not initialize secure element mode. (%s)", nfc_strerror(ifdnfc->device));
    ifdnfc->slot.present = false;
    return false;
  }
  // Let the reader only try once to find a tag
  if (nfc_device_set_property_bool(ifdnfc->device, NP_INFINITE_SELECT, false) < 0) {
    ifdnfc->slot.present = false;
    return false;
  }
  // Read the SAM's info
  const nfc_modulation nmSAM = {
    .nmt = NMT_ISO14443A,
    .nbr = NBR_106,
  };

  int res;
  if ((res = nfc_initiator_select_passive_target(ifdnfc->device, nmSAM, NULL, 0, &(ifdnfc->slot.target))) < 0) {
    Log2(PCSC_LOG_ERROR, "Could not select secure element. (%s)", nfc_strerror(ifdnfc->device));
    ifdnfc->slot.present = false;
    return false;
  } else if (res == 0) {
    Log2(PCSC_LOG_ERROR, "No secure element available. (%s)", nfc_strerror(ifdnfc->device));
    ifdnfc->slot.present = false;
    return false;
  } // else
  Log1(PCSC_LOG_DEBUG, "Secure element selected.");
  ifdnfc_target_to_atr(ifdnfc);
  ifdnfc->slot.present = true;

  return true;
}

static bool ifdnfc_target_is_available(struct ifd_device *ifdnfc)
{
  if (!ifdnfc->connected)
    return false;

  if (ifdnfc->slot.present) {
    if (ifdnfc->slot.initiated) {
      // Target is active and just need a ping-like command (handled by libnfc)
      if (nfc_initiator_target_is_present(ifdnfc->device, &ifdnfc->slot.target) < 0) {
        Log3(PCSC_LOG_INFO, "Connection lost with %s. (%s)", str_nfc_modulation_type(ifdnfc->slot.target.nm.nmt), nfc_strerror(ifdnfc->device));
        ifdnfc->slot.present = false;
        return false;
      }
      return true;
    } else {
      // Target is not initiated and need to be wakeup
      if (nfc_initiator_init(ifdnfc->device) < 0) {
        Log2(PCSC_LOG_ERROR, "Could not initialize initiator mode. (%s)", nfc_strerror(ifdnfc->device));
        ifdnfc->slot.present = false;
        return false;
      }
      // To prevent from multiple init
      ifdnfc->slot.initiated = true;
      if (!ifdnfc_reselect_target(ifdnfc, false)) {
        Log3(PCSC_LOG_INFO, "Connection lost with %s. (%s)", str_nfc_modulation_type(ifdnfc->slot.target.nm.nmt), nfc_strerror(ifdnfc->device));
        ifdnfc->slot.present = false;
        return false;
      }
      if (nfc_initiator_deselect_target(ifdnfc->device) < 0) {
        Log2(PCSC_LOG_ERROR, "Could not deselect target. (%s)", nfc_strerror(ifdnfc->device));
      }
      return true;
    }
  } // else

  // ifdnfc->slot not initialised means the field is not active, so when no target
  // is available ifdnfc needs to generated a field
  if (!ifdnfc->slot.initiated) {
    if (nfc_initiator_init(ifdnfc->device) < 0) {
      Log2(PCSC_LOG_ERROR, "Could not init NFC device in initiator mode (%s).", nfc_strerror(ifdnfc->device));
      return false;
    }
    // To prevent from multiple init
    ifdnfc->slot.initiated = true;
  }

  // find new connection
  size_t i;
  for (i = 0; i < (sizeof(supported_modulations) / sizeof(nfc_modulation)); i++) {
    if (nfc_initiator_list_passive_targets(ifdnfc->device, supported_modulations[i], &(ifdnfc->slot.target), 1) == 1) {
      ifdnfc_target_to_atr(ifdnfc);
      ifdnfc->slot.present = true;
      // XXX Should it be on or off after target selection ?
      ifdnfc->slot.initiated = true;
      Log2(PCSC_LOG_INFO, "Connected to %s.", str_nfc_modulation_type(ifdnfc->slot.target.nm.nmt));
      return true;
    }
  }
  Log1(PCSC_LOG_DEBUG, "Could not find any NFC targets.");
  return false;
}

/*
 * List of Defined Functions Available to IFD_Handler 3.0
 */
RESPONSECODE
IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
  (void) Lun;
  int device_index, i;
  if (! ifdnfc_initialized) {
    Log1(PCSC_LOG_DEBUG, "Driver initialization");
    for (i = 0; i < IFDNFC_MAX_DEVICES; i++)
      ifd_devices[i].Lun = -1;
    nfc_init(&context);
    if (context == NULL) {
      Log1(PCSC_LOG_ERROR, "Unable to init libnfc (malloc)");
      return IFD_COMMUNICATION_ERROR;
    }
    ifdnfc_initialized = true;
    // First slot is free
    device_index = 0;
  } else {
    // Find a free slot
    for (i = 0; i < IFDNFC_MAX_DEVICES; i++)
      if (ifd_devices[i].Lun == -1) {
        device_index = i;
        break;
      } else if (i == IFDNFC_MAX_DEVICES - 1) {
        // No free slot
        return IFD_COMMUNICATION_ERROR;
      }
  }

  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  ifdnfc->device = NULL;
  ifdnfc->connected = false;
  ifdnfc->slot.present = false;

  // USB DeviceNames can be immediately handled, e.g.:
  // usb:1fd3/0608:libudev:0:/dev/bus/usb/002/079
  // => connstring usb:002:079
  int n = strlen(DeviceName) + 1;
  char *vidpid      = malloc(n);
  char *hpdriver    = malloc(n);
  char *ifn         = malloc(n);
  char *devpath     = malloc(n);
  char *dirname     = malloc(n);
  char *filename    = malloc(n);

  int res = sscanf(DeviceName, "usb:%4[^:]:%4[^:]:%32[^:]:%32[^:]", vidpid, hpdriver, ifn, devpath);
  if (res == 4) {
    res = sscanf(devpath, "/dev/bus/usb/%3[^/]/%3[^/]", dirname, filename);
    if (res == 2) {
      strcpy(ifd_connstring, "usb:xxx:xxx");
      memcpy(ifd_connstring + 4, dirname, 3);
      memcpy(ifd_connstring + 8, filename, 3);
      ifdnfc->device = nfc_open(context, ifd_connstring);
      ifdnfc->connected = (ifdnfc->device) ? true : false;
    }

  ifdnfc->Lun = Lun;

  free(vidpid);
  free(hpdriver);
  free(ifn);
  free(devpath);
  free(dirname);
  free(filename);

  if (!ifdnfc->connected)
    Log2(PCSC_LOG_DEBUG, "\"DEVICENAME    %s\" is not used.", DeviceName);
  else
    Log2(PCSC_LOG_DEBUG, "\"DEVICENAME    %s\" is used by libnfc.", DeviceName);
  Log1(PCSC_LOG_INFO, "IFD-handler for NFC devices is ready.");
  return IFD_SUCCESS;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
  char str[16];
  snprintf(str, sizeof str, "/dev/pcsc/%lu", (unsigned long) Channel);

  return IFDHCreateChannelByName(Lun, str);
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHCloseChannel(DWORD Lun)
{
  (void) Lun;
  int device_index = lun2device_index(Lun);
  if (device_index < 0)
    return IFD_COMMUNICATION_ERROR;
  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  ifdnfc_disconnect(ifdnfc);

  // Check if there are still devices used
  int i;
  for (i = 0; i < IFDNFC_MAX_DEVICES; i++)
    if (ifd_devices[i].Lun != -1)
      // yes so don't exit libnfc
      return IFD_SUCCESS;
  // No more device, we can shutdown libnfc
  nfc_exit(context);
  context = NULL;
  return IFD_SUCCESS;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length, PUCHAR Value)
{
  Log4(PCSC_LOG_DEBUG, "IFDHGetCapabilities(DWORD Lun (%08x), DWORD Tag (%08x), PDWORD Length (%lu), PUCHAR Value)", Lun, Tag, *Length);
  (void) Lun;
  int device_index = lun2device_index(Lun);
  if (device_index < 0)
    return IFD_COMMUNICATION_ERROR;
  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  if (!Length || !Value)
    return IFD_COMMUNICATION_ERROR;

  switch (Tag) {
    case TAG_IFD_ATR:
#ifdef SCARD_ATTR_ATR_STRING
    case SCARD_ATTR_ATR_STRING:
#endif
      if (!ifdnfc->connected || !ifdnfc->slot.present)
        return(IFD_COMMUNICATION_ERROR);
      if (*Length < ifdnfc->slot.atr_len)
        return IFD_COMMUNICATION_ERROR;

      memcpy(Value, ifdnfc->slot.atr, ifdnfc->slot.atr_len);
      *Length = ifdnfc->slot.atr_len;
      break;

    case TAG_IFD_SIMULTANEOUS_ACCESS:
      if (*Length >= 1) {
        *Length = 1;
        *Value = 10;
      } else
        return IFD_ERROR_INSUFFICIENT_BUFFER;
      break;
    case TAG_IFD_THREAD_SAFE:
      if (*Length < 1)
        return IFD_COMMUNICATION_ERROR;
      *Value  = 0;
      *Length = 1;
      break;
    case TAG_IFD_SLOTS_NUMBER:
      if (*Length < 1)
        return IFD_COMMUNICATION_ERROR;
      *Value  = 1;
      *Length = 1;
      break;
#if defined(HAVE_DECL_TAG_IFD_STOP_POLLING_THREAD) && HAVE_DECL_TAG_IFD_POLLING_THREAD
    case TAG_IFD_STOP_POLLING_THREAD:
#endif
#if defined(HAVE_DECL_TAG_IFD_POLLING_THREAD_WITH_TIMEOUT) && HAVE_DECL_TAG_IFD_POLLING_THREAD_WITH_TIMEOUT
	case TAG_IFD_POLLING_THREAD_WITH_TIMEOUT:
#endif
	case TAG_IFD_POLLING_THREAD_KILLABLE:
	  return IFD_ERROR_NOT_SUPPORTED;
    default:
      Log3(PCSC_LOG_ERROR, "Tag %08x (%lu) not supported", Tag, (unsigned long) Tag);
      return IFD_ERROR_TAG;
  }

  return IFD_SUCCESS;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length, PUCHAR Value)
{
  (void) Lun;
  (void) Tag;
  (void) Length;
  (void) Value;
  return IFD_ERROR_VALUE_READ_ONLY;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol, UCHAR Flags, UCHAR PTS1,
                          UCHAR PTS2, UCHAR PTS3)
{
  (void) Lun;
  (void) Flags;
  (void) PTS1;
  (void) PTS2;
  (void) PTS3;
  if (Protocol != SCARD_PROTOCOL_T1)
    return IFD_PROTOCOL_NOT_SUPPORTED;

  return IFD_SUCCESS;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr, PDWORD AtrLength)
{
  (void) Lun;
  int device_index = lun2device_index(Lun);
  if (device_index < 0)
    return IFD_COMMUNICATION_ERROR;
  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  if (!Atr || !AtrLength)
    return IFD_COMMUNICATION_ERROR;

  if (!ifdnfc->connected)
    return(IFD_COMMUNICATION_ERROR);

  switch (Action) {
    case IFD_POWER_DOWN:
      // IFD_POWER_DOWN: Power down the card (Atr and AtrLength should be zeroed)
// Warning, this means putting card to HALT, not reader to IDLE or RFoff
// FixMe: Disabling entirely power-down for now because of spurious RFoff/RFon during operation
// Test case: LoGO + JCOP with Vonjeek applet + mrpkey.py (RFIDIOt)
/*
      if (nfc_idle(ifdnfc->device) < 0) {
        Log2(PCSC_LOG_ERROR, "Could not idle NFC device (%s).", nfc_strerror(ifdnfc->device));
        return IFD_ERROR_POWER_ACTION;
      }
*/
      *AtrLength = 0;
      return IFD_SUCCESS;
      break;
    case IFD_RESET:
      // IFD_RESET: Perform a warm reset of the card (no power down). If the card is not powered then power up the card (store and return Atr and AtrLength)
      if (ifdnfc->slot.present) {
        ifdnfc->slot.present = false;
        if (nfc_initiator_deselect_target(ifdnfc->device) < 0) {
          Log2(PCSC_LOG_ERROR, "Could not deselect NFC target (%s).", nfc_strerror(ifdnfc->device));
          *AtrLength = 0;
          return IFD_ERROR_POWER_ACTION;
        }
        if (!ifdnfc_reselect_target(ifdnfc, true)) {
          *AtrLength = 0;
          return IFD_ERROR_POWER_ACTION;
        }
        // In contactless, ATR on warm reset is always same as on cold reset
        if (*AtrLength < ifdnfc->slot.atr_len)
          return IFD_COMMUNICATION_ERROR;
        memcpy(Atr, ifdnfc->slot.atr, ifdnfc->slot.atr_len);
        // memset(Atr + ifdnfc->slot.atr_len, 0, *AtrLength - ifd_slot.atr_len);
        *AtrLength = ifdnfc->slot.atr_len;
        return IFD_SUCCESS;
      }
      break;
    case IFD_POWER_UP:
      // IFD_POWER_UP: Power up the card (store and return Atr and AtrLength)
      if (((ifdnfc->secure_element_as_card) && (ifdnfc_se_is_available(ifdnfc))) || ifdnfc_target_is_available(ifdnfc)) {
        if (*AtrLength < ifdnfc->slot.atr_len)
          return IFD_COMMUNICATION_ERROR;
        memcpy(Atr, ifdnfc->slot.atr, ifdnfc->slot.atr_len);
        // memset(Atr + ifdnfc->slot.atr_len, 0, *AtrLength - ifd_slot.atr_len);
        *AtrLength = ifdnfc->slot.atr_len;
      } else {
        *AtrLength = 0;
        return IFD_COMMUNICATION_ERROR;
      }
      break;
    default:
      Log2(PCSC_LOG_ERROR, "Action %lu not supported", (unsigned long) Action);
      return IFD_NOT_SUPPORTED;
  }

  return IFD_SUCCESS;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci, PUCHAR TxBuffer, DWORD
                  TxLength, PUCHAR RxBuffer, PDWORD RxLength, PSCARD_IO_HEADER RecvPci)
{
  (void) Lun;
  int device_index = lun2device_index(Lun);
  if (device_index < 0)
    return IFD_COMMUNICATION_ERROR;
  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  (void) SendPci;
  if (!RxLength || !RecvPci)
    return IFD_COMMUNICATION_ERROR;

  if (!ifdnfc->connected || !ifdnfc->slot.present) {
    *RxLength = 0;
    return IFD_ICC_NOT_PRESENT;
  }

  if ((TxBuffer[0] == 0xFF) && (TxBuffer[1] == 0xCA)) {
    // Get Data
    LogXxd(PCSC_LOG_INFO, "Intercepting GetData\n", TxBuffer, TxLength);
    size_t Le = TxBuffer[4];
    int RxOff = 0;
    uint8_t * Data;
    size_t DataLength;
    RecvPci->Protocol = 1; // needed??
    if (TxLength != 5) {
      // Wrong length
      RxBuffer[RxOff++] = 0x67;
      RxBuffer[RxOff++] = 0x00;
      *RxLength = RxOff;
      return IFD_SUCCESS;
    }
    switch (TxBuffer[2]) {
      case 0x00: // Get UID
        Data = ifdnfc->slot.target.nti.nai.abtUid;
        DataLength = ifdnfc->slot.target.nti.nai.szUidLen;
        break;
      case 0x01: // Get ATS hist bytes

        if (ifdnfc->slot.target.nm.nmt == NMT_ISO14443A) {
          Data = ifdnfc->slot.target.nti.nai.abtAts;
          DataLength = ifdnfc->slot.target.nti.nai.szAtsLen;
          if (DataLength) {
            size_t idx = 1;
            /* Bits 5 to 7 tell if TA1/TB1/TC1 are available */
            if (Data[0] & 0x10) idx++; // TA
            if (Data[0] & 0x20) idx++; // TB
            if (Data[0] & 0x40) idx++; // TC
            if (DataLength > idx) {
              DataLength -= idx;
              Data += idx;
            } else {
              DataLength = 0;
            }
          }
          break;
        } // else:
      default:
        // Function not supported
        RxBuffer[RxOff++] = 0x6A;
        RxBuffer[RxOff++] = 0x81;
        *RxLength = RxOff;
        return IFD_SUCCESS;
    }
    if (Le == 0) Le = DataLength;
    if (Le < DataLength) {
      // Wrong length
      RxBuffer[RxOff++] = 0x6C;
      RxBuffer[RxOff++] = DataLength;
      *RxLength = RxOff;
      return IFD_SUCCESS;
    }
    RxOff = DataLength;
    memcpy(RxBuffer, Data, RxOff);
    if (Le > RxOff) {
      // End of data reached before Le bytes
      for (;RxOff<Le;) RxBuffer[RxOff++] = 0;
      RxBuffer[RxOff++] = 0x62;
      RxBuffer[RxOff++] = 0x82;
    } else {
      RxBuffer[RxOff++] = 0x90;
      RxBuffer[RxOff++] = 0x00;
    }
    *RxLength = RxOff;
    return IFD_SUCCESS;
  }
  LogXxd(PCSC_LOG_INFO, "Sending to NFC target\n", TxBuffer, TxLength);

  size_t tl = TxLength, rl = *RxLength;
  int res;
  // timeout pushed to 5000ms, cf FWTmax in ISO14443-4
  if ((res = nfc_initiator_transceive_bytes(ifdnfc->device, TxBuffer, tl,
                                            RxBuffer, rl, 5000)) < 0) {
    Log2(PCSC_LOG_ERROR, "Could not transceive data (%s).",
         nfc_strerror(ifdnfc->device));
    *RxLength = 0;
    return(IFD_COMMUNICATION_ERROR);
  }

  *RxLength = res;
  RecvPci->Protocol = 1;

  LogXxd(PCSC_LOG_INFO, "Received from NFC target\n", RxBuffer, *RxLength);

  return IFD_SUCCESS;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHICCPresence(DWORD Lun)
{
  (void) Lun;
  int device_index = lun2device_index(Lun);
  if (device_index < 0)
    return IFD_COMMUNICATION_ERROR;
  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  if (!ifdnfc->connected)
    return IFD_ICC_NOT_PRESENT;
  if (ifdnfc->secure_element_as_card)
    return ifdnfc->slot.present ? IFD_SUCCESS : IFD_ICC_NOT_PRESENT; // If available once, available forever :)
  return ifdnfc_target_is_available(ifdnfc) ? IFD_SUCCESS : IFD_ICC_NOT_PRESENT;
}

RESPONSECODE
// cppcheck-suppress unusedFunction
IFDHControl(DWORD Lun, DWORD dwControlCode, PUCHAR TxBuffer, DWORD TxLength,
            PUCHAR RxBuffer, DWORD RxLength, LPDWORD pdwBytesReturned)
{
  (void) Lun;
  int device_index = lun2device_index(Lun);
  if (device_index < 0)
    return IFD_COMMUNICATION_ERROR;
  struct ifd_device *ifdnfc = &ifd_devices[device_index];
  if (pdwBytesReturned)
    *pdwBytesReturned = 0;

  switch (dwControlCode) {
    case IFDNFC_CTRL_ACTIVE:
      if (TxLength < 1 || !TxBuffer || RxLength < 1 || !RxBuffer)
        return IFD_COMMUNICATION_ERROR;

      switch (*TxBuffer) {
        case IFDNFC_SET_ACTIVE:
        case IFDNFC_SET_ACTIVE_SE: {
          uint16_t u16ConnstringLength;
          if (TxLength < (1 + sizeof(u16ConnstringLength)))
            return IFD_COMMUNICATION_ERROR;
          memcpy(&u16ConnstringLength, TxBuffer + 1, sizeof(u16ConnstringLength));
          if ((TxLength - (1 + sizeof(u16ConnstringLength))) != u16ConnstringLength)
            return IFD_COMMUNICATION_ERROR;
          memcpy(ifd_connstring, TxBuffer + (1 + sizeof(u16ConnstringLength)), u16ConnstringLength);
          ifdnfc->device = nfc_open(context, ifd_connstring);
          ifdnfc->connected = (ifdnfc->device) ? true : false;
          ifdnfc->Lun = Lun;
          ifdnfc->secure_element_as_card = (TxBuffer[0] == IFDNFC_SET_ACTIVE_SE);
        }
        break;
        case IFDNFC_SET_INACTIVE:
          ifdnfc_disconnect(ifdnfc);
          break;
        case IFDNFC_GET_STATUS:
          break;
        default:
          Log4(PCSC_LOG_ERROR, "Value for active request "
               "must be one of %lu %lu %lu.",
               (unsigned long) IFDNFC_SET_ACTIVE,
               (unsigned long) IFDNFC_SET_INACTIVE,
               (unsigned long) IFDNFC_GET_STATUS);
          return IFD_COMMUNICATION_ERROR;
      }

      if ((ifdnfc->connected) && ((!ifdnfc->secure_element_as_card) || ifdnfc_se_is_available(ifdnfc))) {
        Log1(PCSC_LOG_INFO, "IFD-handler for libnfc is active.");
        RxBuffer[0] = IFDNFC_IS_ACTIVE;
        const uint16_t u16ConnstringLength = strlen(ifd_connstring) + 1;
        memcpy(RxBuffer + 1, &u16ConnstringLength, sizeof(u16ConnstringLength));
        memcpy(RxBuffer + 1 + sizeof(u16ConnstringLength), ifd_connstring, u16ConnstringLength);
        if (pdwBytesReturned)
          *pdwBytesReturned = 1 + sizeof(u16ConnstringLength) + u16ConnstringLength;
      } else {
        Log1(PCSC_LOG_INFO, "IFD-handler for libnfc is inactive.");
        if (pdwBytesReturned)
          *pdwBytesReturned = 1;
        *RxBuffer = IFDNFC_IS_INACTIVE;
      }
      break;
    default:
      return IFD_ERROR_NOT_SUPPORTED;
  }

  return IFD_SUCCESS;
}
