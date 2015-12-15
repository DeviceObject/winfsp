/**
 * @file dll/loop.c
 *
 * @copyright 2015 Bill Zissimopoulos
 */

#include <dll/library.h>

typedef struct _FSP_WORK_ITEM
{
    FSP_FILE_SYSTEM *FileSystem;
    __declspec(align(MEMORY_ALLOCATION_ALIGNMENT)) UINT8 RequestBuf[];
} FSP_WORK_ITEM;

FSP_API NTSTATUS FspFileSystemCreate(FSP_FILE_SYSTEM_PR *ProcessRequest,
    FSP_FILE_SYSTEM **PFileSystem)
{
    FSP_FILE_SYSTEM *FileSystem;

    *PFileSystem = 0;

    if (0 == ProcessRequest)
        return STATUS_INVALID_PARAMETER;

    FileSystem = malloc(sizeof *FileSystem);
    if (0 == FileSystem)
        return STATUS_INSUFFICIENT_RESOURCES;

    memset(FileSystem, 0, sizeof *FileSystem);
    FileSystem->ProcessRequest = ProcessRequest;

    *PFileSystem = FileSystem;

    return STATUS_SUCCESS;
}

FSP_API VOID FspFileSystemDelete(FSP_FILE_SYSTEM *FileSystem)
{
    free(FileSystem);
}

FSP_API NTSTATUS FspFileSystemLoop(FSP_FILE_SYSTEM *FileSystem)
{
    NTSTATUS Result;
    PUINT8 RequestBuf, RequestBufEnd;
    SIZE_T RequestBufSize;
    FSP_FSCTL_TRANSACT_REQ *Request, *NextRequest;

    RequestBuf = malloc(FSP_FSCTL_TRANSACT_REQ_BUFFER_SIZEMIN);
    if (0 == RequestBuf)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (;;)
    {
        RequestBufSize = FSP_FSCTL_TRANSACT_REQ_BUFFER_SIZEMIN;
        Result = FspFsctlTransact(FileSystem->VolumeHandle, 0, 0, RequestBuf, &RequestBufSize);
        if (!NT_SUCCESS(Result))
            goto exit;
        RequestBufEnd = RequestBuf + RequestBufSize;

        Request = (PVOID)RequestBuf;
        for (;;)
        {
            NextRequest = FspFsctlTransactConsumeRequest(Request, RequestBufEnd);
            if (0 == NextRequest)
                break;

            Result = FileSystem->ProcessRequest(FileSystem, Request);
            if (!NT_SUCCESS(Result))
                goto exit;

            Request = NextRequest;
        }
    }

exit:
    free(RequestBuf);

    return Result;
}

FSP_API NTSTATUS FspProcessRequestDirect(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request)
{
    if (FspFsctlTransactKindCount <= Request->Kind || 0 == FileSystem->Operations[Request->Kind])
        return FspProduceResponseWithStatus(FileSystem, Request, STATUS_INVALID_DEVICE_REQUEST);

    FileSystem->Operations[Request->Kind](FileSystem, Request);
    return STATUS_SUCCESS;
}

static DWORD WINAPI FspProcessRequestInPoolWorker(PVOID Param)
{
    FSP_WORK_ITEM *WorkItem = Param;
    FSP_FSCTL_TRANSACT_REQ *Request = (PVOID)WorkItem->RequestBuf;

    WorkItem->FileSystem->Operations[Request->Kind](WorkItem->FileSystem, Request);
    free(WorkItem);

    return 0;
}

FSP_API NTSTATUS FspProcessRequestInPool(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request)
{
    if (FspFsctlTransactKindCount <= Request->Kind || 0 == FileSystem->Operations[Request->Kind])
        return FspProduceResponseWithStatus(FileSystem, Request, STATUS_INVALID_DEVICE_REQUEST);

    FSP_WORK_ITEM *WorkItem;
    BOOLEAN Success;

    WorkItem = malloc(sizeof *WorkItem + Request->Size);
    if (0 == WorkItem)
        return STATUS_INSUFFICIENT_RESOURCES;

    WorkItem->FileSystem = FileSystem;
    memcpy(WorkItem->RequestBuf, Request, Request->Size);

    Success = QueueUserWorkItem(FspProcessRequestInPoolWorker, WorkItem, WT_EXECUTEDEFAULT);
    if (!Success)
    {
        free(WorkItem);
        return FspNtStatusFromWin32(GetLastError());
    }

    return STATUS_SUCCESS;
}

FSP_API NTSTATUS FspProduceResponse(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_RSP *Response)
{
    return FspFsctlTransact(FileSystem->VolumeHandle, Response, Response->Size, 0, 0);
}

FSP_API NTSTATUS FspProduceResponseWithStatus(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request, NTSTATUS Result)
{
    FSP_FSCTL_TRANSACT_RSP Response = { 0 };
    Response.Size = sizeof Response;
    Response.Kind = Request->Kind;
    Response.Hint = Request->Hint;
    Response.IoStatus.Status = Result;
    return FspProduceResponse(FileSystem, &Response);
}