// Tests for plugins/graffiti/src/graffiti_core.hpp (the engine-free core of the ported graffiti
// plugin).
//
//   * config parsing against the live server's graffiti.ini
//     (ref/prod-27016/addons/configs/graffiti.ini),
//   * phrase parsing against ref/prod-27016/addons/translations/graffiti.phrases.txt,
//   * the catalog parser against a format-faithful sample of scripts/items/items_game.txt and
//     resource/csgo_russian.txt (test/data/; the real files live in the game install),
//   * the decal geometry (wall vs floor/ceiling, the reversed-ray fallback) and the double-E
//     detector of the reference's frame loop.

#include <plg/plugin.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "graffiti_core.hpp"

// The plugin gets these from PLUGIFY_PLUGIN; pisexcompat references them, the test leaves them null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

#ifndef GR_TEST_DATA_DIR
#define GR_TEST_DATA_DIR "."
#endif
#ifndef GR_PROD_DATA_DIR
#define GR_PROD_DATA_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name)
{
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),             \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_ = (actual);                                                           \
		const std::string e_ = (expected);                                                         \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_, e_);                                                                   \
		}                                                                                          \
	} while (false)

std::string ReadFileText(const std::string& sPath)
{
	std::ifstream file(sPath, std::ios::binary);
	if (!file) {
		std::printf("FATAL: cannot read %s\n", sPath.c_str());
		++g_failures;
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

void TestConfig()
{
	Section("config: the production graffiti.ini");
	gr::Config cfg;
	CHECK(gr::ParseConfigText(ReadFileText(std::string(GR_PROD_DATA_DIR) + "/configs/graffiti.ini"), cfg));
	CHECK(cfg.bEnabled);
	CHECK_EQ_INT(cfg.vMenuCommands.size(), 2);
	CHECK_EQ_STR(cfg.vMenuCommands[0], "!graffiti");
	CHECK_EQ_STR(cfg.vMenuCommands[1], "!spray");
	CHECK(cfg.vSprayCommands.empty());
	CHECK(cfg.bDoubleE);
	CHECK(cfg.flDoubleEWindow > 0.34f && cfg.flDoubleEWindow < 0.36f);
	CHECK(cfg.flCooldown > 29.9f && cfg.flCooldown < 30.1f);
	CHECK(cfg.flMaxDistance > 149.9f && cfg.flMaxDistance < 150.1f);
	CHECK(cfg.bOnePerPlayer);
	CHECK(cfg.flLifetime == 0.0f);
	CHECK_EQ_STR(cfg.sSound, "SprayCan.Paint");
	CHECK(cfg.bUtilsSettings);
	CHECK_EQ_INT(cfg.iDefaultKit, 0);
	CHECK_EQ_INT(cfg.iDefaultTint, 0);

	Section("config: in-memory variants and clamps");
	gr::Config cfg2;
	CHECK(gr::ParseConfigText(R"(
"Graffiti"
{
	"enabled" "0"
	"commands" "!g"
	"spray_commands" "!s1, !s2"
	"double_e_window" "0.0"
	"cooldown" "-5"
	"max_distance" "1"
	"one_per_player" "0"
	"lifetime" "-2"
	"sound" ""
	"default_kit" "5"
	"default_tint" "3"
}
)", cfg2));
	CHECK(!cfg2.bEnabled);
	CHECK_EQ_INT(cfg2.vMenuCommands.size(), 1);
	CHECK_EQ_INT(cfg2.vSprayCommands.size(), 2);
	CHECK(cfg2.flDoubleEWindow >= 0.1f); // clamped
	CHECK(cfg2.flCooldown == 0.0f);
	CHECK(cfg2.flMaxDistance == 32.0f); // clamped
	CHECK(!cfg2.bOnePerPlayer);
	CHECK(cfg2.flLifetime == 0.0f);
	CHECK(cfg2.sSound.empty());
	CHECK_EQ_INT(cfg2.iDefaultKit, 5);
	CHECK_EQ_INT(cfg2.iDefaultTint, 3);
}

gr::Catalog BuildSampleCatalog(std::map<std::string, std::string>& mapOut)
{
	gr::Catalog catalog;
	Section("catalog: items_game sample");
	CHECK(gr::ParseItemsGame(ReadFileText(std::string(GR_TEST_DATA_DIR) + "/items_game_sample.txt"), catalog));
	// 7 sprays across both sticker_kits blocks (the duplicate id 1 and the id 0 entry are dropped),
	// the sticker_* kit is not a spray
	CHECK_EQ_INT(catalog.vKits.size(), 7);
	CHECK_EQ_INT(catalog.vTints.size(), 2);

	std::map<std::string, std::string> mapRu =
		gr::ParseLocalization(ReadFileText(std::string(GR_TEST_DATA_DIR) + "/csgo_russian_sample.txt"));
	mapOut = mapRu;
	CHECK_EQ_INT(mapRu.size(), 7); // 5 SprayKit_ + 2 Attrib_SprayTintValue_ (SomeOther_Token dropped)
	CHECK_EQ_STR(mapRu.at("spraykit_std_dollar"), "Доллар");
	CHECK_EQ_STR(mapRu.at("spraykit_illuminate1_glow"), "Свечение в темноте"); // \n escape

	gr::FinalizeCatalog(catalog, mapRu, {});
	return catalog;
}

void TestCatalog()
{
	std::map<std::string, std::string> mapRu;
	gr::Catalog catalog = BuildSampleCatalog(mapRu);

	Section("catalog: display names, categories, sorting");
	const gr::Kit* k = gr::FindKit(catalog, 1);
	CHECK(k != nullptr);
	if (k)
	{
		CHECK_EQ_STR(k->sName, "spray_std_dollar");
		CHECK_EQ_STR(k->sToken, "SprayKit_std_dollar");
		CHECK_EQ_STR(k->sDisplay, "Доллар");
		CHECK_EQ_STR(k->sCategory, "std");
	}
	// no translation -> the name without the spray_ prefix
	k = gr::FindKit(catalog, 7);
	CHECK(k != nullptr && k->sDisplay == "std3_notranslation");
	// unknown group -> "other"
	k = gr::FindKit(catalog, 5);
	CHECK(k != nullptr && k->sCategory == "other");
	// a group that is not in the list either
	CHECK_EQ_STR(gr::KitCategory("spray_std_dollar"), "std");
	CHECK_EQ_STR(gr::KitCategory("spray_std2_x"), "std2");
	CHECK_EQ_STR(gr::KitCategory("spray_std3_x"), "std3");
	CHECK_EQ_STR(gr::KitCategory("spray_vcap1_x"), "vcap1");
	CHECK_EQ_STR(gr::KitCategory("spray_illuminate1_x"), "illuminate1");
	CHECK_EQ_STR(gr::KitCategory("spray_vcap2_x"), "other");

	// categories in the fixed order of the original, empty ones skipped (the sample has all six)
	CHECK_EQ_INT(catalog.vCategories.size(), 6);
	CHECK_EQ_STR(catalog.vCategories[0], "std");
	CHECK_EQ_STR(catalog.vCategories[1], "std2");
	CHECK_EQ_STR(catalog.vCategories[2], "std3");
	CHECK_EQ_STR(catalog.vCategories[3], "vcap1");
	CHECK_EQ_STR(catalog.vCategories[4], "illuminate1");
	CHECK_EQ_STR(catalog.vCategories[5], "other");

	// tints sorted by id
	CHECK_EQ_INT(catalog.vTints[0].iId, 1);
	CHECK_EQ_STR(catalog.vTints[0].sDisplay, "Кирпично-красный");
	CHECK_EQ_STR(catalog.vTints[1].sDisplay, "Синий");
	const gr::Tint* t = gr::FindTint(catalog, 2);
	CHECK(t != nullptr && t->sName == "navy_blue");
	CHECK(gr::FindTint(catalog, 9) == nullptr);

	// an english fallback is used when the local text is missing
	gr::Catalog catalog2;
	CHECK(gr::ParseItemsGame(ReadFileText(std::string(GR_TEST_DATA_DIR) + "/items_game_sample.txt"), catalog2));
	gr::FinalizeCatalog(catalog2, {}, {{"spraykit_std_dollar", "Dollar"}});
	CHECK_EQ_STR(gr::FindKit(catalog2, 1)->sDisplay, "Dollar");
	CHECK_EQ_STR(gr::FindKit(catalog2, 2)->sDisplay, "std2_heart"); // neither language has it

	Section("catalog: picking");
	CHECK_EQ_INT(gr::PickKitId(catalog, 1, 0), 1); // the player's choice, still valid
	// an unknown choice (or 0) means random: the catalogue entry the random value lands on
	CHECK_EQ_INT(gr::PickKitId(catalog, 999, 5), catalog.vKits[5].iId);
	CHECK_EQ_INT(gr::PickKitId(catalog, 0, 5), catalog.vKits[5].iId);
	{
		// random: the result is always a real kit and covers the whole catalog for some sequence
		std::vector<int> vSeen(catalog.vKits.size(), 0);
		for (uint32_t i = 0; i < 64; i++)
		{
			const int iKit = gr::PickKitId(catalog, 0, i);
			bool bFound = false;
			for (size_t j = 0; j < catalog.vKits.size(); j++)
				if (catalog.vKits[j].iId == iKit) { vSeen[j]++; bFound = true; }
			CHECK(bFound);
		}
		int nSeen = 0;
		for (int n : vSeen) nSeen += n > 0 ? 1 : 0;
		CHECK_EQ_INT(nSeen, (int)catalog.vKits.size());
	}
	{
		gr::Catalog empty;
		CHECK_EQ_INT(gr::PickKitId(empty, 0, 1), 0);
		CHECK_EQ_INT(gr::PickTintId(empty, 0, 1), 0);
	}
	CHECK_EQ_INT(gr::PickTintId(catalog, 2, 0), 2);
	CHECK_EQ_INT(gr::PickTintId(catalog, 42, 3), catalog.vTints[1].iId); // 3 % 2

	CHECK_EQ_STR(gr::ValveLanguage("ru"), "russian");
	CHECK_EQ_STR(gr::ValveLanguage("en"), "english");
	CHECK_EQ_STR(gr::ValveLanguage("xx"), "english");
}

void TestGeometry()
{
	Section("decal geometry");
	// wall: the normal is horizontal, "left" is cross(normal, up)
	{
		const gr::DecalGeometry g = gr::ComputeDecalGeometry({0, 0, 64}, {100, 0, 64}, {0, -1, 0});
		CHECK(g.vecNormal.x == 0.0f && g.vecNormal.y == -1.0f && g.vecNormal.z == 0.0f);
		// cross((0,-1,0), (0,0,1)) = (-1, 0, 0)
		CHECK(g.vecLeft.x == -1.0f && g.vecLeft.y == 0.0f && g.vecLeft.z == 0.0f);
	}
	// floor: the top of the design follows the view direction
	{
		const gr::DecalGeometry g = gr::ComputeDecalGeometry({0, 0, 64}, {100, 0, 0}, {0, 0, 1});
		CHECK(g.vecNormal.x == 0.0f && g.vecNormal.y == 0.0f && g.vecNormal.z == 1.0f);
		// forward = (1,0,0), left = cross(up, forward) = (0,1,0)
		CHECK(g.vecLeft.x == 0.0f && g.vecLeft.y == 1.0f && g.vecLeft.z == 0.0f);
	}
	// a missing normal falls back to the reversed ray
	{
		const gr::DecalGeometry g = gr::ComputeDecalGeometry({0, 0, 64}, {100, 0, 64}, {0, 0, 0});
		CHECK(g.vecNormal.x < -0.99f && g.vecNormal.y == 0.0f && g.vecNormal.z == 0.0f);
	}
	// helpers
	const gr::Vec3 v = gr::Normalized({0, 3, 4});
	CHECK(v.y > 0.59f && v.y < 0.61f && v.z > 0.79f && v.z < 0.81f);
	CHECK(gr::Normalized({0, 0, 0}).x == 0.0f);
	CHECK(gr::Distance({0, 0, 0}, {3, 4, 0}) > 4.99f && gr::Distance({0, 0, 0}, {3, 4, 0}) < 5.01f);
	const gr::Vec3 c = gr::Cross({1, 0, 0}, {0, 1, 0});
	CHECK(c.z == 1.0f);
}

void TestDoubleE()
{
	Section("double E detector");
	gr::DoubleEState s;
	CHECK(!s.Feed(true, 10.00f, 0.35f, false));  // first press
	CHECK(!s.Feed(true, 10.10f, 0.35f, false));  // held, no new press
	CHECK(!s.Feed(false, 10.15f, 0.35f, false)); // released
	CHECK(s.Feed(true, 10.20f, 0.35f, false));   // second press within the window -> spray
	CHECK(!s.Feed(true, 10.30f, 0.35f, false));  // still the same press, pair already used
	CHECK(!s.Feed(false, 10.35f, 0.35f, false));
	CHECK(!s.Feed(true, 11.00f, 0.35f, false)); // too late -> new first press
	CHECK(!s.Feed(false, 11.10f, 0.35f, false));
	CHECK(s.Feed(true, 11.20f, 0.35f, false));

	// defusing/hostage (bBusy) breaks the pair
	gr::DoubleEState busy;
	CHECK(!busy.Feed(true, 5.0f, 0.35f, false));
	CHECK(!busy.Feed(false, 5.05f, 0.35f, false));
	CHECK(!busy.Feed(true, 5.10f, 0.35f, true)); // busy: E is doing something useful
	CHECK(!busy.Feed(false, 5.15f, 0.35f, false));
	CHECK(!busy.Feed(true, 5.20f, 0.35f, false)); // the pair was forgotten
	CHECK(!busy.Feed(false, 5.25f, 0.35f, false));
	CHECK(busy.Feed(true, 5.30f, 0.35f, false));
}

void TestHandleValidity()
{
	Section("entity handle validity");
	// The live-server case: index 93, serial 102949 -> 0xC912805D = -921534371, a *valid* handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	CHECK(gr::IsValidHandle(-921534371));
	CHECK(gr::IsValidHandle(0x7FFFFFFF));
	CHECK(gr::IsValidHandle(1));
	CHECK(!gr::IsValidHandle(0));  // "none" / index 0 (the world)
	CHECK(!gr::IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

void TestPhrases()
{
	Section("phrases: the production file");
	std::map<std::string, std::string> ru;
	std::map<std::string, std::string> en;
	CHECK(gr::ParsePhrasesText(ReadFileText(std::string(GR_PROD_DATA_DIR) + "/translations/graffiti.phrases.txt"), "ru", ru));
	CHECK(gr::ParsePhrasesText(ReadFileText(std::string(GR_PROD_DATA_DIR) + "/translations/graffiti.phrases.txt"), "en", en));
	CHECK(!ru.empty());
	CHECK(!en.empty());
	CHECK(gr::Phrase(ru, "ru", "Menu_Title").size() > 2);
	CHECK(gr::Phrase(ru, "ru", "Cooldown").find("{sec}") != std::string::npos);
	CHECK_EQ_STR(gr::Phrase(en, "en", "Random"), "Random");
	CHECK(gr::HasPhrase(ru, "Category_std"));
	CHECK(gr::HasPhrase(ru, "No_Such_Key_In_File") == false);
	CHECK(gr::HasPhrase(ru, "Menu_Help")); // built-in default
	// unknown key -> the key itself
	CHECK_EQ_STR(gr::Phrase(ru, "ru", "Missing_Key"), "Missing_Key");
}

} // namespace

int main()
{
	TestConfig();
	TestCatalog();
	TestGeometry();
	TestDoubleE();
	TestHandleValidity();
	TestPhrases();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
