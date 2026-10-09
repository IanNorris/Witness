#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include "OnvifPtzClient.h"
#include <tinyxml2.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace {
constexpr const char* Soap = "http://www.w3.org/2003/05/soap-envelope";
constexpr const char* Schema = "http://www.onvif.org/ver10/schema";
constexpr const char* Device = "http://www.onvif.org/ver10/device/wsdl";
constexpr const char* Media = "http://www.onvif.org/ver10/media/wsdl";
constexpr const char* Ptz = "http://www.onvif.org/ver20/ptz/wsdl";
constexpr const char* VelocitySpace = "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace";
using Element = tinyxml2::XMLElement;
bool Is(const Element* e, const char* local, const char* ns) {
    if (!e) return false;
    const std::string name = e->Name(); const auto colon = name.find(':');
    if (name.substr(colon == std::string::npos ? 0 : colon + 1) != local) return false;
    const std::string declaration = colon == std::string::npos ? "xmlns" : "xmlns:" + name.substr(0, colon);
    for (auto p = e; p; p = p->Parent() ? p->Parent()->ToElement() : nullptr)
        if (const char* value = p->Attribute(declaration.c_str())) return std::string(value) == ns;
    return false;
}
Element* Child(Element* e, const char* local, const char* ns) {
    for (auto p = e ? e->FirstChildElement() : nullptr; p; p = p->NextSiblingElement()) if (Is(p, local, ns)) return p;
    return nullptr;
}
std::string Text(Element* e) { return e && e->GetText() ? e->GetText() : ""; }
std::string Escape(const std::string& text) {
    std::string out;
    for (const char c : text) switch(c) {
        case '&': out += "&amp;"; break; case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break; case '\"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break; default: out += c;
    }
    return out;
}
std::string Base64(const unsigned char* data, int length) {
    std::string out(static_cast<size_t>(4 * ((length + 2) / 3)), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, length); return out;
}
std::string Security(const std::string& user, const std::string& password) {
    unsigned char nonce[16], digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) throw std::runtime_error("ONVIF nonce generation failed");
    const std::time_t now = std::time(nullptr); std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char timestamp[32]; std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::string input(reinterpret_cast<char*>(nonce), sizeof(nonce)); input += timestamp; input += password;
    if (EVP_Digest(input.data(), input.size(), digest, &length, EVP_sha1(), nullptr) != 1) throw std::runtime_error("ONVIF authentication digest failed");
    const std::string root = "http://docs.oasis-open.org/wss/2004/01/";
    return "<wsse:Security s:mustUnderstand=\"1\" xmlns:wsse=\"" + root + "oasis-200401-wss-wssecurity-secext-1.0.xsd\" xmlns:wsu=\"" + root + "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:Username>" + Escape(user) + "</wsse:Username><wsse:Password Type=\"" + root + "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" + Base64(digest, static_cast<int>(length)) + "</wsse:Password><wsse:Nonce EncodingType=\"" + root + "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" + Base64(nonce, sizeof(nonce)) + "</wsse:Nonce><wsu:Created>" + timestamp + "</wsu:Created></wsse:UsernameToken></wsse:Security>";
}
// Camera-advertised authorities never change the configured destination or
// receive credentials. Only an ONVIF service path is taken from XAddr.
std::string ServicePath(const std::string& url) {
    const auto scheme = url.find("://");
    const auto slash = scheme == std::string::npos ? 0 : url.find('/', scheme + 3);
    if (slash == std::string::npos) return "";
    const auto path = url.substr(slash);
    return path.starts_with("/onvif/") && path.find_first_of("?#\r\n") == std::string::npos ? path : "";
}
}

