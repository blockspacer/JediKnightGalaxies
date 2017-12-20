#include "jkg_damageareas.h"
#include "g_local.h"
#include "qcommon/q_shared.h"

static std::vector<damageArea_t> damageAreas;

static float CalculateDamageRadius ( const damageArea_t *area )
{
    int t = level.time - area->startTime;
    const radiusParams_t *dmgParams = &area->data->radiusParams;
    float stop = 0.0f;
    
    switch ( dmgParams->radiusFunc )
    {
        case RF_CONSTANT:
            return dmgParams->startRadius;
        break;
        
        case RF_LINEAR:
            return dmgParams->startRadius + ((float)t / (float)area->data->lifetime) * (dmgParams->endRadius - dmgParams->startRadius);
        break;
        
        case RF_NONLINEAR:
            stop = (float)area->data->lifetime * (dmgParams->generic1 * 0.01f);
            if ( t <= stop )
            {
                return dmgParams->startRadius;
            }
            else
            {
                t -= stop;
                return dmgParams->startRadius + ((float)t / (float)(area->data->lifetime - stop)) * (dmgParams->endRadius - dmgParams->startRadius);
            }
        break;
        
        case RF_WAVE:
            // FIXME: I R BORKED.
            stop = sin ((double)(t * dmgParams->generic1));
            stop = stop * 0.5f + 0.5f;
            return dmgParams->startRadius + stop * (dmgParams->endRadius - dmgParams->startRadius);
        break;
        
        case RF_CLAMP:
            stop = (float)area->data->lifetime * (dmgParams->generic1 * 0.01f);
            if ( t >= stop )
            {
                return dmgParams->endRadius;
            }
            else
            {
                return dmgParams->startRadius + ((float)t / stop) * (dmgParams->endRadius - dmgParams->startRadius);
            }
        break;
    }
    
    return 0.0f;
}

static float gaussian ( float x )
{
    // mean = 0, sigma squared = 0.5
    return expf (-2.0f * x * x) / sqrt (0.5f * M_PI);
}

static void SmallestVectorToBBox ( vec3_t out, const vec3_t position, const vec3_t absmin, const vec3_t absmax )
{
    int k;
    for ( k = 0; k < 3; k++ )
    {
        if ( position[k] < absmin[k] ) {
	        out[k] = absmin[k] - position[k];
        } else if ( position[k] > absmax[k] ) {
	        out[k] = position[k] - absmax[k];
        } else {
	        out[k] = 0.0f;
        }
    }
}

static int CalculateDamageForDistance ( const damageArea_t *area, const vec3_t playerMins, const vec3_t playerMaxs, const vec3_t playerOrigin, float damageRadius )
{
    int d; // = area->data->damage;
    vec3_t v;
    float distanceFromOrigin;
    float f = 0.0f;

	if(area->context.damageOverride != 0 && area->context.damageOverride != area->data->damage) {
		d = area->context.damageOverride;
	}
	else {
		d = area->data->damage;
	}
    
    SmallestVectorToBBox (v, area->origin, playerMins, playerMaxs);
    distanceFromOrigin = VectorLength (v);
    
    if ( distanceFromOrigin > damageRadius )
    {
        return 0;
    }
    
    switch ( area->data->radiusParams.damageFunc )
    {
        case DF_CONSTANT:
            return d;
        break;
        
        case DF_GAUSSIAN: // change to sigmoid?
            f = gaussian (distanceFromOrigin / damageRadius);
            f = Q_max (0.0f, Q_min (f, 1.0f));
            return d * f;
        break;
        
        case DF_LINEAR:
            return d * (1.0f - (distanceFromOrigin / damageRadius));
        break;
    }
    
    return d;
}

/*
 *	Removes a buff from a character
 */
