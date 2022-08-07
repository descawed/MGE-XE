
#include "mge/distantland.h"

#include <fstream>
#include <iostream>
#include <Windows.h>



int main() {
	std::ofstream log("mgeMemHost.log");

	auto oldout = std::cout.rdbuf(log.rdbuf());
	auto olderr = std::cerr.rdbuf(log.rdbuf());

	// connect to communication pipe
	auto pipe = CreateFile("\\\\.\\pipe\\mgeXE", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (pipe == INVALID_HANDLE_VALUE) {
		std::cerr << "Couldn't find pipe" << std::endl;
		return 1;
	}

	// message mode
	DWORD dwMode = PIPE_READMODE_MESSAGE;
	if (!SetNamedPipeHandleState(pipe, &dwMode, NULL, NULL)) {
		std::cerr << "Couldn't set pipe mode" << std::endl;
		return 2;
	}

	// get the handle to shared memory
	HANDLE hSharedMem = 0;
	DWORD unused;
	ReadFile(pipe, &hSharedMem, 4, &unused, 0);

	VisibleDistantMeshes* vdm = (VisibleDistantMeshes*)MapViewOfFile(hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (vdm == nullptr) {
		std::cerr << "Couldn't map shared memory" << std::endl;
		return 4;
	}

	std::cout << "Initializing landscape" << std::endl;
	DistantLand::initLandscape(pipe);

	std::cout << "Initializing distant statics" << std::endl;
	DistantLand::initDistantStatics(pipe);

	std::cout << "Initialization complete" << std::endl;

	while (true) {
		DWORD command;

		ReadFile(pipe, &command, 4, &unused, 0);
		switch (command) {
		case 1:
			DistantLand::getDistantLand(pipe, *vdm);
			break;
		case 2:
			DistantLand::getDistantStatics(pipe, *vdm);
			break;
		case 3:
			DistantLand::getDistantGrass(pipe, *vdm);
			break;
		case 4:
			DistantLand::getDistantReflections(pipe, *vdm);
			break;
		case 5:
			DistantLand::getDistantShadows(pipe, *vdm);
			break;
		default:
			std::cout << "Exiting" << std::endl;
			UnmapViewOfFile(vdm);
			CloseHandle(pipe);
			return 0;
		}

		command = 1;
		WriteFile(pipe, &command, 4, &unused, 0);
	}
}