struct OnvifPtzClient::Impl {
    std::recursive_mutex Mutex;
    std::string Host, User, Password, LastError, MediaPath, PtzPath, Profile, ProfileXml;
    int Port;
    std::chrono::steady_clock::time_point Retry{};
    std::map<int, std::string> Tokens;
    bool Ready = false;
    std::string MoveTimeout = "PT10S";
    Impl(const std::string& host, int port, const std::string& user, const std::string& pass) : Host(host), User(user), Password(pass), Port(port) {}
    Element* Call(const std::string& path, const char* ns, const std::string& operation,
        const std::string& parameters, tinyxml2::XMLDocument& doc, ReolinkClient::SendAllowed allowed = nullptr) {
        if (allowed && !allowed()) { LastError = "PTZ movement cancelled"; return nullptr; }
        httplib::Client client(Host, Port);
        client.set_connection_timeout(2); client.set_read_timeout(3); client.set_write_timeout(2);
        // WS-Security only: no HTTP-auth retry may replay a cancelled movement.
        client.set_follow_location(false);
        const std::string request = "<s:Envelope xmlns:s=\"" + std::string(Soap) + "\" xmlns:tt=\"" + Schema + "\" xmlns:m=\"" + ns + "\"><s:Header>" + Security(User, Password) + "</s:Header><s:Body><m:" + operation + ">" + parameters + "</m:" + operation + "></s:Body></s:Envelope>";
        httplib::Request wire; wire.method="POST";wire.path=path;wire.body=request;
        wire.set_header("Content-Type", "application/soap+xml; charset=utf-8; action=\"" + std::string(ns) + "/" + operation + "\"");
        std::string payload;
        wire.content_receiver=[&payload](const char* data,size_t size,uint64_t,uint64_t) {
            if(size>1024*1024-payload.size()) return false;
            payload.append(data,size);return true;
        };
        if (allowed && !allowed()) { LastError="PTZ movement cancelled";return nullptr; }
        auto result = client.send(wire);
        if (!result || result->status != 200) { LastError = "ONVIF " + operation + ": " + (result ? "HTTP " + std::to_string(result->status) : httplib::to_string(result.error())); return nullptr; }
        if (payload.find("<!DOCTYPE") != std::string::npos ||
            doc.Parse(payload.data(), payload.size()) != tinyxml2::XML_SUCCESS) {
            LastError = "ONVIF " + operation + ": invalid XML response"; return nullptr;
        }
        auto envelope = doc.RootElement();
        auto body = Is(envelope, "Envelope", Soap) && !envelope->NextSiblingElement() ? Child(envelope, "Body", Soap) : nullptr;
        auto response = body ? body->FirstChildElement() : nullptr;
        if (!Is(response, (operation + "Response").c_str(), ns) || response->NextSiblingElement()) {
            LastError = "ONVIF " + operation + ": fault or missing acknowledgement"; return nullptr;
        }
        LastError.clear(); return response;
    }
    bool Discover() {
        if (Ready) return true;
        if (std::chrono::steady_clock::now() < Retry) return false;
        Retry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        tinyxml2::XMLDocument caps, profiles, options;
        auto response = Call("/onvif/device_service", Device, "GetCapabilities", "<m:Category>All</m:Category>", caps);
        if(!response) return false;
        {tinyxml2::XMLPrinter printer;caps.Print(&printer);ProfileXml=printer.CStr();}
        auto capabilities = Child(response, "Capabilities", Device);
        MediaPath = ServicePath(Text(Child(Child(capabilities, "Media", Schema), "XAddr", Schema)));
        PtzPath = ServicePath(Text(Child(Child(capabilities, "PTZ", Schema), "XAddr", Schema)));
        if (MediaPath.empty() || PtzPath.empty()) { LastError = "Camera does not advertise ONVIF PTZ"; return false; }
        response = Call(MediaPath, Media, "GetProfiles", "", profiles);
        if(!response) return false;
        {tinyxml2::XMLPrinter printer;profiles.Print(&printer);ProfileXml+=printer.CStr();}
        std::string configuration;
        for (auto p = response ? response->FirstChildElement() : nullptr; p; p = p->NextSiblingElement()) {
            if (!Is(p, "Profiles", Media)) continue;
            auto ptz = Child(p, "PTZConfiguration", Schema);
            if (ptz && p->Attribute("token") && ptz->Attribute("token")) {
                Profile = p->Attribute("token"); configuration = ptz->Attribute("token"); break;
            }
        }
        if (configuration.empty()) { LastError = "No ONVIF PTZ media profile"; return false; }
        response = Call(PtzPath, Ptz, "GetConfigurationOptions", "<m:ConfigurationToken>" + Escape(configuration) + "</m:ConfigurationToken>", options);
        if(!response) return false;
        auto spaces = Child(Child(response, "PTZConfigurationOptions", Ptz), "Spaces", Schema);
        bool supported = false;
        for (auto p = spaces ? spaces->FirstChildElement() : nullptr; p; p = p->NextSiblingElement()) {
            if (!Is(p, "ContinuousPanTiltVelocitySpace", Schema) || Text(Child(p, "URI", Schema)) != VelocitySpace) continue;
            auto x = Child(p, "XRange", Schema), y = Child(p, "YRange", Schema);
            auto minX = Child(x, "Min", Schema), maxX = Child(x, "Max", Schema), minY = Child(y, "Min", Schema), maxY = Child(y, "Max", Schema);
            double a=0,b=0,c=0,d=0;
            supported = minX && maxX && minY && maxY && minX->QueryDoubleText(&a)==0 && maxX->QueryDoubleText(&b)==0 &&
                minY->QueryDoubleText(&c)==0 && maxY->QueryDoubleText(&d)==0 && a<=-1 && b>=1 && c<=-1 && d>=1;
        }
        if (!supported) { LastError = "Camera does not advertise normalized continuous pan/tilt"; return false; }
        auto timeout=Child(Child(response,"PTZConfigurationOptions",Ptz),"PTZTimeout",Schema);
        auto seconds=[](const std::string& value) {
            if(!value.starts_with("PT") || !value.ends_with("S")) return -1.0;
            try {size_t used=0;auto number=std::stod(value.substr(2,value.size()-3),&used);
                return used==value.size()-3 && std::isfinite(number)?number:-1.0;
            } catch(...) {return -1.0;}
        };
        const double minimum=seconds(Text(Child(timeout,"Min",Schema))), maximum=seconds(Text(Child(timeout,"Max",Schema)));
        if(minimum<=0 || maximum<minimum || minimum>10) {LastError="Camera does not advertise a safe PTZ timeout range";return false;}
        MoveTimeout="PT"+std::to_string(std::min(10.0,maximum))+"S";
        Ready = true; return true;
    }
    std::string Token() const { return "<m:ProfileToken>" + Escape(Profile) + "</m:ProfileToken>"; }
};

