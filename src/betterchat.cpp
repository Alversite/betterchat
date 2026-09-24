/**
 * BetterChat - CS2 Metamod:Source plugin
 * See betterchat.h for the "why" behind the connect-hook choice.
 *
 * Message sending (CUserMessageTextMsg / IGameEventSystem::PostEventAbstract)
 * uses the same signature-free technique as the Killhaus "Reklama" plugin, so
 * it survives CS2 updates the same way.
 */

#include "betterchat.h"
#include "vip_api.h"
#include "menus_api.h"

#include "eiface.h"
#include "engine/igameeventsystem.h"
#include "globalvars.h"
#include "icvar.h"
#include "interface.h"
#include "interfaces/interfaces.h"
#include "irecipientfilter.h"
#include "networksystem/inetworkmessages.h"
#include "networksystem/netmessage.h"
#include "playerslot.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"

#include "usermessages.pb.h"

// CS-game-specific numeric IDs for the "same" messages (see UM_TextMsg etc.
// from usermessages.proto) - CS2 dispatches under either ID depending on
// path, so both are checked. Values from
// game/shared/cstrike15/cstrike15_usermessages.proto (hardcoded instead of
// pulling that .proto in: it drags a whole Steam-GC proto dependency chain
// we don't otherwise need, just for 3 integers).
static const int CS_UM_SayText = 305;
static const int CS_UM_SayText2 = 306;
static const int CS_UM_TextMsg = 307;

// Team-change polling (see betterchat.h) - same SchemaEntity technique
// KillhausMonitor already uses successfully in production.
#include "schemasystem/schemasystem.h"
#include <entity2/entitysystem.h>
#include "utils.hpp"
#include "CBaseEntity.h"
#include "CCSPlayerController.h"
#include "module.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>

// ---------------------------------------------------------------------------
// Globals / interfaces
// ---------------------------------------------------------------------------
BetterChat g_BetterChat;
PLUGIN_EXPOSE(BetterChat, g_BetterChat);

IVEngineServer2* g_pEngineServer2 = nullptr;
IGameEventSystem* g_gameEventSystem = nullptr;

// Already declared+defined by the SDK (interfaces.a) - extern only, matching
// KillhausMonitor's exact pattern. Defining it again here causes a link-time
// "multiple definition" error.
extern IServerGameClients* g_pSource2GameClients;
extern ISchemaSystem* g_pSchemaSystem;
extern ISource2Server* g_pSource2Server;

// For the team-poll: same GameEntitySystem() resolution KillhausMonitor
// already uses successfully in production (offset confirmed against current
// CS2Fixes gamedata, per its own comment).
class IGameResourceService;
#ifndef GAMERESOURCESERVICESERVER_INTERFACE_VERSION
#define GAMERESOURCESERVICESERVER_INTERFACE_VERSION "GameResourceServiceServerV001"
#endif
extern IGameResourceService* g_pGameResourceServiceServer; // also SDK-provided
CEntitySystem* g_pEntitySystem = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;

// Not static: entity2/entitysystem.h forward-declares this exact free
// function (SDK code links against it by name), same as KillhausMonitor.
CGameEntitySystem* GameEntitySystem()
{
	if (!g_pGameResourceServiceServer)
		return nullptr;
	return *reinterpret_cast<CGameEntitySystem**>(
		reinterpret_cast<uintptr_t>(g_pGameResourceServiceServer) + WIN_LINUX(0x58, 0x50));
}

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);

// EXPERIMENTAL (see betterchat.h): located via RTTI vtable-name lookup, not
// a live interface pointer from GET_V_IFACE - IGameEventManager2 isn't
// exposed through the normal factory in CS2.
SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent*, bool);
static int g_iFireEventHookId = -1;

static CGlobalVars* GetGlobals()
{
	return g_pEngineServer2 ? g_pEngineServer2->GetServerGlobals() : nullptr;
}

SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const*, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, char const*, uint64, char const*);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, CPlayerSlot, const CCommand&);

// The OTHER PostEventAbstract overload (client-array based) - what the game's
// own internal broadcasts (cash-award text, native radio text, etc.) go
// through. This is a distinct vtable slot from the IRecipientFilter overload
// used by SendChat() below, so hooking it cannot see/affect our own messages.
// Signature verified against cs2kz-metamod (public, production CS2 plugin).
SH_DECL_HOOK8_void(IGameEventSystem, PostEventAbstract, SH_NOATTRIB, 0, CSplitScreenSlot, bool, int, const uint64*,
					INetworkMessageInternal*, const CNetMessage*, unsigned long, NetChannelBufType_t);

// ---------------------------------------------------------------------------
// Recipient filter targeting every currently-connected client (same as Reklama).
// ---------------------------------------------------------------------------
class CBroadcastFilter : public IRecipientFilter
{
public:
	CBroadcastFilter()
	{
		CGlobalVars* pGlobals = GetGlobals();
		int maxClients = pGlobals ? pGlobals->maxClients : 64;
		if (maxClients > 64)
			maxClients = 64;

		for (int i = 0; i < maxClients; i++)
		{
			if (g_pEngineServer2->GetPlayerNetInfo(i))
			{
				m_Recipients.Set(i);
				m_iCount++;
			}
		}
	}

	~CBroadcastFilter() override {}

	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec& GetRecipients() const override { return m_Recipients; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return -1; }

	int Count() const { return m_iCount; }
	bool HasRecipients() const { return m_iCount > 0; }

private:
	CPlayerBitVec m_Recipients;
	int m_iCount = 0;
};

// One player only - replies to !prefix shouldn't go to the whole server.
class CSingleRecipientFilter : public IRecipientFilter
{
public:
	explicit CSingleRecipientFilter(int iSlot)
	{
		if (iSlot >= 0 && iSlot < 64 && g_pEngineServer2->GetPlayerNetInfo(iSlot))
		{
			m_Recipients.Set(iSlot);
			m_iCount = 1;
		}
	}

	~CSingleRecipientFilter() override {}

	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec& GetRecipients() const override { return m_Recipients; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return -1; }

	int Count() const { return m_iCount; }
	bool HasRecipients() const { return m_iCount > 0; }

private:
	CPlayerBitVec m_Recipients;
	int m_iCount = 0;
};

