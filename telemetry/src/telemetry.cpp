#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include "../vendor/minhook/include/MinHook.h"
#pragma pack(push, 1)
struct Header { char magic[4]; uint32_t version, headerSize, recordSize, capacity, pid; volatile LONG sequence; uint32_t reserved; uint64_t frequency, written, dropped, reserved2; };
struct Record { uint64_t serial, qpc; uint32_t owner, thread, returnRva, kind, count, flags; double values[8]; char component[24]; };
#pragma pack(pop)
static_assert(sizeof(Header)==64 && sizeof(Record)==128);
static constexpr unsigned capacity=4096;
static Header* shared;
static HANDLE mapping;
static CRITICAL_SECTION guard;
static uintptr_t base;
static volatile LONG started;
static volatile LONG drops;
static bool physics_mode=false;
static uintptr_t physics_root=0;
extern "C" { void* physics_original=nullptr; }
static uint32_t step_body=0,step_link=0;
static double step_time=0,last_context_time=0;
static uint64_t step_sequence=0;
struct Site { uint32_t call, format, count; };
static const Site sites[]={
 {0x6007fc,0xa11010,3},{0x6041a0,0xa113b8,3},{0x60430b,0xa11380,5},
 {0x6044f7,0xa11330,6},{0x60468e,0xa112fc,3},{0x6047c3,0xa112d0,3},
 {0x6048c4,0xa11270,6},{0x604b3f,0xa11210,7},{0x604dbf,0xa111c0,6},
 {0x604ef6,0xa11148,6},{0x604faa,0xa11120,3},{0x604fec,0xa110f8,3},{0x605054,0xa110b8,4}};
