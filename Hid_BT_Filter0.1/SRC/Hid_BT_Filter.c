// Driver.c: Common entry point and WPP trace filter handler

#include "Hid_BT_Filter.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, PtpFilterEvtDeviceAdd)
#pragma alloc_text (PAGE, PtpFilterEvtDriverContextCleanup)
#pragma alloc_text (PAGE, PtpFilterCreateDevice)
#pragma alloc_text (PAGE, PtpFilterIoQueueInitialize)
#endif



NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    PAGED_CODE();

    KdPrint(("DriverEntry start , %x\n", 0));
   

    // Register a cleanup callback
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = PtpFilterEvtDriverContextCleanup;

    // Register WDF driver
    WDF_DRIVER_CONFIG_INIT(&config, PtpFilterEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
        KdPrint(("WdfDriverCreate failed , %x\n", status));
        return status;
    }

    KdPrint(("DriverEntry end , %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Driver Initialized");
    return STATUS_SUCCESS;
}

NTSTATUS
PtpFilterEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // We do not own power control.
    // In addition we do not own every I/O request.
    WdfFdoInitSetFilter(DeviceInit);

    // Create the device.
    status = PtpFilterCreateDevice(DeviceInit);

    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return status;
}

VOID
PtpFilterEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();

}



NTSTATUS
PtpFilterCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_TIMER_CONFIG timerConfig;
    WDF_WORKITEM_CONFIG workitemConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status;

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    KdPrint(("PtpFilterCreateDevice start , %x\n", 0));

    // Initialize Power Callback
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = PtpFilterPrepareHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = PtpFilterDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = PtpFilterDeviceD0Exit;
    pnpPowerCallbacks.EvtDeviceSelfManagedIoInit = PtpFilterSelfManagedIoInit;
    pnpPowerCallbacks.EvtDeviceSelfManagedIoRestart = PtpFilterSelfManagedIoRestart;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    // Create WDF device object
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfDeviceCreate failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreate failed: %!STATUS!", status);
        goto exit;
    }

    // Initialize context and interface
    deviceContext = PtpFilterGetContext(device);
    deviceContext->Device = device;
    deviceContext->WdmDeviceObject = WdfDeviceWdmGetDeviceObject(device);
    if (deviceContext->WdmDeviceObject == NULL) {
        KdPrint(("PtpFilterCreateDevice WdfDeviceWdmGetDeviceObject failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceWdmGetDeviceObject failed");
        goto exit;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVICEINTERFACE_Hid_BT_Filter, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfDeviceCreateDeviceInterface failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceCreateDeviceInterface failed: %!STATUS!", status);
        goto exit;
    }

    // Initialize read buffer
    status = WdfLookasideListCreate(WDF_NO_OBJECT_ATTRIBUTES, REPORT_BUFFER_SIZE,
                                    NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, PTP_LIST_POOL_TAG,
                                    &deviceContext->HidReadBufferLookaside
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfLookasideListCreate failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfLookasideListCreate failed: %!STATUS!", status);
    }

    // Initialize HID recovery timer
    WDF_TIMER_CONFIG_INIT(&timerConfig, PtpFilterRecoveryTimerCallback);
    timerConfig.AutomaticSerialization = TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.ParentObject = device;
    deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &deviceAttributes, &deviceContext->HidTransportRecoveryTimer);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfTimerCreate failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfTimerCreate failed: %!STATUS!", status);
    }

    // Initialize HID recovery workitem
    WDF_WORKITEM_CONFIG_INIT(&workitemConfig, PtpFilterWorkItemCallback);
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.ParentObject = device;
    status = WdfWorkItemCreate(&workitemConfig, &deviceAttributes, &deviceContext->HidTransportRecoveryWorkItem);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice HidTransportRecoveryWorkItem failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "HidTransportRecoveryWorkItem failed: %!STATUS!", status);
    }

    // Set initial state
    deviceContext->VendorID = 0;
    deviceContext->ProductID = 0;
    deviceContext->VersionNumber = 0;
    deviceContext->DeviceConfigured = FALSE;

    // Initialize IO queue
    status = PtpFilterIoQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice PtpFilterIoQueueInitialize failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "PtpFilterIoQueueInitialize failed: %!STATUS!", status);
    }

