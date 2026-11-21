/*
 * Creature Behaviour
 * Copyright (c) 2026 CageTV
 *
 * Licensed under the GNU General Public License, version 3 or later; see LICENSE.
 */
#include "PCH.h"
#include "Spawner.h"
#include "Enforcer.h"

namespace
{
	void SetupLog()
	{
		const auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "CreatureBehaviour.log").string(), true);
		auto log = std::make_shared<spdlog::logger>("CreatureBehaviour", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		spdlog::set_default_logger(std::move(log));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			try {
				Enforcer::Apply();
			} catch (const std::exception& e) {
				SKSE::log::error("enforcer failed: {}", e.what());
			}
			try {
				Spawner::Init();
			} catch (const std::exception& e) {
				SKSE::log::error("failed: {}", e.what());
			}
		} else if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame || a_msg->type == SKSE::MessagingInterface::kNewGame) {
			Enforcer::ApplyPlayer();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	SKSE::log::info("CreatureBehaviour 0.2.0 (spawn points + record enforcer + Requiem layer), game {}", REL::Module::get().version().string());
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
