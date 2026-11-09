#ifdef ENABLE_ANCHOR

#include "Anchor.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ObjectExtension/ObjectExtension.h"
#include "2s2h/ObjectExtension/ActorListIndex.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "z64effect_ss.h"
extern PlayState* gPlayState;
}

using json = nlohmann::json;

/**
 * Shared enemies: every enemy is matched across games by where it came from (its room and slot in the room's actor
 * list, or its id/params/home position when it was spawned at runtime). Its health is multiplied by the number of
 * players in the scene, and any health it loses in one game is mirrored in the others, so players fight the same enemy.
 */

struct EnemySyncData {
    uint32_t baseKey = 0;
    uint8_t ordinal = 0;
    uint8_t lastHealth = 0;
    uint8_t scale = 1;
};
static ObjectExtension::Register<EnemySyncData> EnemySyncDataRegister;

static uint8_t sCurrentScale = 1;

static uint32_t HashCombine(uint32_t hash, int32_t value) {
    hash ^= (uint32_t)value + 0x9E3779B9 + (hash << 6) + (hash >> 2);
    return hash;
}

static bool IsSyncedEnemy(Actor* actor) {
    // The shooting gallery Wolfos is a reused target that only ever expects its own two health
    if (actor->id == ACTOR_EN_SYATEKI_WF) {
        return false;
    }
    return actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS;
}

/**
 * A remote kill removes the enemy without running its death code. That is wrong for enemies whose death sets a flag,
 * plays a cutscene, spawns something needed, or that other actors keep pointing at, and for any enemy currently
 * holding the player. Those are left at 1 health for a local hit to finish.
 */
static bool CanKillRemotely(Actor* actor) {
    if (actor->category == ACTORCAT_BOSS) {
        return false;
    }

    Player* player = GET_PLAYER(gPlayState);
    if (player->actor.parent == actor || (player->csActor == actor && player->csAction != PLAYER_CSACTION_NONE) ||
        (player->stateFlags2 & PLAYER_STATE2_80)) {
        return false;
    }
    if (actor->csId != CS_ID_NONE && CutsceneManager_GetCurrentCsId() == actor->csId) {
        return false;
    }

    switch (actor->id) {
        // Flags, cutscenes or rewards on death
        case ACTOR_EN_BIGPO:
        case ACTOR_EN_BSB:
        case ACTOR_EN_DEATH:
        case ACTOR_EN_DINOFOS:
        case ACTOR_EN_DRAGON:
        case ACTOR_EN_EGOL:
        case ACTOR_EN_IK:
        case ACTOR_EN_JSO2:
        case ACTOR_EN_KAIZOKU:
        case ACTOR_EN_RD:
        case ACTOR_EN_TALK_GIBUD:
        case ACTOR_EN_THIEFBIRD:
        case ACTOR_EN_WIZ:
        // Leaves something behind that the area needs
        case ACTOR_BOSS_05:
        case ACTOR_EN_DEKUNUTS:
        case ACTOR_EN_RAIL_SKB:
        case ACTOR_EN_RR:
        // Linked to other actors that don't check whether it is still alive
        case ACTOR_BOSS_07:
        case ACTOR_EN_FLOORMAS:
        case ACTOR_EN_PO_SISTERS:
        case ACTOR_EN_PR:
        case ACTOR_EN_SNOWMAN:
            return false;
        // The Graveyard's bats open the way on the second night
        case ACTOR_EN_BAT:
            return actor->room != -1;
        // Skulltulas that set a switch flag
        case ACTOR_EN_ST:
            return (actor->params & 0x3F) == 0x3F;
        // Gold Skulltulas drop their token
        case ACTOR_EN_SW:
            return (actor->params & 3) == 0;
        default:
            return true;
    }
}

static uint32_t GetBaseKey(Actor* actor) {
    uint32_t hash = HashCombine(0, actor->id);
    s16 listIndex = GetActorListIndex(actor);

    if (listIndex >= 0) {
        hash = HashCombine(hash, actor->room);
        hash = HashCombine(hash, listIndex);
    } else {
        hash = HashCombine(hash, actor->params);
        hash = HashCombine(hash, (int32_t)actor->home.pos.x);
        hash = HashCombine(hash, (int32_t)actor->home.pos.y);
        hash = HashCombine(hash, (int32_t)actor->home.pos.z);
        hash = HashCombine(hash, actor->home.rot.y);
    }

    return hash;
}

template <typename F> static void ForEachSyncedEnemy(F&& func) {
    for (u8 category : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[category].first;
        while (actor != nullptr) {
            EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
            if (data != nullptr) {
                func(actor, data);
            }
            actor = actor->next;
        }
    }
}

static Actor* FindEnemy(uint32_t baseKey, uint8_t ordinal) {
    Actor* found = nullptr;
    ForEachSyncedEnemy([&](Actor* actor, EnemySyncData* data) {
        if (found == nullptr && actor->update != nullptr && data->baseKey == baseKey && data->ordinal == ordinal) {
            found = actor;
        }
    });
    return found;
}

static uint8_t ScaleHealth(uint8_t health, uint8_t fromScale, uint8_t toScale) {
    if (health == 0 || fromScale == toScale) {
        return health;
    }
    int scaled = (int)std::lround((float)health * toScale / fromScale);
    return (uint8_t)std::clamp(scaled, 1, 255);
}

uint8_t Anchor::GetPlayersInScene() {
    if (!IsSaveLoaded()) {
        return 1;
    }

    uint8_t count = 1;
    for (auto& [clientId, client] : clients) {
        if (!client.self && client.online && client.isSaveLoaded && client.sceneId == gPlayState->sceneId) {
            count++;
        }
    }
    return count;
}