void G_RemoveBuff(gentity_t* ent, int index)
{
	jkgBuff_t* pBuff = &buffTable[ent->client->ps.buffs[index].buffID];

	ent->client->ps.buffsActive &= ~(1 << index);
	if (pBuff->passive.overridePmoveType.first)
	{
		if (pBuff->passive.overridePmoveType.second == PM_FREEZE || pBuff->passive.overridePmoveType.second == PM_LOCK)
		{
			ent->client->pmfreeze = qfalse;
			ent->client->pmlock = qfalse;
		}
	}
}

/*
 *	Adds a buff to a character
 */
void G_BuffEntity(gentity_t* ent, gentity_t* buffer, int buffID, float intensity, int duration)
{
	int i;
	jkgBuff_t* pBuff;

	if (!ent->client)
	{
		// can only be used on clients for now
		return;
	}

	if (buffID < 0)
	{	// not a valid buff
		return;
	}

	pBuff = &buffTable[buffID];
	
	// Try and cancel out any existing buffs first, this will clear out room for the new buff ideally
	for (i = 0; i < PLAYERBUFF_BITS; i++)
	{
		
		if (ent->client->ps.buffsActive & (1 << i))
		{
			jkgBuff_t* pOtherBuff = &buffTable[ent->client->ps.buffs[i].buffID];

			// this buff is active
			if (pBuff->cancel.noBuffStack)
			{
				// don't allow buffs of the same kind to stack
				if (ent->client->ps.buffs[i].buffID == buffID)
				{
					// extend the duration and change the buffer
					ent->buffData[i].buffer = buffer;
					ent->buffData[i].endTime = level.time + duration;
					return;
				}
			}

			if (pBuff->cancel.noCategoryStack)
			{
				// don't allow buffs in the same category to stack
				if (!Q_stricmp(pOtherBuff->category, pBuff->category))
				{
					if (pOtherBuff->cancel.priority < pBuff->cancel.priority)
					{
						// cancel the other buff and put this one on instead
						G_RemoveBuff(ent, i);
					}
					else
					{
						return; // we can't apply this buff to the player
					}
				}
			}

			if (pBuff->cancel.other.size() > 0)
			{
				// try to cancel out buffs in other categories
				for (auto it = pBuff->cancel.other.begin(); it != pBuff->cancel.other.end(); ++it)
				{
					if (!Q_stricmp(pOtherBuff->category, it->category) && pOtherBuff->cancel.priority < it->priority)
					{
						G_RemoveBuff(ent, i);
					}
				}
			}
		}
	}

	// Now that some space has been cleared out, we need to apply this new buff
	for (i = 0; i < PLAYERBUFF_BITS; i++)
	{
		if (!(ent->client->ps.buffsActive & (1 << i)))
		{
			// this slot is not using anything
			ent->client->ps.buffsActive |= (1 << i);
			ent->client->ps.buffs[i].buffID = buffID;
			ent->client->ps.buffs[i].intensity = intensity;
			ent->buffData[i].buffer = buffer;
			ent->buffData[i].endTime = level.time + duration;
			ent->buffData[i].lastDamageTime = level.time;

			if (pBuff->passive.overridePmoveType.first)
			{
				if (pBuff->passive.overridePmoveType.second == PM_FREEZE)
				{
					VectorClear(ent->client->ps.velocity);
					ent->client->ps.freezeLegsAnim = ent->client->ps.legsAnim;
					ent->client->ps.freezeTorsoAnim = ent->client->ps.torsoAnim;
					ent->client->pmlock = ent->client->pmfreeze = qtrue;
				}
			}
			return;
		}
	}
}

/*
 *	Ticks all of the buffs on this entity and sees if they should be removed or if they should do damage
 */
