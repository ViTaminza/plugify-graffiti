// graffiti — Plugify C++ port of the Metamod:Source plugin cs2-graffiti: CS2 graffiti for
// everyone, sprayed by the server itself (no inventory item). The server creates a
// player_spray_decal with a design (sticker kit spray_*) and a tint from the game files, and the
// clients draw it like a normal graffiti.
//
// Ported 1:1 from cs2-graffiti (config keys incl. the production graffiti.ini, menus,
// cookies, double-E, cooldown, lifetime, one-per-player).
//
// Deliberate differences (forced by the framework):
//   * the catalog is read through s2sdk::ReadFileVPK instead of g_pFullFileSystem->ReadFile;
//   * the utils IPlayersApi::RayTrace is gone: the aim trace is s2sdk::TraceLine (eyes -> view
//     direction * max_distance, the pawn ignored), which also reports the hit entity for the
//     original's "world geometry only" rule (the decal would hang in the air on players and moving
//     props); it gives no surface normal, so the decal faces back along the view direction (the
//     original's own zero-normal fallback). TraceCollideable is NOT a ray with an ignored entity:
//     it traces against that one entity's collideable;
//   * the schema writes of CPlayerSprayDecal go through the s2sdk schema setters on the entity
//     handle (changeState like the original's SetStateChanged);
//   * ICookiesApi is replaced by the official permissions cookies (Graffiti.Kit / Graffiti.Tint /
//     Graffiti.DoubleE), the user is loaded with permissions::LoadUser by a once-per-second
//     reconcile;
//   * the utils settings-menu hook (utils_settings) is not ported (the Plugify menu layer has no
//     settings menu extension point); the !graffiti menu is unchanged.
//
// No s2sdk call happens before a map exists, no exception escapes into s2sdk and the plugin never
// returns Failed from OnPluginStart.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

#include <pisex/pisex.hpp>

#include "graffiti_core.hpp"

namespace {

const char* kLogTag = "[Graffiti]";

constexpr const char* kConfigFile = "graffiti.ini";
constexpr const char* kPhrasesFile = "graffiti.phrases.txt";

// CInButtonsState bits (hl2sdk usercmd in_buttons.h); IN_USE is button 5.
constexpr uint64_t kInUse = 1ull << 5;

// Aims further than this are "too far" (the original compares the trace length).
constexpr float kTraceMask = 0x1C300B; // the mask the Pisex RayTrace used (player trace)

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

gr::Config g_Cfg;
gr::Catalog g_Catalog;
std::map<std::string, std::string> g_mapPhrases;
std::string g_sServerLang = "ru";
bool g_bCatalogLoaded = false;

struct PlayerData
{
	int iKit = 0; // 0 = random
	int iTint = 0;
	bool bDoubleE = true;
	float flLastSpray = -1000.0f;
	// s2sdk entity HANDLE of the player's spray (index | serial << 15), 0 = none.
	// Stored as a handle, not as an index: indexes are recycled by the engine, so a stale index
	// would resolve to a *different* entity and RemoveSpray() would delete that one.
	int iSpray = 0;
	float flSprayTime = 0.0f;
	gr::DoubleEState doubleE;
};
PlayerData g_Players[64];

std::mt19937 g_Rng{std::random_device{}()};
uint64_t g_uLoadedSteam[64] = {};
float g_flReconcileTimer = 0.0f;
bool g_bReady = false;
bool g_bSprayBindings = true; // false when the s2sdk spray path is incomplete (checked at start)
int64_t g_iPluginId = 0;
int g_iUniqueID = 1;

// ---------------------------------------------------------------------------------------------
// Logging / helpers
// ---------------------------------------------------------------------------------------------

void LogMessage(const std::string& sText, plg::Severity eSeverity)
{
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + sText, eSeverity, plg::source_location::current());
}

void Log(const std::string& sText) { LogMessage(sText, plg::Severity::Info); }
void LogError(const std::string& sText) { LogMessage(sText, plg::Severity::Error); }

void Defer(std::function<void()> fn)
{
	pisex::g_pUtils->NextFrame(std::move(fn));
}

std::string Phrase(const std::string& sKey)
{
	return gr::Phrase(g_mapPhrases, g_sServerLang, sKey);
}