// ---------------------------------------------------------------------------
// Chat colors: {TAG} placeholders -> CS2 chat control bytes. Same tag names
// as the old chat_cleaner.phrases.txt / Reklama settings.ini.
// ---------------------------------------------------------------------------
struct ColorTag { const char* name; char code; };
static const ColorTag s_ColorTags[] = {
	{"DEFAULT", '\x01'}, {"WHITE", '\x01'}, {"DARKRED", '\x02'}, {"RED", '\x07'},
	{"LIGHTRED", '\x0F'}, {"PURPLE", '\x03'}, {"LIGHTPURPLE", '\x0E'}, {"GREEN", '\x04'},
	{"LIGHTGREEN", '\x05'}, {"LIME", '\x06'}, {"OLIVE", '\x05'}, {"LIGHTOLIVE", '\x09'},
	{"YELLOW", '\x09'}, {"GOLD", '\x10'}, {"SILVER", '\x0A'}, {"GRAY", '\x08'}, {"GREY", '\x08'},
	{"BLUE", '\x0B'}, {"LIGHTBLUE", '\x0B'}, {"DARKBLUE", '\x0C'}, {"BLUEGREY", '\x0D'},
	{"GRAYBLUE", '\x0D'}, {"MAGENTA", '\x0E'}, {"PINK", '\x0E'},
};

// chat_processor's table, byte for byte. Used for everything the admin edits
// in admin_tags.ini / chat_format.ini, so the colours already chosen in
// chat_processor's admin.ini render exactly as before - several names mean
// a different byte here than in s_ColorTags above (e.g. LIGHTGREEN is \x06
// here, \x05 there). BetterChat's own announcements keep s_ColorTags.
static const ColorTag s_CpColorTags[] = {
	{"DEFAULT", '\x01'}, {"WHITE", '\x01'}, {"RED", '\x02'}, {"LIGHTPURPLE", '\x03'},
	{"GREEN", '\x04'}, {"LIME", '\x05'}, {"LIGHTGREEN", '\x06'}, {"DARKRED", '\x07'},
	{"GRAY", '\x08'}, {"LIGHTOLIVE", '\x09'}, {"OLIVE", '\x10'}, {"LIGHTBLUE", '\x0B'},
	{"BLUE", '\x0C'}, {"PURPLE", '\x0E'}, {"LIGHTRED", '\x0F'}, {"GRAYBLUE", '\x0A'},
	{"TEAM", '\x03'},
	// Not in chat_processor - added for the VIP tags. Aliases for bytes above
	// under the names CS2 actually renders them as (\x0A silver, \x10 gold).
	{"SILVER", '\x0A'}, {"GOLD", '\x10'},
};

// Unknown {TAGS} are left as-is, which is what lets chat_format.ini keep its
// {NAME}/{MESSAGE} placeholders through colourising.
template <size_t N>
static std::string ApplyColorTable(const std::string& in, const ColorTag (&table)[N])
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size();)
	{
		if (in[i] == '{')
		{
			size_t end = in.find('}', i);
			if (end != std::string::npos)
			{
				std::string tag = in.substr(i + 1, end - i - 1);
				for (char& c : tag) c = (char)toupper((unsigned char)c);
				bool matched = false;
				for (const ColorTag& ct : table)
				{
					if (tag == ct.name) { out.push_back(ct.code); matched = true; break; }
				}
				if (matched) { i = end + 1; continue; }
			}
		}
		out.push_back(in[i]);
		i++;
	}
	return out;
}

static std::string ApplyChatColors(const std::string& in) { return ApplyColorTable(in, s_ColorTags); }
static std::string ApplyCpColors(const std::string& in) { return ApplyColorTable(in, s_CpColorTags); }

// ---------------------------------------------------------------------------
// Minimal Valve KeyValues (KV1) parser - same as Reklama, reused verbatim so
// settings.ini keeps working exactly like it did on the old plugin.
// ---------------------------------------------------------------------------
struct KVNode
{
	std::string key;
	std::string value;
	bool isSection = false;
	std::vector<KVNode> children;

	const KVNode* Find(const char* name) const
	{
		for (const KVNode& c : children)
			if (c.key == name)
				return &c;
		return nullptr;
	}
};

class KVParser
{
public:
	explicit KVParser(const std::string& text) : m_text(text) {}

	bool Parse(KVNode& root)
	{
		std::string key;
		if (!NextToken(key)) return false;
		root.key = key;
		root.isSection = true;
		std::string brace;
		if (!NextToken(brace) || brace != "{") return false;
		return ParseSection(root);
	}

private:
	const std::string& m_text;
	size_t m_pos = 0;

	bool ParseSection(KVNode& section)
	{
		for (;;)
		{
			std::string token;
			if (!NextToken(token)) return false;
			if (token == "}") return true;

			KVNode child;
			child.key = token;
			std::string next;
			if (!NextToken(next)) return false;

			if (next == "{")
			{
				child.isSection = true;
				if (!ParseSection(child)) return false;
			}
			else
			{
				child.value = next;
			}
			section.children.push_back(std::move(child));
		}
	}

	bool NextToken(std::string& out)
	{
		SkipTrivia();
		if (m_pos >= m_text.size()) return false;
		char c = m_text[m_pos];

		if (c == '{' || c == '}') { out = std::string(1, c); m_pos++; return true; }

		if (c == '"')
		{
			m_pos++;
			out.clear();
			while (m_pos < m_text.size() && m_text[m_pos] != '"') { out.push_back(m_text[m_pos]); m_pos++; }
			if (m_pos < m_text.size()) m_pos++;
			return true;
		}

		out.clear();
		while (m_pos < m_text.size())
		{
			char b = m_text[m_pos];
			if (b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == '{' || b == '}' || b == '"') break;
			out.push_back(b);
			m_pos++;
		}
		return !out.empty();
	}

	void SkipTrivia()
	{
		for (;;)
		{
			while (m_pos < m_text.size() &&
				   (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\r' || m_text[m_pos] == '\n'))
				m_pos++;
			if (m_pos + 1 < m_text.size() && m_text[m_pos] == '/' && m_text[m_pos + 1] == '/')
			{
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') m_pos++;
				continue;
			}
			break;
		}
	}
};

static std::string ReadWholeFile(const std::string& path, bool* ok)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.good()) { *ok = false; return {}; }
	std::stringstream ss;
	ss << file.rdbuf();
	std::string text = ss.str();
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		text.erase(0, 3);
	*ok = true;
	return text;
}

