#ifdef ENABLE_ANCHOR

#include "Anchor.h"
#include "JsonConversions.hpp"
#include "Puppet.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/ObjectExtension/ObjectExtension.h"
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "build.h"
extern PlayState* gPlayState;
void Actor_AddToCategory(ActorContext* actorCtx, Actor* actor, u8 actorCategory);
Actor* Actor_RemoveFromCategory(PlayState* play, ActorContext* actorCtx, Actor* actorToRemove);
}

Anchor* Anchor::Instance = new Anchor();

// MARK: - Connection

void Anchor::Enable() {
    clientVersion = std::string((char*)gGitCommitHash);
    ownClientId = CVarGetInteger(CVAR_ANCHOR("LastClientId"), 0);
    roomState.ownerClientId = 0;
    Network::Enable(CVarGetString(CVAR_ANCHOR("Host"), ANCHOR_DEFAULT_HOST),
                    CVarGetInteger(CVAR_ANCHOR("Port"), ANCHOR_DEFAULT_PORT));
    RegisterHooks();
}

void Anchor::Disable() {
    Network::Disable();
    wasConnected = false;
    clients.clear();
    RefreshClientActors();
    UpdateEnemyHealthScale();
    RegisterHooks();
}

// Runs on the network thread, so it only flags the new connection for the game thread to pick up
void Anchor::OnConnected() {
    {
        std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
        outgoingPacketQueue = {};
    }
    connectionGeneration++;
}

std::string Anchor::GetTeamId() {
    return CVarGetString(CVAR_ANCHOR("TeamId"), "default");
}

bool Anchor::IsSyncing() {
    return isConnected && roomState.syncItemsAndFlags && IsSaveLoaded();
}

bool Anchor::IsSaveLoaded() {
    return gPlayState != nullptr && GET_PLAYER(gPlayState) != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

// MARK: - Packet queues

void Anchor::SendJsonToRemote(nlohmann::json payload) {
    if (!isConnected) {
        return;
    }

    // Nothing may go out on a new connection ahead of its handshake
    if (payload["type"] != HANDSHAKE && handshakeGeneration != connectionGeneration) {
        return;
    }

    payload["clientId"] = ownClientId;

    std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
    outgoingPacketQueue.push(payload);
}

void Anchor::ProcessOutgoingPackets() {
    std::queue<nlohmann::json> packetsToSend;
    {
        std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
        packetsToSend.swap(outgoingPacketQueue);
    }

    while (!packetsToSend.empty()) {
        Network::SendJsonToRemote(packetsToSend.front());
        packetsToSend.pop();
    }
}

// Runs on the network thread; packets are only looked at once the game thread drains the queue
void Anchor::OnIncomingJson(nlohmann::json payload) {
    std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
    incomingPacketQueue.push(payload);
}

void Anchor::ProcessIncomingPacketQueue() {
    std::queue<nlohmann::json> packetsToProcess;
    {
        std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
        packetsToProcess.swap(incomingPacketQueue);
    }

    while (!packetsToProcess.empty()) {
        nlohmann::json payload = packetsToProcess.front();
        packetsToProcess.pop();

        std::string packetType = payload.value("type", "");
        isProcessingIncomingPacket = true;

        try {
            // Gameplay packets from a different build could corrupt the save, so only accept them from matching
            // versions. Players on another build are still shown, they just don't share anything.
            if (packetType != ALL_CLIENT_STATE && packetType != UPDATE_CLIENT_STATE && packetType != PLAYER_UPDATE &&
                payload.contains("clientId")) {
                auto it = clients.find(payload["clientId"].get<uint32_t>());
                if (it != clients.end() && it->second.clientVersion != clientVersion) {
                    isProcessingIncomingPacket = false;
                    continue;
                }
            }

            if (packetType == ALL_CLIENT_STATE)
                HandlePacket_AllClientState(payload);
            else if (packetType == ENEMY_HEALTH)
                HandlePacket_EnemyHealth(payload);
            else if (packetType == DISABLE_ANCHOR)
                Disable();
            else if (packetType == GIVE_ITEM)
                HandlePacket_GiveItem(payload);
            else if (packetType == PLAYER_UPDATE)
                HandlePacket_PlayerUpdate(payload);
            else if (packetType == REQUEST_TEAM_STATE)
                HandlePacket_RequestTeamState(payload);
            else if (packetType == REQUEST_TELEPORT)
                HandlePacket_RequestTeleport(payload);
            else if (packetType == SERVER_MESSAGE)
                HandlePacket_ServerMessage(payload);
            else if (packetType == SET_FLAG)
                HandlePacket_SetFlag(payload);
            else if (packetType == SET_SCENE_FLAG)
                HandlePacket_SetSceneFlag(payload);
            else if (packetType == SHARED_UPGRADES)
                HandlePacket_SharedUpgrades(payload);
            else if (packetType == TELEPORT_TO)
                HandlePacket_TeleportTo(payload);
            else if (packetType == UPDATE_CLIENT_STATE)
                HandlePacket_UpdateClientState(payload);
            else if (packetType == UPDATE_ROOM_STATE)
                HandlePacket_UpdateRoomState(payload);
            else if (packetType == UPDATE_TEAM_STATE)
                HandlePacket_UpdateTeamState(payload);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[Anchor] Exception while processing {} packet: {}", packetType, e.what());
        }

        isProcessingIncomingPacket = false;
    }
}

// MARK: - Puppets

struct PuppetClientId {
    uint32_t clientId = 0;
};
static ObjectExtension::Register<PuppetClientId> PuppetClientIdRegister;

uint32_t Anchor::GetPuppetClientId(const Actor* actor) {
    const PuppetClientId* data = ObjectExtension::GetInstance().Get<PuppetClientId>(actor);
    return data != nullptr ? data->clientId : 0;
}

void Anchor::SetPuppetClientId(const Actor* actor, uint32_t clientId) {
    ObjectExtension::GetInstance().Set<PuppetClientId>(actor, PuppetClientId{ clientId });
}

uint32_t Anchor::TakeSpawningPuppetClientId() {
    uint32_t clientId = spawningPuppetForClientId;
    spawningPuppetForClientId = 0;
    return clientId;
}

// Kills every puppet in the scene and spawns a fresh one for each other online client
void Anchor::RefreshClientActors() {
    if (!IsSaveLoaded()) {
        return;
    }

    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].first;
    while (actor != nullptr) {
        if (Puppet_IsPuppet(actor)) {
            Actor_Kill(actor);
        }
        actor = actor->next;
    }

    for (auto& [clientId, client] : clients) {
        client.player = nullptr;
        if (!client.online || client.self || !client.isSaveLoaded) {
            continue;
        }

        spawningPuppetForClientId = clientId;
        client.player =
            (Player*)Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, client.posRot.pos.x,
                                 client.posRot.pos.y, client.posRot.pos.z, 0, client.posRot.rot.y, 0, 0);
        spawningPuppetForClientId = 0;
    }
}

