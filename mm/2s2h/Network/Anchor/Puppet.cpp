#ifdef ENABLE_ANCHOR

#include "Anchor.h"
#include "Puppet.h"
#include "2s2h/NameTag/NameTag.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "z64effect.h"
#include "z64malloc.h"
void ResourceMgr_UnregisterSkeleton(SkelAnime* skelAnime);
extern PlayState* gPlayState;
extern PlayerAgeProperties sPlayerAgeProperties[PLAYER_FORM_MAX];
void Player_Draw(Actor* thisx, PlayState* play);
}

/**
 * A puppet is a Player instance driven by another client's PLAYER_UPDATE packets instead of input. It is spawned as
 * ACTOR_PLAYER (so the engine sizes and links it like a player) and converted in the ShouldActorInit hook: moved to
 * the NPC category, given an unused actor id so no game code mistakes it for the real player, and given these
 * init/update/draw/destroy functions. Modeled after Kafei (EnTest3), which is the vanilla Player-based NPC.
 */

static EffectBlureInit2 sPuppetBlureInit = {
    0,
    EFFECT_BLURE_ELEMENT_FLAG_8,
    0,
    { 255, 255, 255, 255 },
    { 255, 255, 255, 64 },
    { 255, 255, 255, 0 },
    { 255, 255, 255, 0 },
    4,
    0,
    EFF_BLURE_DRAW_MODE_SMOOTH,
    0,
    { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },
};

static AnchorClient* Puppet_GetClient(Actor* actor) {
    uint32_t clientId = Anchor::Instance->GetPuppetClientId(actor);
    auto it = Anchor::Instance->clients.find(clientId);
    if (it == Anchor::Instance->clients.end()) {
        return nullptr;
    }
    return &it->second;
}

static bool Puppet_IsVisible(const AnchorClient* client) {
    return client->online && client->isSaveLoaded && gPlayState != nullptr && client->sceneId == gPlayState->sceneId;
}

void Puppet_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;
    AnchorClient* client = Puppet_GetClient(actor);

    if (client == nullptr || client->form >= PLAYER_FORM_MAX) {
        Actor_Kill(actor);
        return;
    }

    Player* realPlayer = GET_PLAYER(play);

    player->actor.room = -1;
    player->csId = CS_ID_NONE;
    player->transformation = client->form;
    player->currentMask = (client->form == PLAYER_FORM_HUMAN) ? PLAYER_MASK_NONE
                                                            : (u8)(client->form + PLAYER_MASK_FIERCE_DEITY);
    player->ageProperties = &sPlayerAgeProperties[client->form];
    player->itemAction = PLAYER_IA_NONE;
    player->heldItemAction = PLAYER_IA_NONE;
    player->heldItemId = ITEM_NONE;

    Player_SetModelGroup(player, PLAYER_MODELGROUP_DEFAULT);
    play->playerInit(player, play, gPlayerSkeletons[client->form]);

    Effect_Add(play, &player->meleeWeaponEffectIndex[0], EFFECT_BLURE2, 0, 0, &sPuppetBlureInit);
    Effect_Add(play, &player->meleeWeaponEffectIndex[1], EFFECT_BLURE2, 0, 0, &sPuppetBlureInit);

    player->maskObjectSegment = ZeldaArena_Malloc(0x3800);
    if (realPlayer != nullptr) {
        player->giObjectSegment = realPlayer->giObjectSegment;
        player->tatlActor = realPlayer->tatlActor;
    }

    // Not targetable and no collision: a puppet is only a visual stand-in for the other player
    player->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    player->actor.flags |= ACTOR_FLAG_LOCK_ON_DISABLED | ACTOR_FLAG_UPDATE_CULLING_DISABLED |
                         ACTOR_FLAG_DRAW_CULLING_DISABLED;

    NameTag_RegisterForActorWithOptions(actor, client->name.c_str(), {});
}

