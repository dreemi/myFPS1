#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A thin wrapper over ENet (UDP with reliable and unreliable channels). This
// header deliberately includes nothing from ENet or the OS so it can sit next
// to raylib, whose names clash with windows.h.
namespace Net {

constexpr uint16_t DEFAULT_PORT = 27015;

struct TransportEvent {
    enum Type { Connected, Disconnected, Received } type;
    uint32_t peer = 0;           // the connection's id (the client sees the host as peer 0)
    std::vector<uint8_t> data;   // Received only
};

class Transport {
public:
    Transport();
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Host: listen for up to maxClients on port (all interfaces). False if the port is taken.
    bool Host(uint16_t port, int maxClients);
    // Client: start connecting (a Connected or Disconnected event follows). False if the address won't resolve.
    bool Connect(const std::string& address, uint16_t port);

    // Sends to one peer; reliable messages arrive in order, unreliable ones may not arrive at all
    void Send(uint32_t peer, const std::vector<uint8_t>& data, bool reliable);
    void Broadcast(const std::vector<uint8_t>& data, bool reliable);
    // Process network traffic; appends what happened
    void Poll(std::vector<TransportEvent>& events);
    void Flush();

    void Disconnect(uint32_t peer);  // politely: the peer gets a Disconnected event
    void Close();                    // disconnect everyone and stop
    bool IsOpen() const { return host != nullptr; }
    int PingMs(uint32_t peer) const; // round-trip estimate

private:
    void* host = nullptr;            // ENetHost*
    std::vector<void*> peers;        // ENetPeer* by id (null when gone)
    bool isClient = false;
};

// One-time setup of the network library (safe to call repeatedly)
bool InitNetworking();
void ShutdownNetworking();
// This machine's local network addresses (to tell friends on the LAN where to connect)
std::vector<std::string> LocalAddresses();

} // namespace Net
