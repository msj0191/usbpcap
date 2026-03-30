/*
 * Copyright (c) 2013-2019 Tomasz Mon <desowin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include "adcusbMain.h"
#include "adcusbBuffer.h"

int _snprintf(char *buffer, unsigned int count, const char *format, ...);

/* -----------------------------------------------------------------------
 * USB Mass Storage Bulk-Only Transport (BOT) structures
 * USB Mass Storage Class - Bulk Only Transport, Revision 1.0
 * ----------------------------------------------------------------------- */
#define USB_CBW_SIGNATURE	0x43425355UL	/* "USBC" little-endian */
#define USB_CBW_LENGTH		31

#pragma pack(push, 1)
typedef struct _USB_CBW
{
	ULONG	dCBWSignature;			 /* 0x43425355 */
	ULONG	dCBWTag;
	ULONG	dCBWDataTransferLength;
	UCHAR	bmCBWFlags;			  /* bit7: 0=Data-Out, 1=Data-In */
	UCHAR	bCBWLUN;			  /* bits 3:0 */
	UCHAR	bCBWCBLength;		 /* bits 4:0, valid length of CBWCB */
	UCHAR	CBWCB[16];			 /* SCSI command block */
} USB_CBW, *PUSB_CBW;
#pragma pack(pop)

/* SCSI opcodes that write, format, or destructively modify media.
 * SYNCHRONIZE_CACHE (0x35) is intentionally excluded - it flushes
 * write-back cache but does not write new data to media. */
static const UCHAR g_scsiWriteOpcodes[] =
{
	0x0A,	/* WRITE_6             */
	0x2A,	/* WRITE_10            */
	0x8A,	/* WRITE_16            */
	0xAA,	/* WRITE_12            */
	0x2E,	/* WRITE_AND_VERIFY_10 */
	0x8E,	/* WRITE_AND_VERIFY_16 */
	0xAE,	/* WRITE_AND_VERIFY_12 */
	0x04,	/* FORMAT_UNIT         */
	0x41,	/* WRITE_SAME_10       */
	0x93,	/* WRITE_SAME_16       */
	0x42,	/* UNMAP (TRIM)        */
	0x15,	/* MODE_SELECT_6       */
	0x55,	/* MODE_SELECT_10      */
	0x48,	/* SANITIZE            */
	0xB5	 /* SECURITY_PROTOCOL_OUT */
};

