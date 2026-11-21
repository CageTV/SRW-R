/*
 * Creature Behaviour
 * Copyright (c) 2026 CageTV
 *
 * Licensed under the GNU General Public License, version 3 or later; see LICENSE.
 */
#pragma once

namespace Enforcer
{
	// Reads Data/SKSE/Plugins/CreatureBehaviour/enforce.json and makes sure the listed records still have
	// what the plugin gives them, even when a later mod overwrote them. Runs at kDataLoaded.
	void Apply();
	// Rules that live on the player (perks); runs on new game and after a load.
	void ApplyPlayer();
}
