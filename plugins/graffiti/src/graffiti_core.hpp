// graffiti — engine-free core of the port (config, the design/colour catalog of the game files,
// the decal geometry and the per-player state helpers).
//
// The reference is ref/src/cs2-graffiti (Metamod + utils): the server itself creates a
// player_spray_decal with a design (sticker kit name spray_*) and a tint, so no inventory item is
// needed. Designs, colours and their names come from the game files
// (scripts/items/items_game.txt and resource/csgo_<language>.txt), which is why the catalog is
// built from text here and only read from the VPK in src/plugin.cpp - test/
// graffiti_core_tests.cpp drives the parser with a format-faithful sample.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gr {

std::string Lower(std::string s);
std::string Trim(const std::string& s);
std::string Replace(std::string s, const std::string& from, const std::string& to);
std::vector<std::string> SplitList(const std::string& s);

struct Vec3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// ---------------------------------------------------------------- config

struct Config
{
	bool bEnabled = true;
	std::vector<std::string> vMenuCommands = {"!graffiti", "!spray"};
	std::vector<std::string> vSprayCommands;
	bool bDoubleE = true;
	float flDoubleEWindow = 0.35f;
	float flCooldown = 30.0f;
	float flMaxDistance = 150.0f;
	bool bOnePerPlayer = true;
	float flLifetime = 0.0f; // 0 = until the end of the round/map
	std::string sSound = "SprayCan.Paint";
	bool bUtilsSettings = true;
	int iDefaultKit = 0; // 0 = random
	int iDefaultTint = 0;
};

bool ParseConfigText(std::string_view text, Config& cfg);

// ---------------------------------------------------------------- catalog

struct Kit
{
	int iId = 0;
	std::string sName;    // spray_std_dollar
	std::string sToken;   // SprayKit_std_dollar
	std::string sDisplay; // localized name, the name without "spray_" when there is no translation
	std::string sCategory;
};

struct Tint
{
	int iId = 0;
	std::string sName; // brick_red
	std::string sDisplay;
};

struct Catalog
{
	std::vector<Kit> vKits;
	std::vector<Tint> vTints;
	std::vector<std::string> vCategories; // "std", "std2", "std3", "vcap1", "illuminate1", "other"
};

// Parses the sticker_kits / graffiti_tints blocks of items_game.txt. false when the text cannot be
// parsed as KeyValues at all.
bool ParseItemsGame(std::string_view text, Catalog& catalog);
// The localization pairs of resource/csgo_<lang>.txt: token (lower case) -> text.
std::map<std::string, std::string> ParseLocalization(std::string_view text);
// Fills sDisplay from the <lang> map with the english one as fallback, sorts the kits by display
// name and the tints by id, and builds vCategories in the order of the original.
void FinalizeCatalog(Catalog& catalog, const std::map<std::string, std::string>& mapLocal,
					 const std::map<std::string, std::string>& mapEnglish);

const Kit* FindKit(const Catalog& catalog, int iId);
const Tint* FindTint(const Catalog& catalog, int iId);
// "spray_<group>_<design>" -> "<group>", unknown groups -> "other".
std::string KitCategory(const std::string& sName);
// The valve spelling of a normalized language ("ru" -> "russian").
const char* ValveLanguage(const std::string& sLang);

// The id a spray uses: the player's choice when it still exists, otherwise a random one (0 when the
// catalog is empty).
int PickKitId(const Catalog& catalog, int iChoice, uint32_t uRandom);
int PickTintId(const Catalog& catalog, int iChoice, uint32_t uRandom);

// ---------------------------------------------------------------- decal geometry

struct DecalGeometry
{
	Vec3 vecNormal;
	Vec3 vecLeft;
};

// The reference's TrySpray geometry: the normal is the hit normal (falling back to the reversed
// ray when the trace did not report one); "left" is horizontal on a wall and follows the view on
// floors/ceilings.
DecalGeometry ComputeDecalGeometry(const Vec3& vecStart, const Vec3& vecEnd, const Vec3& vecHitNormal);

Vec3 Normalized(const Vec3& v);
Vec3 Cross(const Vec3& a, const Vec3& b);
float Distance(const Vec3& a, const Vec3& b);

// ---------------------------------------------------------------- phrases

struct DefaultPhrase
{
	const char* szKey;
	const char* szRu;
	const char* szEn;
};
const std::vector<DefaultPhrase>& DefaultPhrases();
bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases);
std::string Phrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sServerLang,
				   const std::string& sKey);
bool HasPhrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sKey);

// ---------------------------------------------------------------- player state helpers

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; 0 is also the "none"
// this port's own helpers return). The sign means nothing - once the serial counter passes 65535
// bit 31 is set and the handle is a negative int; every `h > 0` test silently drops those entities.
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// The double-E detector of the reference's GameFrame: a rising edge of IN_USE, two presses within
// the window spray.
struct DoubleEState
{
	float flLastPress = -1000.0f;
	bool bUseHeld = false;

	// bHeld: IN_USE of this tick; flNow: curtime; bBusy: the player is defusing or carrying a
	// hostage (E is doing something useful then - the pair is forgotten). Returns true when the
	// press completes a double press (and clears the timestamp so a third press starts a new pair).
	bool Feed(bool bHeld, float flNow, float flWindow, bool bBusy);
	void Reset();
};

} // namespace gr
