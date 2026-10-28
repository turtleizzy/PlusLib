/* Local ndi.raw.v1/minor=0 candidate. See Plus License.txt. */
#include "PlusNDIRaw.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
namespace {
uint64_t
ns()
{
  timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    throw std::runtime_error("CLOCK_MONOTONIC failed");
  return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
std::string
hex(const unsigned char* p, size_t n)
{
  const char* h = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += h[p[i] >> 4];
    s += h[p[i] & 15];
  }
  return s;
}
bool
validUtf8(const std::string& text)
{
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i++];
    if (c < 128)
      continue;
    unsigned length = c >= 0xc2 && c <= 0xdf   ? 2
                      : c >= 0xe0 && c <= 0xef ? 3
                      : c >= 0xf0 && c <= 0xf4 ? 4
                                               : 0;
    if (!length || text.size() - i < length - 1)
      return false;
    uint32_t cp = c & ((1u << (7 - length)) - 1);
    for (unsigned j = 1; j < length; ++j) {
      unsigned char tail = text[i++];
      if ((tail & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (tail & 63);
    }
    if (cp < (length == 2   ? 0x80u
              : length == 3 ? 0x800u
                            : 0x10000u) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
std::string
quote(const std::string& s)
{
  if (!validUtf8(s))
    throw std::invalid_argument("non-UTF8 string in raw snapshot");
  std::ostringstream o;
  o.imbue(std::locale::classic());
  o << '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\')
      o << '\\' << char(c);
    else if (c < 32)
      o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
    else
      o << char(c);
  }
  o << '"';
  return o.str();
}
struct J
{
  std::string s = "null";
  char close = 0;
  bool first = true;
  J() {}
  explicit J(const std::string& x)
    : s(quote(x))
  {
  }
  explicit J(const char* x)
    : s(quote(x))
  {
  }
  static J obj()
  {
    J j;
    j.s = "{";
    j.close = '}';
    return j;
  }
  static J arr()
  {
    J j;
    j.s = "[";
    j.close = ']';
    return j;
  }
  static J b(bool x)
  {
    J j;
    j.s = x ? "true" : "false";
    return j;
  }
  static J i(uint64_t x)
  {
    J j;
    j.s = std::to_string(x);
    return j;
  }
  static J d(uint64_t x) { return J(std::to_string(x)); }
  static J n(double x)
  {
    J j;
    if (std::isfinite(x)) {
      std::ostringstream o;
      o.imbue(std::locale::classic());
      o << std::setprecision(17) << x;
      j.s = o.str();
    }
    return j;
  }
  static J t(uint64_t x) { return x ? d(x) : J(); }
  std::string dump() const { return close ? s + close : s; }
  J& add(const std::string& k, const J& v)
  {
    if (!first)
      s += ',';
    first = false;
    s += quote(k) + ":" + v.dump();
    return *this;
  }
  J& push(const J& v)
  {
    if (!first)
      s += ',';
    first = false;
    s += v.dump();
    return *this;
  }
};
J
optional(const std::string& s)
{
  return s.empty() ? J() : J(s);
}
J
floats(const float* p, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    if (!std::isfinite(p[i]))
      return J();
  J a = J::arr();
  for (size_t i = 0; i < n; ++i)
    a.push(J::n(p[i]));
  return a;
}
J
rom(const PlusNDIMarkerBinding& b)
{
  return J::obj()
    .add("sha256", optional(b.RomSHA256))
    .add("md5", optional(b.RomMD5))
    .add("file_bytes", b.RomMD5.empty() ? J() : J::d(b.RomBytes))
    .add("geometry_interpreted", J::b(false));
}
J
binding(const PlusNDIMarkerBinding& b, uint64_t epoch)
{
  return J::obj()
    .add("tool_id", optional(b.TransformName))
    .add("handle", J::i(b.Handle))
    .add("transform_name", optional(b.TransformName))
    .add("body_frame", optional(b.BodyFrame))
    .add("binding_epoch", J::d(epoch))
    .add("rom", rom(b))
    .add("defined_slots", J())
    .add("defined_slots_source", J("UNAVAILABLE"));
}
J
prefix(const std::string& type,
       const std::string& source,
       const std::string& acq,
       const std::string& session,
       uint64_t seq)
{
  return J::obj()
    .add("schema", J("ndi.raw.v1"))
    .add("schema_minor", J::i(0))
    .add("record_type", J(type))
    .add("source_id", J(source))
    .add("acquisition_id", J(acq))
    .add("session_id", J(session))
    .add("seq", J::d(seq))
    .add("extensions", J::obj());
}
std::string
currentStatus(const ndiBXCapture& r, const ndiBXCapturedTool* t)
{
  if (!r.valid)
    return r.error_code == NDI_TIMEOUT ? "REQ_TIMEOUT" : "INVALID";
  if (!t)
    return "MISSING";
  const unsigned mask = NDI_TOOL_IN_PORT | NDI_INITIALIZED | NDI_ENABLED;
  if (!t->frame_available || (t->port_status & mask) != mask)
    return "MISSING";
  if (t->port_status & NDI_OUT_OF_VOLUME)
    return "OUT_OF_VOLUME";
  if (!t->pose_present)
    return "OUT_OF_VIEW";
  return "OK";
}
J
tool(const PlusNDISnapshot& p, size_t index, const ndiBXCapturedTool* t)
{
  const auto& b = p.Bindings[index];
  const auto& s = p.ToolStates[index];
  const auto& r = p.Reply;
  const bool reported = t != nullptr, sync = !p.Quarantined && !s.Quarantined;
  J o = binding(b, s.BindingEpoch);
  o.add("reported", J::b(reported))
    .add("additional_info_available", J::b(t && t->additional_info_available))
    .add("marker_identity_known", J::b(t && t->marker_identity_known))
    .add("marker_count_reported",
         t && (r.reply_flags & 8) && t->frame_available ? J::i(t->marker_count) : J())
    .add("recovery_state", J(sync ? "SYNCHRONIZED" : "QUARANTINED"))
    .add("handle_status", t ? J::i(t->handle_status) : J())
    .add("port_status", t && t->frame_available ? J::i(t->port_status) : J())
    .add("tool_information", t && t->additional_info_available ? J::i(t->tool_information) : J())
    .add("tool_status", J(s.Status))
    .add("tool_status_origin",
         J(!b.TransformName.empty() && (!r.valid || !b.PlusStatus.empty()) ? "PLUS_CURRENT_BX_STATUS"
                                                                           : "UNAVAILABLE"));
  o.add("counter",
        J::obj()
          .add("value", s.HaveCounter ? J::i(s.Counter) : J())
          .add("bits", J::i(32))
          .add("epoch", J::d(s.CounterEpoch))
          .add("classification", J(s.Classification))
          .add("is_new_device_frame", s.HaveNew ? J::b(s.IsNew) : J())
          .add("delta_modulo", s.HaveDelta ? J::i(s.Delta) : J())
          .add("repeated_payload_changed", J::b(s.Changed)));
  J pose;
  if (t && t->pose_present) {
    bool finite = true;
    double norm = 0;
    for (unsigned i = 0; i < 8; ++i)
      finite = finite && std::isfinite(t->pose[i]);
    for (unsigned i = 0; i < 4; ++i)
      norm += double(t->pose[i]) * t->pose[i];
    const bool unit = std::isfinite(norm) && std::abs(norm - 1) <= 1e-3;
    J reasons = J::arr();
    if (!r.valid)
      reasons.push(J("REPLY_INVALID"));
    if (!sync)
      reasons.push(J("QUARANTINED"));
    if (!finite)
      reasons.push(J("NONFINITE_DEVICE_POSE"));
    if (!unit)
      reasons.push(J("QUATERNION_NOT_UNIT"));
    if (s.Status != "OK")
      reasons.push(J("TOOL_STATUS_NOT_OK"));
    pose = J::obj()
             .add("finite", J::b(finite))
             .add("quaternion_wxyz", floats(t->pose, 4))
             .add("translation_mm", floats(t->pose + 4, 3))
             .add("fit_error_raw", J::n(t->pose[7]))
             .add("fit_error_unit", J("UNKNOWN"))
             .add("fit_error_basis",
                  J("Target firmware fit-error unit not verified; original wire float32 preserved"))
             .add("sample_valid", J::b(r.valid && sync && finite && unit && s.Status == "OK"))
             .add("invalid_reasons", reasons);
  }
  o.add("raw_pose", pose);
  J slots = J::arr(), unknown = J::arr();
  if (t && t->additional_info_available)
    for (unsigned label = 0; label < 20; ++label) {
      const ndiBXCapturedMarker* m = nullptr;
      if (t->marker_identity_known)
        for (unsigned j = 0; j < t->marker_count; ++j)
          if (t->markers[j].label_index == int(label))
            m = &t->markers[j];
      const bool valid =
        r.valid && sync && reported && m && m->finite && !m->out_of_volume && t->marker_status[label] == 3;
      J reasons = J::arr();
      if (!r.valid)
        reasons.push(J("REPLY_INVALID"));
      if (!sync)
        reasons.push(J("QUARANTINED"));
      if (!m)
        reasons.push(J("COORDINATE_OR_IDENTITY_UNAVAILABLE"));
      else {
        if (!m->finite)
          reasons.push(J("NONFINITE_DEVICE_COORDINATE"));
        if (m->out_of_volume)
          reasons.push(J("OUT_OF_VOLUME"));
      }
      if (t->marker_status[label] != 3)
        reasons.push(J("DEVICE_STATUS_NOT_3"));
      slots.push(J::obj()
                   .add("slot_index", J::i(label))
                   .add("slot_label", J(std::string(1, char(65 + label))))
                   .add("device_status", J::i(t->marker_status[label]))
                   .add("identity_known", J::b(t->marker_identity_known))
                   .add("coordinate_available", J::b(m != nullptr))
                   .add("finite", m ? J::b(m->finite) : J())
                   .add("xyz_mm", m && m->finite ? floats(m->xyz, 3) : J())
                   .add("out_of_volume", m ? J::b(m->out_of_volume) : J())
                   .add("visibility",
                        J(m ? (m->out_of_volume ? "MEASURED_OUT_OF_VOLUME" : "MEASURED_IN_VOLUME")
                            : (t->marker_status[label] == 0 ? "NOT_MEASURED" : "UNKNOWN")))
                   .add("sample_valid", J::b(valid))
                   .add("invalid_reasons", reasons));
    }
  if (t && !t->marker_identity_known)
    for (unsigned j = 0; j < t->marker_count; ++j) {
      const auto& m = t->markers[j];
      unknown.push(J::obj()
                     .add("reply_local_index", J::i(j))
                     .add("finite", J::b(m.finite))
                     .add("xyz_mm", m.finite ? floats(m.xyz, 3) : J())
                     .add("out_of_volume", J::b(m.out_of_volume)));
    }
  J submission;
  if (b.Published) {
    J matrix = J::arr();
    bool finite = true;
    for (double x : b.Matrix) {
      finite = finite && std::isfinite(x);
      matrix.push(J::n(x));
    }
    submission = J::obj()
                   .add("status", J(b.PlusStatus))
                   .add("frame_number", J::d(b.PlusFrame))
                   .add("frame_number_source", J(b.PlusFrameSource))
                   .add("host_unfiltered_time_seconds", J::n(b.PlusHostTime))
                   .add("T_tracker_from_tool_row_major", finite ? matrix : J())
                   .add("semantics", J("SUBMISSION_SNAPSHOT_NOT_BUFFER_OR_NETWORK_RECEIPT"));
  }
  return o.add("marker_slots", slots)
    .add("unidentified_positions", unknown)
    .add("plus_submission", submission);
}
void
be(std::string& s, uint64_t x, unsigned n)
{
  for (int i = int(n) - 1; i >= 0; --i)
    s += char(x >> (i * 8));
}
bool
uuidValid(const std::string& s)
{
  if (s.size() != 36)
    return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return false;
    } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                 (s[i] >= 'A' && s[i] <= 'F')))
      return false;
  }
  return true;
}
}
namespace PlusNDIRawWire {
std::string
UUID()
{
  unsigned char b[16];
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("UUID entropy unavailable");
  size_t have = 0;
  while (have < 16) {
    ssize_t n = read(fd, b + have, 16 - have);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      throw std::runtime_error("UUID entropy read failed");
    }
    have += size_t(n);
  }
  close(fd);
  b[6] = (b[6] & 15) | 64;
  b[8] = (b[8] & 63) | 128;
  auto h = hex(b, 16);
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" +
         h.substr(20);
}
std::string
Sha256(const unsigned char* p, size_t n)
{
  unsigned char h[SHA256_DIGEST_LENGTH];
  SHA256(p, n, h);
  return hex(h, sizeof(h));
}
std::string
Data(const PlusNDISnapshot& p)
{
  const auto& r = p.Reply;
  J timing = J::obj()
               .add("command_start_monotonic_ns", J::t(r.command_start_monotonic_ns))
               .add("receive_monotonic_ns", J::t(r.receive_monotonic_ns))
               .add("decode_done_monotonic_ns", J::t(r.decode_done_monotonic_ns))
               .add("receive_wall_unix_ns", J::t(r.receive_wall_ns))
               .add("round_trip_ns",
                    r.command_start_monotonic_ns && r.receive_monotonic_ns >= r.command_start_monotonic_ns
                      ? J::d(r.receive_monotonic_ns - r.command_start_monotonic_ns)
                      : J())
               .add("device_measurement_timestamp", J());
  J reply = J::obj()
              .add("flags", J::i(r.reply_flags))
              .add("valid", J::b(r.valid))
              .add("error_code", J::i(r.error_code))
              .add("device_error_code", r.device_error_code ? J::i(r.device_error_code) : J())
              .add("error_message", r.error_code ? J(r.error_message) : J())
              .add("command_wire_bytes", J::i(r.command_wire_bytes))
              .add("raw_reply_bytes", J::i(r.raw_length))
              .add("header_crc_verified", r.header_crc_valid < 0 ? J() : J::b(r.header_crc_valid))
              .add("payload_crc_verified", r.body_crc_valid < 0 ? J() : J::b(r.body_crc_valid))
              .add("raw_reply_hex", J(hex(r.raw, r.raw_length)))
              .add("raw_reply_sha256", J(Sha256(r.raw, r.raw_length)))
              .add("system_status", r.reply_decoded && !r.device_error_code ? J::i(r.system_status) : J());
  J tools = J::arr();
  for (size_t i = 0; i < p.Bindings.size(); ++i) {
    const ndiBXCapturedTool* t = nullptr;
    for (unsigned j = 0; j < r.tool_count; ++j)
      if (r.tools[j].handle == p.Bindings[i].Handle)
        t = &r.tools[j];
    tools.push(tool(p, i, t));
  }
  return J::obj()
    .add("frame_id", J(p.AcquisitionId + ":" + std::to_string(p.Sequence)))
    .add("reply_seq", J::d(p.Sequence))
    .add("connection_epoch", J::d(p.Epoch))
    .add("recovery_state", J(p.Quarantined ? "QUARANTINED" : "SYNCHRONIZED"))
    .add("timing", timing)
    .add("reply", reply)
    .add("tools", tools)
    .dump();
}
std::string
Frame(const PlusNDISnapshot& p, const std::string& source, const std::string& session, uint64_t seq)
{
  J data;
  data.s = Data(p);
  return prefix("frame", source, p.AcquisitionId, session, seq).add("data", data).dump();
}
std::string
Session(const PlusNDIRawConfig& c,
        const std::string& acquisition,
        const std::string& clock,
        const std::vector<PlusNDIMarkerBinding>& bs,
        const std::string& session,
        const std::string& previous,
        uint64_t first,
        const std::string& resume = "")
{
  J bindings = J::arr();
  for (const auto& b : bs)
    bindings.push(binding(b, b.RawBindingEpoch));
  char hostname[129] = { 0 };
  if (gethostname(hostname, 128))
    std::strcpy(hostname, "UNKNOWN_HOST");
  const char* delay = getenv("NDICAPI_SERIAL_WRITE_DELAY_US");
  J prov = J::obj()
             .add("pluslib_base_commit", J("c796218916c2854c3fd8d2286facac8a5821ceb5"))
             .add("ndicapi_base_commit", J("d75730b7a9d9c243ed0bcaf49ef833a00e374024"))
             .add("source_package_sha256", J())
             .add("binary_sha256", J())
             .add("configuration_sha256", J())
             .add("serial_pacing_environment_us", delay ? J(delay) : J())
             .add("serial_endpoint", optional(c.SerialEndpoint));
  J data =
    J::obj()
      .add("first_reply_seq", J::d(first))
      .add("previous_session_id", optional(previous))
      .add("resume_reason", J(resume.empty() ? (previous.empty() ? "START" : "TCP_RECONNECT") : resume))
      .add("history_replayed", J::b(false))
      .add("source_host_id", J(hostname))
      .add("clock_domain_id", J(clock))
      .add("monotonic_clock", J("CLOCK_MONOTONIC"))
      .add("wall_clock", J("CLOCK_REALTIME"))
      .add("cross_host_clock_relation", J("UNSYNCHRONIZED"))
      .add("units", J("mm"))
      .add("reference_frame", J(c.ReferenceFrame))
      .add("coordinate_convention", J("NDI_POSITION_SENSOR_RIGHT_HANDED_XYZ"))
      .add("pose_convention", J("T_reference_from_original_ROM_tool;quaternion_wxyz"))
      .add("frame_association", J("SAME_BX_REPLY_SAME_HANDLE_BLOCK_NOT_PROOF_OF_SAME_EXPOSURE"))
      .add("requested_rate_hz", J::n(c.RequestedRate))
      .add("bx_flags", J::i(NDI_BX_CAPTURE_FLAGS))
      .add("max_utf8_bytes", J::i(65535))
      .add("raw_wire_policy", J("FULL_REQUIRED"))
      .add("loss_policy", J("DROP_NEW_WITH_ACCOUNTING"))
      .add("counter_policy", J("MODULAR_HALF_RANGE_BACKWARD_QUARANTINE"))
      .add("initial_bindings", bindings)
      .add("provenance", prov);
  return prefix("session", c.SourceId, acquisition, session, 0).add("data", data).dump();
}
std::string
Session(const PlusNDIRawConfig& c,
        const PlusNDIRawModel& m,
        const std::vector<PlusNDIMarkerBinding>& bs,
        const std::string& session,
        const std::string& previous,
        uint64_t first)
{
  return Session(c, m.Acquisition(), m.ClockDomain(), bs, session, previous, first);
}
std::string
Pack(const std::string& payload)
{
  if (!validUtf8(payload))
    throw std::invalid_argument("non-UTF8 IGTL STRING");
  if (payload.empty() || payload.size() > 65535)
    throw std::length_error("IGTL STRING UTF8 payload size");
  std::string body;
  be(body, 106, 2);
  be(body, payload.size(), 2);
  body += payload;
  // OpenIGTLink CRC-64/ECMA, initial 0, non-reflected, xorout 0.
  uint64_t crc = 0;
  for (unsigned char c : body) {
    crc ^= uint64_t(c) << 56;
    for (unsigned i = 0; i < 8; ++i)
      crc = (crc << 1) ^ ((crc >> 63) ? 0x42f0e1eba9ea3693ULL : 0);
  }
  std::string wire;
  be(wire, 1, 2);
  wire += "STRING";
  wire.append(6, '\0');
  wire += "NDI_RAW_V1";
  wire.append(10, '\0');
  be(wire, 0, 8);
  be(wire, body.size(), 8);
  be(wire, crc, 8);
  return wire + body;
}
}
PlusNDIRawModel::PlusNDIRawModel()
  : ClockDomainId(PlusNDIRawWire::UUID())
{
  NewAcquisition();
}
void
PlusNDIRawModel::NewAcquisition(uint64_t initialized)
{
  if (initialized > LastInitializedConnection) {
    LastInitializedConnection = initialized;
    RecoveryRequired = false;
  }
  AcquisitionId = PlusNDIRawWire::UUID();
  PreparedRecoveryId = PlusNDIRawWire::UUID();
  InitialPending = false;
  States.clear();
  QuarantinedTools.clear();
  Quarantined = RecoveryRequired;
  NextBinding = 0;
  PreviousMonotonic = PreviousWall = 0;
}
bool
PlusNDIRawModel::CompleteInitialRecovery(const PlusNDIRawRecoveryEvidence& evidence)
{
  if (!InitialPending || PreparedRecoveryId.empty() || !evidence.Success || !evidence.Reset.verified ||
      !evidence.InitOK || !evidence.CommOK || !evidence.ConfigurationOK || !evidence.PortsOK ||
      !evidence.StartOK || evidence.NewConnection <= LastInitializedConnection ||
      evidence.OldConnection != LastInitializedConnection ||
      evidence.NewConnection != evidence.OldConnection + 1 || !evidence.StartNs ||
      evidence.StartNs < evidence.EndSentNs || evidence.DoneNs < evidence.StartNs ||
      evidence.Reset.start_monotonic_ns < evidence.StartNs ||
      evidence.Reset.done_monotonic_ns < evidence.Reset.start_monotonic_ns ||
      evidence.DoneNs < evidence.Reset.done_monotonic_ns)
    return false;
  LastInitializedConnection = evidence.NewConnection;
  AcquisitionId = std::move(PreparedRecoveryId);
  States.clear();
  QuarantinedTools.clear();
  NextBinding = 0;
  Quarantined = RecoveryRequired = false;
  InitialPending = false;
  return true;
}
void
PlusNDIRawModel::PrepareBindings(std::vector<PlusNDIMarkerBinding>& bindings, uint64_t connection)
{
  for (auto& b : bindings) {
    auto& state = States[b.Handle];
    const std::string key = std::to_string(connection) + ":" + b.TransformName + ":" + b.BodyFrame + ":" +
                            b.RomMD5 + ":" + b.RomSHA256;
    if (state.Binding != key) {
      state = State();
      state.Binding = key;
      state.Value.BindingEpoch = ++NextBinding;
    }
    b.RawBindingEpoch = state.Value.BindingEpoch;
  }
}
std::shared_ptr<const PlusNDISnapshot>
PlusNDIRawModel::Seal(const ndiBXCapture& r,
                      const std::vector<PlusNDIMarkerBinding>& bindings,
                      uint64_t connection,
                      uint64_t reply,
                      uint64_t legacy)
{
  auto p = std::make_shared<PlusNDISnapshot>();
  p->Reply = r;
  p->Bindings = bindings;
  p->Epoch = connection;
  p->Sequence = reply;
  p->LegacySequence = legacy ? legacy : reply;
  p->AcquisitionId = AcquisitionId;
  p->ClockDomainId = ClockDomainId;
  if (!r.command_start_monotonic_ns || !r.receive_monotonic_ns || !r.decode_done_monotonic_ns ||
      !r.receive_wall_ns)
    p->Diagnostics |= 128;
  if (r.receive_monotonic_ns && r.receive_wall_ns) {
    if (PreviousMonotonic && r.receive_monotonic_ns >= PreviousMonotonic) {
      uint64_t elapsed = r.receive_monotonic_ns - PreviousMonotonic;
      if (r.receive_wall_ns < PreviousWall)
        p->Diagnostics |= 8;
      else {
        uint64_t wallElapsed = r.receive_wall_ns - PreviousWall;
        if (wallElapsed > elapsed ? wallElapsed - elapsed > 100000000ULL
                                  : elapsed - wallElapsed > 100000000ULL)
          p->Diagnostics |= 8;
      }
    }
    PreviousMonotonic = r.receive_monotonic_ns;
    PreviousWall = r.receive_wall_ns;
  }
  if (r.command_start_monotonic_ns && r.receive_monotonic_ns < r.command_start_monotonic_ns)
    p->Diagnostics |= 16;
  if (r.transport_state == NDI_BX_RECONNECT_REQUIRED) {
    Quarantined = true;
    RecoveryRequired = true;
  }
  // The initial quarantined observation latches this acquisition until verified
  // serial recovery completes and a new acquisition/session boundary is committed.
  if (r.transport_state == NDI_BX_FIRST_REPLY_QUARANTINED && connection == LastInitializedConnection)
    InitialPending = true;
  p->Quarantined = Quarantined || InitialPending || r.transport_state == NDI_BX_FIRST_REPLY_QUARANTINED;
  for (unsigned j = 0; j < r.tool_count; ++j) {
    bool found = false;
    for (const auto& b : p->Bindings)
      found = found || b.Handle == r.tools[j].handle;
    if (!found) {
      PlusNDIMarkerBinding b;
      b.Handle = r.tools[j].handle;
      p->Bindings.push_back(b);
    }
  }
  bool used[256] = { false };
  std::map<std::string, bool> ids;
  for (auto& b : p->Bindings) {
    if (!b.Handle || b.Handle > 255 || used[b.Handle] || (!b.TransformName.empty() && ids[b.TransformName]))
      p->Quarantined = true;
    if (b.Handle <= 255)
      used[b.Handle] = true;
    if (!b.TransformName.empty())
      ids[b.TransformName] = true;
    const ndiBXCapturedTool* t = nullptr;
    for (unsigned j = 0; j < r.tool_count; ++j)
      if (r.tools[j].handle == b.Handle)
        t = &r.tools[j];
    auto& state = States[b.Handle];
    const std::string key = std::to_string(connection) + ":" + b.TransformName + ":" + b.BodyFrame + ":" +
                            b.RomMD5 + ":" + b.RomSHA256;
    if (state.Binding != key) {
      state = State();
      state.Binding = key;
      state.Value.BindingEpoch = ++NextBinding;
    }
    const std::string quarantineKey = b.TransformName.empty() ? "unbound-handle:" + std::to_string(b.Handle)
                                                              : "configured-tool:" + b.TransformName;
    auto value = state.Value;
    value.Quarantined = value.Quarantined || QuarantinedTools.count(quarantineKey);

    value.Classification = "UNAVAILABLE";
    value.HaveCounter = value.HaveDelta = value.HaveNew = value.Changed = false;
    value.Status = b.TransformName.empty()
                     ? "UNKNOWN"
                     : (!r.valid ? currentStatus(r, t) : (b.PlusStatus.empty() ? "UNKNOWN" : b.PlusStatus));
    if (t && t->frame_available) {
      value.HaveCounter = true;
      value.Counter = t->frame_number;
      std::string block(reinterpret_cast<const char*>(r.raw + t->wire_offset), t->wire_length);
      if (!state.Have)
        value.Classification = "FIRST";
      else {
        value.HaveDelta = true;
        value.Delta = uint32_t(t->frame_number - state.Last);
        if (!value.Delta) {
          value.Classification = "REPEAT";
          value.HaveNew = true;
          value.IsNew = false;
          value.Changed = state.Block != block;
          if (value.Changed) {
            value.Quarantined = true;
            RecoveryRequired = true;
            p->Diagnostics |= 4;
          }
        } else if (value.Delta < 0x80000000u && t->frame_number >= state.Last) {
          value.Classification = "NEW";
          value.HaveNew = true;
          value.IsNew = true;
        } else {
          value.Classification = t->frame_number < state.Last && value.Delta < 0x80000000u
                                   ? "WRAP_CANDIDATE"
                                   : "RESET_OR_OUT_OF_ORDER";
          ++value.CounterEpoch;
          value.Quarantined = true;
          RecoveryRequired = true;
          p->Diagnostics |= value.Classification == "WRAP_CANDIDATE" ? 32 : 64;
        }
      }
      state.Have = true;
      state.Last = t->frame_number;
      state.Block = block;
    }
    if (value.Quarantined)
      QuarantinedTools.insert(quarantineKey);
    state.Value = value;
    b.RawBindingEpoch = value.BindingEpoch;
    p->ToolStates.push_back(value);
  }
  return p;
}
namespace {
struct RawTransport
{
  int FD = -1;
  std::atomic<bool> Dead{ false };
  ~RawTransport()
  {
    if (FD >= 0)
      close(FD);
  }
};
struct RawSession
{
  std::string Id, Acquisition, Clock, Previous, EndReason = "EXPORT_STOP";
  uint64_t First = 0, Last = 0, Next = 1;
  std::vector<PlusNDIMarkerBinding> Bindings;
  std::atomic<uint64_t> Considered{ 0 }, Enqueued{ 0 }, Dropped{ 0 }, Sent{ 0 }, Discarded{ 0 };
  std::atomic<bool> Bad{ false }, Dead{ false }, Frozen{ false };
  std::atomic<unsigned> PendingErrors{ 0 };
  SHA256_CTX Digest;
  std::shared_ptr<RawTransport> Transport{ std::make_shared<RawTransport>() };
  bool KeepOpen = false;
  std::string ResumeReason;
  std::atomic<bool> EndQueued{ false };
  std::atomic<uint64_t> StopDeadline{ 0 };
  bool SessionSent = false;
  std::atomic<bool> EndSent{ false }, EndProcessed{ false };
  std::atomic<uint64_t> EndSentNs{ 0 };
  RawSession() { SHA256_Init(&Digest); }
};
struct RawItem
{
  enum Kind
  {
    Start,
    Frame,
    Error,
    End,
    RecoveryProof
  } Type;
  std::shared_ptr<RawSession> Session;
  std::shared_ptr<const PlusNDISnapshot> Snapshot;
  std::shared_ptr<const PlusNDIRawRecoveryEvidence> Proof;
  uint64_t Seq = 0;
  unsigned ErrorBits = 0;
  size_t Bytes = 0;
};
J
counts(const RawSession& s)
{
  return J::obj()
    .add("frames_considered", J::d(s.Considered))
    .add("frames_enqueued", J::d(s.Enqueued))
    .add("frames_dropped", J::d(s.Dropped))
    .add("frames_sent", J::d(s.Sent))
    .add("frames_discarded", J::d(s.Discarded));
}
J
reasons(const RawSession& s)
{
  J a = J::arr();
  if (s.Dropped)
    a.push(J("FRAME_DROP"));
  if (s.Discarded)
    a.push(J("FRAME_DISCARD"));
  if (s.Bad)
    a.push(J("EXPORT_OR_TRANSPORT_OR_CONTROL_FAILURE"));
  return a;
}
bool
conserved(const RawSession& s)
{
  return s.Considered == s.Enqueued + s.Dropped && s.Enqueued == s.Sent + s.Discarded &&
         (!s.Considered || s.Last - s.First + 1 == s.Considered);
}
std::string
digest(const RawSession& s)
{
  SHA256_CTX copy = s.Digest;
  unsigned char h[32];
  SHA256_Final(h, &copy);
  return hex(h, 32);
}
std::string
endPayload(const RawSession& s, const PlusNDIRawConfig& c, uint64_t seq, bool complete)
{
  return prefix("end", c.SourceId, s.Acquisition, s.Id, seq)
    .add("data",
         J::obj()
           .add("reason", J(s.EndReason))
           .add("first_reply_seq", J::d(s.First))
           .add("last_reply_seq", s.Considered ? J::d(s.Last) : J())
           .add("counters", counts(s))
           .add("sender_complete", J::b(complete))
           .add("incomplete_reasons", reasons(s))
           .add("sent_frame_payload_sha256", J(digest(s))))
    .dump();
}
bool
socketWrite(int fd, const std::string& wire, uint64_t deadline, const std::atomic<uint64_t>* stopDeadline)
{
  size_t have = 0;
  while (have < wire.size()) {
    const uint64_t stopping = stopDeadline->load();
    if (stopping)
      deadline = std::min(deadline, stopping);
    uint64_t now = ns();
    if (now >= deadline)
      return false;
    pollfd p = { fd, POLLOUT, 0 };
    int n = poll(&p, 1, int(std::min<uint64_t>((deadline - now + 999999) / 1000000, 10)));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      return false;
    if (!n)
      continue;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
      return false;
    iovec part = { const_cast<char*>(wire.data() + have), wire.size() - have };
    msghdr message = {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    ssize_t sent = sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      continue;
    if (sent <= 0)
      return false;
    have += size_t(sent);
  }
  return true;
}
bool
fileWrite(int fd, const std::string& bytes)
{
  size_t have = 0;
  while (have < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + have, bytes.size() - have);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    have += size_t(n);
  }
  return true;
}
}
struct PlusNDIRawSender::Impl
{
  PlusNDIRawConfig Config;
  mutable std::mutex Mutex, ErrorMutex, Lifecycle;
  std::deque<RawItem> Queue;
  std::thread Worker;
  std::shared_ptr<RawSession> Active, Pending, RecoveryOld;
  std::shared_ptr<RawTransport> RecoveryTransport;
  std::atomic<bool> Recovering{ false };
  std::atomic<bool> Running{ false }, Stopping{ false }, Failed{ false };
  std::atomic<uint64_t> MaxQueueBytes{ 0 }, MaxQueueFrames{ 0 };
  std::atomic<bool> Final{ false }, WasStarted{ false }, AnySession{ false }, StartupFailed{ false };
  int Listener = -1;
  size_t Bytes = 0;
  unsigned Frames = 0, Controls = 0;
  std::string LastError, Previous, PreparedSessionId;
  void fail(const std::string& why)
  {
    Failed = true;
    std::lock_guard<std::mutex> lock(ErrorMutex);
    if (LastError.empty())
      LastError = why;
  }
  std::string error() const
  {
    std::lock_guard<std::mutex> lock(ErrorMutex);
    return LastError;
  }
  bool put(RawItem item)
  { // caller holds only the short bookkeeping mutex
    if (item.Type == RawItem::Frame) {
      if (Frames >= Config.QueueCapacity ||
          item.Bytes > Config.QueueBytes - std::min<size_t>(Bytes, Config.QueueBytes))
        return false;
      ++Frames;
      Bytes += item.Bytes;
      MaxQueueFrames = std::max<uint64_t>(MaxQueueFrames, Frames);
      MaxQueueBytes = std::max<uint64_t>(MaxQueueBytes, Bytes);
    } else {
      if (Controls >= (item.Type == RawItem::End ? 9u : 8u))
        return false;
      ++Controls;
    }
    try {
      Queue.push_back(std::move(item));
      return true;
    } catch (...) {
      if (item.Type == RawItem::Frame) {
        --Frames;
        Bytes -= item.Bytes;
      } else
        --Controls;
      return false;
    }
  }
  void startupFailure(const std::string& why)
  {
    try {
      const auto path = Config.StatusDirectory + "/raw-startup-" + PlusNDIRawWire::UUID() + ".json";
      int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd >= 0) {
        auto text = J::obj()
                      .add("schema", J("ndi.raw.sender.startup.v1"))
                      .add("state", J("startup_failed"))
                      .add("error", J(why))
                      .add("sender_complete", J::b(false))
                      .dump() +
                    "\n";
        bool ok = fileWrite(fd, text);
        if (fsync(fd))
          ok = false;
        if (close(fd))
          ok = false;
        if (!ok)
          fail("STARTUP_EVIDENCE_WRITE_FAILURE");
      }
    } catch (...) {
    }
  }
  void pendingErrors(const std::shared_ptr<RawSession>& s)
  {
    unsigned bits = s->PendingErrors.exchange(0);
    if (!bits)
      return;
    RawItem item;
    item.Type = RawItem::Error;
    item.Session = s;
    item.Seq = s->Next++;
    item.ErrorBits = bits;
    if (!put(std::move(item))) {
      s->Bad = true;
      fail("CONTROL_QUEUE_OVERFLOW");
    }
  }
  void drop(const std::shared_ptr<RawSession>& s)
  {
    ++s->Dropped;
    s->Bad = true;
    s->PendingErrors.fetch_or(1);
    Failed = true;
  }
  bool sendPayload(const std::shared_ptr<RawSession>& s, const std::string& payload)
  {
    if (s->Dead || s->Transport->Dead) {
      s->Dead = true;
      s->Bad = true;
      return false;
    }
    const uint64_t deadline =
      std::min<uint64_t>(ns() + uint64_t(Config.WriteTimeoutMs) * 1000000,
                         s->StopDeadline.load() ? s->StopDeadline.load() : UINT64_MAX);
    if (!socketWrite(s->Transport->FD, PlusNDIRawWire::Pack(payload), deadline, &s->StopDeadline)) {
      s->Dead = true;
      s->Transport->Dead = true;
      s->Bad = true;
      shutdown(s->Transport->FD, SHUT_RDWR);
      fail("TRANSPORT_WRITE_OR_DEADLINE_FAILURE");
      return false;
    }
    return true;
  }
  std::string sessionPayload(const RawSession& s)
  {
    return PlusNDIRawWire::Session(
      Config, s.Acquisition, s.Clock, s.Bindings, s.Id, s.Previous, s.First, s.ResumeReason);
  }
  void save(const RawSession& s)
  {
    const bool complete = !s.Bad && !s.Dropped && !s.Discarded && s.EndSent && conserved(s);
    J summary = J::obj()
                  .add("schema", J("ndi.raw.sender.summary.v1"))
                  .add("source_id", J(Config.SourceId))
                  .add("acquisition_id", J(s.Acquisition))
                  .add("session_id", J(s.Id))
                  .add("counters", counts(s))
                  .add("first_reply_seq", J::d(s.First))
                  .add("last_reply_seq", s.Considered ? J::d(s.Last) : J())
                  .add("counter_conservation", J::b(conserved(s)))
                  .add("sender_complete", J::b(complete))
                  .add("end_sent", J::b(s.EndSent))
                  .add("sent_frame_payload_sha256", J(digest(s)))
                  .add("max_queued_bytes", J::d(MaxQueueBytes))
                  .add("max_queued_frames", J::d(MaxQueueFrames))
                  .add("incomplete_reasons", reasons(s))
                  .add("receipt_semantics", J("SEND_COMPLETION_IS_NOT_RECEIVER_DURABILITY"));
    const auto path = Config.StatusDirectory + "/raw-" + s.Id + ".json";
    int fd = open((path + ".pending").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = fd >= 0;
    if (ok) {
      ok = fileWrite(fd, summary.dump() + "\n");
      if (fsync(fd))
        ok = false;
      if (close(fd))
        ok = false;
    }
    if (ok && link((path + ".pending").c_str(), path.c_str()))
      ok = false;
    if (ok)
      unlink((path + ".pending").c_str());
    else
      fail("LOCAL_SUMMARY_WRITE_FSYNC_CLOSE_PUBLISH_FAILURE");
  }
  void process(RawItem& item)
  {
    if (item.Type == RawItem::RecoveryProof) {
      try {
        const auto& p = *item.Proof;
        J proof =
          J::obj()
            .add("schema", J("ndi.raw.local-recovery.v1"))
            .add("success", J::b(p.Success))
            .add("old_end_send_completed_monotonic_ns", J::t(p.EndSentNs))
            .add("recovery_start_monotonic_ns", J::t(p.StartNs))
            .add("recovery_done_monotonic_ns", J::t(p.DoneNs))
            .add("old_connection_epoch", J::d(p.OldConnection))
            .add("new_connection_epoch", J::d(p.NewConnection))
            .add("method", J("SERIAL_BREAK_FULL_RESET_TOKEN_INIT_COMM_CONFIGURATION_ROM_ENABLE_TSTART_ACK"))
            .add("reset_verified", J::b(p.Reset.verified))
            .add("reset_error_code", J::i(p.Reset.error_code))
            .add("reset_wire_hex", J(hex(p.Reset.raw, p.Reset.raw_length)))
            .add("init_ok", J::b(p.InitOK))
            .add("init_reply_wire_hex",
                 J(hex(reinterpret_cast<const unsigned char*>(p.InitReply), std::strlen(p.InitReply))))
            .add("comm_ok", J::b(p.CommOK))
            .add("comm_reply_wire_hex",
                 J(hex(reinterpret_cast<const unsigned char*>(p.CommReply), std::strlen(p.CommReply))))
            .add("configuration_ok", J::b(p.ConfigurationOK))
            .add("ports_ok", J::b(p.PortsOK))
            .add("tstart_ok", J::b(p.StartOK))
            .add("tstart_reply_wire_hex",
                 J(hex(reinterpret_cast<const unsigned char*>(p.StartReply), std::strlen(p.StartReply))))
            .add("error", optional(p.Error))
            .add("transaction_association_verified", J::b(false));
        const auto path = Config.StatusDirectory + "/raw-recovery-" + std::to_string(p.StartNs) + ".json";
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        bool ok = fd >= 0;
        if (ok) {
          ok = fileWrite(fd, proof.dump() + "\n");
          if (fsync(fd))
            ok = false;
          if (close(fd))
            ok = false;
        }
        if (!ok)
          fail("RECOVERY_EVIDENCE_STORAGE_FAILED");
      } catch (...) {
        fail("RECOVERY_EVIDENCE_ENCODING_FAILED");
      }
      return;
    }
    auto s = item.Session;
    try {
      if (item.Type == RawItem::Start) {
        s->SessionSent = sendPayload(s, sessionPayload(*s));
      } else if (item.Type == RawItem::Frame) {
        if (s->Dead || s->Transport->Dead) {
          s->Bad = true;
          ++s->Discarded;
          return;
        }
        const auto payload = PlusNDIRawWire::Frame(*item.Snapshot, Config.SourceId, s->Id, item.Seq);
        if (payload.size() > 65535) {
          ++s->Discarded;
          s->Bad = true;
          s->PendingErrors.fetch_or(2);
          fail("OVERSIZE_FRAME");
          return;
        }
        if (sendPayload(s, payload)) {
          ++s->Sent;
          std::string length;
          be(length, payload.size(), 4);
          SHA256_Update(&s->Digest, length.data(), length.size());
          SHA256_Update(&s->Digest, payload.data(), payload.size());
        } else
          ++s->Discarded;
      } else if (item.Type == RawItem::Error) {
        const bool affects = (item.ErrorBits & 3) != 0;
        const char* code = item.ErrorBits & 2     ? "OVERSIZE_FRAME"
                           : item.ErrorBits & 1   ? "QUEUE_DROP_NEW"
                           : item.ErrorBits & 4   ? "REPEATED_COUNTER_PAYLOAD_CHANGED"
                           : item.ErrorBits & 8   ? "WALL_CLOCK_DISCONTINUITY"
                           : item.ErrorBits & 16  ? "MONOTONIC_CLOCK_REVERSED"
                           : item.ErrorBits & 128 ? "HOST_CLOCK_UNAVAILABLE"
                           : item.ErrorBits & 32  ? "COUNTER_WRAP_CANDIDATE"
                                                  : "COUNTER_RESET_OR_OUT_OF_ORDER";
        J d =
          J::obj()
            .add("code", J(code))
            .add("scope", J(affects ? "EXPORT" : "DEVICE"))
            .add("message",
                 J(affects ? "See source sender summary; session incomplete"
                           : "Observation diagnostic; consult the same immutable frame and recovery state"))
            .add("affects_completeness", J::b(affects))
            .add("related_reply_seq", J())
            .add("dropped_seq_first", J())
            .add("dropped_seq_last", J())
            .add("counters", counts(*s));
        sendPayload(s,
                    prefix("error", Config.SourceId, s->Acquisition, s->Id, item.Seq).add("data", d).dump());
      } else {
        // All producer-assigned frame/control records precede this item. Oversize
        // discovered during final drain consumes a final error seq before end.
        const unsigned bits = s->PendingErrors.exchange(0);
        if (bits) {
          RawItem e;
          e.Type = RawItem::Error;
          e.Session = s;
          e.Seq = item.Seq++;
          e.ErrorBits = bits;
          process(e);
        }
        if (!conserved(*s)) {
          s->Bad = true;
          fail("COUNTER_CONSERVATION_FAILURE");
        }
        const bool good = !s->Bad && !s->Dropped && !s->Discarded && conserved(*s) && s->SessionSent;
        s->EndSent = sendPayload(s, endPayload(*s, Config, item.Seq, good));
        if (s->EndSent)
          s->EndSentNs = ns();
        if (!s->EndSent)
          s->Bad = true;
        save(*s);
        s->EndProcessed = true;
        if (!s->KeepOpen && s->Transport->FD >= 0) {
          s->Transport->Dead = true;
          close(s->Transport->FD);
          s->Transport->FD = -1;
        }
      }
    } catch (const std::exception&) {
      s->Bad = true;
      fail("RAW_ENCODING_OR_WORKER_FAILURE");
      if (item.Type == RawItem::Frame)
        ++s->Discarded;
      s->Dead = true;
      s->Transport->Dead = true;
      if (s->Transport->FD >= 0)
        shutdown(s->Transport->FD, SHUT_RDWR);
      if (item.Type == RawItem::End) {
        save(*s);
        s->EndProcessed = true;
      }
    }
  }
  void worker()
  {
    while (true) {
      RawItem item;
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(Mutex);
        if (!Queue.empty()) {
          item = std::move(Queue.front());
          Queue.pop_front();
          if (item.Type == RawItem::Frame) {
            --Frames;
            Bytes -= item.Bytes;
          } else
            --Controls;
          have = true;
        } else if (Stopping)
          break;
      }
      if (have) {
        process(item);
        continue;
      }
      if (Listener >= 0) {
        pollfd p = { Listener, POLLIN, 0 };
        if (poll(&p, 1, 10) > 0) {
          int fd = accept4(Listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd >= 0) {
            try {
              auto s = std::make_shared<RawSession>();
              s->Id = PlusNDIRawWire::UUID();
              s->Transport->FD = fd;
              std::lock_guard<std::mutex> lock(Mutex);
              if (Pending || Active || RecoveryTransport) {
                close(fd);
                s->Transport->FD = -1;
              } else
                Pending = s;
            } catch (...) {
              close(fd);
              fail("ACCEPT_SESSION_ALLOCATION_FAILURE");
            }
          }
        }
      } else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  bool start(const PlusNDIRawConfig& c, int testfd, std::string& error)
  {
    std::lock_guard<std::mutex> life(Lifecycle);
    if (Running) {
      error = "already_started";
      return false;
    }
    struct stat st;
    if (!uuidValid(c.SourceId) || c.ReferenceFrame.empty() || c.ReferenceFrame.size() > 128 ||
        c.QueueCapacity < 1 || c.QueueCapacity > 1024 || c.QueueBytes < sizeof(PlusNDISnapshot) ||
        c.QueueBytes > 256u * 1024 * 1024 || c.WriteTimeoutMs < 1 || c.WriteTimeoutMs > 10000 ||
        c.DrainTimeoutMs < 1 || c.DrainTimeoutMs > 30000 || c.Port < 1 || c.Port > 65535 ||
        !std::isfinite(c.RequestedRate) || c.RequestedRate <= 0 || stat(c.StatusDirectory.c_str(), &st) ||
        !S_ISDIR(st.st_mode)) {
      error = "invalid raw source UUID, profile, bounds or status directory";
      if (testfd >= 0)
        close(testfd);
      WasStarted = true;
      StartupFailed = true;
      Final = false;
      fail(error);
      return false;
    }
    StartupFailed = false;
    AnySession = false;
    Config = c;
    Stopping = false;
    Failed = false;
    Final = false;
    {
      std::lock_guard<std::mutex> errorLock(ErrorMutex);
      LastError.clear();
    }
    Previous.clear();
    Queue.clear();
    Active.reset();
    Pending.reset();
    RecoveryOld.reset();
    RecoveryTransport.reset();
    Recovering = false;
    Bytes = Frames = Controls = 0;
    MaxQueueBytes = MaxQueueFrames = 0;
    try {
      PreparedSessionId = PlusNDIRawWire::UUID();
      if (testfd >= 0) {
        Pending = std::make_shared<RawSession>();
        Pending->Id = PlusNDIRawWire::UUID();
        Pending->Transport->FD = testfd;
        testfd = -1;
      } else {
        Listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (Listener < 0)
          throw std::runtime_error("raw socket creation failed");
        int reuseAddress = 1;
        if (setsockopt(Listener, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress)))
          throw std::runtime_error("raw SO_REUSEADDR failed");
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(c.Port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(Listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || listen(Listener, 1))
          throw std::runtime_error("raw bind/listen failed; no port fallback");
      }
      Worker = std::thread([this] { worker(); });
      Running = true;
      WasStarted = true;
      return true;
    } catch (const std::exception& e) {
      if (testfd >= 0)
        close(testfd);
      Pending.reset();
      if (Listener >= 0) {
        close(Listener);
        Listener = -1;
      }
      error = e.what();
      fail(error);
      startupFailure(error);
      WasStarted = true;
      StartupFailed = true;
      Final = false;
      return false;
    }
  }
};
PlusNDIRawSender::PlusNDIRawSender()
  : P(new Impl)
{
}
PlusNDIRawSender::~PlusNDIRawSender()
{
  Stop();
}
bool
PlusNDIRawSender::Start(const PlusNDIRawConfig& c, std::string& error)
{
  return P->start(c, -1, error);
}
bool
PlusNDIRawSender::StartConnectedForTest(const PlusNDIRawConfig& c, int socket, std::string& error)
{
  return P->start(c, socket, error);
}
void
PlusNDIRawSender::BeginAttempt(uint64_t reply,
                               const std::vector<PlusNDIMarkerBinding>& bindings,
                               const PlusNDIRawModel& model)
{
  if (!P->Running || P->Recovering)
    return;
  std::unique_lock<std::mutex> lock(P->Mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  if (P->Active && (P->Active->Dead || P->Active->Transport->Dead)) {
    auto s = P->Active;
    s->Frozen = true;
    s->EndReason = "ERROR";
    s->KeepOpen = false;
    RawItem e;
    e.Type = RawItem::End;
    e.Session = s;
    e.Seq = s->Next++;
    if (!P->put(std::move(e)))
      P->fail("END_CONTROL_QUEUE_FAILURE");
    P->Previous = s->Id;
    P->Active.reset();
  }
  if (!P->Active && P->Pending) {
    auto s = P->Pending;
    P->Pending.reset();
    s->First = reply;
    s->Acquisition = model.Acquisition();
    s->Clock = model.ClockDomain();
    s->Previous = P->Previous;
    s->ResumeReason = s->Previous.empty() ? "START" : "TCP_RECONNECT";
    s->Bindings = bindings;
    P->Active = s;
    P->AnySession = true;
    RawItem start;
    start.Type = RawItem::Start;
    start.Session = s;
    if (!P->put(std::move(start))) {
      s->Bad = true;
      s->Dead = true;
      P->fail("SESSION_CONTROL_QUEUE_FAILURE");
    }
  }
  if (P->Active)
    P->pendingErrors(P->Active);
}
int
PlusNDIRawSender::PrepareInitialRecovery()
{
  if (!P->Running)
    return -1;
  std::unique_lock<std::mutex> lock(P->Mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return 0;
  if (!P->Recovering) {
    auto old = P->Active;
    P->RecoveryOld = old;
    P->Recovering = true;
    if (old) {
      old->Frozen = true;
      old->EndReason = "RECOVERY";
      old->KeepOpen = true;
      P->RecoveryTransport = old->Transport;
      P->Previous = old->Id;
      P->pendingErrors(old);
      RawItem end;
      end.Type = RawItem::End;
      end.Session = old;
      end.Seq = old->Next++;
      if (!P->put(std::move(end))) {
        old->Bad = true;
        P->fail("RECOVERY_END_QUEUE_FAILED");
        return -1;
      }
      old->EndQueued = true;
      P->Active.reset();
    }
  }
  auto old = P->RecoveryOld;
  if (!old)
    return 1;
  if (!old->EndQueued)
    return -1;
  if (!old->EndProcessed)
    return 0;
  return old->EndSent ? 1 : -1;
}
uint64_t
PlusNDIRawSender::RecoveryEndSentNs() const
{
  return P->RecoveryOld ? P->RecoveryOld->EndSentNs.load() : 0;
}
bool
PlusNDIRawSender::RecordRecoveryEvidence(const PlusNDIRawRecoveryEvidence& evidence)
{
  try {
    RawItem item;
    item.Type = RawItem::RecoveryProof;
    item.Proof = std::make_shared<PlusNDIRawRecoveryEvidence>(evidence);
    std::unique_lock<std::mutex> lock(P->Mutex, std::try_to_lock);
    if (!lock.owns_lock() || !P->put(std::move(item))) {
      P->fail("RECOVERY_EVIDENCE_ADMISSION_FAILED");
      return false;
    }
    return true;
  } catch (...) {
    P->fail("RECOVERY_EVIDENCE_ALLOCATION_FAILED");
    return false;
  }
}
bool
PlusNDIRawSender::CommitInitialRecovery(PlusNDIRawModel& model,
                                        std::vector<PlusNDIMarkerBinding>& bindings,
                                        const PlusNDIRawRecoveryEvidence& evidence)
{
  std::lock_guard<std::mutex> lock(P->Mutex); // worker's short queue bookkeeping only, never network/disk I/O
  try {
    if (!model.CompleteInitialRecovery(evidence))
      return false;
    model.PrepareBindings(bindings, evidence.NewConnection);
    if (P->RecoveryTransport && !P->RecoveryTransport->Dead) {
      if (P->PreparedSessionId.empty())
        throw std::runtime_error("recovery session identity unavailable");
      auto next = std::make_shared<RawSession>();
      next->Transport = P->RecoveryTransport;
      next->Id = std::move(P->PreparedSessionId);
      next->Previous = P->Previous;
      next->ResumeReason = "RECOVERY";
      next->Acquisition = model.Acquisition();
      next->Clock = model.ClockDomain();
      next->First = 1;
      next->Bindings = bindings;
      RawItem start;
      start.Type = RawItem::Start;
      start.Session = next;
      if (!P->put(std::move(start)))
        throw std::runtime_error("recovery session queue failure");
      P->Active = next;
      P->AnySession = true;
    }
    P->RecoveryTransport.reset();
    P->RecoveryOld.reset();
    P->Recovering = false;
    return true;
  } catch (...) {
    P->fail("VERIFIED_RECOVERY_CONTEXT_COMMIT_FAILED");
    return false;
  }
}
bool
PlusNDIRawSender::Enqueue(const std::shared_ptr<const PlusNDISnapshot>& snapshot)
{
  auto s = P->Active;
  if (!s || s->Frozen)
    return false;
  if (snapshot && snapshot->AcquisitionId != s->Acquisition) {
    s->Bad = true;
    P->fail("ACQUISITION_BOUNDARY_NOT_QUEUED");
    return false;
  }
  // Only this producer owns Next/Last. Network worker owns payload/digest/writes.
  const uint64_t seq = s->Next++;
  ++s->Considered;
  s->Last = snapshot ? snapshot->Sequence : s->First + s->Considered - 1;
  if (!snapshot) {
    P->drop(s);
    return false;
  }
  RawItem item;
  item.Type = RawItem::Frame;
  item.Session = s;
  item.Snapshot = snapshot;
  item.Seq = seq;
  item.Bytes = sizeof(PlusNDISnapshot) + 256 + snapshot->Bindings.capacity() * sizeof(PlusNDIMarkerBinding) +
               snapshot->ToolStates.capacity() * sizeof(PlusNDIRawToolState);
  item.Bytes += snapshot->AcquisitionId.capacity() + snapshot->ClockDomainId.capacity() + 64;
  for (const auto& b : snapshot->Bindings)
    item.Bytes += b.TransformName.capacity() + b.BodyFrame.capacity() + b.RomPath.capacity() +
                  b.RomMD5.capacity() + b.RomSHA256.capacity() + b.PlusStatus.capacity() +
                  b.PlusFrameSource.capacity() + 256;
  for (const auto& t : snapshot->ToolStates)
    item.Bytes += t.Classification.capacity() + t.Status.capacity() + 64;
  std::unique_lock<std::mutex> lock(P->Mutex, std::try_to_lock);
  if (!lock.owns_lock() || !P->put(std::move(item))) {
    P->drop(s);
    return false;
  }
  ++s->Enqueued;
  if (snapshot->Diagnostics) {
    RawItem e;
    e.Type = RawItem::Error;
    e.Session = s;
    e.Seq = s->Next++;
    e.ErrorBits = snapshot->Diagnostics;
    if (!P->put(std::move(e))) {
      s->Bad = true;
      P->fail("DIAGNOSTIC_CONTROL_QUEUE_FAILURE");
    }
  }
  return true;
}
bool
PlusNDIRawSender::Stop(const std::string& reason)
{
  std::lock_guard<std::mutex> life(P->Lifecycle);
  if (!P->Running)
    return P->WasStarted.load() ? P->Final.load() : true;
  {
    std::lock_guard<std::mutex> lock(P->Mutex);
    if (P->Active) {
      auto s = P->Active;
      s->Frozen = true;
      s->EndReason = reason;
      s->StopDeadline = ns() + uint64_t(P->Config.DrainTimeoutMs) * 1000000;
      P->pendingErrors(s);
      RawItem e;
      e.Type = RawItem::End;
      e.Session = s;
      e.Seq = s->Next++;
      if (!P->put(std::move(e))) {
        s->Bad = true;
        P->fail("END_CONTROL_QUEUE_FAILURE");
      }
    }
    P->Pending.reset();
    P->Stopping = true;
  }
  if (P->Worker.joinable())
    P->Worker.join();
  if (P->Listener >= 0) {
    close(P->Listener);
    P->Listener = -1;
  }
  if (P->Active && !P->Active->EndProcessed) {
    P->Active->Bad = true;
    P->save(*P->Active);
  }
  P->Active.reset();
  P->RecoveryOld.reset();
  P->RecoveryTransport.reset();
  P->Recovering = false;
  P->Running = false;
  P->Final = !P->Failed;
  return P->Final;
}
std::string
PlusNDIRawSender::State() const
{
  if (P->Running && P->Recovering)
    return P->Failed ? "recovery_export_failed" : "recovering";
  if (!P->Running && P->StartupFailed)
    return "startup_failed";
  if (!P->Running && P->WasStarted && P->Final && !P->AnySession)
    return "stopped_without_session";
  return !P->Running
           ? (P->WasStarted ? (P->Final ? "finalized_complete" : "finalized_incomplete") : "never_started")
           : (P->Failed ? "streaming_incomplete" : "listening_or_streaming");
}
std::string
PlusNDIRawSender::Error() const
{
  return P->error();
}
