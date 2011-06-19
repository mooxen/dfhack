/*
https://github.com/peterix/dfhack
Copyright (c) 2009-2011 Petr Mrázek (peterix@gmail.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "Internal.h"

#define _WIN32_WINNT 0x0501 // needed for INPUT struct
#define WINVER 0x0501       // OpenThread(), PSAPI, Toolhelp32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnt.h>
#include <psapi.h>
#include <tlhelp32.h>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

// FIXME: it is uncertain how these map to 64bit
typedef struct _DEBUG_BUFFER
{
    HANDLE SectionHandle;
    PVOID  SectionBase;
    PVOID  RemoteSectionBase;
    ULONG  SectionBaseDelta;
    HANDLE  EventPairHandle;
    ULONG  Unknown[2];
    HANDLE  RemoteThreadHandle;
    ULONG  InfoClassMask;
    ULONG  SizeOfInfo;
    ULONG  AllocatedSize;
    ULONG  SectionSize;
    PVOID  ModuleInformation;
    PVOID  BackTraceInformation;
    PVOID  HeapInformation;
    PVOID  LockInformation;
    PVOID  Reserved[8];
} DEBUG_BUFFER, *PDEBUG_BUFFER;

typedef struct _DEBUG_HEAP_INFORMATION
{
    ULONG Base; // 0×00
    ULONG Flags; // 0×04
    USHORT Granularity; // 0×08
    USHORT Unknown; // 0x0A
    ULONG Allocated; // 0x0C
    ULONG Committed; // 0×10
    ULONG TagCount; // 0×14
    ULONG BlockCount; // 0×18
    ULONG Reserved[7]; // 0x1C
    PVOID Tags; // 0×38
    PVOID Blocks; // 0x3C
} DEBUG_HEAP_INFORMATION, *PDEBUG_HEAP_INFORMATION;

// RtlQueryProcessDebugInformation.DebugInfoClassMask constants
#define PDI_MODULES                       0x01
#define PDI_BACKTRACE                     0x02
#define PDI_HEAPS                         0x04
#define PDI_HEAP_TAGS                     0x08
#define PDI_HEAP_BLOCKS                   0x10
#define PDI_LOCKS                         0x20

extern "C" __declspec(dllimport) NTSTATUS __stdcall RtlQueryProcessDebugInformation( IN ULONG  ProcessId, IN ULONG  DebugInfoClassMask, IN OUT PDEBUG_BUFFER  DebugBuffer);
extern "C" __declspec(dllimport) PDEBUG_BUFFER __stdcall RtlCreateQueryDebugBuffer( IN ULONG  Size, IN BOOLEAN  EventPair);
extern "C" __declspec(dllimport) NTSTATUS __stdcall RtlDestroyQueryDebugBuffer( IN PDEBUG_BUFFER  DebugBuffer);

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
using namespace std;

#include "dfhack/VersionInfo.h"
#include "dfhack/VersionInfoFactory.h"
#include "dfhack/Error.h"
#include "dfhack/Process.h"
using namespace DFHack;
namespace DFHack
{
    class PlatformSpecific
    {
    public:
        PlatformSpecific()
        {
            base = 0;
            sections = 0;
        };
        HANDLE my_handle;
        vector <HANDLE> threads;
        vector <HANDLE> stoppedthreads;
        uint32_t my_pid;
        IMAGE_NT_HEADERS pe_header;
        IMAGE_SECTION_HEADER * sections;
        uint32_t base;
    };
}
Process::Process(VersionInfoFactory * factory)
{
    HMODULE hmod = NULL;
    DWORD needed;
    bool found = false;
    identified = false;
    my_descriptor = NULL;

    d = new PlatformSpecific();
    // open process
    d->my_pid = GetCurrentProcessId();
    d->my_handle = GetCurrentProcess();
    // try getting the first module of the process
    if(EnumProcessModules(d->my_handle, &hmod, sizeof(hmod), &needed) == 0)
    {
        return; //if enumprocessModules fails, give up
    }

    // got base ;)
    d->base = (uint32_t)hmod;

    // read from this process
    try
    {
        uint32_t pe_offset = Process::readDWord(d->base+0x3C);
        read(d->base + pe_offset, sizeof(d->pe_header), (uint8_t *)&(d->pe_header));
        const size_t sectionsSize = sizeof(IMAGE_SECTION_HEADER) * d->pe_header.FileHeader.NumberOfSections;
        d->sections = (IMAGE_SECTION_HEADER *) malloc(sectionsSize);
        read(d->base + pe_offset + sizeof(d->pe_header), sectionsSize, (uint8_t *)(d->sections));
    }
    catch (exception &)
    {
        return;
    }
    VersionInfo* vinfo = factory->getVersionInfoByPETimestamp(d->pe_header.FileHeader.TimeDateStamp);
    if(vinfo)
    {
        vector<uint32_t> threads_ids;
        if(!getThreadIDs( threads_ids ))
        {
            // thread enumeration failed.
            return;
        }
        identified = true;
        // give the process a data model and memory layout fixed for the base of first module
        my_descriptor  = new VersionInfo(*vinfo);
        my_descriptor->RebaseAll(d->base);
        // keep track of created memory_info object so we can destroy it later
        my_descriptor->setParentProcess(this);
        for(size_t i = 0; i < threads_ids.size();i++)
        {
            HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, (DWORD) threads_ids[i]);
            if(hThread)
                d->threads.push_back(hThread);
            else
                cerr << "Unable to open thread :" << hex << (DWORD) threads_ids[i] << endl;
        }
    }
}

Process::~Process()
{
    // destroy our rebased copy of the memory descriptor
    delete my_descriptor;
    for(size_t i = 0; i < d->threads.size(); i++)
        CloseHandle(d->threads[i]);
    if(d->sections != NULL)
        free(d->sections);
}

bool Process::getThreadIDs(vector<uint32_t> & threads )
{
    HANDLE AllThreads = INVALID_HANDLE_VALUE;
    THREADENTRY32 te32;

    AllThreads = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if( AllThreads == INVALID_HANDLE_VALUE )
    {
        return false;
    }
    te32.dwSize = sizeof(THREADENTRY32 );

    if( !Thread32First( AllThreads, &te32 ) )
    {
        CloseHandle( AllThreads );
        return false;
    }

    do
    {
        if( te32.th32OwnerProcessID == d->my_pid )
        {
            threads.push_back(te32.th32ThreadID);
        }
    } while( Thread32Next(AllThreads, &te32 ) );

    CloseHandle( AllThreads );
    return true;
}
/*
typedef struct _MEMORY_BASIC_INFORMATION
{
  void *  BaseAddress;
  void *  AllocationBase;
  uint32_t  AllocationProtect;
  size_t RegionSize;
  uint32_t  State;
  uint32_t  Protect;
  uint32_t  Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
*/
/*
//Internal structure used to store heap block information.
struct HeapBlock
{
      PVOID dwAddress;
      DWORD dwSize;
      DWORD dwFlags;
      ULONG reserved;
};
*/
void HeapNodes(DWORD pid, map<uint64_t, unsigned int> & heaps)
{
    // Create debug buffer
    PDEBUG_BUFFER db = RtlCreateQueryDebugBuffer(0, FALSE); 
    // Get process heap data
    RtlQueryProcessDebugInformation( pid, PDI_HEAPS/* | PDI_HEAP_BLOCKS*/, db);
    ULONG heapNodeCount = db->HeapInformation ? *PULONG(db->HeapInformation):0;
    PDEBUG_HEAP_INFORMATION heapInfo = PDEBUG_HEAP_INFORMATION(PULONG(db-> HeapInformation) + 1);
    // Go through each of the heap nodes and dispaly the information
    for (unsigned int i = 0; i < heapNodeCount; i++) 
    {
        heaps[heapInfo[i].Base] = i;
    }
    // Clean up the buffer
    RtlDestroyQueryDebugBuffer( db );
}

