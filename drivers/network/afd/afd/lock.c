/* $Id$
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/lock.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */
#include "afd.h"
#include "tdi_proto.h"
#include "tdiconn.h"
#include "debug.h"
#include "pseh/pseh.h"

/* Lock a method_neither request so it'll be available from DISPATCH_LEVEL */
PVOID LockRequest( PIRP Irp, PIO_STACK_LOCATION IrpSp ) {
    BOOLEAN LockFailed = FALSE;

    Irp->MdlAddress =
	IoAllocateMdl( IrpSp->Parameters.DeviceIoControl.Type3InputBuffer,
		       IrpSp->Parameters.DeviceIoControl.InputBufferLength,
		       FALSE,
		       FALSE,
		       NULL );
    if( Irp->MdlAddress ) {
	_SEH_TRY {
	    MmProbeAndLockPages( Irp->MdlAddress, KernelMode, IoModifyAccess );
	} _SEH_HANDLE {
	    LockFailed = TRUE;
	} _SEH_END;

	if( LockFailed ) {
	    IoFreeMdl( Irp->MdlAddress );
	    Irp->MdlAddress = NULL;
	    return NULL;
	}

	IrpSp->Parameters.DeviceIoControl.Type3InputBuffer =
	    MmGetSystemAddressForMdlSafe( Irp->MdlAddress, NormalPagePriority );

	if( !IrpSp->Parameters.DeviceIoControl.Type3InputBuffer ) {
	    IoFreeMdl( Irp->MdlAddress );
	    Irp->MdlAddress = NULL;
	    return NULL;
	}    

	return IrpSp->Parameters.DeviceIoControl.Type3InputBuffer;
    } else return NULL;
}

VOID UnlockRequest( PIRP Irp, PIO_STACK_LOCATION IrpSp ) {
    PVOID Buffer = MmGetSystemAddressForMdlSafe( Irp->MdlAddress, NormalPagePriority );
    if( IrpSp->Parameters.DeviceIoControl.Type3InputBuffer == Buffer || Buffer == NULL ) {
	MmUnmapLockedPages( IrpSp->Parameters.DeviceIoControl.Type3InputBuffer, Irp->MdlAddress );
        MmUnlockPages( Irp->MdlAddress );
        IoFreeMdl( Irp->MdlAddress );
    }

    Irp->MdlAddress = NULL;
}

/* Note: We add an extra buffer if LockAddress is true.  This allows us to
 * treat the address buffer as an ordinary client buffer.  It's only used
 * for datagrams. */

PAFD_WSABUF LockBuffers( PAFD_WSABUF Buf, UINT Count,
			 PVOID AddressBuf, PINT AddressLen,
			 BOOLEAN Write, BOOLEAN LockAddress ) {
    UINT i;
    /* Copy the buffer array so we don't lose it */
    UINT Lock = (LockAddress && AddressLen) ? 2 : 0;
    UINT Size = sizeof(AFD_WSABUF) * (Count + Lock);
    PAFD_WSABUF NewBuf = ExAllocatePool( PagedPool, Size * 2 );
    PMDL NewMdl;
    BOOLEAN LockFailed = FALSE;

    AFD_DbgPrint(MID_TRACE,("Called(%08x)\n", NewBuf));

    if( NewBuf ) {
	PAFD_MAPBUF MapBuf = (PAFD_MAPBUF)(NewBuf + Count + Lock);

        _SEH_TRY {
            RtlCopyMemory( NewBuf, Buf, sizeof(AFD_WSABUF) * Count );
            if( LockAddress ) {
                NewBuf[Count].buf = AddressBuf;
                NewBuf[Count].len = *AddressLen;
                Count++;
                NewBuf[Count].buf = (PVOID)AddressLen;
                NewBuf[Count].len = sizeof(*AddressLen);
                Count++;
            }
        } _SEH_HANDLE {
            AFD_DbgPrint(MIN_TRACE,("Access violation copying buffer info "
                                    "from userland (%x %x)\n",
                                    Buf, AddressLen));
            ExFreePool( NewBuf );
            _SEH_YIELD(return NULL);
        } _SEH_END;

	for( i = 0; i < Count; i++ ) {
	    AFD_DbgPrint(MID_TRACE,("Locking buffer %d (%x:%d)\n",
				    i, NewBuf[i].buf, NewBuf[i].len));

	    if( NewBuf[i].len ) {
		NewMdl = IoAllocateMdl( NewBuf[i].buf,
					NewBuf[i].len,
					FALSE,
					FALSE,
					NULL );
	    } else {
		MapBuf[i].Mdl = NULL;
		continue;
	    }

	    AFD_DbgPrint(MID_TRACE,("NewMdl @ %x\n", NewMdl));

	    MapBuf[i].Mdl = NewMdl;

	    if( MapBuf[i].Mdl ) {
		AFD_DbgPrint(MID_TRACE,("Probe and lock pages\n"));
		_SEH_TRY {
		    MmProbeAndLockPages( MapBuf[i].Mdl, KernelMode,
				         Write ? IoModifyAccess : IoReadAccess );
		} _SEH_HANDLE {
		    LockFailed = TRUE;
		} _SEH_END;
		AFD_DbgPrint(MID_TRACE,("MmProbeAndLock finished\n"));

		if( LockFailed ) {
		    IoFreeMdl( MapBuf[i].Mdl );
		    MapBuf[i].Mdl = NULL;
		    ExFreePool( NewBuf );
		    return NULL;
		}
	    } else {
		ExFreePool( NewBuf );
		return NULL;
	    }     
	}
    }

    AFD_DbgPrint(MID_TRACE,("Leaving %x\n", NewBuf));

    return NewBuf;
}