void Chat(int iSlot, const std::string& s)
{
	pisex::g_pUtils->PrintToChat(iSlot, "%s", s.c_str());
}

int EntityHandle(int iIndex)
{
	if (iIndex <= 0 || !__s2sdk_EntIndexToEntHandle) return 0;
	const int iHandle = s2sdk::EntIndexToEntHandle(iIndex);
	return gr::IsValidHandle(iHandle) ? iHandle : 0;
}

bool HasController(int iSlot)
{
	return iSlot >= 0 && iSlot < 64 && pisex::IsConnected(iSlot) && pisex::GetControllerEntity(iSlot) > 0;
}

bool IsHuman(int iSlot)
{
	return HasController(iSlot) && !pisex::IsFakeClient(iSlot);
}

int AlivePawnOf(int iSlot)
{
	if (!HasController(iSlot) || !pisex::IsAlive(iSlot)) return 0;
	return pisex::GetPawnEntity(iSlot);
}

float Now()
{
	pisex::GlobalVars* pGlobals = pisex::GetCGlobalVars();
	return pGlobals ? pGlobals->curtime : 0.0f;
}

// ---------------------------------------------------------------------------------------------
// Cookies
// ---------------------------------------------------------------------------------------------

bool CookieRead(uint64_t uSteamID64, const char* szName, std::string& sOut)
{
	if (!uSteamID64 || !__permissions_GetCookie) return false;
	try
	{
		plg::any value;
		if (permissions::GetCookie(uSteamID64, plg::string(szName), value) != permissions::Status::Success) return false;
		if (plg::holds_alternative<plg::string>(value))
		{
			const plg::string& s = plg::get<plg::string>(value);
			sOut.assign(s.data(), s.size());
			return true;
		}
		if (plg::holds_alternative<int32_t>(value)) { sOut = std::to_string(plg::get<int32_t>(value)); return true; }
		if (plg::holds_alternative<int64_t>(value)) { sOut = std::to_string(plg::get<int64_t>(value)); return true; }
		if (plg::holds_alternative<bool>(value)) { sOut = plg::get<bool>(value) ? "1" : "0"; return true; }
		return false;
	}
	catch (...)
	{
		return false;
	}
}

void CookieWrite(uint64_t uSteamID64, const char* szName, const std::string& sValue)
{
	if (!uSteamID64 || !__permissions_SetCookie) return;
	try
	{
		permissions::SetCookie(g_iPluginId, uSteamID64, plg::string(szName), plg::any(plg::string(sValue)),
							   /*dontBroadcast=*/false);
	}
	catch (...)
	{
	}
}

void SaveCookie(int iSlot, const char* szName, int iValue)
{
	CookieWrite(pisex::GetSteamID64(iSlot), szName, std::to_string(iValue));
}

// ---------------------------------------------------------------------------------------------
// Player state
// ---------------------------------------------------------------------------------------------

void ResetPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	const int iSpray = g_Players[iSlot].iSpray;
	g_Players[iSlot] = PlayerData();
	g_Players[iSlot].iKit = g_Cfg.iDefaultKit;
	g_Players[iSlot].iTint = g_Cfg.iDefaultTint;
	g_Players[iSlot].bDoubleE = g_Cfg.bDoubleE;
	g_Players[iSlot].iSpray = iSpray;
}

void LoadPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	ResetPlayer(iSlot);
	const uint64_t uSteam = pisex::GetSteamID64(iSlot);
	if (!uSteam) return;
	std::string sValue;
	if (CookieRead(uSteam, "Graffiti.Kit", sValue)) g_Players[iSlot].iKit = atoi(sValue.c_str());
	if (CookieRead(uSteam, "Graffiti.Tint", sValue)) g_Players[iSlot].iTint = atoi(sValue.c_str());
	if (CookieRead(uSteam, "Graffiti.DoubleE", sValue)) g_Players[iSlot].bDoubleE = atoi(sValue.c_str()) != 0;
}

