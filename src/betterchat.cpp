/**
 * BetterChat - CS2 Metamod:Source plugin
 * See betterchat.h for the "why" behind the connect-hook choice.
 *
 * Message sending (CUserMessageTextMsg / IGameEventSystem::PostEventAbstract)
 * uses the same signature-free technique as the Killhaus "Reklama" plugin, so
 * it survives CS2 updates the same way.
 */

#include "betterchat.h"

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

static CGlobalVars* GetGlobals()
{
	return g_pEngineServer2 ? g_pEngineServer2->GetServerGlobals() : nullptr;
}

SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const*, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, char const*, uint64, char const*);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, CPlayerSlot, const CCommand&);

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

static std::string ApplyChatColors(const std::string& in)
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
				for (const ColorTag& ct : s_ColorTags)
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
		}
	}

	LoadPlainTextList(base + "blocked_text.txt", m_vecBlockedChatPhrases);
	LoadPlainTextList(base + "blocked_radio.txt", m_vecBlockedRadioPhrases);

	Msg("[BetterChat] Config loaded: DebugMode=%d, CustomTeamMessages=%d, CustomConnectMessages=%d, "
		"CustomDisconnectMessages=%d, %d blocked chat phrases, %d blocked radio phrases\n",
		m_bDebugMode, m_bCustomTeamMessages, m_bCustomConnectMessages, m_bCustomDisconnectMessages,
		(int)m_vecBlockedChatPhrases.size(), (int)m_vecBlockedRadioPhrases.size());
}

static std::string ToLowerCopy(const std::string& s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)tolower(c); });
	return out;
}

bool BetterChat::IsChatBlocked(const std::string& text) const
{
	std::string lower = ToLowerCopy(text);
	for (const std::string& phrase : m_vecBlockedChatPhrases)
	{
		if (!phrase.empty() && lower.find(ToLowerCopy(phrase)) != std::string::npos)
			return true;
	}
	return false;
}

bool BetterChat::IsRadioBlocked(const std::string& radioCmd) const
{
	for (const std::string& phrase : m_vecBlockedRadioPhrases)
	{
		if (radioCmd == phrase)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Message sending (same technique as Reklama: no hard-coded offsets/signatures)
// ---------------------------------------------------------------------------
static int SendTextMsgTo(const CBroadcastFilter& filter, int hudDest, const char* text)
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

	g_gameEventSystem->PostEventAbstract(-1, false, const_cast<CBroadcastFilter*>(&filter), pNetMsg, pData, 0);

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

	// --- team change: "jointeam <n>" (1=spec, 2=T, 3=CT) ---
	if (m_bCustomTeamMessages && !strcmp(cmd, "jointeam") && args.ArgC() >= 2)
	{
		int team = atoi(args.Arg(1));
		const char* phrase = TeamChangePhrase(team);
		if (phrase)
			SendChat("Игрок {GREEN}%s{DEFAULT} %s", playerName, phrase);

		RETURN_META(MRES_IGNORED);
	}

	// --- chat filter: "say" / "say_team" ---
	if ((!strcmp(cmd, "say") || !strcmp(cmd, "say_team")) && args.ArgC() >= 2)
	{
		std::string text = args.Arg(1);
		if (!text.empty() && text.front() == '"' && text.back() == '"' && text.size() >= 2)
			text = text.substr(1, text.size() - 2);

		if (IsChatBlocked(text))
		{
			if (m_bDebugMode)
				Msg("[BetterChat] Blocked chat from %s: %s\n", playerName, text.c_str());

			RETURN_META(MRES_SUPERCEDE); // swallow the command - it never reaches chat
		}

		RETURN_META(MRES_IGNORED);
	}

	// --- radio filter: "radio1" / "radio2" / "radio3" (voice command groups) ---
	if (!strncmp(cmd, "radio", 5) && args.ArgC() >= 2)
	{
		if (IsRadioBlocked(args.Arg(1)))
		{
			if (m_bDebugMode)
				Msg("[BetterChat] Blocked radio from %s: %s\n", playerName, args.Arg(1));

			RETURN_META(MRES_SUPERCEDE);
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

	SH_ADD_HOOK(IServerGameClients, ClientPutInServer, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientPutInServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientDisconnect), true);
	SH_ADD_HOOK(IServerGameClients, ClientCommand, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientCommand), false);

	LoadConfig();

	Msg("[BetterChat] Plugin loaded.\n");
	return true;
}

bool BetterChat::Unload(char* error, size_t maxlen)
{
	if (g_pSource2GameClients)
	{
		SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientPutInServer), true);
		SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientDisconnect), true);
		SH_REMOVE_HOOK(IServerGameClients, ClientCommand, g_pSource2GameClients, SH_MEMBER(this, &BetterChat::Hook_ClientCommand), false);
	}
	return true;
}