static void LoadPlainTextList(const std::string& path, std::vector<std::string>& out)
{
	out.clear();
	std::ifstream file(path, std::ios::binary);
	if (!file.good())
	{
		Warning("[BetterChat] Could not open %s (list left empty)\n", path.c_str());
		return;
	}
	std::string line;
	while (std::getline(file, line))
	{
		// Strip a leading UTF-8 BOM off the very first line.
		if (out.empty() && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
			line.erase(0, 3);

		// Trim trailing \r (files may be CRLF) and surrounding whitespace.
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		size_t start = line.find_first_not_of(" \t");
		if (start != std::string::npos)
			line = line.substr(start);
		else
			line.clear();

		if (line.empty() || (line.size() >= 2 && line[0] == '/' && line[1] == '/'))
			continue; // blank line or "// comment" line

		out.push_back(line);
	}
}

// Accepts SteamID64 ("76561198871494156"), SteamID2 ("STEAM_1:0:455614214"),
// SteamID3 ("[U:1:911228428]") or a bare account id. Returns 0 if unparseable.
static uint64 ParseSteamId(std::string s)
{
	static const uint64 kIndividualBase = 76561197960265728ULL;

	while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
	size_t start = s.find_first_not_of(" \t");
	if (start == std::string::npos) return 0;
	s = s.substr(start);

	unsigned int x = 0, y = 0, z = 0;
	if (sscanf(s.c_str(), "STEAM_%u:%u:%u", &x, &y, &z) == 3)
		return kIndividualBase + (uint64)z * 2 + y;
	if (sscanf(s.c_str(), "[U:%u:%u]", &x, &z) == 2)
		return kIndividualBase + z;

	for (char c : s)
		if (!isdigit((unsigned char)c))
			return 0;
	uint64 v = strtoull(s.c_str(), nullptr, 10);
	return v < kIndividualBase ? kIndividualBase + v : v;
}

// One tag look: "tag" / "tag_color" / "name_color" / "chat_color". Same keys in
// admin_tags.ini roles and in vip_tags.ini groups.
static BetterChat::AdminRole ParseRole(const KVNode& r)
{
	BetterChat::AdminRole role;
	if (const KVNode* n = r.Find("tag"))
	{
		const KVNode* c = r.Find("tag_color");
		role.tag = ApplyCpColors((c ? c->value : std::string()) + n->value);
		role.tagText = n->value;
	}
	if (const KVNode* n = r.Find("name_color")) role.nameColor = ApplyCpColors(n->value);
	if (const KVNode* n = r.Find("chat_color")) role.chatColor = ApplyCpColors(n->value);
	return role;
}

void BetterChat::LoadAdminTags(const std::string& path)
{
	m_mapRoles.clear();
	m_mapAdmins.clear();

	bool ok = false;
	std::string text = ReadWholeFile(path, &ok);
	if (!ok)
	{
		Warning("[BetterChat] %s not found - admin tags disabled\n", path.c_str());
		return;
	}
	KVNode root;
	KVParser parser(text);
	if (!parser.Parse(root))
	{
		Warning("[BetterChat] Failed to parse %s - admin tags disabled\n", path.c_str());
		return;
	}

	if (const KVNode* roles = root.Find("roles"))
	{
		for (const KVNode& r : roles->children)
			if (r.isSection)
				m_mapRoles[r.key] = ParseRole(r);
	}

	if (const KVNode* admins = root.Find("admins"))
	{
		for (const KVNode& a : admins->children)
		{
			if (a.isSection) continue;
			uint64 xuid = ParseSteamId(a.key);
			if (!xuid)
			{
				Warning("[BetterChat] admin_tags.ini: can't read SteamID \"%s\" - skipped\n", a.key.c_str());
				continue;
			}
			if (m_mapRoles.find(a.value) == m_mapRoles.end())
			{
				Warning("[BetterChat] admin_tags.ini: %s has unknown role \"%s\" - skipped\n", a.key.c_str(), a.value.c_str());
				continue;
			}
			m_mapAdmins[xuid] = a.value;
		}
	}
}

void BetterChat::LoadVipTags(const std::string& path)
{
	m_mapVipRoles.clear();

	bool ok = false;
	std::string text = ReadWholeFile(path, &ok);
	if (!ok)
	{
		Warning("[BetterChat] %s not found - VIP tags disabled\n", path.c_str());
		return;
	}
	KVNode root;
	KVParser parser(text);
	if (!parser.Parse(root))
	{
		Warning("[BetterChat] Failed to parse %s - VIP tags disabled\n", path.c_str());
		return;
	}
	for (const KVNode& g : root.children)
		if (g.isSection)
			m_mapVipRoles[g.key] = ParseRole(g); // key = VIP group name
}

void BetterChat::LoadChatFormat(const std::string& path)
{
	m_mapChatFormat.clear();

	bool ok = false;
	std::string text = ReadWholeFile(path, &ok);
	if (!ok)
	{
		Warning("[BetterChat] %s not found - player chat left as the game sends it\n", path.c_str());
		return;
	}
	KVNode root;
	KVParser parser(text);
	if (!parser.Parse(root))
	{
		Warning("[BetterChat] Failed to parse %s - player chat left as the game sends it\n", path.c_str());
		return;
	}
	for (const KVNode& n : root.children)
		if (!n.isSection)
			m_mapChatFormat[n.key] = ApplyCpColors(n.value);
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------
void BetterChat::LoadConfig()
{
	std::string base = std::string(g_SMAPI->GetBaseDir()) + "/addons/configs/BetterChat/";

	bool ok = false;
	std::string text = ReadWholeFile(base + "settings.ini", &ok);
	if (!ok)
	{
		Warning("[BetterChat] Failed to load %ssettings.ini - using defaults\n", base.c_str());
	}
	else
	{
		KVNode root;
		KVParser parser(text);
		if (!parser.Parse(root))
		{
			Warning("[BetterChat] Failed to parse settings.ini - using defaults\n");
		}
		else
		{
			if (const KVNode* n = root.Find("DebugMode")) m_bDebugMode = atoi(n->value.c_str()) != 0;
			if (const KVNode* n = root.Find("CustomTeamMessages")) m_bCustomTeamMessages = atoi(n->value.c_str()) != 0;
			if (const KVNode* n = root.Find("CustomConnectMessages")) m_bCustomConnectMessages = atoi(n->value.c_str()) != 0;
			if (const KVNode* n = root.Find("CustomDisconnectMessages")) m_bCustomDisconnectMessages = atoi(n->value.c_str()) != 0;
			if (const KVNode* n = root.Find("ConnectDedupSeconds")) m_flConnectDedupSeconds = (float)atof(n->value.c_str());
			if (const KVNode* n = root.Find("SuppressNativeTeamJoinText")) m_bSuppressNativeTeamJoinText = atoi(n->value.c_str()) != 0;
			if (const KVNode* n = root.Find("ChatFormat")) m_bChatFormat = atoi(n->value.c_str()) != 0;
			if (const KVNode* n = root.Find("VipTags")) m_bVipTags = atoi(n->value.c_str()) != 0;
		}
	}

	LoadPlainTextList(base + "blocked_text.txt", m_vecBlockedNativeText);
	LoadPlainTextList(base + "blocked_radio.txt", m_vecBlockedNativeRadio);
	LoadPlainTextList(base + "blocked_chat_words.txt", m_vecBlockedChatWords);
	LoadAdminTags(base + "admin_tags.ini");
	LoadVipTags(base + "vip_tags.ini");
	LoadChatFormat(base + "chat_format.ini");

	// Runtime data, not config - addons/data already exists on every server.
	m_strPrefixFile = std::string(g_SMAPI->GetBaseDir()) + "/addons/data/BetterChat_prefix.ini";
	LoadPrefixChoices();

	Msg("[BetterChat] Config loaded: DebugMode=%d, CustomTeamMessages=%d, CustomConnectMessages=%d, "
		"CustomDisconnectMessages=%d, %d blocked native text keys, %d blocked native radio keys, "
		"%d blocked chat words\n",
		m_bDebugMode, m_bCustomTeamMessages, m_bCustomConnectMessages, m_bCustomDisconnectMessages,
		(int)m_vecBlockedNativeText.size(), (int)m_vecBlockedNativeRadio.size(), (int)m_vecBlockedChatWords.size());
	Msg("[BetterChat] Chat format: %s, %d message types, %d admin roles, %d admins, VIP tags %s (%d groups), %d saved !prefix choices\n",
		m_bChatFormat ? "on" : "off", (int)m_mapChatFormat.size(), (int)m_mapRoles.size(), (int)m_mapAdmins.size(),
		m_bVipTags ? "on" : "off", (int)m_mapVipRoles.size(), (int)m_mapPrefixChoice.size());
}

void BetterChat::LoadPrefixChoices()
{
	m_mapPrefixChoice.clear();

	bool ok = false;
	std::string text = ReadWholeFile(m_strPrefixFile, &ok);
	if (!ok)
		return; // nobody has picked anything yet

	KVNode root;
	KVParser parser(text);
	if (!parser.Parse(root))
	{
		Warning("[BetterChat] Failed to parse %s - !prefix choices reset\n", m_strPrefixFile.c_str());
		return;
	}
	for (const KVNode& n : root.children)
	{
		if (n.isSection) continue;
		uint64 xuid = strtoull(n.key.c_str(), nullptr, 10);
		if (xuid && (n.value == "admin" || n.value == "vip" || n.value == "off"))
			m_mapPrefixChoice[xuid] = n.value;
	}
}

// Whole file every time - it's a handful of lines, written only when someone
// uses the menu. Written to .tmp first so a crash mid-write can't truncate it.
void BetterChat::SavePrefixChoices()
{
	std::string tmp = m_strPrefixFile + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out.good())
		{
			Warning("[BetterChat] Can't write %s - !prefix choice not saved\n", tmp.c_str());
			return;
		}
		out << "// BetterChat: tag chosen with !prefix. admin / vip / off. Written by the plugin.\n";
		out << "\"PrefixChoice\"\n{\n";
		for (const auto& it : m_mapPrefixChoice)
			out << "\t\"" << (unsigned long long)it.first << "\"\t\"" << it.second << "\"\n";
		out << "}\n";
	}
	if (std::rename(tmp.c_str(), m_strPrefixFile.c_str()) != 0)
		Warning("[BetterChat] Can't replace %s - !prefix choice not saved\n", m_strPrefixFile.c_str());
}

