/* Local measured-marker recorder. See Plus License.txt. */
#include "PlusNDIMarkerRecorder.h"
#include "PlusNDIRaw.h"
#include <vtksys/MD5.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
namespace {
std::string quote(const std::string& s)
{
  std::ostringstream o;o<<'"';
  for(unsigned char c:s){
    switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;
      case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;
      default:if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<char(c);}
  }
  o<<'"';return o.str();
}
// Small JSON emitter only, not a parser; no external JSON dependency.
struct J {
  std::string s; char close=0;bool first=true;
  J():s("null"){}
  explicit J(const std::string& v):s(quote(v)){}
  static J object(){J j;j.s="{";j.close='}';return j;}
  static J array(){J j;j.s="[";j.close=']';return j;}
  static J boolean(bool b){J j;j.s=b?"true":"false";return j;}
  static J integer(uint64_t n){J j;j.s=std::to_string(n);return j;}
  static J number(double n){J j;if(std::isfinite(n)){std::ostringstream o;o<<std::setprecision(17)<<n;j.s=o.str();}return j;}
  static J time(uint64_t n){return n?J(std::to_string(n)):J();}
  std::string dump()const{return close?s+close:s;}
  J& add(const std::string& key,const J& v){if(!first)s+=',';first=false;s+=quote(key)+":"+v.dump();return *this;}
  J& append(const J& v){if(!first)s+=',';first=false;s+=v.dump();return *this;}
};
uint64_t ns(clockid_t id){timespec t;if(clock_gettime(id,&t))return 0;return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
std::string hex(const unsigned char* p,unsigned n){static const char* h="0123456789abcdef";std::string s;s.reserve(n*2);for(unsigned i=0;i<n;++i){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;}
bool writeAll(int fd,const std::string& s){size_t n=0;while(n<s.size()){ssize_t r=write(fd,s.data()+n,s.size()-n);if(r<0&&errno==EINTR)continue;if(r<=0)return false;n+=size_t(r);}return true;}
J floats(const float* p,unsigned n){J j=J::array();for(unsigned i=0;i<n;++i)j.append(J::number(p[i]));return j;}
J rom(const PlusNDIMarkerBinding& b){
  if(b.RomPath.empty())return J();
  return J::object().add("path",J(b.RomPath)).add("file_bytes",J::integer(b.RomBytes))
    .add("content_hash_algorithm",J("md5")).add("content_hash",J(b.RomMD5))
    .add("scope",J("original_file_bytes_not_ROM_geometry"));
}
struct Packet {ndiBXCapture Reply;std::vector<PlusNDIMarkerBinding> Bindings;uint64_t Epoch=0,Sequence=0;
 std::shared_ptr<const PlusNDISnapshot> Snapshot;
 const ndiBXCapture& reply()const{return Snapshot?Snapshot->Reply:Reply;}
 const std::vector<PlusNDIMarkerBinding>& bindings()const{return Snapshot?Snapshot->Bindings:Bindings;}
 uint64_t epoch()const{return Snapshot?Snapshot->Epoch:Epoch;}
 uint64_t sequence()const{return Snapshot?Snapshot->LegacySequence:Sequence;}
};
struct FrameState {unsigned Handle=0,Frame=0;uint64_t Connection=0,Binding=0,CounterEpoch=1;bool HaveFrame=false;std::string Rom,Block;};
}
struct PlusNDIMarkerRecorder::Impl
{
  std::mutex Mutex;std::condition_variable Ready;std::deque<std::unique_ptr<Packet>> Queue;std::thread Worker;
  enum Lifecycle {NeverStarted,Starting,Recording,StartupFailed,FinalizedComplete,FinalizedIncomplete};
  static constexpr uint64_t Closed=uint64_t(1)<<63;
  std::atomic<int> Phase{NeverStarted};std::atomic<uint64_t> ProducerGate{Closed};
  std::atomic<uint64_t> OutsideSession{0};
  std::atomic<bool> Failed{false};bool Accepting=false,Stopping=false,Started=false,OwnDirectory=false,TerminalResult=false;unsigned Capacity=64;
  mutable std::mutex ErrorMutex;
  std::atomic<uint64_t> Attempted{0},Enqueued{0},Written{0},DropFull{0},DropBusy{0},DropAllocation{0},Rejected{0},Discarded{0};
  uint64_t ErrorReplies=0,Quarantined=0,Repeats=0,Wraps=0,Resets=0,NextBinding=0;
  std::map<std::string,FrameState> States;std::map<unsigned,std::string> Owners;
  std::string Path,Session,TrackerFrame,LastError;int FD=-1;
  void failure(const std::string& why){
    {std::lock_guard<std::mutex> lock(ErrorMutex);if(LastError.empty())LastError=why;}
    Failed=true;std::cerr<<"NDI_MARKER_SIDECAR_ERROR: "<<why<<std::endl;
  }
  std::string error()const{std::lock_guard<std::mutex> lock(ErrorMutex);return LastError;}
  void reset(){
    // Lifecycle owner is serialized; no producer can enter while this gate is closed.
    Accepting=Stopping=Started=OwnDirectory=TerminalResult=false;Failed=false;
    Attempted=Enqueued=Written=DropFull=DropBusy=DropAllocation=Rejected=Discarded=0;
    ErrorReplies=Quarantined=Repeats=Wraps=Resets=NextBinding=0;States.clear();Owners.clear();Queue.clear();
    Path.clear();Session.clear();{std::lock_guard<std::mutex> lock(ErrorMutex);LastError.clear();}
    FD=-1;Phase=Starting;
  }
  bool finalize(bool startup){
    int summaryFD=-1;bool saved=false,complete=false;
    try {
      if(FD>=0){
        if(!Failed&&!writeAll(FD,prefix("session_end").add("counters",counters()).dump()+"\n"))failure("session footer write failed");
        if(fsync(FD))failure("data fsync failed");
        if(close(FD))failure("data close failed");FD=-1;
      }
      const bool conserved=Attempted==Enqueued+DropFull+DropBusy+DropAllocation+Rejected && Enqueued==Written+Discarded;
      complete=!startup&&!Failed&&conserved&&DropFull==0&&DropBusy==0&&DropAllocation==0&&Rejected==0&&Discarded==0;
      if(!complete&&!Failed)failure("archive incomplete: dropped/rejected records or counter conservation failure");
      if(OwnDirectory){
        summaryFD=open((Path+"/summary.pending.json").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        saved=summaryFD>=0;
        if(saved){
          const auto lastError=error();
          J summary=prefix("summary");summary.add("storage_complete",J::boolean(complete)).add("counters",counters())
            .add("error",lastError.empty()?J():J(lastError)).add("counter_conservation",J::boolean(conserved))
            .add("producer_boundary",J("sealed_before_final_counters"))
            .add("lifecycle",J(startup?"startup_failed":(complete?"finalized_complete":"finalized_incomplete")));
          saved=writeAll(summaryFD,summary.dump()+"\n");
          if(fsync(summaryFD))saved=false;if(close(summaryFD))saved=false;summaryFD=-1;
        }
        if(saved&&link((Path+"/summary.pending.json").c_str(),(Path+"/summary.json").c_str()))saved=false;
        if(saved)unlink((Path+"/summary.pending.json").c_str());
        if(!saved)failure("summary write/fsync/close/publish failed; archive incomplete");
      }
    }catch(const std::exception& e){failure(std::string("archive finalization exception: ")+e.what());}
     catch(...){failure("unknown archive finalization exception");}
    if(FD>=0){close(FD);FD=-1;}if(summaryFD>=0)close(summaryFD);
    Started=Accepting=false;TerminalResult=complete&&saved&&!Failed;
    Phase=startup?StartupFailed:(TerminalResult?FinalizedComplete:FinalizedIncomplete);
    return TerminalResult;
  }
  J prefix(const std::string& type)const{return J::object().add("schema",J("ndi.marker.capture.v1")).add("record_type",J(type)).add("session_id",J(Session));}
  J counters()const{
    return J::object().add("attempted",J::integer(Attempted)).add("enqueued",J::integer(Enqueued))
      .add("written",J::integer(Written)).add("dropped_queue_full",J::integer(DropFull))
      .add("dropped_queue_busy",J::integer(DropBusy)).add("dropped_allocation",J::integer(DropAllocation))
      .add("rejected_not_accepting",J::integer(Rejected)).add("discarded_after_write_failure",J::integer(Discarded))
      .add("error_replies",J::integer(ErrorReplies)).add("quarantined_replies",J::integer(Quarantined)).add("repeat_tool_frames",J::integer(Repeats))
      .add("counter_wraps",J::integer(Wraps)).add("counter_reset_or_out_of_order",J::integer(Resets));
  }
  J tool(const Packet& p,const PlusNDIMarkerBinding& b,const ndiBXCapturedTool* t){
    bool have=t&&(p.reply().valid||(p.Snapshot&&p.reply().reply_decoded));
    const PlusNDIRawToolState* meta=nullptr;
    if(p.Snapshot)for(size_t i=0;i<p.Snapshot->Bindings.size();++i)if(p.Snapshot->Bindings[i].Handle==b.Handle&&p.Snapshot->Bindings[i].TransformName==b.TransformName){meta=&p.Snapshot->ToolStates[i];break;}
    J o=J::object();
    o.add("handle",b.Handle?J::integer(b.Handle):J()).add("transform_name",b.TransformName.empty()?J():J(b.TransformName))
      .add("body_frame",b.BodyFrame.empty()?J():J(b.BodyFrame)).add("rom",rom(b))
      .add("rom_marker_membership",J("unavailable_without_ROM_geometry_interpretation"))
      .add("handle_status",have?J::integer(t->handle_status):J())
      .add("port_status",have&&t->frame_available?J::integer(t->port_status):J())
      .add("device_frame_number",have&&t->frame_available?J::integer(t->frame_number):J())
      .add("frame_number_bits",J::integer(32)).add("frame_modulus",J::integer(4294967296ULL));
    std::string key=b.TransformName.empty()?"unknown-handle-"+std::to_string(b.Handle):b.TransformName;
    auto& s=States[key];
    std::string classification="unavailable";J fresh,delta;bool changed=false;
    if(meta){
      s.Binding=meta->BindingEpoch;s.CounterEpoch=meta->CounterEpoch;
      const auto& c=meta->Classification;
      classification=c=="FIRST"?"first_frame":c=="NEW"?"new_frame":c=="REPEAT"?"repeated_frame":c=="WRAP_CANDIDATE"?"counter_wrap":c=="RESET_OR_OUT_OF_ORDER"?"counter_reset_or_out_of_order":"unavailable";
      fresh=meta->HaveNew?J::boolean(meta->IsNew):J();delta=meta->HaveDelta?J::integer(meta->Delta):J();changed=meta->Changed;
      if(c=="REPEAT")++Repeats;if(c=="WRAP_CANDIDATE")++Wraps;if(c=="RESET_OR_OUT_OF_ORDER")++Resets;
      o.add("raw_counter_classification",J(c)).add("raw_recovery_state",J(p.Snapshot->Quarantined||meta->Quarantined?"QUARANTINED":"SYNCHRONIZED"));
    }else {
    if(s.Connection!=p.epoch()||s.Handle!=b.Handle||s.Rom!=b.RomMD5||Owners[b.Handle]!=key){
      s=FrameState();s.Connection=p.epoch();s.Handle=b.Handle;s.Rom=b.RomMD5;s.Binding=++NextBinding;Owners[b.Handle]=key;
    }
    if(have&&t->frame_available){
      std::string block(reinterpret_cast<const char*>(p.reply().raw+t->wire_offset),t->wire_length);
      if(!s.HaveFrame){classification="first_frame";fresh=J::boolean(true);}
      else {uint32_t d=t->frame_number-s.Frame;delta=J::integer(d);
        if(!d){classification="repeated_frame";fresh=J::boolean(false);changed=block!=s.Block;++Repeats;}
        else if(d<0x80000000u){classification=t->frame_number<s.Frame?"counter_wrap":"new_frame";fresh=J::boolean(true);if(t->frame_number<s.Frame)++Wraps;}
        else{classification="counter_reset_or_out_of_order";++s.CounterEpoch;++Resets;}
      }
      s.HaveFrame=true;s.Frame=t->frame_number;s.Block=block;
    }
    }
    o.add("binding_epoch",J::integer(s.Binding)).add("counter_epoch",J::integer(s.CounterEpoch))
      .add("frame_classification",J(classification)).add("is_new_device_frame",fresh).add("device_counter_delta",delta)
      .add("repeated_frame_payload_changed",J::boolean(changed))
      .add("frame_association",J("same_BX_tool_block;marker_exposure_frame_not_independently_reported"))
      .add("marker_device_frame_number",J()).add("marker_device_timestamp",J())
      .add("freshness_classification_basis",J("reported_tool_frame_counter;not_an_independent_marker_timestamp"));
    J pose;
    if(have&&t->pose_present)pose=J::object().add("quaternion_wxyz",floats(t->pose,4))
      .add("translation_mm",floats(t->pose+4,3)).add("fit_error_mm",J::number(t->pose[7]));
    o.add("pose",pose);
    J pub;
    if(b.Published){
      J matrix=J::array();for(double x:b.Matrix)matrix.append(J::number(x));
      pub=J::object().add("status",J(b.PlusStatus)).add("frame_number",J::integer(b.PlusFrame))
        .add("frame_number_source",J(b.PlusFrameSource)).add("host_unfiltered_time_seconds",J::number(b.PlusHostTime))
        .add("T_tracker_from_tool_row_major",matrix).add("network_packet_time_association",J("unavailable"));
    }
    o.add("plus_buffer_publication",pub).add("tool_information",have&&t->additional_info_available?J::integer(t->tool_information):J())
      .add("marker_identity_mapping",J(!have?"unavailable":(t->marker_identity_known?"matched_0002_nonmissing_A_to_T_slots":"unknown_count_mismatch_or_not_reported")));
    J statuses;
    if(have&&t->additional_info_available){statuses=J::array();for(unsigned j=0;j<20;++j)statuses.append(J::integer(t->marker_status[j]));}
    o.add("marker_status_slots",statuses);
    J markers=J::array(),unknown=J::array();
    if(have&&t->additional_info_available){
      for(int label=0;label<20;++label){
        const ndiBXCapturedMarker* m=nullptr;
        if(t->marker_identity_known)for(unsigned j=0;j<t->marker_count;++j)if(t->markers[j].label_index==label)m=&t->markers[j];
        unsigned status=t->marker_status[label];
        std::string state=m?(m->finite?(m->out_of_volume?"measured_out_of_volume":(status==3?"measured":"measured_not_used_for_pose")):"nonfinite_device_coordinate"):(status==0?"missing_or_not_defined":"coordinate_identity_unavailable");
        markers.append(J::object().add("label",J(std::string(1,char('A'+label))))
          .add("label_index_zero_based",J::integer(label)).add("device_status",J::integer(status))
          .add("detected",J::boolean(status!=0)).add("xyz_mm",m&&m->finite?floats(m->xyz,3):J())
          .add("out_of_volume",m?J::boolean(m->out_of_volume):J())
          .add("used_for_tool_pose",J::boolean(status==3||status==4))
          .add("sample_valid",J::boolean(m&&m->finite&&!m->out_of_volume&&status==3&&(!p.Snapshot||(!p.Snapshot->Quarantined&&meta&&!meta->Quarantined)))).add("state",J(state)));
      }
    }
    if(have&&!t->marker_identity_known)for(unsigned j=0;j<t->marker_count;++j){
      const auto& m=t->markers[j];
      unknown.append(J::object().add("reply_local_index",J::integer(j)).add("label",J())
        .add("xyz_mm",m.finite?floats(m.xyz,3):J()).add("out_of_volume",J::boolean(m.out_of_volume)).add("cross_frame_identity",J("unknown")));
    }
    return o.add("markers",markers).add("unidentified_measured_positions",unknown);
  }
  std::string serialize(const Packet& p){
    const auto& r=p.reply();if(r.transport_state==NDI_BX_FIRST_REPLY_QUARANTINED)++Quarantined;else if(!r.valid)++ErrorReplies;
    J o=prefix("measurement_reply");
    o.add("connection_epoch",J::integer(p.epoch())).add("host_poll_sequence",J::integer(p.sequence()))
      .add("reply_flags",J::integer(r.reply_flags)).add("reply_valid",J::boolean(r.valid))
      .add("reply_decoded",J::boolean(r.reply_decoded)).add("request_sent",J::boolean(r.request_sent))
      .add("transport_state",J(r.transport_state==NDI_BX_RECONNECT_REQUIRED?"reconnect_required":(r.transport_state==NDI_BX_FIRST_REPLY_QUARANTINED?"first_reply_quarantined":(r.transport_state==NDI_BX_ORDERED_UNVERIFIED?"serialized_order_assumed":"decoder_only_no_transport"))))
      .add("request_association_verified",J::boolean(false))
      .add("error_code",J::integer(r.error_code)).add("device_error_code",r.device_error_code?J::integer(r.device_error_code):J())
      .add("error",r.error_code?J(r.error_message):J())
      .add("capability",J(r.device_error_code==1||r.device_error_code==7?"device_rejected_request":(r.valid?"reply_received":"unavailable_this_poll")))
      .add("raw_measurement_timestamp",J()).add("raw_measurement_timestamp_source",J("unavailable_in_BX"))
      .add("host_command_start_monotonic_ns",J::time(r.command_start_monotonic_ns))
      .add("host_receive_monotonic_ns",J::time(r.receive_monotonic_ns)).add("host_receive_wall_unix_ns",J::time(r.receive_wall_ns))
      .add("host_decode_done_monotonic_ns",J::time(r.decode_done_monotonic_ns))
      .add("round_trip_ns",J::time(r.transport_state==NDI_BX_ORDERED_UNVERIFIED&&r.request_sent&&r.receive_monotonic_ns>=r.command_start_monotonic_ns?r.receive_monotonic_ns-r.command_start_monotonic_ns:0))
      .add("command_wire_bytes",r.command_write_count_known?J::integer(r.command_wire_bytes):J()).add("command_write_count_known",J::boolean(r.command_write_count_known)).add("reply_wire_bytes",J::integer(r.raw_length))
      .add("header_crc_verified",r.header_crc_valid<0?J():J::boolean(r.header_crc_valid))
      .add("payload_crc_verified",r.body_crc_valid<0?J():J::boolean(r.body_crc_valid))
      .add("raw_reply_hex",J(hex(r.raw,r.raw_length))).add("tracker_frame",J(TrackerFrame))
      .add("coordinate_source",J("NDI_BX_0008_measured_float32_in_position_sensor_frame")).add("units",J("mm"))
      .add("system_status",r.valid?J::integer(r.system_status):J());
    J tools=J::array();std::vector<bool> mapped(r.tool_count,false);
    for(const auto& b:p.bindings()){
      const ndiBXCapturedTool* t=nullptr;
      for(unsigned i=0;i<r.tool_count;++i)if(r.tools[i].handle==b.Handle){t=&r.tools[i];mapped[i]=true;}
      tools.append(tool(p,b,t));
    }
    for(unsigned i=0;i<r.tool_count;++i)if(!mapped[i]){PlusNDIMarkerBinding b;b.Handle=r.tools[i].handle;tools.append(tool(p,b,&r.tools[i]));}
    o.add("tools",tools).add("recorder_counters",counters());
    if(p.Snapshot){
      J data;data.s=PlusNDIRawWire::Data(*p.Snapshot);
      o.add("frame_id",J(p.Snapshot->AcquisitionId+":"+std::to_string(p.Snapshot->Sequence)))
       .add("reply_seq",J(std::to_string(p.Snapshot->Sequence))).add("acquisition_id",J(p.Snapshot->AcquisitionId))
       .add("clock_domain_id",J(p.Snapshot->ClockDomainId)).add("raw_v1_snapshot_data",data);
    }
    return o.dump()+"\n";
  }
  void worker(){
    for(;;){
      std::unique_ptr<Packet> p;
      {std::unique_lock<std::mutex> lock(Mutex);Ready.wait(lock,[this]{return Stopping||!Queue.empty();});
       if(Queue.empty()&&Stopping)break;p=std::move(Queue.front());Queue.pop_front();}
      if(Failed){++Discarded;continue;}
      try{if(!writeAll(FD,serialize(*p))){failure("write failed: "+std::string(std::strerror(errno)));++Discarded;}else ++Written;}
      catch(const std::exception& e){failure(std::string("serialization/write exception: ")+e.what());++Discarded;}
    }
  }
};
PlusNDIMarkerRecorder::PlusNDIMarkerRecorder():P(new Impl){}
PlusNDIMarkerRecorder::~PlusNDIMarkerRecorder(){Stop();}
bool PlusNDIMarkerRecorder::IdentifyRom(const std::string& path,std::string& digest,uint64_t& size){
  std::ifstream f(path.c_str(),std::ios::binary);if(!f)return false;
  auto* h=vtksysMD5_New();vtksysMD5_Initialize(h);char block[4096];size=0;
  while(f){f.read(block,sizeof(block));auto n=f.gcount();if(n){vtksysMD5_Append(h,reinterpret_cast<unsigned char*>(block),int(n));size+=uint64_t(n);}}
  char result[33]={0};vtksysMD5_FinalizeHex(h,result);vtksysMD5_Delete(h);digest=result;return !f.bad()&&size>0;
}
bool PlusNDIMarkerRecorder::Start(const std::string& parent,const std::string& tracker,unsigned capacity,double rate,unsigned baud,std::string& error,const std::string& sourceEndpoint){
  std::lock_guard<std::mutex> lifecycle(LifecycleMutex);
  if(P->Started){error="recorder already started";return false;}
  // Impl stays at one stable address. Sealed Enqueue calls cannot race a P.reset().
  P->reset();P->Capacity=capacity;
  struct stat st;
  if(parent.empty()||parent[0]!='/'||capacity<1||capacity>1024||stat(parent.c_str(),&st)||!S_ISDIR(st.st_mode)){
    error="existing absolute sidecar parent and queue capacity 1..1024 required";
    P->failure(error);P->finalize(true);return false;
  }
  static std::atomic<uint64_t> nextSession{0};
  try {
    P->TrackerFrame=tracker;
    P->Session=std::to_string(ns(CLOCK_REALTIME))+"-"+std::to_string(getpid())+"-"+std::to_string(++nextSession);
    P->Path=parent+"/ndi-marker-"+P->Session;
    if(mkdir(P->Path.c_str(),0700))throw std::runtime_error("cannot exclusively create session directory");
    P->OwnDirectory=true;
    P->FD=open((P->Path+"/measurements.jsonl").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(P->FD<0)throw std::runtime_error("cannot open sidecar file");
    const char* delay=getenv("NDICAPI_SERIAL_WRITE_DELAY_US");
    J start=P->prefix("session_start");
    start.add("reply_flags",J::integer(NDI_BX_CAPTURE_FLAGS)).add("device_frame_bits",J::integer(32))
      .add("frame_modulus",J::integer(4294967296ULL)).add("requested_acquisition_rate_hz",J::number(rate))
      .add("requested_baud",J::integer(baud)).add("serial_write_delay_environment_us",delay?J(delay):J())
      .add("delay_semantics",J("inter_byte_not_inter_command")).add("raw_measurement_timestamp",J())
      .add("network_time_alignment",J("unavailable")).add("serial_endpoint",J(sourceEndpoint))
      .add("source_provenance",J(sourceEndpoint.find("/dev/pts/")==0?"synthetic_PTY_transport":(sourceEndpoint=="synthetic_fixture"?"synthetic_fixture":(sourceEndpoint.empty()?"unknown":"serial_device_reply_not_physically_authenticated"))))
      .add("request_association_policy",J("fail_closed_after_error;first_reply_quarantined;serialized_order_assumed_no_transaction_id"))
      .add("marker_identity",J("0002 alphabetic slots matched to 0008 detected positions; no ROM geometry used"));
    if(!writeAll(P->FD,start.dump()+"\n"))throw std::runtime_error("session header write failed");
    // Publish Started/Accepting and open the producer gate only AFTER construction.
    P->Worker=std::thread([this]{P->worker();});
    P->Started=P->Accepting=true;P->Phase=Impl::Recording;
    P->ProducerGate.store(0,std::memory_order_release);
    error.clear();return true;
  } catch(const std::exception& e) { P->failure(e.what()); }
    catch(...) { P->failure("unknown recorder startup exception"); }
  P->Accepting=P->Started=false;P->Stopping=true;
  P->ProducerGate.store(Impl::Closed,std::memory_order_release);
  P->finalize(true);error=P->error();return false;
}
bool PlusNDIMarkerRecorder::Enqueue(const ndiBXCapture& reply,const std::vector<PlusNDIMarkerBinding>& bindings,uint64_t epoch,uint64_t sequence){
  // A call belongs to this session iff it enters the gate before Stop seals it.
  // Calls after the seal return false without mutating finalized session counters.
  uint64_t gate=P->ProducerGate.load(std::memory_order_acquire);
  do {if(gate&Impl::Closed){++P->OutsideSession;return false;}}
  while(!P->ProducerGate.compare_exchange_weak(gate,gate+1,std::memory_order_acq_rel));
  struct ProducerExit {Impl* State;~ProducerExit(){
    // Pair the final count transition with Stop's CV mutex: a notification
    // outside this mutex can be lost between the predicate check and wait.
    std::lock_guard<std::mutex> lock(State->Mutex);
    State->ProducerGate.fetch_sub(1,std::memory_order_acq_rel);State->Ready.notify_all();
  }} producer{P.get()};
  ++P->Attempted;
  std::unique_ptr<Packet> p;
  try{p.reset(new Packet);p->Reply=reply;p->Bindings=bindings;p->Epoch=epoch;p->Sequence=sequence;}
  catch(...){++P->DropAllocation;return false;}
  std::unique_lock<std::mutex> lock(P->Mutex,std::try_to_lock);
  if(!lock.owns_lock()){++P->DropBusy;return false;}
  if(!P->Accepting||P->Failed){++P->Rejected;return false;}
  if(P->Queue.size()>=P->Capacity){++P->DropFull;return false;}
  try{P->Queue.push_back(std::move(p));}
  catch(...){++P->DropAllocation;return false;}
  ++P->Enqueued;lock.unlock();P->Ready.notify_one();return true;
}
bool PlusNDIMarkerRecorder::Enqueue(const std::shared_ptr<const PlusNDISnapshot>& snapshot){
  // A call belongs to this session iff it enters the gate before Stop seals it.
  // Calls after the seal return false without mutating finalized session counters.
  uint64_t gate=P->ProducerGate.load(std::memory_order_acquire);
  do {if(gate&Impl::Closed){++P->OutsideSession;return false;}}
  while(!P->ProducerGate.compare_exchange_weak(gate,gate+1,std::memory_order_acq_rel));
  struct ProducerExit {Impl* State;~ProducerExit(){
    // Pair the final count transition with Stop's CV mutex: a notification
    // outside this mutex can be lost between the predicate check and wait.
    std::lock_guard<std::mutex> lock(State->Mutex);
    State->ProducerGate.fetch_sub(1,std::memory_order_acq_rel);State->Ready.notify_all();
  }} producer{P.get()};
  ++P->Attempted;
  std::unique_ptr<Packet> p;
  try{p.reset(new Packet);p->Snapshot=snapshot;}
  catch(...){++P->DropAllocation;return false;}
  std::unique_lock<std::mutex> lock(P->Mutex,std::try_to_lock);
  if(!lock.owns_lock()){++P->DropBusy;return false;}
  if(!P->Accepting||P->Failed){++P->Rejected;return false;}
  if(P->Queue.size()>=P->Capacity){++P->DropFull;return false;}
  try{P->Queue.push_back(std::move(p));}
  catch(...){++P->DropAllocation;return false;}
  ++P->Enqueued;lock.unlock();P->Ready.notify_one();return true;
}
bool PlusNDIMarkerRecorder::Stop(const std::string& why){
  std::lock_guard<std::mutex> lifecycle(LifecycleMutex);
  if(!P->Started)return P->Phase==Impl::NeverStarted ? true : P->TerminalResult;
  P->ProducerGate.fetch_or(Impl::Closed,std::memory_order_acq_rel);
  {
    std::unique_lock<std::mutex> lock(P->Mutex);
    P->Ready.wait(lock,[this]{return P->ProducerGate.load(std::memory_order_acquire)==Impl::Closed;});
    P->Accepting=false;P->Stopping=true;
  }
  P->Ready.notify_all();if(P->Worker.joinable())P->Worker.join();
  if(!why.empty())P->failure(why);
  return P->finalize(false);
}
bool PlusNDIMarkerRecorder::Healthy()const{return !P->Failed&&P->DropFull==0&&P->DropBusy==0&&P->DropAllocation==0&&P->Rejected==0&&P->Discarded==0;}
std::string PlusNDIMarkerRecorder::Error()const{return P->error();}
std::string PlusNDIMarkerRecorder::Directory()const{std::lock_guard<std::mutex> lock(LifecycleMutex);return P->Path;}
std::string PlusNDIMarkerRecorder::StorageState()const{
  switch(P->Phase.load()){
    case Impl::NeverStarted:return "never_started";
    case Impl::Starting:return "starting";
    case Impl::Recording:return Healthy()?"recording":"recording_incomplete";
    case Impl::StartupFailed:return "startup_failed";
    case Impl::FinalizedComplete:return "finalized_complete";
    default:return "finalized_incomplete";
  }
}

uint64_t PlusNDIMarkerRecorder::RejectedOutsideSession()const{return P->OutsideSession.load();}