void Puppet_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;
    AnchorClient* client = Puppet_GetClient(actor);

    if (client == nullptr) {
        Actor_Kill(actor);
        return;
    }

    if (!Puppet_IsVisible(client)) {
        actor->world.pos = { -9999.0f, -9999.0f, -9999.0f };
        actor->shape.shadowAlpha = 0;
        return;
    }

    actor->shape.shadowAlpha = 255;
    actor->world.pos = client->posRot.pos;
    actor->shape.rot = client->posRot.rot;
    actor->world.rot = client->posRot.rot;

    for (int i = 0; i < PLAYER_LIMB_MAX; i++) {
        player->skelAnime.jointTable[i] = client->jointTable[i];
    }
    GET_APPEARANCE_FROM_JOINT_TABLE(player->skelAnime.jointTable) = client->appearanceInfo;

    player->skelAnime.movementFlags = client->movementFlags;
    player->skelAnime.prevTransl = client->prevTransl;
    player->upperLimbRot = client->upperLimbRot;
    player->stateFlags1 = client->stateFlags1;
    player->stateFlags2 = client->stateFlags2;
    player->stateFlags3 = client->stateFlags3;
    player->currentShield = client->currentShield;
    player->currentBoots = client->currentBoots;
    player->itemAction = client->itemAction;
    player->heldItemAction = client->heldItemAction;
    player->heldItemId = client->heldItemId;
    player->invincibilityTimer = client->invincibilityTimer;
    player->unk_ABC = client->unk_ABC;
    player->av1.actionVar1 = client->actionVar1;

    if (player->currentMask != client->currentMask) {
        player->currentMask = client->currentMask;
    }
    // Advances the mask "load" state machine so the current mask is drawn
    func_801229FC(player);

    if (player->modelGroup != client->modelGroup) {
        Player_SetModelGroup(player, (PlayerModelGroup)client->modelGroup);
    }

    // Apply animation root motion the same way the real player does
    Vec3f diff;
    SkelAnime_UpdateTranslation(&player->skelAnime, &diff, player->actor.shape.rot.y);
    if (player->skelAnime.movementFlags & ANIM_FLAG_1) {
        actor->world.pos.x += diff.x * player->ageProperties->unk_08 * actor->scale.x;
        actor->world.pos.z += diff.z * player->ageProperties->unk_08 * actor->scale.z;
    }
    if (player->skelAnime.movementFlags & ANIM_FLAG_UPDATE_Y) {
        if (!(player->skelAnime.movementFlags & ANIM_FLAG_4)) {
            diff.y *= player->ageProperties->unk_08;
        }
        actor->world.pos.y += diff.y * actor->scale.y;
    }

    Actor_SetFocus(actor, player->ageProperties->ceilingCheckHeight);
}

void Puppet_Draw(Actor* actor, PlayState* play) {
    AnchorClient* client = Puppet_GetClient(actor);

    if (client == nullptr || !Puppet_IsVisible(client)) {
        return;
    }

    Player_Draw(actor, play);
}

void Puppet_Destroy(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    // Spawned as ACTOR_PLAYER: restore the id so the overlay's loaded count is decremented correctly
    actor->id = ACTOR_PLAYER;

    NameTag_RemoveAllForActor(actor);
    ResourceMgr_UnregisterSkeleton(&player->skelAnime);
    ResourceMgr_UnregisterSkeleton(&player->skelAnimeUpper);
    Effect_Destroy(play, player->meleeWeaponEffectIndex[0]);
    Effect_Destroy(play, player->meleeWeaponEffectIndex[1]);
    Effect_Destroy(play, player->meleeWeaponEffectIndex[2]);
    Collider_DestroyCylinder(play, &player->cylinder);
    Collider_DestroyCylinder(play, &player->shieldCylinder);
    Collider_DestroyQuad(play, &player->meleeWeaponQuads[0]);
    Collider_DestroyQuad(play, &player->meleeWeaponQuads[1]);
    Collider_DestroyQuad(play, &player->shieldQuad);
    ZeldaArena_Free(player->maskObjectSegment);

    AnchorClient* client = Puppet_GetClient(actor);
    if (client != nullptr && client->player == player) {
        client->player = nullptr;
    }
}

bool Puppet_IsPuppet(Actor* actor) {
    return actor->update == Puppet_Update;
}

#endif