static std::string ToLowerCopy(const std::string& s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)tolower(c); });
	return out;
}

bool BetterChat::IsPlayerChatBlocked(const std::string& text) const
{
	std::string lower = ToLowerCopy(text);
	for (const std::string& phrase : m_vecBlockedChatWords)
	{
		if (!phrase.empty() && lower.find(ToLowerCopy(phrase)) != std::string::npos)
			return true;
	}
	return false;
}

bool BetterChat::IsNativeTextKeyBlocked(const std::string& key) const
{
	// CS2 sends localization keys with a leading '#' (e.g.
	// "#Cstrike_TitlesTXT_Game_connected") - confirmed live 26.08.2026 -
	// while blocked_text.txt/blocked_radio.txt (ported from the old
	// chat_cleaner) list them without it. Strip it before comparing.
	const std::string& k2 = (!key.empty() && key[0] == '#') ? key.substr(1) : key;

	for (const std::string& k : m_vecBlockedNativeText)
		if (k2 == k)
			return true;
	for (const std::string& k : m_vecBlockedNativeRadio)
		if (k2 == k)
			return true;
	return false;
}

// Asks the engine first so tags work even for players who were already on the
// server when the plugin (re)loaded; m_Slots only learns xuids on connect.
uint64 BetterChat::GetSlotXuid(int iSlot) const
{
	if (iSlot < 0 || iSlot >= 64)
		return 0;
	uint64 xuid = g_pEngineServer2 ? g_pEngineServer2->GetClientXUID(CPlayerSlot(iSlot)) : 0;
	return xuid ? xuid : m_Slots[iSlot].xuid;
}

// Both VIP and menus live in other plugins, so the lookup is lazy and retried
// at most every 10 s while missing - chat keeps working without them, it just
// loses VIP tags / the !prefix menu. OnPluginUnload drops the pointers.
template <typename TIface>
static TIface* LookupIface(const char* szName, TIface*& pCached, PluginId& idCached, float& flNextTry, float flNow, bool bForce)
{
	if (pCached)
		return pCached;
	if (!bForce && flNow < flNextTry)
		return nullptr;
	flNextTry = flNow + 10.0f;

	int ret = META_IFACE_FAILED;
	PluginId id = 0;
	void* p = g_SMAPI->MetaFactory(szName, &ret, &id);
	if (ret == META_IFACE_FAILED || !p)
		return nullptr;

	pCached = static_cast<TIface*>(p);
	idCached = id;
	return pCached;
}

