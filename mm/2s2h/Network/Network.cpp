#ifdef ENABLE_ANCHOR

#include "Network.h"
#include <chrono>
#include <spdlog/spdlog.h>

static bool sNetInitialized = false;

void Network::Enable(const char* host, uint16_t port) {
    if (isEnabled) {
        return;
    }

    if (!sNetInitialized) {
        if (SDLNet_Init() == -1) {
            SPDLOG_ERROR("[Network] SDLNet_Init: {}", SDLNet_GetError());
            return;
        }
        sNetInitialized = true;
    }

    if (receiveThread.joinable()) {
        receiveThread.join();
    }

    networkHost = host;
    networkPort = port;
    isEnabled = true;

    receiveThread = std::thread(&Network::ReceiveFromServer, this);
}

void Network::Disable() {
    if (!isEnabled) {
        return;
    }

    isEnabled = false;
    receiveThread.join();
}

void Network::OnIncomingJson(nlohmann::json payload) {
}

void Network::OnConnected() {
}

void Network::OnDisconnected() {
}

void Network::ProcessOutgoingPackets() {
}

void Network::SendDataToRemote(const char* payload) {
    SDLNet_TCP_Send(networkSocket, payload, static_cast<int>(strlen(payload) + 1));
}

void Network::SendJsonToRemote(nlohmann::json payload) {
    SendDataToRemote(payload.dump().c_str());
}

void Network::ReceiveFromServer() {
    bool loggedResolveError = false;

    while (isEnabled) {
        while (!isConnected && isEnabled) {
            // Resolved here so a slow or failed lookup never blocks the game, and is retried with the connection
            IPaddress networkAddress;
            if (SDLNet_ResolveHost(&networkAddress, networkHost.c_str(), networkPort) == 0) {
                loggedResolveError = false;
                networkSocket = SDLNet_TCP_Open(&networkAddress);
            } else if (!loggedResolveError) {
                loggedResolveError = true;
                SPDLOG_ERROR("[Network] SDLNet_ResolveHost: {}", SDLNet_GetError());
            }

            if (networkSocket) {
                receivedData.clear();
                SPDLOG_INFO("[Network] Connection to server established!");
                OnConnected();
                isConnected = true;
                break;
            }

            // Retry once a second instead of spinning while the server is unreachable
            for (int i = 0; i < 10 && isEnabled; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        SDLNet_SocketSet socketSet = SDLNet_AllocSocketSet(1);
        if (networkSocket) {
            SDLNet_TCP_AddSocket(socketSet, networkSocket);
        }

        while (isConnected && networkSocket && isEnabled) {
            // Short timeout so this thread sleeps while idle but still flushes outgoing packets promptly
            int socketsReady = SDLNet_CheckSockets(socketSet, 5);

            if (socketsReady == -1) {
                SPDLOG_ERROR("[Network] SDLNet_CheckSockets: {}", SDLNet_GetError());
                break;
            }

            ProcessOutgoingPackets();

            if (socketsReady == 0) {
                continue;
            }

            char remoteDataReceived[512];
            int len = SDLNet_TCP_Recv(networkSocket, &remoteDataReceived, sizeof(remoteDataReceived));
            if (len <= 0) {
                SPDLOG_ERROR("[Network] SDLNet_TCP_Recv: {}", SDLNet_GetError());
                break;
            }

            receivedData.append(remoteDataReceived, len);

            size_t delimiterPos = receivedData.find('\0');
            while (delimiterPos != std::string::npos) {
                std::string packet = receivedData.substr(0, delimiterPos);
                receivedData.erase(0, delimiterPos + 1);
                HandleRemoteJson(packet);
                delimiterPos = receivedData.find('\0');
            }
        }

        if (socketSet) {
            SDLNet_FreeSocketSet(socketSet);
        }

        if (networkSocket) {
            SDLNet_TCP_Close(networkSocket);
            networkSocket = nullptr;
        }

        if (isConnected) {
            isConnected = false;
            receivedData.clear();
            OnDisconnected();
            SPDLOG_INFO("[Network] Disconnected from server");
        }
    }
}

void Network::HandleRemoteJson(std::string payload) {
    nlohmann::json jsonPayload;
    try {
        jsonPayload = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Network] Failed to parse json: {}", e.what());
        return;
    }

    try {
        OnIncomingJson(jsonPayload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Network] Exception handling incoming JSON: {}", e.what());
    } catch (...) { SPDLOG_ERROR("[Network] Unknown exception handling incoming JSON"); }
}

#endif
