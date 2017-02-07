
#pragma once

#include <stdint.h>
#include <algorithm>
#include <array>

namespace mp {

template <size_t... Is>
struct indices {};

template <size_t N, size_t... Is>
struct build_indices : build_indices<N - 1, N - 1, Is...> {};

template <size_t... Is>
struct build_indices<0, Is...> : indices<Is...> {};

template <typename T>
struct type_id
{
	static uintptr_t const value;

private:
	static char storage;
};

template <typename T> char type_id<T>::storage;
template <typename T> uintptr_t const type_id<T>::value = (uintptr_t)&type_id<T>::storage;

}

template <typename T>
struct range
{
	range(std::pair<T, T> pair)
		: m_begin(pair.first)
		, m_end(pair.second)
	{}

	range(T begin, T end)
	: m_begin(begin)
	, m_end(end)
	{}

	T begin() { return m_begin; }
	T end() { return m_end; }
	size_t size() const { return std::distance(m_begin, m_end); }

	T m_begin;
	T m_end;
};

template <typename T>
range<T*> make_range(T* array, size_t size)
{
	return range<T*>{ array, array + size };
}

template <typename T, size_t N>
range<T*> make_range(std::array<T, N>& array)
{
	return { array.data(), array.data() + N };
}
