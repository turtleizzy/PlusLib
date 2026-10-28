#!/usr/bin/env python3
"""Read v1 sidecars without Plus/NDI dependencies. JSONL remains authoritative.

Checks envelope, raw CRC/length, null and identity rules, and final loss counters.
CSV is a convenience view; it must stay beside the source JSONL + summary.
Does not infer exposure times, ROM geometry, missing XYZ, or physical accuracy.
"""
import argparse
import collections
import csv
import json
import math
from pathlib import Path
import struct

SCHEMA='ndi.marker.capture.v1'
def crc16(data):
    value=0
    for byte in data:
        value^=byte
        for _ in range(8):value=(value>>1)^0xa001 if value&1 else value>>1
    return value

def require(condition,message):
    if not condition:raise ValueError(message)

def check_xyz(value):
    require(value is None or (isinstance(value,list) and len(value)==3 and
            all(isinstance(x,(float,int)) and not isinstance(x,bool) and math.isfinite(x) for x in value)),
            'XYZ must be null or three finite numbers')

def validate_record(r):
    require(r.get('schema')==SCHEMA,'unsupported schema')
    require(isinstance(r.get('session_id'),str) and r['session_id'],'missing session_id')
    kind=r.get('record_type')
    require(kind in ('session_start','measurement_reply','session_end'),'unknown record_type')
    if kind!='measurement_reply':return
    require(r.get('reply_flags')==0x080b,'unsupported marker reply flags')
    if 'request_association_verified' in r:
        require(r['request_association_verified'] is False,'BX cannot verify a transaction ID')
    if r.get('transport_state') in ('first_reply_quarantined','reconnect_required'):
        require(not r['reply_valid'] and r.get('round_trip_ns') is None,'uncertain transport cannot be a fresh valid reply/RTT')
    require(r.get('units')=='mm','expected mm')
    require(r.get('raw_measurement_timestamp') is None,'v1 BX has no device timestamp')
    for name in ('host_command_start_monotonic_ns','host_receive_monotonic_ns','host_receive_wall_unix_ns',
                 'host_decode_done_monotonic_ns','round_trip_ns'):
        value=r.get(name)
        require(value is None or isinstance(value,str) and value.isdecimal(),name+' must be null or decimal string')
    raw=bytes.fromhex(r['raw_reply_hex'])
    require(len(raw)==r['reply_wire_bytes'],'raw reply length mismatch')
    if r['reply_valid']:
        require(len(raw)>=11 and raw[:2]==b'\xc4\xa5','invalid binary BX header')
        length,hcrc=struct.unpack_from('<HH',raw,2)
        require(length+8==len(raw),'BX header length mismatch')
        require(crc16(raw[:4])==hcrc,'BX header CRC mismatch')
        require(crc16(raw[6:-2])==struct.unpack_from('<H',raw,len(raw)-2)[0],'BX payload CRC mismatch')
        require(r['header_crc_verified'] is True and r['payload_crc_verified'] is True,'missing CRC evidence')
    for t in r['tools']:
        frame=t['device_frame_number']
        require(frame is None or isinstance(frame,int) and 0<=frame<2**32,'invalid device frame')
        require(t['marker_device_frame_number'] is None and t['marker_device_timestamp'] is None,
                'independent marker frame/time unavailable in BX')
        if not r['reply_valid']:
            require(frame is None and not t['markers'] and not t['unidentified_measured_positions'] and t['pose'] is None,
                    'failed reply must not contain coordinates or old frame')
        for m in t['markers']:
            check_xyz(m['xyz_mm'])
            require(m['label']==chr(65+m['label_index_zero_based']),'marker label mismatch')
            if m['device_status']==0:require(m['xyz_mm'] is None,'missing marker must have null XYZ')
            if m['sample_valid']:
                require(m['xyz_mm'] is not None and m['device_status']==3 and m['out_of_volume'] is False,
                        'invalid point marked sample_valid')
        for m in t['unidentified_measured_positions']:
            check_xyz(m['xyz_mm'])
            require(m['label'] is None and m['cross_frame_identity']=='unknown','invented unknown-marker identity')

