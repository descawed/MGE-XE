#include "ipc/dlshare.h"
#include "ipc/server.h"
#include "support/log.h"

#include <cassert>

#include <d3dx9.h>

template<class T> static inline void CleanupIfc(T*& resource) {
	if (resource != nullptr) {
		resource->Release();
		resource = nullptr;
	}
}

namespace IPC {
	Server::Server(HANDLE sharedMem, HANDLE clientProcess, HANDLE rpcStartEvent, HANDLE rpcCompleteEvent) :
		m_sharedMem(sharedMem),
		m_clientProcess(clientProcess),
		m_rpcStartEvent(rpcStartEvent),
		m_rpcCompleteEvent(rpcCompleteEvent),
		m_ipcParameters(nullptr),
		m_freeVecs(),
		m_d3d(nullptr),
		m_device(nullptr),
		m_occlusionQuery(nullptr),
		m_occlusionSurface(nullptr),
		m_occlusionTexture(nullptr),
		m_occlusionIndexes(nullptr),
		m_occlusionRender(nullptr),
		m_hasOcclusion(false)
	{ }

	Server::~Server() {
		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		CleanupIfc(m_occlusionRender);
		CleanupIfc(m_occlusionIndexes);
		CleanupIfc(m_occlusionTexture);
		CleanupIfc(m_occlusionSurface);
		CleanupIfc(m_occlusionQuery);
		CleanupIfc(m_device);
		CleanupIfc(m_d3d);

		CleanupHandle(m_sharedMem);
		CleanupHandle(m_clientProcess);
		CleanupHandle(m_rpcStartEvent);
		CleanupHandle(m_rpcCompleteEvent);
	}

	bool Server::complete() {
		if (!SetEvent(m_rpcCompleteEvent)) {
			LOG::winerror("Failed to signal RPC completion");
			return false;
		}

		return true;
	}

	bool Server::init() {
		// all occlusion bounding boxes use the same indexes
		constexpr UINT indexBufferSize = 6 * 6 * 2; // 6 faces with 6 indexes each taking 2 bytes
		constexpr WORD indexes[] = {
			// Front face
			4, 5, 6,  5, 7, 6,
			// Back face
			1, 0, 2,  1, 2, 3,
			// Left face
			0, 4, 2,  4, 6, 2,
			// Right face
			5, 1, 7,  1, 3, 7,
			// Top face
			2, 6, 3,  6, 7, 3,
			// Bottom face
			0, 1, 4,  1, 5, 4
		};

		WNDCLASSA wndClass = {
			CS_CLASSDC,
			DefWindowProcA,
			0,
			0,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			"mgeHost64",
		};
		D3DPRESENT_PARAMETERS presentParams = { };
		HWND hWnd = NULL;
		HRESULT hr = S_OK;
		WORD* indexBuffer = nullptr;

		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		m_ipcParameters = static_cast<Parameters*>(MapViewOfFile(m_sharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Parameters)));
		if (m_ipcParameters == nullptr) {
			LOG::winerror("Failed to map IPC parameters shared memory");
			return false;
		}

