/*
 * Creature Behaviour
 * Copyright (c) 2026 CageTV
 *
 * Licensed under the GNU General Public License, version 3 or later; see LICENSE.
 */
#include "PCH.h"
#include "Spawner.h"

#include <nlohmann/json.hpp>

// SPIKE: proves four things on the real game, no Papyrus, no placed references:
//   1. a creature can be spawned from a JSON-defined point (position relative to the player for the test),
//   2. the engine resolves a leveled NPC list on spawn (the log says what base the new reference has),
//   3. day/night gating (nocturnal at night, diurnal by day) with a per-point cap,
//   4. spawned creatures are cleaned up when the window closes or the player is far away.
namespace Spawner
{
	namespace
	{
		enum class When { kAny, kDay, kNight };

		struct Point
		{
			std::string  id;
			std::string  plugin;
			std::uint32_t formId{ 0 };      // leveled NPC list (or a single NPC), local FormID in `plugin`
			RE::NiPoint3 offset{};          // test anchor: relative to the player (x forward, y right in the player's frame)
			When         when{ When::kAny };
			int          cap{ 1 };
			float        minGapHours{ 0.5f };
			// absolute mode (real spawn points): a fixed place in a worldspace; spawns when the player is near, not in view
			bool          absolute{ false };
			RE::NiPoint3  pos{};
			std::string   worldPlugin;
			std::uint32_t worldForm{ 0 };
			float         trigger{ 4500.0f };  // spawn only when the player is closer than this
			float         minDist{ 1800.0f };  // ... and farther than this (no popping in view)
			float        lastSpawnHours{ -1e9f };
			std::vector<RE::ObjectRefHandle> alive;
		};

		struct Config
		{
			bool         enabled{ true };
			float        tickSeconds{ 5.0f };
			float        dayStart{ 6.0f };
			float        dayEnd{ 20.0f };
			float        despawnDistance{ 9000.0f };
			float        quietDistance{ 3000.0f };  // creatures are only removed when the player is at least this far (no popping in view)
			std::vector<Point> points;
		};

		Config             g_cfg;
		std::atomic<bool>  g_run{ false };
		std::mt19937       g_rng{ std::random_device{}() };

		std::filesystem::path ConfigPath()
		{
			return std::filesystem::path("Data/SKSE/Plugins/CreatureBehaviour/points.json");
		}

		When ParseWhen(const std::string& a_s)
		{
			if (a_s == "day") return When::kDay;
			if (a_s == "night") return When::kNight;
			return When::kAny;
		}

		const char* WhenName(When a_w)
		{
			return a_w == When::kDay ? "day" : a_w == When::kNight ? "night" : "any";
		}

		void Load()
		{
			g_cfg = Config{};
			const auto path = ConfigPath();
			std::ifstream in(path);
			if (!in) {
				SKSE::log::warn("no {} (spawner idle)", path.string());
				g_cfg.enabled = false;
				return;
			}
			const auto j = nlohmann::json::parse(in, nullptr, false, true);
			if (j.is_discarded()) {
				SKSE::log::error("{} is not valid JSON (spawner idle)", path.string());
				g_cfg.enabled = false;
				return;
			}
			g_cfg.enabled = j.value("enabled", true);
			g_cfg.tickSeconds = std::max(1.0f, j.value("tickSeconds", 5.0f));
			g_cfg.dayStart = j.value("dayStartHour", 6.0f);
			g_cfg.dayEnd = j.value("dayEndHour", 20.0f);
			g_cfg.despawnDistance = j.value("despawnDistance", 9000.0f);
			g_cfg.quietDistance = j.value("quietDistance", 3000.0f);
			for (const auto& p : j.value("points", nlohmann::json::array())) {
				Point pt;
				pt.id = p.value("id", std::string("point"));
				pt.plugin = p.value("plugin", std::string("Skyrim.esm"));
				pt.formId = static_cast<std::uint32_t>(std::stoul(p.value("form", std::string("0")), nullptr, 16));
				const auto off = p.value("offset", std::vector<float>{ 800.0f, 0.0f, 0.0f });
				if (off.size() == 3) {
					pt.offset = { off[0], off[1], off[2] };
				}
				if (p.contains("pos") && p["pos"].is_array() && p["pos"].size() == 3) {
					pt.absolute = true;
					pt.pos = { p["pos"][0].get<float>(), p["pos"][1].get<float>(), p["pos"][2].get<float>() };
					pt.worldPlugin = p.value("worldPlugin", std::string("Skyrim.esm"));
					pt.worldForm = static_cast<std::uint32_t>(std::stoul(p.value("world", std::string("3c")), nullptr, 16));
					pt.trigger = p.value("trigger", 4500.0f);
					pt.minDist = p.value("minDist", 1800.0f);
				}
				pt.when = ParseWhen(p.value("when", std::string("any")));
				pt.cap = std::max(1, p.value("cap", 1));
				pt.minGapHours = p.value("minGapHours", 0.5f);
				if (pt.formId != 0) {
					g_cfg.points.push_back(std::move(pt));
				}
			}
			SKSE::log::info("config: {} point(s), tick {}s, day {}-{}h, despawn {}", g_cfg.points.size(), g_cfg.tickSeconds, g_cfg.dayStart, g_cfg.dayEnd, g_cfg.despawnDistance);
		}