void ReconcileCookies(float flDelta)
{
	if (!__permissions_LoadUser) return;
	g_flReconcileTimer += flDelta;
	if (g_flReconcileTimer < 1.0f) return;
	g_flReconcileTimer = 0.0f;

	for (int i = 0; i < 64; i++)
	{
		if (!HasController(i) || pisex::IsFakeClient(i) || !pisex::IsAuthenticated(i))
		{
			if (g_uLoadedSteam[i])
			{
				g_uLoadedSteam[i] = 0;
				ResetPlayer(i);
			}
			continue;
		}
		const uint64_t uSteam = pisex::GetSteamID64(i);
		if (!uSteam || g_uLoadedSteam[i] == uSteam) continue;
		const char* szName = pisex::GetPlayerName(i);
		if (permissions::LoadUser(g_iPluginId, uSteam, plg::string(szName ? szName : ""), /*offline=*/false,
								  /*dontBroadcast=*/false) == permissions::Status::Success)
		{
			g_uLoadedSteam[i] = uSteam;
			LoadPlayer(i);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Config / phrases / catalog
// ---------------------------------------------------------------------------------------------

void LoadConfig()
{
	bool ok = false;
	const std::string sText = pisex::ReadFile(pisex::ConfigDir() + "/" + kConfigFile, &ok);
	if (!ok || !gr::ParseConfigText(sText, g_Cfg))
	{
		LogError(std::string("failed to read ") + pisex::ConfigDir() + "/" + kConfigFile + " - using the defaults");
		return;
	}
	for (int i = 0; i < 64; i++)
	{
		g_Players[i].iKit = g_Cfg.iDefaultKit;
		g_Players[i].iTint = g_Cfg.iDefaultTint;
	}
}

void LoadPhrases()
{
	g_mapPhrases.clear();
	const char* szLang = pisex::g_pUtils->GetLanguage();
	g_sServerLang = gr::Lower(gr::Trim(szLang && szLang[0] ? szLang : "ru"));
	if (g_sServerLang.empty()) g_sServerLang = "ru";
	bool ok = false;
	const std::string sText = pisex::ReadFile(pisex::TranslationsDir() + "/" + kPhrasesFile, &ok);
	if (!ok) return;
	gr::ParsePhrasesText(sText, g_sServerLang, g_mapPhrases);
}

std::string ReadGameFile(const std::string& sPath)
{
	if (!__s2sdk_ReadFileVPK) return {};
	try
	{
		const plg::string sText = s2sdk::ReadFileVPK(plg::string(sPath), plg::string("GAME"));
		std::string s(sText.data(), sText.size());
		// UTF-8 BOM
		if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
			s.erase(0, 3);
		return s;
	}
	catch (...)
	{
		return {};
	}
}

void LoadCatalog()
{
	g_Catalog = gr::Catalog();
	g_bCatalogLoaded = false;

	const std::string sItems = ReadGameFile("scripts/items/items_game.txt");
	if (sItems.empty() || !gr::ParseItemsGame(sItems, g_Catalog))
	{
		LogError("cannot read/parse scripts/items/items_game.txt - the graffiti list stays empty");
		return;
	}

	const std::string sValveLang = gr::ValveLanguage(g_sServerLang);
	std::map<std::string, std::string> mapLocal = gr::ParseLocalization(ReadGameFile("resource/csgo_" + sValveLang + ".txt"));
	std::map<std::string, std::string> mapEnglish;
	if (g_sServerLang != "en")
		mapEnglish = gr::ParseLocalization(ReadGameFile("resource/csgo_english.txt"));

	gr::FinalizeCatalog(g_Catalog, mapLocal, mapEnglish);
	g_bCatalogLoaded = true;
	Log("catalog: " + std::to_string(g_Catalog.vKits.size()) + " designs, " + std::to_string(g_Catalog.vTints.size())
		+ " colors, names: " + (mapLocal.empty() ? "english" : sValveLang));
}

// ---------------------------------------------------------------------------------------------
// Schema field helpers (CPlayerSprayDecal)
// ---------------------------------------------------------------------------------------------

constexpr const char* kDecalClass = "CPlayerSprayDecal";

void SetDecalInt(int iHandle, const char* szField, int64_t iValue)
{
	try { s2sdk::SetEntSchema(iHandle, plg::string(kDecalClass), plg::string(szField), iValue, /*changeState=*/true, 0); }
	catch (...) {}
}

void SetDecalFloat(int iHandle, const char* szField, float flValue)
{
	try { s2sdk::SetEntSchemaFloat(iHandle, plg::string(kDecalClass), plg::string(szField), flValue, true, 0); }
	catch (...) {}
}

void SetDecalVector(int iHandle, const char* szField, const plg::vec3& v)
{
	try { s2sdk::SetEntSchemaVector3D(iHandle, plg::string(kDecalClass), plg::string(szField), v, true, 0); }
	catch (...) {}
}

void DumpSchema()
{
	// The original walked the live schema; s2sdk resolves one class/field pair at a time, so the
	// known fields are checked by name and their offsets printed.
	static const char* kFields[] = {
		"m_vecEndPos", "m_vecStart", "m_vecLeft", "m_vecNormal", "m_nPlayer", "m_nTintID",
		"m_unAccountID", "m_nUniqueID", "m_rtGcTime", "m_nEntity", "m_nHitbox", "m_flCreationTime",
	};
	Log("CPlayerSprayDecal of this build:");
	for (const char* szField : kFields)
	{
		const int iOffset = pisex::SchemaOffset(kDecalClass, szField);
		Log(std::string("  ") + szField + (iOffset >= 0 ? " +0x" + std::to_string(iOffset) : " MISSING"));
	}
}

// ---------------------------------------------------------------------------------------------
// Spray
// ---------------------------------------------------------------------------------------------

void RemoveSpray(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	const int iHandle = g_Players[iSlot].iSpray;
	g_Players[iSlot].iSpray = 0;
	if (!gr::IsValidHandle(iHandle) || !__s2sdk_RemoveEntity) return;
	// the handle carries the serial: IsValidEntHandle() is false once the decal is gone (or its
	// index was recycled), so nothing else is ever removed
	try
	{
		if (__s2sdk_IsValidEntHandle && s2sdk::IsValidEntHandle(iHandle)) s2sdk::RemoveEntity(iHandle);
	}
	catch (...)
	{
	}
}

void ClearAllSprays()
{
	for (int i = 0; i < 64; i++) RemoveSpray(i);
}

// true when the spray was placed; bMessages decides whether the player learns why it was not.
bool TrySpray(int iSlot, bool bMessages)
{
	if (!g_bSprayBindings) return false; // logged once at plugin start
	if (!g_Cfg.bEnabled || !pisex::IsGameReady() || g_Catalog.vKits.empty() || !IsHuman(iSlot)) return false;

	const int iPawn = AlivePawnOf(iSlot);
	if (!iPawn) return false;
	const int iPawnHandle = EntityHandle(iPawn);
	const int iController = pisex::GetControllerEntity(iSlot);
	if (!gr::IsValidHandle(iPawnHandle) || !iController) return false;

	PlayerData& p = g_Players[iSlot];
	const float flNow = Now();
	const float flLeft = p.flLastSpray + g_Cfg.flCooldown - flNow;
	if (flLeft > 0.0f)
	{
		if (bMessages) Chat(iSlot, gr::Replace(Phrase("Cooldown"), "{sec}", std::to_string((int)ceilf(flLeft))));
		return false;
	}

	// eyes -> view direction * max_distance, the pawn ignored (the Pisex RayTrace of the original)
	const plg::vec3 vecStart = s2sdk::GetClientEyePosition(iSlot);
	const plg::vec3 vecAngles = s2sdk::GetClientEyeAngles(iSlot);
	const plg::vec3 vecForward = s2sdk::AnglesToVector(vecAngles);
	const plg::vec3 vecEnd{vecStart.x + vecForward.x * g_Cfg.flMaxDistance,
						   vecStart.y + vecForward.y * g_Cfg.flMaxDistance,
						   vecStart.z + vecForward.z * g_Cfg.flMaxDistance};

	// The hit comes from a ray with the pawn ignored - the original's Pisex RayTrace, i.e.
	// s2sdk::TraceLine(mask, ignore). TraceCollideable traces against ONE entity's collideable
	// (the parameter is that entity's handle): given the pawn it never saw the wall, and every
	// spray died silently below (double E did nothing at all).
	plg::vec3 vecHit{};
	double flFraction = 1.0;
	bool bHit = false, bStartSolid = false;
	int32_t iEntHit = -1;
	if (!__s2sdk_TraceLine || !__s2sdk_EntHandleToEntIndex
		|| !s2sdk::TraceLine(vecStart, vecEnd, (int32_t)kTraceMask, iPawnHandle, vecHit, flFraction, bHit, iEntHit,
							 bStartSolid))
	{
		if (bMessages) Chat(iSlot, Phrase("Too_Far"));
		return false;
	}

	const float flDist = gr::Distance({vecStart.x, vecStart.y, vecStart.z}, {vecHit.x, vecHit.y, vecHit.z});
	// a failed trace returns zeroes: start and end coincide
	if (!bHit || flDist < 1.0f || flDist > g_Cfg.flMaxDistance || flFraction >= 1.0)
	{
		if (bMessages) Chat(iSlot, Phrase("Too_Far"));
		return false;
	}

	// world geometry only: on players, weapons and moving objects the decal would hang in the air
	// (the trace reports the hit as an entity handle; the world entity is index 0)
	if (s2sdk::EntHandleToEntIndex(iEntHit) > 0)
	{
		if (bMessages) Chat(iSlot, Phrase("Bad_Surface"));
		return false;
	}

	// TraceLine gives no surface normal: a zero normal makes ComputeDecalGeometry face the decal
	// back along the view direction - the original's own zero-normal fallback.
	const plg::vec3 vecNormal{};

	const gr::DecalGeometry geom =
		gr::ComputeDecalGeometry({vecStart.x, vecStart.y, vecStart.z}, {vecHit.x, vecHit.y, vecHit.z},
								 {vecNormal.x, vecNormal.y, vecNormal.z});

	const int iKit = gr::PickKitId(g_Catalog, p.iKit, (uint32_t)g_Rng());
	const int iTint = gr::PickTintId(g_Catalog, p.iTint, (uint32_t)g_Rng());
	if (iKit <= 0) return false;

	int iDecalHandle = 0;
	try { iDecalHandle = s2sdk::CreateEntityByName(plg::string("player_spray_decal")); }
	catch (...) { iDecalHandle = 0; }
	if (!gr::IsValidHandle(iDecalHandle) || !s2sdk::IsValidEntHandle(iDecalHandle))
	{
		LogError("CreateEntityByName(player_spray_decal) returned nothing");
		return false;
	}

	const uint64_t uSteamID = pisex::GetSteamID64(iSlot);
	SetDecalVector(iDecalHandle, "m_vecEndPos", vecHit);
	SetDecalVector(iDecalHandle, "m_vecStart", vecStart);
	SetDecalVector(iDecalHandle, "m_vecLeft", plg::vec3{geom.vecLeft.x, geom.vecLeft.y, geom.vecLeft.z});
	SetDecalVector(iDecalHandle, "m_vecNormal", plg::vec3{geom.vecNormal.x, geom.vecNormal.y, geom.vecNormal.z});
	SetDecalInt(iDecalHandle, "m_nPlayer", iKit);
	SetDecalInt(iDecalHandle, "m_nTintID", iTint);
	SetDecalInt(iDecalHandle, "m_unAccountID", (int64_t)(uSteamID & 0xFFFFFFFFull));
	SetDecalInt(iDecalHandle, "m_nUniqueID", g_iUniqueID++);
	SetDecalInt(iDecalHandle, "m_rtGcTime", (int64_t)time(nullptr));
	SetDecalInt(iDecalHandle, "m_nEntity", 0);
	SetDecalInt(iDecalHandle, "m_nHitbox", 0);
	SetDecalFloat(iDecalHandle, "m_flCreationTime", flNow);
	try { s2sdk::DispatchSpawn(iDecalHandle); }
	catch (...)
	{
	}

	if (g_Cfg.bOnePerPlayer) RemoveSpray(iSlot);
	p.iSpray = iDecalHandle; // the handle, see PlayerData::iSpray
	p.flSprayTime = flNow;
	p.flLastSpray = flNow;

	if (!g_Cfg.sSound.empty())
	{
		plg::vector<int32_t> vListeners;
		for (int i = 0; i < 64; i++)
			if (IsHuman(i)) vListeners.push_back(i);
		if (!vListeners.empty())
		{
			try { s2sdk::EmitSoundToClient(iPawnHandle, vListeners, plg::string(g_Cfg.sSound), 1.0f, 100.0f); }
			catch (...) {}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------------------------

std::string KitName(int iKit)
{
	const gr::Kit* k = gr::FindKit(g_Catalog, iKit);
	return k ? k->sDisplay : Phrase("Random");
}

std::string TintName(int iTint)
{
	const gr::Tint* t = gr::FindTint(g_Catalog, iTint);
	return t ? t->sDisplay : Phrase("Random");
}

void ShowMainMenu(int iSlot);
void ShowCategories(int iSlot);

void ShowKits(int iSlot, const std::string& sCategory)
{
	pisex::Menu hMenu;
	const std::string sTitle = gr::HasPhrase(g_mapPhrases, "Category_" + sCategory) ? Phrase("Category_" + sCategory) : sCategory;
	pisex::g_pMenus->SetTitleMenu(hMenu, sTitle.c_str());
	for (const gr::Kit& k : g_Catalog.vKits)
	{
		if (k.sCategory != sCategory) continue;
		const std::string sText = (k.iId == g_Players[iSlot].iKit ? "► " : "") + k.sDisplay;
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(k.iId).c_str(), sText.c_str(), pisex::ITEM_DEFAULT);
	}
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "back") { Defer([iSlot]() { ShowCategories(iSlot); }); return; }
		if (sBack == "exit" || sBack == "next") return;
		const int iKit = atoi(sBack.c_str());
		Defer([iSlot, iKit]() {
			if (!gr::FindKit(g_Catalog, iKit)) return;
			g_Players[iSlot].iKit = iKit;
			SaveCookie(iSlot, "Graffiti.Kit", iKit);
			Chat(iSlot, gr::Replace(Phrase("Selected_Kit"), "{name}", KitName(iKit)));
			ShowMainMenu(iSlot);
		});
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowCategories(int iSlot)
{
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase("Menu_Kits_Title").c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "random", ((g_Players[iSlot].iKit == 0 ? "► " : "") + Phrase("Random")).c_str(), pisex::ITEM_DEFAULT);
	for (const std::string& sCategory : g_Catalog.vCategories)
	{
		size_t n = 0;
		for (const gr::Kit& k : g_Catalog.vKits)
			if (k.sCategory == sCategory) n++;
		const std::string sText = (gr::HasPhrase(g_mapPhrases, "Category_" + sCategory) ? Phrase("Category_" + sCategory) : sCategory)
			+ " (" + std::to_string(n) + ")";
		pisex::g_pMenus->AddItemMenu(hMenu, ("cat_" + sCategory).c_str(), sText.c_str(), pisex::ITEM_DEFAULT);
	}
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "back") { Defer([iSlot]() { ShowMainMenu(iSlot); }); return; }
		if (sBack == "exit" || sBack == "next") return;
		if (sBack == "random")
		{
			Defer([iSlot]() {
				g_Players[iSlot].iKit = 0;
				SaveCookie(iSlot, "Graffiti.Kit", 0);
				Chat(iSlot, gr::Replace(Phrase("Selected_Kit"), "{name}", Phrase("Random")));
				ShowMainMenu(iSlot);
			});
			return;
		}
		if (sBack.rfind("cat_", 0) == 0)
		{
			const std::string sCategory = sBack.substr(4);
			Defer([iSlot, sCategory]() { ShowKits(iSlot, sCategory); });
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowTints(int iSlot)
{
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase("Menu_Tints_Title").c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "0", ((g_Players[iSlot].iTint == 0 ? "► " : "") + Phrase("Random")).c_str(), pisex::ITEM_DEFAULT);
	for (const gr::Tint& t : g_Catalog.vTints)
	{
		const std::string sText = (t.iId == g_Players[iSlot].iTint ? "► " : "") + t.sDisplay;
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(t.iId).c_str(), sText.c_str(), pisex::ITEM_DEFAULT);
	}
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "back") { Defer([iSlot]() { ShowMainMenu(iSlot); }); return; }
		if (sBack == "exit" || sBack == "next") return;
		const int iTint = atoi(sBack.c_str());
		Defer([iSlot, iTint]() {
			if (iTint != 0 && !gr::FindTint(g_Catalog, iTint)) return;
			g_Players[iSlot].iTint = iTint;
			SaveCookie(iSlot, "Graffiti.Tint", iTint);
			Chat(iSlot, gr::Replace(Phrase("Selected_Tint"), "{name}", TintName(iTint)));
			ShowMainMenu(iSlot);
		});
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void SetDoubleE(int iSlot, bool bOn)
{
	g_Players[iSlot].bDoubleE = bOn;
	SaveCookie(iSlot, "Graffiti.DoubleE", bOn ? 1 : 0);
	Chat(iSlot, gr::Replace(Phrase("DoubleE_Set"), "{state}", Phrase(bOn ? "On" : "Off")));
}

void ShowMainMenu(int iSlot)
{
	if (!IsHuman(iSlot)) return;
	const PlayerData& p = g_Players[iSlot];
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase("Menu_Title").c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "kit", gr::Replace(Phrase("Menu_Kit"), "{name}", KitName(p.iKit)).c_str(), pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "tint", gr::Replace(Phrase("Menu_Tint"), "{name}", TintName(p.iTint)).c_str(), pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "double_e", gr::Replace(Phrase("Menu_DoubleE"), "{state}", Phrase(p.bDoubleE ? "On" : "Off")).c_str(), pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "help", Phrase("Menu_Help").c_str(), pisex::ITEM_DEFAULT);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "exit" || sBack == "back" || sBack == "next") return;
		Defer([iSlot, sBack]() {
			if (sBack == "kit") ShowCategories(iSlot);
			else if (sBack == "tint") ShowTints(iSlot);
			else if (sBack == "double_e") { SetDoubleE(iSlot, !g_Players[iSlot].bDoubleE); ShowMainMenu(iSlot); }
			else if (sBack == "help") Chat(iSlot, Phrase("Help"));
		});
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

// ---------------------------------------------------------------------------------------------
// Frame work: lifetime and double E
// ---------------------------------------------------------------------------------------------

// The two pawn fields the every-frame double-E check reads: CCSPlayerPawn::m_bIsDefusing and
// m_bIsGrabbingHostage (1-byte bools). The typed GetEntSchema of the old helper built the
// class+field schema-key strings and took the schema table's shared_mutex on every call - twice per
// alive player per frame. The offsets are resolved once (OnFrame only runs with a live map, so the
// first lookup sees an initialised schema system) and the bytes are read raw, like the glove fields
// of fortnite_emotes.
struct PawnBusyOffsets
{
	int iDefusing = -1;
	int iHostage = -1;
	bool bResolved = false;

	void Resolve()
	{
		if (bResolved) return;
		bResolved = true;
		iDefusing = pisex::SchemaOffset("CCSPlayerPawn", "m_bIsDefusing");
		iHostage = pisex::SchemaOffset("CCSPlayerPawn", "m_bIsGrabbingHostage");
	}
};

PawnBusyOffsets g_PawnBusy;

bool GetPawnBoolAt(int iPawn, int iOffset)
{
	if (iOffset < 0 || !__s2sdk_GetEntData) return false;
	const int iHandle = EntityHandle(iPawn);
	if (!gr::IsValidHandle(iHandle)) return false;
	try { return s2sdk::GetEntData(iHandle, iOffset, 1) != 0; }
	catch (...) { return false; }
}

// E is doing something useful while defusing / carrying a hostage: do not spray then.
bool IsPawnBusy(int iPawn)
{
	g_PawnBusy.Resolve();
	return GetPawnBoolAt(iPawn, g_PawnBusy.iDefusing) || GetPawnBoolAt(iPawn, g_PawnBusy.iHostage);
}

void OnFrame()
{
	if (!g_Cfg.bEnabled || !pisex::IsGameReady()) return;
	const float flNow = Now();

	for (int i = 0; i < 64; i++)
	{
		PlayerData& p = g_Players[i];
		if (g_Cfg.flLifetime > 0.0f && gr::IsValidHandle(p.iSpray) && flNow - p.flSprayTime > g_Cfg.flLifetime) RemoveSpray(i);
		if (!p.bDoubleE) continue;

		const int iPawn = AlivePawnOf(i);
		if (!iPawn || pisex::IsFakeClient(i))
		{
			p.doubleE.Reset();
			continue;
		}

		const uint64_t uButtons = __s2sdk_GetClientButtons ? s2sdk::GetClientButtons(i, 0) : 0;
		const bool bHeld = (uButtons & kInUse) != 0;
		const bool bBusy = IsPawnBusy(iPawn);
		if (p.doubleE.Feed(bHeld, flNow, g_Cfg.flDoubleEWindow, bBusy))
			TrySpray(i, false);
	}
}

// ---------------------------------------------------------------------------------------------
// Events / commands
// ---------------------------------------------------------------------------------------------

void HookEvents()
{
	pisex::g_pUtils->HookEvent("player_disconnect", [](const char*, pisex::IGameEvent* pEvent, bool) {
		const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
		if (iSlot < 0 || iSlot >= 64) return;
		RemoveSpray(iSlot);
		ResetPlayer(iSlot);
		g_uLoadedSteam[iSlot] = 0;
	});
	// entities of the previous map are gone; the timestamps start over with the new map
	pisex::RegisterMapStartCallback([](const char*) {
		for (int i = 0; i < 64; i++)
		{
			g_Players[i].iSpray = 0;
			g_Players[i].flLastSpray = -1000.0f;
			g_Players[i].doubleE.Reset();
		}
		if (!g_bCatalogLoaded && pisex::IsGameReady()) LoadCatalog();
	});
}

// Every s2sdk entry point the spray uses, checked once at plugin start: a missing one means this
// plugin and the installed s2sdk do not match (the manifest constrains the version, so this is a
// "fail soft, log once" net rather than the expected case).
void CheckSprayBindings()
{
	const bool bOk = __s2sdk_GetClientEyePosition && __s2sdk_GetClientEyeAngles && __s2sdk_AnglesToVector
		&& __s2sdk_TraceCollideable && __s2sdk_TraceLine && __s2sdk_EntHandleToEntIndex
		&& __s2sdk_CreateEntityByName && __s2sdk_DispatchSpawn && __s2sdk_IsValidEntHandle
		&& __s2sdk_RemoveEntity && __s2sdk_EmitSoundToClient && __s2sdk_GetEntSchema
		&& __s2sdk_SetEntSchema && __s2sdk_SetEntSchemaVector3D;
	if (bOk) return;

	g_bSprayBindings = false;
	LogError("the s2sdk bindings needed for spraying are incomplete - spraying stays disabled "
			 "(catalog, menu and settings keep working)");
}

void RegisterCommands()
{
	pisex::g_pUtils->RegCommand({}, g_Cfg.vMenuCommands, [](int iSlot, const char*) -> bool {
		if (iSlot >= 0 && g_Cfg.bEnabled) ShowMainMenu(iSlot);
		return true;
	});
	pisex::g_pUtils->RegCommand({"mm_spray"}, g_Cfg.vSprayCommands, [](int iSlot, const char*) -> bool {
		if (iSlot >= 0) TrySpray(iSlot, true);
		return true;
	});
	pisex::g_pUtils->RegCommand({"mm_graffiti_reload"}, {}, [](int iSlot, const char*) -> bool {
		if (iSlot != -1) return true;
		LoadConfig();
		LoadPhrases();
		LoadCatalog();
		return true;
	});
	pisex::g_pUtils->RegCommand({"mm_graffiti_clear"}, {}, [](int iSlot, const char*) -> bool {
		if (iSlot == -1) ClearAllSprays();
		return true;
	});
	pisex::g_pUtils->RegCommand({"mm_graffiti_schema"}, {}, [](int iSlot, const char*) -> bool {
		if (iSlot == -1) DumpSchema();
		return true;
	});
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------

class GraffitiPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override
	{
		try
		{
			pisex::Init("graffiti");
			g_iPluginId = static_cast<int64_t>(GetId());

			LoadConfig();
			LoadPhrases();
			LoadCatalog();
			for (int i = 0; i < 64; i++)
			{
				g_Players[i].iKit = g_Cfg.iDefaultKit;
				g_Players[i].iTint = g_Cfg.iDefaultTint;
				g_Players[i].bDoubleE = g_Cfg.bDoubleE;
			}

			HookEvents();
			RegisterCommands();
			CheckSprayBindings();

			g_bReady = true;
			Log("loaded: " + std::to_string(g_Catalog.vKits.size()) + " designs, "
				+ std::to_string(g_Catalog.vTints.size()) + " colors");
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override
	{
		try
		{
			pisex::Poll();
			if (!g_bReady) return {};
			ReconcileCookies((float)dt.count() / 1000.0f);
			OnFrame();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("update failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override
	{
		try
		{
			g_bReady = false;
			ClearAllSprays();
			pisex::Shutdown();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

GraffitiPlugin g_graffitiPlugin;

PLUGIFY_PLUGIN(PLUGIN_API, &g_graffitiPlugin)
