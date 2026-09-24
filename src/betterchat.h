/**
 * BetterChat - CS2 Metamod:Source plugin
 *
 * Replacement for the old "chat_cleaner" plugin: custom connect / disconnect /
 * team-change chat announcements, plus a player chat filter (blocked words /
 * URLs / ad-bot spam).
 *
 * Root-cause fix vs. the old plugin: the old plugin double-printed the
 * "connected" message because CS2 fires its early network-connect
 * notification TWICE per real client (an initial handshake attempt that gets
 * torn down with NETWORK_DISCONNECT_LOOPSHUTDOWN, then a second one that
 * actually sticks). This plugin hooks ISource2GameClients::ClientPutInServer
 * instead, which only fires once - when the player is genuinely, fully in
 * the game - so there is nothing left to de-duplicate.
 */
#pragma once

#include <ISmmPlugin.h>
#include <eiface.h>
#include <playerslot.h>
#include <networksystem/inetworkserializer.h>
#include <igameevents.h>

#include <string>
#include <vector>
#include <unordered_map>

class CUserMessageSayText2; // usermessages.pb.h, only needed in betterchat.cpp
class IVIPApi;              // vip_api.h
class IMenusApi;            // menus_api.h

class BetterChat : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	bool Pause(char* error, size_t maxlen) { return true; }
	bool Unpause(char* error, size_t maxlen) { return true; }
	void AllPluginsLoaded();

public: // IMetamodListener - drop the VIP/menus pointers if their plugin goes away
	void OnPluginUnload(PluginId id);

public: // SourceHook callbacks (all on ISource2GameClients - one interface, one proven ABI)
	void Hook_ClientPutInServer(CPlayerSlot slot, char const* pszName, int type, uint64 xuid);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
								char const* pszName, uint64 xuid, char const* pszNetworkID);
	void Hook_ClientCommand(CPlayerSlot slot, const CCommand& args);

	// Suppresses Valve's own native UM_TextMsg broadcasts (cash-award spam,
	// "Cstrike_TitlesTXT_Game_connected" etc, radio callout text) whose param(0)
	// key is listed in blocked_text.txt / blocked_radio.txt - same technique
	// public CS2 plugins (e.g. cs2kz-metamod) use for this exact purpose.
	// Different vtable slot than the IRecipientFilter overload SendChat() uses,
	// so this can never intercept BetterChat's own outgoing messages.
	// Also handles SayText/SayText2 (real player chat) for blocked_chat_words.txt.
	void Hook_PostEventAbstract(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients,
								 INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize,
								 NetChannelBufType_t bufType);

	// Team changes in CS2 don't go through a "jointeam" ClientCommand (verified
	// live on 26.08.2026 - it never fires) and there's no clean, signature-free
	// hook for it either. So: poll each connected player's team every half
	// second via GameEntitySystem/schema (same technique KillhausMonitor already
	// uses successfully in production) and diff against the last-known value.
	void Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void PollTeamChanges();

	// EXPERIMENTAL (risk accepted 26.08.2026): the client-rendered native
	// "X is joining the Terrorists/CT" line isn't a chat message at all - the
	// client draws it locally off the "player_team" game event. The only way
	// to stop it is to keep that specific event from broadcasting to clients.
	// Found via IGameEventManager2, located at runtime by RTTI vtable name
	// lookup (DynLibUtils::CModule::GetVirtualTableByName, same technique
	// CS2Fixes uses in production) - NOT a raw byte-signature scan, so it
	// should survive engine updates better, but is still less proven than
	// everything else in this plugin. Gated by "SuppressNativeTeamJoinText"
	// in settings.ini so it can be switched off instantly without a rebuild
	// if it ever misbehaves.
	bool Hook_FireEvent(IGameEvent* event, bool bDontBroadcast);

public: // logic
	void LoadConfig();
	void LoadAdminTags(const std::string& path);
	void LoadChatFormat(const std::string& path);
	void SendChat(const char* fmt, ...);
	bool IsPlayerChatBlocked(const std::string& text) const;    // player-typed spam (say/say_team)
	bool IsNativeTextKeyBlocked(const std::string& key) const;  // Valve's own UM_TextMsg param(0)

	// Replaces what chat_processor did: rewrites a player's SayText2 in place
	// so it renders with the chat_format.ini template, plus the admin tag from
	// admin_tags.ini. Returns false (message untouched) when there's no
	// template for this message type.
	bool FormatPlayerChat(int iSlot, CUserMessageSayText2* msg);
	uint64 GetSlotXuid(int iSlot) const;
	IVIPApi* GetVipApi();
	IMenusApi* GetMenusApi();
	void SendChatTo(int iSlot, const char* fmt, ...);

	// !prefix / /prefix: pick which tag to show when a player has several
	// (admin + VIP), or none. Opened from GameFrame, never from inside the
	// PostEventAbstract hook that spotted the command - the menu sends its
	// own net messages, and re-entering PostEventAbstract isn't worth the risk.
	static bool IsPrefixCommand(const std::string& text);
	void OpenPrefixMenu(int iSlot);
	void OnPrefixMenuSelect(int iSlot, const char* szKey);
	void ProcessPendingMenus();
	void LoadPrefixChoices();
	void SavePrefixChoices();

