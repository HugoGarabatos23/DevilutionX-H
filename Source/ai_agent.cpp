#include "ai_agent.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "diablo.h"
#include "items.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "quests.h"
#include "items.h"
#include "tables/itemdat.h"
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
	Env_OtherSolidObjects = 1 << 8,
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
	Env_Bookcase = 1 << 24,
	Env_Gold = 1ull << 25,
	Env_Equipment = 1ull << 26,
	Env_Potion = 1ull << 27,
	Env_MagicText = 1ull << 28,
	Env_QuestItem = 1ull << 29,
};

void AIAgent::ReadActionFromJSON()
{
	std::string actionPath = paths::PrefPath() + "ai_action.json";
	std::ifstream in(actionPath);
	if (!in.is_open()) return;

	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();

	if (content.empty()) return;

	size_t actPos = content.find("\"action\":");
	size_t xPos = content.find("\"x\":");
	size_t yPos = content.find("\"y\":");

	if (actPos != std::string::npos && xPos != std::string::npos && yPos != std::string::npos) {
		int targetX = std::stoi(content.substr(xPos + 4));
		int targetY = std::stoi(content.substr(yPos + 4));
		Point target = { targetX, targetY };

		if (content.find("\"mover\"") != std::string::npos) {
			NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, target);
		} else if (content.find("\"interactuar\"") != std::string::npos) {
			NetSendCmdLoc(MyPlayerId, true, CMD_OPOBJXY, target);
		}
	}

	// Truncamos/vaciamos el archivo para consumir la orden y no repetir el comando
	std::ofstream clearFile(actionPath, std::ofstream::trunc);
	clearFile.close();
}

void AIAgent::SyncTick()
{
	ReadActionFromJSON();

	Player &myPlayer = Players[MyPlayerId];
	bool isIdle = (myPlayer._pmode == PM_STAND);

	static int idleTicks = 0;

	if (isIdle) {
		idleTicks++;
		// idleTicks == 1 -> Acaba de terminar el paso en este exacto milisegundo (Disparo instantáneo)
		// idleTicks % 15 == 0 -> Sigue quieto esperando órdenes (Latido de seguridad cada 0.75s)
		if (idleTicks == 1 || idleTicks % 15 == 0) {
			DumpStateToJSON(isIdle);
		}
	} else {
		// Está caminando: prohibido tocar el disco duro para no dar tirones de FPS
		idleTicks = 0;
	}
}

void AIAgent::DumpStateToJSON(bool isIdle)
{
	std::string dumpPath = paths::PrefPath() + "ai_state.json";
	std::ofstream out(dumpPath);
	if (!out.is_open()) return;

	Player &myPlayer = Players[MyPlayerId];

	std::stringstream json;
	json << "{\n";

	// 1. INFO DEL PERSONAJE EN NIVEL
	json << "  \"level\": " << static_cast<int>(currlevel) << ",\n";
	json << "  \"seed\": " << static_cast<uint32_t>(DungeonSeeds[currlevel]) << ",\n";
	json << "  \"player\": {\n";
	json << "    \"x\": " << static_cast<int>(myPlayer.position.tile.x) << ",\n";
	json << "    \"y\": " << static_cast<int>(myPlayer.position.tile.y) << ",\n";
	json << "    \"hp\": " << static_cast<int>(myPlayer._pHitPoints >> 6) << ",\n";
	json << "    \"max_hp\": " << static_cast<int>(myPlayer._pMaxHP >> 6) << ",\n";
	json << "    \"mana\": " << static_cast<int>(myPlayer._pMana >> 6) << ",\n";
	json << "    \"is_idle\": " << (isIdle ? "true" : "false") << "\n"; // <-- Inyección del estado para Python
	json << "  },\n";

	// 2. MATRIZ DEL MAPA (Bitmasks)
	json << "  \"map\": [\n";

	int px = myPlayer.position.tile.x;
	int py = myPlayer.position.tile.y;

	for (int y = 0; y < MAXDUNY; y++) {
		json << "    [";
		for (int x = 0; x < MAXDUNX; x++) {
			Point pt = { x, y };
			uint64_t cellMask = 0;

			bool isExplored = (static_cast<int>(dFlags[x][y]) & static_cast<int>(DungeonFlag::Explored)) != 0;
			bool isLit = (static_cast<int>(dFlags[x][y]) & static_cast<int>(DungeonFlag::Lit)) != 0;
			bool isNear = (std::abs(x - px) <= 7 && std::abs(y - py) <= 7);

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
					int oid = dObject[x][y];
					int oi = (oid > 0 ? oid : -oid) - 1;
					auto &obj = Objects[oi];
					_object_id otype = obj._otype;

					// --- INYECCIÓN DE RAYOS X ---
					// Empaquetamos el ID del objeto en los bits 24-31 de la máscara
					cellMask |= ((static_cast<uint64_t>(otype) & 0xFF) << 56);
					// ----------------------------

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
					} else if (otype == OBJ_BOOKCASEL || otype == OBJ_BOOKCASER || otype == OBJ_BOOKSTAND || otype == OBJ_BOOKCANDLE) {
						// Las librerías y atriles pierden su hitbox al soltar el libro
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
							// Englobamos cofres, librerías grandes y atriles de hechizos
							cellMask |= Env_Chest;
						} else if (otype == OBJ_BOOKCASEL || otype == OBJ_BOOKCASER || otype == OBJ_BOOKSTAND || otype == OBJ_BOOKCANDLE) {
							cellMask |= Env_Bookcase;
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
						} else if (otype == 0) {
							// IGNORAR ABSOLUTO: Es luz invisible. Para la IA es suelo vacío. No hacemos nada.
						} else if (otype >= OBJ_TNUDEM1 && otype <= OBJ_TORTURE5) {
							// Silenciador masivo para atrezo de tortura: la IA los registra como muros/obstáculos sólidos.
							cellMask |= Env_OtherSolidObjects;

						} else if (otype == OBJ_CANDLE1 || otype == OBJ_CANDLE2 || otype == OBJ_TBCROSS)
							{
							// Elementos de luz físicos: Son obstáculos sólidos del escenario. 
							cellMask |= Env_OtherSolidObjects;

						} else {
							if (!AllObjects[otype].isSolid()) {
								cellMask |= Env_Decor;
							} else {
								cellMask |= Env_OtherSolidObjects;
							}
						}
					}

					if (dItem[x][y] != 0) {
						int dItemId = dItem[x][y];
						int iid = (dItemId > 0 ? dItemId : -dItemId) - 1; // Soporta índices negativos si el motor los usa
						auto &item = Items[iid];   // Accedemos al ítem real en memoria

						if (item.isGold()) { 
							cellMask |= Env_Gold;
						} else if (item._iClass == ICLASS_QUEST) { 
							cellMask |= Env_QuestItem;
						} else if (item.isEquipment()) { 
							cellMask |= Env_Equipment;
						} else if (item._iMiscId == IMISC_BOOK || item.isScroll()) { 
							cellMask |= Env_MagicText;
						} else if (item._iMiscId == IMISC_HEAL || item._iMiscId == IMISC_MANA || item._iMiscId == IMISC_REJUV || item._iMiscId == IMISC_FULLHEAL || item._iMiscId == IMISC_FULLMANA || item._iMiscId == IMISC_FULLREJUV) { //[cite: 7, 8]
							cellMask |= Env_Potion;
						} else {
							cellMask |= Env_Item; // Fallback genérico para runas, aceites, orejas, etc.
						}
						
					}
				}

				if ((cellMask & Env_Visible) != 0) {
					if (x == px && y == py) {
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
