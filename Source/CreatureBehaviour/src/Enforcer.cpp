/*
 * Creature Behaviour
 * Copyright (c) 2026 CageTV
 *
 * Licensed under the GNU General Public License, version 3 or later; see LICENSE.
 */
#include "PCH.h"
#include "Enforcer.h"

#include <nlohmann/json.hpp>

#ifdef AddForm
#	undef AddForm  // winspool.h macro
#endif

// The records live in our plugin; this makes the same changes survive another mod overriding them.
// Every rule is idempotent: nothing is touched when the record already has what the rule wants.
//   ingestible_effects : { "form": "Plugin|ID", "effects": ["Plugin|ID", ...] }   effect is added when missing
//   formlist_add       : { "list": "Plugin|ID", "items": ["Plugin|ID", ...] }
//   keyword_add        : { "forms": ["Plugin|ID", ...], "keywords": ["Plugin|ID", ...] }  any keyword-bearing form
//   weapon_keyword     : { "nameContains": [..], "nameExclude": [..], "requireAny": [kw..], "requireAll": [kw..], "keyword": kw }
//   armor_warmth       : { "warm": kw, "cold": kw, "warmIfAny": [kw..], "coldIfAny": [kw..], "slotMask": n }
//   player_perks       : ["Plugin|ID", ...]
//   leveled_edit       : { "list": "Plugin|ID", "add": [{"form": "Plugin|ID", "level": n, "count": n, "times": n}], "remove": [...] }  leveled NPC list entries;
//                        an entry is added until the list holds "times" copies of it, so it is idempotent; a form from a mod that is not loaded is skipped
//   race_spell_add     : { "races": ["Plugin|ID", ...], "spells": ["Plugin|ID", ...] }   a spell, ability or power is added to the race
//   npc_spell_add      : { "npcs": [...], "spells": [...] }
//   npc_perk_add       : { "npcs": [...], "perks": [...] }
//   leveled_edit may also carry "flags_set": n (bits of the leveled list flags to turn on)
//   any rule may carry "requires": "Plugin.esp" and is then skipped, quietly, when that plugin is not loaded
namespace Enforcer
{
	namespace
	{
		std::filesystem::path ConfigPath()
		{
			return std::filesystem::path("Data/SKSE/Plugins/CreatureBehaviour/enforce.json");
		}

		RE::TESForm* Find(const std::string& a_ref)
		{
			const auto bar = a_ref.find('|');
			if (bar == std::string::npos) {
				return nullptr;
			}
			std::uint32_t id = 0;
			try {
				id = static_cast<std::uint32_t>(std::stoul(a_ref.substr(bar + 1), nullptr, 16));
			} catch (...) {
				return nullptr;
			}
			return RE::TESDataHandler::GetSingleton()->LookupForm(id, a_ref.substr(0, bar));
		}

		std::vector<std::string> Strings(const nlohmann::json& a_j, const char* a_key)
		{
			std::vector<std::string> out;
			if (a_j.contains(a_key) && a_j[a_key].is_array()) {
				for (const auto& v : a_j[a_key]) {
					if (v.is_string()) {
						out.push_back(v.get<std::string>());
					}
				}
			}
			return out;
		}

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		struct Tally
		{
			int rules{ 0 }, changed{ 0 }, already{ 0 }, missing{ 0 };
		};

		void IngestibleEffects(const nlohmann::json& a_r, Tally& t)
		{
			auto* f0 = Find(a_r.value("form", ""));
			auto* alch = f0 ? f0->As<RE::AlchemyItem>() : nullptr;
			if (!alch) {
				SKSE::log::warn("ingestible_effects: {} not found, skipped", a_r.value("form", ""));
				++t.missing;
				return;
			}
			for (const auto& ref : Strings(a_r, "effects")) {
				auto* f1 = Find(ref);
				auto* base = f1 ? f1->As<RE::EffectSetting>() : nullptr;
				if (!base) {
					SKSE::log::warn("ingestible_effects: effect {} not found", ref);
					++t.missing;
					continue;
				}
				bool has = false;
				for (auto* e : alch->effects) {
					if (e && e->baseEffect == base) {
						has = true;
						break;
					}
				}
				if (has) {
					++t.already;
					continue;
				}
				auto* e = new RE::Effect();
				e->baseEffect = base;
				e->effectItem.magnitude = 0.0f;
				e->effectItem.area = 0;
				e->effectItem.duration = 0;
				e->cost = base->data.baseCost;
				alch->effects.push_back(e);
				SKSE::log::info("  + effect {} on {:08X} {}", ref, alch->GetFormID(), alch->GetName());
				++t.changed;
			}
		}

