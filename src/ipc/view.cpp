#pragma once

#define _IPC_VIEW_CPP

#ifndef _IPC_VIEW_H
#include "ipc/view.h"
#endif
#include "support/log.h"

namespace IPC {
	template<typename T>
	VecView<T>::VecView() :
		VecBase(InvalidVector, nullptr, 0, 0, 0, 0, 0),
		m_nextWindowIndex(0),
		m_currentWindow(0),
		m_currentWindowIndex(0),
		m_index(0),
		m_subIndex(0),
		m_buffer(nullptr)
	{

	}

	template<typename T>
	VecView<T>::VecView(VecId id, VecShare* shared, std::size_t windowElements, std::size_t windowBytes, std::size_t maxElements, std::size_t reservedBytes, std::size_t headerBytes) :
		VecBase(id, shared, windowElements, windowBytes, maxElements, reservedBytes, headerBytes),
		m_nextWindowIndex(windowElements),
		m_currentWindow(0),
		m_currentWindowIndex(0),
		m_index(0),
		m_subIndex(0),
		m_buffer(nullptr)
	{
		InterlockedIncrement(&m_shared->users);
	}

	template<typename T>
	VecView<T>::VecView(const VecView<T>& other) :
		VecBase(other.m_id, other.m_shared, other.m_windowSize, other.m_windowBytes, other.m_maxSize, other.m_reservedBytes, other.m_headerBytes),
		m_currentWindow(other.m_currentWindow),
		m_nextWindowIndex(other.m_nextWindowIndex),
		m_currentWindowIndex(other.m_currentWindowIndex),
		m_index(other.m_index),
		m_subIndex(other.m_subIndex),
		m_buffer(nullptr)
	{
		InterlockedIncrement(&m_shared->users);

		init();
	}

	template<typename T>
	VecView<T>::VecView(VecView<T>&& other) noexcept :
		VecBase(other.m_id, other.m_shared, other.m_windowSize, other.m_windowBytes, other.m_maxSize, other.m_reservedBytes, other.m_headerBytes),
		m_currentWindow(other.m_currentWindow),
		m_currentWindowIndex(other.m_currentWindowIndex),
		m_nextWindowIndex(other.m_nextWindowIndex),
		m_index(other.m_index),
		m_subIndex(other.m_subIndex),
		m_buffer(other.m_buffer)
	{
		other.m_buffer = nullptr;
		other.m_shared = nullptr;
	}

	template<typename T>
	VecView<T>& VecView<T>::operator=(const VecView<T>& other) {
		// if we're being assigned to ourselves, do nothing
		if (this == &other)
			return *this;

		// if we're being assigned a different view of the same
		// vec, take the fast path
		if (m_shared == other.m_shared) {
			m_index = other.m_index;
			m_subIndex = other.m_subIndex;
			m_currentWindowIndex = other.m_currentWindowIndex;
			m_nextWindowIndex = other.m_nextWindowIndex;
			slide_window(other.m_currentWindow);
			return *this;
		}

		// we're being assigned a view of a different vec, so free
		// all our old stuff first
		if (m_buffer != nullptr) {
			UnmapViewOfFile(m_buffer);
			m_buffer = nullptr;
		}

		if (m_shared != nullptr) {
			InterlockedDecrement(&m_shared->users);
		}

		m_shared = other.m_shared;
		m_index = other.m_index;
		m_subIndex = other.m_subIndex;
		m_nextWindowIndex = other.m_nextWindowIndex;
		m_currentWindowIndex = other.m_currentWindowIndex;
		m_currentWindow = other.m_currentWindow;
		m_windowBytes = other.m_windowBytes;
		m_windowSize = other.m_windowSize;
		m_maxSize = other.m_maxSize;
		m_reservedBytes = other.m_reservedBytes;

		InterlockedIncrement(&m_shared->users);

		init();

		return *this;
	}

