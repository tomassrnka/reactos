/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/context.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"

NTSTATUS NTAPI
AfdGetContext( PDEVICE_OBJECT DeviceObject, PIRP Irp,
               PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    UINT ContextSize = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    /* METHOD_NEITHER: Irp->UserBuffer is the raw user output pointer. Probe it
     * for the write (user-mode requests) and copy under SEH so a kernel address
     * or a concurrently-unmapped buffer cannot become a controlled kernel write. */
    _SEH2_TRY {
        if (Irp->RequestorMode != KernelMode && ContextSize != 0)
            ProbeForWrite(Irp->UserBuffer, ContextSize, sizeof(CHAR));

        if (ContextSize == sizeof(FCB->SharedData))
        {
            RtlCopyMemory(Irp->UserBuffer,
                          &FCB->SharedData,
                          ContextSize);
            Status = STATUS_SUCCESS;
        }
        else if (FCB->Context)
        {
            ContextSize = min(ContextSize, FCB->ContextSize);
            RtlCopyMemory( Irp->UserBuffer,
                           FCB->Context,
                           ContextSize );
            Status = STATUS_SUCCESS;
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        AFD_DbgPrint(MIN_TRACE,("Exception writing context\n"));
        Status = STATUS_INVALID_PARAMETER;
        ContextSize = 0;
    } _SEH2_END;

    AFD_DbgPrint(MID_TRACE,("Returning %x\n", Status));

    return UnlockAndMaybeComplete( FCB, Status, Irp, ContextSize );
}

NTSTATUS NTAPI
AfdGetContextSize( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                   PIO_STACK_LOCATION IrpSp )
{
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
    {
        AFD_DbgPrint(MIN_TRACE,("Buffer too small\n"));
        return UnlockAndMaybeComplete(FCB, STATUS_BUFFER_TOO_SMALL, Irp, sizeof(ULONG));
    }

    /* METHOD_NEITHER: probe the raw user output pointer and copy under SEH. */
    _SEH2_TRY {
        if (Irp->RequestorMode != KernelMode)
            ProbeForWrite(Irp->UserBuffer, sizeof(ULONG), sizeof(ULONG));
        RtlCopyMemory(Irp->UserBuffer,
                      &FCB->ContextSize,
                      sizeof(ULONG));
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        AFD_DbgPrint(MIN_TRACE,("Exception writing context size\n"));
        Status = STATUS_INVALID_PARAMETER;
    } _SEH2_END;

    return UnlockAndMaybeComplete(FCB, Status, Irp, NT_SUCCESS(Status) ? sizeof(ULONG) : 0);
}

NTSTATUS NTAPI
AfdSetContext( PDEVICE_OBJECT DeviceObject, PIRP Irp,
               PIO_STACK_LOCATION IrpSp ) {
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PVOID Context = LockRequest(Irp, IrpSp, FALSE, NULL);

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if (!Context)
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

    if( FCB->Context ) {
        ExFreePoolWithTag(FCB->Context, TAG_AFD_SOCKET_CONTEXT);
        FCB->ContextSize = 0;
    }

    FCB->Context = ExAllocatePoolWithTag(PagedPool,
                                         IrpSp->Parameters.DeviceIoControl.InputBufferLength,
                                         TAG_AFD_SOCKET_CONTEXT);

    if( !FCB->Context ) return UnlockAndMaybeComplete( FCB, STATUS_NO_MEMORY, Irp, 0 );

    FCB->ContextSize = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

    RtlCopyMemory( FCB->Context,
                   Context,
                   FCB->ContextSize );

    return UnlockAndMaybeComplete( FCB, STATUS_SUCCESS, Irp, 0 );
}
