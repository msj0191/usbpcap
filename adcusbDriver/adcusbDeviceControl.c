/*
 * Copyright (c) 2013-2019 Tomasz Mon <desowin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include "adcusbMain.h"
#include "include\adcusb.h"
#include "adcusbURB.h"
#include "adcusbRootHubControl.h"
#include "adcusbBuffer.h"
#include "adcusbHelperFunctions.h"

static NTSTATUS
HandleadcusbControlIOCTL(PIRP pIrp, PIO_STACK_LOCATION pStack,
                          PDEVICE_EXTENSION rootExt, PADCUSB_ROOTHUB_DATA pRootData,
                          BOOLEAN allowCapture,
                          SIZE_T *outLength)
{
    NTSTATUS ntStat = STATUS_SUCCESS;
    if (pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_ADCUSB_GET_HUB_SYMLINK)
    {
        PWSTR interfaces;
        SIZE_T length;

        DkDbgStr("IOCTL_ADCUSB_GET_HUB_SYMLINK");

        interfaces = adcusbGetHubInterfaces(rootExt->pNextDevObj);
        if (interfaces == NULL)
        {
            return STATUS_NOT_FOUND;
        }

        length = wcslen(interfaces);
        length = (length+1)*sizeof(WCHAR);

        if (pStack->Parameters.DeviceIoControl.OutputBufferLength < length)
        {
            DkDbgVal("Too small buffer", length);
            ntStat = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            RtlCopyMemory(pIrp->AssociatedIrp.SystemBuffer,
                          (PVOID)interfaces,
                          length);
            *outLength = length;
            DkDbgVal("Successfully copied data", length);
        }
        ExFreePool((PVOID)interfaces);
        return ntStat;
    }

    /* HardwareID     / IOCTL
     * - FILE_ANY_ACCESS  x     
     * - allowCapture     OK */
    if (pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_ADCUSB_ADD_BLOCK_RULE ||
        pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_ADCUSB_REMOVE_BLOCK_RULE)
    {
        PADCUSB_BLOCK_RULE pRule;
        KIRQL              oldIrql;
        BOOLEAN            isAdd;

        if (pStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(ADCUSB_BLOCK_RULE))
        {
            DbgPrint("adcusb: BLOCK_RULE IOCTL     (%lu bytes)\n",
                     pStack->Parameters.DeviceIoControl.InputBufferLength);
            return STATUS_INVALID_PARAMETER;
        }

        pRule = (PADCUSB_BLOCK_RULE)pIrp->AssociatedIrp.SystemBuffer;
        isAdd = (pStack->Parameters.DeviceIoControl.IoControlCode ==
                 IOCTL_ADCUSB_ADD_BLOCK_RULE);

        /*    null   (untrusted   ) */
        pRule->pattern[ADCUSB_BLOCK_RULE_PATTERN_LEN - 1] = L'\0';

        KeAcquireSpinLock(&g_blockRules.lock, &oldIrql);
        if (isAdd)
        {
            if (g_blockRules.count < ADCUSB_MAX_BLOCK_RULES)
            {
                RtlCopyMemory(g_blockRules.patterns[g_blockRules.count],
                              pRule->pattern,
                              sizeof(g_blockRules.patterns[0]));
                g_blockRules.count++;
                DbgPrint("adcusb: ADD_BLOCK_RULE  [%lu] = '%S'  active_rules=%lu\n",
                         g_blockRules.count - 1, pRule->pattern, g_blockRules.count);
            }
            else
            {
                DbgPrint("adcusb: ADD_BLOCK_RULE :  %d \n",
                         ADCUSB_MAX_BLOCK_RULES);
                ntStat = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        else
        {
            ULONG j;
            BOOLEAN found = FALSE;
            for (j = 0; j < g_blockRules.count; j++)
            {
                if (wcsncmp(g_blockRules.patterns[j],
                             pRule->pattern,
                             ADCUSB_BLOCK_RULE_PATTERN_LEN - 1) == 0)
                {
                    /*  :     */
                    if (j + 1 < g_blockRules.count)
                    {
                        RtlMoveMemory(g_blockRules.patterns[j],
                                      g_blockRules.patterns[j + 1],
                                      (g_blockRules.count - j - 1) *
                                      sizeof(g_blockRules.patterns[0]));
                    }
                    g_blockRules.count--;
                    found = TRUE;
                    DbgPrint("adcusb: REMOVE_BLOCK_RULE  '%S' (  %lu)\n",
                             pRule->pattern, g_blockRules.count);
                    break;
                }
            }
            if (!found)
            {
                DbgPrint("adcusb: REMOVE_BLOCK_RULE :  '%S' \n",
                         pRule->pattern);
                ntStat = STATUS_NOT_FOUND;
            }
        }
        KeReleaseSpinLock(&g_blockRules.lock, oldIrql);
        return ntStat;
    }

    if (pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_ADCUSB_PROTECTION_CONTROL)
    {
        PADCUSB_PROTECTION_CONTROL pCtrl;

        if (pStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(ADCUSB_PROTECTION_CONTROL))
        {
            DbgPrint("adcusb: PROTECTION_CONTROL input buffer too small (%lu bytes)\n",
                     pStack->Parameters.DeviceIoControl.InputBufferLength);
            return STATUS_INVALID_PARAMETER;
        }

        pCtrl = (PADCUSB_PROTECTION_CONTROL)pIrp->AssociatedIrp.SystemBuffer;
        InterlockedExchange(&g_protectionEnabled, pCtrl->ulEnable ? 1L : 0L);
        DbgPrint("adcusb: PROTECTION_CONTROL -> g_protectionEnabled = %ld\n",
                 (LONG)InterlockedCompareExchange(&g_protectionEnabled, 0, 0));
        {
            ULONG ulCount;
            KIRQL irql;

            KeAcquireSpinLock(&g_blockRules.lock, &irql);
            ulCount = g_blockRules.count;
            KeReleaseSpinLock(&g_blockRules.lock, irql);

            DbgPrint("adcusb: PROTECTION_CONTROL active_rules=%lu\n", ulCount);
        }
        return STATUS_SUCCESS;
    }

    if (pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_ADCUSB_LIST_BLOCK_RULES)
    {
        ADCUSB_BLOCK_RULE_LIST_RESPONSE response;
        KIRQL                           oldIrql;

        if (pStack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(ADCUSB_BLOCK_RULE_LIST_RESPONSE))
        {
            DbgPrint("adcusb: LIST_BLOCK_RULES output buffer too small (%lu bytes)\n",
                     pStack->Parameters.DeviceIoControl.OutputBufferLength);
            *outLength = sizeof(ADCUSB_BLOCK_RULE_LIST_RESPONSE);
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlZeroMemory(&response, sizeof(response));
        KeAcquireSpinLock(&g_blockRules.lock, &oldIrql);
        response.count = g_blockRules.count;
        RtlCopyMemory(response.patterns,
                      g_blockRules.patterns,
                      sizeof(g_blockRules.patterns));
        KeReleaseSpinLock(&g_blockRules.lock, oldIrql);

        RtlCopyMemory(pIrp->AssociatedIrp.SystemBuffer, &response, sizeof(response));
        *outLength = sizeof(ADCUSB_BLOCK_RULE_LIST_RESPONSE);
        DbgPrint("adcusb: LIST_BLOCK_RULES returned %lu rules\n", response.count);
        return STATUS_SUCCESS;
    }

    /* Other IOCTLs are allowed only for the capture handle (exclusive) */
    if (!allowCapture)
    {
        return STATUS_ACCESS_DENIED;
    }

    switch (pStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_ADCUSB_SETUP_BUFFER:
        {
            PADCUSB_IOCTL_SIZE  pBufferSize;

            if (pStack->Parameters.DeviceIoControl.InputBufferLength !=
                sizeof(ADCUSB_IOCTL_SIZE))
            {
                ntStat = STATUS_INVALID_PARAMETER;
                break;
            }

            pBufferSize = (PADCUSB_IOCTL_SIZE)pIrp->AssociatedIrp.SystemBuffer;
            DkDbgVal("IOCTL_ADCUSB_SETUP_BUFFER", pBufferSize->size);

            ntStat = adcusbSetUpBuffer(pRootData, pBufferSize->size);
            break;
        }

        case IOCTL_ADCUSB_START_FILTERING:
        {
            PADCUSB_ADDRESS_FILTER pAddressFilter;

            if (pStack->Parameters.DeviceIoControl.InputBufferLength !=
                sizeof(ADCUSB_ADDRESS_FILTER))
            {
                ntStat = STATUS_INVALID_PARAMETER;
                break;
            }

            pAddressFilter = (PADCUSB_ADDRESS_FILTER)pIrp->AssociatedIrp.SystemBuffer;
            memcpy(&pRootData->filter, pAddressFilter,
                   sizeof(ADCUSB_ADDRESS_FILTER));

            DkDbgStr("IOCTL_ADCUSB_START_FILTERING");
            DkDbgVal("", pAddressFilter->addresses[0]);
            DkDbgVal("", pAddressFilter->addresses[1]);
            DkDbgVal("", pAddressFilter->addresses[2]);
            DkDbgVal("", pAddressFilter->addresses[3]);
            DkDbgVal("", pAddressFilter->filterAll);
            break;
        }

        case IOCTL_ADCUSB_STOP_FILTERING:
            DkDbgStr("IOCTL_ADCUSB_STOP_FILTERING");
            memset(&pRootData->filter, 0,
                   sizeof(ADCUSB_ADDRESS_FILTER));
            break;

        case IOCTL_ADCUSB_SET_SNAPLEN_SIZE:
        {
            PADCUSB_IOCTL_SIZE  pSnaplen;

            if (pStack->Parameters.DeviceIoControl.InputBufferLength !=
                sizeof(ADCUSB_IOCTL_SIZE))
            {
                ntStat = STATUS_INVALID_PARAMETER;
                break;
            }

            pSnaplen = (PADCUSB_IOCTL_SIZE)pIrp->AssociatedIrp.SystemBuffer;
            DkDbgVal("IOCTL_ADCUSB_SET_SNAPLEN_SIZE", pSnaplen->size);

            ntStat = adcusbSetSnaplenSize(pRootData, pSnaplen->size);
            break;
        }

        default:
        {
            ULONG ctlCode = IoGetFunctionCodeFromCtlCode(pStack->Parameters.DeviceIoControl.IoControlCode);
            DkDbgVal("This: IOCTL_XXXXX", ctlCode);
            ntStat = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
    }
    return ntStat;
}

///////////////////////////////////////////////////////////////////////
// I/O device control request handlers
//
NTSTATUS DkDevCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS;
    PDEVICE_EXTENSION   pDevExt = NULL;
    PIO_STACK_LOCATION  pStack = NULL;
    ULONG               ulRes = 0;

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        DkDbgVal("Error lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    pStack = IoGetCurrentIrpStackLocation(pIrp);

    if (pDevExt->deviceMagic == ADCUSB_MAGIC_ROOTHUB ||
        pDevExt->deviceMagic == ADCUSB_MAGIC_DEVICE  ||
        pDevExt->deviceMagic == ADCUSB_MAGIC_VMUSB)
    {
        IoSkipCurrentIrpStackLocation(pIrp);
        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
    }
    else if (pDevExt->deviceMagic == ADCUSB_MAGIC_CONTROL)
    {
        PDEVICE_EXTENSION      rootExt;
        PADCUSB_ROOTHUB_DATA  pRootData;
        BOOLEAN                allowCapture = FALSE;
        SIZE_T                 length = 0;
        UINT_PTR               info;

        rootExt = (PDEVICE_EXTENSION)pDevExt->context.control.pRootHubObject->DeviceExtension;
        pRootData = (PADCUSB_ROOTHUB_DATA)rootExt->context.usb.pDeviceData->pRootData;
        if (InterlockedCompareExchangePointer(&pDevExt->context.control.pCaptureObject, NULL, NULL) == pStack->FileObject)
        {
            allowCapture = TRUE;
        }

        ntStat = HandleadcusbControlIOCTL(pIrp, pStack, rootExt, pRootData, allowCapture, &length);

        info = (UINT_PTR)length;

        DkCompleteRequest(pIrp, ntStat, info);
    }

    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return ntStat;
}


//
//--------------------------------------------------------------------------
//

////////////////////////////////////////////////////////////////////////////
// Internal I/O device control request handlers
//
NTSTATUS DkInDevCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS;
    PDEVICE_EXTENSION   pDevExt = NULL;
    PIO_STACK_LOCATION  pStack = NULL;
    PDEVICE_OBJECT      pNextDevObj = NULL;
    ULONG               ctlCode = 0;

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        DkDbgVal("Error lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    pStack = IoGetCurrentIrpStackLocation(pIrp);

    ctlCode = IoGetFunctionCodeFromCtlCode(pStack->Parameters.DeviceIoControl.IoControlCode);

    if (pDevExt->deviceMagic == ADCUSB_MAGIC_ROOTHUB)
    {
        DkDbgVal("Hub Filter: IOCTL_INTERNAL_XXXXX", ctlCode);
        pNextDevObj = pDevExt->pNextDevObj;
    }
    else if (pDevExt->deviceMagic == ADCUSB_MAGIC_DEVICE)
    {
        ntStat = DkTgtInDevCtl(pDevExt, pStack, pIrp);

        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

        return ntStat;
    }
    else
    {
        DkDbgVal("This: IOCTL_INTERNAL_XXXXX", ctlCode);
        pNextDevObj = pDevExt->pNextDevObj;
    }

    if (pNextDevObj == NULL)
    {
        ntStat = STATUS_INVALID_DEVICE_REQUEST;
        DkCompleteRequest(pIrp, ntStat, 0);
    }
    else
    {
        IoSkipCurrentIrpStackLocation(pIrp);
        ntStat = IoCallDriver(pNextDevObj, pIrp);
    }

    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return ntStat;
}


NTSTATUS DkTgtInDevCtl(PDEVICE_EXTENSION pDevExt, PIO_STACK_LOCATION pStack, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS;
    PURB                pUrb = NULL;
    ULONG               ctlCode = 0;

    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        DkDbgVal("Error lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    ctlCode = IoGetFunctionCodeFromCtlCode(pStack->Parameters.DeviceIoControl.IoControlCode);

    // Our interest is IOCTL_INTERNAL_USB_SUBMIT_URB, where USB device driver send URB to
    // it's USB bus driver
    if (pStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB)
    {
        /* code here should cope with DISPATCH_LEVEL */

        // URB is collected BEFORE forward to bus driver or next lower object
        pUrb = (PURB) pStack->Parameters.Others.Argument1;
        if (pUrb != NULL)
        {
            adcusbAnalyzeURB(pIrp, pUrb, FALSE,
                              pDevExt->context.usb.pDeviceData);
        }

        // Forward this request to bus driver or next lower object
        // with completion routine
        IoCopyCurrentIrpStackLocationToNext(pIrp);
        IoSetCompletionRoutine(pIrp,
            (PIO_COMPLETION_ROUTINE) DkTgtInDevCtlCompletion,
            NULL, TRUE, TRUE, TRUE);

        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
    }
    else
    {
        DkDbgVal("IOCTL_INTERNAL_USB_XXXX", ctlCode);

        IoSkipCurrentIrpStackLocation(pIrp);
        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);

        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    }

    return ntStat;
}

NTSTATUS DkTgtInDevCtlCompletion(PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pCtx)
{
    NTSTATUS            status;
    PDEVICE_EXTENSION   pDevExt = NULL;
    PURB                pUrb = NULL;
    PIO_STACK_LOCATION  pStack = NULL;

    if (pIrp->PendingReturned)
        IoMarkIrpPending(pIrp);

    status = pIrp->IoStatus.Status;

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    // URB is collected AFTER forward to bus driver or next lower object
    pStack = IoGetCurrentIrpStackLocation(pIrp);
    pUrb = (PURB) pStack->Parameters.Others.Argument1;
    if (pUrb != NULL)
    {
        adcusbAnalyzeURB(pIrp, pUrb, TRUE,
                          pDevExt->context.usb.pDeviceData);
    }

    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return status;
}