public: // ISmmPlugin metadata
	const char* GetAuthor() { return "Killhaus"; }
	const char* GetName() { return "BetterChat"; }
	const char* GetDescription() { return "Connect/disconnect/team announcer, chat filter, chat format, admin/VIP tags + !prefix"; }
	const char* GetURL() { return "https://killhaus.su"; }
	const char* GetLicense() { return "MIT"; }
	const char* GetVersion() { return "1.2.0"; }
	const char* GetDate() { return __DATE__; }
	const char* GetLogTag() { return "BETTERCHAT"; }

public: // config (settings.ini - same keys as the old chat_cleaner)
	bool m_bDebugMode = false;
	bool m_bCustomTeamMessages = true;
	bool m_bCustomConnectMessages = true;
	bool m_bCustomDisconnectMessages = true;

	// New (optional, safe defaults keep old behaviour): defensive re-fire guard.
	// ClientPutInServer should only ever fire once per real join, but this is a
	// zero-cost safety net in case of an edge case we haven't seen.
	float m_flConnectDedupSeconds = 3.0f;

	// EXPERIMENTAL, see Hook_FireEvent - default OFF until confirmed safe on
	// a live server. Flip to "1" in settings.ini to try it.
	bool m_bSuppressNativeTeamJoinText = false;

	// Same meaning as in the old chat_cleaner: keys of Valve's own native
	// UM_TextMsg broadcasts (cash-award spam, "Cstrike_TitlesTXT_Game_connected"
	// etc. / radio callout text) that get suppressed so only BetterChat's own
	// custom message shows. Matched by exact key, not substring.
	std::vector<std::string> m_vecBlockedNativeText;  // configs/BetterChat/blocked_text.txt
	std::vector<std::string> m_vecBlockedNativeRadio; // configs/BetterChat/blocked_radio.txt

	// NEW: what real players type in chat (say/say_team) - substring match,
	// case-insensitive. Not part of the old chat_cleaner; added against the
	// ad-bot spam ("cs2commends.com" etc.) found on 26.08.2026.
	std::vector<std::string> m_vecBlockedChatWords; // configs/BetterChat/blocked_chat_words.txt

	// Replacement for chat_processor (disabled 25.09.2026 - it blanked any
	// message type missing from its phrases file, and its admin-tag module
	// was never loaded). "ChatFormat" in settings.ini, default ON so chat
	// keeps looking the way chat_processor made it look.
	bool m_bChatFormat = true;

	struct AdminRole
	{
		std::string tag;        // already colorized (control bytes, not {TAGS})
		std::string nameColor;  // "
		std::string chatColor;  // "
		std::string tagText;    // plain tag text - shown in the !prefix menu,
		                        // and what "same tag" means when merging options
	};
	std::unordered_map<std::string, AdminRole> m_mapRoles;   // role name -> look   (admin_tags.ini "roles")
	std::unordered_map<uint64, std::string> m_mapAdmins;      // SteamID64 -> role   (admin_tags.ini "admins")
	std::unordered_map<std::string, std::string> m_mapChatFormat; // message type -> template (chat_format.ini)

	// VIP tags: VIP group (Pisex VIP's groups.ini) -> role, asked of the VIP
	// plugin at message time. Soft dependency - no VIP plugin means no VIP
	// tags, nothing else changes. "VipTags" in settings.ini, default ON.
	bool m_bVipTags = true;
	std::unordered_map<std::string, std::string> m_mapVipGroups; // admin_tags.ini "vip_groups"
	IVIPApi* m_pVip = nullptr;
	PluginId m_iVipPluginId = 0;
	float m_flNextVipLookup = 0.0f;

	IMenusApi* m_pMenus = nullptr;
	PluginId m_iMenusPluginId = 0;
	float m_flNextMenusLookup = 0.0f;

	// Per-server !prefix choice, SteamID64 -> "admin" / "vip" / "off".
	// Stored as the SOURCE rather than a role name, so a VIP moving from
	// silver to gold keeps "show my VIP tag". addons/data/BetterChat_prefix.ini
	std::unordered_map<uint64, std::string> m_mapPrefixChoice;
	std::string m_strPrefixFile;

	bool m_bPendingPrefixMenu[64] = {};
	bool m_bPendingMenuClose[64] = {};
	bool m_bOurMenuOpen[64] = {};  // closed on Unload: utils holds a callback into our .so

	// Tags this player may pick from. Same tag text from both sources
	// collapses into one entry - that's how superadmin/emerald (identical as
	// admin and as VIP) end up with a single choice.
	void GetRoleOptions(int iSlot, const AdminRole** ppAdmin, const AdminRole** ppVip,
						const char** pszAdminRole, const char** pszVipRole);

	// Honours the !prefix choice; default admin over VIP. nullptr = no tag.
	const AdminRole* FindRoleForSlot(int iSlot, const char** pszRoleName);

public: // per-slot bookkeeping (no entity/schema lookups needed)
	struct SlotInfo
	{
		std::string name;
		uint64 xuid = 0;
		bool connected = false;
		float lastConnectTime = -1000.0f;
		int lastKnownTeam = -1; // -1 = not yet observed (don't announce on first sight)
	};
	SlotInfo m_Slots[64];

	float m_flTeamPollAccum = 0.0f;
	float m_flLastFrameCurtime = 0.0f;
};

extern BetterChat g_BetterChat;

PLUGIN_GLOBALVARS();
