
#pragma once

#include "libs.h"
#include <stdint.h>
#include <vector>
#include <array>
#include <assert.h>

#pragma warning(disable: 4200)

namespace entity
{

class Context;

struct Entity
{
	uint16_t type;
	uint16_t generation;
	uint32_t index;

	Entity() : type(-1), generation(-1), index(-1) {}
	Entity(uint16_t type, uint16_t generation, uint32_t index) : type(type), generation(generation), index(index) {}
};

using Type_id = uint32_t;

template <typename... Components>
class Foreach
{
public:
	template <typename Fn>
	void operator()(Context& ctx, Fn fn)
	{
		ctx.foreach(*this, std::forward<Fn>(fn));
	}

private:
	friend Context;
	uint32_t m_index;
};

class Context
{
public:
	~Context();

	//
	template <typename... Components>
	Type_id define()
	{
		std::array<uintptr_t, sizeof...(Components)> component_ids = { mp::type_id<Components>::value... };
		std::sort(component_ids.begin(), component_ids.end());
		
		add_components<0, Components...>();
		return find_create_entity_type(make_range(component_ids)) - m_entity_types.data();
	}

	//
	template <typename... Components>
	void define(entity::Foreach<Components...>& foreach)
	{
		foreach.m_index = define_foreach({ mp::type_id<Components>::value... });
	}

	//
	void setup();

	//
	Entity create(Type_id type_id);

	//
	void destroy(Entity entity);

	//
	void clear();

	//
	bool is_alive(Entity entity) const;

	//
	template <typename Component>
	Component* get(Entity entity)
	{
		return static_cast<Component*>(get(entity, mp::type_id<Component>::value));
	}

	//
	template <typename Fn, typename... Components>
	void foreach(entity::Foreach<Components...> foreach_, Fn fn)
	{
		auto& foreach = m_foreaches[foreach_.m_index];
		std::tuple<Components*...> component_arrays;
		for (uint32_t i = foreach.foreach_entity_first, ei = foreach.foreach_entity_first + foreach.foreach_entity_count; i < ei; ++i)
		{
			auto& foreach_entity = m_foreach_entities[i];
			assert(sizeof...(Components) == foreach_entity.component_ref_index_count);

			auto& entity_type = m_entity_types[foreach_entity.entity_index];
	
			uint32_t count = unwrap_component_arrays<0, decltype(component_arrays), Components...>(component_arrays, entity_type.components_ref_first, foreach_entity.component_ref_index_first);
			
			for (uint32_t j = 0; j < count; ++j)
			{
				invoke_foreach_fn(std::forward<Fn>(fn), component_arrays, j, mp::build_indices<sizeof...(Components)>{});
			}
		}
	}

private:
	struct Component_range
	{
#ifdef _DEBUG
		uint32_t component_index;
#endif
		uint32_t entity_type_index;
		uint32_t first;
		uint32_t size;
		uint32_t shift;
	};

	struct Component
	{
		uintptr_t id;
		uint32_t instance_size;
		uint32_t ranges_first;
		uint32_t ranges_count;
		uint32_t array_capacity;
		char*    array;
	};

	struct Component_ref
	{
		uintptr_t component_id;
		uint16_t component_index;
		uint16_t component_range_global_index;
	};

	struct Entity_type
	{
		uint32_t components_ref_first;
		uint32_t components_ref_count;
		std::vector<uint16_t> generation;
		std::vector<uint32_t> entity_to_component;
		std::vector<uint32_t> component_to_entity;
	}; 

	struct Foreach
	{
		uint32_t component_id_first;
		uint32_t component_id_count;
		uint32_t foreach_entity_first;
		uint32_t foreach_entity_count;
	};

	struct Foreach_entity
	{
		uint32_t entity_index;
		uint32_t component_ref_index_first;
		uint32_t component_ref_index_count;
	};

private:
	template <size_t I, typename T, typename... Ts>
	void add_components()
	{
		static_assert(std::is_trivial<T>::value, "Components type must be trivial.");

		auto component = find_component(mp::type_id<T>::value);
		if (!component)
		{
			static_assert(alignof(T) <= alignof(double), "Alignment greater than double not supported.");
			m_components.push_back(Component{ mp::type_id<T>::value, sizeof(T) });
		}
		add_components<I + 1, Ts...>();
	}

	template <size_t I>
	void add_components() {}

	Component const* find_component(size_t component_id) const;

	Component*       find_component(size_t component_id) { return const_cast<Component*>(static_cast<Context const*>(this)->find_component(component_id)); }

	Context::Entity_type* find_create_entity_type(range<uintptr_t const*> component_ids);

	uint32_t& component_push_back(uint32_t component_index, uint32_t component_range_index);

	void* get(Entity entity, uint32_t component_id);

	uint32_t define_foreach(std::initializer_list<uint32_t> component_ids);
	
	template <int I, typename Tuple, typename T, typename... Ts>
	uint32_t unwrap_component_arrays(Tuple& arrays, uint32_t component_ref_first, uint32_t component_ref_index_first)
	{
		auto& component_ref = m_component_refs[component_ref_first + m_ids[component_ref_index_first + I]];
		auto& component = m_components[component_ref.component_index];
		auto& range = m_component_ranges[component_ref.component_range_global_index];
		
		std::get<I>(arrays) = reinterpret_cast<T*>(component.array + range.first * component.instance_size);
		
		unwrap_component_arrays<I + 1, Tuple, Ts...>(arrays, component_ref_first, component_ref_index_first);
		
		return range.size;
	}

	template <int I, typename Tuple>
	void unwrap_component_arrays(Tuple&, uint32_t, uint32_t)
	{
	}

	template <typename Fn, typename Tuple, size_t... Is>
	static void invoke_foreach_fn(Fn fn, Tuple const& args, uint32_t i, mp::indices<Is...>)
	{
		return fn(std::get<Is>(args)[i]...);
	}

	static uint32_t shift_components_instances_index(Entity_type const& entity_type, Component_range const & range, uint32_t unshifted_index);

private:
	range<Component_ref*> get_component_ref_range(Entity_type const& etype)
	{
		auto begin = m_component_refs.data() + etype.components_ref_first;
		return { begin, begin + etype.components_ref_count };
	}

private:
	template<typename...> friend class Type;
	std::vector<uintptr_t> m_ids;
	std::vector<Component> m_components;
	std::vector<Component_range> m_component_ranges;
	std::vector<Entity_type> m_entity_types;
	std::vector<Component_ref> m_component_refs;
	std::vector<Foreach> m_foreaches;
	std::vector<Foreach_entity> m_foreach_entities;
};


} // entity