def read_records(path):
    records=[];session=None;ended=False
    with Path(path).open() as stream:
        for number,line in enumerate(stream,1):
            try:
                r=json.loads(line);validate_record(r)
                if session is None:
                    require(r['record_type']=='session_start','first line must be session_start')
                    session=r['session_id']
                require(r['session_id']==session,'session changed inside one JSONL')
                require(not ended,'record after session_end')
                ended=r['record_type']=='session_end';records.append(r)
            except (ValueError,KeyError,TypeError) as error:raise ValueError(f'{path}:{number}: {error}') from error
    require(bool(records),'empty capture')
    return records

def validate_summary(summary,records):
    require(summary['schema']==SCHEMA and summary['record_type']=='summary','invalid summary')
    require(summary['session_id']==records[0]['session_id'],'summary/session mismatch')
    c=summary['counters']
    losses=sum(c[k] for k in ('dropped_queue_full','dropped_queue_busy','dropped_allocation','rejected_not_accepting'))
    require(c['attempted']==c['enqueued']+losses,'enqueue counters do not balance')
    require(c['enqueued']==c['written']+c['discarded_after_write_failure'],'write counters do not balance')
    require(c['written']==sum(r['record_type']=='measurement_reply' for r in records),'written count mismatch')
    if summary['storage_complete']:
        require(not losses and not c['discarded_after_write_failure'] and summary['error'] is None,'complete with record loss')
        require(records[-1]['record_type']=='session_end','complete without session_end')

def describe(records):
    counts=collections.Counter();tools={};latencies=[]
    for r in records:
        if r['record_type']!='measurement_reply':continue
        counts['polls']+=1;counts['valid_replies' if r['reply_valid'] else 'error_replies']+=1
        if r['round_trip_ns'] is not None:latencies.append(int(r['round_trip_ns'])/1e6)
        for t in r['tools']:
            key='/'.join(str(x) for x in (r['connection_epoch'],t['binding_epoch'],t['transform_name'],t['handle']))
            info=tools.setdefault(key,collections.Counter());info[t['frame_classification']]+=1
            info['valid_marker_samples_in_new_tool_frames']+=sum(m['sample_valid'] for m in t['markers']) if t['is_new_device_frame'] is True else 0
    latencies.sort()
    def quantile(q):return latencies[min(int(q*(len(latencies)-1)),len(latencies)-1)] if latencies else None
    return {'source_provenance':records[0]['source_provenance'],'counts':dict(counts),'tool_counter_classification':tools,
            'host_round_trip_ms':{'min':quantile(0),'median':quantile(.5),'p95':quantile(.95),'max':quantile(1)},
            'timing_limit':'host round trip, not exposure timestamp; PTY fixtures are not hardware evidence'}

FIELDS=['session_id','connection_epoch','host_poll_sequence','binding_epoch','handle','transform_name',
        'device_frame_number','frame_classification','is_new_device_frame','host_receive_monotonic_ns',
        'host_receive_wall_unix_ns','marker_label','reply_local_index','state','device_status','sample_valid',
        'out_of_volume','x_mm','y_mm','z_mm']
def write_csv(records,path):
    with Path(path).open('x',newline='') as stream:
        writer=csv.DictWriter(stream,FIELDS);writer.writeheader()
        for r in records:
            if r['record_type']!='measurement_reply':continue
            for t in r['tools']:
                base={k:r[k] for k in FIELDS[:3]+['host_receive_monotonic_ns','host_receive_wall_unix_ns']}
                base.update({k:t[k] for k in ['binding_epoch','handle','transform_name','device_frame_number','frame_classification','is_new_device_frame']})
                points=[dict(m,marker_label=m['label']) for m in t['markers']]
                points += [dict(m,marker_label=None,state='identity_unknown',sample_valid=False) for m in t['unidentified_measured_positions']]
                if not points:points=[dict(state='unavailable_this_poll',xyz_mm=None,sample_valid=False)]
                for m in points:
                    row=dict(base,**{k:m.get(k) for k in ['marker_label','reply_local_index','state','device_status','sample_valid','out_of_volume']})
                    xyz=m['xyz_mm'];row.update(zip(['x_mm','y_mm','z_mm'],xyz if xyz is not None else [None]*3));writer.writerow(row)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('jsonl',type=Path)
    p.add_argument('--summary',type=Path);p.add_argument('--csv',type=Path)
    args=p.parse_args();records=read_records(args.jsonl)
    if args.summary:validate_summary(json.loads(args.summary.read_text()),records)
    if args.csv:write_csv(records,args.csv)
    print(json.dumps(describe(records),indent=2))
if __name__=='__main__':main()
