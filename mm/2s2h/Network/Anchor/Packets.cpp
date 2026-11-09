#ifdef ENABLE_ANCHOR

#include "Anchor.h"
#include "JsonConversions.hpp"
#include "2s2h/BenGui/Notification.h"
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
extern TexturePtr gItemIcons[131];
extern u16 sPersistentCycleWeekEventRegs[ARRAY_COUNT(gSaveContext.save.saveInfo.weekEventReg)];
typedef struct {
    u32 switch0;
    u32 switch1;
    u32 chest;
    u32 collectible;
} AnchorPersistentCycleSceneFlags;
extern AnchorPersistentCycleSceneFlags sPersistentCycleSceneFlags[SCENE_MAX];
}

using json = nlohmann::json;

// MARK: - Client state

json Anchor::PrepClientState() {
    json payload;
    payload["name"] = CVarGetString(CVAR_ANCHOR("Name"), "");
    payload["color"] = Color_RGB8{ 100, 255, 100 };
    payload["clientVersion"] = clientVersion;
    payload["teamId"] = GetTeamId();
    payload["online"] = true;
    payload["seed"] = 0;
    payload["isGameComplete"] = false;

    if (IsSaveLoaded()) {
        payload["isSaveLoaded"] = true;
        payload["sceneId"] = gPlayState->sceneId;
        payload["roomNum"] = gPlayState->roomCtx.curRoom.num;
        payload["entrance"] = gSaveContext.save.entrance;
    } else {
        payload["isSaveLoaded"] = false;
        payload["sceneId"] = SCENE_MAX;
        payload["roomNum"] = -1;
        payload["entrance"] = 0;
    }

    return payload;
}