OnvifPtzClient::OnvifPtzClient(const std::string& h, int p, const std::string& u, const std::string& pass) : State(std::make_unique<Impl>(h,p,u,pass)) {}
OnvifPtzClient::~OnvifPtzClient() = default;
std::shared_ptr<OnvifPtzClient> OnvifPtzClient::Get(const std::string& h, int p, const std::string& u, const std::string& pass) {
    static std::mutex mutex;
    static std::map<std::tuple<std::string,int,std::string,std::string>, std::shared_ptr<OnvifPtzClient>> clients;
    std::lock_guard lock(mutex);
    auto key = std::make_tuple(h,p,u,pass);
    if (auto it=clients.find(key); it!=clients.end()) return it->second;
    if (clients.size()>=64) {
        for (auto it=clients.begin(); it!=clients.end();) if(it->second.use_count()==1) it=clients.erase(it); else ++it;
        if (clients.size()>=64) return nullptr;
    }
    auto result = std::shared_ptr<OnvifPtzClient>(new OnvifPtzClient(h,p,u,pass)); clients.emplace(std::move(key),result); return result;
}
std::string OnvifPtzClient::Error() const { std::lock_guard lock(State->Mutex); return State->LastError; }
std::string OnvifPtzClient::ProfileEvidence() const { std::lock_guard lock(State->Mutex);return State->ProfileXml; }
bool OnvifPtzClient::Move(PtzOp op, int speed, ReolinkClient::SendAllowed allowed) {
    std::lock_guard lock(State->Mutex);
    double x=0,y=0;
    switch(op) {
        case PtzOp::Left: x=-1; break; case PtzOp::Right: x=1; break;
        case PtzOp::Up: y=1; break; case PtzOp::Down: y=-1; break;
        case PtzOp::LeftUp: x=-1;y=1;break; case PtzOp::LeftDown:x=-1;y=-1;break;
        case PtzOp::RightUp:x=1;y=1;break; case PtzOp::RightDown:x=1;y=-1;break;
        default: State->LastError="This ONVIF camera supports pan/tilt only"; return false;
    }
    if (speed<1 || speed>64 || !State->Discover()) return false;
    const double scale=static_cast<double>(speed)/64;
    tinyxml2::XMLDocument doc;
    return State->Call(State->PtzPath,Ptz,"ContinuousMove",State->Token()+"<m:Velocity><tt:PanTilt x=\""+std::to_string(x*scale)+"\" y=\""+std::to_string(y*scale)+"\" space=\""+VelocitySpace+"\"/></m:Velocity><m:Timeout>"+State->MoveTimeout+"</m:Timeout>",doc,allowed)!=nullptr;
}
bool OnvifPtzClient::Stop() {
    std::lock_guard lock(State->Mutex);
    if (!State->Discover()) return false;
    tinyxml2::XMLDocument doc;
    return State->Call(State->PtzPath,Ptz,"Stop",State->Token()+"<m:PanTilt>true</m:PanTilt><m:Zoom>false</m:Zoom>",doc)!=nullptr;
}
bool OnvifPtzClient::SupportsPanTilt() { std::lock_guard lock(State->Mutex); return State->Discover(); }
std::vector<PtzPreset> OnvifPtzClient::Presets() {
    std::lock_guard lock(State->Mutex); std::vector<PtzPreset> result;
    if (!State->Discover()) return result;
    tinyxml2::XMLDocument doc;
    auto response=State->Call(State->PtzPath,Ptz,"GetPresets",State->Token(),doc);
    for(auto p=response?response->FirstChildElement():nullptr;p;p=p->NextSiblingElement()) {
        if(!Is(p,"Preset",Ptz) || !p->Attribute("token")) continue;
        const std::string token=p->Attribute("token");
        int id=0;
        for(const auto& [existing,value]:State->Tokens) if(value==token) {id=existing;break;}
        if(!id && State->Tokens.size()<65535) { id=static_cast<int>(State->Tokens.size())+1; State->Tokens[id]=token; }
        if(id) result.push_back({id,Text(Child(p,"Name",Schema)),true});
    }
    return result;
}
bool OnvifPtzClient::GoToPreset(int id, ReolinkClient::SendAllowed allowed) {
    std::lock_guard lock(State->Mutex);
    const auto it=State->Tokens.find(id);
    if(it==State->Tokens.end()) {State->LastError="Reload saved positions before selecting a preset";return false;}
    tinyxml2::XMLDocument doc;
    return State->Call(State->PtzPath,Ptz,"GotoPreset",State->Token()+"<m:PresetToken>"+Escape(it->second)+"</m:PresetToken>",doc,allowed)!=nullptr;
}
