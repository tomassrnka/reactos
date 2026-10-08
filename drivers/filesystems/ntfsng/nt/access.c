/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Access checks of opens, creates, deletes and renames
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

/*
 * The object manager does not check file descriptors (a new file object takes its create-handle
 * path), so the file system does, the way NT file systems do: against the descriptor of the file
 * on every open, against the directory's for creates, with FILE_DELETE_CHILD and
 * FILE_LIST_DIRECTORY on the directory implying DELETE and FILE_READ_ATTRIBUTES on its entries.
 * Kernel-mode opens without SL_FORCE_ACCESS_CHECK are trusted.  Rights granted through the
 * backup and restore privileges arrive in PreviouslyGrantedAccess (the I/O manager checks them).
 */

BOOLEAN NgCreateChecksAccess(PIRP Irp, PIO_STACK_LOCATION Stack)
{
    return (Irp->RequestorMode != KernelMode || (Stack->Flags & SL_FORCE_ACCESS_CHECK)) &&
           Stack->Parameters.Create.SecurityContext->AccessState != NULL;
}

/* SeAccessCheck for a locked subject; used privileges go into the access state if As is given. */
static BOOLEAN NgSeCheck(PSECURITY_DESCRIPTOR Sd, PSECURITY_SUBJECT_CONTEXT Subject, PACCESS_STATE As,
                         ACCESS_MASK Desired, ACCESS_MASK Previous, PACCESS_MASK Granted, PNTSTATUS Status)
{
    PPRIVILEGE_SET Privileges = NULL;
    BOOLEAN Ok;

    *Granted = 0;
    Ok = SeAccessCheck(Sd, Subject, TRUE, Desired, Previous, &Privileges, IoGetFileObjectGenericMapping(),
                       UserMode, Granted, Status);
    if (Privileges)
    {
        if (As && Ok)
            SeAppendPrivileges(As, Privileges);
        SeFreePrivileges(Privileges);
    }
    if (!Ok)
    {
        *Granted = 0;
        if (NT_SUCCESS(*Status))
            *Status = STATUS_ACCESS_DENIED;
    }
    return Ok;
}

static BOOLEAN NgSdGrants(PSECURITY_DESCRIPTOR Sd, PSECURITY_SUBJECT_CONTEXT Subject, ACCESS_MASK Right)
{
    ACCESS_MASK Granted;
    NTSTATUS Status;
    return Sd && NgSeCheck(Sd, Subject, NULL, Right, 0, &Granted, &Status);
}

/* Records a successful check in the access state: the handle gets PreviouslyGrantedAccess. */
static VOID NgGrant(PACCESS_STATE As, ACCESS_MASK Granted)
{
    As->PreviouslyGrantedAccess |= Granted;
    As->RemainingDesiredAccess &= ~(Granted | MAXIMUM_ALLOWED);
}

NTSTATUS NgCheckExistingAccess(PACCESS_STATE As, PSECURITY_DESCRIPTOR Sd, PSECURITY_DESCRIPTOR ParentSd,
                               ACCESS_MASK Implied)
{
    PSECURITY_SUBJECT_CONTEXT Subject = &As->SubjectSecurityContext;
    ACCESS_MASK Desired = As->RemainingDesiredAccess, Previous = As->PreviouslyGrantedAccess;
    ACCESS_MASK Granted = 0, FromParent = 0;
    BOOLEAN Maximum;
    NTSTATUS Status;

    RtlMapGenericMask(&Desired, IoGetFileObjectGenericMapping());
    Maximum = (Desired & MAXIMUM_ALLOWED) != 0;
    Implied &= ~(Desired | Previous);
    SeLockSubjectContext(Subject);
    if (!NgSeCheck(Sd, Subject, As, Desired | Implied, Previous, &Granted, &Status) || Maximum)
    {
        if (ParentSd)
        {
            if ((Desired & (DELETE | MAXIMUM_ALLOWED)) && !(Previous & DELETE) && !(Granted & DELETE) &&
                NgSdGrants(ParentSd, Subject, FILE_DELETE_CHILD))
                FromParent |= DELETE;
            if ((Desired & (FILE_READ_ATTRIBUTES | MAXIMUM_ALLOWED)) && !(Previous & FILE_READ_ATTRIBUTES) &&
                !(Granted & FILE_READ_ATTRIBUTES) && NgSdGrants(ParentSd, Subject, FILE_LIST_DIRECTORY))
                FromParent |= FILE_READ_ATTRIBUTES;
        }
        if (!NT_SUCCESS(Status) && FromParent)
        {
            ACCESS_MASK Rest = (Desired | Implied) & ~FromParent;
            if (Rest)
                NgSeCheck(Sd, Subject, As, Rest, Previous | FromParent, &Granted, &Status);
            else
            {
                Granted = Previous | FromParent;
                Status = STATUS_SUCCESS;
            }
        }
        if (NT_SUCCESS(Status))
            Granted |= FromParent;
    }
    SeUnlockSubjectContext(Subject);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!Maximum)
        Granted &= ~Implied;
    NgGrant(As, Granted);
    return STATUS_SUCCESS;
}

NTSTATUS NgCheckAccessRight(PACCESS_STATE As, PSECURITY_DESCRIPTOR Sd, ACCESS_MASK Right)
{
    BOOLEAN Ok;
    SeLockSubjectContext(&As->SubjectSecurityContext);
    Ok = NgSdGrants(Sd, &As->SubjectSecurityContext, Right);
    SeUnlockSubjectContext(&As->SubjectSecurityContext);
    return Ok ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}

NTSTATUS NgCheckCreateAccess(PACCESS_STATE As, PSECURITY_DESCRIPTOR ParentSd, BOOLEAN IsDir)
{
    /* The restore privilege (backup intent) creates anywhere. */
    if (As->Flags & TOKEN_HAS_RESTORE_PRIVILEGE)
        return STATUS_SUCCESS;
    return NgCheckAccessRight(As, ParentSd, IsDir ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE);
}

VOID NgGrantNewFile(PACCESS_STATE As)
{
    ACCESS_MASK Desired = As->RemainingDesiredAccess;
    RtlMapGenericMask(&Desired, IoGetFileObjectGenericMapping());
    if (Desired & MAXIMUM_ALLOWED)
        Desired = (Desired & ~MAXIMUM_ALLOWED) | FILE_ALL_ACCESS;
    /* ACCESS_SYSTEM_SECURITY stays for the object manager's privilege check. */
    NgGrant(As, Desired & ~ACCESS_SYSTEM_SECURITY);
}

NTSTATUS NgCheckDeleteEntry(PSECURITY_DESCRIPTOR Sd, PSECURITY_DESCRIPTOR DirSd)
{
    SECURITY_SUBJECT_CONTEXT Subject;
    BOOLEAN Ok;

    SeCaptureSubjectContext(&Subject);
    SeLockSubjectContext(&Subject);
    Ok = NgSdGrants(DirSd, &Subject, FILE_DELETE_CHILD) || NgSdGrants(Sd, &Subject, DELETE);
    SeUnlockSubjectContext(&Subject);
    SeReleaseSubjectContext(&Subject);
    return Ok ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}
