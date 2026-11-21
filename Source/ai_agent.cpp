#include "ai_agent.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#endif

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "diablo.h"
#include "engine/random.hpp"
#include "items.h"
#include "levels/gendung.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "quests.h"
#include "items.h"
#include "tables/itemdat.h"
#include "utils/paths.h"
#include "interfac.h"
#include "msg.h"
#include "multi.h"
#include "portal.h"
#include "utils/sdl_compat.h"


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

static bool waitingForLevelReset = false;
static bool waitingForTownTransition = false;
static bool newGameEventPending = false;
static int observedLevel = -1;

void AIAgent::ReadActionFromJSON()
{
	std::string actionPath = paths::PrefPath() + "ai_action.json";
	std::ifstream in(actionPath);
	if (!in.is_open()) return;

	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();

	if (content.empty()) return;

	// Truncamos primero el archivo para asegurarnos de consumir la orden
	std::ofstream clearFile(actionPath, std::ofstream::trunc);
	clearFile.close();

	size_t actPos = content.find("\"action\":");
	if (actPos != std::string::npos) {
		const size_t actionValuePos = content.find_first_not_of(" \t\r\n", actPos + 9);
		constexpr char ResetAction[] = "\"reiniciar_partida\"";
		if (actionValuePos != std::string::npos && content.compare(actionValuePos, sizeof(ResetAction) - 1, ResetAction) == 0) {
			waitingForLevelReset = true;
			newGameEventPending = true;

			SDL_Event event;
			CustomEventToSdlEvent(event, WM_DIABNEWGAME);
			SDLC_PushEvent(&event);
			return;
		}
	}

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
}

void AIAgent::PrepareNewGame()
{
	if (!newGameEventPending)
		return;

	SetRndSeed(0);
	InitGameInfo();
	xoshiro128plusplus gameGenerator(sgGameInitInfo.gameSeed);
	for (int i = 0; i < NUMLEVELS; i++) {
		DungeonSeeds[i] = gameGenerator.next();
		LevelSeeds[i] = std::nullopt;
	}
	DungeonSeeds[0] = GenerateSeed();

	InitLevels();

	if (MyPlayer != nullptr) {
		for (bool &levelVisited : MyPlayer->_pLvlVisited) {
			levelVisited = false;
		}
		for (bool &subLevelVisited : MyPlayer->_pSLvlVisited) {
			subLevelVisited = false;
		}
		MyPlayer->pTownWarps = 0;
	}

	InitQuests();
	InitPortals();
	InitDungMsgs(*MyPlayer);
	DeltaSyncJunk();
	newGameEventPending = false;
}

void AIAgent::SyncTick()
{
	if (newGameEventPending)
		return;

	if (waitingForLevelReset) {
		waitingForLevelReset = false;
		if (currlevel == 0) {
			waitingForTownTransition = true;
			StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, 1);
			return;
		}

		// 3.Generar la "Vista de Dios" para el A* de Python
		DumpStateToJSON(true, true);

		// Generar la "Vista del Agente" con niebla para empezar a jugar
		DumpStateToJSON(true, false);
		observedLevel = currlevel;
		return;
	}

	if (waitingForTownTransition) {
		if (currlevel == 0)
			return;

		waitingForTownTransition = false;
		DumpStateToJSON(true, true);
		DumpStateToJSON(true, false);
		observedLevel = currlevel;
		return;
	}

	if (observedLevel == -1) {
		observedLevel = currlevel;
	} else if (observedLevel != currlevel) {
		// SyncTick se reanuda tras completar la carga del nivel.
		DumpStateToJSON(true, true);
		DumpStateToJSON(true, false);
		observedLevel = currlevel;
		return;
	}

	ReadActionFromJSON();

	if (waitingForLevelReset) {
		// Acabamos de encolar WM_DIABNEWGAME en este tick. No volcamos el mapa viejo.
		return;
	}

	Player &myPlayer = Players[MyPlayerId];
	bool isIdle = (myPlayer._pmode == PM_STAND);

	static int idleTicks = 0;

	if (isIdle) {
		idleTicks++;
		if (idleTicks == 1 || idleTicks % 15 == 0) {
			DumpStateToJSON(isIdle);
		}
	} else {
		idleTicks = 0;
	}
}

void AIAgent::DumpStateToJSON(bool isIdle, bool isRawMap)
{
	// 1. Escribir en un archivo distinto si es el mapa crudo
	std::string fileName = isRawMap ? "ai_raw_map.json" : "ai_state.json";
	std::string dumpPath = paths::PrefPath() + fileName;
	std::string tmpPath = dumpPath + ".tmp";
	std::ofstream out(tmpPath);
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

			// Si flag mapa crudo, desvelamos todo
			if (isRawMap) {
				isExplored = true;
				isLit = true;
				isNear = true;
			}

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
#ifdef _WIN32
	MoveFileExA(tmpPath.c_str(), dumpPath.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
	std::rename(tmpPath.c_str(), dumpPath.c_str());
#endif
}

} // namespace devilution