exit:
    KdPrint(("PtpFilterCreateDevice end, %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

NTSTATUS
PtpFilterPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
)
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    // We don't need to retrieve resources since this works as a filter now
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    deviceContext = PtpFilterGetContext(Device);

    // Initialize IDs, set to zero
    deviceContext->VendorID = 0;
    deviceContext->ProductID = 0;
    deviceContext->VersionNumber = 0;
    deviceContext->DeviceConfigured = FALSE;

    KdPrint(("PtpFilterPrepareHardware end, %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

NTSTATUS
PtpFilterDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");


    //
    runtimes_IOCTL = 0;
    runtimes_IOREAD = 0;



    KdPrint(("PtpFilterDeviceD0Entry end, %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

NTSTATUS
PtpFilterDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;
    WDFREQUEST outstandingRequest;

    UNREFERENCED_PARAMETER(TargetState);

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    deviceContext = PtpFilterGetContext(Device);

    // Reset device state
    deviceContext->DeviceConfigured = FALSE;

    // Cancelling all outstanding requests
    while (NT_SUCCESS(status)) {
        status = WdfIoQueueRetrieveNextRequest(
            deviceContext->HidReadQueue,
            &outstandingRequest
        );

        if (NT_SUCCESS(status)) {
            WdfRequestComplete(outstandingRequest, STATUS_CANCELLED);
        }
    }

    KdPrint(("PtpFilterDeviceD0Exit end, %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS
PtpFilterSelfManagedIoInit(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status;
    PDEVICE_CONTEXT deviceContext;
    WDF_MEMORY_DESCRIPTOR hidAttributeMemoryDescriptor;
    HID_DEVICE_ATTRIBUTES deviceAttributes;

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

    deviceContext = PtpFilterGetContext(Device);
    status = PtpFilterDetourWindowsHIDStack(Device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterSelfManagedIoInit PtpFilterDetourWindowsHIDStack failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! PtpFilterDetourWindowsHIDStack failed, Status = %!STATUS!", status);
        goto exit;
    }

    // Request device attribute descriptor for self-identification.
    RtlZeroMemory(&deviceAttributes, sizeof(deviceAttributes));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &hidAttributeMemoryDescriptor,
        (PVOID)&deviceAttributes,
        sizeof(deviceAttributes)
    );

    status = WdfIoTargetSendInternalIoctlSynchronously(
        deviceContext->HidIoTarget, NULL,
        IOCTL_HID_GET_DEVICE_ATTRIBUTES,
        NULL, &hidAttributeMemoryDescriptor, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterSelfManagedIoInit WdfIoTargetSendInternalIoctlSynchronously failed, %x\n", status));
        goto exit;
    }

    deviceContext->VendorID = deviceAttributes.VendorID;
    deviceContext->ProductID = deviceAttributes.ProductID;
    deviceContext->VersionNumber = deviceAttributes.VersionNumber;

    KdPrint(("PtpFilterSelfManagedIoInit deviceAttributes.VendorID = %x, ProductID = %x, VersionNumber = %x, \n", \
            deviceContext->VendorID, deviceContext->ProductID, deviceContext->VersionNumber));

    status = PtpFilterConfigureMultiTouch(Device);
    if (!NT_SUCCESS(status)) {
        // If this failed, we will retry after 2 seconds (and pretend nothing happens)
        KdPrint(("PtpFilterSelfManagedIoInit PtpFilterConfigureMultiTouch failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! PtpFilterConfigureMultiTouch failed, Status = %!STATUS!", status);
        status = STATUS_SUCCESS;
        WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(2));
        goto exit;
    }

    // Stamp last query performance counter
    KeQueryPerformanceCounter(&deviceContext->LastReportTime);

    // Set device state
    deviceContext->DeviceConfigured = TRUE;

exit:
    KdPrint(("PtpFilterSelfManagedIoInit end, %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

NTSTATUS
PtpFilterSelfManagedIoRestart(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    KdPrint(("PtpFilterSelfManagedIoRestart start, %x\n", status));

    deviceContext = PtpFilterGetContext(Device);

    // If this is first D0, it will be done in self-managed IO init.
    if (deviceContext->IsHidIoDetourCompleted) {
        status = PtpFilterConfigureMultiTouch(Device);
        if (!NT_SUCCESS(status)) {
            KdPrint(("PtpFilterSelfManagedIoRestart PtpFilterConfigureMultiTouch failed, %x\n", status));
            //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! PtpFilterConfigureMultiTouch failed, Status = %!STATUS!", status);
            // If this failed, we will retry after 2 seconds (and pretend nothing happens)
            status = STATUS_SUCCESS;
            WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(2));
            goto exit;
        }
    }
    else {
        KdPrint(("PtpFilterSelfManagedIoRestart HID detour should already complete here, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! HID detour should already complete here");
        status = STATUS_INVALID_STATE_TRANSITION;
    }

    // Stamp last query performance counter
    KeQueryPerformanceCounter(&deviceContext->LastReportTime);

    // Set device state
    deviceContext->DeviceConfigured = TRUE;

exit:
    KdPrint(("PtpFilterSelfManagedIoRestart end, %x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

NTSTATUS
PtpFilterConfigureMultiTouch(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;

    UCHAR hidPacketBuffer[HID_XFER_PACKET_SIZE];
    PHID_XFER_PACKET pHidPacket;
    WDFMEMORY hidMemory;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_REQUEST_SEND_OPTIONS configRequestSendOptions;
    WDFREQUEST configRequest;
    PIRP pConfigIrp = NULL;

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    deviceContext = PtpFilterGetContext(Device);

    if (1) {
        return status;
    }


    RtlZeroMemory(hidPacketBuffer, sizeof(hidPacketBuffer));
    pHidPacket = (PHID_XFER_PACKET)&hidPacketBuffer;

    if (deviceContext->VendorID == HID_VID_APPLE_USB) {
        pHidPacket->reportId = 0x02;
        pHidPacket->reportBufferLen = 0x04;
        pHidPacket->reportBuffer = (PUCHAR)pHidPacket + sizeof(HID_XFER_PACKET);
        pHidPacket->reportBuffer[0] = 0x02;
        pHidPacket->reportBuffer[1] = 0x01;
        pHidPacket->reportBuffer[2] = 0x00;
        pHidPacket->reportBuffer[3] = 0x00;
    }
    else if (deviceContext->VendorID == HID_VID_APPLE_BT) {

        pHidPacket->reportId = 0xF1;
        pHidPacket->reportBufferLen = 0x03;
        pHidPacket->reportBuffer = (PUCHAR)pHidPacket + sizeof(HID_XFER_PACKET);
        pHidPacket->reportBuffer[0] = 0xF1;
        pHidPacket->reportBuffer[1] = 0x02;
        pHidPacket->reportBuffer[2] = 0x01;
    }
    else {
        // Something we don't support yet.
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! Unrecognized device detected");
        status = STATUS_NOT_SUPPORTED;
        goto exit;
    }

    // Init a request entity.
    // Because we bypassed HIDCLASS driver, there's a few things that we need to manually take care of.
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, deviceContext->HidIoTarget, &configRequest);
    if (!NT_SUCCESS(status)) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestCreate failed, Status = %!STATUS!", status);
        goto exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = configRequest;
    status = WdfMemoryCreatePreallocated(&attributes, (PVOID)pHidPacket, HID_XFER_PACKET_SIZE, &hidMemory);
    if (!NT_SUCCESS(status)) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfMemoryCreatePreallocated failed, Status = %!STATUS!", status);
        goto cleanup;
    }

    status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget,
                                                      configRequest, IOCTL_HID_SET_FEATURE,
                                                      hidMemory, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfIoTargetFormatRequestForInternalIoctl failed, Status = %!STATUS!", status);
        goto cleanup;
    }

    // Manually take care of IRP to meet requirements of mini drivers.
    pConfigIrp = WdfRequestWdmGetIrp(configRequest);
    if (pConfigIrp == NULL) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestWdmGetIrp failed");
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    // God-damn-it we have to configure it by ourselves :)
    pConfigIrp->UserBuffer = pHidPacket;

    WDF_REQUEST_SEND_OPTIONS_INIT(&configRequestSendOptions, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    if (WdfRequestSend(configRequest, deviceContext->HidIoTarget, &configRequestSendOptions) == FALSE) {
        status = WdfRequestGetStatus(configRequest);
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestSend failed, Status = %!STATUS!", status);
        goto cleanup;
    }
    else {
        //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Changed trackpad status to multitouch mode");
        status = STATUS_SUCCESS;
    }

cleanup:
    if (configRequest != NULL) {
        WdfObjectDelete(configRequest);
    }
exit:
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

VOID
PtpFilterRecoveryTimerCallback(
    WDFTIMER Timer
)
{
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status;

    device = WdfTimerGetParentObject(Timer);
    deviceContext = PtpFilterGetContext(device);

    // We will try to reinitialize the device
    status = PtpFilterSelfManagedIoRestart(device);
    if (NT_SUCCESS(status)) {
        // If succeeded, proceed to reissue the request.
        // Otherwise it will retry the process after a few seconds.
        PtpFilterInputIssueTransportRequest(device);
    }
}



NTSTATUS
PtpFilterDetourWindowsHIDStack(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;
    PDEVICE_OBJECT  hidTransportWdmDeviceObject = NULL;
    PDRIVER_OBJECT  hidTransportWdmDriverObject = NULL;
    PIO_CLIENT_EXTENSION hidTransportIoClientExtension = NULL;
    PHIDCLASS_DRIVER_EXTENSION hidTransportClassExtension = NULL;

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");
    deviceContext = PtpFilterGetContext(Device);

    if (deviceContext->WdmDeviceObject == NULL || deviceContext->WdmDeviceObject->DriverObject == NULL) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WDM Device Object or Driver Object is null, can't continue");
        status = STATUS_UNSUCCESSFUL;
        goto exit;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack WdmDeviceObject= , %S\n", deviceContext->WdmDeviceObject->DriverObject->DriverName.Buffer));

    // Access the driver object to find next low-level device (in our case, we expect it to be HID transport driver)
    hidTransportWdmDeviceObject = IoGetLowerDeviceObject(deviceContext->WdmDeviceObject);
    if (hidTransportWdmDeviceObject == NULL) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! IoGetLowerDeviceObject returns null, can't continue");
        status = STATUS_UNSUCCESSFUL;
        goto exit;
    }

    
    hidTransportWdmDriverObject = hidTransportWdmDeviceObject->DriverObject;
    if (hidTransportWdmDriverObject == NULL) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! DriverObject of HID transport Device Object is null, can't continue");
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack hidTransportWdmDeviceObject= , %S\n", hidTransportWdmDeviceObject->DriverObject->DriverName.Buffer));


    // Verify if the driver extension is what we expected.
    if (hidTransportWdmDriverObject->DriverExtension == NULL) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! DriverExtension of HID transport Driver Object is null, can't continue");
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack hidTransportWdmDeviceObject->DriverExtension ok\n", 1));

    // Just two more check...
    hidTransportIoClientExtension = ((PDRIVER_EXTENSION_EXT)hidTransportWdmDriverObject->DriverExtension)->IoClientExtension;
    if (hidTransportIoClientExtension == NULL) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! IO Extension is NULL, can't continue");
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }
    KdPrint(("PtpFilterDetourWindowsHIDStack hidTransportIoClientExtension ok\n", 1));

    if (strncmp(HID_CLASS_EXTENSION_LITERAL_ID, hidTransportIoClientExtension->ClientIdentificationAddress, sizeof(HID_CLASS_EXTENSION_LITERAL_ID)) != 0) {
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! IO Extension mistmatch, can't continue");
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack strncmp ok\n", 1));

    hidTransportClassExtension = (PHIDCLASS_DRIVER_EXTENSION)(hidTransportIoClientExtension + 1);
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Sanity check seems okay, safe to patch IO handler routines");

    // HIDClass overrides:
    // IRP_MJ_SYSTEM_CONTROL, IRP_MJ_WRITE, IRP_MJ_READ, IRP_MJ_POWER, IRP_MJ_PNP, IRP_MJ_INTERNAL_DEVICE_CONTROL, IRP_MJ_DEVICE_CONTROL
    // IRP_MJ_CREATE, IRP_MJ_CLOSE
    // For us, overriding IRP_MJ_DEVICE_CONTROL and IRP_MJ_INTERNAL_DEVICE_CONTROL might be sufficient.
    // Details: https://ligstd.visualstudio.com/Apple%20PTP%20Trackpad/_wiki/wikis/Apple-PTP-Trackpad.wiki/47/Hijack-HIDCLASS
    hidTransportWdmDriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = hidTransportClassExtension->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL];
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! IRP_MJ_INTERNAL_DEVICE_CONTROL patched");

    // Mark detour as completed.
    deviceContext->IsHidIoDetourCompleted = TRUE;
    deviceContext->HidIoTarget = WdfDeviceGetIoTarget(Device);

cleanup:
    if (hidTransportWdmDeviceObject != NULL) {
        ObDereferenceObject(hidTransportWdmDeviceObject);
    }
exit:
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}



NTSTATUS
PtpFilterIoQueueInitialize(
    _In_ WDFDEVICE Device
)
{
    WDFQUEUE queue;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    PDEVICE_CONTEXT deviceContext;
    PQUEUE_CONTEXT queueContext;
    NTSTATUS status;

    PAGED_CODE();
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    deviceContext = PtpFilterGetContext(Device);

    // First queue for system-wide HID controls
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, QUEUE_CONTEXT);
    queueConfig.EvtIoInternalDeviceControl = FilterEvtIoIntDeviceControl;
    queueConfig.EvtIoStop = FilterEvtIoStop;
    status = WdfIoQueueCreate(Device, &queueConfig, &queueAttributes, &queue);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("PtpFilterIoQueueInitialize WdfIoQueueCreate failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! WdfIoQueueCreate failed %!STATUS!", status);
        goto exit;
    }

    queueContext = PtpFilterQueueGetContext(queue);
    queueContext->Device = deviceContext->Device;
    queueContext->DeviceIoTarget = deviceContext->HidIoTarget;

    // Second queue for HID read requests
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->HidReadQueue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterIoQueueInitialize WdfIoQueueCreate Input failed, %x\n", status));
        //TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! WdfIoQueueCreate (Input) failed %!STATUS!", status);
    }

exit:
    KdPrint(("PtpFilterIoQueueInitialize end,%x\n", status));
    //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit, Status = %!STATUS!", status);
    return status;
}

VOID
FilterEvtIoIntDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    PQUEUE_CONTEXT queueContext;
    PDEVICE_CONTEXT deviceContext;
    BOOLEAN requestPending = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    queueContext = PtpFilterQueueGetContext(Queue);
    deviceContext = PtpFilterGetContext(queueContext->Device);

    runtimes_IOCTL++;
    KdPrint(("FilterEvtIoIntDeviceControl runtimes_IOCTL,%x\n", runtimes_IOCTL));


    switch (IoControlCode)
    {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_DEVICE_DESCRIPTOR,%x\n", runtimes_IOCTL));
            status = PtpFilterGetHidDescriptor(queueContext->Device, Request);
            break;
        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_DEVICE_ATTRIBUTES,%x\n", runtimes_IOCTL));
            status = PtpFilterGetDeviceAttribs(queueContext->Device, Request);
            break;
        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_REPORT_DESCRIPTOR,%x\n", runtimes_IOCTL));
            status = PtpFilterGetReportDescriptor(queueContext->Device, Request);
            break;
        case IOCTL_HID_GET_STRING:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_STRING,%x\n", runtimes_IOCTL));
            status = PtpFilterGetStrings(queueContext->Device, Request, &requestPending);
            break;
        case IOCTL_HID_READ_REPORT:
            runtimes_IOREAD++;
            if (runtimes_IOREAD == 1) {
                KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_READ_REPORT,%x\n", runtimes_IOREAD));
            }
            PtpFilterInputProcessRequest(queueContext->Device, Request);
            requestPending = TRUE;
            break;
        case IOCTL_HID_GET_FEATURE:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_FEATURE,%x\n", runtimes_IOCTL));
            status = PtpFilterGetHidFeatures(queueContext->Device, Request);
            break;
        case IOCTL_HID_SET_FEATURE:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_SET_FEATURE,%x\n", runtimes_IOCTL));
            status = PtpFilterSetHidFeatures(queueContext->Device, Request);
            break;
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
        case IOCTL_UMDF_HID_GET_INPUT_REPORT:
        case IOCTL_HID_ACTIVATE_DEVICE:
        case IOCTL_HID_DEACTIVATE_DEVICE:
        case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
        default:
            status = STATUS_NOT_SUPPORTED;
            KdPrint(("FilterEvtIoIntDeviceControl STATUS_NOT_SUPPORTED,%x\n", runtimes_IOCTL));
            //TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE, "%!FUNC!: %s is not yet implemented", PtpFilterDiagnosticsIoControlGetString(IoControlCode));
            break;
    }

    if (requestPending != TRUE)
    {
        KdPrint(("FilterEvtIoIntDeviceControl Status,%x\n", status));
        //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC!: %s, Status = %!STATUS!", PtpFilterDiagnosticsIoControlGetString(IoControlCode), status);
        WdfRequestComplete(Request, status);
    }
}

VOID
FilterEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ActionFlags);

    KdPrint(("FilterEvtIoStop end,%x\n", 0));
}



NTSTATUS
PtpFilterGetHidDescriptor(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS        status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;
	size_t			hidDescriptorSize = 0;
	WDFMEMORY       requestMemory;

	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Entry");
	deviceContext = PtpFilterGetContext(Device);

	status = WdfRequestRetrieveOutputMemory(Request, &requestMemory);
	if (!NT_SUCCESS(status)) {
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!", status);
		goto exit;
	}

	hidDescriptorSize = DefaultHidDescriptor.bLength;


	status = WdfMemoryCopyFromBuffer(requestMemory, 0, (PVOID)&DefaultHidDescriptor, hidDescriptorSize);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterGetHidDescriptor WdfMemoryCopyFromBuffer err,%x\n", status));
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!", status);
		goto exit;
	}

	WdfRequestSetInformation(Request, hidDescriptorSize);


exit:
	KdPrint(("PtpFilterGetHidDescriptor end,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Exit");
	return status;
}

NTSTATUS
PtpFilterGetDeviceAttribs(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS               status = STATUS_SUCCESS;
	PDEVICE_CONTEXT        deviceContext;
	PHID_DEVICE_ATTRIBUTES pDeviceAttributes = NULL;

	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Entry");
	deviceContext = PtpFilterGetContext(Device);

	status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES), &pDeviceAttributes, NULL);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterGetDeviceAttribs WdfRequestRetrieveOutputBuffer err,%x\n", status));
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!", status);
		goto exit;
	}

	pDeviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	// Okay here's one thing: we cannot report the real ID here, otherwise there's will be some great conflict with the USB/BT driver.
	// Therefore Vendor ID is changed to a hardcoded number
	pDeviceAttributes->ProductID = deviceContext->ProductID;
	pDeviceAttributes->VendorID = deviceContext->VendorID;
	pDeviceAttributes->VersionNumber = deviceContext->VersionNumber;
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

exit:
	KdPrint(("PtpFilterGetDeviceAttribs end,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Exit");
	return status;
}

NTSTATUS
PtpFilterGetReportDescriptor(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS               status = STATUS_SUCCESS;
	PDEVICE_CONTEXT        deviceContext;
	size_t			       hidDescriptorSize = 0;
	WDFMEMORY              requestMemory;

	KdPrint(("PtpFilterGetReportDescriptor start,%x\n", 0));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Entry");
	deviceContext = PtpFilterGetContext(Device);

	status = WdfRequestRetrieveOutputMemory(Request, &requestMemory);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterGetReportDescriptor WdfRequestRetrieveOutputBuffer err,%x\n", status));
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!", status);
		goto exit;
	}

	hidDescriptorSize = DefaultHidDescriptor.DescriptorList[0].wReportLength;


	status = WdfMemoryCopyFromBuffer(requestMemory, 0, (PVOID)&ParallelMode_PtpReportDescriptor, hidDescriptorSize);
	if (!NT_SUCCESS(status)) {
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!", status);
		goto exit;
	}

	WdfRequestSetInformation(Request, hidDescriptorSize);


exit:
	KdPrint(("PtpFilterGetReportDescriptor end,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Exit");
	return status;
}

NTSTATUS
PtpFilterGetStrings(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_Out_ BOOLEAN* Pending
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;
	BOOLEAN requestSent;
	WDF_REQUEST_SEND_OPTIONS sendOptions;

    KdPrint(("PtpFilterGetStrings start,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Entry");
	deviceContext = PtpFilterGetContext(Device);

	// Forward the IRP to our upstream IO target
	// We don't really care about the content
	WdfRequestFormatRequestUsingCurrentType(Request);
	WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	// This IOCTL is METHOD_NEITHER, so we just send it without IRP modification
	requestSent = WdfRequestSend(Request, deviceContext->HidIoTarget, &sendOptions);
	*Pending = TRUE;

	if (!requestSent)
	{
		status = WdfRequestGetStatus(Request);
		*Pending = FALSE;
	}

    KdPrint(("PtpFilterGetStrings end,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Exit");
	return status;
}

NTSTATUS
PtpFilterGetHidFeatures(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;

	WDF_REQUEST_PARAMETERS requestParameters;
	size_t reportSize;
	PHID_XFER_PACKET hidContent;

	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Entry");
	deviceContext = PtpFilterGetContext(Device);

	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(Request, &requestParameters);
	if (requestParameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
        KdPrint(("PtpFilterGetHidFeatures STATUS_BUFFER_TOO_SMALL,%x\n", status));
		status = STATUS_BUFFER_TOO_SMALL;
		goto exit;
	}

	hidContent = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
	if (hidContent == NULL)
	{
        KdPrint(("PtpFilterGetHidFeatures STATUS_INVALID_DEVICE_REQUEST,%x\n", status));
		status = STATUS_INVALID_DEVICE_REQUEST;
		goto exit;
	}

	switch (hidContent->reportId)
	{
		case FAKE_REPORTID_DEVICE_CAPS:
		{
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_DEVICE_CAPS is requested");

			// Size sanity check
			reportSize = sizeof(PTP_DEVICE_CAPS_FEATURE_REPORT);
			if (hidContent->reportBufferLen < reportSize) {
				status = STATUS_INVALID_BUFFER_SIZE;
                KdPrint(("PtpFilterGetHidFeatures STATUS_INVALID_BUFFER_SIZE,%x\n", status));
				//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! Report buffer is too small");
				goto exit;
			}

			PPTP_DEVICE_CAPS_FEATURE_REPORT capsReport = (PPTP_DEVICE_CAPS_FEATURE_REPORT)hidContent->reportBuffer;
			capsReport->MaximumContactPoints = PTP_MAX_CONTACT_POINTS;
			capsReport->ButtonType = PTP_BUTTON_TYPE_CLICK_PAD;
			capsReport->ReportID = FAKE_REPORTID_DEVICE_CAPS;

            KdPrint(("PtpFilterGetHidFeatures FAKE_REPORTID_DEVICE_CAPS,%x\n", status));
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_DEVICE_CAPS has maximum contact points of %d", capsReport->MaximumContactPoints);
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_DEVICE_CAPS has touchpad type %d", capsReport->ButtonType);
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_DEVICE_CAPS is fulfilled");
			break;
		}
		case FAKE_REPORTID_PTPHQA:
		{
            KdPrint(("PtpFilterGetHidFeatures FAKE_REPORTID_PTPHQA,%x\n", status));
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_PTPHQA is requested");

			// Size sanity check
			reportSize = sizeof(PTP_DEVICE_HQA_CERTIFICATION_REPORT);
			if (hidContent->reportBufferLen < reportSize)
			{
				status = STATUS_INVALID_BUFFER_SIZE;
                KdPrint(("PtpFilterGetHidFeatures FAKE_REPORTID_PTPHQA STATUS_INVALID_BUFFER_SIZE,%x\n", status));
				//TraceEvents(TRACE_LEVEL_ERROR, TRACE_HID, "%!FUNC! Report buffer is too small.");
				goto exit;
			}

			PPTP_DEVICE_HQA_CERTIFICATION_REPORT certReport = (PPTP_DEVICE_HQA_CERTIFICATION_REPORT)hidContent->reportBuffer;
			*certReport->CertificationBlob = DEFAULT_PTP_HQA_BLOB;
			certReport->ReportID = FAKE_REPORTID_PTPHQA;

			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_PTPHQA is fulfilled");
			break;
		}
		default:
		{
            KdPrint(("PtpFilterGetHidFeatures STATUS_NOT_SUPPORTED,%x\n", status));
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Unsupported type %d is requested", hidContent->reportId);
			status = STATUS_NOT_SUPPORTED;
			goto exit;
		}
	}

exit:
    KdPrint(("PtpFilterGetHidFeatures end,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Exit");
	return status;
}

NTSTATUS
PtpFilterSetHidFeatures(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;

	PHID_XFER_PACKET hidPacket;
	WDF_REQUEST_PARAMETERS requestParameters;

    KdPrint(("PtpFilterSetHidFeatures start,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Entry");
	deviceContext = PtpFilterGetContext(Device);

	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(Request, &requestParameters);
	if (requestParameters.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
        KdPrint(("PtpFilterSetHidFeatures STATUS_BUFFER_TOO_SMALL,%x\n", status));
		status = STATUS_BUFFER_TOO_SMALL;
		goto exit;
	}

	hidPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
	if (hidPacket == NULL)
	{
		status = STATUS_INVALID_DEVICE_REQUEST;
        KdPrint(("PtpFilterSetHidFeatures STATUS_INVALID_DEVICE_REQUEST,%x\n", status));
		goto exit;
	}

	switch (hidPacket->reportId)
	{
		case FAKE_REPORTID_INPUTMODE:
		{
            KdPrint(("PtpFilterSetHidFeatures FAKE_REPORTID_INPUTMODE,%x\n", status));
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_REPORTMODE is requested");

			PPTP_DEVICE_INPUT_MODE_REPORT DeviceInputMode = (PPTP_DEVICE_INPUT_MODE_REPORT)hidPacket->reportBuffer;
			switch (DeviceInputMode->Mode)
			{
				case PTP_COLLECTION_MOUSE:
				{
                    KdPrint(("PtpFilterSetHidFeatures PTP_COLLECTION_MOUSE,%x\n", status));
					//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_REPORTMODE requested Mouse Input");
					deviceContext->PtpInputOn = FALSE;
					break;
				}
				case PTP_COLLECTION_WINDOWS:
				{
                    KdPrint(("PtpFilterSetHidFeatures PTP_COLLECTION_WINDOWS,%x\n", status));
					//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_REPORTMODE requested Windows PTP Input");
					deviceContext->PtpInputOn = TRUE;
					break;
				}
			}

			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_REPORTMODE is fulfilled");
			break;
		}
		case FAKE_REPORTID_FUNCTION_SWITCH:
		{
            KdPrint(("PtpFilterSetHidFeatures FAKE_REPORTID_FUNCTION_SWITCH,%x\n", status));
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Report REPORTID_FUNCSWITCH is requested");

			PPTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT InputSelection = (PPTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT)hidPacket->reportBuffer;
			deviceContext->PtpReportButton = InputSelection->ButtonReport;
			deviceContext->PtpReportTouch = InputSelection->SurfaceReport;


			break;
		}
		default:
		{
            KdPrint(("PtpFilterSetHidFeatures STATUS_NOT_SUPPORTED,%x\n", status));
			//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Unsupported type %d is requested", hidPacket->reportId);
			status = STATUS_NOT_SUPPORTED;
			goto exit;
		}
	}

exit:

    KdPrint(("PtpFilterSetHidFeatures end,%x\n", status));
	//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HID, "%!FUNC! Exit");
	return status;
}



VOID
PtpFilterInputProcessRequest(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;

	deviceContext = PtpFilterGetContext(Device);
	status = WdfRequestForwardToIoQueue(Request, deviceContext->HidReadQueue);
	if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterInputProcessRequest WdfRequestForwardToIoQueue failed,%x\n", status));
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! WdfRequestForwardToIoQueue fails, status = %!STATUS!", status);
		WdfRequestComplete(Request, status);
		return;
	}

	// Only issue request when fully configured.
	// Otherwise we will let power recovery process to triage it
	if (deviceContext->DeviceConfigured == TRUE) {
		PtpFilterInputIssueTransportRequest(Device);
	}
}

VOID
PtpFilterWorkItemCallback(
	_In_ WDFWORKITEM WorkItem
)
{
	WDFDEVICE Device = WdfWorkItemGetParentObject(WorkItem);
	PtpFilterInputIssueTransportRequest(Device);
}

VOID
PtpFilterInputIssueTransportRequest(
	_In_ WDFDEVICE Device
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDFREQUEST hidReadRequest;
	WDFMEMORY hidReadOutputMemory;
	PWORKER_REQUEST_CONTEXT requestContext;
	BOOLEAN requestStatus = FALSE;

	deviceContext = PtpFilterGetContext(Device);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, WORKER_REQUEST_CONTEXT);
	attributes.ParentObject = Device;
	status = WdfRequestCreate(&attributes, deviceContext->HidIoTarget, &hidReadRequest);
	if (!NT_SUCCESS(status)) {
		// This can fail for Bluetooth devices. We will set up a 3 second timer for retry triage.
		// Typically this should not fail for USB transport.
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestCreate fails, status = %!STATUS!", status);
        KdPrint(("PtpFilterInputIssueTransportRequest WdfRequestCreate failed,%x\n", status));
		deviceContext->DeviceConfigured = FALSE;
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		return;
	}

	status = WdfMemoryCreateFromLookaside(deviceContext->HidReadBufferLookaside, &hidReadOutputMemory);
	if (!NT_SUCCESS(status)) {
		// tbh if you fail here, something seriously went wrong...request a restart.
        
        KdPrint(("PtpFilterInputIssueTransportRequest WdfMemoryCreateFromLookaside failed,%x\n", status));
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!", status);
		WdfObjectDelete(hidReadRequest);
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	// Assign context information
	// And format HID read request.
	requestContext = WorkerRequestGetContext(hidReadRequest);
	requestContext->DeviceContext = deviceContext;
	requestContext->RequestMemory = hidReadOutputMemory;
	status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget, hidReadRequest,
													  IOCTL_HID_READ_REPORT, NULL, 0, hidReadOutputMemory, 0);
	if (!NT_SUCCESS(status)) {
		// tbh if you fail here, something seriously went wrong...request a restart.
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!", status);
        KdPrint(("PtpFilterInputIssueTransportRequest WdfIoTargetFormatRequestForInternalIoctl failed,%x\n", status));

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}

		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	// Set callback
	WdfRequestSetCompletionRoutine(hidReadRequest, PtpFilterInputRequestCompletionCallback, requestContext);

	requestStatus = WdfRequestSend(hidReadRequest, deviceContext->HidIoTarget, NULL);
	if (!requestStatus) {
		// Retry after 3 seconds, in case this is a transportation issue.
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! PtpFilterInputIssueTransportRequest request failed to sent");
        KdPrint(("PtpFilterInputIssueTransportRequest WdfRequestSend failed,%x\n", status));

		deviceContext->DeviceConfigured = FALSE;
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}
	}
}

VOID
PtpFilterInputRequestCompletionCallback(
	_In_ WDFREQUEST Request,
	_In_ WDFIOTARGET Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
	_In_ WDFCONTEXT Context
)
{
	PWORKER_REQUEST_CONTEXT requestContext;
	PDEVICE_CONTEXT deviceContext;
	NTSTATUS status = STATUS_SUCCESS;

	WDFREQUEST ptpRequest;
    struct mouse_report_t OutputReport;
    PTP_REPORT_DUET InputReport;
	WDFMEMORY  ptpRequestMemory;

	size_t responseLength;
	PUCHAR TouchDataBuffer;

	size_t OutputSize = sizeof(struct mouse_report_t);
    size_t InputSize = sizeof(PTP_REPORT_DUET);

	UNREFERENCED_PARAMETER(Target);

	requestContext = (PWORKER_REQUEST_CONTEXT)Context;
	deviceContext = requestContext->DeviceContext;
	responseLength = (size_t)(LONG)WdfRequestGetInformation(Request);
	TouchDataBuffer = WdfMemoryGetBuffer(Params->Parameters.Ioctl.Output.Buffer, NULL);

	RtlZeroMemory(&OutputReport, OutputSize);
    KdPrint(("PtpFilterInputRequestCompletionCallback start,%x\n", status));

	// Pre-flight check 1: if size is 0, this is not something we need. Ignore the read, and issue next request.
	if (responseLength <= 0) {
		WdfWorkItemEnqueue(requestContext->DeviceContext->HidTransportRecoveryWorkItem);
		goto cleanup;
	}

    KdPrint(("PtpFilterInputRequestCompletionCallback responseLength=,%x\n", (ULONG)responseLength));
    for (UINT32 i = 0; i < responseLength; i++) {
        KdPrint(("PtpFilterInputRequestCompletionCallback TouchDataBuffer[%x]=,%x\n", i,TouchDataBuffer[i]));
    }


	// Pre-flight check 2: the response size should be sane
	if (responseLength < InputSize) {
		KdPrint(("PtpFilterInputRequestCompletionCallback input received. Length err,%x\n", status));
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		goto cleanup;
	}

	// Read report and fulfill PTP request. If no report is found, just exit.
	status = WdfIoQueueRetrieveNextRequest(deviceContext->HidReadQueue, &ptpRequest);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterInputRequestCompletionCallback WdfIoQueueRetrieveNextRequest failed,%x\n", status));
		goto cleanup;
	}

    InputReport = *(PTP_REPORT_DUET*)TouchDataBuffer;
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ReportID =,%x\n", InputReport.ReportID));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ContactCount =,%x\n", InputReport.ContactCount));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.IsButtonClicked =,%x\n", InputReport.IsButtonClicked));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ScanTime =,%x\n", InputReport.ScanTime));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].Confidence =,%x\n", InputReport.Contacts[0].Confidence));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].TipSwitch =,%x\n", InputReport.Contacts[0].TipSwitch));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].ContactID =,%x\n", InputReport.Contacts[0].ContactID));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].XL =,%x\n", InputReport.Contacts[0].XL));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].XH =,%x\n", InputReport.Contacts[0].XH));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].YL =,%x\n", InputReport.Contacts[0].YL));
    KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].YH =,%x\n", InputReport.Contacts[0].YH));

	// Report header
    OutputReport.report_id = FAKE_REPORTID_MOUSE;
    OutputReport.dx  = 1;
    OutputReport.dy = 1;

	status = WdfRequestRetrieveOutputMemory(ptpRequest, &ptpRequestMemory);
	if (!NT_SUCCESS(status))
	{
		KdPrint(("PtpFilterInputRequestCompletionCallback WdfRequestRetrieveOutputMemory failed,%x\n", status));
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		goto cleanup;
	}

	status = WdfMemoryCopyFromBuffer(ptpRequestMemory, 0, (PVOID)&OutputReport, OutputSize);
	if (!NT_SUCCESS(status))
	{
		KdPrint(("PtpFilterInputRequestCompletionCallback WdfMemoryCopyFromBuffer failed,%x\n", status));
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		goto cleanup;
	}

	WdfRequestSetInformation(ptpRequest, OutputSize);
	WdfRequestComplete(ptpRequest, status);