// MARK: - Hooks

static bool sLastSaveLoaded = false;
static s16 sLastSceneId = -1;

void Anchor::RegisterHooks() {
    bool enabled = isEnabled;

    COND_HOOK(OnGameStateMainStart, enabled, []() {
        Anchor* anchor = Anchor::Instance;
        bool saveLoaded = anchor->IsSaveLoaded();
        s16 sceneId = saveLoaded ? gPlayState->sceneId : -1;

        bool connected = anchor->isConnected;
        uint32_t generation = anchor->connectionGeneration;
        if (connected && anchor->handshakeGeneration != generation) {
            // The handshake carries the current scene, so there is nothing else to report yet
            anchor->handshakeGeneration = generation;
            anchor->wasConnected = true;
            anchor->SendPacket_Handshake();
            anchor->justLoadedSave = saveLoaded;
            sLastSaveLoaded = saveLoaded;
            sLastSceneId = sceneId;
        } else if (!connected && anchor->wasConnected) {
            anchor->wasConnected = false;
            for (auto& [clientId, client] : anchor->clients) {
                if (!client.self) {
                    client.online = false;
                }
            }
            anchor->RefreshClientActors();
            Notification::Emit({ .prefix = "Anchor:", .message = "Disconnected" });
        }

        // Before the incoming packets, so an upgrade a teammate sends can't hide one earned here last frame
        anchor->CheckSharedUpgrades();
        anchor->ProcessIncomingPacketQueue();
        anchor->UpdateEnemyHealthScale();

        if (!anchor->isConnected) {
            return;
        }

        if (saveLoaded != sLastSaveLoaded || sceneId != sLastSceneId) {
            if (saveLoaded && !sLastSaveLoaded) {
                anchor->justLoadedSave = true;
            }
            sLastSaveLoaded = saveLoaded;
            sLastSceneId = sceneId;
            anchor->SendPacket_UpdateClientState();

            if (saveLoaded) {
                anchor->shouldRefreshActors = true;
                if (!anchor->justLoadedSave) {
                    // Keep the server's copy of the team save current so a teammate joining later gets it
                    anchor->SendPacket_UpdateTeamState();
                }
            }
        }
    });

    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, enabled, [](Actor* actor, bool* should) {
        uint32_t clientId = Anchor::Instance->TakeSpawningPuppetClientId();
        if (clientId == 0) {
            return;
        }

        Anchor::Instance->SetPuppetClientId(actor, clientId);
        // Move out of the player list immediately rather than at the next frame's deferred category update
        Actor_RemoveFromCategory(gPlayState, &gPlayState->actorCtx, actor);
        Actor_AddToCategory(&gPlayState->actorCtx, actor, ACTORCAT_NPC);
        actor->id = ACTOR_EN_TEST2;
        actor->init = Puppet_Init;
        actor->update = Puppet_Update;
        actor->draw = Puppet_Draw;
        actor->destroy = Puppet_Destroy;
    });

    COND_ID_HOOK(OnActorUpdate, ACTOR_PLAYER, enabled, [](Actor* actor) {
        Anchor* anchor = Anchor::Instance;
        if (!anchor->isConnected || actor != (Actor*)GET_PLAYER(gPlayState)) {
            return;
        }

        if (anchor->justLoadedSave) {
            anchor->justLoadedSave = false;
            anchor->SendPacket_RequestTeamState();
        }

        if (anchor->shouldRefreshActors) {
            anchor->shouldRefreshActors = false;
            anchor->RefreshClientActors();
        }

        anchor->SendPacket_PlayerUpdate();
    });

    COND_HOOK(OnItemGive, enabled, [](u8 item) { Anchor::Instance->SendPacket_GiveItem(item); });

    COND_HOOK(OnFlagSet, enabled, [](FlagType flagType, u32 flag) {
        Anchor::Instance->SendPacket_SetFlag(flagType, flag);
    });

    COND_HOOK(OnSceneFlagSet, enabled, [](s16 sceneId, FlagType flagType, u32 flag) {
        Anchor::Instance->SendPacket_SetSceneFlag(sceneId, flagType, flag);
    });

    COND_HOOK(AfterEndOfCycleSave, enabled, []() { Anchor::Instance->SendPacket_UpdateTeamState(); });

    RegisterEnemyHooks(enabled);
}

static void RegisterAnchor() {
    if (CVarGetInteger(CVAR_ANCHOR("Enabled"), 0)) {
        Anchor::Instance->Enable();
    }
}

static RegisterShipInitFunc initFunc(RegisterAnchor);

#endif
