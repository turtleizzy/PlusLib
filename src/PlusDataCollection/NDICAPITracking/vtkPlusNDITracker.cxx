/*=Plus=header=begin======================================================
  Program: Plus
  Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
  See License.txt for details.
=========================================================Plus=header=end*/

/*=========================================================================
The following copyright notice is applicable to parts of this file:

Copyright (c) 2000-2005 Atamai, Inc.

Use, modification and redistribution of the software, in source or
binary forms, are permitted provided that the following terms and
conditions are met:

1) Redistribution of the source code, in verbatim or modified
form, must retain the above copyright notice, this license,
the following disclaimer, and any notices that refer to this
license and/or the following disclaimer.

2) Redistribution in binary form must include the above copyright
notice, a copy of this license and the following disclaimer
in the documentation or with other materials provided with the
distribution.

3) Modified copies of the source code must be clearly marked as such,
and must not be misrepresented as verbatim copies of the source code.

THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE SOFTWARE "AS IS"
WITHOUT EXPRESSED OR IMPLIED WARRANTY INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE.  IN NO EVENT SHALL ANY COPYRIGHT HOLDER OR OTHER PARTY WHO MAY
ODIFY AND/OR REDISTRIBUTE THE SOFTWARE UNDER THE TERMS OF THIS LICENSE
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, LOSS OF DATA OR DATA BECOMING INACCURATE
OR LOSS OF PROFIT OR BUSINESS INTERRUPTION) ARISING IN ANY WAY OUT OF
THE USE OR INABILITY TO USE THE SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGES.

=========================================================================*/

// Local includes
#include "PlusConfigure.h"
#include "vtkIGSIORecursiveCriticalSection.h"
#include "vtkPlusDataSource.h"
#include "vtkPlusNDITracker.h"
#include "PlusNDIMarkerRecorder.h"
#include "PlusNDIRaw.h"
#include <fstream>
#include <cmath>
#include <stdexcept>
#include <time.h>
#include <sys/stat.h>

// NDI includes
#include <ndicapi.h>
#include <ndicapi_math.h>
#include <ndicapi_serial.h>

// VTK includes
#include <vtkCharArray.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>
#include <vtkTimerLog.h>
#include <vtkTransform.h>

// igsio includes
#include <igsioCommon.h>

// System includes
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>

#if defined(HAVE_FUTURE)
  #include <future>
#endif

namespace
{
  const int VIRTUAL_SROM_SIZE = 1024;
}

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkPlusNDITracker);

//----------------------------------------------------------------------------
vtkPlusNDITracker::vtkPlusNDITracker()
  : EnableMeasuredMarkers(false)
  , MarkerQueueCapacity(64)
  , MarkerReplyTimeoutMs(5000)
  , MarkerConnectionEpoch(0)
  , MarkerPollSequence(0)
  , MarkerErrorReported(false)
  , MarkerRequestUnsupported(false)
  , MarkerTransportDesynchronized(false)
  , MarkerRecorder(nullptr)
  , MarkerReply(nullptr)
  , LastFrameNumber(0)
  , Device(nullptr)
  , SerialDevice("")
  , SerialPort(-1)
  , BaudRate(0)
  , IsDeviceTracking(0)
  , LeaveDeviceOpenAfterProbe(false)
  , CheckDSR(true)
  , MeasurementVolumeNumber(0)
  , NetworkHostname("")
  , NetworkPort(8765)
  , TrackingFrequencyNumber(0)
  , FirmwareMajorRevision(1)
  , FirmwareMinorRevision(1)
  , HardwareDataAveragingDepth(1)
  , CommandMutex(vtkIGSIORecursiveCriticalSection::New())
{
  memset(this->CommandReply, 0, VTK_NDI_REPLY_LEN);

  // PortName for data source is not required if RomFile is specified, so we don't need to enable this->RequirePortNameInDeviceSetConfiguration

  // No callback function provided by the device, so the data capture thread will be used to poll the hardware and add new items to the buffer
  this->StartThreadForInternalUpdates = true;
  this->AcquisitionRate = 50;
}

//----------------------------------------------------------------------------
vtkPlusNDITracker::~vtkPlusNDITracker()
{
  if (this->Recording)
  {
    this->StopRecording();
  }
  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    delete [] toolDescriptorIt->second.VirtualSROM;
    toolDescriptorIt->second.VirtualSROM = NULL;
  }
  if (this->MarkerRecorder) { this->MarkerRecorder->Stop(); delete this->MarkerRecorder; }
  delete this->RawSender;
  delete this->RawModel;
  delete this->MarkerReply;
  this->CommandMutex->Delete();
  this->CommandMutex = nullptr;
}

//----------------------------------------------------------------------------
void vtkPlusNDITracker::PrintSelf(ostream& os, vtkIndent indent)
{
  Superclass::PrintSelf(os, indent);

  char ndiLog[USHRT_MAX];
  ndiLogState(this->Device, ndiLog);

  os << indent << "SerialDevice: " << this->SerialDevice << std::endl;
  os << indent << "SerialPort: " << this->SerialPort << std::endl;
  os << indent << "BaudRate: " << this->BaudRate << std::endl;
  os << indent << "IsDeviceTracking: " << this->IsDeviceTracking << std::endl;
  os << indent << "MeasurementVolumeNumber: " << this->MeasurementVolumeNumber << std::endl;
  os << indent << "TrackingFrequencyNumber: " << this->TrackingFrequencyNumber << std::endl;
  os << indent << "CommandReply: " << this->CommandReply << std::endl;
  os << indent << "LastFrameNumber: " << this->LastFrameNumber << std::endl;
  os << indent << "LeaveDeviceOpenAfterProbe: " << this->LeaveDeviceOpenAfterProbe << std::endl;
  os << indent << "CheckDSR: " << this->CheckDSR << std::endl;
  os << indent << "HardwareDataAveragingDepth" << this->HardwareDataAveragingDepth << std::endl;
  os << indent << "FirmwareMajorRevision" << this->FirmwareMajorRevision << std::endl;
  os << indent << "FirmwareMinorRevision" << this->FirmwareMinorRevision << std::endl;
  for (auto iter = this->NdiToolDescriptors.begin(); iter != this->NdiToolDescriptors.end(); ++iter)
  {
    os << indent << iter->first << ": " << std::endl;
    os << indent << "  " << "PortEnabled: " << iter->second.PortEnabled << std::endl;
    os << indent << "  " << "PortHandle: " << iter->second.PortHandle << std::endl;
    os << indent << "  " << "VirtualSROM: " << iter->second.VirtualSROM << std::endl;
    os << indent << "  " << "WiredPortNumber: " << iter->second.WiredPortNumber << std::endl;
  }
}