cleanup:
	// Cleanup
	WdfObjectDelete(Request);
	if (requestContext->RequestMemory != NULL) {
		WdfObjectDelete(requestContext->RequestMemory);
	}

	KdPrint(("PtpFilterInputRequestCompletionCallback end,%x\n", status));
	// We don't issue new request here (unless it's a spurious request - which is handled earlier) to
	// keep the request pipe go through one-way.
}




VOID PtpFilterDiagnosticsInitializeContinuousRead(
	_In_ WDFDEVICE Device
)
{

	// Issue a request to underlying HID target. Then start to have a continus stream reading it
	PtpFilterDiagnosticsInputIssueRequest(Device);

}

VOID
PtpFilterDiagnosticsInputIssueRequest(
	_In_ WDFDEVICE Device
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;
	WDF_OBJECT_ATTRIBUTES attributes;
	BOOLEAN requestStatus = FALSE;
	WDFREQUEST hidReadRequest;
	WDFMEMORY hidReadOutputMemory;
	PWORKER_REQUEST_CONTEXT requestContext;

	deviceContext = PtpFilterGetContext(Device);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, WORKER_REQUEST_CONTEXT);
	attributes.ParentObject = Device;

	status = WdfRequestCreate(&attributes, deviceContext->HidIoTarget, &hidReadRequest);
	if (!NT_SUCCESS(status))
	{
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestCreate fails, status = %!STATUS!", status);
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_US(200));
		return;
	}

	status = WdfMemoryCreateFromLookaside(deviceContext->HidReadBufferLookaside, &hidReadOutputMemory);
	if (!NT_SUCCESS(status))
	{
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!", status);
		WdfObjectDelete(hidReadRequest);
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_US(200));
		return;
	}

	// Assign context information
	requestContext = WorkerRequestGetContext(hidReadRequest);
	requestContext->DeviceContext = deviceContext;
	requestContext->RequestMemory = hidReadOutputMemory;

	// Invoke HID read request to the device.
	status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget, hidReadRequest,
													  IOCTL_HID_READ_REPORT, NULL, 0, hidReadOutputMemory, 0);

	if (!NT_SUCCESS(status))
	{
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!", status);

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}

		return;
	}

	WdfRequestSetCompletionRoutine(
		hidReadRequest,
		PtpFilterDiagnosticsRequestCompletionRoutine,
		requestContext
	);

	requestStatus = WdfRequestSend(
		hidReadRequest,
		deviceContext->HidIoTarget,
		NULL
	);

	if (!requestStatus)
	{
		//TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! AmtPtpSpiInputRoutineWorker request failed to sent");
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_US(50));

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}
	}
}

