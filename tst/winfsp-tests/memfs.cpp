/**
 * @file memfs.c
 *
 * @copyright 2015 Bill Zissimopoulos
 */

#include "memfs.h"
#include <map>

#define MEMFS_SECTOR_SIZE               512

typedef struct _MEMFS_FILE_NODE
{
    WCHAR FileName[MAX_PATH];
    DWORD FileAttributes;
    SIZE_T FileSecuritySize;
    PVOID FileSecurity;
    ULONG AllocationSize;
    ULONG FileSize;
    PVOID FileData;
    ULONG OpenCount;
} MEMFS_FILE_NODE;

struct MEMFS_FILE_NODE_LESS
{
    bool operator()(PWSTR a, PWSTR b) const
    {
        return 0 > wcscmp(a, b);
    }
};
typedef std::map<PWSTR, MEMFS_FILE_NODE *, MEMFS_FILE_NODE_LESS> MEMFS_FILE_NODE_MAP;

typedef struct _MEMFS
{
    FSP_FILE_SYSTEM *FileSystem;
    MEMFS_FILE_NODE_MAP *FileNodeMap;
    ULONG MaxFileNodes;
    ULONG MaxFileSize;
    CRITICAL_SECTION Lock;
} MEMFS;

static inline
NTSTATUS MemfsFileNodeCreate(PWSTR FileName, MEMFS_FILE_NODE **PFileNode)
{
    MEMFS_FILE_NODE *FileNode;

    *PFileNode = 0;

    if (MAX_PATH <= wcslen(FileName))
        return STATUS_OBJECT_NAME_INVALID;

    FileNode = (MEMFS_FILE_NODE *)malloc(sizeof *FileNode);
    if (0 == FileNode)
        return STATUS_INSUFFICIENT_RESOURCES;

    memset(FileNode, 0, sizeof *FileNode);
    wcscpy_s(FileNode->FileName, sizeof FileNode->FileName / sizeof(WCHAR), FileName);

    *PFileNode = FileNode;

    return STATUS_SUCCESS;
}

static inline
VOID MemfsFileNodeDelete(MEMFS_FILE_NODE *FileNode)
{
    free(FileNode->FileData);
    free(FileNode->FileSecurity);
    free(FileNode);
}