	template<typename T>
	bool VecView<T>::init() {
		// the compiler complains about converting a 32-bit value to a HANDLE on the 64-bit side, but this code is only used on the 32-bit side
#ifndef MGE64_HOST
		m_buffer = static_cast<T*>(VirtualAlloc2(GetCurrentProcess(), NULL, m_windowBytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_READWRITE, NULL, 0));
		if (m_buffer == nullptr) {
			LOG::winerror("View of vector %llu failed to reserve placeholder memory", m_id);
			return false;
		}

		if (MapViewOfFile3(m_shared->sharedMem32, GetCurrentProcess(), m_buffer, m_currentWindow * m_windowBytes + m_headerBytes, m_windowBytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) == NULL) {
			LOG::winerror("View of vector %llu failed to map shared memory", m_id);
			VirtualFree(m_buffer, 0, MEM_RELEASE);
			m_buffer = nullptr;
			return false;
		}

		if (m_shared->committedBytes < m_windowBytes) {
			if (VirtualAlloc(m_buffer, m_windowBytes, MEM_COMMIT, PAGE_READWRITE) == NULL) {
				LOG::winerror("View of vector %llu failed to commit from shared memory", m_id);
				UnmapViewOfFile(m_buffer);
				m_buffer = nullptr;
				return false;
			}

			m_shared->committedBytes = m_windowBytes;
		}
#endif

		return true;
	}

	template<typename T>
	bool VecView<T>::slide_window(std::size_t i, bool isAppend) {
		// the compiler complains about converting a 32-bit value to a HANDLE on the 64-bit side, but this code is only used on the 32-bit side
#ifndef MGE64_HOST
		if (i == m_currentWindow && m_shared->committedBytes > 0)
			return true;

		if (m_buffer != nullptr && !UnmapViewOfFileEx(m_buffer, MEM_PRESERVE_PLACEHOLDER)) {
			LOG::winerror("View of vector %llu failed to unmap shared memory back to placeholder", m_id);
			return false;
		}

		auto offset = i * m_windowBytes + m_headerBytes;
		m_buffer = static_cast<T*>(MapViewOfFile3(m_shared->sharedMem32, GetCurrentProcess(), m_buffer, static_cast<ULONG64>(offset), m_windowBytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0));
		if (m_buffer == nullptr) {
			LOG::winerror("View of vector %llu failed to remap shared memory at offset %zu", m_id, offset);
			m_buffer = nullptr;
			return false;
		}

		auto newEnd = offset + m_windowBytes;
		// commit everything between the current end and the new end
		while (m_shared->committedBytes < newEnd) {
			if (VirtualAlloc(m_buffer, m_windowBytes, MEM_COMMIT, PAGE_READWRITE) == NULL) {
				LOG::winerror("View of vector %llu failed to commit %zu bytes of shared memory at address %p", m_id, m_windowBytes, m_buffer);
				UnmapViewOfFile(m_buffer);
				m_buffer = nullptr;
				return false;
			}
			m_shared->committedBytes += m_windowBytes;
			if (m_shared->committedBytes < newEnd) {
				UnmapViewOfFileEx(m_buffer, MEM_PRESERVE_PLACEHOLDER);
			}
		}

		m_currentWindow = i;
		m_currentWindowIndex = m_currentWindow * m_windowSize;
		m_nextWindowIndex = m_currentWindowIndex + m_windowSize;

		if (m_writing && isAppend) {
			return update();
		}
#endif

		return true;
	}

	template<typename T>
	VecView<T>::~VecView() {
		if (m_shared != nullptr) {
			InterlockedDecrement(&m_shared->users);
		}

		if (m_buffer != nullptr) {
			UnmapViewOfFile(m_buffer);
			m_buffer = nullptr;
		}
	}

	template<typename T>
	T& VecView<T>::operator*() const {
		return m_buffer[m_subIndex];
	}

	template<typename T>
	T* VecView<T>::operator->() const {
		return &m_buffer[m_subIndex];
	}

	template<typename T>
	VecView<T>& VecView<T>::operator++() {
		m_index++;
		m_subIndex++;
		if (m_index >= m_shared->size) {
			wait_read();
		}

		if (m_index >= m_nextWindowIndex) {
			m_subIndex = 0;
			slide_window(m_currentWindow + 1);
		}

		return *this;
	}

