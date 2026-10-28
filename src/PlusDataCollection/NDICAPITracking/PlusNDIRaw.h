/* Local ndi.raw.v1/minor=0 candidate. See Plus License.txt. */
#ifndef PlusNDIRaw_h
#define PlusNDIRaw_h
#include "PlusNDIMarkerRecorder.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>

struct PlusNDIRawToolState
{
  uint64_t BindingEpoch = 1, CounterEpoch = 1;
  unsigned Counter = 0, Delta = 0;
  bool HaveCounter = false, HaveDelta = false, HaveNew = false, IsNew = false, Changed = false,
       Quarantined = false;
  std::string Classification = "UNAVAILABLE", Status = "UNKNOWN";
};
// Built and sealed by the one acquisition producer before either sink sees it.
struct PlusNDISnapshot
{
  ndiBXCapture Reply;
  std::vector<PlusNDIMarkerBinding> Bindings;
  std::vector<PlusNDIRawToolState> ToolStates;
  uint64_t Epoch = 0, Sequence = 0, LegacySequence = 0;
  std::string AcquisitionId, ClockDomainId, PreparedRecoveryId;
  bool InitialPending = false;
  bool Quarantined = false;
  unsigned Diagnostics = 0;
};
struct PlusNDIRawRecoveryEvidence
{
  ndiBXRecoveryReset Reset{};
  uint64_t EndSentNs = 0, StartNs = 0, DoneNs = 0, OldConnection = 0, NewConnection = 0;
  bool InitOK = false, CommOK = false, ConfigurationOK = false, PortsOK = false, StartOK = false,
       Success = false;
  char InitReply[32] = { 0 }, CommReply[32] = { 0 }, StartReply[32] = { 0 }, Error[160] = { 0 };
};
class vtkPlusDataCollectionExport PlusNDIRawModel
{
public:
  PlusNDIRawModel();
  void PrepareBindings(std::vector<PlusNDIMarkerBinding>&, uint64_t connection);
  void NewAcquisition(uint64_t reinitializedConnection = 0);
  bool InitialRecoveryPending() const { return InitialPending; }
  bool CompleteInitialRecovery(
    const PlusNDIRawRecoveryEvidence&); // consumes startup-prepared identity; no producer I/O
  void MarkFailed()
  {
    RecoveryRequired = true;
    Quarantined = true;
  } // acquisition producer only
  const std::string& Acquisition() const { return AcquisitionId; }
  const std::string& ClockDomain() const { return ClockDomainId; }
  std::shared_ptr<const PlusNDISnapshot> Seal(const ndiBXCapture&,
                                              const std::vector<PlusNDIMarkerBinding>&,
                                              uint64_t connection,
                                              uint64_t reply,
                                              uint64_t legacy = 0);

private:
  struct State
  {
    PlusNDIRawToolState Value;
    std::string Binding, Block;
    unsigned Last = 0;
    bool Have = false;
  };
  std::map<unsigned, State> States;
  std::set<std::string> QuarantinedTools;
  std::string AcquisitionId, ClockDomainId, PreparedRecoveryId;
  bool InitialPending = false;
  bool Quarantined = false, RecoveryRequired = false;
  uint64_t LastInitializedConnection = 0;
  uint64_t NextBinding = 0, PreviousMonotonic = 0, PreviousWall = 0;
};
struct PlusNDIRawConfig
{
  std::string SourceId, ReferenceFrame = "Tracker", SerialEndpoint, StatusDirectory;
  unsigned Port = 18945, QueueCapacity = 64, QueueBytes = 16 * 1024 * 1024, WriteTimeoutMs = 500,
           DrainTimeoutMs = 2000;
  double RequestedRate = 20;
};
class vtkPlusDataCollectionExport PlusNDIRawSender
{
public:
  PlusNDIRawSender();
  ~PlusNDIRawSender();
  bool Start(const PlusNDIRawConfig&, std::string& error);
  // Activation of an accepted connection is exclusively at this pre-BX boundary.
  void BeginAttempt(uint64_t reply, const std::vector<PlusNDIMarkerBinding>&, const PlusNDIRawModel&);
  // Freeze export without waiting; original BX polling may continue outside the old stream.
  int PrepareInitialRecovery(); // 0=wait, 1=END sent/no stream, -1=export failed
  uint64_t RecoveryEndSentNs() const;
  bool CommitInitialRecovery(PlusNDIRawModel&,
                             std::vector<PlusNDIMarkerBinding>&,
                             const PlusNDIRawRecoveryEvidence&);
  bool RecordRecoveryEvidence(const PlusNDIRawRecoveryEvidence&);
  bool Enqueue(const std::shared_ptr<const PlusNDISnapshot>&);
  bool Stop(const std::string& reason = "EXPORT_STOP");
  std::string State() const;
  std::string Error() const;
  // Socketpair harness only, never enabled by XML or network commands.
  bool StartConnectedForTest(const PlusNDIRawConfig&, int ownedSocket, std::string& error);

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};
namespace PlusNDIRawWire {
std::string
UUID();
std::string
Sha256(const unsigned char*, size_t);
std::string
Data(const PlusNDISnapshot&);
std::string
Frame(const PlusNDISnapshot&, const std::string& source, const std::string& session, uint64_t seq);
std::string
Session(const PlusNDIRawConfig&,
        const PlusNDIRawModel&,
        const std::vector<PlusNDIMarkerBinding>&,
        const std::string& session,
        const std::string& previous,
        uint64_t first);
std::string
Pack(const std::string& utf8);
}
#endif