static bool copySafe(void* dst,const void* src,size_t size) {
 __try { memcpy(dst,src,size); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void Store(Record& r) {
 r.serial=shared->written+1;
 InterlockedIncrement(&shared->sequence);
 memcpy(reinterpret_cast<char*>(shared)+64+((r.serial-1)%capacity)*128,&r,sizeof(r));
 shared->written=r.serial; shared->dropped=static_cast<uint32_t>(drops);
 MemoryBarrier(); InterlockedIncrement(&shared->sequence);
}
static void BodySnapshot(const Record& source, const double* time_override=nullptr) {
 const uintptr_t root=source.owner;
 uint32_t link=0,wrapper=0,body=0; int32_t gears[3]{},maxGear=0; double omega=0,time=0;
 if(!copySafe(&link,reinterpret_cast<void*>(root),4) || !copySafe(&wrapper,reinterpret_cast<void*>(root+0x33a0),4)
  || !copySafe(&body,reinterpret_cast<void*>(wrapper),4) || !body
  || !copySafe(gears,reinterpret_cast<void*>(root+0x4010),12)
  || !copySafe(&maxGear,reinterpret_cast<void*>(root+0x3fc0),4)
  || !copySafe(&omega,reinterpret_cast<void*>(root+0x3cf8),8)
  || !copySafe(&time,reinterpret_cast<void*>(root+0xa0),8))return;
 double position[3],velocity[3],angular[3],force[3],inverseMass; float matrix[9];
 if(!copySafe(position,reinterpret_cast<void*>(body),24) || !copySafe(velocity,reinterpret_cast<void*>(body+0x78),24)
  || !copySafe(force,reinterpret_cast<void*>(body+0x60),24) || !copySafe(&inverseMass,reinterpret_cast<void*>(body+0x90),8)
  || !copySafe(angular,reinterpret_cast<void*>(body+0x18),24) || !copySafe(matrix,reinterpret_cast<void*>(body+0xd4),36))return;
 if(time_override)time=*time_override;
 Record r{};r.qpc=source.qpc;r.owner=source.owner;r.thread=source.thread;r.returnRva=source.returnRva;r.flags=1;
 r.kind=13;r.count=8;r.values[0]=gears[0];r.values[1]=gears[1];r.values[2]=gears[2];r.values[3]=omega;r.values[4]=time;r.values[5]=link;r.values[6]=body;r.values[7]=maxGear;Store(r);
 r.kind=14;r.count=6;memcpy(r.values,position,24);memcpy(r.values+3,velocity,24);Store(r);
 r.kind=15;r.count=6;memcpy(r.values,angular,24);for(unsigned i=0;i<3;++i)r.values[3+i]=matrix[i];Store(r);
 r.kind=16;r.count=6;for(unsigned i=0;i<6;++i)r.values[i]=matrix[i+3];Store(r);
 r.kind=17;r.count=4;memcpy(r.values,force,24);r.values[3]=inverseMass;Store(r);
}
extern "C" void __cdecl Capture(const uint32_t* frame) {
 const uint32_t ret=frame[9]-static_cast<uint32_t>(base);
 unsigned kind=0; for(;kind<_countof(sites);++kind) if(ret==sites[kind].call+5) break;
 if(kind==_countof(sites) || frame[10]!=base+sites[kind].format) return;
 if(!TryEnterCriticalSection(&guard)) { InterlockedIncrement(&drops); return; }
 Record r{}; r.serial=shared->written+1; LARGE_INTEGER now; QueryPerformanceCounter(&now); r.qpc=now.QuadPart;
 r.owner=frame[1]; r.thread=GetCurrentThreadId(); r.returnRva=ret; r.kind=kind; r.count=sites[kind].count;
 const uint32_t* args=frame+11;
 if(kind==6) { const char* name=reinterpret_cast<const char*>(*args++); for(unsigned i=0;i<sizeof(r.component)-1;++i) { if(!copySafe(r.component+i,name+i,1) || !r.component[i]) break; } }
 if(copySafe(r.values,args,r.count*sizeof(double))) r.flags=1;
 Store(r);
 if(!physics_mode && kind==2 && r.flags)BodySnapshot(r);
 LeaveCriticalSection(&guard);
}
extern "C" __declspec(naked) void TraceHook() {
 __asm {
  pushfd
  pushad
  mov edx,esp
  sub esp,528
  and esp,0fffffff0h
  fxsave [esp]
  fninit
  sub esp,16
  mov [esp+4],edx
  lea eax,[esp+16]
  mov [esp+8],eax
  push edx
  call Capture
  add esp,4
  mov eax,[esp+8]
  fxrstor [eax]
  mov esp,[esp+4]
  popad
  popfd
  ret
 }
}
extern "C" void __cdecl CaptureStep(const uint32_t* frame) {
 InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&shared->reserved2));
 uint32_t wrapper=0,body=0,link=0;
 double dt=0,context_time=0;
 const uintptr_t root=physics_root;
 if(!copySafe(&wrapper,reinterpret_cast<void*>(root+0x33a0),4)
  || !copySafe(&body,reinterpret_cast<void*>(wrapper),4) || body!=frame[1]
  || !copySafe(&link,reinterpret_cast<void*>(root),4)
  || !copySafe(&context_time,reinterpret_cast<void*>(root+0xa0),8)
  || !copySafe(&dt,reinterpret_cast<void*>(static_cast<uintptr_t>(frame[4])+8),8)
  || !std::isfinite(dt) || dt<=0 || dt>.1 || !std::isfinite(context_time))return;
 if(!TryEnterCriticalSection(&guard)){InterlockedIncrement(&drops);return;}
 if(body!=step_body || link!=step_link || context_time<last_context_time-.01) {
  step_time=context_time;step_body=body;step_link=link;
 } else step_time+=dt;
 last_context_time=context_time;++step_sequence;
 Record r{};LARGE_INTEGER now;QueryPerformanceCounter(&now);r.qpc=now.QuadPart;
 r.owner=static_cast<uint32_t>(root);r.thread=GetCurrentThreadId();r.returnRva=0x6536b0;
 BodySnapshot(r,&step_time);
 r.kind=18;r.count=4;r.flags=1;r.values[0]=dt;r.values[1]=context_time;r.values[2]=step_time;r.values[3]=static_cast<double>(step_sequence);Store(r);
 LeaveCriticalSection(&guard);
}
extern "C" __declspec(naked) void PhysicsHook() {
 __asm {
  pushfd
  pushad
  mov edx,esp
  sub esp,528
  and esp,0fffffff0h
  fxsave [esp]
  fninit
  sub esp,16
  mov [esp+4],edx
  lea eax,[esp+16]
  mov [esp+8],eax
  push edx
  call CaptureStep
  add esp,4
  mov eax,[esp+8]
  fxrstor [eax]
  mov esp,[esp+4]
  popad
  popfd
  jmp dword ptr [physics_original]
 }
}
static bool verified() {
 wchar_t path[32768]; if(!GetModuleFileNameW(nullptr,path,_countof(path)))return false;
 HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); if(f==INVALID_HANDLE_VALUE)return false;
 BCRYPT_ALG_HANDLE a=nullptr; BCRYPT_HASH_HANDLE h=nullptr; unsigned char hash[32]{},buffer[65536]; DWORD got=0; bool ok=false;
 if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 && BCryptCreateHash(a,&h,nullptr,0,nullptr,0,0)>=0) {
  bool readok=true; for(;;) { if(!ReadFile(f,buffer,sizeof(buffer),&got,nullptr)){readok=false;break;} if(!got)break; if(BCryptHashData(h,buffer,got,0)<0){readok=false;break;} }
  if(readok && BCryptFinishHash(h,hash,sizeof(hash),0)>=0) {
   const unsigned char expected[]={0xbf,0x1f,0x9f,0xcf,0xf5,0x87,0xd5,0x5b,0xa1,0x90,0x70,0x7e,0xfe,0x53,0xe5,0x8b,0x41,0xdd,0x6a,0x3b,0x26,0xfd,0x59,0xcf,0xee,0xd8,0xa5,0x3a,0x8f,0x16,0xd1,0x57}; ok=memcmp(hash,expected,32)==0;
  }
 }
 if(h)BCryptDestroyHash(h); if(a)BCryptCloseAlgorithmProvider(a,0); CloseHandle(f); return ok;
}
extern "C" DWORD WINAPI Shift2TelemetryStart() {
 if(InterlockedCompareExchange(&started,1,0))return ERROR_ALREADY_INITIALIZED;
 if(!verified())return ERROR_BAD_EXE_FORMAT;
 base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 const unsigned char signature[]={0x55,0x8b,0xec,0x83,0x3d,0x00,0xc1,0xf5,0x00,0x00};
 if(memcmp(reinterpret_cast<void*>(base+0x5f0660),signature,sizeof(signature)))return ERROR_INVALID_DATA;
 for(const auto& s:sites) { unsigned char call[5]; if(!copySafe(call,reinterpret_cast<void*>(base+s.call),5)||call[0]!=0xe8)return ERROR_INVALID_DATA; int32_t delta; memcpy(&delta,call+1,4); if(base+s.call+5+delta!=base+0x5f0660)return ERROR_INVALID_DATA; }
 const unsigned char step_signature[]={0x5e,0x8b,0xe5,0x5d,0x8b,0xe3,0x5b,0xc2,0x08,0x00};
 if(memcmp(reinterpret_cast<void*>(base+0x6536b0),step_signature,sizeof(step_signature)))return ERROR_INVALID_DATA;
 physics_root=base+0xb5e800;
 wchar_t name[100]; wsprintfW(name,L"Local\\Shift2Telemetry-v1-%lu",GetCurrentProcessId());
 mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,64+capacity*128,name); if(!mapping)return GetLastError();
 shared=static_cast<Header*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,0)); if(!shared){DWORD e=GetLastError();CloseHandle(mapping);return e;}
 memset(shared,0,64+capacity*128); memcpy(shared->magic,"STL1",4); shared->version=1; shared->headerSize=64;shared->recordSize=128;shared->capacity=capacity;shared->pid=GetCurrentProcessId(); LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);shared->frequency=freq.QuadPart;
 InitializeCriticalSection(&guard);
 void* original=nullptr;
 if(MH_Initialize()!=MH_OK
  || MH_CreateHook(reinterpret_cast<void*>(base+0x5f0660),TraceHook,&original)!=MH_OK
  || MH_CreateHook(reinterpret_cast<void*>(base+0x6536b0),PhysicsHook,&physics_original)!=MH_OK
  || MH_QueueEnableHook(reinterpret_cast<void*>(base+0x5f0660))!=MH_OK
  || MH_QueueEnableHook(reinterpret_cast<void*>(base+0x6536b0))!=MH_OK) {
  MH_DisableHook(MH_ALL_HOOKS);MH_RemoveHook(MH_ALL_HOOKS);UnmapViewOfFile(shared);shared=nullptr;CloseHandle(mapping);return ERROR_INVALID_FUNCTION;
 }
 physics_mode=true;shared->reserved=1;
 if(MH_ApplyQueued()!=MH_OK) {
  MH_DisableHook(MH_ALL_HOOKS);MH_RemoveHook(MH_ALL_HOOKS);UnmapViewOfFile(shared);shared=nullptr;CloseHandle(mapping);return ERROR_INVALID_FUNCTION;
 }
 HMODULE pin; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Shift2TelemetryStart),&pin);
 return ERROR_SUCCESS;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(instance);return TRUE;}