void G_TickBuffs(gentity_t* ent)
{
	if (!JKG_ClientAlive(ent))
	{
		// if we are dead, we have no buffs active
		ent->client->ps.buffsActive = 0;
		return;
	}

	for (int i = 0; i < PLAYERBUFF_BITS; i++)
	{
		// check to see if it's active
		if (ent->client->ps.buffsActive & (1 << i))
		{
			jkgBuff_t* pBuff = &buffTable[ent->client->ps.buffs[i].buffID];
			// check for removal
			if (ent->buffData[i].endTime < level.time)
			{
				G_RemoveBuff(ent, i);
				continue;
			}

			// check for dealing damage
			if (ent->buffData[i].lastDamageTime + pBuff->damage.damageRate < level.time)
			{
				G_Damage(ent, ent->buffData[i].buffer, ent->buffData[i].buffer, vec3_origin, vec3_origin, 
					pBuff->damage.damage, DAMAGE_NO_KNOCKBACK | DAMAGE_NO_DISMEMBER | DAMAGE_NO_KNOCKBACK, pBuff->damage.meansOfDeath);
				ent->buffData[i].lastDamageTime = level.time;
			}
		}
		
	}
}

/*
 *	Applies debuffs as given to the player with damage areas
 */
static void DebuffPlayer ( gentity_t *player, damageArea_t *area, int damage, int mod )
{
    vec3_t dir;
    int i = 0;
    int flags = 0;
    
    if ( !player->client )
    {
        return;
    }

	if (!JKG_ClientAlive(player)) {	// Don't allow us to be debuffed if we are dead
		return;
	}
    
    SmallestVectorToBBox (dir, area->origin, player->r.absmin, player->r.absmax);
    dir[2] += 24.0f; // Push the player up a bit to get some air time.
    VectorNormalize (dir);

	for (i = 0; i < area->data->numberDebuffs; i++)
	{
		G_BuffEntity(player, area->context.attacker, 
			area->data->debuffs[i].debuff, area->data->debuffs[i].intensity, area->data->debuffs[i].duration);
	}
    
    if ( damage > 0 )
    {
        // Save some information in case the player dies, and gets disintegrated.
        int legsAnim = player->client->ps.legsAnim;
        int torsoAnim = player->client->ps.torsoAnim;
        vec3_t viewAngles;
        VectorCopy (player->client->ps.viewangles, viewAngles);
        
        if ( !area->data->radial )
        {
            VectorCopy (area->context.direction, dir);
        }
        
        G_Damage (player, area->context.inflictor, area->context.attacker, dir, player->client->ps.origin, damage, flags | area->context.damageFlags, mod);
    }
    
    area->lastDamageTime = level.time;
}

/*
 *	Finds a free damage area to work on.
 *	Really lame old relic from how this code used to work.
 */
static damageArea_t *GetFreeDamageArea()
{
    int i = 0;
	damageArea_t area;
	damageAreas.push_back(area);
    
    return &damageAreas.back();
}

/*
 * Damages players in the damage area. Returns qtrue if the area has decayed and we need to remove it.
 */