		bool IsDay(float a_hour)
		{
			return a_hour >= g_cfg.dayStart && a_hour < g_cfg.dayEnd;
		}

		bool WindowOpen(const Point& a_pt, float a_hour)
		{
			switch (a_pt.when) {
			case When::kDay: return IsDay(a_hour);
			case When::kNight: return !IsDay(a_hour);
			default: return true;
			}
		}

		// Picks an entry of a leveled NPC list by hand (used only if the engine does not resolve the list on spawn).
		RE::TESBoundObject* ResolveByHand(RE::TESLevCharacter* a_list, std::uint16_t a_playerLevel, int a_depth = 0)
		{
			if (!a_list || a_depth > 6) {
				return nullptr;
			}
			std::vector<RE::TESForm*> ok;
			std::uint16_t             best = 0;
			for (std::uint32_t i = 0; i < a_list->numEntries; ++i) {
				const auto& e = a_list->entries[i];
				if (e.form && e.level <= a_playerLevel) {
					best = std::max(best, e.level);
				}
			}
			const bool allLevels = (static_cast<std::uint32_t>(a_list->llFlags) & 1u) != 0;  // calculate from all levels <= player
			for (std::uint32_t i = 0; i < a_list->numEntries; ++i) {
				const auto& e = a_list->entries[i];
				if (e.form && e.level <= a_playerLevel && (allLevels || e.level == best)) {
					for (std::uint16_t c = 0; c < std::max<std::uint16_t>(1, e.count); ++c) {
						ok.push_back(e.form);
					}
				}
			}
			if (ok.empty()) {
				return nullptr;
			}
			auto* pick = ok[std::uniform_int_distribution<std::size_t>(0, ok.size() - 1)(g_rng)];
			if (auto* nested = pick->As<RE::TESLevCharacter>()) {
				return ResolveByHand(nested, a_playerLevel, a_depth + 1);
			}
			return pick->As<RE::TESBoundObject>();
		}

		// Removing a live actor in the same frame crashed in its movement controller (2026-11-17):
		// disable now, delete one tick later when its 3D and AI are gone.
		std::vector<RE::ObjectRefHandle> g_pendingDelete;

		void Retire(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}
			a_ref->Disable();
			g_pendingDelete.push_back(a_ref->GetHandle());
		}

		void FlushPending()
		{
			for (auto& h : g_pendingDelete) {
				if (auto r = h.get()) {
					r->SetDelete(true);
				}
			}
			g_pendingDelete.clear();
		}

		void DespawnAll(Point& a_pt, const char* a_why)
		{
			for (auto& h : a_pt.alive) {
				if (auto r = h.get()) {
					Retire(r.get());
				}
			}
			SKSE::log::info("[{}] despawned {} ({})", a_pt.id, a_pt.alive.size(), a_why);
			a_pt.alive.clear();
		}

