#pragma once

#if defined(ENABLE_ANCHOR) && defined(__cplusplus)

#include "2s2h/Network/Network.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <map>
#include <mutex>
#include <queue>
#include <string>

extern "C" {
#include "z64.h"
}

#define CVAR_ANCHOR(name) "gNetwork.Anchor." name
#define ANCHOR_DEFAULT_HOST "anchor.hm64.org"
#define ANCHOR_DEFAULT_PORT 43383

struct AnchorClient {
    uint32_t clientId = 0;
    std::string name;
    Color_RGB8 color = { 100, 255, 100 };
    std::string clientVersion;
    std::string teamId;
    bool online = false;
    bool self = false;
    uint32_t seed = 0;
    bool isSaveLoaded = false;
    bool isGameComplete = false;
    s16 sceneId = SCENE_MAX;
    s8 roomNum = -1;
    s32 entrance = 0;

    // Only available in PLAYER_UPDATE packets
    u8 form = PLAYER_FORM_HUMAN;
    PosRot posRot = {};
    Vec3s jointTable[PLAYER_LIMB_MAX] = {};
    s16 appearanceInfo = 0;
    u8 movementFlags = 0;
    Vec3s prevTransl = {};
    Vec3s upperLimbRot = {};
    u32 stateFlags1 = 0;
    u32 stateFlags2 = 0;
    u32 stateFlags3 = 0;
    s8 currentShield = 0;
    s8 currentBoots = 0;
    u8 currentMask = 0;
    s8 itemAction = 0;
    s8 heldItemAction = 0;
    u8 heldItemId = 0;
    u8 modelGroup = 0;
    s8 invincibilityTimer = 0;
    f32 unk_ABC = 0.0f;
    s8 actionVar1 = 0;

    // The puppet actor representing this client in the current scene
    Player* player = nullptr;
};

struct RoomState {
    uint32_t ownerClientId = 0;
    u8 teleportMode = 1;      // 0 = off, 1 = team, 2 = all
    u8 syncItemsAndFlags = 1; // 0 = off, 1 = on
};

class Anchor : public Network {
  private:
    uint32_t spawningPuppetForClientId = 0;
    bool shouldRefreshActors = false;
    bool justLoadedSave = false;
    bool isProcessingIncomingPacket = false;
    std::queue<nlohmann::json> incomingPacketQueue;
    std::mutex incomingPacketQueueMutex;
    std::queue<nlohmann::json> outgoingPacketQueue;
    std::mutex outgoingPacketQueueMutex;

    nlohmann::json PrepClientState();
    nlohmann::json PrepRoomState();
    nlohmann::json PrepTeamState();
    void ApplyTeamState(const nlohmann::json& state);
    void RefreshClientActors();

    void HandlePacket_AllClientState(nlohmann::json payload);
    void HandlePacket_GiveItem(nlohmann::json payload);
    void HandlePacket_PlayerUpdate(nlohmann::json payload);
    void HandlePacket_RequestTeamState(nlohmann::json payload);
    void HandlePacket_RequestTeleport(nlohmann::json payload);
    void HandlePacket_ServerMessage(nlohmann::json payload);
    void HandlePacket_SetFlag(nlohmann::json payload);
    void HandlePacket_SetSceneFlag(nlohmann::json payload);
    void HandlePacket_TeleportTo(nlohmann::json payload);
    void HandlePacket_UpdateClientState(nlohmann::json payload);
    void HandlePacket_UpdateRoomState(nlohmann::json payload);
    void HandlePacket_UpdateTeamState(nlohmann::json payload);

  public:
    uint32_t ownClientId = 0;
    std::string clientVersion;

    // Packet types //
    inline static const std::string ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
    inline static const std::string DISABLE_ANCHOR = "DISABLE_ANCHOR";
    inline static const std::string GIVE_ITEM = "GIVE_ITEM";
    inline static const std::string HANDSHAKE = "HANDSHAKE";
    inline static const std::string PLAYER_UPDATE = "PLAYER_UPDATE";
    inline static const std::string REQUEST_TEAM_STATE = "REQUEST_TEAM_STATE";
    inline static const std::string REQUEST_TELEPORT = "REQUEST_TELEPORT";
    inline static const std::string SERVER_MESSAGE = "SERVER_MESSAGE";
    inline static const std::string SET_FLAG = "SET_FLAG";
    inline static const std::string SET_SCENE_FLAG = "SET_SCENE_FLAG";
    inline static const std::string TELEPORT_TO = "TELEPORT_TO";
    inline static const std::string UPDATE_CLIENT_STATE = "UPDATE_CLIENT_STATE";
    inline static const std::string UPDATE_ROOM_STATE = "UPDATE_ROOM_STATE";
    inline static const std::string UPDATE_TEAM_STATE = "UPDATE_TEAM_STATE";

    static Anchor* Instance;
    std::map<uint32_t, AnchorClient> clients;
    RoomState roomState;

    void Enable();
    void Disable();
    void OnIncomingJson(nlohmann::json payload) override;
    void OnConnected() override;
    void OnDisconnected() override;
    void ProcessOutgoingPackets() override;
    void SendJsonToRemote(nlohmann::json packet) override;
    void ProcessIncomingPacketQueue();
    void RegisterHooks();
    bool IsSaveLoaded();
    bool CanTeleportTo(uint32_t clientId);
    bool IsSyncing();
    std::string GetTeamId();

    // Puppet bookkeeping
    uint32_t GetPuppetClientId(const Actor* actor);
    void SetPuppetClientId(const Actor* actor, uint32_t clientId);
    uint32_t TakeSpawningPuppetClientId();

    void SendPacket_GiveItem(u8 itemId);
    void SendPacket_Handshake();
    void SendPacket_PlayerUpdate();
    void SendPacket_RequestTeamState();
    void SendPacket_RequestTeleport(uint32_t clientId);
    void SendPacket_SetFlag(s16 flagType, u32 flag);
    void SendPacket_SetSceneFlag(s16 sceneId, s16 flagType, u32 flag);
    void SendPacket_TeleportTo(uint32_t clientId);
    void SendPacket_UpdateClientState();
    void SendPacket_UpdateRoomState();
    void SendPacket_UpdateTeamState();
};

void Anchor_DrawMenu();

#endif