static qboolean DamagePlayersInArea ( damageArea_t *area )
{
    float damageRadiusSquared = 0.0f;
    int entList[MAX_GENTITIES] = { 0 };
    int numEnts = 0;
    vec3_t areaMins, areaMaxs;
    gentity_t *ent = NULL;
    int j = 0;
    float damageRadius;
    
    if ( area->startTime > level.time )
    {
        // Delayed start. Doing do anything yet.
        return qfalse;
    }
    
    if ( (area->startTime + area->data->lifetime) < level.time )
    {
        // Area has decayed, set as inactive.
		return qtrue;
    }
    
    if ( (area->lastDamageTime + area->data->damageDelay) > level.time )
    {
        // Too soon to try to damage players again.
        return qfalse;
    }
    
    damageRadius = CalculateDamageRadius (area);
    damageRadiusSquared = damageRadius * damageRadius;
    
    for ( j = 0; j < 3; j++ )
    {
        areaMins[j] = area->origin[j] - damageRadius;
        areaMaxs[j] = area->origin[j] + damageRadius;
    }
    
    numEnts = trap->EntitiesInBox (areaMins, areaMaxs, entList, MAX_GENTITIES);
    for ( j = 0; j < numEnts; j++ )
    {
        vec3_t playerOrigin;
        trace_t tr;
        int damage;
        vec3_t v;
        
        ent = &g_entities[entList[j]];
        if ( !ent->inuse || !ent->client )
        {
            continue;
        }
        
        if ( ent->s.eType != ET_NPC &&
            (ent->client->pers.connected != CON_CONNECTED || 
                ent->client->sess.sessionTeam == TEAM_SPECTATOR) )
        {
            continue;
        }
        
        if ( !ent->takedamage || ent == area->context.ignoreEnt )
        {
            continue;
        }
        
        if ( ent->health <= 0 )
        {
            continue;
        }

        // Check to make sure the player is within the radius.
        SmallestVectorToBBox (v, area->origin, ent->r.absmin, ent->r.absmax);
        if ( VectorLengthSquared (v) > damageRadiusSquared )
        {
            continue;
        }
        
        VectorCopy (ent->client->ps.origin, playerOrigin);
        if ( area->data->penetrationType != PT_WALLS )
        {
            trap->Trace (&tr, area->origin, NULL, NULL, playerOrigin, -1, CONTENTS_SOLID, 0, 0, 0);
            if ( tr.fraction != 1.0f )
            {
                continue;
            }
        }
        
        // Check for armor etc
        // if 
        
        // Apply the damage and its effects.
        damage = CalculateDamageForDistance (area, ent->r.absmin, ent->r.absmax, playerOrigin, damageRadius);
		DebuffPlayer (ent, area, damage, area->context.methodOfDeath);
    }

	return qfalse;
}

//=========================================================
// JKG_ChargeDamageOverride
//---------------------------------------------------------
// Description: Calculates damage override, which is 
// often modified as a result of a charging weapon
//=========================================================
int JKG_ChargeDamageOverride( gentity_t *inflictor, bool bIsTraceline ) {
	int damage = 0;
	if(inflictor->s.generic1 != 0 || bIsTraceline) {
		weaponData_t *wp = GetWeaponData(inflictor->s.weapon, inflictor->s.weaponVariation);
		int firemode = inflictor->s.firingMode;
		if(wp->firemodes[firemode].chargeTime) {
			if(bIsTraceline) {
				damage = WP_GetWeaponDamage(inflictor, inflictor->s.firingMode);
			}
			else {
				int current = ( level.time - inflictor->s.generic1 ) / wp->firemodes[firemode].chargeTime;
				float maximum = wp->firemodes[firemode].chargeMaximum / wp->firemodes[firemode].chargeTime;

				if ( current > maximum ) current = maximum;

				damage = wp->firemodes[firemode].baseDamage * current * wp->firemodes[firemode].chargeMultiplier;
				if(damage < wp->firemodes[firemode].baseDamage) {
					damage = wp->firemodes[firemode].baseDamage;
				}
			}
		}
	}
	return damage;
}

//=========================================================
// JKG_DoDamage
//---------------------------------------------------------
// Description: This is a wrapper for the G_Damage
// function, which also does debuffs. It does _not_ create
// damage areas. It only does direct damage like with
// G_Damage.
//=========================================================
void JKG_DoDirectDamage ( damageSettings_t* data, gentity_t *targ, gentity_t *inflictor, gentity_t *attacker, vec3_t dir, vec3_t origin, int dflags, int mod )
{
    damageArea_t area;
	int damage;
        
    if ( !targ->takedamage )
    {
        return;
    }
    
    if ( targ->health <= 0 )
    {
        return;
    }
    
    if ( !targ->client )
    {
        return;
    }
    
    memset (&area, 0, sizeof (area));

	area.data = data;
	// The firing mode's base damage can lie! It doesn't account for dynamic damage amounts (ie weapon charging)
	area.context.damageOverride = JKG_ChargeDamageOverride(inflictor, inflictor == attacker);
	if(area.context.damageOverride != 0 && area.data->damage != area.context.damageOverride) {
		damage = area.context.damageOverride;
	}
	else {
		damage = data->damage;
	}

    VectorCopy (dir, area.context.direction);
    area.context.ignoreEnt = NULL;
    area.context.attacker = attacker;
    area.context.damageFlags = dflags;
    area.context.inflictor = inflictor;
    area.context.methodOfDeath = mod;
    area.startTime = level.time;
    area.lastDamageTime = 0;
    VectorCopy (origin, area.origin);
    
	DebuffPlayer (targ, &area, damage, mod);
}

