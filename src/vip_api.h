/**
 * Minimal copy of Pisex's IVIPApi (https://github.com/Pisex/cs2-vip,
 * include/vip.h), just the leading virtuals BetterChat calls.
 *
 * Only a PREFIX of the real vtable is declared, which is enough to call slots
 * 0..5 - so the order below must match theirs exactly. Verified against VIP
 * 1.2.5 (the version on the KILLHAUS servers) on 25.09.2026. If a future VIP
 * update reorders these, set "VipTags" "0" in settings.ini until this is
 * re-synced; calling a shifted slot would be undefined behaviour.
 */
#pragma once

#define VIP_INTERFACE "IVIPApi"

class IVIPApi
{
public:
	virtual bool VIP_IsVIPLoaded() = 0;                                                  // 0
	virtual bool VIP_IsClientVIP(int iSlot) = 0;                                          // 1
	virtual int VIP_GetClientAccessTime(int iSlot) = 0;                                   // 2
	virtual bool VIP_SetClientAccessTime(int iSlot, int iTime, bool bInDB = true) = 0;    // 3
	virtual bool VIP_SetClientVIPGroup(int iSlot, const char* szGroup, bool bInDB = true) = 0; // 4
	virtual const char* VIP_GetClientVIPGroup(int iSlot) = 0;                             // 5
};
