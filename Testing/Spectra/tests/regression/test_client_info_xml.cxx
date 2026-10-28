#include "PlusIgtlClientInfo.h"
#include <iostream>
#include <string>

int main()
{
  const std::string body = "<ClientInfo ClientHeaderVersion=\"2\"><MessageTypes><Message Type=\"TRANSFORM\" /></MessageTypes><TransformNames><Transform Name=\"Test1ToTracker\" /><Transform Name=\"Test2ToTracker\" /></TransformNames></ClientInfo>";
  const char* declarations[] = {"<?xml version='1.0' encoding='ascii'?>\n",
    "<?xml version=\"1.0\" encoding=\"ascii\"?>\n",
    "<?xml version='1.0' encoding='utf-8'?>\n", ""};
  int failures = 0;
  for (int i = 0; i < 4; ++i)
  {
    PlusIgtlClientInfo info;
    bool okay = info.SetClientInfoFromXmlData((declarations[i] + body).c_str()) == PLUS_SUCCESS
      && info.GetClientHeaderVersion() == 2 && info.TransformNames.size() == 2
      && info.TransformNames[0].GetTransformName() == "Test1ToTracker"
      && info.TransformNames[1].GetTransformName() == "Test2ToTracker";
    std::cout << "encoding case " << i << ": " << (okay ? "PASS" : "FAIL") << std::endl;
    failures += !okay;
  }
  PlusIgtlClientInfo invalid;
  bool rejected = invalid.SetClientInfoFromXmlData("<ClientInfo") == PLUS_FAIL;
  std::cout << "malformed XML rejected: " << (rejected ? "PASS" : "FAIL") << std::endl;
  return failures + !rejected;
}