static inline
NTSTATUS MemfsFileNodeMapCreate(MEMFS_FILE_NODE_MAP **PFileNodeMap)
{
    *PFileNodeMap = 0;
    try
    {
        *PFileNodeMap = new MEMFS_FILE_NODE_MAP;
        return STATUS_SUCCESS;
    }
    catch (...)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

static inline
VOID MemfsFileNodeMapDelete(MEMFS_FILE_NODE_MAP *FileNodeMap)
{
    for (MEMFS_FILE_NODE_MAP::iterator p = FileNodeMap->begin(), q = FileNodeMap->end(); p != q; ++p)
        MemfsFileNodeDelete(p->second);

    delete FileNodeMap;
}

static inline
SIZE_T MemfsFileNodeMapCount(MEMFS_FILE_NODE_MAP *FileNodeMap)
{
    return FileNodeMap->size();
}

static inline
MEMFS_FILE_NODE *MemfsFileNodeMapGet(MEMFS_FILE_NODE_MAP *FileNodeMap, PWSTR FileName)
{
    MEMFS_FILE_NODE_MAP::iterator iter = FileNodeMap->find(FileName);
    if (iter == FileNodeMap->end())
        return 0;
    return iter->second;
}

static inline
MEMFS_FILE_NODE *MemfsFileNodeMapGetParent(MEMFS_FILE_NODE_MAP *FileNodeMap, PWSTR FileName)
{
    PWSTR Remain, Suffix;
    FspPathSuffix(FileName, &Remain, &Suffix);
    MEMFS_FILE_NODE_MAP::iterator iter = FileNodeMap->find(Remain);
    FspPathCombine(Remain, Suffix);
    if (iter == FileNodeMap->end())
        return 0;
    return iter->second;
}

static inline
NTSTATUS MemfsFileNodeMapInsert(MEMFS_FILE_NODE_MAP *FileNodeMap, MEMFS_FILE_NODE *FileNode,
    PBOOLEAN PInserted)
{
    *PInserted = 0;
    try
    {
        *PInserted = FileNodeMap->insert(MEMFS_FILE_NODE_MAP::value_type(FileNode->FileName, FileNode)).second;
        return STATUS_SUCCESS;
    }
    catch (...)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

static inline
VOID MemfsFileNodeMapRemove(MEMFS_FILE_NODE_MAP *FileNodeMap, PWSTR FileName)
{
    FileNodeMap->erase(FileName);
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, PDWORD PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    MEMFS_FILE_NODE *FileNode;

    FileNode = MemfsFileNodeMapGet(Memfs->FileNodeMap, FileName);
    if (0 == FileNode)
        return !MemfsFileNodeMapGetParent(Memfs->FileNodeMap, FileName) ?
            STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;

    if (0 != PFileAttributes)
        *PFileAttributes = FileNode->FileAttributes;

    if (0 == SecurityDescriptor)
    {
        if (0 != PSecurityDescriptorSize)
            *PSecurityDescriptorSize = FileNode->FileSecuritySize;
    }
    else
    {
        if (0 != PSecurityDescriptorSize)
        {
            if (0 < FileNode->FileSecuritySize &&
                FileNode->FileSecuritySize <= *PSecurityDescriptorSize)
                memcpy(SecurityDescriptor, FileNode->FileSecurity, FileNode->FileSecuritySize);
            *PSecurityDescriptorSize = FileNode->FileSecuritySize;
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request,
    PWSTR FileName, BOOLEAN CaseSensitive, DWORD CreateOptions,
    DWORD FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
    FSP_FILE_NODE_INFO *NodeInfo)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    MEMFS_FILE_NODE *FileNode;
    NTSTATUS Result;
    BOOLEAN Inserted;

    FileNode = MemfsFileNodeMapGet(Memfs->FileNodeMap, FileName);
    if (0 != FileNode)
        return STATUS_OBJECT_NAME_COLLISION;

    if (!MemfsFileNodeMapGetParent(Memfs->FileNodeMap, FileName))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (MemfsFileNodeMapCount(Memfs->FileNodeMap) >= Memfs->MaxFileNodes)
        return STATUS_CANNOT_MAKE;

    if (AllocationSize > Memfs->MaxFileSize)
        return STATUS_DISK_FULL;

    Result = MemfsFileNodeCreate(FileName, &FileNode);
    if (!NT_SUCCESS(Result))
        return Result;

    FileNode->FileAttributes = FileAttributes;

    if (0 != SecurityDescriptor)
    {
        FileNode->FileSecuritySize = GetSecurityDescriptorLength(SecurityDescriptor);
        FileNode->FileSecurity = (PSECURITY_DESCRIPTOR)malloc(FileNode->FileSecuritySize);
        if (0 == FileNode->FileSecuritySize)
        {
            MemfsFileNodeDelete(FileNode);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    FileNode->AllocationSize = FSP_FSCTL_ALIGN_UP((ULONG)AllocationSize, MEMFS_SECTOR_SIZE);
    if (0 != FileNode->AllocationSize)
    {
        FileNode->FileData = malloc(FileNode->AllocationSize);
        if (0 == FileNode->FileData)
        {
            MemfsFileNodeDelete(FileNode);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Result = MemfsFileNodeMapInsert(Memfs->FileNodeMap, FileNode, &Inserted);
    if (!NT_SUCCESS(Result) || !Inserted)
    {
        MemfsFileNodeDelete(FileNode);
        if (NT_SUCCESS(Result))
            Result = STATUS_OBJECT_NAME_COLLISION; /* should not happen! */
        return Result;
    }

    FileNode->OpenCount++;
    NodeInfo->FileAttributes = FileNode->FileAttributes;
    NodeInfo->AllocationSize = FileNode->AllocationSize;
    NodeInfo->FileSize = FileNode->FileSize;
    NodeInfo->FileNode = FileNode;

    return STATUS_SUCCESS;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request,
    PWSTR FileName, BOOLEAN CaseSensitive, DWORD CreateOptions,
    FSP_FILE_NODE_INFO *NodeInfo)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    MEMFS_FILE_NODE *FileNode;

    FileNode = MemfsFileNodeMapGet(Memfs->FileNodeMap, FileName);
    if (0 == FileNode)
        return !MemfsFileNodeMapGetParent(Memfs->FileNodeMap, FileName) ?
            STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;

    FileNode->OpenCount++;
    NodeInfo->FileAttributes = FileNode->FileAttributes;
    NodeInfo->AllocationSize = FileNode->AllocationSize;
    NodeInfo->FileSize = FileNode->FileSize;
    NodeInfo->FileNode = FileNode;

    return STATUS_SUCCESS;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request,
    PVOID FileNode0, DWORD FileAttributes, BOOLEAN ReplaceFileAttributes,
    FSP_FILE_SIZE_INFO *SizeInfo)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    MEMFS_FILE_NODE *FileNode = (MEMFS_FILE_NODE *)FileNode0;

    if (ReplaceFileAttributes)
        FileNode->FileAttributes = FileAttributes;
    else
        FileNode->FileAttributes |= FileAttributes;

    FileNode->FileSize = 0;

    SizeInfo->AllocationSize = FileNode->AllocationSize;
    SizeInfo->FileSize = FileNode->FileSize;

    return STATUS_SUCCESS;
}

static VOID Cleanup(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request,
    PVOID FileNode0, BOOLEAN Delete)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    MEMFS_FILE_NODE *FileNode = (MEMFS_FILE_NODE *)FileNode0;

    if (Delete)
        MemfsFileNodeMapRemove(Memfs->FileNodeMap, FileNode->FileName);
}

static VOID Close(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_TRANSACT_REQ *Request,
    PVOID FileNode0)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    MEMFS_FILE_NODE *FileNode = (MEMFS_FILE_NODE *)FileNode0;

    if (0 == --FileNode->OpenCount)
        MemfsFileNodeDelete(FileNode);
}

static FSP_FILE_SYSTEM_INTERFACE MemfsInterface =
{
    GetSecurity,
    Create,
    Open,
    Overwrite,
    Cleanup,
    Close,
};

static VOID MemfsEnterOperation(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_TRANSACT_REQ *Request)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    EnterCriticalSection(&Memfs->Lock);
}

static VOID MemfsLeaveOperation(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_TRANSACT_REQ *Request)
{
    MEMFS *Memfs = (MEMFS *)FileSystem->UserContext;
    LeaveCriticalSection(&Memfs->Lock);
}

NTSTATUS MemfsCreate(ULONG Flags, ULONG MaxFileNodes, ULONG MaxFileSize,
    MEMFS **PMemfs)
{
    NTSTATUS Result;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    PWSTR DevicePath = (Flags & MemfsNet) ?
        L"" FSP_FSCTL_NET_DEVICE_NAME : L"" FSP_FSCTL_DISK_DEVICE_NAME;
    MEMFS *Memfs;

    *PMemfs = 0;

    Memfs = (MEMFS *)malloc(sizeof *Memfs);
    if (0 == Memfs)
        return STATUS_INSUFFICIENT_RESOURCES;

    memset(Memfs, 0, sizeof *Memfs);
    Memfs->MaxFileNodes = MaxFileNodes;
    Memfs->MaxFileSize = FSP_FSCTL_ALIGN_UP(MaxFileSize, MEMFS_SECTOR_SIZE);

    Result = MemfsFileNodeMapCreate(&Memfs->FileNodeMap);
    if (!NT_SUCCESS(Result))
    {
        free(Memfs);
        return Result;
    }

    memset(&VolumeParams, 0, sizeof VolumeParams);
    VolumeParams.SectorSize = MEMFS_SECTOR_SIZE;
    wcscpy_s(VolumeParams.Prefix, sizeof VolumeParams.Prefix / sizeof(WCHAR), L"\\memfs\\share");

    Result = FspFileSystemCreate(DevicePath, &VolumeParams, &MemfsInterface, &Memfs->FileSystem);
    if (!NT_SUCCESS(Result))
    {
        MemfsFileNodeMapDelete(Memfs->FileNodeMap);
        free(Memfs);
        return Result;
    }
    Memfs->FileSystem->UserContext = Memfs;

    InitializeCriticalSection(&Memfs->Lock);

    if (Flags & MemfsThreadPool)
        FspFileSystemSetDispatcher(Memfs->FileSystem,
            FspFileSystemPoolDispatcher,
            MemfsEnterOperation,
            MemfsLeaveOperation);

    *PMemfs = Memfs;

    return STATUS_SUCCESS;
}

VOID MemfsDelete(MEMFS *Memfs)
{
    DeleteCriticalSection(&Memfs->Lock);

    FspFileSystemDelete(Memfs->FileSystem);

    MemfsFileNodeMapDelete(Memfs->FileNodeMap);

    free(Memfs);
}