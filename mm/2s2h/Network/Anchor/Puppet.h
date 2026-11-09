#pragma once

#if defined(ENABLE_ANCHOR) && defined(__cplusplus)

extern "C" {
#include "z64.h"
}

void Puppet_Init(Actor* actor, PlayState* play);
void Puppet_Update(Actor* actor, PlayState* play);
void Puppet_Draw(Actor* actor, PlayState* play);
void Puppet_Destroy(Actor* actor, PlayState* play);
bool Puppet_IsPuppet(Actor* actor);

#endif
