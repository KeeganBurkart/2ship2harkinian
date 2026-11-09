#pragma once

#if defined(ENABLE_ANCHOR) && defined(__cplusplus)

#include <atomic>
#include <string>
#include <thread>
#include <SDL2/SDL_net.h>
#include <nlohmann/json.hpp>

/**
 * Minimal TCP client that exchanges null-terminated JSON packets with a server on a background thread.
 * Ported from Ship of Harkinian's soh/Network/Network.
 */
class Network {
  private:
    std::string networkHost;
    uint16_t networkPort = 0;
    TCPsocket networkSocket = nullptr;
    std::thread receiveThread;
    std::string receivedData;

    void ReceiveFromServer();
    void HandleRemoteJson(std::string payload);

  public:
    std::atomic<bool> isEnabled = false;
    std::atomic<bool> isConnected = false;

    virtual ~Network() = default;

    void Enable(const char* host, uint16_t port);
    void Disable();

    // Called on the network thread for every complete packet
    virtual void OnIncomingJson(nlohmann::json payload);
    // Called on the network thread, just before isConnected becomes true
    virtual void OnConnected();
    virtual void OnDisconnected();
    // Called on the network thread in its loop, the only place outgoing data should be written
    virtual void ProcessOutgoingPackets();
    void SendDataToRemote(const char* payload);
    virtual void SendJsonToRemote(nlohmann::json packet);
};

#endif
