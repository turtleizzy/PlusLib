/* Local opt-in sidecar, 2026-10-26. Plus license; no ROM-derived coordinates. */
#ifndef PlusNDIMarkerRecorder_h
#define PlusNDIMarkerRecorder_h
#include "vtkPlusDataCollectionExport.h"
#include "ndicapi_bx_capture.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <stdint.h>
struct PlusNDIMarkerBinding
{
  unsigned int Handle=0;
  std::string TransformName,BodyFrame,RomPath,RomMD5,RomSHA256;
  uint64_t RawBindingEpoch=1;
  uint64_t RomBytes=0;
  bool Published=false;
  double Matrix[16]={0};
  std::string PlusStatus,PlusFrameSource;
  unsigned long PlusFrame=0;
  double PlusHostTime=0;
};
struct PlusNDISnapshot;
class vtkPlusDataCollectionExport PlusNDIMarkerRecorder
{
public:
  PlusNDIMarkerRecorder();
  ~PlusNDIMarkerRecorder();
  bool Start(const std::string& parent,const std::string& tracker,unsigned capacity,
             double requestedRate,unsigned requestedBaud,std::string& error,const std::string& sourceEndpoint="");
  bool Enqueue(const ndiBXCapture&,const std::vector<PlusNDIMarkerBinding>&,uint64_t epoch,uint64_t poll);
  bool Enqueue(const std::shared_ptr<const PlusNDISnapshot>&);
  bool Stop(const std::string& failure="");
  bool Healthy()const;
  std::string Error()const;
  std::string Directory()const;
  // Storage lifecycle only; never denotes tool/marker measurement validity.
  std::string StorageState()const;
  uint64_t RejectedOutsideSession()const;
  static bool IdentifyRom(const std::string& path,std::string& md5,uint64_t& bytes);
private:
  struct Impl;
  std::unique_ptr<Impl> P;
  mutable std::mutex LifecycleMutex;
  PlusNDIMarkerRecorder(const PlusNDIMarkerRecorder&)=delete;
  void operator=(const PlusNDIMarkerRecorder&)=delete;
};
#endif
