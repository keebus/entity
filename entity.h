
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

// The integer type of an entity type id.
using Entity_type_id = uint16_t;

// An entity is only a lightweight collection of three indices: the entity-type id, used to identify
// which entity type (collection of components) the entity belongs to, a generation counter, used to
// track entity lifetime and the actual entity index within the entity type.
// All entity operations are performed through a `Context` instance.
struct Entity
{
	// The entity type id.
	Entity_type_id type;

	// The entity generation counter, for lifetime tracking.
	uint16_t generation;

	// The entity index within its entity type.
	uint32_t index;

	Entity() : type(-1), generation(-1), index(-1) {}

	Entity(Entity_type_id type, uint16_t generation, uint32_t index) : type(type), generation(generation), index(index) {}
};

// This class is used to refer to a prepared entity foreach statement. This is a thin wrapper over
// the foreach object living in the Context. This class is mainly used to wrap together the list
// of components to iterate over and provide a convenient way to invoke the foreach.
template <typename... Components>
class Foreach
{
public:
	// See `Context::foreach`.
	template <typename Fn>
	void operator()(Context& ctx, Fn fn)
	{
		ctx.foreach(*this, std::forward<Fn>(fn));
	}

private:
	friend Context;

	// Index of the defined foreach statement in the Context.
	uint32_t m_index;
};

// A context manages all entity operations. Start by defining entity types, i.e. the various possible
// sets of components that make up the entities in your application. You can only create entities out
// of a previously defined entity type. Entities created out of an entity type will be mapped to an
// instance of each of entity type previously declared components. Once your entity types have been
// defined you should define foreach statements. These let you iterate over all specific sets of
// components belonging to the same entities. Finally, setup the context. This will optimize and
// compile defined entity types and foreach statements. After setup, a Context allows you to create,
// manage and destroy instances, as well as invoke defined foreach statements. The implementation is
// optimized for the most frequent case in a real time application, which is iterating and updating
// existing entities, at the cost of making entity creation less efficient in the worst case. That
// said, the cost of creating/destructing entities is amortized over time.
class Context
{
public:
	~Context();

	// Defines an entity type to have specified set of Components. The order of components is
	// irrelevant. It returns the entity type id associated to specified set of Components.
	// Call this function strictly before calling `setup()`.
	template <typename... Components>
	Entity_type_id define()
	{
		std::array<uintptr_t, sizeof...(Components)> component_ids = { mp::type_id<Components>::value... };
		std::sort(component_ids.begin(), component_ids.end());
		
		add_components<0, Components...>();
		return find_create_entity_type(make_range(component_ids)) - m_entity_types.data();
	}

	// Defines a foreach statement to iterate over specified list of Components. The order is
	// important as it will match the order in which arguments are declared in the foreach function
	// body.
	// Call this function strictly before calling `setup()`.
	template <typename... Components>
	void define(entity::Foreach<Components...>& foreach)
	{
		foreach.m_index = define_foreach({ mp::type_id<Components>::value... });
	}

	// Optimizes and compiles previously defined entity types and foreach statements. After setting
	// the context up, new entity types or foreach statements are not allowed to be defined and all
	// entity operations (create, destroy, etc) are available, as well as invoking foreach
	// statements.
	void setup();

	// Creates an entity of specified [type].
	Entity create(Entity_type_id type);

	// Destroys [entity]. The entity must be alive.
	void destroy(Entity entity);

	// Destroys all entities without actually releasing any memory.
	void clear();

	// Returns whether [entity] is alive (has not been destroyed or context cleared).
	bool is_alive(Entity entity) const;

	// Retrieves the specified `Component` from given [entity]. If entity does not specify the
	// content, nullptr is returned instead.
	template <typename Component>
	Component* get(Entity entity)
	{
		return static_cast<Component*>(get(entity, mp::type_id<Component>::value));
	}

	// Executes provided function [fn] over all instances of Components... in the context [ctx]
	// belonging to a live entity. The function is expected to take a non-const reference to
	// all Components and return void.
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

	struct Helper;

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