	template<typename T>
	VecView<T> VecView<T>::operator++(int) {
		auto beforeIncrement = *this;
		++(*this);
		return beforeIncrement;
	}

	template<typename T>
	VecView<T>& VecView<T>::operator--() {
		m_index--;
		m_subIndex--;
		if (m_index < m_currentWindowIndex) {
			m_subIndex = m_windowSize - 1;
			slide_window(m_currentWindow - 1);
		}

		return *this;
	}

	template<typename T>
	VecView<T> VecView<T>::operator--(int) {
		auto beforeDecrement = *this;
		--(*this);
		return beforeDecrement;
	}

	template<typename T>
	bool VecView<T>::operator==(const VecView<T>& other) const {
		return m_shared == other.m_shared && m_index == other.m_index;
	}

	template<typename T>
	bool VecView<T>::operator!=(const VecView<T>& other) const {
		return m_shared != other.m_shared || m_index != other.m_index;
	}

	template<typename T>
	VecView<T> VecView<T>::begin() const {
		VecView it(*this);
		it.set_index(0);
		return it;
	}

	template<typename T>
	VecView<T> VecView<T>::end() const {
		VecView it(*this);
		// slight hack to point the view past the end
		it.m_index = static_cast<std::size_t>(m_shared->size);
		it.m_subIndex = it.m_index % m_windowSize;
		it.slide_window(it.m_index / m_windowSize);
		return it;
	}

	template<typename T>
	void VecView<T>::truncate(std::size_t numElements) {
		if (numElements < m_shared->size) {
			m_shared->size = numElements;
			if (m_index >= numElements) {
				m_index = numElements - 1;
				m_subIndex = m_index % m_windowSize;
			}
		}
	}

	template<typename T>
	bool VecView<T>::set_index(std::size_t i) {
		if (m_index == i)
			return true;

		if (i >= m_shared->size)
			return false;

		m_index = i;
		m_subIndex = i % m_windowSize;
		return slide_window(i / m_windowSize);
	}

	template<typename T>
	bool VecView<T>::at_end() const {
		return m_index >= m_shared->size;
	}

	template<typename T>
	T& VecView<T>::operator[](std::size_t i) {
		auto window = i / m_windowSize;
		m_subIndex = i % m_windowSize;
		m_index = i;
		slide_window(window);
		return m_buffer[m_subIndex];
	}

	template<typename T>
	T& VecView<T>::front() {
		m_index = 0;
		m_subIndex = 0;
		slide_window(0);
		return *m_buffer;
	}

	template<typename T>
	T& VecView<T>::back() {
		return (*this)[m_shared->size - 1];
	}

	template<typename T>
	bool VecView<T>::push_back(const T& value) {
		auto newIndex = m_shared->size;
		auto window = newIndex / m_windowSize;
		m_subIndex = newIndex % m_windowSize;
		if (!slide_window(static_cast<std::size_t>(window), true))
			return false;

		m_buffer[m_subIndex] = value;
		m_index = static_cast<std::size_t>(m_shared->size++);
		return true;
	}

	template<typename T>
	bool VecView<T>::push_back(T&& value) {
		auto newIndex = m_shared->size;
		auto window = newIndex / m_windowSize;
		m_subIndex = newIndex % m_windowSize;
		if (!slide_window(static_cast<std::size_t>(window), true))
			return false;

		m_buffer[m_subIndex] = value;
		m_index = static_cast<std::size_t>(m_shared->size++);
		return true;
	}

	template<typename T>
	std::optional<T> VecView<T>::pop_back() {
		if (m_shared->size == 0)
			return std::nullopt;

		m_index = static_cast<std::size_t>(--m_shared->size);
		return (*this)[m_index];
	}

	template<typename T>
	bool VecView<T>::reserve(std::size_t count) {
		auto windowsNeeded = (count + m_windowSize - 1) / m_windowSize;
		auto bytesNeeded = windowsNeeded * m_windowBytes;
		if (bytesNeeded > m_shared->committedBytes) {
			if (!slide_window(windowsNeeded - 1))
				return false;
			// move back to original window position
			if (!set_index(m_index))
				return false;
		}

		return true;
	}
}