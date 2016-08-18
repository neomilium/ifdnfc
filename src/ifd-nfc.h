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
#ifndef _IFD_NFC_H_
#define _IFD_NFC_H_

#define IFDNFC_READER_NAME   "IFD-NFC"

#define IFDNFC_CTRL_ACTIVE   1

#define IFDNFC_IS_ACTIVE     1
#define IFDNFC_IS_INACTIVE   0

#define IFDNFC_SET_INACTIVE      0
#define IFDNFC_SET_ACTIVE        1
#define IFDNFC_SET_ACTIVE_SE     2
#define IFDNFC_GET_STATUS        3

#ifndef IFD_NFC_OPEN_RETRY_INTERVAL 
#define IFD_NFC_OPEN_RETRY_INTERVAL 10
#endif

#include <nfc/nfc.h>
typedef struct {
    int command;
    nfc_connstring connstring;
} IFDNFC_CONTROL_REQ;

typedef struct {
    int mode;
    int connected;
    int se_avail;
    nfc_connstring connstring;
} IFDNFC_CONTROL_RESP;

#endif