		void Spawn(Point& a_pt, RE::PlayerCharacter* a_player, float a_hoursNow)
		{
			auto* anyForm = RE::TESDataHandler::GetSingleton()->LookupForm(a_pt.formId, a_pt.plugin);
			auto* base = anyForm ? anyForm->As<RE::TESBoundObject>() : nullptr;
			if (!base) {
				SKSE::log::error("[{}] {}|{:06X} not found ({}; form type {})", a_pt.id, a_pt.plugin, a_pt.formId,
					anyForm ? "found but not a bound object" : "no such form",
					anyForm ? static_cast<int>(anyForm->GetFormType()) : -1);
				a_pt.lastSpawnHours = a_hoursNow;
				return;
			}
			// position in the player's frame: x forward, y right
			const float yaw = a_player->GetAngleZ();
			const auto  pp = a_player->GetPosition();
			const RE::NiPoint3 pos = a_pt.absolute ? a_pt.pos : RE::NiPoint3{ pp.x + a_pt.offset.x * std::sin(yaw) + a_pt.offset.y * std::cos(yaw),
				pp.y + a_pt.offset.x * std::cos(yaw) - a_pt.offset.y * std::sin(yaw), pp.z + a_pt.offset.z };
			const RE::NiPoint3 rot{ 0.0f, 0.0f, yaw + 3.14159265f };

			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, pos, rot, a_player->GetParentCell(), a_player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), false, true);
			auto ref = handle.get();
			if (!ref) {
				SKSE::log::error("[{}] CreateReferenceAtLocation returned nothing", a_pt.id);
				a_pt.lastSpawnHours = a_hoursNow;
				return;
			}
			auto* actualBase = ref->GetBaseObject();
			const bool resolved = actualBase && !actualBase->As<RE::TESLevCharacter>();
			SKSE::log::info("[{}] spawned ref {:08X}: asked {}|{:06X} ({}), engine gave base {:08X} '{}' type {} -> list {}",
				a_pt.id, ref->GetFormID(), a_pt.plugin, a_pt.formId, base->GetFormType() == RE::FormType::LeveledNPC ? "leveled list" : "single form",
				actualBase ? actualBase->GetFormID() : 0, actualBase ? actualBase->GetName() : "", actualBase ? static_cast<int>(actualBase->GetFormType()) : -1,
				resolved ? "RESOLVED by the engine" : "NOT resolved");
			if (!resolved) {
				// fall back to resolving the list by hand and spawning the result instead
				Retire(ref.get());
				auto* picked = ResolveByHand(base->As<RE::TESLevCharacter>(), a_player->GetLevel());
				if (!picked) {
					SKSE::log::error("[{}] list has no entry for level {}", a_pt.id, a_player->GetLevel());
					a_pt.lastSpawnHours = a_hoursNow;
					return;
				}
				handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
					picked, pos, rot, a_player->GetParentCell(), a_player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), false, true);
				ref = handle.get();
				if (!ref) {
					return;
				}
				SKSE::log::info("[{}] by hand: spawned {:08X} '{}'", a_pt.id, picked->GetFormID(), picked->GetName());
			}
			a_pt.alive.push_back(handle);
			a_pt.lastSpawnHours = a_hoursNow;
		}

		void Tick()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cal = RE::Calendar::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			if (!player || !cal || !player->Is3DLoaded() || !player->GetParentCell() || (ui && ui->GameIsPaused())) {
				return;
			}
			FlushPending();
			// never spawn while the player is indoors (caves, dungeons, the Helgen start)
			if (player->GetParentCell()->IsInteriorCell()) {
				return;
			}
			const float hour = cal->GetHour();
			const float hoursNow = cal->GetCurrentGameTime() * 24.0f;
			const auto  pp = player->GetPosition();

			for (auto& pt : g_cfg.points) {
				// forget what is gone, dead or far away
				std::erase_if(pt.alive, [&](const RE::ObjectRefHandle& h) {
					auto r = h.get();
					if (!r || r->IsDisabled() || r->IsDeleted()) {
						return true;
					}
					if (auto* a = r->As<RE::Actor>(); a && a->IsDead()) {
						return true;
					}
					if (pp.GetDistance(r->GetPosition()) > g_cfg.despawnDistance) {
						Retire(r.get());
						SKSE::log::info("[{}] removed {:08X}: too far", pt.id, r->GetFormID());
						return true;
					}
					return false;
				});

				const bool open = WindowOpen(pt, hour);
				if (!open) {
					// window closed: remove the ones the player is not looking at (far enough), keep the rest until they leave
					std::erase_if(pt.alive, [&](const RE::ObjectRefHandle& h) {
						auto r = h.get();
						if (r && pp.GetDistance(r->GetPosition()) >= g_cfg.quietDistance) {
							Retire(r.get());
							SKSE::log::info("[{}] removed {:08X}: window {} closed at {:.1f}h", pt.id, r->GetFormID(), WhenName(pt.when), hour);
							return true;
						}
						return false;
					});
					continue;
				}
				if (pt.absolute) {
					const auto* ws = player->GetWorldspace();
					const auto* want = RE::TESDataHandler::GetSingleton()->LookupForm(pt.worldForm, pt.worldPlugin);
					const float d = pp.GetDistance(pt.pos);
					if (!ws || ws != want || d > pt.trigger || d < pt.minDist) {
						continue;
					}
				}
				if (static_cast<int>(pt.alive.size()) < pt.cap && hoursNow - pt.lastSpawnHours >= pt.minGapHours) {
					Spawn(pt, player, hoursNow);
				}
			}
		}

		void Loop()
		{
			while (g_run.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(g_cfg.tickSeconds * 1000.0f)));
				if (const auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([] { Tick(); });
				}
			}
		}
	}

	void Init()
	{
		Load();
		if (!g_cfg.enabled || g_cfg.points.empty()) {
			SKSE::log::info("spawner not started");
			return;
		}
		g_run = true;
		std::thread(Loop).detach();
		SKSE::log::info("spawner started");
	}
}
