#include "ai_agent.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "items.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "quests.h"
#include "utils/paths.h"


namespace devilution {

// El enum con los bitmasks
enum EnvironmentFlag : uint32_t {
	Env_Player = 1 << 0,
	Env_Wall = 1 << 1,
	Env_PrevTrigger = 1 << 2,
	Env_NextTrigger = 1 << 3,
	Env_WarpTrigger = 1 << 4,
	Env_Door = 1 << 5,
	Env_Missile = 1 << 6,
	Env_Monster = 1 << 7,
	Env_UnknownObject = 1 << 8,
	Env_Crucifix = 1 << 9,
	Env_Barrel = 1 << 10,
	Env_Chest = 1 << 11,
	Env_Sarcophagus = 1 << 12,
	Env_Item = 1 << 13,
	Env_Explored = 1 << 14,
	Env_Visible = 1 << 15,
	Env_Interactable = 1 << 16,
	Env_Open = 1 << 17,
	Env_Goal = 1 << 18,
	Env_Decor = 1 << 19,
	Env_Quest = 1 << 20,
	Env_Shrine = 1 << 21,   // Santuarios estándar (conocidos/predecibles)
	Env_Hazard = 1 << 22,  // Goat Shrine / Cauldron (efectos aleatorios / alto riesgo)
	Env_Fountain = 1 << 23, // Fuentes de vida, maná o purificación
};

void AIAgent::SyncTick()
{
	static int tickCounter = 0;
	if (tickCounter++ % 50 == 0) {
		DumpStateToJSON();
	}
}

void AIAgent::DumpStateToJSON()
{
	std::string dumpPath = paths::PrefPath() + "ai_state.json";
	std::ofstream out(dumpPath);
	if (!out.is_open()) return;

	Player &myPlayer = Players[MyPlayerId];

	std::stringstream json;
	json << "{\n";

	// 1. INFO DEL PERSONAJE
	json << "  \"player\": {\n";
	json << "    \"x\": " << static_cast<int>(myPlayer.position.tile.x) << ",\n";
	json << "    \"y\": " << static_cast<int>(myPlayer.position.tile.y) << ",\n";
	json << "    \"hp\": " << static_cast<int>(myPlayer._pHitPoints >> 6) << ",\n";
	json << "    \"max_hp\": " << static_cast<int>(myPlayer._pMaxHP >> 6) << ",\n";
	json << "    \"mana\": " << static_cast<int>(myPlayer._pMana >> 6) << "\n";
	json << "  },\n";

	// 2. MATRIZ DEL MAPA (Bitmasks)
	json << "  \"map\": [\n";

	int px = myPlayer.position.tile.x;
	int py = myPlayer.position.tile.y;

	for (int y = 0; y < MAXDUNY; y++) {
		json << "    [";
		for (int x = 0; x < MAXDUNX; x++) {
			Point pt = { x, y };
			uint32_t cellMask = 0;

			bool isExplored = (static_cast<int>(dFlags[x][y]) & static_cast<int>(DungeonFlag::Explored)) != 0;
			bool isLit = (static_cast<int>(dFlags[x][y]) & static_cast<int>(DungeonFlag::Lit)) != 0;
			bool isNear = (std::abs(x - px) <= 12 && std::abs(y - py) <= 12);

			if (isExplored) {
				cellMask |= Env_Explored;
				if (isLit && isNear) {
					cellMask |= Env_Visible;
				}
			}

			if (isExplored) {
				if (TileHasAny(pt, TileProperties::Solid)) {
					cellMask |= Env_Wall;
				}

				for (int i = 0; i < numtrigs; i++) {
					if (trigs[i].position == pt) {
						if (trigs[i]._tmsg == WM_DIABPREVLVL) {
							cellMask |= Env_PrevTrigger;
						} else if (trigs[i]._tmsg == WM_DIABNEXTLVL) {
							cellMask |= Env_NextTrigger;
						} else {
							cellMask |= Env_WarpTrigger;
						}
						break;
					}
				}

				for (auto &quest : Quests) {
					// Si la misión está activa y su entrada coincide con esta casilla
					if (quest._qactive != 0 && quest.position == pt) {
						cellMask |= Env_Quest;
					}
				}

				if (dObject[x][y] != 0) {
					int objIndex = std::abs(dObject[x][y]) - 1;
					auto &obj = Objects[objIndex];
					auto otype = obj._otype;

					bool isOpen = false;
					bool isInteractable = false;
					bool isDestroyed = false; // Bandera para barriles que desaparecen como obstáculo


					 // 1. EVALUAR ESTADO SEGÚN EL TIPO DE OBJETO
					if (otype == OBJ_L1LDOOR || otype == OBJ_L1RDOOR || otype == OBJ_L2LDOOR || otype == OBJ_L2RDOOR || otype == OBJ_L3LDOOR || otype == OBJ_L3RDOOR) {
						isOpen = (obj._oVar4 == 1);
						isInteractable = true;
					} else if (otype == OBJ_BARREL || otype == OBJ_BARRELEX) {
						isDestroyed = obj.IsBroken();
						if (!isDestroyed) {
							isInteractable = (dObject[x][y] > 0);
						}
					
					} else if (otype == OBJ_SARC) {
						// El sarcófago SOLO depende de si su animación de abrirse ha terminado
						isOpen = (obj._oAnimFrame == obj._oAnimLen);
						isInteractable = (!isOpen) && (dObject[x][y] > 0);
					} else if (otype == OBJ_CHEST1 || otype == OBJ_CHEST2 || otype == OBJ_CHEST3) {
						// El cofre SOLO depende de la variable interna de estado
						isOpen = (obj.selectionRegion == SelectionRegion::None);
						isInteractable = (!isOpen) && (dObject[x][y] > 0);
					} else {
						// Santuarios, fuentes y restos de objetos genéricos interactuables
						isOpen = false;
						isInteractable = (dObject[x][y] > 0);
					}


					// 2. APLICAR MASKS DE ESTADO Y TIPO
					if (!isDestroyed) {
						if (isOpen) {
							cellMask |= Env_Open;
						}
						if (isInteractable) {
							cellMask |= Env_Interactable;
						}

						// 3. TIPO DE OBJETO
						if (otype == OBJ_L1LDOOR || otype == OBJ_L1RDOOR || otype == OBJ_L2LDOOR || otype == OBJ_L2RDOOR || otype == OBJ_L3LDOOR || otype == OBJ_L3RDOOR) {
							cellMask |= Env_Door;
						} else if (otype == OBJ_CHEST1 || otype == OBJ_CHEST2 || otype == OBJ_CHEST3) {
							cellMask |= Env_Chest;
						} else if (otype == OBJ_BARREL || otype == OBJ_BARRELEX) {
							cellMask |= Env_Barrel;
						} else if (otype == OBJ_SARC) {
							cellMask |= Env_Sarcophagus;
						} else if (otype == OBJ_CRUX1 || otype == OBJ_CRUX2 || otype == OBJ_CRUX3) {
							cellMask |= Env_Crucifix;
						} else if (otype == OBJ_STORYBOOK || otype == OBJ_BLINDBOOK || otype == OBJ_BLOODBOOK || otype == OBJ_STEELTOME) {
							cellMask |= Env_Quest;
						} else if (otype == OBJ_SHRINEL || otype == OBJ_SHRINER) {
							cellMask |= Env_Shrine;
						} else if (otype == OBJ_GOATSHRINE || otype == OBJ_CAULDRON) {
							cellMask |= Env_Hazard;
						} else if (otype == OBJ_BLOODFTN || otype == OBJ_PURIFYINGFTN || otype == OBJ_MURKYFTN || otype == OBJ_TEARFTN) {
							// Forzamos la detección explícita de la fuente de sangre aquí
							cellMask |= Env_Fountain;
						} else {
							if (!AllObjects[otype].isSolid()) {
								cellMask |= Env_Decor;
							} else {
								cellMask |= Env_UnknownObject;
							}
						}
					}

					if (dItem[x][y] != 0) {
						cellMask |= Env_Item;
					}
				}

				if ((cellMask & Env_Visible) != 0) {
					if (dPlayer[x][y] != 0) {
						cellMask |= Env_Player;
					}
					if (dMonster[x][y] != 0) {
						cellMask |= Env_Monster;
					}
					if ((static_cast<int>(dFlags[x][y]) & static_cast<int>(DungeonFlag::Missile)) != 0) {
						cellMask |= Env_Missile;
					}
				}
			}

			json << cellMask;
			if (x < MAXDUNX - 1) json << ", ";
		}
		json << (y == MAXDUNY - 1 ? "]\n" : "],\n");
	}

	json << "  ]\n";
	json << "}\n";

	out << json.str();
	out.close();
}

} // namespace devilution