static float CurTime()
{
	CGlobalVars* pGlobals = GetGlobals();
	return pGlobals ? pGlobals->curtime : 0.0f;
}

IVIPApi* BetterChat::GetVipApi()
{
	return LookupIface(VIP_INTERFACE, m_pVip, m_iVipPluginId, m_flNextVipLookup, CurTime(), false);
}

IMenusApi* BetterChat::GetMenusApi()
{
	return LookupIface(MENUS_INTERFACE, m_pMenus, m_iMenusPluginId, m_flNextMenusLookup, CurTime(), false);
}

void BetterChat::AllPluginsLoaded()
{
	float now = CurTime();
	LookupIface(VIP_INTERFACE, m_pVip, m_iVipPluginId, m_flNextVipLookup, now, true);
	LookupIface(MENUS_INTERFACE, m_pMenus, m_iMenusPluginId, m_flNextMenusLookup, now, true);
	Msg("[BetterChat] VIP API %s, menus API %s\n", m_pVip ? "found" : "NOT found", m_pMenus ? "found" : "NOT found");
}

void BetterChat::OnPluginUnload(PluginId id)
{
	if (m_pVip && id == m_iVipPluginId)
	{
		m_pVip = nullptr;
		m_flNextVipLookup = 0.0f;
	}
	if (m_pMenus && id == m_iMenusPluginId)
	{
		m_pMenus = nullptr;
		m_flNextMenusLookup = 0.0f;
		for (bool& b : m_bOurMenuOpen) b = false; // utils took the menus with it
	}
}

void BetterChat::GetRoleOptions(int iSlot, const AdminRole** ppAdmin, const AdminRole** ppVip,
								const char** pszAdminRole, const char** pszVipRole)
{
	*ppAdmin = *ppVip = nullptr;
	*pszAdminRole = *pszVipRole = nullptr;
	if (iSlot < 0 || iSlot >= 64)
		return; // never hand the VIP plugin an out-of-range slot

	auto admin = m_mapAdmins.find(GetSlotXuid(iSlot));
	if (admin != m_mapAdmins.end())
	{
		auto role = m_mapRoles.find(admin->second);
		if (role != m_mapRoles.end())
		{
			*ppAdmin = &role->second;
			*pszAdminRole = role->first.c_str();
		}
	}

	if (m_bVipTags)
	{
		IVIPApi* vip = GetVipApi();
		if (vip && vip->VIP_IsVIPLoaded() && vip->VIP_IsClientVIP(iSlot))
		{
			const char* group = vip->VIP_GetClientVIPGroup(iSlot);
			auto vg = (group && group[0]) ? m_mapVipRoles.find(group) : m_mapVipRoles.end();
			if (vg != m_mapVipRoles.end())
			{
				*ppVip = &vg->second;
				*pszVipRole = vg->first.c_str();
			}
		}
	}

	// Same tag both ways (superadmin, emerald) is one choice, not two.
	if (*ppAdmin && *ppVip && (*ppAdmin)->tagText == (*ppVip)->tagText)
	{
		*ppVip = nullptr;
		*pszVipRole = nullptr;
	}
}

const BetterChat::AdminRole* BetterChat::FindRoleForSlot(int iSlot, const char** pszRoleName)
{
	const AdminRole *pAdmin, *pVip;
	const char *szAdmin, *szVip;
	GetRoleOptions(iSlot, &pAdmin, &pVip, &szAdmin, &szVip);

	std::string choice;
	auto it = m_mapPrefixChoice.find(GetSlotXuid(iSlot));
	if (it != m_mapPrefixChoice.end())
		choice = it->second;

	const AdminRole* pRole = nullptr;
	const char* szRole = nullptr;
	if (choice == "off")                   { }
	else if (choice == "vip" && pVip)      { pRole = pVip;   szRole = szVip; }
	else if (choice == "admin" && pAdmin)  { pRole = pAdmin; szRole = szAdmin; }
	else if (pAdmin)                       { pRole = pAdmin; szRole = szAdmin; } // no choice, or the chosen tag is gone
	else if (pVip)                         { pRole = pVip;   szRole = szVip; }

	if (pszRoleName)
		*pszRoleName = szRole;
	return pRole;
}

bool BetterChat::IsPrefixCommand(const std::string& text)
{
	size_t b = text.find_first_not_of(" \t");
	size_t e = text.find_last_not_of(" \t");
	if (b == std::string::npos)
		return false;
	std::string cmd = ToLowerCopy(text.substr(b, e - b + 1));
	return cmd == "!prefix" || cmd == "/prefix";
}

void BetterChat::OpenPrefixMenu(int iSlot)
{
	const AdminRole *pAdmin, *pVip;
	const char *szAdmin, *szVip;
	GetRoleOptions(iSlot, &pAdmin, &pVip, &szAdmin, &szVip);

	if (!pAdmin && !pVip)
	{
		SendChatTo(iSlot, "У тебя нет доступных тегов.");
		return;
	}

	IMenusApi* menus = GetMenusApi();
	if (!menus)
	{
		SendChatTo(iSlot, "Меню выбора тега сейчас недоступно.");
		return;
	}

	// Which entry is in effect right now, to mark it in the menu.
	const char* szActive = nullptr;
	const AdminRole* pActive = FindRoleForSlot(iSlot, nullptr);
	if (!pActive)                 szActive = "off";
	else if (pActive == pAdmin)   szActive = "admin";
	else                          szActive = "vip";

	auto label = [szActive](const char* key, const std::string& text) {
		return strcmp(key, szActive) == 0 ? text + " (выбран)" : text;
	};

	Menu hMenu;
	menus->SetTitleMenu(hMenu, "Выбор тега");
	if (pAdmin) menus->AddItemMenu(hMenu, "admin", label("admin", pAdmin->tagText).c_str());
	if (pVip)   menus->AddItemMenu(hMenu, "vip", label("vip", pVip->tagText).c_str());
	menus->AddItemMenu(hMenu, "off", label("off", "Без тега").c_str());
	menus->SetBackMenu(hMenu, false);
	menus->SetExitMenu(hMenu, true);
	menus->SetCallback(hMenu, [this](const char* szBack, const char* szFront, int iItem, int iCbSlot) {
		if (iCbSlot < 0 || iCbSlot >= 64)
			return;
		// 7/8/9 are utils' back/next/exit buttons, not our items.
		if (iItem >= 7 || !szBack)
		{
			if (szBack && !strcmp(szBack, "exit"))
				m_bOurMenuOpen[iCbSlot] = false;
			return;
		}
		OnPrefixMenuSelect(iCbSlot, szBack);
	});
	// 4-arg overload on purpose: the 3-arg one is ambiguous with it.
	menus->DisplayPlayerMenu(hMenu, iSlot, true, true);
	m_bOurMenuOpen[iSlot] = true;
}