void Anchor::SendPacket_Handshake() {
    json payload;
    payload["type"] = HANDSHAKE;
    payload["roomId"] = CVarGetString(CVAR_ANCHOR("RoomId"), "");
    payload["roomState"] = PrepRoomState();
    payload["clientState"] = PrepClientState();
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_UpdateClientState() {
    json payload;
    payload["type"] = UPDATE_CLIENT_STATE;
    payload["state"] = PrepClientState();
    SendJsonToRemote(payload);
}

static void CopyClientState(AnchorClient& dest, const AnchorClient& src) {
    dest.clientId = src.clientId;
    dest.name = src.name;
    dest.color = src.color;
    dest.clientVersion = src.clientVersion;
    dest.teamId = src.teamId;
    dest.online = src.online;
    dest.seed = src.seed;
    dest.isSaveLoaded = src.isSaveLoaded;
    dest.isGameComplete = src.isGameComplete;
    dest.sceneId = src.sceneId;
    dest.roomNum = src.roomNum;
    dest.entrance = src.entrance;
}

void Anchor::HandlePacket_AllClientState(json payload) {
    std::vector<AnchorClient> newClients = payload["state"].get<std::vector<AnchorClient>>();

    for (auto& client : newClients) {
        bool known = clients.contains(client.clientId);

        if (client.self) {
            ownClientId = client.clientId;
            CVarSetInteger(CVAR_ANCHOR("LastClientId"), ownClientId);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        } else if ((!known && client.online) || (known && clients[client.clientId].online != client.online)) {
            Notification::Emit({ .prefix = client.name, .message = client.online ? "Connected" : "Disconnected" });
        }

        CopyClientState(clients[client.clientId], client);
        clients[client.clientId].self = client.self;
    }

    std::vector<uint32_t> removed;
    for (auto& [clientId, client] : clients) {
        if (std::none_of(newClients.begin(), newClients.end(),
                         [clientId](const AnchorClient& c) { return c.clientId == clientId; })) {
            removed.push_back(clientId);
        }
    }
    for (uint32_t clientId : removed) {
        clients.erase(clientId);
    }

    shouldRefreshActors = true;
}

void Anchor::HandlePacket_UpdateClientState(json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    auto it = clients.find(clientId);
    if (it == clients.end()) {
        return;
    }

    bool wasLoaded = it->second.isSaveLoaded;
    AnchorClient client = payload["state"].get<AnchorClient>();
    client.clientId = clientId;
    bool self = it->second.self;
    CopyClientState(it->second, client);
    it->second.self = self;

    if (wasLoaded != it->second.isSaveLoaded) {
        shouldRefreshActors = true;
    }
}

// MARK: - Room state

json Anchor::PrepRoomState() {
    json payload;
    payload["ownerClientId"] = ownClientId;
    payload["teleportMode"] = CVarGetInteger(CVAR_ANCHOR("RoomSettings.TeleportMode"), 1);
    payload["syncItemsAndFlags"] = CVarGetInteger(CVAR_ANCHOR("RoomSettings.SyncItemsAndFlags"), 1);
    payload["shareEnemyDamage"] = CVarGetInteger(CVAR_ANCHOR("RoomSettings.ShareEnemyDamage"), 1);
    payload["scaleEnemyHealth"] = CVarGetInteger(CVAR_ANCHOR("RoomSettings.ScaleEnemyHealth"), 1);
    return payload;
}

void Anchor::SendPacket_UpdateRoomState() {
    json payload;
    payload["type"] = UPDATE_ROOM_STATE;
    payload["state"] = PrepRoomState();
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UpdateRoomState(json payload) {
    if (!payload.contains("state")) {
        return;
    }

    const json& state = payload["state"];
    roomState.ownerClientId = state.value("ownerClientId", (uint32_t)0);
    roomState.teleportMode = state.value("teleportMode", (u8)1);
    roomState.syncItemsAndFlags = state.value("syncItemsAndFlags", (u8)1);
    roomState.shareEnemyDamage = state.value("shareEnemyDamage", (u8)1);
    roomState.scaleEnemyHealth = state.value("scaleEnemyHealth", (u8)1);
}

void Anchor::HandlePacket_ServerMessage(json payload) {
    Notification::Emit({
        .prefix = "Server:",
        .prefixColor = ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
        .message = payload.value("message", ""),
    });
}

// MARK: - Player update

void Anchor::SendPacket_PlayerUpdate() {
    if (!IsSaveLoaded()) {
        return;
    }

    std::vector<uint32_t> targets;
    for (auto& [clientId, client] : clients) {
        if (!client.self && client.online && client.isSaveLoaded && client.sceneId == gPlayState->sceneId) {
            targets.push_back(clientId);
        }
    }
    if (targets.empty()) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    json payload;
    payload["type"] = PLAYER_UPDATE;
    payload["quiet"] = true;
    payload["sceneId"] = gPlayState->sceneId;
    payload["form"] = player->transformation;
    payload["posRot"]["pos"] = player->actor.world.pos;
    payload["posRot"]["rot"] = player->actor.shape.rot;

    std::vector<int> joints;
    joints.reserve(PLAYER_LIMB_MAX * 3);
    for (int i = 0; i < PLAYER_LIMB_MAX; i++) {
        joints.push_back(player->skelAnime.jointTable[i].x);
        joints.push_back(player->skelAnime.jointTable[i].y);
        joints.push_back(player->skelAnime.jointTable[i].z);
    }
    payload["jointTable"] = joints;
    payload["appearanceInfo"] = GET_APPEARANCE_FROM_JOINT_TABLE(player->skelAnime.jointTable);
    payload["movementFlags"] = player->skelAnime.movementFlags;
    payload["prevTransl"] = player->skelAnime.prevTransl;
    payload["upperLimbRot"] = player->upperLimbRot;
    payload["stateFlags1"] = player->stateFlags1;
    payload["stateFlags2"] = player->stateFlags2;
    payload["stateFlags3"] = player->stateFlags3;
    payload["currentShield"] = player->currentShield;
    payload["currentBoots"] = player->currentBoots;
    payload["currentMask"] = player->currentMask;
    payload["itemAction"] = player->itemAction;
    payload["heldItemAction"] = player->heldItemAction;
    payload["heldItemId"] = player->heldItemId;
    payload["modelGroup"] = player->modelGroup;
    payload["invincibilityTimer"] = player->invincibilityTimer;
    payload["unk_ABC"] = player->unk_ABC;
    payload["actionVar1"] = player->av1.actionVar1;

    for (uint32_t clientId : targets) {
        payload["targetClientId"] = clientId;
        SendJsonToRemote(payload);
    }
}

void Anchor::HandlePacket_PlayerUpdate(json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    auto it = clients.find(clientId);
    if (it == clients.end()) {
        return;
    }

    AnchorClient& client = it->second;
    u8 form = payload.value("form", (u8)PLAYER_FORM_HUMAN);
    if (form >= PLAYER_FORM_MAX) {
        return;
    }

    // A form change needs a different skeleton, so respawn the puppet
    if (client.form != form || (client.player == nullptr && client.isSaveLoaded)) {
        shouldRefreshActors = true;
    }

    client.form = form;
    client.sceneId = payload.value("sceneId", (s16)SCENE_MAX);
    client.posRot = payload.value("posRot", PosRot{});

    std::vector<int> joints = payload.value("jointTable", std::vector<int>{});
    joints.resize(PLAYER_LIMB_MAX * 3);
    for (int i = 0; i < PLAYER_LIMB_MAX; i++) {
        client.jointTable[i].x = joints[i * 3];
        client.jointTable[i].y = joints[i * 3 + 1];
        client.jointTable[i].z = joints[i * 3 + 2];
    }

    client.appearanceInfo = payload.value("appearanceInfo", (s16)0);
    client.movementFlags = payload.value("movementFlags", (u8)0);
    client.prevTransl = payload.value("prevTransl", Vec3s{});
    client.upperLimbRot = payload.value("upperLimbRot", Vec3s{});
    client.stateFlags1 = payload.value("stateFlags1", (u32)0);
    client.stateFlags2 = payload.value("stateFlags2", (u32)0);
    client.stateFlags3 = payload.value("stateFlags3", (u32)0);
    client.currentShield = payload.value("currentShield", (s8)0);
    client.currentBoots = payload.value("currentBoots", (s8)0);
    client.currentMask = payload.value("currentMask", (u8)0);
    client.itemAction = payload.value("itemAction", (s8)0);
    client.heldItemAction = payload.value("heldItemAction", (s8)0);
    client.heldItemId = payload.value("heldItemId", (u8)ITEM_NONE);
    client.modelGroup = payload.value("modelGroup", (u8)0);
    client.invincibilityTimer = payload.value("invincibilityTimer", (s8)0);
    client.unk_ABC = payload.value("unk_ABC", 0.0f);
    client.actionVar1 = payload.value("actionVar1", (s8)0);
}

// MARK: - Items

// Progress items are shared. Consumables, rupees, bottle contents, cycle trade items and dungeon-specific items stay
// personal, since they either refill freely or only make sense in the receiver's own cycle or dungeon.
static bool ShouldShareItem(u8 item) {
    switch (item) {
        case ITEM_OCARINA_OF_TIME:
        case ITEM_BOW:
        case ITEM_ARROW_FIRE:
        case ITEM_ARROW_ICE:
        case ITEM_ARROW_LIGHT:
        case ITEM_PICTOGRAPH_BOX:
        case ITEM_LENS_OF_TRUTH:
        case ITEM_HOOKSHOT:
        case ITEM_SWORD_GREAT_FAIRY:
        case ITEM_BOTTLE:
        case ITEM_SWORD_KOKIRI:
        case ITEM_SWORD_RAZOR:
        case ITEM_SWORD_GILDED:
        case ITEM_SWORD_DEITY:
        case ITEM_SHIELD_HERO:
        case ITEM_SHIELD_MIRROR:
        case ITEM_QUIVER_30:
        case ITEM_QUIVER_40:
        case ITEM_QUIVER_50:
        case ITEM_BOMB_BAG_20:
        case ITEM_BOMB_BAG_30:
        case ITEM_BOMB_BAG_40:
        case ITEM_WALLET_ADULT:
        case ITEM_WALLET_GIANT:
        case ITEM_REMAINS_ODOLWA:
        case ITEM_REMAINS_GOHT:
        case ITEM_REMAINS_GYORG:
        case ITEM_REMAINS_TWINMOLD:
        case ITEM_BOMBERS_NOTEBOOK:
        case ITEM_HEART_CONTAINER:
        case ITEM_HEART_PIECE:
        case ITEM_HEART_PIECE_2:
        case ITEM_DEKU_STICK_UPGRADE_20:
        case ITEM_DEKU_STICK_UPGRADE_30:
        case ITEM_DEKU_NUT_UPGRADE_30:
        case ITEM_DEKU_NUT_UPGRADE_40:
            return true;
        default:
            break;
    }

    if (item >= ITEM_MASK_DEKU && item <= ITEM_MASK_GIANT) {
        return true;
    }
    if (item >= ITEM_SONG_SONATA && item <= ITEM_SONG_SUN) {
        return true;
    }
    if (item >= ITEM_MAP_POINT_GREAT_BAY && item <= ITEM_MAP_POINT_ZORA_CAPE) {
        return true;
    }
    return false;
}

void Anchor::SendPacket_GiveItem(u8 itemId) {
    if (isProcessingIncomingPacket || !IsSyncing() || !ShouldShareItem(itemId)) {
        return;
    }

    json payload;
    payload["type"] = GIVE_ITEM;
    payload["targetTeamId"] = GetTeamId();
    payload["addToQueue"] = true;
    payload["itemId"] = itemId;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_GiveItem(json payload) {
    if (!IsSyncing()) {
        return;
    }

    u8 itemId = payload.at("itemId").get<u8>();
    if (!ShouldShareItem(itemId)) {
        return;
    }

    // Owned equipment, masks and songs would only be re-given, skip them quietly
    if (Item_CheckObtainability(itemId) == ITEM_NONE && itemId != ITEM_HEART_PIECE && itemId != ITEM_HEART_PIECE_2 &&
        itemId != ITEM_HEART_CONTAINER && itemId != ITEM_BOTTLE) {
        return;
    }

    Item_Give(gPlayState, itemId);

    // Heart pieces normally become a container when their message box closes, which a remote give never shows
    if (EQ_MAX_QUEST_HEART_PIECE_COUNT) {
        RESET_HEART_PIECE_COUNT;
        gSaveContext.save.saveInfo.playerData.healthCapacity += 0x10;
        gSaveContext.save.saveInfo.playerData.health += 0x10;
    }

    std::string name = "A teammate";
    auto it = clients.find(payload.value("clientId", (uint32_t)0));
    if (it != clients.end()) {
        name = it->second.name;
    }
    Notification::Emit({
        .itemIcon = itemId < ARRAY_COUNT(gItemIcons) ? (const char*)gItemIcons[itemId] : nullptr,
        .prefix = name,
        .message = "shared an item",
    });
}

// MARK: - Flags

static bool IsPersistentWeekEventReg(u32 flag) {
    u32 index = flag >> 8;
    u8 mask = flag & 0xFF;
    if (index >= ARRAY_COUNT(sPersistentCycleWeekEventRegs) || mask == 0) {
        return false;
    }
    return (sPersistentCycleWeekEventRegs[index] >> (2 * BIT_FLAG_TO_SHIFT(mask))) & 3;
}

void Anchor::SendPacket_SetFlag(s16 flagType, u32 flag) {
    if (isProcessingIncomingPacket || !IsSyncing()) {
        return;
    }

    // Only progress that survives the Song of Time is shared; per-cycle events stay with each player's own cycle
    if (flagType != FLAG_WEEK_EVENT_REG || !IsPersistentWeekEventReg(flag)) {
        return;
    }

    json payload;
    payload["type"] = SET_FLAG;
    payload["targetTeamId"] = GetTeamId();
    payload["addToQueue"] = true;
    payload["flagType"] = flagType;
    payload["flag"] = flag;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SetFlag(json payload) {
    if (!IsSyncing()) {
        return;
    }

    s16 flagType = payload.at("flagType").get<s16>();
    u32 flag = payload.at("flag").get<u32>();
    if (flagType == FLAG_WEEK_EVENT_REG && IsPersistentWeekEventReg(flag)) {
        SET_WEEKEVENTREG(flag);
    }
}

static u32 PersistentSceneMask(s16 sceneId, s16 flagType, u32 flag) {
    if (sceneId < 0 || sceneId >= SCENE_MAX) {
        return 0;
    }
    const AnchorPersistentCycleSceneFlags& persistent = sPersistentCycleSceneFlags[sceneId];
    switch (flagType) {
        case FLAG_CYCL_SCENE_SWITCH:
            if (flag < 32) {
                return persistent.switch0 & (1u << flag);
            } else if (flag < 64) {
                return persistent.switch1 & (1u << (flag - 32));
            }
            return 0;
        case FLAG_CYCL_SCENE_CHEST:
            return flag < 32 ? persistent.chest & (1u << flag) : 0;
        case FLAG_CYCL_SCENE_COLLECTIBLE:
            return flag < 32 ? persistent.collectible & (1u << flag) : 0;
        default:
            return 0;
    }
}

void Anchor::SendPacket_SetSceneFlag(s16 sceneId, s16 flagType, u32 flag) {
    if (isProcessingIncomingPacket || !IsSyncing()) {
        return;
    }

    s16 originalSceneId = Play_GetOriginalSceneId(sceneId);
    if (PersistentSceneMask(originalSceneId, flagType, flag) == 0) {
        return;
    }

    json payload;
    payload["type"] = SET_SCENE_FLAG;
    payload["targetTeamId"] = GetTeamId();
    payload["addToQueue"] = true;
    payload["sceneId"] = originalSceneId;
    payload["flagType"] = flagType;
    payload["flag"] = flag;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SetSceneFlag(json payload) {
    if (!IsSyncing()) {
        return;
    }

    s16 sceneId = payload.at("sceneId").get<s16>();
    s16 flagType = payload.at("flagType").get<s16>();
    u32 flag = payload.at("flag").get<u32>();
    if (PersistentSceneMask(sceneId, flagType, flag) == 0) {
        return;
    }

    // The current scene's flags live in the actor context until the scene is left
    if (Play_GetOriginalSceneId(gPlayState->sceneId) == sceneId) {
        switch (flagType) {
            case FLAG_CYCL_SCENE_SWITCH:
                Flags_SetSwitch(gPlayState, flag);
                break;
            case FLAG_CYCL_SCENE_CHEST:
                Flags_SetTreasure(gPlayState, flag);
                break;
            case FLAG_CYCL_SCENE_COLLECTIBLE:
                Flags_SetCollectible(gPlayState, flag);
                break;
        }
        return;
    }

    CycleSceneFlags& flags = gSaveContext.cycleSceneFlags[sceneId];
    switch (flagType) {
        case FLAG_CYCL_SCENE_SWITCH:
            if (flag < 32) {
                flags.switch0 |= (1u << flag);
            } else {
                flags.switch1 |= (1u << (flag - 32));
            }
            break;
        case FLAG_CYCL_SCENE_CHEST:
            flags.chest |= (1u << flag);
            break;
        case FLAG_CYCL_SCENE_COLLECTIBLE:
            flags.collectible |= (1u << flag);
            break;
    }
}

// MARK: - Team state

/**
 * The team state is the shared progress a teammate needs when they join later: owned items, upgrades, quest status,
 * health and magic, owl statues, and the weekEventReg and scene flags that persist across the Song of Time. Applying
 * it only ever adds progress, so a teammate who is ahead never loses anything.
 */
json Anchor::PrepTeamState() {
    SaveInfo& saveInfo = gSaveContext.save.saveInfo;

    // The current scene's flags are only copied to the save context when the scene is left
    Play_SaveCycleSceneFlags(gPlayState);

    json state;
    state["inventory"] = saveInfo.inventory;
    state["healthCapacity"] = saveInfo.playerData.healthCapacity;
    state["magicLevel"] = saveInfo.playerData.magicLevel;
    state["isMagicAcquired"] = saveInfo.playerData.isMagicAcquired;
    state["isDoubleMagicAcquired"] = saveInfo.playerData.isDoubleMagicAcquired;
    state["doubleDefense"] = saveInfo.playerData.doubleDefense;
    state["owlActivationFlags"] = saveInfo.playerData.owlActivationFlags;

    std::vector<u8> weekEventReg(ARRAY_COUNT(saveInfo.weekEventReg));
    for (size_t i = 0; i < weekEventReg.size(); i++) {
        u8 persistentMask = 0;
        for (int bit = 0; bit < 8; bit++) {
            if ((sPersistentCycleWeekEventRegs[i] >> (2 * bit)) & 3) {
                persistentMask |= (1 << bit);
            }
        }
        weekEventReg[i] = saveInfo.weekEventReg[i] & persistentMask;
    }
    state["weekEventReg"] = weekEventReg;

    json sceneFlags = json::array();
    for (int i = 0; i < SCENE_MAX; i++) {
        const CycleSceneFlags& flags = gSaveContext.cycleSceneFlags[i];
        const AnchorPersistentCycleSceneFlags& persistent = sPersistentCycleSceneFlags[i];
        sceneFlags.push_back({ flags.switch0 & persistent.switch0, flags.switch1 & persistent.switch1,
                               flags.chest & persistent.chest, flags.collectible & persistent.collectible });
    }
    state["sceneFlags"] = sceneFlags;

    return state;
}

void Anchor::ApplyTeamState(const json& state) {
    SaveInfo& saveInfo = gSaveContext.save.saveInfo;
    Inventory incoming = state.at("inventory").get<Inventory>();
    Inventory& inventory = saveInfo.inventory;

    for (int slot = 0; slot < ARRAY_COUNT(inventory.items); slot++) {
        u8 item = incoming.items[slot];
        if (item == ITEM_NONE || inventory.items[slot] != ITEM_NONE) {
            continue;
        }
        if (slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_6) {
            // Share the bottle, not what's in it
            inventory.items[slot] = ITEM_BOTTLE;
        } else if (slot == SLOT_TRADE_DEED || slot == SLOT_TRADE_KEY_MAMA || slot == SLOT_TRADE_COUPLE ||
                   slot == SLOT_POWDER_KEG || slot == SLOT_MAGIC_BEANS) {
            // Per-cycle trade items and consumables stay with each player
            continue;
        } else {
            inventory.items[slot] = item;
        }
    }

    // Upgrades are bit fields of capacity levels; take the larger level of each
    for (int type = 0; type < ARRAY_COUNT(gUpgradeMasks); type++) {
        u32 shift = gUpgradeShifts[type];
        u32 mask = gUpgradeMasks[type];
        u32 mine = (inventory.upgrades & mask) >> shift;
        u32 theirs = (incoming.upgrades & mask) >> shift;
        if (theirs > mine) {
            inventory.upgrades = (inventory.upgrades & ~mask) | (theirs << shift);
        }
    }

    // Quest items are flags, except the heart piece count in the top nibble
    u32 heartPieceMask = 0xF0000000;
    s16 incomingCapacity = state.at("healthCapacity").get<s16>();
    u32 myPieces = inventory.questItems & heartPieceMask;
    u32 theirPieces = incoming.questItems & heartPieceMask;
    inventory.questItems = ((inventory.questItems | incoming.questItems) & ~heartPieceMask);
    if (incomingCapacity > saveInfo.playerData.healthCapacity) {
        saveInfo.playerData.healthCapacity = incomingCapacity;
        inventory.questItems |= theirPieces;
    } else if (incomingCapacity == saveInfo.playerData.healthCapacity) {
        inventory.questItems |= std::max(myPieces, theirPieces);
    } else {
        inventory.questItems |= myPieces;
    }

    if (state.value("isMagicAcquired", (u8)0)) {
        saveInfo.playerData.isMagicAcquired = true;
    }
    if (state.value("isDoubleMagicAcquired", (u8)0)) {
        saveInfo.playerData.isDoubleMagicAcquired = true;
    }
    saveInfo.playerData.magicLevel = std::max(saveInfo.playerData.magicLevel, state.value("magicLevel", (s8)0));
    if (state.value("doubleDefense", (u8)0)) {
        saveInfo.playerData.doubleDefense = true;
        inventory.defenseHearts = 20;
    }
    saveInfo.playerData.owlActivationFlags |= state.value("owlActivationFlags", (u16)0);

    std::vector<u8> weekEventReg = state.value("weekEventReg", std::vector<u8>{});
    for (size_t i = 0; i < weekEventReg.size() && i < ARRAY_COUNT(saveInfo.weekEventReg); i++) {
        saveInfo.weekEventReg[i] |= weekEventReg[i];
    }

    const json& sceneFlags = state.at("sceneFlags");
    s16 currentScene = IsSaveLoaded() ? Play_GetOriginalSceneId(gPlayState->sceneId) : -1;
    for (int i = 0; i < SCENE_MAX && i < (int)sceneFlags.size(); i++) {
        const json& f = sceneFlags[i];
        CycleSceneFlags& flags = gSaveContext.cycleSceneFlags[i];
        flags.switch0 |= f[0].get<u32>();
        flags.switch1 |= f[1].get<u32>();
        flags.chest |= f[2].get<u32>();
        flags.collectible |= f[3].get<u32>();

        if (i == currentScene) {
            gPlayState->actorCtx.sceneFlags.switches[0] |= f[0].get<u32>();
            gPlayState->actorCtx.sceneFlags.switches[1] |= f[1].get<u32>();
            gPlayState->actorCtx.sceneFlags.chest |= f[2].get<u32>();
            gPlayState->actorCtx.sceneFlags.collectible[0] |= f[3].get<u32>();
        }
    }
}

void Anchor::SendPacket_UpdateTeamState() {
    if (!IsSyncing()) {
        return;
    }

    json payload;
    payload["type"] = UPDATE_TEAM_STATE;
    payload["targetTeamId"] = GetTeamId();
    payload["queue"] = json::array();
    payload["state"] = PrepTeamState();
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_RequestTeamState() {
    if (!IsSyncing()) {
        return;
    }

    json payload;
    payload["type"] = REQUEST_TEAM_STATE;
    payload["targetTeamId"] = GetTeamId();
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RequestTeamState(json payload) {
    SendPacket_UpdateTeamState();
}

void Anchor::HandlePacket_UpdateTeamState(json payload) {
    if (!IsSyncing()) {
        return;
    }

    if (payload.contains("state") && payload["state"].contains("inventory")) {
        ApplyTeamState(payload["state"]);
        Notification::Emit({ .message = "Progress synced from team" });
    }

    // Packets sent while this client was offline, replayed in order
    if (payload.contains("queue")) {
        std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
        for (auto& item : payload["queue"]) {
            incomingPacketQueue.push(json::parse(item.get<std::string>()));
        }
    }
}

// MARK: - Teleport

bool Anchor::CanTeleportTo(uint32_t clientId) {
    if (roomState.teleportMode == 0 || !IsSaveLoaded()) {
        return false;
    }

    auto it = clients.find(clientId);
    if (it == clients.end()) {
        return false;
    }

    const AnchorClient& client = it->second;
    if (client.self || !client.online || !client.isSaveLoaded || client.sceneId == SCENE_MAX) {
        return false;
    }

    return roomState.teleportMode == 2 || client.teamId == GetTeamId();
}

void Anchor::SendPacket_RequestTeleport(uint32_t clientId) {
    if (!CanTeleportTo(clientId)) {
        return;
    }

    json payload;
    payload["type"] = REQUEST_TELEPORT;
    payload["targetClientId"] = clientId;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RequestTeleport(json payload) {
    if (!IsSaveLoaded()) {
        return;
    }
    SendPacket_TeleportTo(payload.at("clientId").get<uint32_t>());
}

void Anchor::SendPacket_TeleportTo(uint32_t clientId) {
    if (!IsSaveLoaded()) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    json payload;
    payload["type"] = TELEPORT_TO;
    payload["targetClientId"] = clientId;
    payload["entrance"] = gSaveContext.save.entrance;
    payload["roomIndex"] = gPlayState->roomCtx.curRoom.num;
    payload["posRot"]["pos"] = player->actor.world.pos;
    payload["posRot"]["rot"] = player->actor.shape.rot;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_TeleportTo(json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    s32 entrance = payload.at("entrance").get<s32>();
    s8 roomIndex = payload.at("roomIndex").get<s8>();
    if (entrance < 0 || roomIndex < 0) {
        return;
    }
    PosRot posRot = payload.at("posRot").get<PosRot>();

    // Same warp as the WarpPoint dev tool: reload at the entrance, then respawn at the teammate's position
    gPlayState->nextEntrance = entrance;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entrance = entrance;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = roomIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = posRot.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = posRot.rot.y;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = PLAYER_PARAMS(0xFF, PLAYER_START_MODE_D);
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.respawnFlag = -8;
}

#endif
