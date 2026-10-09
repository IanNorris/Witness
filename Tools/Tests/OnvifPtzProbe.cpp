// Camera-independent protocol fixture. All mutating requests are loopback-only.
#include "OnvifPtzClient.h"
#include <cstdlib>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    char* end=nullptr;const long port=std::strtol(argv[1],&end,10);
    if(!end || *end || port<1 || port>65535) return 2;
    auto client=OnvifPtzClient::Get("127.0.0.1",static_cast<int>(port),"fixture&user","fixture-password");
    const std::string operation=argv[2];bool ok=false;
    if(operation=="move") ok=client->Move(PtzOp::LeftUp,32,[]{return true;});
    else if(operation=="stop") ok=client->Stop();
    else if(operation=="discover") ok=client->SupportsPanTilt();
    else if(operation=="cancel") ok=!client->Move(PtzOp::Left,32,[]{return false;});
    else if(operation=="preset") {auto presets=client->Presets();ok=presets.size()==1 && presets[0].Name=="Gate & drive" && client->GoToPreset(presets[0].Id,[]{return true;});}
    else return 2;
    if(!ok) std::cerr<<client->Error()<<'\n';
    return ok?0:3;
}
