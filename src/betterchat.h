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

#include <string>
#include <vector>
#include <unordered_map>

class BetterChat : public ISmmPlugin
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	bool Pause(char* error, size_t maxlen) { return true; }
	bool Unpause(char* error, size_t maxlen) { return true; }
	void AllPluginsLoaded() {}

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
	void Hook_PostEventAbstract(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients,
								 INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize,
								 NetChannelBufType_t bufType);

public: // logic
	void LoadConfig();
	void SendChat(const char* fmt, ...);
	bool IsPlayerChatBlocked(const std::string& text) const;    // player-typed spam (say/say_team)
	bool IsNativeTextKeyBlocked(const std::string& key) const;  // Valve's own UM_TextMsg param(0)

public: // ISmmPlugin metadata
	const char* GetAuthor() { return "Killhaus"; }
	const char* GetName() { return "BetterChat"; }
	const char* GetDescription() { return "Connect/disconnect/team chat announcer + chat filter"; }
	const char* GetURL() { return "https://killhaus.su"; }
	const char* GetLicense() { return "MIT"; }
	const char* GetVersion() { return "1.0.0"; }
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

public: // per-slot bookkeeping (no entity/schema lookups needed)
	struct SlotInfo
	{
		std::string name;
		uint64 xuid = 0;
		bool connected = false;
		float lastConnectTime = -1000.0f;
	};
	SlotInfo m_Slots[64];
};

extern BetterChat g_BetterChat;

PLUGIN_GLOBALVARS();
