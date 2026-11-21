"""Read the versioned Shift 2 LiveTrace ring without injecting into the game."""
import argparse, ctypes as c, json, struct, time
from pathlib import Path
KINDS=[('engine',['torque_nm','rpm','power_hp']),('body_time',['time','total_cg','body_cg']),('height_air_velocity',['front_height','rear_height','air_x','air_y','air_z']),('body_aero',['lift','lift_z','lift_pitch','drag','drag_y','drag_pitch']),('stall',['height1','height2','downforce_multiplier']),('diffuser',['lift','z','pitch']),('component',['lift','lift_z','lift_pitch','drag','drag_y','drag_pitch']),('front_wing',['lift','lift_z','lift_pitch','drag','drag_y','drag_pitch','height']),('rear_wing',['lift','lift_z','lift_pitch','drag','drag_y','drag_pitch']),('total_aero',['lift','lift_pitch','drag','drag_pitch','other_pitch','torque_arm']),('aero_force',['x','y','z']),('aero_torque',['x','y','z']),('cg_offset',['x','y','z','measured_arm'])]
KINDS += [('drivetrain_raw',['gear_index','gear_target','shift_state','engine_rad_s','time','linked_pointer','body_pointer','max_gears']),('body_position_velocity_raw',['px','py','pz','vx','vy','vz']),('angular_matrix_row0_raw',['wx','wy','wz','m00','m01','m02']),('matrix_rows12_raw',['m10','m11','m12','m20','m21','m22'])]
KINDS += [('force_accumulator_raw',['fx','fy','fz','inverse_mass'])]
KINDS += [('physics_step',['dt','context_time','sample_time','step_sequence'])]
HEADER=struct.Struct('<4s7I4Q'); RECORD=struct.Struct('<QQ6I8d24s')
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--pid',type=int,required=True);ap.add_argument('--seconds',type=float,default=60);ap.add_argument('--output',type=Path);args=ap.parse_args()
 k=c.WinDLL('kernel32',use_last_error=True)
 k.OpenFileMappingW.argtypes=[c.c_uint32,c.c_int,c.c_wchar_p];k.OpenFileMappingW.restype=c.c_void_p
 k.MapViewOfFile.argtypes=[c.c_void_p,c.c_uint32,c.c_uint32,c.c_uint32,c.c_size_t];k.MapViewOfFile.restype=c.c_void_p
 k.UnmapViewOfFile.argtypes=[c.c_void_p];k.CloseHandle.argtypes=[c.c_void_p]
 handle=k.OpenFileMappingW(4,False,f'Local\\Shift2Telemetry-v1-{args.pid}')
 if not handle: raise c.WinError(c.get_last_error())
 address=k.MapViewOfFile(handle,4,0,0,0)
 if not address:k.CloseHandle(handle);raise c.WinError(c.get_last_error())
 output=None
 try:
  h=HEADER.unpack(c.string_at(address,64))
  if h[:5]!=(b'STL1',1,64,128,4096) or h[5]!=args.pid:raise RuntimeError('Unsupported telemetry ABI')
  if args.output:args.output.parent.mkdir(parents=True,exist_ok=True);output=args.output.open('w')
  cursor=0; missed=0; owners={};start=time.monotonic();last_print=0
  while time.monotonic()-start<args.seconds:
   blob=None
   for attempt in range(20):
    seq=c.c_int32.from_address(address+24).value
    if seq&1:continue
    candidate=c.string_at(address,64+4096*128)
    if seq==c.c_int32.from_address(address+24).value:blob=candidate;break
   if blob:
    h=HEADER.unpack_from(blob);written=h[9];first=max(cursor+1,written-4096+1)
    missed+=max(0,first-cursor-1)
    for serial in range(first,written+1):
     r=RECORD.unpack_from(blob,64+((serial-1)%4096)*128)
     if r[0]!=serial:raise RuntimeError('Ring serial mismatch')
     kind,fields=KINDS[r[5]] if r[5]<len(KINDS) else (f'unknown_{r[5]}',[f'value{i}' for i in range(r[6])]);values=dict(zip(fields,r[8:8+r[6]]));owner=f'0x{r[2]:08x}'
     item={'serial':serial,'qpc_seconds':r[1]/h[8],'owner_candidate':owner,'thread':r[3],'kind':kind,'valid':bool(r[7]&1),'values':values}
     if r[5]==6:item['component']=r[-1].split(b'\0')[0].decode('ascii','replace')
     owners.setdefault(owner,{})[kind]=values
     if output:output.write(json.dumps(item,allow_nan=True)+'\n')
    cursor=written
    if time.monotonic()-last_print>=2:
     print(json.dumps({'records':written,'hook_dropped':h[10],'reader_missed':missed,'candidates':owners}),flush=True);last_print=time.monotonic()
   time.sleep(.05)
  if output:output.flush()
 finally:
  if output:output.close()
  k.UnmapViewOfFile(address);k.CloseHandle(handle)
if __name__=='__main__':main()