//----------------------------------------------------------------------------
std::string vtkPlusNDITracker::GetSdkVersion()
{
  std::ostringstream version;
  version << "NDICAPI-" << NDICAPI_MAJOR_VERSION << "." << NDICAPI_MINOR_VERSION;
  return version.str();
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::Probe()
{
  if (this->IsDeviceTracking)
  {
    return PLUS_SUCCESS;
  }

  if (this->SerialPort > 0)
  {
    const char* devicename = NULL;
    int errnum = NDI_OPEN_ERROR;
    devicename = ndiSerialDeviceName(this->SerialPort - 1);
    if (devicename)
    {
      errnum = ndiSerialProbe(devicename, this->CheckDSR);
      LOG_DEBUG("Serial port " << devicename << " probe error (" << errnum << "): " << ndiErrorString(errnum));
    }

    if (errnum != NDI_OKAY)
    {
      return PLUS_FAIL;
    }

    this->Device = ndiOpenSerial(devicename);
    if (this->Device && !this->LeaveDeviceOpenAfterProbe)
    {
      CloseDevice(this->Device);
    }
    return PLUS_SUCCESS;
  }
  else if (this->NetworkHostname.empty())
  {
#if defined(HAVE_FUTURE)
    return ProbeSerialInternal();
#else
    // if SerialPort is set to -1 (default), then probe the first N serial ports
    const char* devicename = NULL;
    int errnum = NDI_OPEN_ERROR;

    const int MAX_SERIAL_PORT_NUMBER = 20; // the serial port is almost surely less than this number
    for (int i = 0; i < MAX_SERIAL_PORT_NUMBER; i++)
    {
      devicename = ndiSerialDeviceName(i);
      LOG_DEBUG("Testing serial port: " << devicename);
      if (devicename)
      {
        errnum = ndiSerialProbe(devicename, this->CheckDSR);
        LOG_DEBUG("Serial port " << devicename << " probe error (" << errnum << "): " << ndiErrorString(errnum));
        if (errnum == NDI_OKAY)
        {
          this->SerialPort = i + 1;
          this->Device = ndiOpenSerial(devicename);
          LOG_DEBUG("device: " << (this->Device == nullptr));
          if (this->Device && !this->LeaveDeviceOpenAfterProbe)
          {
            CloseDevice(this->Device);
          }
          return PLUS_SUCCESS;
        }
      }
    }
#endif
  }
  else
  {
    // TODO: probe network?
  }

  return PLUS_FAIL;
}

//----------------------------------------------------------------------------
// Send a raw command to the tracking unit.
// If communication has already been opened with the NDI,
// then lock the mutex to get exclusive access and then
// send the command.
// Otherwise, open communication with the unit, send the command,
// and close communication.
std::string vtkPlusNDITracker::Command(const char* format, ...)
{
  va_list ap;            // see stdarg.h
  va_start(ap, format);

  this->CommandReply[0] = '\0';

  char command[2048];
  if (format != nullptr)
  {
    vsprintf(command, format, ap);
    LOG_DEBUG("NDI Command:" << command);
  }
  else
  {
    LOG_DEBUG("NDI Command:send serial break");
  }

  // Linux and MacOSX require resetting of va parameters
  va_end(ap);
  va_start(ap, format);

  if (this->Device)
  {
    igsioLockGuard<vtkIGSIORecursiveCriticalSection> lock(this->CommandMutex);
    strncpy(this->CommandReply, ndiCommandVA(this->Device, format, ap), VTK_NDI_REPLY_LEN - 1);
    this->CommandReply[VTK_NDI_REPLY_LEN - 1] = '\0';
  }

  std::string cmd(command);
  if (cmd.find(':') != std::string::npos)
  {
    cmd = cmd.substr(0, cmd.find(':'));
  }
  else if (cmd.find(' ') != std::string::npos)
  {
    cmd = cmd.substr(0, cmd.find(' '));
  }
  bool isBinary = (cmd == "BX" || cmd == "BX2" || cmd == "GETLOG" || cmd == "VGET");

  if (vtkIGSIOLogger::Instance()->GetLogLevel() >= vtkIGSIOLogger::LOG_LEVEL_DEBUG)
  {
    if (!isBinary)
    {
      LOG_DEBUG("NDI Reply: " << this->CommandReply);
    }
    else
    {
      std::stringstream ss;
      ss << "NDI Reply: ";
      ss << std::hex;
      for (unsigned int i = 0; i < ndiGetBX2ReplyLength(this->Device); ++i)
      {
        ss << std::setfill('0') << std::setw(2) << (unsigned int)(unsigned char)this->CommandReply[i];
      }
      ss << std::endl;
      LOG_DEBUG(ss.str());
    }
  }

  va_end(ap);

  return this->CommandReply;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalConnect()
{
  PlusStatus result;
  if (!this->NetworkHostname.empty())
  {
    result = InternalConnectNetwork();
  }
  else
  {
    result = InternalConnectSerial();
  }

  if (result != PLUS_SUCCESS)
  {
    LOG_ERROR("Unable to connect to NDI device.");
    return result;
  }

  GetFirmwareRevision();
  if (this->EnableMeasuredMarkers && this->FirmwareMajorRevision >= 3)
  {
    LOG_ERROR("Measured markers require the supported legacy BX serial path; BX2 is not implemented.");
    CloseDevice(this->Device);
    return PLUS_FAIL;
  }

  SelectDataAveraging();

  SelectMeasurementVolume();
  SelectTrackingFrequency();

  if (this->EnableToolPorts() != PLUS_SUCCESS)
  {
    LOG_ERROR("Failed to enable tool ports");
    return PLUS_FAIL;
  }

  if (this->EnableMeasuredMarkers)
  {
    ++this->MarkerConnectionEpoch;
    this->MarkerRequestUnsupported = false;
    this->MarkerTransportDesynchronized = false;
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::GetFirmwareRevision()
{
  this->FirmwareMajorRevision = 1;
  std::string revision = "";
  revision = this->Command("GET:%s", "Features.Firmware.API Revision");
  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    revision = this->Command("APIREV ");
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
      return PLUS_FAIL;
    }
    else
    {
      // Parse APIREV reply
      // revision format "G.00X.00Y"
      std::vector<std::string> result = igsioCommon::SplitStringIntoTokens(revision, '.', false);
      uint32_t ver;
      igsioCommon::StringToNumber<uint32_t>(result[1].c_str(), ver);
      this->FirmwareMajorRevision = ver;
      igsioCommon::StringToNumber<uint32_t>(result[2].c_str(), ver);
      this->FirmwareMinorRevision = ver;
    }
  }
  else
  {
    // Parse GET reply
    // revision format "G.00X.00Y"
    std::vector<std::string> answers = igsioCommon::SplitStringIntoTokens(revision, '=', false);
    std::vector<std::string> result = igsioCommon::SplitStringIntoTokens(answers[1], '.', false);
    uint32_t ver;
    igsioCommon::StringToNumber<uint32_t>(result[1].c_str(), ver);
    this->FirmwareMajorRevision = ver;
    igsioCommon::StringToNumber<uint32_t>(result[2].c_str(), ver);
    this->FirmwareMinorRevision = ver;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalConnectSerial()
{
  this->LeaveDeviceOpenAfterProbe = true;
  if (this->Probe() == PLUS_FAIL)
  {
    LOG_ERROR("Failed to detect device" << (this->SerialPort < 0 ? ". Port scanning failed. " : " on serial port " + std::string(ndiSerialDeviceName(this->SerialPort)) + " (index " + igsioCommon::ToString<int>(this->SerialPort) + "). ") << ndiErrorString(NDI_OPEN_ERROR));
    return PLUS_FAIL;
  }

  if (this->BaudRate != 0)
  {
    int baudVal = vtkPlusNDITracker::ConvertBaudToNDIEnum(this->BaudRate);
    // BaudRate has been requested, let's attempt it before falling back to best available
    this->Command("COMM:%X%03d%d", baudVal, NDI_8N1, NDI_NOHANDSHAKE);
    int errnum = ndiGetError(this->Device);
    if (errnum == NDI_OKAY)
    {
      return PLUS_SUCCESS;
    }
    else
    {
      LOG_WARNING("Unable to set requested baud rate. Reverting to auto-select.");
    }
  }

#if defined(WIN32)
  // TODO: use the lines in this comment to replace next 5 lines when VS2010 support is dropped:
  //auto baudRates = { NDI_1228739, NDI_921600, NDI_230400, NDI_115200, NDI_57600, NDI_38400, NDI_19200, NDI_14400, NDI_9600 };
  //for (auto baud : baudRates)
  const unsigned int numberOfBaudRates = 9;
  int baudRates[numberOfBaudRates] = { NDI_1228739, NDI_921600, NDI_230400, NDI_115200, NDI_57600, NDI_38400, NDI_19200, NDI_14400, NDI_9600 };
#elif defined(__APPLE__)
  const unsigned int numberOfBaudRates = 6;
  int baudRates[numberOfBaudRates] = { NDI_115200, NDI_57600, NDI_38400, NDI_19200, NDI_14400, NDI_9600 };
#elif defined(__linux__)
  const unsigned int numberOfBaudRates = 6;
  int baudRates[numberOfBaudRates] = { NDI_115200, NDI_57600, NDI_38400, NDI_19200, NDI_14400, NDI_9600 };
#else
  const unsigned int numberOfBaudRates = 6;
  int baudRates[numberOfBaudRates] = { NDI_115200, NDI_57600, NDI_38400, NDI_19200, NDI_14400, NDI_9600 };
#endif
  for (unsigned int baudIndex = 0; baudIndex < numberOfBaudRates; baudIndex++)
  {
    int baud = baudRates[baudIndex];

    this->Command("COMM:%X%03d%d", baud, NDI_8N1, NDI_NOHANDSHAKE);
    int errnum = ndiGetError(this->Device);
    if (errnum != NDI_OKAY && errnum != NDI_COMM_FAIL)
    {
      // A real problem, abort
      LOG_ERROR(ndiErrorString(errnum));
      ndiCloseSerial(this->Device);
      this->Device = nullptr;
      return PLUS_FAIL;
    }
    if (errnum == NDI_OKAY)
    {
      // Fastest baud rate set, end
      break;
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalConnectNetwork()
{
  this->Device = ndiOpenNetwork(this->NetworkHostname.c_str(), this->NetworkPort);

  if (this->Device == nullptr)
  {
    LOG_ERROR("Unable to connect to " << this->NetworkHostname << ":" << this->NetworkPort);
    return PLUS_FAIL;
  }

  // First init can take up to 5 seconds
  ndiTimeoutSocket(this->Device, 5000);

  auto reply = this->Command("INIT");

  // Subsequent commands should be much faster
  ndiTimeoutSocket(this->Device, 100);

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalDisconnect()
{
  PlusStatus result = PLUS_SUCCESS;
  if (this->MarkerRecorder && !this->MarkerRecorder->Stop(this->RawSnapshotFailed?"raw snapshot allocation failure":""))
  {
    result = PLUS_FAIL;
    LOG_ERROR("Measured marker sidecar did not close completely: " << this->MarkerRecorder->Error());
  }
  if (!this->RawStartupError.empty()) { result = PLUS_FAIL; }
  if (this->RawSender && !this->RawSender->Stop("SOURCE_STOP")) { result = PLUS_FAIL; }
  if (!this->Device) { return result; }
  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    this->ClearVirtualSromInTracker(toolDescriptorIt->second);
  }

  this->DisableToolPorts();

  // return to default comm settings
  this->Command("COMM:00000");
  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    LOG_ERROR(ndiErrorString(errnum));
  }
  CloseDevice(this->Device);

  return result;
}


//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalStartRecording()
{
  if (this->IsDeviceTracking)
  {
    return PLUS_SUCCESS;
  }

  if (this->EnableMeasuredMarkers)
  {
    if (this->MarkerTransportDesynchronized || (this->Device && this->Device->BxCaptureSynchronization < 0))
    {
      LOG_ERROR("BX stream requires disconnect/reconnect and initialization before restarting capture.");
      return PLUS_FAIL;
    }
    if (!this->MarkerRecorder) { this->MarkerRecorder = new PlusNDIMarkerRecorder; }
    if (!this->MarkerReply) { this->MarkerReply = new ndiBXCapture; }
    std::string error;
    if (!this->MarkerRecorder->Start(this->MarkerSidecarDirectory, this->GetToolReferenceFrameName(),
        this->MarkerQueueCapacity, this->AcquisitionRate, this->BaudRate, error, ndiGetSerialDeviceName(this->Device)))
    {
      LOG_ERROR("Cannot start measured marker sidecar: " << error);
      return PLUS_FAIL;
    }
    this->MarkerPollSequence = 0;
    this->RawReplySequence=0;
    this->RawSnapshotFailed = false;
    if (this->EnableRawEndpoint)
    {
      this->RawStartupError.clear();
      try {
      if(this->RawProtocol!="ndi.raw.v1" || this->RawSchemaMinor!=0)throw std::invalid_argument("raw profile requires explicit RawProtocol=ndi.raw.v1 and RawSchemaMinor=0");
      if (!this->RawModel)this->RawModel=new PlusNDIRawModel;
      this->RawModel->NewAcquisition(this->MarkerConnectionEpoch);
      if (!this->RawSender)this->RawSender=new PlusNDIRawSender;
      PlusNDIRawConfig config;config.SourceId=this->RawSourceId;
      config.ReferenceFrame=this->GetToolReferenceFrameName();
      config.SerialEndpoint=ndiGetSerialDeviceName(this->Device);
      config.StatusDirectory=this->MarkerSidecarDirectory;
      config.Port=this->RawPort;config.QueueCapacity=this->RawQueueCapacity;
      config.QueueBytes=this->RawQueueBytes;config.WriteTimeoutMs=this->RawWriteTimeoutMs;
      config.DrainTimeoutMs=this->RawDrainTimeoutMs;config.RequestedRate=this->AcquisitionRate;
      if (!this->RawSender->Start(config,error))
      {
        LOG_ERROR("Raw endpoint startup failed; existing TRANSFORM and sidecar continue: " << error);
      }
      this->Device->BxRawCapture=1;
      }catch(const std::exception& e){
        this->RawStartupError=e.what();
        LOG_ERROR("Raw initialization failed; legacy collection continues: " << this->RawStartupError);
      }
    }
    this->MarkerErrorReported = false;
    LOG_INFO("Measured marker sidecar: " << this->MarkerRecorder->Directory());
  }
  this->Command("TSTART:");
  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    LOG_ERROR("Failed TSTART: " << ndiErrorString(errnum));
    if (this->RawSender)this->RawSender->Stop("ERROR");
    if (this->MarkerRecorder) { this->MarkerRecorder->Stop("TSTART failed"); }
    CloseDevice(this->Device);
    return PLUS_FAIL;
  }

  this->IsDeviceTracking = 1;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalStopRecording()
{
  PlusStatus result = PLUS_SUCCESS;
  if (this->Device)
  {
    if (this->IsDeviceTracking)
    {
      this->Command("TSTOP:");
      int errnum = ndiGetError(this->Device);
      if (errnum) { LOG_ERROR(ndiErrorString(errnum)); result = PLUS_FAIL; }
    }
  }
  else { result = PLUS_FAIL; }
  this->IsDeviceTracking = 0;
  if (!this->RawStartupError.empty()) { result = PLUS_FAIL; }
  if (this->RawSender && !this->RawSender->Stop("SOURCE_STOP")) { result = PLUS_FAIL; }
  // Always finalize, including a missing/closed device. Storage status is separate.
  if (this->MarkerRecorder && !this->MarkerRecorder->Stop(this->RawSnapshotFailed?"raw snapshot allocation failure":""))
  {
    LOG_ERROR("Measured marker archive is incomplete: " << this->MarkerRecorder->Error());
    result = PLUS_FAIL;
  }
  return result;
}

// Base Disconnect discards its InternalDisconnect result; keep the correction
// local to this tracker. Repeated public Stop/Disconnect must retain archive failure.
PlusStatus vtkPlusNDITracker::StopRecording()
{
  PlusStatus result = this->Superclass::StopRecording();
  if (!this->RawStartupError.empty()) { result = PLUS_FAIL; }
  if (this->RawSender && !this->RawSender->Stop("SOURCE_STOP")) { result = PLUS_FAIL; }
  if (this->MarkerRecorder && !this->MarkerRecorder->Stop(this->RawSnapshotFailed?"raw snapshot allocation failure":"")) { result = PLUS_FAIL; }
  return result;
}
PlusStatus vtkPlusNDITracker::Disconnect()
{
  PlusStatus result = PLUS_SUCCESS;
  if (this->Recording && this->StopRecording() != PLUS_SUCCESS) { result = PLUS_FAIL; }
  if (this->Superclass::Disconnect() != PLUS_SUCCESS) { result = PLUS_FAIL; }
  if (!this->RawStartupError.empty()) { result = PLUS_FAIL; }
  if (this->RawSender && !this->RawSender->Stop("SOURCE_STOP")) { result = PLUS_FAIL; }
  if (this->MarkerRecorder && !this->MarkerRecorder->Stop(this->RawSnapshotFailed?"raw snapshot allocation failure":"")) { result = PLUS_FAIL; }
  return result;
}
std::string vtkPlusNDITracker::GetMeasuredMarkerStorageState() const
{
  return this->MarkerRecorder ? this->MarkerRecorder->StorageState() : "never_started";
}
std::string vtkPlusNDITracker::GetMeasuredMarkerStorageError() const
{
  return this->MarkerRecorder ? this->MarkerRecorder->Error() : std::string();
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::InternalUpdate()
{
  if (!this->IsDeviceTracking)
  {
    LOG_ERROR("called Update() when NDI was not tracking");
    return PLUS_FAIL;
  }

  if (this->EnableMeasuredMarkers && (this->MarkerRequestUnsupported || this->MarkerTransportDesynchronized)) { return PLUS_FAIL; }
  if (this->FirmwareMajorRevision < 3)
  {
    return this->DoBXUpdate();
  }
  else
  {
    return this->DoBX2Update();
  }
  
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::ReadSromFromFile(NdiToolDescriptor& toolDescriptor, const char* filename)
{
  FILE* file = fopen(filename, "rb");
  if (file == NULL)
  {
    LOG_ERROR("Couldn't find srom file " << filename);
    return PLUS_FAIL;
  }

  if (toolDescriptor.VirtualSROM == 0)
  {
    toolDescriptor.VirtualSROM = new unsigned char[VIRTUAL_SROM_SIZE];
  }

  memset(toolDescriptor.VirtualSROM, 0, VIRTUAL_SROM_SIZE);
  fread(toolDescriptor.VirtualSROM, 1, VIRTUAL_SROM_SIZE, file);
  fclose(file);
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::EnableToolPorts()
{
  PlusStatus status = PLUS_SUCCESS;

  // stop tracking
  if (this->IsDeviceTracking)
  {
    this->Command("TSTOP:");
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
      status = PLUS_FAIL;
    }
  }

  // free ports that are waiting to be freed
  this->Command("PHSR:01");
  int errnum = ndiGetError(this->Device);
  if (errnum != NDI_OKAY)
  {
    LOG_ERROR("Unable to get the number of handles. Error: " << ndiErrorString(errnum));
    return PLUS_FAIL;
  }

  int ntools = ndiGetPHSRNumberOfHandles(this->Device);
  for (int ndiToolIndex = 0; ndiToolIndex < ntools; ndiToolIndex++)
  {
    int portHandle = ndiGetPHSRHandle(this->Device, ndiToolIndex);
    this->Command("PHF:%02X", portHandle);
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
      status = PLUS_FAIL;
    }
  }

  // Set port handles and send SROM files to tracker
  // We need to do this before initializing and enabling
  // the ports waiting to be initialized.
  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    if (toolDescriptorIt->second.VirtualSROM != NULL)   // wireless tool (or wired tool with virtual rom)
    {
      if (this->UpdatePortHandle(toolDescriptorIt->second) != PLUS_SUCCESS)
      {
        LOG_ERROR("Failed to determine NDI port handle for tool " << toolDescriptorIt->first);
        return PLUS_FAIL;
      }
      if (this->SendSromToTracker(toolDescriptorIt->second) != PLUS_SUCCESS)
      {
        LOG_ERROR("Failed send SROM to NDI tool " << toolDescriptorIt->first);
        return PLUS_FAIL;
      }
    }
  }

  // initialize ports waiting to be initialized
  errnum = 0;
  ntools = 0;
  do // repeat as necessary (in case multi-channel tools are used)
  {
    this->Command("PHSR:02");
    ntools = ndiGetPHSRNumberOfHandles(this->Device);
    for (int ndiToolIndex = 0; ndiToolIndex < ntools; ndiToolIndex++)
    {
      int portHandle = ndiGetPHSRHandle(this->Device, ndiToolIndex);
      this->Command("PINIT:%02X", portHandle);
      errnum = ndiGetError(this->Device);
      if (errnum)
      {
        std::stringstream ss;
        ss << ndiErrorString(errnum) << ". errnum: " << errnum;
        LOG_ERROR(ss.str());
        status = PLUS_FAIL;
      }
    }
  }
  while (ntools > 0 && errnum == 0);

  // enable initialized tools
  this->Command("PHSR:03");
  ntools = ndiGetPHSRNumberOfHandles(this->Device);
  for (int ndiToolIndex = 0; ndiToolIndex < ntools; ndiToolIndex++)
  {
    int portHandle = ndiGetPHSRHandle(this->Device, ndiToolIndex);
    this->Command("PHINF:%02X0001", portHandle);
    char identity[34];
    ndiGetPHINFToolInfo(this->Device, identity);
    int mode = 'D'; // default
    if (identity[1] == 0x03)   // button-box
    {
      mode = 'B';
    }
    else if (identity[1] == 0x01)   // reference
    {
      mode = 'S';
    }
    // enable the tool
    this->Command("PENA:%02X%c", portHandle, mode);
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
      status = PLUS_FAIL;
    }
  }

  // Set wired port handles and send SROM files to tracker
  // We need to do this after enabling all the tools because tools on
  // splitters (two 5-DOF tools with one connector) only appear after the tool is enabled.
  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    if (toolDescriptorIt->second.WiredPortNumber >= 0 && toolDescriptorIt->second.VirtualSROM == NULL)   //wired tool, no virtual ROM
    {
      if (this->UpdatePortHandle(toolDescriptorIt->second) != PLUS_SUCCESS)
      {
        LOG_ERROR("Failed to determine NDI port handle for tool " << toolDescriptorIt->first);
        return PLUS_FAIL;
      }
      if (this->SendSromToTracker(toolDescriptorIt->second) != PLUS_SUCCESS)
      {
        LOG_ERROR("Failed send SROM to NDI tool " << toolDescriptorIt->first);
        return PLUS_FAIL;
      }
    }
  }

  // Update tool info
  this->Command("PHSR:00");

  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    vtkPlusDataSource* trackerTool = NULL;
    if (this->GetTool(toolDescriptorIt->first, trackerTool) != PLUS_SUCCESS)
    {
      LOG_ERROR("Failed to get NDI tool: " << toolDescriptorIt->first);
      status = PLUS_FAIL;
      continue;
    }

    this->Command("PHINF:%02X0025", toolDescriptorIt->second.PortHandle);
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
      status = PLUS_FAIL;
      continue;
    }

    // decompose identity string from end to front
    char identity[34];
    ndiGetPHINFToolInfo(this->Device, identity);
    identity[31] = '\0';
    std::string serialNumber(&identity[23]);
    igsioCommon::Trim(serialNumber);
    trackerTool->SetCustomProperty("SerialNumber", serialNumber);
    identity[23] = '\0';
    std::string toolRevision(&identity[20]);
    igsioCommon::Trim(toolRevision);
    trackerTool->SetCustomProperty("Revision", toolRevision);
    identity[20] = '\0';
    std::string toolManufacturer(&identity[8]);
    igsioCommon::Trim(toolManufacturer);
    trackerTool->SetCustomProperty("Manufacturer", toolManufacturer);
    identity[8] = '\0';
    std::string ndiIdentity(&identity[0]);
    igsioCommon::Trim(ndiIdentity);
    trackerTool->SetCustomProperty("NdiIdentity", ndiIdentity);
    char partNumber[24];
    ndiGetPHINFPartNumber(this->Device, partNumber);
    partNumber[20] = '\0';
    std::string toolPartNumber(&partNumber[0]);
    igsioCommon::Trim(toolPartNumber);
    trackerTool->SetCustomProperty("PartNumber", toolPartNumber);
    int status = ndiGetPHINFPortStatus(this->Device);

    toolDescriptorIt->second.PortEnabled = ((status & NDI_ENABLED) != 0);
    if (!toolDescriptorIt->second.PortEnabled)
    {
      LOG_ERROR("Failed to enable NDI tool " << toolDescriptorIt->first);
      status = PLUS_FAIL;
    }
  }

  // re-start the tracking
  if (this->IsDeviceTracking)
  {
    this->Command("TSTART:");
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR("Failed TSTART: " << ndiErrorString(errnum));
      status = PLUS_FAIL;
    }
  }

  return status;
}

//----------------------------------------------------------------------------
// Disable all enabled tool ports.
void vtkPlusNDITracker::DisableToolPorts()
{
  // stop tracking
  if (this->IsDeviceTracking)
  {
    this->Command("TSTOP:");
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
    }
  }

  // disable all enabled tools
  this->Command("PHSR:04");
  int ntools = ndiGetPHSRNumberOfHandles(this->Device);
  for (int ndiToolIndex = 0; ndiToolIndex < ntools; ndiToolIndex++)
  {
    int portHandle = ndiGetPHSRHandle(this->Device, ndiToolIndex);
    this->Command("PDIS:%02X", portHandle);
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
    }
  }

  // disable the enabled ports
  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    toolDescriptorIt->second.PortEnabled = false;
  }

  // re-start the tracking
  if (this->IsDeviceTracking)
  {
    this->Command("TSTART:");
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR(ndiErrorString(errnum));
    }
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::Beep(int n)
{
  if (this->Recording)
  {
    LOG_ERROR("vtkPlusNDITracker::Beep failed: not connected to the device");
    return PLUS_FAIL;
  }
  if (n > 9)
  {
    n = 9;
  }
  if (n < 0)
  {
    n = 0;
  }
  this->Command("BEEP:%i", n);
  int errnum = ndiGetError(this->Device);

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus  vtkPlusNDITracker::SetToolLED(const char* sourceId, int led, LedState state)
{
  if (!this->Recording)
  {
    LOG_ERROR("vtkPlusNDITracker::InternalSetToolLED failed: not recording");
    return PLUS_FAIL;
  }
  NdiToolDescriptorsType::iterator ndiToolDescriptorIt = this->NdiToolDescriptors.find(sourceId);
  if (ndiToolDescriptorIt == this->NdiToolDescriptors.end())
  {
    LOG_ERROR("InternalSetToolLED failed: Tool descriptor is not found for tool " << sourceId);
    return PLUS_FAIL;
  }
  int portHandle = ndiToolDescriptorIt->second.PortHandle;
  if (portHandle <= 0)
  {
    LOG_ERROR("vtkPlusNDITracker::InternalSetToolLED failed: invalid port handle");
    return PLUS_FAIL;
  }

  int plstate = NDI_BLANK;
  switch (state)
  {
    case TR_LED_OFF:
      plstate = NDI_BLANK;
      break;
    case TR_LED_ON:
      plstate = NDI_SOLID;
      break;
    case TR_LED_FLASH:
      plstate = NDI_FLASH;
      break;
    default:
      LOG_ERROR("vtkPlusNDITracker::InternalSetToolLED failed: unsupported LED state: " << state);
      return PLUS_FAIL;
  }

  this->Command("LED:%02X%d%c", portHandle, led + 1, plstate);
  int errnum = ndiGetError(this->Device);

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::UpdatePortHandle(NdiToolDescriptor& toolDescriptor)
{
  if (toolDescriptor.WiredPortNumber >= 0)   // wired tool
  {
    this->Command("PHSR:00");
    int ntools = ndiGetPHSRNumberOfHandles(this->Device);
    int ndiToolIndex = 0;
    for (; ndiToolIndex < ntools; ndiToolIndex++)
    {
      if (ndiGetPHSRInformation(this->Device, ndiToolIndex) & NDI_TOOL_IN_PORT)
      {
        int portHandle = ndiGetPHSRHandle(this->Device, ndiToolIndex);
        this->Command("PHINF:%02X0021", portHandle);
        char location[14];
        ndiGetPHINFPortLocation(this->Device, location);
        int foundWiredPortNumber = (location[10] - '0') * 10 + (location[11] - '0') - 1;
        int foundWiredPortChannel = (location[12] - '0') * 10 + (location[13] - '0');     // this is nonzero if 5-DOF tools with splitter
        int combinedPortAndChannelNumber = foundWiredPortChannel * 100 + foundWiredPortNumber;
        if (toolDescriptor.WiredPortNumber == combinedPortAndChannelNumber)
        {
          // found the portHandle
          toolDescriptor.PortHandle = portHandle;
          break;
        }
      }
    }
    if (ndiToolIndex == ntools)
    {
      LOG_ERROR("Active NDI tool not found in port " << toolDescriptor.WiredPortNumber << ". Make sure the tool is plugged in.");
      return PLUS_FAIL;
    }
  }
  else // wireless tool
  {
    this->Command("PHRQ:*********1****");
    int portHandle = ndiGetPHRQHandle(this->Device);
    toolDescriptor.PortHandle = portHandle;
  }

  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    LOG_ERROR(ndiErrorString(errnum));
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}


//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::SendSromToTracker(const NdiToolDescriptor& toolDescriptor)
{
  if (toolDescriptor.VirtualSROM == NULL)
  {
    // nothing to load
    return PLUS_SUCCESS;
  }

  igsioLockGuard<vtkIGSIORecursiveCriticalSection> lock(this->CommandMutex);
  const int TRANSFER_BLOCK_SIZE = 64; // in bytes
  char hexbuffer[TRANSFER_BLOCK_SIZE * 2];
  for (int i = 0; i < VIRTUAL_SROM_SIZE; i += TRANSFER_BLOCK_SIZE)
  {
    RETRY_UNTIL_TRUE(
      strcmp(this->Command("PVWR:%02X%04X%.128s", toolDescriptor.PortHandle, i,
                           ndiHexEncode(hexbuffer, &(toolDescriptor.VirtualSROM[i]), TRANSFER_BLOCK_SIZE)).c_str(), "OKAY") == 0,
      10, 0.1);

    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      std::stringstream ss;
      ss << "Failed to send SROM to NDI tracker: " << ndiErrorString(errnum);
      LOG_ERROR(ss.str());
      return PLUS_FAIL;
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::ClearVirtualSromInTracker(NdiToolDescriptor& toolDescriptor)
{
  if (toolDescriptor.VirtualSROM == NULL)
  {
    // nothing to clear
    return PLUS_SUCCESS;
  }

  this->Command("PHF:%02X", toolDescriptor.PortHandle);
  toolDescriptor.PortEnabled = false;
  toolDescriptor.PortHandle = 0;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  // Clean up any previously read config data
  for (NdiToolDescriptorsType::iterator toolDescriptorIt = this->NdiToolDescriptors.begin(); toolDescriptorIt != this->NdiToolDescriptors.end(); ++toolDescriptorIt)
  {
    delete [] toolDescriptorIt->second.VirtualSROM;
    toolDescriptorIt->second.VirtualSROM = NULL;
  }
  this->NdiToolDescriptors.clear();

  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);

  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(unsigned long, SerialPort, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(unsigned long, BaudRate, deviceConfig);
  if (deviceConfig->GetAttribute("BaudRate") != NULL && vtkPlusNDITracker::ConvertBaudToNDIEnum(this->BaudRate) == -1)
  {
    LOG_WARNING("Invalid baud rate specified, reverting to auto-select.");
    this->BaudRate = 0;
  }
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, MeasurementVolumeNumber, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, TrackingFrequencyNumber, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, HardwareDataAveragingDepth, deviceConfig);

  XML_READ_STRING_ATTRIBUTE_OPTIONAL(NetworkHostname, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, NetworkPort, deviceConfig);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(CheckDSR, deviceConfig);

  this->RawStartupError.clear();
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(EnableMeasuredMarkers, deviceConfig);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(EnableRawEndpoint, deviceConfig);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(RawRecoverInitial, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(RawSourceId, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(RawProtocol, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, RawSchemaMinor, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, RawPort, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, RawQueueCapacity, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, RawQueueBytes, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, RawWriteTimeoutMs, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, RawDrainTimeoutMs, deviceConfig);
  if (this->EnableRawEndpoint && !this->EnableMeasuredMarkers)
  {
    this->RawStartupError="Raw endpoint requires the same-BX measured-marker sidecar path";
    LOG_ERROR(this->RawStartupError << "; raw disabled.");
    this->EnableRawEndpoint=false;
  }
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(MarkerSidecarDirectory, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, MarkerQueueCapacity, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, MarkerReplyTimeoutMs, deviceConfig);
  if (this->EnableMeasuredMarkers)
  {
#if !defined(__linux__)
    LOG_ERROR("Measured marker capture currently supports Linux serial only.");
    return PLUS_FAIL;
#endif
    struct stat directoryStatus;
    if (!this->NetworkHostname.empty() || this->MarkerSidecarDirectory.empty() ||
        this->MarkerSidecarDirectory[0] != '/' ||
        stat(this->MarkerSidecarDirectory.c_str(), &directoryStatus) != 0 ||
        !S_ISDIR(directoryStatus.st_mode) || this->MarkerQueueCapacity < 1 ||
        this->MarkerQueueCapacity > 1024 || this->MarkerReplyTimeoutMs < 50 ||
        this->MarkerReplyTimeoutMs > 60000)
    {
      LOG_ERROR("Measured markers require serial, an existing absolute sidecar directory, queue 1..1024, timeout 50..60000 ms.");
      return PLUS_FAIL;
    }
  }

  XML_FIND_NESTED_ELEMENT_REQUIRED(dataSourcesElement, deviceConfig, "DataSources");

  for (int nestedElementIndex = 0; nestedElementIndex < dataSourcesElement->GetNumberOfNestedElements(); nestedElementIndex++)
  {
    vtkXMLDataElement* toolDataElement = dataSourcesElement->GetNestedElement(nestedElementIndex);
    if (STRCASECMP(toolDataElement->GetName(), "DataSource") != 0)
    {
      // if this is not a data source element, skip it
      continue;
    }
    bool isEqual(false);
    if (igsioCommon::XML::SafeCheckAttributeValueInsensitive(*toolDataElement, "Type", vtkPlusDataSource::DATA_SOURCE_TYPE_TOOL_TAG, isEqual) != PLUS_SUCCESS || !isEqual)
    {
      // if this is not a Tool element, skip it
      continue;
    }
    const char* toolId = toolDataElement->GetAttribute("Id");
    if (toolId == NULL)
    {
      LOG_ERROR("Failed to initialize NDI tool: DataSource Id is missing");
      continue;
    }
    igsioTransformName toolTransformName(toolId, this->GetToolReferenceFrameName());
    std::string toolSourceId = toolTransformName.GetTransformName();
    vtkPlusDataSource* trackerTool = NULL;
    if (this->GetTool(toolSourceId, trackerTool) != PLUS_SUCCESS || trackerTool == NULL)
    {
      LOG_ERROR("Failed to get NDI tool: " << toolSourceId);
      continue;
    }

    int wiredPortNumber = -1;
    if (toolDataElement->GetAttribute("PortName") != NULL)
    {
      if (!toolDataElement->GetScalarAttribute("PortName", wiredPortNumber))
      {
        LOG_WARNING("NDI wired tool's PortName attribute has to be an integer >=0");
        continue;
      }
    }

    NdiToolDescriptor toolDescriptor;
    toolDescriptor.PortEnabled = false;
    toolDescriptor.PortHandle = 0;
    toolDescriptor.MarkerBodyFrame = toolId;
    toolDescriptor.VirtualSROM = NULL;
    toolDescriptor.WiredPortNumber = wiredPortNumber;

    const char* romFileName = toolDataElement->GetAttribute("RomFile");
    if (romFileName)
    {
      // Passive (wireless) tool or wired tool with virtual ROM
      if (wiredPortNumber >= 0)
      {
        LOG_WARNING("NDI PortName and RomFile are both specified for tool " << toolSourceId << ". Assuming broken wired rom, using virtual rom instead");
      }
      std::string romFilePath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(romFileName);
      if (this->EnableMeasuredMarkers)
      {
        toolDescriptor.MarkerRomPath = romFilePath;
        if (!PlusNDIMarkerRecorder::IdentifyRom(romFilePath, toolDescriptor.MarkerRomMD5,
            toolDescriptor.MarkerRomBytes))
        {
          LOG_ERROR("Cannot identify original ROM for measured marker capture: " << romFilePath);
          return PLUS_FAIL;
        }
      }
      if (this->EnableRawEndpoint)
      {
        std::ifstream stream(romFilePath, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (stream.is_open() && !stream.bad() && bytes.size()==toolDescriptor.MarkerRomBytes)toolDescriptor.MarkerRomSHA256=PlusNDIRawWire::Sha256(bytes.data(),bytes.size());
      }
      this->ReadSromFromFile(toolDescriptor, romFilePath.c_str());
    }

    this->NdiToolDescriptors[toolSourceId] = toolDescriptor;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::WriteConfiguration(vtkXMLDataElement* rootConfig)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_WRITING(trackerConfig, rootConfig);
  if (this->SerialPort > 0)
  {
    trackerConfig->SetIntAttribute("SerialPort", this->SerialPort);
  }

  if (this->EnableMeasuredMarkers)
  {
    trackerConfig->SetAttribute("EnableRawEndpoint", this->EnableRawEndpoint?"true":"false");
    if(this->EnableRawEndpoint){
      trackerConfig->SetAttribute("RawSourceId", this->RawSourceId.c_str());
      trackerConfig->SetAttribute("RawProtocol", this->RawProtocol.c_str());
      trackerConfig->SetIntAttribute("RawSchemaMinor", this->RawSchemaMinor);
      trackerConfig->SetAttribute("RawRecoverInitial",this->RawRecoverInitial?"true":"false");
      trackerConfig->SetIntAttribute("RawPort", this->RawPort);
      trackerConfig->SetIntAttribute("RawQueueCapacity", this->RawQueueCapacity);
      trackerConfig->SetIntAttribute("RawQueueBytes", this->RawQueueBytes);
      trackerConfig->SetIntAttribute("RawWriteTimeoutMs", this->RawWriteTimeoutMs);
      trackerConfig->SetIntAttribute("RawDrainTimeoutMs", this->RawDrainTimeoutMs);
    }
    trackerConfig->SetAttribute("EnableMeasuredMarkers", "true");
    trackerConfig->SetAttribute("MarkerSidecarDirectory", this->MarkerSidecarDirectory.c_str());
    trackerConfig->SetIntAttribute("MarkerQueueCapacity", this->MarkerQueueCapacity);
    trackerConfig->SetIntAttribute("MarkerReplyTimeoutMs", this->MarkerReplyTimeoutMs);
  }
  XML_WRITE_STRING_ATTRIBUTE_IF_NOT_EMPTY(NetworkHostname, trackerConfig);
  if (!this->NetworkHostname.empty())
  {
    trackerConfig->SetIntAttribute("NetworkPort", this->NetworkPort);
  }

  if (this->HardwareDataAveragingDepth != 1)
  {
    trackerConfig->SetIntAttribute("HardwareDataAveragingDepth", this->HardwareDataAveragingDepth);
  }

  if (this->BaudRate > 0)
  {
    trackerConfig->SetIntAttribute("BaudRate", this->BaudRate);
  }
  if (this->MeasurementVolumeNumber > 0)
  {
    trackerConfig->SetIntAttribute("MeasurementVolumeNumber", this->MeasurementVolumeNumber);
  }
  if (this->TrackingFrequencyNumber > 0)
  {
    trackerConfig->SetIntAttribute("TrackingFrequencyNumber", this->TrackingFrequencyNumber);
  }
  trackerConfig->SetAttribute("CheckDSR", this->CheckDSR ? "true" : "false");

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkPlusNDITracker::LogVolumeList(int selectedVolume, vtkPlusLogger::LogLevelType logLevel)
{
  auto reply = this->Command("GETINFO:Param.Tracking.Available Volumes");

  unsigned int numVolumes(0);
  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    const unsigned char MODE_GET_VOLUMES_LIST = 0x03; // list of volumes available
    auto volumeListCommandReply = this->Command("SFLIST:%02X", MODE_GET_VOLUMES_LIST);
    numVolumes = ndiHexToUnsignedInt(volumeListCommandReply.c_str(), 1);
  }
  else
  {
    auto tokens = igsioCommon::SplitStringIntoTokens(reply, '=', false);
    auto dataFields = igsioCommon::SplitStringIntoTokens(tokens[1], ';', true);
    numVolumes = dataFields.size() / 7;
  }

  if (selectedVolume == 0)
  {
    LOG_DYNAMIC("Number of available measurement volumes: " << numVolumes, logLevel);
  }

  reply = this->Command("GETINFO:Features.Volumes.*");
  if (reply.find("ERROR") != std::string::npos)
  {
    // Most likely error 34 "Parameter not found", revert to SFLIST method
    LogVolumeListSFLIST(numVolumes, selectedVolume, logLevel);
    return;
  }

  auto lines = igsioCommon::SplitStringIntoTokens(reply, '\n', false);

  for (auto iter = begin(lines); iter != end(lines); ++iter)
  {
    auto tokens = igsioCommon::SplitStringIntoTokens(*iter, '=', true);
    auto dataFields = igsioCommon::SplitStringIntoTokens(tokens[1], ';', true);
    if (selectedVolume > 0 &&
        tokens[0].find("Features.Volumes.Index") != std::string::npos &&
        dataFields[0][0] - '0' == selectedVolume - 1) // NDI index by 0, Plus index by 1
    {
      LOG_DYNAMIC("Measurement volume " << selectedVolume, logLevel);

      // Selected index, next 13 lines are details about the selected volume
      // Features.Volumes.Name        The volume name                                   Read
      // Features.Volumes.Shape       The shape type                                    Read
      // Features.Volumes.Wavelengths Which wavelengths are supported in the volume     Read
      // Features.Volumes.Paramn      Shape parameters as described in SFLIST           Read
      for (int i = 0; i < 13; ++i)
      {
        auto tokens = igsioCommon::SplitStringIntoTokens(*(iter + i), '=', true);
        auto names = igsioCommon::SplitStringIntoTokens(tokens[0], '.', false);
        auto dataFields = igsioCommon::SplitStringIntoTokens(tokens[1], ';', true);
        LOG_DYNAMIC(" " << names[2] << ": " << dataFields[0], logLevel);
      }

      return;
    }

    auto names = igsioCommon::SplitStringIntoTokens(tokens[0], '.', false);
    LOG_DYNAMIC(names[2] << ": " << dataFields[0], logLevel);
  }
}

//----------------------------------------------------------------------------
void vtkPlusNDITracker::LogVolumeListSFLIST(unsigned int numVolumes, int selectedVolume, vtkPlusLogger::LogLevelType logLevel)
{
  std::string volumeListCommandReply = this->Command("SFLIST:03");
  for (unsigned int volIndex = 0; volIndex < numVolumes; ++volIndex)
  {
    const char* volDescriptor = volumeListCommandReply.c_str() + 1 + volIndex * 74;

    LOG_DYNAMIC("Measurement volume " << volIndex + 1, logLevel);

    std::string shapeType;
    bool isOptical(false);
    switch (volDescriptor[0])
    {
      case '9':
        shapeType = "Cube";
        break;
      case 'A':
        shapeType = "Dome";
        break;
      case '5':
        shapeType = "Polaris (Pyramid or extended pyramid)";
        isOptical = true;
        break;
      case '7':
        shapeType = "Vicra (Pyramid)";
        isOptical = true;
        break;
      default:
        shapeType = "unknown";
    }
    LOG_DYNAMIC(" Shape type: " << shapeType << " (" << volDescriptor[0] << ")", logLevel);

    LOG_DYNAMIC(" D1  = " << ndiSignedToLong(volDescriptor + 1, 7) / 100, logLevel);
    LOG_DYNAMIC(" D2  = " << ndiSignedToLong(volDescriptor + 8, 7) / 100, logLevel);
    LOG_DYNAMIC(" D3  = " << ndiSignedToLong(volDescriptor + 15, 7) / 100, logLevel);
    LOG_DYNAMIC(" D4  = " << ndiSignedToLong(volDescriptor + 22, 7) / 100, logLevel);
    LOG_DYNAMIC(" D5  = " << ndiSignedToLong(volDescriptor + 29, 7) / 100, logLevel);
    LOG_DYNAMIC(" D6  = " << ndiSignedToLong(volDescriptor + 36, 7) / 100, logLevel);
    LOG_DYNAMIC(" D7  = " << ndiSignedToLong(volDescriptor + 43, 7) / 100, logLevel);
    LOG_DYNAMIC(" D8  = " << ndiSignedToLong(volDescriptor + 50, 7) / 100, logLevel);
    LOG_DYNAMIC(" D9  = " << ndiSignedToLong(volDescriptor + 57, 7) / 100, logLevel);
    LOG_DYNAMIC(" D10 = " << ndiSignedToLong(volDescriptor + 64, 7) / 100, logLevel);

    if (!isOptical)
    {
      LOG_DYNAMIC(" Reserved: " << volDescriptor[71], logLevel);

      std::string metalResistant;
      switch (volDescriptor[72])
      {
        case '0':
          metalResistant = "no information";
          break;
        case '1':
          metalResistant = "metal resistant";
          break;
        case '2':
          metalResistant = "not metal resistant";
          break;
        default:
          metalResistant = "unknown";
      }
      LOG_DYNAMIC(" Metal resistant: " << metalResistant << " (" << volDescriptor[72] << ")", logLevel);
    }
    else
    {
      unsigned int numWavelengths = volDescriptor[71] - '0';
      LOG_DYNAMIC(" Number of wavelengths supported: " << numWavelengths, logLevel);
      for (unsigned int i = 0; i < numWavelengths; ++i)
      {
        switch (volDescriptor[72 + i])
        {
          case '0':
            LOG_DYNAMIC("  Wavelength " << i << ": 930nm", logLevel);
            break;
          case '1':
            LOG_DYNAMIC("  Wavelength " << i << ": 880nm", logLevel);
            break;
          case '4':
            LOG_DYNAMIC("  Wavelength " << i << ": 870nm", logLevel);
            break;
          default:
            LOG_DYNAMIC("  Wavelength " << i << ": unknown (" << volDescriptor[72 + i] << ")", logLevel);
            break;
        }
      }
    }
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::CloseDevice(ndicapi*& device)
{
  if (this->NetworkHostname.empty())
  {
    ndiCloseSerial(device);
  }
  else
  {
    ndiCloseNetwork(device);
  }
  device = nullptr;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::SelectMeasurementVolume()
{
  if (this->MeasurementVolumeNumber > 0)
  {
    std::string reply = this->Command("GETINFO:Param.Tracking.Available Volumes");
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      return SelectMeasurementVolumeDeprecated();
    }
    else
    {
      auto tokens = igsioCommon::SplitStringIntoTokens(reply, '=', false);
      auto dataFields = igsioCommon::SplitStringIntoTokens(tokens[1], ';', true);

      // <Value>;<Type>;<Attribute>;<Minimum>;<Maximum>;<Enumeration>;<Description>
      if (static_cast<unsigned int>(this->MeasurementVolumeNumber) - 1 > dataFields.size() / 7)
      {
        LOG_ERROR("Selected measurement volume does not exist. Using default.");
        LogVolumeList(this->MeasurementVolumeNumber, vtkPlusLogger::LOG_LEVEL_DEBUG);
        return PLUS_FAIL;
      }
      else
      {
        // Use SET command with parameter
        std::string volumeSelectCommandReply = this->Command("SET:Param.Tracking.Selected Volume=%d", this->MeasurementVolumeNumber);
        int errnum = ndiGetError(this->Device);
        if (errnum)
        {
          LOG_ERROR("Failed to set measurement volume " << this->MeasurementVolumeNumber << ": " << ndiErrorString(errnum));
          LogVolumeList(0, vtkPlusLogger::LOG_LEVEL_INFO);
          CloseDevice(this->Device);
          return PLUS_FAIL;
        }
      }
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::SelectMeasurementVolumeDeprecated()
{
  auto volumeSelectCommandReply = this->Command("VSEL:%d", this->MeasurementVolumeNumber);
  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    LOG_ERROR("Failed to set measurement volume " << this->MeasurementVolumeNumber << ": " << ndiErrorString(errnum));
    LogVolumeList(this->MeasurementVolumeNumber, vtkPlusLogger::LOG_LEVEL_DEBUG);
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::SelectTrackingFrequency()
{
  /*
  Frame Frequency (or Frame Rate)
  
    The frequency in [Hz] at which
      the image sensors are read
      out.

    Param.Tracking.Frame Frequency
      Vega ST/VT: fixed at 60 Hz
      Vega XT: configurable 60-400 Hz
      Lyra: configurable 10-120 Hz (10-250 Hz with the MR125 option)

   Track Frequency 
    The frequency at which unaveraged tracking data is reported. The actual track frequency is influenced by:
      • The parameter setting
      • The number of different tool types tracked (active wired/active wireless/passive)

      For Vega, when tracking a single tool type, the Track Frequency is equal to the
        Frame Frequency. If more than one tool type is tracked, the Track Frequency is a fraction
        (1/2 or 1/3) of that, even if the parameter is set to 1/1 of Frame Frequency.

      For Lyra, when tracking one or two tool types, the Track Frequency is 1/2. When
        tracking three tool types, the Track Frequency is 1/3.
      
    Param.Tracking.Track Frequency
      This parameter is an enumeration whose values are derived from the frame frequency.
      For Vega, this can be:
        • 1/3 of frame frequency
        • 1/2 of frame frequency
        • 1/1 of frame frequency
      For Lyra, this can be:
        • 1/3 of frame frequency
        • 1/2 of frame frequency
      
   Reporting
    Frequency The result of the actual track frequency divided by the averaging depth specified by Param.Data Averaging.Depth.
  */

  if (this->TrackingFrequencyNumber > 0)
  {
    std::string reply = this->Command("GET:Param.Tracking.Track Frequency");

    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      return SelectTrackingFrequencyDeprecated();
    }
    else
    {
      // Use SET command with parameter
      std::string frequencySelectCommandReply = this->Command("SET:Param.Tracking.Track Frequency=%d", this->TrackingFrequencyNumber);
      int errnum = ndiGetError(this->Device);
      if (errnum)
      {
        LOG_ERROR("Failed to set tracking frequency " << this->TrackingFrequencyNumber << ": " << ndiErrorString(errnum));
        return PLUS_FAIL;
      }
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::SelectTrackingFrequencyDeprecated()
{
  auto frequencySelectCommandReply = this->Command("IRATE:%d", this->TrackingFrequencyNumber);
  int errnum = ndiGetError(this->Device);
  if (errnum)
  {
    LOG_ERROR("Failed to set tracking frequency " << this->TrackingFrequencyNumber << ": " << ndiErrorString(errnum));
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::SelectDataAveraging()
{
  if (this->GetFirmwareMajorRevision() >= 3 && this->GetFirmwareMinorRevision() >= 6)
  {
    std::string model = this->Command("GET:%s", "Features.Hardware.Model");

    this->Command("SET:%s=%d", "Param.Data Averaging.Depth", this->HardwareDataAveragingDepth);
    int errnum = ndiGetError(this->Device);
    if (errnum)
    {
      LOG_ERROR("Unable to set data averaging depth: " << ndiErrorString(errnum));
      return PLUS_FAIL;
    }

    if (this->HardwareDataAveragingDepth > 1)
    {
      // Calculate the new timeout
      std::string result = this->Command("GET:%s", "Param.Tracking.Frame Frequency");
      std::vector<std::string> tokens = igsioCommon::SplitStringIntoTokens(result, '=');
      unsigned int frameFreq;
      igsioCommon::StringToNumber<unsigned int>(tokens[1], frameFreq);

      result = this->Command("GET:%s", "Param.Tracking.Track Frequency");
      tokens = igsioCommon::SplitStringIntoTokens(result, '=');
      unsigned int trackFreqEnum;
      igsioCommon::StringToNumber<unsigned int>(tokens[1], trackFreqEnum);

      /*
          Param.Tracking.Track Frequency
      This parameter is an enumeration whose values are derived from the frame frequency.
      For Vega, this can be:
        • 1/3 of frame frequency
        • 1/2 of frame frequency
        • 1/1 of frame frequency
      For Lyra, this can be:
        • 1/3 of frame frequency
        • 1/2 of frame frequency
      */
      double trackFreq(0);
      if (model.find("Vega") != std::string::npos)
      {
        switch (trackFreqEnum)
        {
        case 0:
          trackFreq = frameFreq / 3;
          break;
        case 1:
          trackFreq = frameFreq / 2;
          break;
        case 2:
          trackFreq = frameFreq;
          break;
        default:
          break;
        }
      }
      else if (model.find("Lyra") != std::string::npos)
      {
        switch (trackFreqEnum)
        {
        case 0:
          trackFreq = frameFreq / 3;
          break;
        case 1:
          trackFreq = frameFreq / 2;
          break;
        default:
          break;
        }
      }

      if (trackFreq != 0)
      {
        double timeoutMs = 1. / trackFreq * 1000 * this->HardwareDataAveragingDepth;
        ndiTimeoutSocket(this->Device, static_cast<unsigned int>(timeoutMs) + 25); // Add 25 ms buffer for kicks
      }
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
int vtkPlusNDITracker::ConvertBaudToNDIEnum(int baudRate)
{
  switch (baudRate)
  {
    case 9600:
      return NDI_9600;
    case 14400:
      return NDI_14400;
    case 19200:
      return NDI_19200;
    case 38400:
      return NDI_38400;
    case 57600:
      return NDI_57600;
    case 115200:
      return NDI_115200;
    case 230400:
      return NDI_230400;
    case 921600:
      return NDI_921600;
    case 1228739:
      return NDI_1228739;
    default:
      return -1;
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::DoBXUpdate()
{
  int errnum = 0;
  // get the transforms for all tools from the NDI
  std::vector<PlusNDIMarkerBinding> markerBindings;
  if (this->EnableMeasuredMarkers)
  {
    for (const auto& entry : this->NdiToolDescriptors)
    {
      PlusNDIMarkerBinding b;
      b.Handle = entry.second.PortHandle;
      b.TransformName = entry.first;
      b.BodyFrame = entry.second.MarkerBodyFrame;
      b.RomPath = entry.second.MarkerRomPath;
      b.RomMD5 = entry.second.MarkerRomMD5;
      b.RomBytes = entry.second.MarkerRomBytes;
      b.RomSHA256 = entry.second.MarkerRomSHA256;
      markerBindings.push_back(b);
    }
    if (this->EnableRawEndpoint && this->RawModel && this->RawSender && this->RawStartupError.empty())
    {
      try {
        if(this->RawRecoverInitial && this->RawModel->InitialRecoveryPending() && this->RawSender->PrepareInitialRecovery()==1){
          PlusNDIRawRecoveryEvidence evidence{};evidence.EndSentNs=this->RawSender->RecoveryEndSentNs();
          const bool recovered=this->RecoverRawSerial(evidence)==PLUS_SUCCESS;
          const bool recorded=this->RawSender->RecordRecoveryEvidence(evidence);
          // INIT may assign different handles. Rebuild from the restored descriptors.
          markerBindings.clear();
          for(const auto& entry:this->NdiToolDescriptors){
            PlusNDIMarkerBinding b;b.Handle=entry.second.PortHandle;b.TransformName=entry.first;
            b.BodyFrame=entry.second.MarkerBodyFrame;b.RomPath=entry.second.MarkerRomPath;
            b.RomMD5=entry.second.MarkerRomMD5;b.RomBytes=entry.second.MarkerRomBytes;
            b.RomSHA256=entry.second.MarkerRomSHA256;markerBindings.push_back(b);
          }
          if(!recovered||!recorded||!this->RawSender->CommitInitialRecovery(*this->RawModel,markerBindings,evidence)){
            this->RawModel->MarkFailed();this->MarkerTransportDesynchronized=true;this->RawStartupError="Verified serial recovery failed; raw remains quarantined";return PLUS_FAIL;
          }
          this->RawReplySequence=0;
        }
        ++this->RawReplySequence;
        this->RawModel->PrepareBindings(markerBindings,this->MarkerConnectionEpoch);
        this->RawSender->BeginAttempt(this->RawReplySequence,markerBindings,*this->RawModel);
      }catch(...) { this->RawSnapshotFailed=true;this->RawModel->MarkFailed(); }
    }
    ++this->MarkerPollSequence;
    {
      igsioLockGuard<vtkIGSIORecursiveCriticalSection> lock(this->CommandMutex);
      errnum = ndiCaptureBX(this->Device, NDI_BX_CAPTURE_FLAGS, this->MarkerReplyTimeoutMs, this->MarkerReply);
    }
    if (this->MarkerReply->transport_state == NDI_BX_RECONNECT_REQUIRED)
    {
      this->MarkerTransportDesynchronized = true;
      LOG_ERROR("BX stream is desynchronized; subsequent marker requests are inhibited until reconnect.");
    }
    if (errnum == 1 || errnum == 7)
    {
      this->MarkerRequestUnsupported = true;
      LOG_ERROR("Device rejected BX marker capture; no fallback or derived coordinates will be used.");
    }
  }
  else
  {
    this->Command("BX:0801");
    errnum = ndiGetError(this->Device);
  }
  if (errnum)
  {
    if (this->EnableMeasuredMarkers) { this->RecordMeasuredMarkers(markerBindings); }
    if (errnum == NDI_BX_CAPTURE_RECOVERY_REPLY)
    {
      LOG_INFO("First BX reply quarantined; later request association assumes serialized protocol order.");
    }
    else if (errnum == NDI_BAD_CRC || errnum == NDI_TIMEOUT)   // common errors
    {
      LOG_WARNING(ndiErrorString(errnum));
    }
    else
    {
      LOG_ERROR(ndiErrorString(errnum));
    }
    return PLUS_FAIL;
  }

  // default to incrementing frame count by one (in case a frame index cannot be retrieved from the tracker for a specific tool)
  this->LastFrameNumber++;
  int defaultToolFrameNumber = this->LastFrameNumber;
  const double toolTimestamp = vtkIGSIOAccurateTimer::GetSystemTime(); // unfiltered timestamp
  vtkSmartPointer<vtkMatrix4x4> toolToTrackerTransform = vtkSmartPointer<vtkMatrix4x4>::New();
  for (DataSourceContainerConstIterator it = this->GetToolIteratorBegin(); it != this->GetToolIteratorEnd(); ++it)
  {
    ToolStatus toolFlags = TOOL_OK;
    toolToTrackerTransform->Identity();
    unsigned long toolFrameNumber = defaultToolFrameNumber;
    vtkPlusDataSource* trackerTool = it->second;
    std::string toolSourceId = trackerTool->GetId();
    NdiToolDescriptorsType::iterator ndiToolDescriptorIt = this->NdiToolDescriptors.find(toolSourceId);
    if (ndiToolDescriptorIt == this->NdiToolDescriptors.end())
    {
      LOG_ERROR("Tool descriptor is not found for tool " << toolSourceId);
      if (this->EnableMeasuredMarkers) { toolFlags = TOOL_MISSING; }
      this->ToolTimeStampedUpdate(trackerTool->GetId(), toolToTrackerTransform, toolFlags, toolFrameNumber, toolTimestamp);
      continue;
    }
    int portHandle = ndiToolDescriptorIt->second.PortHandle;
    if (portHandle <= 0)
    {
      LOG_ERROR("Port handle is invalid for tool " << toolSourceId);
      if (this->EnableMeasuredMarkers) { toolFlags = TOOL_MISSING; }
      this->ToolTimeStampedUpdate(toolSourceId.c_str(), toolToTrackerTransform, toolFlags, toolFrameNumber, toolTimestamp);
      continue;
    }

    float ndiTransform[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
    int ndiToolAbsent = 1;
    int ndiPortStatus = 0;
    unsigned long ndiFrameIndex = 0;
    if (this->EnableMeasuredMarkers)
    {
      // Opt-in path uses only this reply's snapshot, never invalid legacy caches.
      for (unsigned i = 0; i < this->MarkerReply->tool_count; ++i)
      {
        const auto& tool = this->MarkerReply->tools[i];
        if (tool.handle != static_cast<unsigned>(portHandle)) { continue; }
        if (tool.frame_available) { ndiPortStatus = tool.port_status; ndiFrameIndex = tool.frame_number; }
        bool finite=true;for(float f:tool.pose)finite=finite&&std::isfinite(f);
        if (tool.pose_present && finite) { std::copy(tool.pose, tool.pose + 8, ndiTransform); ndiToolAbsent = 0; }
        break;
      }
    }
    else
    {
      ndiToolAbsent = ndiGetBXTransform(this->Device, portHandle, ndiTransform);
      ndiPortStatus = ndiGetBXPortStatus(this->Device, portHandle);
      ndiFrameIndex = ndiGetBXFrame(this->Device, portHandle);
    }

    // convert status flags from NDI to Plus format
    const unsigned long ndiPortStatusValidFlags = NDI_TOOL_IN_PORT | NDI_INITIALIZED | NDI_ENABLED;
    if ((ndiPortStatus & ndiPortStatusValidFlags) != ndiPortStatusValidFlags)
    {
      toolFlags = TOOL_MISSING;
    }
    else
    {
      if (ndiToolAbsent)
      {
        toolFlags = TOOL_OUT_OF_VIEW;
      }
      if (ndiPortStatus & NDI_OUT_OF_VOLUME)
      {
        toolFlags = TOOL_OUT_OF_VOLUME;
      }
      // TODO all these button state toolFlags are on regardless of the actual state
      //if (ndiPortStatus & NDI_SWITCH_1_ON)  { toolFlags = TOOL_SWITCH1_IS_ON; }
      //if (ndiPortStatus & NDI_SWITCH_2_ON)  { toolFlags = TOOL_SWITCH2_IS_ON; }
      //if (ndiPortStatus & NDI_SWITCH_3_ON)  { toolFlags = TOOL_SWITCH3_IS_ON; }
    }

    ndiTransformToMatrixfd(ndiTransform, *toolToTrackerTransform->Element);
    toolToTrackerTransform->Transpose();

    // by default (if there is no camera frame number associated with
    // the tool transformation) the most recent timestamp is used.
    if (!ndiToolAbsent && ndiFrameIndex)
    {
      // this will create a timestamp from the frame number
      toolFrameNumber = ndiFrameIndex;
      if (ndiFrameIndex > this->LastFrameNumber)
      {
        this->LastFrameNumber = ndiFrameIndex;
      }
    }

    // send the matrix and status to the tool's vtkPlusDataBuffer
    PlusStatus publication = this->ToolTimeStampedUpdate(toolSourceId.c_str(), toolToTrackerTransform, toolFlags, toolFrameNumber, toolTimestamp);
    if (this->EnableMeasuredMarkers)
    {
      for (auto& b : markerBindings)
      {
        if (b.TransformName != toolSourceId) { continue; }
        b.Published = publication == PLUS_SUCCESS;
        b.PlusStatus = igsioCommon::ConvertToolStatusToString(toolFlags);
        b.PlusFrame = toolFrameNumber;
        b.PlusFrameSource = (!ndiToolAbsent && ndiFrameIndex) ? "NDI_BX_device_frame" : "Plus_host_counter_fallback";
        b.PlusHostTime = toolTimestamp;
        for (int row = 0; row < 4; ++row)
          for (int col = 0; col < 4; ++col)
            b.Matrix[row * 4 + col] = toolToTrackerTransform->GetElement(row, col);
      }
    }
  }

  if (this->EnableMeasuredMarkers) { this->RecordMeasuredMarkers(markerBindings); }

  // Update tool connections if a wired tool is plugged in
  if (ndiGetBXSystemStatus(this->Device) & NDI_PORT_OCCUPIED)
  {
    LOG_WARNING("A wired tool has been plugged into tracker " << (this->GetDeviceId().empty() ? this->GetDeviceId() : "(unknown NDI tracker"));
    // Make the newly connected tools available
    this->EnableToolPorts();
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::DoBX2Update()
{
  int errnum = 0;
  // get the transforms for all tools from the NDI
  this->Command("BX2:--6d=tools --1d=none");
  errnum = ndiGetError(this->Device);
  if (errnum)
  {
    if (errnum == NDI_BX_CAPTURE_RECOVERY_REPLY)
    {
      LOG_INFO("First BX reply quarantined; later request association assumes serialized protocol order.");
    }
    else if (errnum == NDI_BAD_CRC || errnum == NDI_TIMEOUT)   // common errors
    {
      LOG_WARNING(ndiErrorString(errnum));
    }
    else
    {
      LOG_ERROR(ndiErrorString(errnum));
    }
    return PLUS_FAIL;
  }

  // default to incrementing frame count by one (in case a frame index cannot be retrieved from the tracker for a specific tool)
  this->LastFrameNumber++;
  int defaultToolFrameNumber = this->LastFrameNumber;
  const double toolTimestamp = vtkIGSIOAccurateTimer::GetSystemTime(); // unfiltered timestamp
  vtkSmartPointer<vtkMatrix4x4> toolToTrackerTransform = vtkSmartPointer<vtkMatrix4x4>::New();
  unsigned long ndiFrameIndex = ndiGetBX2Frame(this->Device);
  for (DataSourceContainerConstIterator it = this->GetToolIteratorBegin(); it != this->GetToolIteratorEnd(); ++it)
  {
    ToolStatus toolFlags = TOOL_OK;
    toolToTrackerTransform->Identity();
    unsigned long toolFrameNumber = defaultToolFrameNumber;
    vtkPlusDataSource* trackerTool = it->second;
    std::string toolSourceId = trackerTool->GetId();
    NdiToolDescriptorsType::iterator ndiToolDescriptorIt = this->NdiToolDescriptors.find(toolSourceId);
    if (ndiToolDescriptorIt == this->NdiToolDescriptors.end())
    {
      LOG_ERROR("Tool descriptor is not found for tool " << toolSourceId);
      if (this->EnableMeasuredMarkers) { toolFlags = TOOL_MISSING; }
      this->ToolTimeStampedUpdate(trackerTool->GetId(), toolToTrackerTransform, toolFlags, toolFrameNumber, toolTimestamp);
      continue;
    }
    int portHandle = ndiToolDescriptorIt->second.PortHandle;
    if (portHandle <= 0)
    {
      LOG_ERROR("Port handle is invalid for tool " << toolSourceId);
      if (this->EnableMeasuredMarkers) { toolFlags = TOOL_MISSING; }
      this->ToolTimeStampedUpdate(toolSourceId.c_str(), toolToTrackerTransform, toolFlags, toolFrameNumber, toolTimestamp);
      continue;
    }

    float ndiTransform[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
    int ndiToolAbsent = ndiGetBX2Transform(this->Device, portHandle, ndiTransform);
    int ndiPortStatus = ndiGetBX2PortStatus(this->Device, portHandle);

    // convert status flags from NDI to Plus format
    if (ndiPortStatus & NDI_BX2_MISSING_BIT)
    {
      toolFlags = TOOL_MISSING;
    }
    else
    {
      if (ndiPortStatus == NDI_BX2_PARTIAL_VIEW)
      {
        toolFlags = TOOL_OUT_OF_VIEW;
      }
      if (ndiPortStatus == NDI_BX2_OUT_OF_VOLUME)
      {
        toolFlags = TOOL_OUT_OF_VOLUME;
      }
      // TODO all these button state toolFlags are on regardless of the actual state
      //if (ndiPortStatus & NDI_SWITCH_1_ON)  { toolFlags = TOOL_SWITCH1_IS_ON; }
      //if (ndiPortStatus & NDI_SWITCH_2_ON)  { toolFlags = TOOL_SWITCH2_IS_ON; }
      //if (ndiPortStatus & NDI_SWITCH_3_ON)  { toolFlags = TOOL_SWITCH3_IS_ON; }
    }

    ndiTransformToMatrixfd(ndiTransform, *toolToTrackerTransform->Element);
    toolToTrackerTransform->Transpose();

    // by default (if there is no camera frame number associated with
    // the tool transformation) the most recent timestamp is used.
    if (ndiToolAbsent == NDI_BX2_ENABLED && ndiFrameIndex)
    {
      // this will create a timestamp from the frame number
      toolFrameNumber = ndiFrameIndex;
      if (ndiFrameIndex > this->LastFrameNumber)
      {
        this->LastFrameNumber = ndiFrameIndex;
      }
    }

    // send the matrix and status to the tool's vtkPlusDataBuffer
    this->ToolTimeStampedUpdate(toolSourceId.c_str(), toolToTrackerTransform, toolFlags, toolFrameNumber, toolTimestamp);
  }

  int alerts = ndiGetBX2SystemAlertsCount(this->Device);
  for (int i = 0; i < alerts; ++i)
  {
    unsigned short* alert = ndiGetBX2SystemAlert(this->Device, i);
    if (alert[1] == NDI_SYS_EVENT_TOOL_CONNECTED)
    {
      LOG_WARNING("A wired tool has been plugged into tracker " << (this->GetDeviceId().empty() ? this->GetDeviceId() : "(unknown NDI tracker"));
      // Make the newly connected tools available
      this->EnableToolPorts();
    }
  }

  return PLUS_SUCCESS;
}

#if defined(HAVE_FUTURE)
//----------------------------------------------------------------------------
PlusStatus vtkPlusNDITracker::ProbeSerialInternal()
{
  const int MAX_SERIAL_PORT_NUMBER = 20; // the serial port is almost surely less than this number
  std::vector<bool> deviceExists(MAX_SERIAL_PORT_NUMBER);
  std::fill(begin(deviceExists), end(deviceExists), false);
  std::vector<std::future<void>> tasks;
  for (int i = 0; i < MAX_SERIAL_PORT_NUMBER; i++)
  {
    const char* dev = ndiSerialDeviceName(i);
    if (dev != nullptr)
    {
      LOG_DEBUG("Testing serial port: " << dev);
      std::string devName = std::string(dev);
      std::future<void> result = std::async([this, i, &deviceExists, devName]()
      {
        int errnum = ndiSerialProbe(devName.c_str(), this->CheckDSR);
        LOG_DEBUG("Serial port " << devName << " probe error (" << errnum << "): " << ndiErrorString(errnum));
        if (errnum == NDI_OKAY)
        {
          deviceExists[i] = true;
        }
      });
      tasks.push_back(std::move(result));
    }
  }
  for (std::vector<std::future<void>>::size_type i = 0; i < tasks.size(); i++)
  {
    tasks[i].wait();
  }
  for (int i = 0; i < MAX_SERIAL_PORT_NUMBER; i++)
  {
    // use first device found
    if (deviceExists[i] == true)
    {
      const char* devicename = ndiSerialDeviceName(i);
      this->SerialPort = i + 1;
      if (this->LeaveDeviceOpenAfterProbe)
      {
        this->Device = ndiOpenSerial(devicename);
      }
      return PLUS_SUCCESS;
    }
  }

  return PLUS_FAIL;
}
#endif


// Opt-in only; formatting and all file I/O are performed by the recorder worker.
void vtkPlusNDITracker::RecordMeasuredMarkers(const std::vector<PlusNDIMarkerBinding>& bindings)
{
  bool accepted=false;
  if (this->EnableRawEndpoint && this->RawModel && this->RawSender && this->RawStartupError.empty())
  {
    try {
      const auto snapshot=this->RawModel->Seal(*this->MarkerReply,bindings,this->MarkerConnectionEpoch,this->RawReplySequence,this->MarkerPollSequence);
      this->RawSender->Enqueue(snapshot);
      accepted=this->MarkerRecorder->Enqueue(snapshot);
    }catch(...) {
      this->RawSnapshotFailed=true;
      this->RawModel->MarkFailed();
      this->RawSender->Enqueue(nullptr);
      accepted=this->MarkerRecorder->Enqueue(*this->MarkerReply,bindings,this->MarkerConnectionEpoch,this->MarkerPollSequence);
    }
  }
  else accepted = this->MarkerRecorder && this->MarkerReply &&
    this->MarkerRecorder->Enqueue(*this->MarkerReply, bindings, this->MarkerConnectionEpoch, this->MarkerPollSequence);
  if ((!accepted || !this->MarkerRecorder->Healthy()) && !this->MarkerErrorReported)
  {
    LOG_ERROR("Measured marker sidecar lost records or failed; inspect its counters/summary. Tool TRANSFORM updates continue.");
    this->MarkerErrorReported = true;
  }
}

std::string vtkPlusNDITracker::GetRawEndpointState() const
{ return !this->RawStartupError.empty()?"startup_failed":this->RawSender?this->RawSender->State():"disabled"; }
std::string vtkPlusNDITracker::GetRawEndpointError() const
{ return !this->RawStartupError.empty()?this->RawStartupError:this->RawSender?this->RawSender->Error():std::string(); }

// Explicit, same-owner serial recovery. No BX polling is added. A new raw
// acquisition is committed only after the exact reset token and all setup ACKs.
PlusStatus vtkPlusNDITracker::RecoverRawSerial(PlusNDIRawRecoveryEvidence& proof)
{
  auto mono=[](){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;};
  proof.StartNs=mono();proof.OldConnection=this->MarkerConnectionEpoch;
  auto finish=[&](const char* error){proof.DoneNs=mono();std::snprintf(proof.Error,sizeof(proof.Error),"%s",error);return PLUS_FAIL;};
  igsioLockGuard<vtkIGSIORecursiveCriticalSection> lock(this->CommandMutex);
  if(!this->Device)return finish("device unavailable");
  if(ndiBXResetForRecovery(this->Device,7000,&proof.Reset)!=NDI_OKAY)return finish("full reset token not verified");
  this->IsDeviceTracking=0;
  const char* reply=ndiINIT(this->Device);proof.InitOK=ndiGetError(this->Device)==NDI_OKAY&&reply&&!std::strcmp(reply,"OKAY");
  std::snprintf(proof.InitReply,sizeof(proof.InitReply),"%s",this->Device->Reply);
  if(!proof.InitOK)return finish("INIT acknowledgement failed");
  int baud=vtkPlusNDITracker::ConvertBaudToNDIEnum(this->BaudRate);
  if(baud<0)return finish("unsupported configured baud");
  reply=ndiCommand(this->Device,"COMM:%X%03d%d",baud,NDI_8N1,NDI_NOHANDSHAKE);
  proof.CommOK=ndiGetError(this->Device)==NDI_OKAY&&reply&&!std::strcmp(reply,"OKAY");
  std::snprintf(proof.CommReply,sizeof(proof.CommReply),"%s",this->Device->Reply);
  if(!proof.CommOK)return finish("COMM acknowledgement failed");
  proof.ConfigurationOK=this->GetFirmwareRevision()==PLUS_SUCCESS&&this->FirmwareMajorRevision<3&&
    this->SelectDataAveraging()==PLUS_SUCCESS&&this->SelectMeasurementVolume()==PLUS_SUCCESS&&this->SelectTrackingFrequency()==PLUS_SUCCESS;
  if(!proof.ConfigurationOK)return finish("configured device profile could not be restored");
  proof.PortsOK=this->EnableToolPorts()==PLUS_SUCCESS;
  if(!proof.PortsOK)return finish("ROM and tool enable verification failed");
  reply=ndiTSTART(this->Device);proof.StartOK=ndiGetError(this->Device)==NDI_OKAY&&reply&&!std::strcmp(reply,"OKAY");
  std::snprintf(proof.StartReply,sizeof(proof.StartReply),"%s",this->Device->Reply);
  if(!proof.StartOK)return finish("TSTART acknowledgement failed");
  ++this->MarkerConnectionEpoch;proof.NewConnection=this->MarkerConnectionEpoch;
  this->Device->BxCaptureSynchronization=1;this->IsDeviceTracking=1;
  this->MarkerRequestUnsupported=this->MarkerTransportDesynchronized=false;
  proof.Success=true;proof.DoneNs=mono();return PLUS_SUCCESS;
}
