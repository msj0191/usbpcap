/*
 * Copyright (c) 2013 Tomasz Mon <desowin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "iocontrol.h"
#include <winioctl.h>

/*
 * Determines range and index for given address.
 *
 * Returns TRUE on success (address is within <0; 127>), FALSE otherwise.
 */
static BOOLEAN adcusbGetAddressRangeAndIndex(int address, UINT8 *range, UINT8 *index)
{
    if ((address < 0) || (address > 127))
    {
        fprintf(stderr, "Invalid address: %d\n", address);
        return FALSE;
    }

    *range = address / 32;
    *index = address % 32;
    return TRUE;
}

BOOLEAN adcusbIsDeviceFiltered(PADCUSB_ADDRESS_FILTER filter, int address)
{
    BOOLEAN filtered = FALSE;
    UINT8 range;
    UINT8 index;

    if (filter->filterAll == TRUE)
    {
        /* Do not check individual bit if all devices are filtered. */
        return TRUE;
    }

    if (adcusbGetAddressRangeAndIndex(address, &range, &index) == FALSE)
    {
        /* Assume that invalid addresses are filtered. */
        return TRUE;
    }

    if (filter->addresses[range] & (1 << index))
    {
        filtered = TRUE;
    }

    return filtered;
}

BOOLEAN adcusbSetDeviceFiltered(PADCUSB_ADDRESS_FILTER filter, int address)
{
    UINT8 range;
    UINT8 index;

    if (adcusbGetAddressRangeAndIndex(address, &range, &index) == FALSE)
    {
        return FALSE;
    }

    filter->addresses[range] |= (1 << index);
    return TRUE;
}

/*
 * Initializes address filter with given NULL-terminated, comma separated list of addresses.
 *
 * Returns TRUE on success, FALSE otherwise (malformed list or invalid filter pointer).
 */
BOOLEAN adcusbInitAddressFilter(PADCUSB_ADDRESS_FILTER filter, PCHAR list, BOOLEAN filterAll)
{
    ADCUSB_ADDRESS_FILTER tmp;

    if (filter == NULL)
    {
        return FALSE;
    }

    memset(&tmp, 0, sizeof(ADCUSB_ADDRESS_FILTER));
    tmp.filterAll = filterAll;

    if (list != NULL)
    {
        while (*list)
        {
            if (isdigit(*list))
            {
                int number;
                number = atoi(list);

                if (adcusbSetDeviceFiltered(&tmp, number) == FALSE)
                {
                    /* Address list contains invalid address. */
                    return FALSE;
                }

                /* Move past number. */
                do
                {
                    list++;
                }
                while (isdigit(*list));
            }
            else if (*list == ',')
            {
                /* Found valid separator, advance to next number. */
                list++;
            }
            else
            {
                fprintf(stderr, "Malformed address list. Invalid character: %c.\n", *list);
                return FALSE;
            }
        }
    }

    /* Address list was valid. Copy resulting structure. */
    memcpy(filter, &tmp, sizeof(ADCUSB_ADDRESS_FILTER));
    return TRUE;
}

/*
 * ???? ???? ???: ADD ??? REMOVE IOCTL ?? ???????? ????.
 * pattern: null ???? ????? ?????, ??: L"Vid_0E0F&Pid_0001"
 * isAdd:   TRUE ?? IOCTL_ADCUSB_ADD_BLOCK_RULE, FALSE ?? REMOVE
 */
static BOOL adcusbSendBlockRule(HANDLE hDevice, LPCWSTR pattern, BOOL isAdd)
{
    ADCUSB_BLOCK_RULE rule;
    DWORD bytesReturned = 0;
    DWORD ioctl = isAdd ? IOCTL_ADCUSB_ADD_BLOCK_RULE : IOCTL_ADCUSB_REMOVE_BLOCK_RULE;

    if ((hDevice == NULL) || (hDevice == INVALID_HANDLE_VALUE))
    {
        return FALSE;
    }

    memset(&rule, 0, sizeof(rule));
    wcsncpy(rule.pattern, pattern, ADCUSB_BLOCK_RULE_PATTERN_LEN - 1);
    rule.pattern[ADCUSB_BLOCK_RULE_PATTERN_LEN - 1] = L'\0';

    return DeviceIoControl(hDevice, ioctl,
                           &rule, sizeof(rule),
                           NULL, 0,
                           &bytesReturned, NULL);
}

/*
 * adcusbAddBlockRule: HardwareID ??????? ?????? ?????? ???? ???? ???.
 *   hDevice: adcusbX ????? ??? ??? (CreateFile ?? ????? ??)
 *   pattern: ??) L"Vid_0E0F&Pid_0001"
 *   ?????:  TRUE = ????, FALSE = ???? (GetLastError ?? ???? ??? ???)
 */
BOOL adcusbAddBlockRule(HANDLE hDevice, LPCWSTR pattern)
{
    return adcusbSendBlockRule(hDevice, pattern, TRUE);
}

/*
 * adcusbRemoveBlockRule: ?????? ???? ?????? ???? ????.
 *   hDevice: adcusbX ????? ??? ???
 *   pattern: ?????? ???? (??? ?? ????? ??? ??????? ??)
 */
BOOL adcusbRemoveBlockRule(HANDLE hDevice, LPCWSTR pattern)
{
    return adcusbSendBlockRule(hDevice, pattern, FALSE);
}

BOOL adcusbSetProtection(HANDLE hDevice, ULONG ulEnable)
{
    ADCUSB_PROTECTION_CONTROL ctrl;
    DWORD dwBytesReturned = 0;
    BOOL bResult = FALSE;

    ctrl.ulEnable = ulEnable;

    bResult = DeviceIoControl(
        hDevice,
        IOCTL_ADCUSB_PROTECTION_CONTROL,
        &ctrl,
        sizeof(ADCUSB_PROTECTION_CONTROL),
        NULL,
        0,
        &dwBytesReturned,
        NULL
    );

    return bResult;
}

BOOL adcusbListBlockRules(HANDLE hDevice, PADCUSB_BLOCK_RULE_LIST_RESPONSE pResponse)
{
	DWORD bytesReturned = 0;

	if ((hDevice == NULL) || (hDevice == INVALID_HANDLE_VALUE))
	{
		return FALSE;
	}

	return DeviceIoControl(hDevice, IOCTL_ADCUSB_LIST_BLOCK_RULES,
	                       NULL, 0,
	                       pResponse, sizeof(*pResponse),
	                       &bytesReturned, NULL);
}
