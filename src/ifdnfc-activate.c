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
#include "ifd-nfc.h"
#include <pcsclite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <winscard.h>
#include <nfc/nfc.h>
#include <strings.h>
#include <errno.h>

#define MAX_DEVICE_COUNT 16

#ifdef __APPLE__
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint8_t BYTE;
#endif

char *mode_to_str(int mode) {
  switch(mode) {
    case IFDNFC_SET_INACTIVE:
        return "Inactive";
    case IFDNFC_SET_ACTIVE:
        return "Active";
    case IFDNFC_SET_ACTIVE_SE:
        return "Active Secure Element";
    default:
        return "Undefined";
  }
}

int get_connstring(nfc_connstring *pConnstring) {
    
    (*pConnstring)[0] = 0;

    // Initialize libnfc
    nfc_context *context;
    nfc_init(&context);
    if (context == NULL) {
      fprintf(stderr, "Unable to init libnfc: %s\n", strerror(errno));
      return -1;
    }
    
    // List devices    
    nfc_connstring connstrings[MAX_DEVICE_COUNT];
    size_t szDeviceFound = nfc_list_devices(context, connstrings, MAX_DEVICE_COUNT);

    int connstring_index = -1, selection;
    switch (szDeviceFound) {
      case 0:
        fprintf(stderr, "Unable to activate ifdnfc: no NFC device found.\n");
        break;
      case 1:
        // Only one NFC device available, so auto-select it!
        connstring_index = 0;
        break;
      default:
        // More than one available NFC devices, propose a shell menu:
        printf("%d NFC devices found, please select one:\n", (int)szDeviceFound);
        for (size_t i = 0; i < szDeviceFound; i++) {
          nfc_device *pnd = nfc_open(context, connstrings[i]);
          if (pnd != NULL) {
            printf("[%d] %s\t  (%s)\n", (int)i, nfc_device_get_name(pnd), nfc_device_get_connstring(pnd));
            nfc_close(pnd);
          } else {
            fprintf(stderr, "nfc_open failed for %s.\n", connstrings[i]);
          }
        }

        printf(">> ");
        // Take user's choice
        if (1 != scanf("%2d", &selection)) {
          fprintf(stderr, "Value must an integer.\n");
          break;
        }
        if ((selection < 0) || (selection >= (int)szDeviceFound)) {
          fprintf(stderr, "Invalid index selection.\n");
          break;
        }
        connstring_index = selection;
        break;
    }
    nfc_exit(context);
    if(connstring_index > -1) {
      strncpy(*pConnstring, connstrings[connstring_index], sizeof(nfc_connstring));
      (*pConnstring)[sizeof(nfc_connstring)-1] = 0;
      return 0;
    }
    return 1;
}

int print_status(char *reader, IFDNFC_CONTROL_RESP *rxmsg, int rxlen) {
  if(rxlen != sizeof(IFDNFC_CONTROL_RESP)) { 
    printf("Reader '%s', PCSC Control Error (while getting status): %d length response from PCSC, expected %d.\n", 
      reader, rxlen, sizeof(rxmsg));
    return -1;
  }
  printf("Reader '%s', mode='%s', connected='%s', se='%s', connstring='%s'.\n", 
    reader, mode_to_str(rxmsg->mode), 
    (rxmsg->connected ? "Yes" : "No"), 
    (rxmsg->se_avail ? "Yes" : "No"), 
    rxmsg->connstring);
  return 0;
}

