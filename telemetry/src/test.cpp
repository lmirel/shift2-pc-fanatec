#include "telemetry.cpp"
#include <cstdio>
__declspec(noinline) int Fixture(int a) { volatile int v=a; return v+1; }
static void* fixture_step_end=nullptr;
extern "C" __declspec(naked) void FixtureStep() {
 __asm {
  mov eax,offset step_end
  mov fixture_step_end,eax
  push ebx
  mov ebx,esp
  sub esp,8
  and esp,0fffffff8h
  add esp,4
  push ebp
  mov ebp,[ebx+4]
  mov [esp+4],ebp
  mov ebp,esp
  sub esp,1ch
  push esi
  mov esi,[ecx]
  fld qword ptr [esi]
  fld1
  faddp st(1),st(0)
  fstp qword ptr [esi]
 step_end:
  pop esi
  mov esp,ebp
  pop ebp
  mov esp,ebx
  pop ebx
  ret 8
 }
}
int main() {
 if(Shift2TelemetryStart()!=ERROR_BAD_EXE_FORMAT)return 1;
 void* original=nullptr;
 if(MH_Initialize()!=MH_OK || MH_CreateHook(Fixture,TraceHook,&original)!=MH_OK || MH_EnableHook(Fixture)!=MH_OK)return 6;
 double before=123.456,after=0; unsigned reg=0,oldmx=0,newmx=0;
 __asm { stmxcsr oldmx
 fld before
 mov esi,012345678h
 push 42
 call Fixture
 add esp,4
 mov reg,esi
 fstp after
 stmxcsr newmx
 }
 if(after!=before||reg!=0x12345678||oldmx!=newmx)return 2;
 if(MH_DisableHook(Fixture)!=MH_OK || Fixture(42)!=43)return 7;
 shared=static_cast<Header*>(VirtualAlloc(nullptr,64+capacity*128,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
 InitializeCriticalSection(&guard);base=0x400000;
 uint32_t frame[28]{};frame[1]=0x1234;frame[9]=0xa007fc+5;frame[10]=0xe11010;
 double values[]={250,6000,210};memcpy(frame+11,values,sizeof(values));
 for(unsigned i=0;i<capacity+7;++i)Capture(frame);
 if(shared->written!=capacity+7 || (shared->sequence&1))return 3;
 Record r;memcpy(&r,reinterpret_cast<char*>(shared)+64+6*128,128);
 if(r.serial!=capacity+7 || r.owner!=0x1234 || r.values[1]!=6000 || r.kind!=0 || r.flags!=1)return 4;
 frame[9]++;Capture(frame);if(shared->written!=capacity+7)return 5;
 auto root=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x4100,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
 auto body=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
 uint32_t bodyptr=reinterpret_cast<uint32_t>(body),wrapper=reinterpret_cast<uint32_t>(&bodyptr),linked=0x123456;
 memcpy(root,&linked,4);memcpy(root+0x33a0,&wrapper,4);int gear=3;memcpy(root+0x4010,&gear,4);
 double xyz[]={1,2,3};memcpy(body,xyz,24);double velocity[]={4,5,6};memcpy(body+0x78,velocity,24);
 frame[1]=reinterpret_cast<uint32_t>(root);frame[9]=0xa0430b+5;frame[10]=0xe11380;
 auto previous=shared->written;Capture(frame);if(shared->written!=previous+6)return 8;
 Record check;memcpy(&check,reinterpret_cast<char*>(shared)+64+((previous+1)%capacity)*128,128);
 if(check.kind!=13||check.values[0]!=3||check.values[5]!=linked||check.values[6]!=bodyptr)return 9;
 memcpy(&check,reinterpret_cast<char*>(shared)+64+((previous+2)%capacity)*128,128);
 if(check.kind!=14||check.values[0]!=1||check.values[5]!=6)return 10;
 double dt=.01;
 __asm { sub esp,8
 fld dt
 fstp qword ptr [esp]
 mov ecx,wrapper
 call FixtureStep
 }
 physics_root=reinterpret_cast<uintptr_t>(root);physics_mode=true;
 if(MH_CreateHook(fixture_step_end,PhysicsHook,&physics_original)!=MH_OK || MH_EnableHook(fixture_step_end)!=MH_OK)return 11;
 previous=shared->written;after=0;
 __asm { fld before
 mov esi,012345678h
 sub esp,8
 fld dt
 fstp qword ptr [esp]
 mov ecx,wrapper
 call FixtureStep
 mov reg,esi
 fstp after
 }
 if(after!=before || reg!=0x12345678 || shared->written!=previous+6)return 12;
 memcpy(&check,reinterpret_cast<char*>(shared)+64+(previous%capacity)*128,128);
 if(check.kind!=13)return 13;
 memcpy(&check,reinterpret_cast<char*>(shared)+64+((previous+1)%capacity)*128,128);
 if(check.kind!=14 || check.values[0]!=3)return 14;
 memcpy(&check,reinterpret_cast<char*>(shared)+64+((previous+5)%capacity)*128,128);
 if(check.kind!=18 || check.values[0]!=dt || check.values[3]!=1)return 15;
 if(MH_DisableHook(fixture_step_end)!=MH_OK)return 16;
 puts("PASS: unsupported build rejected; x87, MXCSR, ESI preserved; ring wrap, call-site validation, linked body/drivetrain snapshots; completed-step epilogue, dt, x87, callee registers and stack cleanup");return 0;
}