void BetterChat::OnPrefixMenuSelect(int iSlot, const char* szKey)
{
	std::string key = szKey;
	if (key != "admin" && key != "vip" && key != "off")
		return;

	uint64 xuid = GetSlotXuid(iSlot);
	if (!xuid)
		return;

	m_mapPrefixChoice[xuid] = key;
	SavePrefixChoices();

	const char* szRole = nullptr;
	const AdminRole* pRole = FindRoleForSlot(iSlot, &szRole);
	if (pRole)
		SendChatTo(iSlot, "Теперь твой тег: %s", pRole->tag.c_str());
	else
		SendChatTo(iSlot, "Тег выключен.");

	if (m_bDebugMode)
		Msg("[BetterChat] !prefix: slot %d chose %s -> %s\n", iSlot, key.c_str(), szRole ? szRole : "none");

	// Closing from inside utils' own input handler would clear the menu it is
	// still iterating - VIP defers this to the next frame too.
	m_bPendingMenuClose[iSlot] = true;
}

void BetterChat::ProcessPendingMenus()
{
	for (int i = 0; i < 64; i++)
	{
		if (m_bPendingMenuClose[i])
		{
			m_bPendingMenuClose[i] = false;
			if (m_bOurMenuOpen[i])
			{
				if (IMenusApi* menus = GetMenusApi())
					menus->ClosePlayerMenu(i);
				m_bOurMenuOpen[i] = false;
			}
		}
		if (m_bPendingPrefixMenu[i])
		{
			m_bPendingPrefixMenu[i] = false;
			OpenPrefixMenu(i);
		}
	}
}

// Player-supplied text must not be able to inject colour bytes (or line
// breaks) into the line we build. chat_processor backslash-escaped '{' and
// '}' instead, which left visible backslashes in chat.
static std::string StripControlBytes(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	for (unsigned char c : in)
		if (c >= 0x20 && c != 0x7F)
			out.push_back((char)c);
	return out;
}

bool BetterChat::FormatPlayerChat(int iSlot, CUserMessageSayText2* msg)
{
	std::string type = msg->messagename();
	if (!type.empty() && type[0] == '#')
		type.erase(0, 1);

	// Only types we have a template for. Anything else - including a message
	// we already rewrote, whose messagename is now the finished line - passes
	// through untouched, so a second PostEventAbstract for the same message
	// can't decorate it twice.
	auto fmt = m_mapChatFormat.find(type);
	if (fmt == m_mapChatFormat.end())
		return false;

	std::string name = StripControlBytes(msg->param1());
	std::string text = StripControlBytes(msg->param2());

	const char* szRole = nullptr;
	const AdminRole* pRole = FindRoleForSlot(iSlot, &szRole);
	if (pRole)
	{
		if (!pRole->tag.empty())
			name = pRole->tag + " " + pRole->nameColor + name;
		else
			name = pRole->nameColor + name;
		text = pRole->chatColor + text;
	}

	// Single pass over the already-colourised template: inserted name/text
	// are never scanned again, so a "{MESSAGE}" typed into a nickname stays
	// literal.
	const std::string& tpl = fmt->second;
	std::string out;
	out.reserve(tpl.size() + name.size() + text.size());
	for (size_t i = 0; i < tpl.size();)
	{
		if (tpl.compare(i, 6, "{NAME}") == 0)          { out += name; i += 6; }
		else if (tpl.compare(i, 9, "{MESSAGE}") == 0)  { out += text; i += 9; }
		else                                           { out.push_back(tpl[i]); i++; }
	}

	if (m_bDebugMode)
		Msg("[BetterChat] Formatted %s from slot %d, role %s\n", type.c_str(), iSlot, szRole ? szRole : "-");

	msg->set_messagename(out);
	return true;
}

// ---------------------------------------------------------------------------
// Message sending (same technique as Reklama: no hard-coded offsets/signatures)
// ---------------------------------------------------------------------------
template <typename TFilter>
static int SendTextMsgTo(const TFilter& filter, int hudDest, const char* text)
{
	if (!g_pNetworkMessages || !g_gameEventSystem)
		return -1;
	if (!filter.HasRecipients())
		return 0;

	INetworkMessageInternal* pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	if (!pNetMsg)
		return -1;

	auto* pData = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	pData->set_dest(hudDest);
	pData->add_param(text);

	g_gameEventSystem->PostEventAbstract(-1, false, const_cast<TFilter*>(&filter), pNetMsg, pData, 0);

	delete pData;
	return filter.Count();
}

void BetterChat::SendChat(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	std::string colored = ApplyChatColors(buf);
	CBroadcastFilter filter;
	SendTextMsgTo(filter, 3 /*HUD_PRINTTALK*/, colored.c_str());
}

void BetterChat::SendChatTo(int iSlot, const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	std::string colored = ApplyChatColors(buf);
	CSingleRecipientFilter filter(iSlot);
	SendTextMsgTo(filter, 3 /*HUD_PRINTTALK*/, colored.c_str());
}

// ---------------------------------------------------------------------------
// SourceHook callbacks
// ---------------------------------------------------------------------------
void BetterChat::Hook_ClientPutInServer(CPlayerSlot slot, char const* pszName, int type, uint64 xuid)
{
	int i = slot.Get();
	if (i < 0 || i >= 64)
	{
		RETURN_META(MRES_IGNORED);
	}

	SlotInfo& info = m_Slots[i];

	// Defensive dedup: ClientPutInServer is documented single-fire per real
	// join, but if a rapid re-fire for the same slot+xuid is ever observed
	// (e.g. a future engine quirk), don't print the join line twice.
	CGlobalVars* pGlobals = GetGlobals();
	float now = pGlobals ? pGlobals->curtime : 0.0f;
	bool isReFire = info.connected && info.xuid == xuid && (now - info.lastConnectTime) < m_flConnectDedupSeconds;

	info.name = pszName ? pszName : "";
	info.xuid = xuid;
	info.connected = true;
	info.lastConnectTime = now;

	if (m_bDebugMode)
		Msg("[BetterChat] ClientPutInServer slot=%d name=%s type=%d xuid=%llu refire=%d\n",
			i, info.name.c_str(), type, (unsigned long long)xuid, isReFire ? 1 : 0);

	if (m_bCustomConnectMessages && !isReFire)
		SendChat("Игрок {GREEN}%s{DEFAULT} подключился к серверу", info.name.c_str());

	RETURN_META(MRES_IGNORED);
}