VOID UnlockBuffers( PAFD_WSABUF Buf, UINT Count, BOOL Address ) {
    UINT Lock = Address ? 2 : 0;
    PAFD_MAPBUF Map = (PAFD_MAPBUF)(Buf + Count + Lock);
    UINT i;

    if( !Buf ) return;

    for( i = 0; i < Count + Lock; i++ ) {
	if( Map[i].Mdl ) {
	    MmUnlockPages( Map[i].Mdl );
	    IoFreeMdl( Map[i].Mdl );
	    Map[i].Mdl = NULL;
	}
    }

    ExFreePool( Buf );
    Buf = NULL;
}

/* Produce a kernel-land handle array with handles replaced by object
 * pointers.  This will allow the system to do proper alerting */
PAFD_HANDLE LockHandles( PAFD_HANDLE HandleArray, UINT HandleCount ) {
    UINT i;
    NTSTATUS Status;

    PAFD_HANDLE FileObjects = ExAllocatePool
	( NonPagedPool, HandleCount * sizeof(AFD_HANDLE) );

    for( i = 0; FileObjects && i < HandleCount; i++ ) {
	HandleArray[i].Status = 0;
	HandleArray[i].Events = HandleArray[i].Events;
        FileObjects[i].Handle = 0;
	Status = ObReferenceObjectByHandle
	    ( (PVOID)HandleArray[i].Handle,
	      FILE_ALL_ACCESS,
	      NULL,
	      KernelMode,
	      (PVOID*)&FileObjects[i].Handle,
	      NULL );
    }

    return FileObjects;
}

VOID UnlockHandles( PAFD_HANDLE HandleArray, UINT HandleCount ) {
    UINT i;

    for( i = 0; i < HandleCount; i++ ) {
	if( HandleArray[i].Handle )
	    ObDereferenceObject( (PVOID)HandleArray[i].Handle );
    }

    ExFreePool( HandleArray );
}