		void FormListAdd(const nlohmann::json& a_r, Tally& t)
		{
			auto* f0 = Find(a_r.value("list", ""));
			auto* list = f0 ? f0->As<RE::BGSListForm>() : nullptr;
			if (!list) {
				SKSE::log::warn("formlist_add: {} not found, skipped", a_r.value("list", ""));
				++t.missing;
				return;
			}
			for (const auto& ref : Strings(a_r, "items")) {
				auto* f = Find(ref);
				if (!f) {
					SKSE::log::warn("formlist_add: item {} not found", ref);
					++t.missing;
					continue;
				}
				if (list->HasForm(f)) {
					++t.already;
					continue;
				}
				list->AddForm(f);
				SKSE::log::info("  + {:08X} in list {:08X}", f->GetFormID(), list->GetFormID());
				++t.changed;
			}
		}

		std::vector<RE::BGSKeyword*> Keywords(const std::vector<std::string>& a_refs, Tally& t)
		{
			std::vector<RE::BGSKeyword*> out;
			for (const auto& ref : a_refs) {
				auto* f = Find(ref);
				auto* k = f ? f->As<RE::BGSKeyword>() : nullptr;
				if (k) {
					out.push_back(k);
				} else {
					SKSE::log::warn("keyword {} not found", ref);
					++t.missing;
				}
			}
			return out;
		}

		void KeywordAdd(const nlohmann::json& a_r, Tally& t)
		{
			const auto kws = Keywords(Strings(a_r, "keywords"), t);
			for (const auto& ref : Strings(a_r, "forms")) {
				auto* f = Find(ref);
				auto* kf = f ? f->As<RE::BGSKeywordForm>() : nullptr;
				if (!kf) {
					SKSE::log::warn("keyword_add: {} not found or has no keywords", ref);
					++t.missing;
					continue;
				}
				for (auto* k : kws) {
					if (kf->HasKeyword(k)) {
						++t.already;
					} else if (kf->AddKeyword(k)) {
						SKSE::log::info("  + keyword {} on {:08X} {}", k->GetFormEditorID(), f->GetFormID(), f->GetName());
						++t.changed;
					}
				}
			}
		}

		void WeaponKeyword(const nlohmann::json& a_r, Tally& t)
		{
			const auto kw = Keywords({ a_r.value("keyword", "") }, t);
			if (kw.empty()) {
				return;
			}
			const auto any = Keywords(Strings(a_r, "requireAny"), t);
			const auto all = Keywords(Strings(a_r, "requireAll"), t);
			std::vector<std::string> inc, exc;
			for (auto& s : Strings(a_r, "nameContains")) {
				inc.push_back(Lower(s));
			}
			for (auto& s : Strings(a_r, "nameExclude")) {
				exc.push_back(Lower(s));
			}
			for (auto* w : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectWEAP>()) {
				if (!w) {
					continue;
				}
				if (!inc.empty()) {
					const std::string n = Lower(w->GetName());
					bool ok = std::any_of(inc.begin(), inc.end(), [&](const std::string& s) { return n.find(s) != std::string::npos; });
					if (ok && std::any_of(exc.begin(), exc.end(), [&](const std::string& s) { return n.find(s) != std::string::npos; })) {
						ok = false;
					}
					if (!ok) {
						continue;
					}
				}
				if (!all.empty() && !std::all_of(all.begin(), all.end(), [&](auto* k) { return w->HasKeyword(k); })) {
					continue;
				}
				if (!any.empty() && !std::any_of(any.begin(), any.end(), [&](auto* k) { return w->HasKeyword(k); })) {
					continue;
				}
				if (w->HasKeyword(kw[0])) {
					++t.already;
				} else if (w->AddKeyword(kw[0])) {
					SKSE::log::info("  + {} on weapon {:08X} {}", kw[0]->GetFormEditorID(), w->GetFormID(), w->GetName());
					++t.changed;
				}
			}
		}

