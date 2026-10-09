// graffiti — engine-free core (see graffiti_core.hpp).
#include "graffiti_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <pisex/keyvalues.hpp>

namespace gr {

std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
	size_t pos = 0;
	while (!from.empty() && (pos = s.find(from, pos)) != std::string::npos)
	{
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

std::vector<std::string> SplitList(const std::string& s)
{
	std::vector<std::string> v;
	size_t start = 0;
	while (start <= s.size())
	{
		size_t end = s.find(',', start);
		std::string part = Trim(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (!part.empty()) v.push_back(part);
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return v;
}

// ---------------------------------------------------------------- config

bool ParseConfigText(std::string_view text, Config& cfg)
{
	cfg = Config();
	pisex::KeyValues kv("Graffiti");
	if (!kv.LoadFromBuffer(text)) return false;

	cfg.bEnabled = kv.GetInt("enabled", 1) != 0;
	auto vMenu = SplitList(kv.GetString("commands", "!graffiti,!spray"));
	if (!vMenu.empty()) cfg.vMenuCommands = vMenu;
	cfg.vSprayCommands = SplitList(kv.GetString("spray_commands", ""));
	cfg.bDoubleE = kv.GetInt("double_e", 1) != 0;
	cfg.flDoubleEWindow = std::max(0.1f, kv.GetFloat("double_e_window", 0.35f));
	cfg.flCooldown = std::max(0.0f, kv.GetFloat("cooldown", 30.0f));
	cfg.flMaxDistance = std::max(32.0f, kv.GetFloat("max_distance", 150.0f));
	cfg.bOnePerPlayer = kv.GetInt("one_per_player", 1) != 0;
	cfg.flLifetime = std::max(0.0f, kv.GetFloat("lifetime", 0.0f));
	cfg.sSound = kv.GetString("sound", "SprayCan.Paint");
	cfg.bUtilsSettings = kv.GetInt("utils_settings", 1) != 0;
	cfg.iDefaultKit = kv.GetInt("default_kit", 0);
	cfg.iDefaultTint = kv.GetInt("default_tint", 0);
	return true;
}

// ---------------------------------------------------------------- catalog

std::string KitCategory(const std::string& sName)
{
	// spray_<group>_<design>
	std::string s = sName.size() > 6 ? sName.substr(6) : std::string();
	std::string sGroup = s.substr(0, s.find('_'));
	if (sGroup == "std" || sGroup == "std2" || sGroup == "std3" || sGroup == "vcap1" || sGroup == "illuminate1")
		return sGroup;
	return "other";
}

bool ParseItemsGame(std::string_view text, Catalog& catalog)
{
	catalog = Catalog();
	pisex::KeyValues kv("items_game");
	if (!kv.LoadFromBuffer(text)) return false;

	// several sticker_kits blocks exist (stickers and sprays were added in parts)
	for (pisex::KeyValues* pKits = kv.GetFirstTrueSubKey(); pKits; pKits = pKits->GetNextTrueSubKey())
	{
		if (strcmp(pKits->GetName(), "sticker_kits") != 0) continue;
		for (pisex::KeyValues* pKit = pKits->GetFirstTrueSubKey(); pKit; pKit = pKit->GetNextTrueSubKey())
		{
			const char* szName = pKit->GetString("name", "");
			if (strncmp(szName, "spray_", 6) != 0) continue;
			Kit k;
			k.iId = atoi(pKit->GetName());
			if (k.iId <= 0 || FindKit(catalog, k.iId)) continue;
			k.sName = szName;
			const char* szToken = pKit->GetString("item_name", "");
			k.sToken = szToken[0] == '#' ? szToken + 1 : szToken;
			k.sCategory = KitCategory(k.sName);
			catalog.vKits.push_back(std::move(k));
		}
	}
	if (pisex::KeyValues* pTints = kv.FindKey("graffiti_tints"))
	{
		for (pisex::KeyValues* pTint = pTints->GetFirstTrueSubKey(); pTint; pTint = pTint->GetNextTrueSubKey())
		{
			Tint t;
			t.iId = pTint->GetInt("id", 0);
			t.sName = pTint->GetName();
			if (t.iId > 0) catalog.vTints.push_back(std::move(t));
		}
	}
	return true;
}

std::map<std::string, std::string> ParseLocalization(std::string_view text)
{
	// resource/csgo_<lang>.txt: "<token>" "<text>" pairs, one per line, \n escapes inside
	std::map<std::string, std::string> mapOut;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t end = text.find('\n', pos);
		if (end == std::string_view::npos) end = text.size();
		const char* p = text.data() + pos;
		const char* pEnd = text.data() + end;
		std::string parts[2];
		int n = 0;
		while (p < pEnd && n < 2)
		{
			while (p < pEnd && *p != '"') p++;
			if (p >= pEnd) break;
			p++;
			std::string s;
			while (p < pEnd && *p != '"')
			{
				if (*p == '\\' && p + 1 < pEnd) { p++; s += (*p == 'n') ? ' ' : *p; }
				else s += *p;
				p++;
			}
			p++;
			parts[n++] = s;
		}
		if (n == 2 && (strncasecmp(parts[0].c_str(), "SprayKit_", 9) == 0
					   || strncasecmp(parts[0].c_str(), "Attrib_SprayTintValue_", 22) == 0))
			mapOut[Lower(parts[0])] = parts[1];
		pos = end + 1;
	}
	return mapOut;
}

const Kit* FindKit(const Catalog& catalog, int iId)
{
	for (const Kit& k : catalog.vKits)
		if (k.iId == iId) return &k;
	return nullptr;
}

const Tint* FindTint(const Catalog& catalog, int iId)
{
	for (const Tint& t : catalog.vTints)
		if (t.iId == iId) return &t;
	return nullptr;
}

void FinalizeCatalog(Catalog& catalog, const std::map<std::string, std::string>& mapLocal,
					 const std::map<std::string, std::string>& mapEnglish)
{
	auto name = [&](const std::string& sToken, const std::string& sFallback) {
		const std::string sKey = Lower(sToken);
		auto it = mapLocal.find(sKey);
		if (it != mapLocal.end() && !it->second.empty()) return it->second;
		it = mapEnglish.find(sKey);
		if (it != mapEnglish.end() && !it->second.empty()) return it->second;
		return sFallback;
	};

	for (Kit& k : catalog.vKits) k.sDisplay = name(k.sToken, k.sName.size() > 6 ? k.sName.substr(6) : k.sName);
	for (Tint& t : catalog.vTints)
		t.sDisplay = name("Attrib_SprayTintValue_" + std::to_string(t.iId), t.sName);

	std::sort(catalog.vKits.begin(), catalog.vKits.end(),
			  [](const Kit& a, const Kit& b) { return a.sDisplay < b.sDisplay; });
	std::sort(catalog.vTints.begin(), catalog.vTints.end(),
			  [](const Tint& a, const Tint& b) { return a.iId < b.iId; });

	catalog.vCategories.clear();
	for (const char* sz : {"std", "std2", "std3", "vcap1", "illuminate1", "other"})
		for (const Kit& k : catalog.vKits)
			if (k.sCategory == sz) { catalog.vCategories.push_back(sz); break; }
}

const char* ValveLanguage(const std::string& sLang)
{
	static const std::map<std::string, const char*> map = {
		{"ru", "russian"}, {"en", "english"}, {"uk", "ukrainian"}, {"ua", "ukrainian"}, {"de", "german"},
		{"fr", "french"}, {"pl", "polish"}, {"pt", "brazilian"}, {"es", "spanish"}, {"tr", "turkish"},
		{"cs", "czech"}, {"zh", "schinese"}, {"ja", "japanese"}, {"ko", "koreana"}, {"it", "italian"},
	};
	auto it = map.find(sLang);
	return it != map.end() ? it->second : "english";
}

int PickKitId(const Catalog& catalog, int iChoice, uint32_t uRandom)
{
	if (const Kit* k = FindKit(catalog, iChoice)) return k->iId;
	if (catalog.vKits.empty()) return 0;
	return catalog.vKits[uRandom % catalog.vKits.size()].iId;
}

int PickTintId(const Catalog& catalog, int iChoice, uint32_t uRandom)
{
	if (const Tint* t = FindTint(catalog, iChoice)) return t->iId;
	if (catalog.vTints.empty()) return 0;
	return catalog.vTints[uRandom % catalog.vTints.size()].iId;
}

// ---------------------------------------------------------------- decal geometry

Vec3 Normalized(const Vec3& v)
{
	const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (len <= 1e-4f) return Vec3(0, 0, 0);
	return Vec3(v.x / len, v.y / len, v.z / len);
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
	return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

float Distance(const Vec3& a, const Vec3& b)
{
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

DecalGeometry ComputeDecalGeometry(const Vec3& vecStart, const Vec3& vecEnd, const Vec3& vecHitNormal)
{
	const Vec3 vecDelta = Vec3(vecEnd.x - vecStart.x, vecEnd.y - vecStart.y, vecEnd.z - vecStart.z);

	Vec3 vecNormal = Normalized(vecHitNormal);
	if (vecNormal.x == 0.0f && vecNormal.y == 0.0f && vecNormal.z == 0.0f)
		vecNormal = Normalized(Vec3(-vecDelta.x, -vecDelta.y, -vecDelta.z)); // reversed ray fallback

	const Vec3 vecUp(0, 0, 1);
	Vec3 vecLeft;
	if (fabsf(vecNormal.z) < 0.7f)
		vecLeft = Normalized(Cross(vecNormal, vecUp)); // wall: "left" is horizontal
	else
	{
		// floor or ceiling: the top of the design follows the view
		Vec3 vecForward = Normalized(Vec3(vecDelta.x, vecDelta.y, 0));
		if (vecForward.x == 0.0f && vecForward.y == 0.0f) vecForward = Vec3(1, 0, 0);
		vecLeft = Normalized(Cross(vecUp, vecForward));
	}
	return DecalGeometry{vecNormal, vecLeft};
}

// ---------------------------------------------------------------- phrases

const std::vector<DefaultPhrase>& DefaultPhrases()
{
	static const std::vector<DefaultPhrase> v = {
		{"Prefix", "{GREEN}[Граффити]{DEFAULT}", "{GREEN}[Graffiti]{DEFAULT}"},
		{"Menu_Title", "Граффити", "Graffiti"},
		{"Menu_Kit", "Рисунок: {name}", "Design: {name}"},
		{"Menu_Tint", "Цвет: {name}", "Color: {name}"},
		{"Menu_DoubleE", "Рисовать двойным E: {state}", "Spray with double E: {state}"},
		{"Menu_Help", "Как нарисовать", "How to spray"},
		{"Menu_Kits_Title", "Рисунок", "Design"},
		{"Menu_Tints_Title", "Цвет", "Color"},
		{"Random", "Случайный", "Random"},
		{"On", "вкл", "on"},
		{"Off", "выкл", "off"},
		{"Category_std", "Стандартные", "Standard"},
		{"Category_std2", "Стандартные II", "Standard II"},
		{"Category_std3", "Оружие", "Weapons"},
		{"Category_vcap1", "Капсула 1", "Capsule 1"},
		{"Category_illuminate1", "Illuminate", "Illuminate"},
		{"Category_other", "Особые", "Special"},
		{"Selected_Kit", "{PREFIX} Рисунок: {GREEN}{name}", "{PREFIX} Design: {GREEN}{name}"},
		{"Selected_Tint", "{PREFIX} Цвет: {GREEN}{name}", "{PREFIX} Color: {GREEN}{name}"},
		{"Cooldown", "{PREFIX} Следующее граффити через {RED}{sec}{DEFAULT} сек.", "{PREFIX} Next graffiti in {RED}{sec}{DEFAULT} s."},
		{"Too_Far", "{PREFIX} Подойдите ближе к стене", "{PREFIX} Move closer to a wall"},
		{"Bad_Surface", "{PREFIX} Здесь рисовать нельзя", "{PREFIX} You can't spray here"},
		{"Help", "{PREFIX} Посмотрите на стену и дважды нажмите {GREEN}E{DEFAULT} или нажмите клавишу с биндом {GREEN}bind t mm_spray{DEFAULT} (в консоли)",
		 "{PREFIX} Look at a wall and double-tap {GREEN}E{DEFAULT}, or use a key bound in console: {GREEN}bind t mm_spray"},
		{"DoubleE_Set", "{PREFIX} Двойное E: {GREEN}{state}", "{PREFIX} Double E: {GREEN}{state}"},
		{"Settings_Item", "Граффити двойным E", "Graffiti with double E"},
	};
	return v;
}

bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases)
{
	mapPhrases.clear();
	pisex::KeyValues kv("Phrases");
	if (!kv.LoadFromBuffer(text)) return false;
	for (pisex::KeyValues* pKey = kv.GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
	{
		const char* sz = pKey->GetString(sServerLang.c_str(), "");
		if (!sz[0]) sz = pKey->GetString("en", "");
		if (sz[0]) mapPhrases[pKey->GetName()] = sz;
	}
	return true;
}

std::string Phrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sServerLang,
				   const std::string& sKey)
{
	std::string s;
	auto it = mapPhrases.find(sKey);
	if (it != mapPhrases.end()) s = it->second;
	else
	{
		s = sKey;
		for (const DefaultPhrase& d : DefaultPhrases())
			if (sKey == d.szKey) { s = sServerLang == "ru" ? d.szRu : d.szEn; break; }
	}
	if (sKey != "Prefix") s = Replace(s, "{PREFIX}", Phrase(mapPhrases, sServerLang, "Prefix"));
	return s;
}

bool HasPhrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sKey)
{
	if (mapPhrases.count(sKey)) return true;
	for (const DefaultPhrase& d : DefaultPhrases())
		if (sKey == d.szKey) return true;
	return false;
}

// ---------------------------------------------------------------- double E

bool DoubleEState::Feed(bool bHeld, float flNow, float flWindow, bool bBusy)
{
	const bool bPressed = bHeld && !bUseHeld;
	bUseHeld = bHeld;
	if (!bPressed) return false;
	if (bBusy)
	{
		flLastPress = -1000.0f;
		return false;
	}
	if (flNow - flLastPress <= flWindow)
	{
		flLastPress = -1000.0f;
		return true;
	}
	flLastPress = flNow;
	return false;
}

void DoubleEState::Reset()
{
	flLastPress = -1000.0f;
	bUseHeld = false;
}

} // namespace gr
