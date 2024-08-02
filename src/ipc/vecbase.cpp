#include "ipc/vecbase.h"
#include "support/log.h"

namespace IPC {
	VecBase::VecBase(VecId id, VecShare* shared, std::uint32_t windowElements, std::uint32_t windowBytes, std::uint32_t maxElements, std::uint32_t reservedBytes, std::uint32_t headerBytes) :
		m_id(id),
		m_shared(shared),
		m_windowSize(windowElements),
		m_maxSize(maxElements),
		m_windowBytes(windowBytes),
		m_reservedBytes(reservedBytes),
		m_headerBytes(headerBytes),
		m_writing(false)
	{ }

	VecBase::~VecBase() {
		m_shared = nullptr;
	}

	void VecBase::start_write() {
		m_writing = true;
	}

	bool VecBase::update() {
		return SetEvent(ARCH(m_shared->updateEvent));
	}

	bool VecBase::complete() {
		return SetEvent(ARCH(m_shared->completeEvent));
	}

	WakeReason VecBase::await_update(DWORD ms) const {
		auto result = WaitForMultipleObjects(2, ARCH(m_shared->waitHandles), FALSE, ms);
		switch (result) {
		case WAIT_FAILED:
			LOG::winerror("Wait for vec update failed");
			return WakeReason::Error;
		case WAIT_TIMEOUT:
			return WakeReason::Timeout;
		default:
			auto handleIndex = result - WAIT_OBJECT_0;
			switch (handleIndex) {
			case 0:
				return WakeReason::Update;
			case 1:
				return WakeReason::Complete;
			default:
				return WakeReason::Error;
			}
		}
	}

	bool VecBase::end_write() {
		m_writing = false;
		return complete();
	}

	void VecBase::start_read() {
		ARCH(m_shared->reading) = true;
		// wait for the first batch of data to become available
		wait_read();
	}

	void VecBase::wait_read(DWORD ms) {
		// there could be old updates for elements we've already seen,
		// so loop until we either get the completion signal or there are more elements available
		auto oldSize = m_shared->size;
		while (ARCH(m_shared->reading) && oldSize == m_shared->size) {
			if (await_update(ms) == WakeReason::Complete) {
				ARCH(m_shared->reading) = false;
			}
		}
	}

	void VecBase::end_read() {
		ARCH(m_shared->reading) = false;
	}

	std::uint32_t VecBase::size() const {
		return m_shared->size;
	}

	std::uint32_t VecBase::max_size() const noexcept {
		return m_maxSize;
	}

	std::uint32_t VecBase::capacity() const {
		return (m_shared->committedBytes / m_windowBytes) * m_windowSize;
	}

	bool VecBase::empty() const {
		return m_shared->size == 0;
	}

	void VecBase::clear() {
		m_shared->size = 0;
	}

	VecId VecBase::id() const {
		return m_id;
	}
}