		// vanilla Survival Mode marks armor warm or cold with keywords; mods and Requiem drop that. Armor that has neither
		// keyword gets the vanilla default: heavy/light armor is warm, poor clothing is cold. Jewelry, shields and
		// anything outside the slot mask are left alone.
		void ArmorWarmth(const nlohmann::json& a_r, Tally& t)
		{
			const auto warm = Keywords({ a_r.value("warm", "") }, t);
			const auto cold = Keywords({ a_r.value("cold", "") }, t);
			if (warm.empty() || cold.empty()) {
				return;
			}
			const auto warmIf = Keywords(Strings(a_r, "warmIfAny"), t);
			const auto coldIf = Keywords(Strings(a_r, "coldIfAny"), t);
			const std::uint32_t mask = a_r.value("slotMask", 0x108Fu);
			for (auto* a : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectARMO>()) {
				if (!a || a->HasKeyword(warm[0]) || a->HasKeyword(cold[0])) {
					if (a) {
						++t.already;
					}
					continue;
				}
				if ((static_cast<std::uint32_t>(a->GetSlotMask().underlying()) & mask) == 0) {
					continue;
				}
				const auto any = [&](const std::vector<RE::BGSKeyword*>& a_k) {
					return std::any_of(a_k.begin(), a_k.end(), [&](auto* k) { return a->HasKeyword(k); });
				};
				RE::BGSKeyword* pick = any(coldIf) ? cold[0] : any(warmIf) ? warm[0] : nullptr;
				if (pick && a->AddKeyword(pick)) {
					++t.changed;
				}
			}
			SKSE::log::info("  armor warmth pass done");
		}

		// Creatures from other mods have their own NPC records and so miss the daily schedule (feeding, hunting, resting) that
		// the Savage World plugin attaches to its own creatures. Every non-unique NPC of a listed race that does not already
		// carry a list from the schedule plugin gets the race's schedule list.
		void PackageListByRace(const nlohmann::json& a_rules, Tally& t)
		{
			struct Rule
			{
				RE::TESRace*     race;
				RE::BGSListForm* list;
				int              applied{ 0 };
				std::string      name;
			};
			std::vector<Rule> rules;
			for (const auto& r : a_rules) {
				auto* f0 = Find(r.value("race", ""));
				auto* f1 = Find(r.value("list", ""));
				auto* race = f0 ? f0->As<RE::TESRace>() : nullptr;
				auto* list = f1 ? f1->As<RE::BGSListForm>() : nullptr;
				if (!race || !list) {
					++t.missing;
					continue;
				}
				rules.push_back({ race, list, 0, r.value("_name", "") });
			}
			if (rules.empty()) {
				return;
			}
			const auto owner = [](RE::BGSListForm* a_l) {
				const auto* file = a_l ? a_l->GetFile(0) : nullptr;
				return file ? std::string(file->GetFilename()) : std::string();
			};
			int total = 0, skippedUnique = 0;
			for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
				// templated NPCs take race and AI from their template (many SICSE records carry a placeholder race)
				if (!npc || !npc->race || npc->baseTemplateForm) {
					continue;
				}
				for (auto& r : rules) {
					if (npc->race != r.race) {
						continue;
					}
					if (npc->IsUnique()) {
						++skippedUnique;
						break;
					}
					if (npc->defaultPackList == r.list) {
						++t.already;
						break;
					}
					const auto cur = owner(npc->defaultPackList);
					if (!cur.empty() && cur == owner(r.list)) {
						++t.already;
						break;
					}
					npc->defaultPackList = r.list;
					++r.applied;
					++total;
					++t.changed;
					break;
				}
			}
			for (const auto& r : rules) {
				if (r.applied > 0) {
					SKSE::log::info("  schedule list {:08X} given to {} creatures of race {}", r.list->GetFormID(), r.applied, r.name);
				}
			}
			SKSE::log::info("  package_list_by_race: {} creatures got a schedule list, {} unique creatures skipped", total, skippedUnique);
		}

