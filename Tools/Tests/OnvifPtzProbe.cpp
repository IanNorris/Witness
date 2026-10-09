// Camera-independent protocol fixture. All mutating requests are loopback-only.
#include "OnvifPtzClient.h"
#include <cstdlib>
#include <iostream>
#include <string>
static std::string Environment(const char* name) {
#ifdef _WIN32
    char* value=nullptr;size_t size=0;
    if(_dupenv_s(&value,&size,name)!=0) return {};
    const std::string result=value?value:"";std::free(value);return result;
#else
    const auto value=std::getenv(name);return value?value:"";
#endif
}
int main(int argc,char** argv) {
    // Explicit read-only C210 diagnosis. Credentials enter through environment,
    // never command arguments/output; this path cannot dispatch motor commands.
    if(argc==2 && std::string(argv[1])=="--read-only-discovery") {
        const auto user=Environment("WITNESS_ONVIF_USER"),pass=Environment("WITNESS_ONVIF_PASSWORD"),host=Environment("WITNESS_ONVIF_HOST");
        if(user.empty() || pass.empty() || host.empty()) return 2;
        auto client=OnvifPtzClient::Get(host,2020,user,pass);
        bool ok=client->SupportsPanTilt();
        std::cout<<"supported="<<ok<<'\n'<<client->Error()<<'\n'<<client->ProfileEvidence();return ok?0:3;
    }
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
