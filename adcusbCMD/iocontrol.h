/*
 * Copyright (c) 2013 Tomasz Mon <desowin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ADCUSB_CMD_IOCONTROL_H
#define ADCUSB_CMD_IOCONTROL_H

#include <basetsd.h>
#include <wtypes.h>
#include "adcusb.h"

BOOLEAN adcusbIsDeviceFiltered(PADCUSB_ADDRESS_FILTER filter, int address);
BOOLEAN adcusbSetDeviceFiltered(PADCUSB_ADDRESS_FILTER filter, int address);
BOOLEAN adcusbInitAddressFilter(PADCUSB_ADDRESS_FILTER filter, PCHAR list, BOOLEAN filterAll);

/* VMware USB (vmusb) ?????? ID ???? ?? ???? */
BOOL adcusbAddBlockRule(HANDLE hDevice, LPCWSTR pattern);
BOOL adcusbRemoveBlockRule(HANDLE hDevice, LPCWSTR pattern);
BOOL adcusbSetProtection(HANDLE hDevice, ULONG ulEnable);
BOOL adcusbListBlockRules(HANDLE hDevice, PADCUSB_BLOCK_RULE_LIST_RESPONSE pResponse);

#endif /* ADCUSB_CMD_IOCONTROL_H */
