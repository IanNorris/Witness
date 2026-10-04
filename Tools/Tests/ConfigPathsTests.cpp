#include "ConfigPaths.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

static void Check(bool Value, const char* Message) { if (!Value) throw std::runtime_error(Message); }
template<class Function> static void Reject(Function Action) {
    bool Rejected = false;
    try { Action(); } catch (const std::invalid_argument&) { Rejected = true; }
    Check(Rejected, "invalid configuration path accepted");
}
struct OverrideEnvironment {
#ifdef _WIN32
    std::optional<std::wstring> Previous;
    OverrideEnvironment() {
        wchar_t* Buffer = nullptr;
        size_t Size = 0;
        const auto Error = _wdupenv_s(&Buffer, &Size, L"WITNESS_DATA_ROOT");
        const std::unique_ptr<wchar_t, decltype(&std::free)> Owned(Buffer, &std::free);
        Check(Error == 0, "environment read failed");
        if (Buffer) Previous = Buffer;
    }
    static void Set(const std::filesystem::path& Path) {
        Check(_wputenv_s(L"WITNESS_DATA_ROOT", Path.c_str()) == 0, "environment write failed");
    }
    ~OverrideEnvironment() { _wputenv_s(L"WITNESS_DATA_ROOT", Previous ? Previous->c_str() : L""); }
#else
    std::optional<std::string> Previous;
    OverrideEnvironment() { if (const auto* Value = std::getenv("WITNESS_DATA_ROOT")) Previous = Value; }
    static void Set(const std::filesystem::path& Path) {
        Check(setenv("WITNESS_DATA_ROOT", Path.c_str(), 1) == 0, "environment write failed");
    }
    ~OverrideEnvironment() {
        if (Previous) setenv("WITNESS_DATA_ROOT", Previous->c_str(), 1);
        else unsetenv("WITNESS_DATA_ROOT");
    }
#endif
};
int main(int Argc, char** Argv) {
    if (Argc != 2) return 2;
    const std::filesystem::path Root(Argv[1]);
    if (!Root.is_absolute() || std::filesystem::exists(Root)) return 2;
    try {
        using Witness::Configuration::ResolveFile;
        const auto Default = Root / "default";
        Check(ResolveFile(Default, {}, "server.db") == Default / "server.db", "default location changed");
        Check(ResolveFile(Default, Root, "server.db") == Root / "server.db", "override ignored");
        Check(ResolveFile({}, Root, "logs") == Root / "logs", "override depends on default root");
        Check(ResolveFile(Default, Root / "child" / "..", "client-health.db") == Root / "client-health.db", "normalization failed");
        for (const std::string Name : {"", ".", "..", "../server.db", "sub/db", "sub\\db", "server.db:stream", "/server.db"})
            Reject([&] { ResolveFile(Default, Root, Name); });
        Reject([&] { ResolveFile(Default, Root, std::string("x\0db", 4)); });
        Reject([&] { ResolveFile(Default, "relative", "server.db"); });
        Reject([&] { ResolveFile("relative", {}, "server.db"); });
        Reject([&] { ResolveFile({}, Root.root_path(), "server.db"); });
        Check(!std::filesystem::exists(Root), "pure resolution wrote files");
        OverrideEnvironment Environment;
        Environment.Set("relative");
        Reject([&] { GetConfigFilePath("server.db"); });
        Check(!std::filesystem::exists(Root), "invalid override wrote files");
        Environment.Set(Root);
        Check(GetConfigFilePath("server.db") == Root / "server.db", "production override was not used");
        Check(GetConfigFilePath("logs") == Root / "logs", "logs use a different root");
        Check(GetConfigFilePath("client-health.db") == Root / "client-health.db", "history uses a different root");
        Check(std::filesystem::is_directory(Root) && std::filesystem::is_empty(Root), "path lookup created data files");
        std::filesystem::remove(Root); // Exact empty directory created by this test.
        std::cout << "PASS default/override path selection, containment and production environment isolation\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
