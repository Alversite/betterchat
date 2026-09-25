/**
 * Minimal copy of Pisex's IAdminApi (https://github.com/Pisex/cs2-admin_system,
 * include/admin.h), just the leading virtuals up to the two BetterChat calls.
 *
 * Only a PREFIX of the real vtable is declared, which is enough to call slots
 * 0..7 - so the order below must match theirs exactly. Slots BetterChat never
 * calls are placeholders: their real parameter types (std::function callbacks,
 * IMySQLConnection) don't affect the slot position and aren't worth pulling in.
 * Checked identical in 1.0.7.4, 1.0.8, main, and the copy the admin_system
 * modules are built against; the servers run core 1.0.8f (25.09.2026). If a
 * future admin_system update reorders these, set "AdminTags" "0" in
 * settings.ini until this is re-synced.
 */
#pragma once

#define ADMIN_INTERFACE "IAdminApi"

class IAdminApi
{
public:
	virtual float GetPluginVersion() = 0;                          // 0
	virtual const char* GetTranslation(const char* szKey) = 0;     // 1
	virtual void* _GetMySQLConnection() = 0;                       // 2 (placeholder)
	virtual void _RegisterCategory() = 0;                          // 3 (placeholder)
	virtual void _RegisterItem() = 0;                              // 4 (placeholder)
	virtual bool HasPermission(int iSlot, const char* szPermission) = 0; // 5
	virtual bool HasFlag(int iSlot, const char* szFlag) = 0;       // 6
	virtual bool IsAdmin(int iSlot) = 0;                           // 7
};
