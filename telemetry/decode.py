"""Decode correlated extended DLL records; native coordinates retained."""
import argparse,json,math
from pathlib import Path
def snapshots(rows):
 pending={}
 for r in rows:
  if r['kind']=='drivetrain_raw' and r['valid']:
   pending={'qpc_seconds':r['qpc_seconds'],'context':r['owner_candidate'],'drive':r['values']}
  elif pending and r['qpc_seconds']==pending['qpc_seconds'] and r['valid']:
   pending[r['kind']]=r['values']
   if r['kind']=='force_accumulator_raw':
    required=['body_position_velocity_raw','angular_matrix_row0_raw','matrix_rows12_raw']
    if not all(x in pending for x in required):continue
    d=pending['drive'];pv=pending[required[0]];ar=pending[required[1]];mr=pending[required[2]]
    matrix=[ar[x] for x in ['m00','m01','m02']]+[mr[x] for x in ['m10','m11','m12','m20','m21','m22']]
    v=[pv[x] for x in ['vx','vy','vz']];speed=math.sqrt(sum(x*x for x in v))
    yield {'time':d['time'],'qpc_seconds':pending['qpc_seconds'],'context':pending['context'],'body':hex(int(d['body_pointer'])),'linked':hex(int(d['linked_pointer'])),'gear_index':int(d['gear_index']),'gear_target':int(d['gear_target']),'shift_state':int(d['shift_state']),'rpm':d['engine_rad_s']*60/(2*math.pi),'speed_native_units_per_second':speed,'speed_kmh_if_metres':speed*3.6,'speed_mph_if_metres':speed*2.2369362920544,'position_native':[pv[x] for x in ['px','py','pz']],'velocity_world_native':v,'angular_velocity_world_native':[ar[x] for x in ['wx','wy','wz']],'rotation_body_to_world_native':matrix,'force_accumulator_native':pending['force_accumulator_raw'],'player_identity_verified':False,'motion_output_enabled':False}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('capture',type=Path);ap.add_argument('--output',type=Path);args=ap.parse_args();rows=[json.loads(x) for x in args.capture.read_text().splitlines() if x.strip()];data=list(snapshots(rows))
 if args.output:args.output.write_text(''.join(json.dumps(x)+'\n' for x in data))
 print(json.dumps({'complete_body_snapshots':len(data),'latest':data[-1] if data else None},indent=2))
if __name__=='__main__':main()