int
main(int argc, char *argv[])
{
  LONG rv;
  SCARDCONTEXT hContext;
  SCARDHANDLE hCard;
  char *reader;
  DWORD dwActiveProtocol, rxlen, dwReaders;
  char* mszReaders = NULL;
  char *devicename_prefix = IFDNFC_READER_NAME;

  IFDNFC_CONTROL_REQ txmsg;
  IFDNFC_CONTROL_RESP rxmsg;

  nfc_connstring connstring;

  int command = -1;

  switch(argc) {
    case 1:
      command = IFDNFC_SET_ACTIVE;
      break;
    case 2:
    case 3:
      if(!strcmp(argv[1], "yes"))
        command = IFDNFC_SET_ACTIVE;
      else 
        if(!strcmp(argv[1], "no"))
          command = IFDNFC_SET_INACTIVE;
        else 
          if(!strcmp(argv[1], "se"))
            command = IFDNFC_SET_ACTIVE_SE;
          else 
            if(!strcmp(argv[1], "status"))
              command = IFDNFC_GET_STATUS;
      if(argc == 3)
        devicename_prefix = argv[2];
      break;
    default:
      break;
  }

  if(command < 0) {
    printf("Usage: %s [yes|no|status] [nameprefix]\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  
  if((rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &hContext)) < 0) {
    printf("SCardEstablishContext Error: %s\n", pcsc_stringify_error(rv));
    exit(EXIT_FAILURE);
  }

  dwReaders = 0;
  // Ask how many bytes readers list take
  if((rv = SCardListReaders(hContext, NULL, NULL, &dwReaders)) >= 0) {
    // Then allocate and fill mszReaders
    mszReaders = malloc(dwReaders);
    rv = SCardListReaders(hContext, NULL, mszReaders, &dwReaders);
  }
  if(rv < 0) {
    printf("SCardListReaders Error: %s\n", pcsc_stringify_error(rv));
    free(mszReaders);
    exit(EXIT_FAILURE);
  }

  int l, foundcount = 0, prefixlen = strlen(devicename_prefix);
  for(reader = mszReaders; dwReaders > 0; l = strlen(reader) + 1, dwReaders -= l, reader += l) {
    // If reader FRIENDLYNAME does not start with "IFD-NFC" (the default) or specified prefix, we skip it.
    printf("'%s' '%s' %d\n", devicename_prefix, reader, prefixlen);
    if(strncmp(devicename_prefix, reader, prefixlen) != 0)
      continue;

    foundcount++;

    if((rv = SCardConnect(hContext, reader, SCARD_SHARE_DIRECT, 0, &hCard, &dwActiveProtocol)) < 0) {
      printf("Reader '%s', SCardConnect Error: %s\n", reader, pcsc_stringify_error(rv));
      continue;
    }
        
    switch(command) {
      case IFDNFC_SET_ACTIVE:
      case IFDNFC_SET_ACTIVE_SE:
        if(!strcmp(devicename_prefix, IFDNFC_READER_NAME)) {
          // Backward compatibility, if friendlyname is just "IFD-NFC" then we assume that there is one and only one
          // device that we are accessing through ifdnfc, and that DEVICENAME is missing or not meaningful, eg:
          //   FRIENDLYNAME "IFD-NFC"
          memset(&txmsg, 0, sizeof(txmsg));
          txmsg.command = IFDNFC_SET_INACTIVE;
          if((rv = SCardControl(hCard, IFDNFC_CTRL_ACTIVE, &txmsg, sizeof(txmsg), &rxmsg, sizeof(rxmsg), &rxlen)) < 0) {
            printf("Reader '%s', SCardControl Error (Setting Inactive): %s\n", reader, pcsc_stringify_error(rv));
            break;
          }
          if(get_connstring(&connstring) != 0) {
            printf("Reader '%s', did not get NFC connect string so can't activate.\n", reader);
            break;
          }
          printf("Reader '%s', activating ifdnfc with '%s'.\n", reader, connstring);
        } else {
          // In the new, multiple-device capable method, FRIENLYNAME should be in the form IFD-NFC-unique,
          // and DEVICENAME must be the libnfc connstring, eg, /etc/readers.d/reader_1.conf:
          //   FRIENDLYNAME "IFD-NFC-NXP-PN532.1"
          //   DEVICENAME pn532_uart:/dev/ttyS0
          //   LIBPATH /usr/lib/pcsc/drivers/ifdnfc.bundle/Contents/Linux/libifdnfc.so.0.1.4
          // Since pcscd passes DEVICENAME to ifdnfc, we don't need to know it here.
          printf("Reader '%s', activating ifdnfc using nfc_connstring in pcscd DEVICENAME.\n", reader);
          connstring[0] = 0;
        }
        memset(&txmsg, 0, sizeof(txmsg));
        txmsg.command = command;
        strncpy(txmsg.connstring, connstring, sizeof(txmsg.connstring));
        txmsg.connstring[sizeof(txmsg.connstring)-1] = 0;
        // pbSendBuf = { IFDNFC_SET_ACTIVE (1 byte), length (2 bytes), nfc_connstring (lenght bytes)}
        if((rv = SCardControl(hCard, IFDNFC_CTRL_ACTIVE, &txmsg, sizeof(txmsg), &rxmsg, sizeof(rxmsg), &rxlen)) < 0) {
          printf("Reader '%s', SCardControl Error (while setting mode %d): %s\n", reader, command, pcsc_stringify_error(rv));
          break;
        }
        print_status(reader, &rxmsg, rxlen);
        break;
      case IFDNFC_SET_INACTIVE:
        memset(&txmsg, 0, sizeof(txmsg));
        txmsg.command = command;
        if((rv = SCardControl(hCard, IFDNFC_CTRL_ACTIVE, &txmsg, sizeof(txmsg), &rxmsg, sizeof(rxmsg), &rxlen)) < 0) {
          printf("Reader '%s', SCardControl Error (while setting Inactive): %s\n", reader, pcsc_stringify_error(rv));
          break;
        }
        print_status(reader, &rxmsg, rxlen);
        break;
      case IFDNFC_GET_STATUS:
        memset(&txmsg, 0, sizeof(txmsg));
        txmsg.command = command;
        if((rv = SCardControl(hCard, IFDNFC_CTRL_ACTIVE, &txmsg, sizeof(txmsg), &rxmsg, sizeof(rxmsg), &rxlen)) < 0) {
          printf("Reader '%s', SCardControl Error (while getting status): %s\n", reader, pcsc_stringify_error(rv));
          break;
        }
        print_status(reader, &rxmsg, rxlen);
        break;
      default:
        printf("Invalid command number: %d.\n", command);
    }  
  }

  if((rv = SCardDisconnect(hCard, SCARD_LEAVE_CARD)) < 0) {
    printf("SCardDisconnect Error: %s\n", pcsc_stringify_error(rv));
  }
  free(mszReaders);

  if(!foundcount) {
    printf("Could not find any pcsc readers with name prefix of: %s.  Check your configuration.\n", IFDNFC_READER_NAME);
    exit(EXIT_FAILURE);
  }
  exit(EXIT_SUCCESS);
}