uint8_t Anchor::GetEnemyHealthScale() {
    if (!isConnected || !roomState.scaleEnemyHealth) {
        return 1;
    }
    return GetPlayersInScene();
}

// Rescales enemies already in the scene when a player arrives or leaves
void Anchor::UpdateEnemyHealthScale() {
    uint8_t scale = IsSaveLoaded() ? GetEnemyHealthScale() : 1;
    if (scale == sCurrentScale) {
        return;
    }
    sCurrentScale = scale;

    if (!IsSaveLoaded()) {
        return;
    }

    ForEachSyncedEnemy([&](Actor* actor, EnemySyncData* data) {
        actor->colChkInfo.health = ScaleHealth(actor->colChkInfo.health, data->scale, scale);
        data->scale = scale;
        data->lastHealth = actor->colChkInfo.health;
    });
}

void Anchor::SendPacket_EnemyHealth(Actor* actor, uint8_t baseKeyOrdinal, uint32_t baseKey) {
    std::vector<uint32_t> targets;
    for (auto& [clientId, client] : clients) {
        if (!client.self && client.online && client.isSaveLoaded && client.sceneId == gPlayState->sceneId) {
            targets.push_back(clientId);
        }
    }
    if (targets.empty()) {
        return;
    }

    json payload;
    payload["type"] = ENEMY_HEALTH;
    payload["quiet"] = true;
    payload["sceneId"] = gPlayState->sceneId;
    payload["key"] = baseKey;
    payload["ordinal"] = baseKeyOrdinal;
    payload["actorId"] = actor->id;
    payload["health"] = actor->colChkInfo.health;

    for (uint32_t clientId : targets) {
        payload["targetClientId"] = clientId;
        SendJsonToRemote(payload);
    }
}

void Anchor::HandlePacket_EnemyHealth(json payload) {
    if (!roomState.shareEnemyDamage || !IsSaveLoaded() || payload.value("sceneId", (s16)-1) != gPlayState->sceneId) {
        return;
    }

    Actor* actor = FindEnemy(payload.value("key", (uint32_t)0), payload.value("ordinal", (uint8_t)0));
    if (actor == nullptr || actor->id != payload.value("actorId", (s16)-1)) {
        return;
    }

    EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
    uint8_t health = payload.value("health", (uint8_t)255);
    if (health >= actor->colChkInfo.health) {
        return;
    }

    if (health == 0 && CanKillRemotely(actor)) {
        func_800B3030(gPlayState, &actor->world.pos, &gZeroVec3f, &gZeroVec3f, 100, 0, 0);
        SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->world.pos, 11, NA_SE_EN_EXTINCT);
        actor->colChkInfo.health = 0;
        data->lastHealth = 0;
        Actor_Kill(actor);
        return;
    }

    // Leave at least 1 so the enemy's own death code still runs on the next local hit
    actor->colChkInfo.health = std::max<uint8_t>(health, 1);
    data->lastHealth = actor->colChkInfo.health;
    Actor_SetColorFilter(actor, COLORFILTER_COLORFLAG_RED, 255, COLORFILTER_BUFFLAG_OPA, 8);
}

static void TrackEnemy(Actor* actor) {
    uint32_t baseKey = GetBaseKey(actor);
    uint8_t ordinal = 0;
    ForEachSyncedEnemy([&](Actor* other, EnemySyncData* otherData) {
        if (other != actor && otherData->baseKey == baseKey) {
            ordinal = std::max<uint8_t>(ordinal, otherData->ordinal + 1);
        }
    });

    uint8_t scale = Anchor::Instance->GetEnemyHealthScale();
    actor->colChkInfo.health = ScaleHealth(actor->colChkInfo.health, 1, scale);

    ObjectExtension::GetInstance().Set<EnemySyncData>(actor, EnemySyncData{
                                                                 .baseKey = baseKey,
                                                                 .ordinal = ordinal,
                                                                 .lastHealth = actor->colChkInfo.health,
                                                                 .scale = scale,
                                                             });
}

void Anchor::RegisterEnemyHooks(bool enabled) {
    COND_HOOK(OnActorInit, enabled, [](Actor* actor) {
        if (!IsSyncedEnemy(actor) || actor->update == nullptr || !Anchor::Instance->IsSaveLoaded()) {
            return;
        }
        TrackEnemy(actor);
    });

    // Connecting mid-scene: pick up the enemies that spawned before the hooks existed, in spawn order
    if (enabled && IsSaveLoaded()) {
        for (u8 category : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
            for (Actor* actor = gPlayState->actorCtx.actorLists[category].first; actor != nullptr;
                 actor = actor->next) {
                if (IsSyncedEnemy(actor) && actor->update != nullptr &&
                    ObjectExtension::GetInstance().Get<EnemySyncData>(actor) == nullptr) {
                    TrackEnemy(actor);
                }
            }
        }
    }

    COND_HOOK(OnActorUpdate, enabled, [](Actor* actor) {
        if (!IsSyncedEnemy(actor)) {
            return;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || actor->colChkInfo.health == data->lastHealth) {
            return;
        }

        bool tookDamage = actor->colChkInfo.health < data->lastHealth;
        data->lastHealth = actor->colChkInfo.health;

        Anchor* anchor = Anchor::Instance;
        if (tookDamage && anchor->isConnected && anchor->roomState.shareEnemyDamage) {
            anchor->SendPacket_EnemyHealth(actor, data->ordinal, data->baseKey);
        }
    });
}

#endif