void BetterChat::Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
										char const* pszName, uint64 xuid, char const* pszNetworkID)
{
	int i = slot.Get();
	if (i >= 0 && i < 64)
	{
		SlotInfo& info = m_Slots[i];

		if (m_bDebugMode)
			Msg("[BetterChat] ClientDisconnect slot=%d name=%s\n", i, pszName ? pszName : "");

		if (m_bCustomDisconnectMessages && info.connected)
			SendChat("Игрок {GREEN}%s{DEFAULT} покинул сервер", pszName ? pszName : info.name.c_str());

		info.connected = false;
		info.lastKnownTeam = -1; // fresh slate for whoever connects into this slot next

		// utils drops a leaving player's menu itself; just forget ours.
		m_bPendingPrefixMenu[i] = false;
		m_bPendingMenuClose[i] = false;
		m_bOurMenuOpen[i] = false;
	}

	RETURN_META(MRES_IGNORED);
}

static const char* TeamChangePhrase(int team)
{
	switch (team)
	{
		case 1: return "перешёл в {GRAY}Наблюдатели";
		case 2: return "перешёл в {OLIVE}Террористы";
		case 3: return "перешёл в {BLUE}Контр-Террористы";
		default: return nullptr;
	}
}

void BetterChat::Hook_ClientCommand(CPlayerSlot slot, const CCommand& args)
{
	int i = slot.Get();
	if (i < 0 || i >= 64 || args.ArgC() < 1)
	{
		RETURN_META(MRES_IGNORED);
	}

	const char* cmd = args.Arg(0);
	SlotInfo& info = m_Slots[i];
	const char* playerName = info.name.empty() ? "?" : info.name.c_str();

	// NOTE: verified live on 26.08.2026 that neither "jointeam" nor "say" ever
	// reach ClientCommand in CS2 - team changes and chat are handled through
	// other paths entirely (see PollTeamChanges() and Hook_PostEventAbstract()).
	// Kept as a debug trace in case some OTHER command turns out useful here.
	if (m_bDebugMode)
		Msg("[BetterChat] ClientCommand slot=%d cmd=%s argc=%d\n", i, cmd, args.ArgC());

	RETURN_META(MRES_IGNORED);
}

// Polls each connected player's team once every ~0.5s and fires the
// team-change chat line when it differs from the last known value. Not
// event-driven because CS2 doesn't expose a clean, signature-free hook for
// team changes (see betterchat.h for why).
void BetterChat::PollTeamChanges()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = reinterpret_cast<CEntitySystem*>(g_pGameEntitySystem);
	if (!g_pEntitySystem)
		return;

	for (CEntityInstance* e : UTIL_FindEntityByClassnameAll("cs_player_controller"))
	{
		CCSPlayerController* pc = reinterpret_cast<CCSPlayerController*>(e);
		if (!pc || !pc->IsConnected())
			continue;

		int slot = pc->GetPlayerSlot();
		if (slot < 0 || slot >= 64)
			continue;

		SlotInfo& info = m_Slots[slot];
		int team = pc->GetTeam();

		if (info.lastKnownTeam == -1)
		{
			// First time we see this player - just record the team, don't
			// announce (avoids a spurious message right after connect).
			info.lastKnownTeam = team;
			continue;
		}

		if (team != info.lastKnownTeam)
		{
			info.lastKnownTeam = team;

			const char* name = pc->GetPlayerName();
			if (!name || !name[0])
				name = info.name.empty() ? "?" : info.name.c_str();

			if (m_bDebugMode)
				Msg("[BetterChat] Team change slot=%d name=%s team=%d\n", slot, name, team);

			if (m_bCustomTeamMessages)
			{
				const char* phrase = TeamChangePhrase(team);
				if (phrase)
					SendChat("Игрок {GREEN}%s{DEFAULT} %s", name, phrase);
			}
		}
	}
}

// EXPERIMENTAL - see betterchat.h. Re-fires "player_team" with
// bDontBroadcast forced to true (so it still runs server-side - other
// plugins/game logic listening for it are unaffected - it just never
// reaches clients, which is what stops the client-drawn "is joining the
// Terrorists" line). Everything else passes through untouched.
bool BetterChat::Hook_FireEvent(IGameEvent* event, bool bDontBroadcast)
{
	if (m_bSuppressNativeTeamJoinText && event && !bDontBroadcast)
	{
		const char* name = event->GetName();
		if (name && !strcmp(name, "player_team"))
		{
			IGameEventManager2* pMgr = META_IFACEPTR(IGameEventManager2);
			if (pMgr)
			{
				if (m_bDebugMode)
					Msg("[BetterChat] Re-firing player_team with bDontBroadcast=true\n");

				bool result = SH_CALL(pMgr, &IGameEventManager2::FireEvent)(event, true);
				RETURN_META_VALUE(MRES_SUPERCEDE, result);
			}
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, false);
}

void BetterChat::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	ProcessPendingMenus();

	CGlobalVars* pGlobals = GetGlobals();
	if (pGlobals)
	{
		float dt = pGlobals->curtime - m_flLastFrameCurtime;
		m_flLastFrameCurtime = pGlobals->curtime;
		if (dt < 0.0f || dt > 1.0f)
			dt = 0.0f;

		m_flTeamPollAccum += dt;
		if (m_flTeamPollAccum >= 0.5f)
		{
			m_flTeamPollAccum = 0.0f;
			PollTeamChanges();
		}
	}
}