static BOOLEAN
adcusbIsScsiWriteOpcode(
	UCHAR ucOpcode)
{
	ULONG ulIdx;

	for (ulIdx = 0; ulIdx < ARRAYSIZE(g_scsiWriteOpcodes); ulIdx++)
	{
		if (g_scsiWriteOpcodes[ulIdx] == ucOpcode)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/*  HardwareID   .
 * DriverEntry KeInitializeSpinLock ,
 * IOCTL_ADCUSB_ADD/REMOVE_BLOCK_RULE   .
 *       . */
ADCUSB_BLOCK_RULE_LIST g_blockRules = {0};

/* PsGetProcessImageFileName:   API (NTKRNLMP.EXE ,   )
 * WDK 7       .
 * : EPROCESS.ImageFileName   ( 15 + NULL, UTF-8/ANSI) */
NTKERNELAPI PUCHAR PsGetProcessImageFileName(PEPROCESS Process);

////////////////////////////////////////////////////////////////////////////
// (Create), (Close), (Cleanup)  
// - ROOTHUB / DEVICE / VMUSB : IRP    
// - CONTROL (adcusbX):   ( ,  /)
//
NTSTATUS DkCreateClose(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    NTSTATUS              ntStat = STATUS_SUCCESS; /*  NTSTATUS ,   */
    PDEVICE_EXTENSION     pDevExt = NULL;          /*     */
    PIO_STACK_LOCATION    pStack = NULL;           /*  IRP    */
    PDEVICE_OBJECT        pNextDevObj = NULL;      /* ()   ( , ) */

    /*       */
    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    /* RemoveLock :  (PnP Remove)  IRP     */
    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat))
    {
        /* RemoveLock  : NTSTATUS     IRP    */
        DkDbgVal("Error acquire lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    /*  IRP   : MajorFunction,       */
    pStack = IoGetCurrentIrpStackLocation(pIrp);

    if (pDevExt->deviceMagic == ADCUSB_MAGIC_ROOTHUB ||
        pDevExt->deviceMagic == ADCUSB_MAGIC_DEVICE  ||
        pDevExt->deviceMagic == ADCUSB_MAGIC_VMUSB)
    {
        /*    /   / vmusb  :
         * Create/Close/Cleanup IRP        */
        IoSkipCurrentIrpStackLocation(pIrp);            /*        */
        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp); /* (next)  IRP  */
    }
    else if (pDevExt->deviceMagic == ADCUSB_MAGIC_CONTROL)
    {
        /*  (adcusbX)  Create/Close/Cleanup   */
        switch (pStack->MajorFunction)
        {
            case IRP_MJ_CREATE:
                /*    
                 *   adcusbCMD():
                 *   DesiredAccess = SYNCHRONIZE|READ_CONTROL|FILE_WRITE_ATTRIBUTES|
                 *                   FILE_READ_ATTRIBUTES|FILE_WRITE_EA|FILE_READ_EA|
                 *                   FILE_APPEND_DATA|FILE_WRITE_DATA|FILE_READ_DATA
                 *  adcusbCMD(  ):
                 *   DesiredAccess = SYNCHRONIZE|FILE_READ_ATTRIBUTES
                 * READ       */
                if (pStack->Parameters.Create.SecurityContext->DesiredAccess & (READ_CONTROL | FILE_READ_DATA))
                {
                    PFILE_OBJECT *previous;
                    /* pCaptureObject  :
                     *   NULL   FileObject   (: previous == NULL)
                     *   NULL     (: previous != NULL) */
                    previous = InterlockedCompareExchangePointer(&pDevExt->context.control.pCaptureObject, pStack->FileObject, NULL);
                    if (previous)
                    {
                        /*          */
                        ntStat = STATUS_ACCESS_DENIED;
                    }
                    /* previous == NULL  pCaptureObject   */
                }
                else
                {
                    /* READ   : IOCTL_ADCUSB_GET_HUB_SYMLINK   */
                }
                break;


            case IRP_MJ_CLEANUP:
                /*     IRP  
                 *  FileObject   (pCaptureObject)     */
                if (InterlockedCompareExchangePointer(&pDevExt->context.control.pCaptureObject, NULL, NULL) == pStack->FileObject)
                {
                    PDEVICE_EXTENSION     rootExt;  /*      */
                    PADCUSB_ROOTHUB_DATA pRootData; /*      */
                    /* Cancel-Safe Queue     IRP   */
                    DkCsqCleanUpQueue(pDevObj, pIrp);
                    /*  :      0  */
                    rootExt = (PDEVICE_EXTENSION)pDevExt->context.control.pRootHubObject->DeviceExtension;
                    pRootData = (PADCUSB_ROOTHUB_DATA)rootExt->context.usb.pDeviceData->pRootData;
                    /*   0   USB   */
                    memset(&pRootData->filter, 0, sizeof(ADCUSB_ADDRESS_FILTER));
                    /*         */
                    adcusbBufferRemoveBuffer(pDevExt);
                }
                break;


            case IRP_MJ_CLOSE:
                /*     :
                 *  FileObject  pCaptureObject   NULL   
                 * ( FileObject   ,  READ   ) */
                InterlockedCompareExchangePointer(&pDevExt->context.control.pCaptureObject, NULL, pStack->FileObject);
                break;


            default:
                /*   MajorFunction:   */
                ntStat = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }

        /* IRP   (0)    */
        DkCompleteRequest(pIrp, ntStat, 0);
    }

    /* RemoveLock : IoAcquireRemoveLock     */
    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return ntStat;
}


//
//--------------------------------------------------------------------------
//

////////////////////////////////////////////////////////////////////////////
// (Read)  (Write)  
// - ROOTHUB / DEVICE: IRP    
// - VMUSB: IRP_MJ_WRITE   (STATUS_ACCESS_DENIED),    
// - CONTROL:     ( )
//

////////////////////////////////////////////////////////////////////////////
// vmusb IRP_MJ_WRITE     DbgPrint  
//
static VOID
DkVmusbLogBlockedWrite(
    PDEVICE_OBJECT     pDevObj,
    PDEVICE_EXTENSION  pDevExt,
    PIRP               pIrp,
    PIO_STACK_LOCATION pStack)
{
    ULONG   writeLength = 0;
    PVOID   writeBuffer = NULL;
    HANDLE  pid;
    HANDLE  tid;
    PUCHAR  procName;
    if (pIrp->MdlAddress != NULL)
    {
        writeLength = MmGetMdlByteCount(pIrp->MdlAddress);
        writeBuffer = MmGetSystemAddressForMdlSafe(
                          pIrp->MdlAddress, NormalPagePriority);
    }
    else if (pIrp->AssociatedIrp.SystemBuffer != NULL)
    {
        writeLength = pStack->Parameters.Write.Length;
        writeBuffer = pIrp->AssociatedIrp.SystemBuffer;
    }
    pid      = PsGetCurrentProcessId();
    tid      = PsGetCurrentThreadId();
    procName = (PUCHAR)PsGetProcessImageFileName(PsGetCurrentProcess());
    DbgPrint("adcusb: [VMUSB][WRITE BLOCKED] ===== IRP_MJ_WRITE   =====\n");
    DbgPrint("adcusb: [VMUSB]  Process=%-15s  PID=%Iu  TID=%Iu\n",
             procName ? (const char *)procName : "(unknown)",
             (ULONG_PTR)pid,
             (ULONG_PTR)tid);
    DbgPrint("adcusb: [VMUSB]  DevObj(filter)=%p  NextDevObj(vmusb FDO)=%p\n",
             pDevExt->pThisDevObj,
             pDevExt->pNextDevObj);
    DbgPrint("adcusb: [VMUSB]  PdoName=%S\n",
             pDevExt->context.vmusb.pdoName);
    {
        ULONG devFlags = (ULONG)pDevObj->Flags;
        DbgPrint("adcusb: [VMUSB]  DeviceType=0x%08lX  DeviceFlags=0x%08lX\n",
                 (ULONG)pDevObj->DeviceType, devFlags);
        DbgPrint("adcusb: [VMUSB]  Flags:%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
                 (devFlags & 0x0002) ? " VERIFY_VOLUME"        : "",
                 (devFlags & 0x0004) ? " BUFFERED_IO"          : "",
                 (devFlags & 0x0008) ? " EXCLUSIVE"            : "",
                 (devFlags & 0x0010) ? " DIRECT_IO"            : "",
                 (devFlags & 0x0020) ? " MAP_IO_BUFFER"        : "",
                 (devFlags & 0x0040) ? " DEVICE_HAS_NAME"      : "",
                 (devFlags & 0x0080) ? " DEVICE_INITIALIZING"  : "",
                 (devFlags & 0x0100) ? " BOOT_PARTITION"       : "",
                 (devFlags & 0x0200) ? " LONG_TERM_REQUESTS"   : "",
                 (devFlags & 0x0400) ? " NEVER_LAST_DEVICE"    : "",
                 (devFlags & 0x0800) ? " SHUTDOWN_REGISTERED"  : "",
                 (devFlags & 0x1000) ? " BUS_ENUMERATED"       : "",
                 (devFlags & 0x2000) ? " POWER_PAGABLE"        : "",
                 (devFlags & 0x4000) ? " POWER_INRUSH"         : "");
    }
    if (pDevExt->pDrvObj != NULL)
    {
        DbgPrint("adcusb: [VMUSB]  DriverName=%wZ\n",
                 &pDevExt->pDrvObj->DriverName);
    }
    DbgPrint("adcusb: [VMUSB]  IRP->Flags=0x%08lX  MinorFunction=0x%02X\n",
             pIrp->Flags,
             pStack->MinorFunction);
    DbgPrint("adcusb: [VMUSB]  WriteByteOffset=0x%I64X  WriteLength=%lu  Buffer=%p\n",
             pStack->Parameters.Write.ByteOffset.QuadPart,
             writeLength,
             writeBuffer);
    if (writeBuffer != NULL && writeLength > 0)
    {
        ULONG  dumpLen = (writeLength < 16) ? writeLength : 16;
        PUCHAR p       = (PUCHAR)writeBuffer;
        char   szHex[64];
        ULONG  k;
        int    pos;
        int    written;
        UCHAR  byte;
        pos = 0;
        szHex[0] = '\0';
        for (k = 0; k < 16; k++)
        {
            byte = (k < dumpLen) ? p[k] : 0;
            if (k == 15)
            {
                written = _snprintf(szHex + pos, (unsigned int)(sizeof(szHex) - 1 - pos), "%02X", byte);
            }
            else if ((k == 4) || (k == 8) || (k == 12))
            {
                written = _snprintf(szHex + pos, (unsigned int)(sizeof(szHex) - 1 - pos), " %02X", byte);
            }
            else
            {
                written = _snprintf(szHex + pos, (unsigned int)(sizeof(szHex) - 1 - pos), "%02X ", byte);
            }
            if (written < 0)
            {
                break;
            }
            pos += written;
            if (pos >= (int)(sizeof(szHex) - 1))
            {
                break;
            }
        }
        if (pos < (int)sizeof(szHex))
        {
            szHex[pos] = '\0';
        }
        else
        {
            szHex[sizeof(szHex) - 1] = '\0';
        }
        DbgPrint("adcusb: [VMUSB]  Data[0..%lu]: %s\n", dumpLen, szHex);
    }
    DbgPrint("adcusb: [VMUSB][WRITE BLOCKED] vmusb FDO    STATUS_ACCESS_DENIED \n");
}

NTSTATUS DkReadWrite(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    NTSTATUS            ntStat = STATUS_SUCCESS; /*  NTSTATUS ,   */
    PDEVICE_EXTENSION   pDevExt = NULL;          /*     */
    PIO_STACK_LOCATION  pStack = NULL;           /*  IRP    */

    /*       */
    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    /* RemoveLock :  (PnP Remove)  IRP     */
    ntStat = IoAcquireRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
    if (!NT_SUCCESS(ntStat)){
        /* RemoveLock  :    IRP    */
        DkDbgVal("Error acquire lock!", ntStat);
        DkCompleteRequest(pIrp, ntStat, 0);
        return ntStat;
    }

    /*  IRP   : MajorFunction,       */
    pStack = IoGetCurrentIrpStackLocation(pIrp);

    if (pDevExt->deviceMagic == ADCUSB_MAGIC_ROOTHUB ||
        pDevExt->deviceMagic == ADCUSB_MAGIC_DEVICE)
    {
        /*      :
         * Read/Write IRP        */
        IoSkipCurrentIrpStackLocation(pIrp);            /*        */
        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp); /* (next)  IRP  */
    }
    else if (pDevExt->deviceMagic == ADCUSB_MAGIC_VMUSB)
    {
        /* vmusb  :
         * g_blockRules.patterns[]   HardwareID   IRP_MJ_WRITE .
         *     vmusb FDO    (   ). */
        if (pStack->MajorFunction == IRP_MJ_WRITE)
        {
            g_protectionEnabled = 1;
            if (InterlockedCompareExchange(&g_protectionEnabled, 0, 0) == 0)
            {
                IoSkipCurrentIrpStackLocation(pIrp);
                ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
                IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
                return ntStat;
            }

            /* ---- g_blockRules     (SpinLock ) ---- */
            {
                KIRQL   oldIrql;
                ULONG   ruleIdx;
                BOOLEAN shouldBlock = FALSE;
                ULONG   ulWriteLength = 0;
                PVOID   pvWriteBuffer = NULL;
                PUSB_CBW pCbw          = NULL;
                UCHAR   ucOpcode      = 0;

                KeAcquireSpinLock(&g_blockRules.lock, &oldIrql);
                for (ruleIdx = 0; ruleIdx < g_blockRules.count && !shouldBlock; ruleIdx++)
                {
                    /* hwId  REG_MULTI_SZ: L"str1\0str2\0...\0" ( null )
                     *  null   wcsstr    */
                    PCWSTR hwIdEntry = pDevExt->context.vmusb.hwId;
                    while (*hwIdEntry != L'\0')
                    {
                        if (wcsstr(hwIdEntry, g_blockRules.patterns[ruleIdx]) != NULL)
                        {
                            shouldBlock = TRUE;
                            break;
                        }
                        hwIdEntry += wcslen(hwIdEntry) + 1;
                        /* hwId[512]    */
                        if (hwIdEntry > pDevExt->context.vmusb.hwId +
                                        (sizeof(pDevExt->context.vmusb.hwId) / sizeof(WCHAR)) - 1)
                        {
                            break;
                        }
                    }
                }
                KeReleaseSpinLock(&g_blockRules.lock, oldIrql);
                if (!shouldBlock)
                {
                    IoSkipCurrentIrpStackLocation(pIrp);
                    ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
                    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
                    return ntStat;
                }

                if (shouldBlock)
                {
                    if (pIrp->MdlAddress != NULL)
                    {
                        ulWriteLength = MmGetMdlByteCount(pIrp->MdlAddress);
                        pvWriteBuffer = MmGetSystemAddressForMdlSafe(
                            pIrp->MdlAddress, NormalPagePriority);
                    }
                    else if (pIrp->AssociatedIrp.SystemBuffer != NULL)
                    {
                        ulWriteLength = pStack->Parameters.Write.Length;
                        pvWriteBuffer = pIrp->AssociatedIrp.SystemBuffer;
                    }

                    if (pvWriteBuffer == NULL ||
                        ulWriteLength != USB_CBW_LENGTH)
                    {
                        IoSkipCurrentIrpStackLocation(pIrp);
                        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
                        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
                        return ntStat;
                    }

                    pCbw = (PUSB_CBW)pvWriteBuffer;

                    if (pCbw->dCBWSignature != USB_CBW_SIGNATURE ||
                        pCbw->bCBWCBLength == 0)
                    {
                        IoSkipCurrentIrpStackLocation(pIrp);
                        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
                        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
                        return ntStat;
                    }

                    ucOpcode = pCbw->CBWCB[0];

                    if (!adcusbIsScsiWriteOpcode(ucOpcode))
                    {
                        IoSkipCurrentIrpStackLocation(pIrp);
                        ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
                        IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);
                        return ntStat;
                    }
                }
            }

            //DkVmusbLogBlockedWrite(pDevObj, pDevExt, pIrp, pStack);
            DkCompleteRequest(pIrp, STATUS_ACCESS_DENIED, 0);
            ntStat = STATUS_ACCESS_DENIED;
        }
        else
        {
            /* (IRP_MJ_WRITE)  IRP (: IRP_MJ_READ)
             *  (vmusb)   */
            IoSkipCurrentIrpStackLocation(pIrp);
            ntStat = IoCallDriver(pDevExt->pNextDevObj, pIrp);
        }
    }
    else if (pDevExt->deviceMagic == ADCUSB_MAGIC_CONTROL)
    {
        UINT32 bytesRead = 0; /*        */
        /*  (adcusbX)  Read/Write  */
        switch (pStack->MajorFunction)
        {
            case IRP_MJ_READ:
            {
                /* READ  (pCaptureObject)  FileObject     */
                if (pStack->FileObject == InterlockedCompareExchangePointer(&pDevExt->context.control.pCaptureObject, NULL, NULL))
                {
                    /*   :    USB    
                     *    IRP   (STATUS_PENDING) */
                    ntStat = adcusbBufferHandleReadIrp(pIrp, pDevExt,
                                                        &bytesRead);
                }
                else
                {
                    /*       :  */
                    ntStat = STATUS_ACCESS_DENIED;
                }
                break;
            }

            case IRP_MJ_WRITE:
                /*       */
                ntStat = STATUS_NOT_SUPPORTED;
                break;


            default:
                /*    MajorFunction:      */
                DkDbgVal("Unknown IRP Major function", pStack->MajorFunction);
                ntStat = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }

        /* IRP  (STATUS_PENDING)     
         * (      ) */
        if (ntStat != STATUS_PENDING)
        {
            DkCompleteRequest(pIrp, ntStat, (ULONG_PTR)bytesRead);
        }
    }

    /* RemoveLock : IoAcquireRemoveLock     */
    IoReleaseRemoveLock(&pDevExt->removeLock, (PVOID) pIrp);

    return ntStat;
}