// FIXME: NEEDS TESTING!
void Process::getMemRanges( vector<t_memrange> & ranges )
{
    MEMORY_BASIC_INFORMATION MBI;
    map<uint64_t, unsigned int> heaps;
    uint64_t movingStart = 0;
    map <uint64_t, string> nameMap;

    // get page size
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t PageSize = si.dwPageSize;
    // enumerate heaps
    HeapNodes(d->my_pid, heaps);
    // go through all the VM regions, convert them to our internal format
    while (VirtualQueryEx(d->my_handle, (const void*) (movingStart), &MBI, sizeof(MBI)) == sizeof(MBI))
    {
        movingStart = ((uint64_t)MBI.BaseAddress + MBI.RegionSize);
        if(movingStart % PageSize != 0)
            movingStart = (movingStart / PageSize + 1) * PageSize;
        // skip empty regions and regions we share with other processes (DLLs)
        if( !(MBI.State & MEM_COMMIT) /*|| !(MBI.Type & MEM_PRIVATE)*/ )
            continue;
        t_memrange temp;
        temp.start   = (uint64_t) MBI.BaseAddress;
        temp.end     =  ((uint64_t)MBI.BaseAddress + (uint64_t)MBI.RegionSize);
        temp.read    = MBI.Protect & PAGE_EXECUTE_READ || MBI.Protect & PAGE_EXECUTE_READWRITE || MBI.Protect & PAGE_READONLY || MBI.Protect & PAGE_READWRITE;
        temp.write   = MBI.Protect & PAGE_EXECUTE_READWRITE || MBI.Protect & PAGE_READWRITE;
        temp.execute = MBI.Protect & PAGE_EXECUTE_READ || MBI.Protect & PAGE_EXECUTE_READWRITE || MBI.Protect & PAGE_EXECUTE;
        temp.valid = true;
        if(!GetModuleBaseName(d->my_handle, (HMODULE) temp.start, temp.name, 1024))
        {
            if(nameMap.count(temp.start))
            {
                // potential buffer overflow...
                strcpy(temp.name, nameMap[temp.start].c_str());
            }
            else
            {
                // filter away shared segments without a name.
                if( !(MBI.Type & MEM_PRIVATE) )
                    continue;
                else
                {
                    // could be a heap?
                    if(heaps.count(temp.start))
                    {
                        sprintf(temp.name,"HEAP %d",heaps[temp.start]);
                    }
                    else temp.name[0]=0;
                }
            }
        }
        else
        {
            // this is our executable! (could be generalized to pull segments from libs, but whatever)
            if(d->base == temp.start)
            {
                for(int i = 0; i < d->pe_header.FileHeader.NumberOfSections; i++)
                {
                    char sectionName[9];
                    memcpy(sectionName,d->sections[i].Name,8);
                    sectionName[8] = 0;
                    string nm;
                    nm.append(temp.name);
                    nm.append(" : ");
                    nm.append(sectionName);
                    nameMap[temp.start + d->sections[i].VirtualAddress] = nm;
                }
            }
            else
                continue;
        }
        ranges.push_back(temp);
    }
}

string Process::doReadClassName (uint32_t vptr)
{
    int rtti = readDWord(vptr - 0x4);
    int typeinfo = readDWord(rtti + 0xC);
    string raw = readCString(typeinfo + 0xC); // skips the .?AV
    raw.resize(raw.length() - 2);// trim @@ from end
    return raw;
}

string Process::getPath()
{
    HMODULE hmod;
    DWORD junk;
    char String[255];
    EnumProcessModules(d->my_handle, &hmod, 1 * sizeof(HMODULE), &junk); //get the module from the handle
    GetModuleFileNameEx(d->my_handle,hmod,String,sizeof(String)); //get the filename from the module
    string out(String);
    return(out.substr(0,out.find_last_of("\\")));
}
