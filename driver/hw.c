#include "hw.h"

#include "modules.h"

#define PCI_VENDOR_ID_OFFSET 0x00

/*
 * Every PCI device has a set of registers commonly referred to as the PCI configuration space. In
 * modern PCI-e devices an extended configuration space was implemented. These configuration spaces
 * are mapped into main memory and this allows us to read/write to the registers.
 *
 * The configuration space consists of a standard header, containing information such as the
 * DeviceID, VendorID, Status and so on. Below is the header schema including offsets.
 *
 *  | Offset 0x00: Header Type
 *  | Offset 0x01: Multi-Function Device Indicator
 *  | Offset 0x02: Device ID (Low Byte)
 *  | Offset 0x03: Device ID (High Byte)
 *  | Offset 0x04: Status Register (16 bits)
 *  | Offset 0x06: Command Register (16 bits)
 *  | Offset 0x08: Class Code
 *  | Offset 0x09: Subclass Code
 *  | Offset 0x0A: Prog IF (Programming Interface)
 *  | Offset 0x0B: Revision ID
 *  | Offset 0x0C: BIST (Built-in Self-Test)
 *  | Offset 0x0D: Header Type (Secondary)
 *  | Offset 0x0E: Latency Timer
 *  | Offset 0x0F: Cache Line Size
 *  | Offset 0x10: Base Address Register 0 (BAR0) - 32 bits
 *  | Offset 0x14: Base Address Register 1 (BAR1) - 32 bits
 *  | Offset 0x18: Base Address Register 2 (BAR2) - 32 bits
 *  | Offset 0x1C: Base Address Register 3 (BAR3) - 32 bits
 *  | Offset 0x20: Base Address Register 4 (BAR4) - 32 bits
 *  | Offset 0x24: Base Address Register 5 (BAR5) - 32 bits
 *  | Offset 0x28: Cardbus CIS Pointer (for Cardbus bridges)
 *  | Offset 0x2C: Subsystem Vendor ID
 *  | Offset 0x2E: Subsystem ID
 *  | Offset 0x30: Expansion ROM Base Address
 *  | Offset 0x34: Reserved
 *  | Offset 0x38: Reserved
 *  | Offset 0x3C: Max_Lat (Maximum Latency)
 *  | Offset 0x3D: Min_Gnt (Minimum Grant)
 *  | Offset 0x3E: Interrupt Pin
 *  | Offset 0x3F: Interrupt Line
 *
 * We can use this to then query important information from PCI devices within the device tree. To
 * keep up with modern windows kernel programming, we can make use of the IRP_MN_READ_CONFIG code,
 * which as the name suggests, reads from a PCI devices configuration space.
 */
STATIC
NTSTATUS
QueryPciDeviceConfigurationSpace(_In_ PDEVICE_OBJECT DeviceObject,
                                 _In_ UINT32         Offset,
                                 _Out_ PVOID         Buffer,
                                 _In_ UINT32         BufferLength)
{
        NTSTATUS           status            = STATUS_UNSUCCESSFUL;
        KEVENT             event             = {0};
        IO_STATUS_BLOCK    io                = {0};
        PIRP               irp               = NULL;
        PIO_STACK_LOCATION io_stack_location = NULL;

        if (BufferLength == 0)
                return STATUS_BUFFER_TOO_SMALL;

        KeInitializeEvent(&event, NotificationEvent, FALSE);

        irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, DeviceObject, NULL, 0, NULL, &event, &io);

        if (!irp)
        {
                DEBUG_ERROR("IoBuildSynchronousFsdRequest failed with no status.");
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        io_stack_location                                        = IoGetNextIrpStackLocation(irp);
        io_stack_location->MinorFunction                         = IRP_MN_READ_CONFIG;
        io_stack_location->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
        io_stack_location->Parameters.ReadWriteConfig.Offset     = Offset;
        io_stack_location->Parameters.ReadWriteConfig.Buffer     = Buffer;
        io_stack_location->Parameters.ReadWriteConfig.Length     = BufferLength;

        status = IoCallDriver(DeviceObject, irp);

        if (status = STATUS_PENDING)
        {
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                status = io.Status;
        }

        if (!NT_SUCCESS(status))
        {
                DEBUG_ERROR("Failed to read configuration space with status %x", status);
                return status;
        }

        return status;
}

/*
 * NOTE: Caller is responsible for freeing the array.
 */