/* Returns transitioned state or SOCKET_STATE_INVALID_TRANSITION */
UINT SocketAcquireStateLock( PAFD_FCB FCB ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID CurrentThread = KeGetCurrentThread();

    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);

    AFD_DbgPrint(MAX_TRACE,("Called on %x, attempting to lock\n", FCB));

    /* Wait for the previous user to unlock the FCB state.  There might be
     * multiple waiters waiting to change the state.  We need to check each
     * time we get the event whether somebody still has the state locked */

    if( !FCB ) return FALSE;

    if( CurrentThread == FCB->CurrentThread ) {
	FCB->LockCount++;
	AFD_DbgPrint(MID_TRACE,
		     ("Same thread, lock count %d\n", FCB->LockCount));
	return TRUE;
    } else {
	AFD_DbgPrint(MID_TRACE,
		     ("Thread %x opposes lock thread %x\n",
		      CurrentThread, FCB->CurrentThread));
    }


    ExAcquireFastMutex( &FCB->Mutex );

    while( FCB->Locked ) {
	AFD_DbgPrint
	    (MID_TRACE,("FCB %x is locked, waiting for notification\n",
			FCB));
	ExReleaseFastMutex( &FCB->Mutex );
	Status = KeWaitForSingleObject( &FCB->StateLockedEvent,
					UserRequest,
					KernelMode,
					FALSE,
					NULL );
	ExAcquireFastMutex( &FCB->Mutex );
    }
    FCB->Locked = TRUE;
    FCB->CurrentThread = CurrentThread;
    FCB->LockCount++;
    ExReleaseFastMutex( &FCB->Mutex );

    AFD_DbgPrint(MAX_TRACE,("Got lock (%d).\n", FCB->LockCount));

    return TRUE;
}

VOID SocketStateUnlock( PAFD_FCB FCB ) {
#ifdef DBG
    PVOID CurrentThread = KeGetCurrentThread();
#endif
    ASSERT(FCB->LockCount > 0);
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);

    ExAcquireFastMutex( &FCB->Mutex );
    FCB->LockCount--;

    if( !FCB->LockCount ) {
	FCB->CurrentThread = NULL;
	FCB->Locked = FALSE;

	AFD_DbgPrint(MAX_TRACE,("Unlocked.\n"));
	KePulseEvent( &FCB->StateLockedEvent, IO_NETWORK_INCREMENT, FALSE );
    } else {
	AFD_DbgPrint(MAX_TRACE,("New lock count: %d (Thr: %x)\n",
				FCB->LockCount, CurrentThread));
    }
    ExReleaseFastMutex( &FCB->Mutex );
}

NTSTATUS NTAPI UnlockAndMaybeComplete
( PAFD_FCB FCB, NTSTATUS Status, PIRP Irp,
  UINT Information,
  PIO_COMPLETION_ROUTINE Completion ) {

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;

    if( Status == STATUS_PENDING ) {
	/* We should firstly mark this IRP as pending, because
	   otherwise it may be completed by StreamSocketConnectComplete()
	   before we return from SocketStateUnlock(). */
	IoMarkIrpPending( Irp );
	SocketStateUnlock( FCB );
    } else {
	if ( Irp->MdlAddress ) UnlockRequest( Irp, IoGetCurrentIrpStackLocation( Irp ) );
	SocketStateUnlock( FCB );
	if( Completion )
	    Completion( FCB->DeviceExt->DeviceObject, Irp, FCB );
	IoCompleteRequest( Irp, IO_NETWORK_INCREMENT );
    }
    return Status;
}


NTSTATUS LostSocket( PIRP Irp ) {
    NTSTATUS Status = STATUS_FILE_CLOSED;
    AFD_DbgPrint(MIN_TRACE,("Called.\n"));
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = Status;
    if ( Irp->MdlAddress ) UnlockRequest( Irp, IoGetCurrentIrpStackLocation( Irp ) );
    IoCompleteRequest( Irp, IO_NO_INCREMENT );
    return Status;
}

NTSTATUS LeaveIrpUntilLater( PAFD_FCB FCB, PIRP Irp, UINT Function ) {
    InsertTailList( &FCB->PendingIrpList[Function],
		    &Irp->Tail.Overlay.ListEntry );
    return UnlockAndMaybeComplete( FCB, STATUS_PENDING, Irp, 0, NULL );
}

VOID SocketCalloutEnter( PAFD_FCB FCB ) {
    ASSERT(!FCB->Critical);
    ASSERT(FCB->Locked);
    FCB->Critical = TRUE;
    SocketStateUnlock( FCB );
}

VOID SocketCalloutLeave( PAFD_FCB FCB ) {
    ASSERT(FCB->Critical);
    ASSERT(!FCB->Locked);
    FCB->Critical = FALSE;
    SocketAcquireStateLock( FCB );
}