		// initialize D3D for occlusion testing
		hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d);
		if (FAILED(hr)) {
			LOG::logline("Failed to create Direct3D object: %08X", hr);
			goto d3dCreateFailed;
		}

		// create dummy window
		if (!RegisterClassA(&wndClass)) {
			LOG::winerror("Failed to register window class");
			goto registerClassFailed;
		}

		hWnd = CreateWindowA("mgeHost64", "mgeHost64", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
		if (hWnd == NULL) {
			LOG::winerror("Failed to create dummy window");
			goto createWindowFailed;
		}

		presentParams.Windowed = TRUE;
		presentParams.BackBufferFormat = D3DFMT_UNKNOWN;
		presentParams.BackBufferWidth = 1;
		presentParams.BackBufferHeight = 1;
		presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
		hr = m_d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &presentParams, NULL, &m_device);
		if (FAILED(hr)) {
			LOG::logline("Failed to create D3D device: %08X", hr);
			goto deviceCreateFailed;
		}

		// create any occlusion stuff that we don't need other information for
		hr = m_device->CreateIndexBuffer(indexBufferSize, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_occlusionIndexes, NULL);
		if (FAILED(hr)) {
			LOG::logline("Failed to create index buffer: %08X", hr);
			goto indexBufferCreateFailed;
		}

		hr = m_occlusionIndexes->Lock(0, indexBufferSize, reinterpret_cast<void**>(&indexBuffer), 0);
		if (FAILED(hr)) {
			LOG::logline("Failed to lock index buffer: %08X", hr);
			goto indexBufferWriteFailed;
		}

		std::memcpy(indexBuffer, indexes, indexBufferSize);

		m_occlusionIndexes->Unlock();

		hr = m_device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &m_occlusionQuery);
		if (FAILED(hr)) {
			LOG::logline("Failed to create occlusion query: %08X", hr);
			goto createQueryFailed;
		}

		return true;

	createQueryFailed:
	indexBufferWriteFailed:
		CleanupIfc(m_occlusionIndexes);
	indexBufferCreateFailed:
		CleanupIfc(m_device);
	deviceCreateFailed:
		DestroyWindow(hWnd);
		hWnd = NULL;
	createWindowFailed:
		UnregisterClassA("mgeHost64", GetModuleHandleA(NULL));
	registerClassFailed:
		CleanupIfc(m_d3d);
	d3dCreateFailed:
		UnmapViewOfFile(m_ipcParameters);
		m_ipcParameters = nullptr;

		return false;
	}

	bool Server::listen() {
		while (true) {
			// signal the completion of whatever we were doing before (also signals that we've finished initializing on the first iteration)
			SetEvent(m_rpcCompleteEvent);

			// 0 = client process, 1 = RPC start event
			auto waitResult = WaitForMultipleObjects(2, m_waitHandles, FALSE, INFINITE);
			if (waitResult == WAIT_FAILED) {
				LOG::winerror("Failed to wait for RPC event");
				return false;
			}

			if (waitResult == WAIT_OBJECT_0) {
				LOG::logline("Morrowind process exited; exiting 64-bit host");
				return true;
			}

			switch (m_ipcParameters->command) {
			case Command::None:
				break;
			case Command::AllocVec:
				allocVec();
				break;
			case Command::FreeVec:
				freeVec();
				break;
			case Command::Exit:
				LOG::logline("Host process received exit command");
				return true;
			case Command::UpdateDynVis:
				updateDynVis();
				break;
			case Command::InitDistantStatics:
				initDistantStatics();
				break;
			case Command::InitLandscape:
				initLandscape();
				break;
			case Command::SetWorldSpace:
				setWorldSpace();
				break;
			case Command::GetVisibleMeshesCoarse:
				getVisibleMeshesCoarse();
				break;
			case Command::GetVisibleMeshes:
				getVisibleMeshes();
				break;
			case Command::SortVisibleSet:
				sortVisibleSet();
				break;
			case Command::InitOcclusion:
				initOcclusion();
				break;
			case Command::GenerateOcclusionMask:
				generateOcclusionMask();
				break;
			default:
				LOG::logline("Received unknown command value %u", m_ipcParameters->command);
				break;
			}
		}
	}

	template<typename T>
	Vec<T>& Server::getVec(VecId id) {
		auto pVec = m_vecs[id];
		// when the client requests to allocate a shared vector, there's no way we can communicate a template
		// argument to the server, so all vectors are stored with a dummy type of char. the actual contained
		// type doesn't affect the layout of the vector (as all it holds is a pointer to the elements), so
		// we can freely cast between types without breaking the class itself. we will do an assert to make
		// sure the size of the type we're being told the vector contains matches the size of the type it
		// was told it contained when it was created.
		assert(sizeof(T) == pVec->m_elementBytes);
		return *reinterpret_cast<Vec<T>*>(pVec);
	}

	bool Server::allocVec() {
		auto& params = m_ipcParameters->params.allocVecParams;

		Vec<char>* vec = nullptr;
		VecId id = InvalidVector;
		if (!m_freeVecs.empty()) {
			id = m_freeVecs.front();
			m_freeVecs.pop();
			m_vecs[id] = vec = new Vec<char>(id, nullptr, params.maxCapacityInElements, params.windowSizeInElements, params.elementSize);
		} else {
			id = static_cast<VecId>(m_vecs.size());
			vec = new Vec<char>(id, nullptr, params.maxCapacityInElements, params.windowSizeInElements, params.elementSize);
			m_vecs.push_back(vec);
		}

		m_ipcParameters->params.allocVecParams.id = id;

		if (!(vec->init(m_clientProcess, params) && vec->reserve(params.initialCapacity))) {
			delete vec;
			m_vecs[id] = nullptr;
			// mark this slot free again
			m_freeVecs.push(id);
			return false;
		}

		return true;
	}

	bool Server::freeVec() {
		auto& params = m_ipcParameters->params.freeVecParams;
		params.wasFreed = false;

		auto& vec = m_vecs[params.id];
		if (vec != nullptr) {
			if (!vec->can_free())
				return false;

			delete vec;
			vec = nullptr;
			m_freeVecs.push(params.id);
			params.wasFreed = true;
		}

		return true;
	}

	void Server::updateDynVis() {
		auto& params = m_ipcParameters->params.dynVisParams;
		auto& vec = getVec<DynVisFlag>(params.id);
		for (auto& update : vec) {
			for (auto mesh : DistantLandShare::dynamicVisGroupsServer[update.groupIndex]) {
				mesh->enabled = update.enable;
			}
		}
	}

	bool Server::initOcclusion() {
		constexpr UINT resolutionScale = 4; // quarter-res occlusion

		auto& params = m_ipcParameters->params.initOcclusionParams;
		params.success = false;

		auto& displayMode = params.displayMode;
		auto hr = D3DXCreateTexture(m_device, displayMode.Width / resolutionScale, displayMode.Height / resolutionScale,
			1, D3DUSAGE_RENDERTARGET, displayMode.Format, D3DPOOL_DEFAULT, &m_occlusionTexture);
		if (FAILED(hr)) {
			LOG::logline("Failed to create occlusion texture: %08X", hr);
			return false;
		}

		D3DSURFACE_DESC desc;
		m_occlusionTexture->GetSurfaceLevel(0, &m_occlusionSurface);
		m_occlusionSurface->GetDesc(&desc);

		hr = D3DXCreateRenderToSurface(m_device, desc.Width, desc.Height, desc.Format, TRUE, D3DFMT_D16, &m_occlusionRender);
		if (FAILED(hr)) {
			LOG::logline("Failed to create occlusion render-to-surface: %08X", hr);
			CleanupIfc(m_occlusionSurface);
			CleanupIfc(m_occlusionTexture);
			return false;
		}

		params.success = true;
		return true;
	}

	bool Server::initDistantStatics() {
		auto& params = m_ipcParameters->params.distantStaticParams;
		auto& distantStatics = getVec<DistantStatic>(params.distantStatics);
		auto& distantSubsets = getVec<DistantSubset>(params.distantSubsets);
		return DistantLandShare::initDistantStaticsServer(distantStatics, distantSubsets, m_device);
	}

	bool Server::initLandscape() {
		auto& params = m_ipcParameters->params.initLandscapeParams;
		return DistantLandShare::initLandscapeServer(getVec<LandscapeBuffers>(params.buffers), params.texWorldColour);
	}

	void Server::setWorldSpace() {
		auto& params = m_ipcParameters->params.worldSpaceParams;
		params.cellFound = DistantLandShare::setCurrentWorldSpace(params.cellname);
	}

	void Server::getVisibleMeshesCoarse() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::getVisibleMeshesCoarse(vec, params.viewFrustum, params.sort, params.setFlags);
	}

	void Server::getVisibleMeshes() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::getVisibleMeshes(vec, params.viewFrustum, params.viewSphere, params.sort, params.setFlags);
	}

	void Server::sortVisibleSet() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::sortVisibleSet(vec, params.sort);
	}

	bool Server::generateOcclusionMask() {
		auto& params = m_ipcParameters->params.occlusionMaskParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		
		auto hr = m_device->SetTransform(D3DTS_VIEW, &params.view);
		if (FAILED(hr)) {
			LOG::logline("Failed to set view matrix: %08X", hr);
			return false;
		}

		hr = m_device->SetTransform(D3DTS_PROJECTION, &params.proj);
		if (FAILED(hr)) {
			LOG::logline("Failed to set projection matrix: %08X", hr);
			return false;
		}

		return true;
	}
}