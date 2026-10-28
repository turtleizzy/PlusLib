// Test only: deterministic, owned in-memory buffers. No serial or network access.
#include "vtkPlusChannel.h"
#include "vtkPlusDataSource.h"
#include "vtkPlusBuffer.h"
#include "vtkIGSIOTrackedFrameList.h"
#include "igsioTrackedFrame.h"
#include "vtkPlusLogger.h"
#include <vtkMatrix4x4.h>
#include <vtkSmartPointer.h>
#include <iostream>
#include <iomanip>
#include <set>
int main(int argc,char** argv){
 const std::string mode=argc>1?argv[1]:"cursor";
 vtkPlusLogger::Instance()->SetLogLevel(mode.rfind("invalid",0)==0?vtkPlusLogger::LOG_LEVEL_DEBUG:vtkPlusLogger::LOG_LEVEL_WARNING);
 auto channel=vtkSmartPointer<vtkPlusChannel>::New();
 auto a=vtkSmartPointer<vtkPlusDataSource>::New(),b=vtkSmartPointer<vtkPlusDataSource>::New();
 a->SetId("Test1ToTracker");b->SetId("Test2ToTracker");channel->AddTool(a);channel->AddTool(b);
 auto m=vtkSmartPointer<vtkMatrix4x4>::New();m->Identity();
 for(int i=0;i<4;++i){double t=10+i*.05;a->AddTimeStampedItem(m,TOOL_OK,i,t,t);b->AddTimeStampedItem(m,TOOL_OK,i,t-.000005,t-.000005);}
 if(mode=="near"){
  StreamBufferItem item;auto status=b->GetStreamBufferItemFromTime(10.1,&item,vtkPlusBuffer::INTERPOLATED);
  std::cout<<std::setprecision(17)<<"status="<<status<<" requested=10.1 returned="<<item.GetTimestamp(0)<<"\n";return 0;
 }
 if(mode=="invalid"||mode=="invalid_second"){
  for(int i=4;i<7;++i){double t=10+i*.05;b->AddTimeStampedItem(m,TOOL_OUT_OF_VIEW,i,t,t);}
  for(int i=0;i<1000;++i){StreamBufferItem item;b->GetStreamBufferItemFromTime(mode=="invalid_second"?10.17:10.3,&item,vtkPlusBuffer::INTERPOLATED);if(item.GetStatus()==TOOL_OK)return 6;}
  return 0;
 }
 double cursor=mode=="future"?20:10.049;unsigned count=0,errors=0;std::set<double> unique;
 for(int i=0;i<1000;++i){auto frames=vtkSmartPointer<vtkIGSIOTrackedFrameList>::New();
  if(channel->GetTrackedFrameList(cursor,frames,100)!=PLUS_SUCCESS)++errors;
  for(unsigned k=0;k<frames->GetNumberOfTrackedFrames();++k){auto frame=frames->GetTrackedFrame(k);++count;unique.insert(frame->GetTimestamp());}
 }
 std::cout<<std::setprecision(17)<<"frames="<<count<<" unique="<<unique.size()<<" errors="<<errors<<" cursor="<<cursor<<"\n";
 return 0;
}
