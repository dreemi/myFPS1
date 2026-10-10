#include "net/transport.hpp"
#include <enet/enet.h>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace Net {

static int initCount = 0;
static const int CHANNELS = 2; // 0 = reliable, ordered; 1 = unreliable, sequenced

bool InitNetworking() {
    if (initCount++ > 0) return true;
    if (enet_initialize() != 0) {
        initCount = 0;
        return false;
    }
    return true;
}

void ShutdownNetworking() {
    if (initCount > 0 && --initCount == 0) enet_deinitialize();
}

std::vector<std::string> LocalAddresses() {
    std::vector<std::string> out;
    char name[256] = {};
    if (gethostname(name, sizeof(name)) != 0) return out;
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, nullptr, &hints, &list) != 0) return out;
    for (addrinfo* a = list; a; a = a->ai_next) {
        char buf[64];
        const sockaddr_in* sin = (const sockaddr_in*)a->ai_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            std::string s = buf;
            if (s.rfind("127.", 0) != 0) {
                bool dup = false;
                for (const std::string& o : out) dup |= o == s;
                if (!dup) out.push_back(s);
            }
        }
    }
    freeaddrinfo(list);
    return out;
}

Transport::Transport() { InitNetworking(); }

Transport::~Transport() {
    Close();
    ShutdownNetworking();
}

bool Transport::Host(uint16_t port, int maxClients) {
    Close();
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    host = enet_host_create(&address, (size_t)maxClients, CHANNELS, 0, 0);
    isClient = false;
    return host != nullptr;
}

bool Transport::Connect(const std::string& addressText, uint16_t port) {
    Close();
    ENetAddress address{};
    if (enet_address_set_host(&address, addressText.c_str()) != 0) return false;
    address.port = port;
    host = enet_host_create(nullptr, 1, CHANNELS, 0, 0);
    if (!host) return false;
    ENetPeer* peer = enet_host_connect((ENetHost*)host, &address, CHANNELS, 0);
    if (!peer) {
        enet_host_destroy((ENetHost*)host);
        host = nullptr;
        return false;
    }
    // Give up after ~5 s without a reply rather than ENet's default 30 s
    enet_peer_timeout(peer, 0, 4000, 5000);
    isClient = true;
    peers.assign(1, peer);
    peer->data = (void*)(uintptr_t)1; // id 0, stored +1 so null means "unknown"
    return true;
}

static ENetPacket* MakePacket(const std::vector<uint8_t>& data, bool reliable) {
    return enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
}

void Transport::Send(uint32_t peer, const std::vector<uint8_t>& data, bool reliable) {
    if (!host || peer >= peers.size() || !peers[peer]) return;
    enet_peer_send((ENetPeer*)peers[peer], reliable ? 0 : 1, MakePacket(data, reliable));
}

void Transport::Broadcast(const std::vector<uint8_t>& data, bool reliable) {
    if (!host) return;
    enet_host_broadcast((ENetHost*)host, reliable ? 0 : 1, MakePacket(data, reliable));
}

void Transport::Poll(std::vector<TransportEvent>& events) {
    if (!host) return;
    ENetEvent e;
    while (host && enet_host_service((ENetHost*)host, &e, 0) > 0) {
        switch (e.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            uint32_t id;
            if (isClient) {
                id = 0;
            } else {
                // Reuse a free slot, else add one
                id = (uint32_t)peers.size();
                for (uint32_t i = 0; i < peers.size(); i++)
                    if (!peers[i]) { id = i; break; }
                if (id == peers.size()) peers.push_back(nullptr);
                peers[id] = e.peer;
                e.peer->data = (void*)(uintptr_t)(id + 1);
                enet_peer_timeout(e.peer, 0, 4000, 8000);
            }
            events.push_back({ TransportEvent::Connected, id, {} });
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            uint32_t id = e.peer->data ? (uint32_t)(uintptr_t)e.peer->data - 1 : 0;
            TransportEvent t{ TransportEvent::Received, id, {} };
            t.data.assign(e.packet->data, e.packet->data + e.packet->dataLength);
            events.push_back(std::move(t));
            enet_packet_destroy(e.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            uint32_t id = e.peer->data ? (uint32_t)(uintptr_t)e.peer->data - 1 : 0;
            if (id < peers.size()) peers[id] = nullptr;
            e.peer->data = nullptr;
            events.push_back({ TransportEvent::Disconnected, id, {} });
            break;
        }
        default: break;
        }
    }
}

void Transport::Flush() {
    if (host) enet_host_flush((ENetHost*)host);
}

void Transport::Disconnect(uint32_t peer) {
    if (!host || peer >= peers.size() || !peers[peer]) return;
    enet_peer_disconnect_later((ENetPeer*)peers[peer], 0);
}

void Transport::Close() {
    if (!host) return;
    ENetHost* h = (ENetHost*)host;
    // Tell everyone we're leaving, give it a moment to get out, then drop the rest
    for (void* p : peers)
        if (p) enet_peer_disconnect((ENetPeer*)p, 0);
    enet_host_flush(h);
    ENetEvent e;
    int waited = 0;
    while (waited < 200 && enet_host_service(h, &e, 10) >= 0) {
        waited += 10;
        if (e.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e.packet);
        bool anyLeft = false;
        for (size_t i = 0; i < h->peerCount; i++)
            if (h->peers[i].state != ENET_PEER_STATE_DISCONNECTED) anyLeft = true;
        if (!anyLeft) break;
    }
    for (size_t i = 0; i < h->peerCount; i++)
        if (h->peers[i].state != ENET_PEER_STATE_DISCONNECTED) enet_peer_reset(&h->peers[i]);
    enet_host_destroy(h);
    host = nullptr;
    peers.clear();
}

int Transport::PingMs(uint32_t peer) const {
    if (!host || peer >= peers.size() || !peers[peer]) return 0;
    return (int)((ENetPeer*)peers[peer])->roundTripTime;
}

} // namespace Net
