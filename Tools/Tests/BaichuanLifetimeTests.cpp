// Calls the exported DLL API, not a second compilation of its implementation.
#include <ReolinkBaichuanClient.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#define NOMINMAX
#include <WinSock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket InvalidSocket = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket InvalidSocket = -1;
#define closesocket close
#endif

using Witness::Camera::ReolinkBaichuanClient;
static_assert(!std::is_copy_constructible_v<ReolinkBaichuanClient>);
static_assert(!std::is_move_constructible_v<ReolinkBaichuanClient>);
static_assert(sizeof(ReolinkBaichuanClient) == sizeof(void*));

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct OwnedSocket
{
    Socket value = InvalidSocket;
    ~OwnedSocket() { if (value != InvalidSocket) closesocket(value); }
};

static bool Readable(Socket socket)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{3, 0};
    return select(static_cast<int>(socket + 1), &set, nullptr, nullptr, &timeout) == 1;
}

int main()
{
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
    try
    {
        // No camera credentials, network hardware, authentication bypass or PTZ.
        OwnedSocket listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        Require(listener.value != InvalidSocket, "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        AddressLength length = sizeof(address);
        Require(getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname");
        Require(listen(listener.value, 1) == 0, "listen");
        const int port = ntohs(address.sin_port);

        for (int iteration = 0; iteration < 20; ++iteration)
        {
            auto* client = new ReolinkBaichuanClient("127.0.0.1", port, "synthetic", "not-a-credential");
            try
            {
                Require(!client->IsConnected(), "initial connection state");
                Require(client->GetLastError().empty(), "initial error state");
                auto snapshot = client->GetDetections();
                Require(!snapshot.HasData && !snapshot.HasMotion && snapshot.Detections.empty(), "initial detections");
                snapshot.Detections.emplace_back();
                Require(client->GetDetections().Detections.empty(), "snapshot ownership");
                client->Stop(); // Idle stop must be safe before starting.
                client->Start();
                client->Start(); // Existing idempotent start contract.
                Require(Readable(listener.value), "accept timeout");
                OwnedSocket peer{accept(listener.value, nullptr, nullptr)};
                Require(peer.value != InvalidSocket, "accept");
                char negotiation[20];
                int received = 0;
                while (received < static_cast<int>(sizeof(negotiation)))
                {
                    Require(Readable(peer.value), "negotiation timeout");
                    const int count = recv(peer.value, negotiation + received, static_cast<int>(sizeof(negotiation)) - received, 0);
                    Require(count > 0, "negotiation receive");
                    received += count;
                }
                // Peer deliberately never replies. Destruction must interrupt
                // the DLL-owned blocking receive and join its thread first.
                const auto start = std::chrono::steady_clock::now();
                if (iteration % 2 == 0) { client->Stop(); client->Stop(); }
                delete client;
                client = nullptr;
                Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "shutdown exceeded bound");
            }
            catch (...)
            {
                delete client;
                throw;
            }
        }
        std::cout << "20 cross-DLL snapshot and stalled TCP shutdown cycles passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