		// Prey animals must run, not fight. Mods give deer and elk bulls Average/Brave confidence with an aggro radius, so they
		// attack the player and each other on sight. Cowardly + Unaggressive + HelpsNobody makes them flee instead; the aggro
		// radius then works as a flee radius (vanilla deer). Unique NPCs are skipped, and templated NPCs follow their template.
		void AiByRace(const nlohmann::json& a_rules, Tally& t)
		{
			int total = 0;
			for (const auto& r : a_rules) {
				auto* f0 = Find(r.value("race", ""));
				auto* race = f0 ? f0->As<RE::TESRace>() : nullptr;
				if (!race) {
					++t.missing;
					continue;
				}
				const auto aggr = static_cast<RE::ACTOR_AGGRESSION>(r.value("aggression", 0));
				const auto conf = static_cast<RE::ACTOR_CONFIDENCE>(r.value("confidence", 0));
				const auto assi = static_cast<RE::ACTOR_ASSISTANCE>(r.value("assistance", 0));
				// optional: "aggroRadius": false switches the aggro radius off (the animal then only fights back when attacked)
				const bool setRadius = r.contains("aggroRadius");
				const bool radiusOn = r.value("aggroRadius", true);
				int        n = 0;
				for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
					if (!npc || npc->race != race || npc->IsUnique()) {
						continue;
					}
					const bool radiusOk = !setRadius || npc->aiData.aggroRadiusBehaviour == radiusOn;
					if (npc->GetAggressionLevel() == aggr && npc->GetConfidenceLevel() == conf && npc->GetAssistanceLevel() == assi && radiusOk) {
						++t.already;
						continue;
					}
					npc->SetAggressionLevel(aggr);
					npc->SetConfidenceLevel(conf);
					npc->SetAssistanceLevel(assi);
					if (setRadius) {
						npc->aiData.aggroRadiusBehaviour = radiusOn;
					}
					++n;
					++t.changed;
				}
				total += n;
				SKSE::log::info("  ai_by_race: {} {} NPC(s) made passive (aggression {}, confidence {})", r.value("_name", std::string("?")), n, r.value("aggression", 0), r.value("confidence", 0));
			}
			SKSE::log::info("  ai_by_race: {} NPC(s) changed", total);
		}

		// Placed references of other mods that should never be there (for example the SkyTEST rats and spiders in the Helgen cave).
		// The initially-disabled flag is set before the cell loads, so they never appear.
		void LeveledEdit(const nlohmann::json& a_r, Tally& t)
		{
			auto* f0 = Find(a_r.value("list", ""));
			auto* list = f0 ? f0->As<RE::TESLevCharacter>() : nullptr;
			if (!list) {
				++t.missing;
				return;
			}
			const auto holds = [&](const RE::TESForm* a_f, int a_level, int a_count) {
				int n = 0;
				for (std::uint8_t i = 0; i < list->numEntries; ++i) {
					const auto& e = list->entries[i];
					n += (e.form == a_f && e.level == a_level && e.count == a_count) ? 1 : 0;
				}
				return n;
			};
			if (a_r.contains("flags_set") && a_r["flags_set"].is_number_integer()) {
				const auto bits = static_cast<std::uint8_t>(a_r["flags_set"].get<int>());
				if ((static_cast<std::uint8_t>(list->llFlags) & bits) != bits) {
					list->llFlags = static_cast<RE::TESLeveledList::Flag>(static_cast<std::uint8_t>(list->llFlags) | bits);
					++t.changed;
				}
			}
			// removals first (compact in place)
			if (a_r.contains("remove") && a_r["remove"].is_array()) {
				for (const auto& it : a_r["remove"]) {
					auto* f = Find(it.value("form", ""));
					if (!f) {
						continue;
					}
					int left = it.value("times", 1);
					const int level = it.value("level", 1), cnt = it.value("count", 1);
					std::uint8_t out = 0;
					for (std::uint8_t i = 0; i < list->numEntries; ++i) {
						const auto& e = list->entries[i];
						if (left > 0 && e.form == f && e.level == level && e.count == cnt) {
							--left;
							++t.changed;
							continue;
						}
						if (out != i) {
							list->entries[out] = list->entries[i];
						}
						++out;
					}
					list->numEntries = out;
				}
			}
			struct Add
			{
				RE::TESForm* form;
				int          level, count;
			};
			std::vector<Add> adds;
			if (a_r.contains("add") && a_r["add"].is_array()) {
				for (const auto& it : a_r["add"]) {
					auto* f = Find(it.value("form", ""));
					if (!f) {
						++t.missing;
						continue;
					}
					const int level = it.value("level", 1), cnt = it.value("count", 1), times = it.value("times", 1);
					const int need = times - holds(f, level, cnt) - static_cast<int>(std::count_if(adds.begin(), adds.end(), [&](const Add& a) { return a.form == f && a.level == level && a.count == cnt; }));
					if (need <= 0) {
						++t.already;
						continue;
					}
					for (int k = 0; k < need; ++k) {
						adds.push_back({ f, level, cnt });
					}
				}
			}
			if (adds.empty()) {
				return;
			}
			const std::size_t room = 255u - list->numEntries;
			if (adds.size() > room) {
				SKSE::log::warn("leveled_edit: {} would get {} entries, only {} fit in a leveled list; the rest is dropped", a_r.value("_name", a_r.value("list", std::string("?"))), adds.size(), room);
				adds.resize(room);
			}
			const std::size_t total = list->numEntries + adds.size();
			if (list->entries.size() < total) {
				list->entries.resize(total);
			}
			for (const auto& a : adds) {
				auto& e = list->entries[list->numEntries++];
				e.form = a.form;
				e.count = static_cast<std::uint16_t>(a.count);
				e.level = static_cast<std::uint16_t>(a.level);
				e.pad0C = 0;
				e.itemExtra = nullptr;
				++t.changed;
			}
		}

		std::vector<RE::TESForm*> FindAll(const nlohmann::json& a_r, const char* a_key)
		{
			std::vector<RE::TESForm*> out;
			for (const auto& ref : Strings(a_r, a_key)) {
				if (auto* f = Find(ref)) {
					out.push_back(f);
				}
			}
			return out;
		}

		void AddSpellTo(RE::TESSpellList::SpellData* a_list, RE::SpellItem* a_spell, Tally& t)
		{
			if (!a_list) {
				++t.missing;
				return;
			}
			if (a_list->GetIndex(a_spell).has_value()) {
				++t.already;
				return;
			}
			if (a_list->AddSpell(a_spell)) {
				++t.changed;
			}
		}

		void RaceSpellAdd(const nlohmann::json& a_r, Tally& t)
		{
			const auto spells = FindAll(a_r, "spells");
			for (auto* f : FindAll(a_r, "races")) {
				auto* race = f->As<RE::TESRace>();
				if (!race) {
					continue;
				}
				for (auto* sf : spells) {
					if (auto* sp = sf->As<RE::SpellItem>()) {
						AddSpellTo(race->actorEffects, sp, t);
					}
				}
			}
			if (spells.empty()) {
				++t.missing;
			}
		}

		void NpcSpellAdd(const nlohmann::json& a_r, Tally& t)
		{
			const auto spells = FindAll(a_r, "spells");
			for (auto* f : FindAll(a_r, "npcs")) {
				auto* npc = f->As<RE::TESNPC>();
				if (!npc) {
					continue;
				}
				for (auto* sf : spells) {
					if (auto* sp = sf->As<RE::SpellItem>()) {
						AddSpellTo(npc->GetSpellList(), sp, t);
					}
				}
			}
			if (spells.empty()) {
				++t.missing;
			}
		}

		void NpcPerkAdd(const nlohmann::json& a_r, Tally& t)
		{
			const auto perks = FindAll(a_r, "perks");
			for (auto* f : FindAll(a_r, "npcs")) {
				auto* npc = f->As<RE::TESNPC>();
				if (!npc) {
					continue;
				}
				for (auto* pf : perks) {
					auto* perk = pf->As<RE::BGSPerk>();
					if (!perk) {
						continue;
					}
					bool has = false;
					for (std::uint32_t i = 0; i < npc->perkCount; ++i) {
						has = has || (npc->perks && npc->perks[i].perk == perk);
					}
					if (has) {
						++t.already;
					} else if (npc->AddPerk(perk, 1)) {
						++t.changed;
					}
				}
			}
			if (perks.empty()) {
				++t.missing;
			}
		}

		void DisableRefs(const nlohmann::json& a_r, Tally& t)
		{
			for (const auto& ref : Strings(a_r, "refs")) {
				auto* f = Find(ref);
				if (!f) {
					SKSE::log::warn("disable_refs: {} not found", ref);
					++t.missing;
					continue;
				}
				if ((f->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled) != 0) {
					++t.already;
					continue;
				}
				f->formFlags |= RE::TESForm::RecordFlags::kDisabled;
				++t.changed;
			}
		}

		nlohmann::json Load()
		{
			std::ifstream in(ConfigPath());
			if (!in) {
				return nlohmann::json::value_t::discarded;
			}
			return nlohmann::json::parse(in, nullptr, false, true);
		}
	}

	void Apply()
	{
		const auto j = Load();
		if (j.is_discarded() || !j.is_object()) {
			SKSE::log::warn("enforcer: no usable {} (idle)", ConfigPath().string());
			return;
		}
		Tally t;
		const auto run = [&](const char* a_key, auto a_fn) {
			if (j.contains(a_key) && j[a_key].is_array()) {
				for (const auto& r : j[a_key]) {
					if (r.is_object() && r.contains("requires") && !RE::TESDataHandler::GetSingleton()->LookupModByName(r.value("requires", std::string()))) {
						continue;  // the mod this rule is for is not loaded
					}
					++t.rules;
					try {
						a_fn(r, t);
					} catch (const std::exception& e) {
						SKSE::log::error("enforcer: rule in {} failed: {}", a_key, e.what());
					}
				}
			}
		};
		run("ingestible_effects", IngestibleEffects);
		run("formlist_add", FormListAdd);
		run("keyword_add", KeywordAdd);
		run("weapon_keyword", WeaponKeyword);
		run("armor_warmth", ArmorWarmth);
		run("disable_refs", DisableRefs);
		run("leveled_edit", LeveledEdit);
		run("race_spell_add", RaceSpellAdd);
		run("npc_spell_add", NpcSpellAdd);
		run("npc_perk_add", NpcPerkAdd);
		if (j.contains("ai_by_race") && j["ai_by_race"].is_array()) {
			++t.rules;
			AiByRace(j["ai_by_race"], t);
		}
		if (j.contains("package_list_by_race") && j["package_list_by_race"].is_array()) {
			++t.rules;
			PackageListByRace(j["package_list_by_race"], t);
		}
		SKSE::log::info("enforcer: {} rules, {} changes applied, {} already in place, {} references missing", t.rules, t.changed, t.already, t.missing);
	}

	void ApplyPlayer()
	{
		const auto j = Load();
		if (j.is_discarded() || !j.is_object()) {
			return;
		}
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		for (const auto& ref : Strings(j, "player_perks")) {
			auto* f = Find(ref);
			auto* perk = f ? f->As<RE::BGSPerk>() : nullptr;
			if (!perk) {
				SKSE::log::warn("player_perks: {} not found", ref);
			} else if (!pc->HasPerk(perk)) {
				pc->AddPerk(perk);
				SKSE::log::info("enforcer: gave the player perk {}", ref);
			}
		}
	}
}
