#pragma once

namespace devilution {

class AIAgent {
public:
	// Función principal que llamaremos en cada frame
	static void SyncTick();

private:
	// Función interna para volcar el estado
	static void DumpStateToJSON();
};

} // namespace devilution