STATIC
NTSTATUS
EnumerateDriverObjectDeviceObjects(_In_ PDRIVER_OBJECT    DriverObject,
                                   _Out_ PDEVICE_OBJECT** DeviceObjectArray,
                                   _Out_ PUINT32          ArrayEntries)
{
        NTSTATUS        status       = STATUS_UNSUCCESSFUL;
        UINT32          object_count = 0;
        PDEVICE_OBJECT* buffer       = NULL;
        UINT32          buffer_size  = 0;

        *DeviceObjectArray = NULL;

        status = IoEnumerateDeviceObjectList(DriverObject, NULL, 0, &object_count);

        if (status != STATUS_BUFFER_TOO_SMALL)
        {
                DEBUG_ERROR("IoEnumerateDeviceObjectList failed with status %x", status);
                return status;
        }

        buffer_size = object_count * sizeof(UINT64);
        buffer      = ExAllocatePool2(POOL_FLAG_NON_PAGED, buffer_size, POOL_TAG_HW);

        if (!buffer)
                return STATUS_INSUFFICIENT_RESOURCES;

        status = IoEnumerateDeviceObjectList(DriverObject, buffer, buffer_size, &object_count);

        if (!NT_SUCCESS(status))
        {
                DEBUG_ERROR("IoEnumerateDeviceObjectList failed with status %x", status);
                ExFreePoolWithTag(buffer, POOL_TAG_HW);
                return status;
        }

        DEBUG_VERBOSE("EnumerateDriverObjectDeviceObjects: Object Count: %lx", object_count);

        *DeviceObjectArray = buffer;
        *ArrayEntries      = object_count;

        return status;
}

/*
 * While this isnt a perfect check to determine whether a DEVICE_OBJECT is indeed a PDO or FDO, this
 * is Peters preferred method... hence it is now my preferred method... :smiling_imp:
 */
STATIC
BOOLEAN
IsDeviceObjectValidPdo(_In_ PDEVICE_OBJECT DeviceObject)
{
        return DeviceObject->Flags & DO_BUS_ENUMERATED_DEVICE ? TRUE : FALSE;
}

/*
 * Windows splits DEVICE_OBJECTS up into 2 categories:
 *
 * Physical Device Object (PDO)
 * Functional Device Object (FDO)
 *
 * A PDO represents each device that is connected to a physical bus. Each PDO has an associated
 * DEVICE_NODE. An FDO represents the functionality of the device. Its how the system interacts with
 * the device objects.
 *
 * More information can be found here:
 * https://learn.microsoft.com/en-gb/windows-hardware/drivers/gettingstarted/device-nodes-and-device-stacks
 *
 * A device stack can have multiple PDO's, but can only have one FDO. This means to access each PCI
 * device on the system, we can enumerate all device objects given the PCI FDO which is called
 * pci.sys.
 */
NTSTATUS
EnumeratePciDeviceObjects()
{
        NTSTATUS        status                   = STATUS_UNSUCCESSFUL;
        UNICODE_STRING  pci                      = RTL_CONSTANT_STRING(L"\\Driver\\pci");
        PDRIVER_OBJECT  pci_driver_object        = NULL;
        PDEVICE_OBJECT* pci_device_objects       = NULL;
        PDEVICE_OBJECT  current_device           = NULL;
        UINT32          pci_device_objects_count = 0;
        USHORT          vendor_id                = 0;

        status = GetDriverObjectByDriverName(&pci, &pci_driver_object);

        if (!NT_SUCCESS(status))
        {
                DEBUG_ERROR("GetDriverObjectByDriverName failed with status %x", status);
                return status;
        }

        status = EnumerateDriverObjectDeviceObjects(
            pci_driver_object, &pci_device_objects, &pci_device_objects_count);

        if (!NT_SUCCESS(status))
        {
                DEBUG_ERROR("EnumerateDriverObjectDeviceObjects failed with status %x", status);
                return status;
        }

        for (UINT32 index = 0; index < pci_device_objects_count; index++)
        {
                current_device = pci_device_objects[index];

                /* make sure we have a valid PDO */
                if (!IsDeviceObjectValidPdo(current_device))
                        continue;

                status = QueryPciDeviceConfigurationSpace(
                    current_device, PCI_VENDOR_ID_OFFSET, &vendor_id, sizeof(USHORT));

                if (!NT_SUCCESS(status))
                {
                        DEBUG_ERROR("QueryPciDeviceConfigurationSpace failed with status %x",
                                    status);
                        continue;
                }

                DEBUG_VERBOSE("Device: %llx, VendorID: %lx", current_device, vendor_id);
        }

end:
        if (pci_device_objects)
                ExFreePoolWithTag(pci_device_objects, POOL_TAG_HW);

        return status;
}