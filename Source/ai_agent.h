#pragma once

namespace devilution {

class AIAgent {
public:
	// Función principal que llamaremos en cada frame
	static void SyncTick();
	static void PrepareNewGame();

private:
	// Función interna para volcar el estado, rawmap para procesar si tiene salida al inciio del nivel
	static void DumpStateToJSON(bool isIdle, bool isRawMap = false);

	// Función interna para leer y consumir las órdenes de Python
	static void ReadActionFromJSON();
};

} // namespace devilution