// Suppresses:
//  - Valve's own native TextMsg broadcasts whose param(0) is a blocked key
//    (blocked_text.txt / blocked_radio.txt)
//  - real player chat (SayText2) whose text matches blocked_chat_words.txt
// by zeroing the recipient bitmask before the original call runs - same
// technique cs2kz-metamod's kz_quiet.cpp uses in production. Never touches
// BetterChat's own messages: those go through the OTHER PostEventAbstract
// overload entirely (the IRecipientFilter one SendChat() calls).
//
// CS2 dispatches the "same" message under two different numeric IDs
// depending on path (the generic engine one and the CS-game-specific one),
// so - same as cs2kz-metamod - both are checked.
void BetterChat::Hook_PostEventAbstract(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients,
										 INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize,
										 NetChannelBufType_t bufType)
{
	if (!pEvent || !pData || !clients)
	{
		RETURN_META(MRES_IGNORED);
	}

	NetMessageInfo_t* info = pEvent->GetNetMessageInfo();
	if (!info)
	{
		RETURN_META(MRES_IGNORED);
	}

	if (m_bDebugMode)
		Msg("[BetterChat] PostEventAbstract msgid=%d\n", (int)info->m_MessageId);

	if (info->m_MessageId == UM_TextMsg || info->m_MessageId == CS_UM_TextMsg)
	{
		auto* msg = const_cast<CNetMessage*>(pData)->ToPB<CUserMessageTextMsg>();
		if (msg->param_size() >= 1)
		{
			const std::string key = msg->param(0);

			if (m_bDebugMode)
				Msg("[BetterChat] TextMsg dest=%d param0=%s\n", msg->dest(), key.c_str());

			if (IsNativeTextKeyBlocked(key))
			{
				if (m_bDebugMode)
					Msg("[BetterChat] Suppressed native TextMsg: %s\n", key.c_str());

				*const_cast<uint64*>(clients) = 0;
			}
		}
	}
	else if (info->m_MessageId == UM_SayText2 || info->m_MessageId == CS_UM_SayText2)
	{
		auto* msg = const_cast<CNetMessage*>(pData)->ToPB<CUserMessageSayText2>();
		std::string text = msg->param2();

		if (m_bDebugMode)
			Msg("[BetterChat] SayText2 messagename=%s param1=%s param2=%s\n",
				msg->messagename().c_str(), msg->param1().c_str(), text.c_str());

		if (IsPrefixCommand(text))
		{
			// Swallow the command itself and open the menu next frame (see
			// betterchat.h for why not from here).
			int iSlot = msg->entityindex() - 1;
			if (iSlot >= 0 && iSlot < 64)
				m_bPendingPrefixMenu[iSlot] = true;
			*const_cast<uint64*>(clients) = 0;
		}
		else if (!text.empty() && IsPlayerChatBlocked(text))
		{
			if (m_bDebugMode)
				Msg("[BetterChat] Blocked chat text: %s\n", text.c_str());

			*const_cast<uint64*>(clients) = 0;
		}
		else if (m_bChatFormat)
		{
			// Rewritten in place and left to go out - same approach as
			// chat_processor. entityindex is the controller, i.e. slot + 1.
			FormatPlayerChat(msg->entityindex() - 1, msg);
		}
	}
	else if (info->m_MessageId == UM_SayText || info->m_MessageId == CS_UM_SayText)
	{
		auto* msg = const_cast<CNetMessage*>(pData)->ToPB<CUserMessageSayText>();
		std::string text = msg->text();

		if (!text.empty() && IsPlayerChatBlocked(text))
		{
			if (m_bDebugMode)
				Msg("[BetterChat] Blocked chat text (SayText): %s\n", text.c_str());

			*const_cast<uint64*>(clients) = 0;
		}
	}

	RETURN_META(MRES_IGNORED);
}

// ---------------------------------------------------------------------------
// Metamod entry points
// ---------------------------------------------------------------------------
bool BetterChat::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer2, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);

	SH_ADD_HOOK(IServerGameClients, ClientPutInServer, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientPutInServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientDisconnect), true);
	SH_ADD_HOOK(IServerGameClients, ClientCommand, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientCommand), false);
	SH_ADD_HOOK(IGameEventSystem, PostEventAbstract, g_gameEventSystem, SH_MEMBER(this, &BetterChat::Hook_PostEventAbstract), false);
	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &BetterChat::Hook_GameFrame), true);

	LoadConfig();

	// OnPluginUnload - so a VIP/utils unload can't leave us calling freed code.
	g_SMAPI->AddListener(this, this);

	// EXPERIMENTAL, gated by config (see betterchat.h) - locate
	// IGameEventManager2's vtable by RTTI name and hook FireEvent directly on
	// it. No live interface pointer needed for this: SH_ADD_DVPHOOK operates
	// on the vtable itself.
	if (m_bSuppressNativeTeamJoinText)
	{
		static DynLibUtils::CModule s_ServerModule("server");
		DynLibUtils::CMemory vtbl = s_ServerModule.GetVirtualTableByName("CGameEventManager");
		if (vtbl)
		{
			IGameEventManager2* pVtblAsInstance = vtbl.RCast<IGameEventManager2*>();
			g_iFireEventHookId = SH_ADD_DVPHOOK(IGameEventManager2, FireEvent, pVtblAsInstance,
												 SH_MEMBER(this, &BetterChat::Hook_FireEvent), false);
			Msg("[BetterChat] Native team-join suppression: CGameEventManager vtable %s, hook %s\n",
				vtbl ? "found" : "NOT found", g_iFireEventHookId >= 0 ? "installed" : "FAILED");
		}
		else
		{
			Warning("[BetterChat] Native team-join suppression enabled in config, but CGameEventManager "
					"vtable was not found - feature disabled this run.\n");
		}
	}

	Msg("[BetterChat] Plugin loaded.\n");
	return true;
}

bool BetterChat::Unload(char* error, size_t maxlen)
{
	// utils keeps a copy of our menu callback, which lives in this .so - close
	// any !prefix menu still open before the code goes away.
	if (m_pMenus)
	{
		for (int i = 0; i < 64; i++)
			if (m_bOurMenuOpen[i] && m_pMenus->IsMenuOpen(i))
				m_pMenus->ClosePlayerMenu(i);
	}

	if (g_pSource2GameClients)
	{
		SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientPutInServer), true);
		SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientDisconnect), true);
		SH_REMOVE_HOOK(IServerGameClients, ClientCommand, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientCommand), false);
	}
	if (g_gameEventSystem)
	{
		SH_REMOVE_HOOK(IGameEventSystem, PostEventAbstract, g_gameEventSystem, SH_MEMBER(this, &BetterChat::Hook_PostEventAbstract), false);
	}
	if (g_pSource2Server)
	{
		SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &BetterChat::Hook_GameFrame), true);
	}
	if (g_iFireEventHookId >= 0)
	{
		SH_REMOVE_HOOK_ID(g_iFireEventHookId);
		g_iFireEventHookId = -1;
	}
	return true;
}