VOID
PtpFilterDiagnosticsRequestCompletionRoutine(
	_In_ WDFREQUEST Request,
	_In_ WDFIOTARGET Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
	_In_ WDFCONTEXT Context
)
{
	PWORKER_REQUEST_CONTEXT requestContext;
	PDEVICE_CONTEXT deviceContext;
	LONG responseLength;

	UNREFERENCED_PARAMETER(Request);
	UNREFERENCED_PARAMETER(Target);
	UNREFERENCED_PARAMETER(Params);

	requestContext = (PWORKER_REQUEST_CONTEXT)Context;
	deviceContext = requestContext->DeviceContext;

	// Decode request
	responseLength = (LONG)WdfRequestGetInformation(Request);
	if (responseLength > 0) {
		//TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT, "Request received with size %d", responseLength);
	}

	// Cleanup
	WdfObjectDelete(Request);
	if (requestContext->RequestMemory != NULL) {
		WdfObjectDelete(requestContext->RequestMemory);
	}

	// Issue next request
	PtpFilterDiagnosticsInputIssueRequest(requestContext->DeviceContext->Device);
}

PCHAR
PtpFilterDiagnosticsIoControlGetString(
	_In_ ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
		case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
			return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
		case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
			return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
		case IOCTL_HID_GET_REPORT_DESCRIPTOR:
			return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
		case IOCTL_HID_GET_STRING:
			return "IOCTL_HID_GET_STRING";
		case IOCTL_HID_READ_REPORT:
			return "IOCTL_HID_READ_REPORT";
		case IOCTL_HID_WRITE_REPORT:
			return "IOCTL_HID_WRITE_REPORT";
		case IOCTL_UMDF_HID_GET_INPUT_REPORT:
			return "IOCTL_UMDF_HID_GET_INPUT_REPORT";
		case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
			return "IOCTL_UMDF_HID_SET_OUTPUT_REPORT";
		case IOCTL_UMDF_HID_GET_FEATURE:
			return "IOCTL_UMDF_HID_GET_FEATURE";
		case IOCTL_UMDF_HID_SET_FEATURE:
			return "IOCTL_UMDF_HID_SET_FEATURE";
		case IOCTL_HID_ACTIVATE_DEVICE:
			return "IOCTL_HID_ACTIVATE_DEVICE";
		case IOCTL_HID_DEACTIVATE_DEVICE:
			return "IOCTL_HID_DEACTIVATE_DEVICE";
		case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
			return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
		case IOCTL_HID_GET_FEATURE:
			return "IOCTL_HID_GET_FEATURE";
		case IOCTL_HID_SET_FEATURE:
			return "IOCTL_HID_SET_FEATURE";
		default:
			return "IOCTL_UNKNOWN";
	}
}