//=========================================================
// JKG_DoSplashDamage
//---------------------------------------------------------
// Description: This is a _replacement_ function for
// G_RadiusDamage. It does all the same things, in addition
// to the debuffs.
//=========================================================
void JKG_DoSplashDamage ( damageSettings_t* data, const vec3_t origin, gentity_t *inflictor, gentity_t *attacker, gentity_t *ignoreEnt, int mod )
{
	bool bDoDamageOverride = false;

	if (inflictor != attacker) {
		bDoDamageOverride = true;
	}

    if ( !data->radial )
    {
        return;
    }
    
    if ( data->lifetime )
    {
        damageArea_t *area = GetFreeDamageArea();
        if ( !area )
        {
            return;
        }

        if ( !data->planar )
        {
            area->data = data;
            area->lastDamageTime = 0;
            VectorCopy (origin, area->origin);
            VectorClear (area->context.direction);
            area->context.ignoreEnt = ignoreEnt;
            area->context.attacker = attacker;
            area->context.damageFlags = 0;
            area->context.inflictor = inflictor;
            area->context.methodOfDeath = mod;
            area->startTime = level.time + data->delay;

			if(bDoDamageOverride) {
				area->context.damageOverride = JKG_ChargeDamageOverride(inflictor, false);
			}
			else {
				area->context.damageOverride = area->data->damage;
			}
        }
        else
        {
            // check nearest surface. If can't find one, then don't create an area..
        }
    }
    else
    {
        // This is similar to the old style splash damage
        damageArea_t a;
        memset (&a, 0, sizeof (a));
        a.data = data;
        a.lastDamageTime = 0;
        VectorCopy (origin, a.origin);
        VectorClear (a.context.direction);
        a.context.ignoreEnt = ignoreEnt;
        a.context.attacker = attacker;
        a.context.damageFlags = 0;
        a.context.inflictor = inflictor;
        a.context.methodOfDeath = mod;
        a.startTime = level.time;

		if(bDoDamageOverride) {
			a.context.damageOverride = JKG_ChargeDamageOverride(inflictor, false);
		}
		else {
			a.context.damageOverride = a.data->damage;
		}
        
        DamagePlayersInArea (&a);
    }
}

//=========================================================
// JKG_DoDamage
//---------------------------------------------------------
// Description: This is a wrapper for the G_Damage
// function, which also does debuffs and stuff. Might
// create a damage area instead, if the handle does
// splash damage.
//=========================================================
void JKG_DoDamage ( damageSettings_t* data, gentity_t *targ, gentity_t *inflictor, gentity_t *attacker, vec3_t dir, vec3_t origin, int dflags, int mod )
{
    if ( data->radial )
    {
        JKG_DoSplashDamage (data, origin, inflictor, attacker, NULL, mod);
    }

    JKG_DoDirectDamage (data, targ, inflictor, attacker, dir, origin, dflags, mod);
}

//=========================================================
// JKG_DamagePlayers
//---------------------------------------------------------
// Description: To be called on every server frame update.
// This goes through all active areas and deals damage to
// players within those areas.
//=========================================================
void JKG_DamagePlayers ( void )
{
	for (auto it = damageAreas.begin(); it != damageAreas.end(); ++it)
	{
		DamagePlayersInArea(&(*it));
	}
}
