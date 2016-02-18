/**********************************************************************
 * Copyright (c) 2016  Red Hat, Inc.
 *
 * File: VirtIOWdf.c
 *
 * Author(s):
 *  Ladi Prosek <lprosek@redhat.com>
 *
 * Implementation of VirtioLib-WDF driver API
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
**********************************************************************/
#include "osdep.h"
#include "virtio_pci.h"
#include "virtio_config.h"
#include "VirtioWDF.h"
#include "private.h"

#include <wdmguid.h>

extern VirtIOSystemOps VirtIOWdfSystemOps;

NTSTATUS VirtIOWdfInitialize(PVIRTIO_WDF_DRIVER pWdfDriver,
                             WDFDEVICE Device,
                             WDFCMRESLIST ResourcesTranslated,
                             WDFINTERRUPT ConfigInterrupt,
                             ULONG MemoryTag,
                             USHORT nMaxQueues)
{
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T cbVIODevice;
    int err;

    RtlZeroMemory(pWdfDriver, sizeof(*pWdfDriver));
    pWdfDriver->MemoryTag = MemoryTag;

    /* get the PCI bus interface */
    status = WdfFdoQueryForInterface(
        Device,
        &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&pWdfDriver->PCIBus,
        sizeof(pWdfDriver->PCIBus),
        1 /* version */,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* set up resources */
    status = PCIAllocBars(ResourcesTranslated, pWdfDriver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* initialize the underlying VirtIODevice */
    cbVIODevice = VirtIODeviceSizeRequired(nMaxQueues);
    pWdfDriver->pVIODevice = (VirtIODevice *)ExAllocatePoolWithTag(
        NonPagedPool,
        cbVIODevice,
        pWdfDriver->MemoryTag);
    
    err = virtio_device_initialize(
        pWdfDriver->pVIODevice,
        &VirtIOWdfSystemOps,
        pWdfDriver,
        (ULONG)cbVIODevice);
    if (err != 0) {
        ExFreePoolWithTag(pWdfDriver->pVIODevice, pWdfDriver->MemoryTag);
        PCIFreeBars(pWdfDriver);
        status = virtio_error_to_ntstatus(err);
    }
    
    pWdfDriver->ConfigInterrupt = ConfigInterrupt;
    VirtIODeviceSetMSIXUsed(pWdfDriver->pVIODevice, pWdfDriver->nMSIInterrupts > 0);

    return status;
}

ULONGLONG VirtIOWdfGetDeviceFeatures(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    return virtio_get_features(pWdfDriver->pVIODevice);
}

NTSTATUS VirtIOWdfSetDriverFeatures(PVIRTIO_WDF_DRIVER pWdfDriver,
                                    ULONGLONG uFeatures)
{
    int err;

    /* make sure that we always follow the status bit-setting protocol */
    u8 status = virtio_get_status(pWdfDriver->pVIODevice);
    if (!(status & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
        virtio_add_status(pWdfDriver->pVIODevice, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    }
    if (!(status & VIRTIO_CONFIG_S_DRIVER)) {
        virtio_add_status(pWdfDriver->pVIODevice, VIRTIO_CONFIG_S_DRIVER);
    }

    pWdfDriver->pVIODevice->features = uFeatures;
    err = virtio_finalize_features(pWdfDriver->pVIODevice);
    return virtio_error_to_ntstatus(err);
}

NTSTATUS VirtIOWdfInitQueues(PVIRTIO_WDF_DRIVER pWdfDriver,
                             ULONG nQueues,
                             struct virtqueue **pQueues,
                             PVIRTIO_WDF_QUEUE_PARAM pQueueParams)
{
    int err;
    ULONG i;
    LPCSTR *pszNames;

    /* make sure that we always follow the status bit-setting protocol */
    u8 status = virtio_get_status(pWdfDriver->pVIODevice);
    if (!(status & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
        virtio_add_status(pWdfDriver->pVIODevice, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    }
    if (!(status & VIRTIO_CONFIG_S_DRIVER)) {
        virtio_add_status(pWdfDriver->pVIODevice, VIRTIO_CONFIG_S_DRIVER);
    }
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        err = virtio_finalize_features(pWdfDriver->pVIODevice);
        if (err != 0) {
            return virtio_error_to_ntstatus(err);
        }
    }

    /* extract and validate queue names */
    pszNames = (LPCSTR *)ExAllocatePoolWithTag(
        NonPagedPool,
        nQueues * sizeof(LPCSTR),
        pWdfDriver->MemoryTag);
    if (pszNames == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < nQueues; i++) {
        if (pQueueParams[i].szName) {
            pszNames[i] = pQueueParams[i].szName;
        }
        else {
            pszNames[i] = "_unnamed_queue";
        }
    }

    /* find and initialize queues */
    pWdfDriver->pQueueParams = pQueueParams;
    err = pWdfDriver->pVIODevice->config->find_vqs(
        pWdfDriver->pVIODevice,
        nQueues,
        pQueues,
        NULL,
        pszNames);
    pWdfDriver->pQueueParams = NULL;

    ExFreePoolWithTag((PVOID)pszNames, pWdfDriver->MemoryTag);

    if (err) {
        return virtio_error_to_ntstatus(err);
    }

    /* set interrupt suppression flags */
    for (i = 0; i < nQueues; i++) {
        virtqueue_set_event_suppression(
            pQueues[i],
            pQueueParams[i].bEnableInterruptSuppression);
    }

    return STATUS_SUCCESS;
}

void VirtIOWdfSetDriverOK(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    virtio_device_ready(pWdfDriver->pVIODevice);
}

void VirtIOWdfSetDriverFailed(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    virtio_add_status(pWdfDriver->pVIODevice, VIRTIO_CONFIG_S_FAILED);
}

NTSTATUS VirtIOWdfShutdown(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    virtio_device_shutdown(pWdfDriver->pVIODevice);

    PCIFreeBars(pWdfDriver);
    if (pWdfDriver->pVIODevice) {
        ExFreePoolWithTag(pWdfDriver->pVIODevice, pWdfDriver->MemoryTag);
        pWdfDriver->pVIODevice = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS VirtIOWdfDestroyQueues(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    pWdfDriver->pVIODevice->config->reset(pWdfDriver->pVIODevice);
    pWdfDriver->pVIODevice->config->del_vqs(pWdfDriver->pVIODevice);

    return STATUS_SUCCESS;
}

void VirtIOWdfDeviceGet(PVIRTIO_WDF_DRIVER pWdfDriver,
                        ULONG offset,
                        PVOID buf,
                        ULONG len)
{
    pWdfDriver->pVIODevice->config->get(
        pWdfDriver->pVIODevice,
        offset,
        buf,
        len);
}

void VirtIOWdfDeviceSet(PVIRTIO_WDF_DRIVER pWdfDriver,
                        ULONG offset,
                        CONST PVOID buf,
                        ULONG len)
{
    pWdfDriver->pVIODevice->config->set(
        pWdfDriver->pVIODevice,
        offset,
        buf,
        len);
}

UCHAR VirtIOWdfGetISRStatus(PVIRTIO_WDF_DRIVER pWdfDriver)
{
    return virtio_read_isr_status(pWdfDriver->pVIODevice);
}
