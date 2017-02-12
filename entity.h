
#pragma once

#include "libs.h"
#include <stdint.h>
#include <vector>
#include <array>
#include <assert.h>
#include <deque>

#pragma warning(disable: 4200)

namespace entity {

class Context;

// The integer type of an entity type id.
using Type = uint16_t;

// An entity is only a lightweight collection of three indices: the entity-type id, used to identify
// which entity type (collection of components) the entity belongs to, a generation counter, used to
// track entity lifetime and the actual entity index within the entity type.
// All entity operations are performed through a `Context` instance.
struct Entity
{
	// The entity type id.
	Type type;

	// The entity generation counter, for lifetime tracking.
	uint16_t generation;

	// The entity index within its entity type.
	uint32_t index;

	Entity() : type((Type)-1), generation(0), index(0) {}

	Entity(Type type, uint16_t generation, uint32_t index) : type(type), generation(generation), index(index) {}
};

template <typename... Components>
class Foreach_control;

// This class is used to refer to a prepared entity foreach statement. This is a thin wrapper over
// the foreach object living in the Context. This class is mainly used to wrap together the list
// of components to iterate over and provide a convenient way to invoke the foreach.
template <typename... Components>
class Foreach
{
private:
	friend Context;

	// Index of the defined foreach in the Context.
	uint32_t m_index = (uint32_t)-1;
};

enum Flags
{
	// Tells the context that some entity was created.
	Entity_created = 1,

	// Tells the context that some entity was destroyed.
	Entity_destroyed = 2,
};

// A context manages all entity operations. Start by defining entity types, i.e. the various possible
// sets of components that make up the entities in your application. You can only create entities out
// of a previously defined entity type. Entities created out of an entity type will be mapped to an
// instance of each of entity type previously declared components. Once your entity types have been
// defined you should define foreach instances. These let you iterate over all specific sets of
// components belonging to the same entities. Finally, setup the context. This will optimize and
// compile defined entity types and foreach instances. After setup, a Context allows you to create,
// manage and destroy instances, as well as invoke defined foreach instances. The implementation is
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
	Type define()
	{
		assert(!is_setup());
		std::array<uintptr_t, sizeof...(Components)> component_ids = { mp::type_id<Components>::value... };
		add_components<0, Components...>();
		return (Type)(find_create_entity_type(make_range(component_ids)) - m_entity_types.data());
	}

	// Defines a foreach instances to iterate over specified list of Components. The order is
	// important as it will match the order in which arguments are declared in the foreach function
	// body.
	// Call this function strictly before calling `setup()`.
	template <typename... Components>
	void define(entity::Foreach<Components...>& foreach)
	{
		assert(!is_setup());
		foreach.m_index = define_foreach({ mp::type_id<Components>::value... });
	}

	// Optimizes and compiles previously defined entity types and foreach instances. After setting
	// the context up, new entity types or foreach instances are not allowed to be defined and all
	// entity operations (create, destroy, etc) are available, as well as invoking foreach
	// statements.
	void setup();

	// Returns whether the Context has been set up.
	bool is_setup() const { return m_components.size() && m_components[0].array_capacity; }

	// Creates an entity of specified [type].
	Entity create(Type type);

	// Destroys [entity]. The entity must be alive.
	void destroy(Entity entity);

	// Destroys all entities without actually releasing any memory.
	void clear();

	// Returns whether [entity] is alive (has not been destroyed or context cleared).
	bool is_alive(Entity entity) const;

	// Retrieves the specified `Component` from given [entity]. If entity does not specify the
	// content, nullptr is returned instead.
	template <typename Component>
	Component* try_get(Entity entity)
	{
		assert(is_setup());
		return static_cast<Component*>(get_component_instance(entity, mp::type_id<Component>::value));
	}

	// Retrieves the specified `Component` from given [entity]. If entity does not specify the
	// content, nullptr is returned instead.
	template <typename Component>
	Component& get(Entity entity)
	{
		assert(is_setup());
		auto component = try_get<Component>(entity);
		assert(component);
		return *component;
	}

	// Executes provided function [fn] over all instances of Components... in the context [ctx]
	// belonging to a live entity. The function is expected to take a non-const reference to
	// all Components and return void.
	template <typename Fn, typename... Components>
	auto foreach(entity::Foreach<Components...> foreach_, Fn fn)
	{
		assert(is_setup());
		assert(foreach_.m_index < m_foreaches.size() && "Executing undefined foreach.");
		auto& foreach = m_foreaches[foreach_.m_index];
		std::tuple<Components*...> component_arrays;
		for (auto& foreach_stmt : make_range(m_foreach_stmts.data() + foreach.foreach_stmt_first, foreach.foreach_stmt_count))
		{
			auto& entity_type = m_entity_types[foreach_stmt.entity_type_index];
			assert(sizeof...(Components) == foreach_stmt.component_ref_index_count);
	
			unwrap_component_arrays<0, decltype(component_arrays), Components...>(component_arrays, entity_type.components_ref_first, foreach_stmt.component_ref_index_first);
			
			for (uint32_t j = 0, n = entity_type.alive_count; j < n; ++j)
			{
				invoke_foreach_fn(std::forward<Fn>(fn), component_arrays, j, mp::build_indices<sizeof...(Components)>{});
			}
		}
	}

	// Executes provided function [fn] over all instances of Components... in the context [ctx]
	// belonging to a live entity. The function is expected to take a non-const reference to
	// Foreach_control followed by all Components and return void.
	// The user should use this version when new entities are created/destroyed during iteration.
	template <typename Fn, typename... Components>
	auto foreach_control(entity::Foreach<Components...> foreach_, Fn fn)
	{
		do_foreach_control<Fn, Components...>(foreach_.m_index, std::forward<Fn>(fn), 0, 0);
	}
	
private:
	// Wraps info about a component type.
	struct Component
	{
		// The id of the component equal to mp::type_of<Component>::value.
		uintptr_t id;

		// Size in bytes of a single instance of this component.
		uint32_t instance_size;

		// Wraps the instance default constructor.
		void (*construct)(void*);

		// Index of the first range associated to this component in `m_component_ranges`.
		uint32_t ranges_first;

		// Number of ranges associated to this component. This also means number of entities that
		// have this component.
		uint32_t ranges_count;

		// This array maps all instances of this component indices to their logical entity index
		// within the component range they belong to.
		uint32_t* physical_to_logical;

		// Capacity (number of instances) the array allocation can hold.
		uint32_t array_capacity;

		// Array of component instances.
		char*    array;
	};
	
	// Identifies a range of components in the component array.
	struct Component_range
	{
#ifdef _DEBUG
		// Component index, used for debugging.
		uint32_t component_index;
#endif
		
		// Index of the entity type this range belongs to.
		Type entity_type_index;

		// Index of the first component in the component array.
		uint32_t first_physical_index;
		
		// Mapping of entity index to component index in component ranges associated to this entity
		// type before range shifting.
		std::vector<uint32_t> logical_to_physical;
	};

	// Wraps info about an entity type (collection of components).
	struct Entity_type
	{
		// Index of the first component ref in `m_component_refs` this entity has.
		uint32_t components_ref_first;

		// Number of components this entity has.
		uint32_t components_count;
		
		// Number of live entities of this type; also the span of each Component_range.
		uint32_t alive_count;

		// Array of generation counters per entity index, used to track entity lifetime.
		std::vector<uint16_t> generation;
		
		// Deque of indices free to be used. #todo this should be optimized to a linked list of
		// indices instead reusing the index arrays like logical_to_physical.
		std::deque<uint32_t> free_indices;
	};
	
	// Entity type reference to a component type.
	struct Component_ref
	{
		// Id of the component.
		uintptr_t component_id;

		// Index of the component in `m_components`.
		uint32_t component_index;

		// Index in `m_component_ranges` of the range of instances belonging to this entity type.
		uint32_t component_range_global_index;
	};

	// Wraps info about a foreach instances.
	struct Foreach
	{
		// First index in `m_ids` that maps to the first component in this foreach component list.
		uint32_t component_id_first;

		// Number of components in this foreach component list.
		uint32_t component_id_count;

		// Index `m_foreach_entities` to the first foreach statement.
		uint32_t foreach_stmt_first;

		// Number of foreach statements.
		uint32_t foreach_stmt_count;
	};

	// A foreach statement, that provides info about an entity providing this foreach component list.
	struct Foreach_stmt
	{
		// Index in `m_entity_types` of entity type it stmt iterates over.
		Type  entity_type_index;
		
		// Index in `m_ids` of the first index in this stmt entity type component refs.
		uint32_t component_ref_index_first;

		// #todo remove
		uint32_t component_ref_index_count;
	};

private:
	// Tests each component type T whether is already in the `m_components` array and if it isn't
	// it adds it.
	template <size_t I, typename T, typename... Ts>
	void add_components()
	{
		static_assert(std::is_default_constructible<T>::value, "Components type must be default constructible.");
		static_assert(std::is_trivially_copy_constructible<T>::value && std::is_trivially_copy_assignable<T>::value, "Components type must be trivially copy constructible/assignable.");
		static_assert(std::is_trivially_destructible<T>::value, "Components type must be trivially destructible.");

		auto component = find_component(mp::type_id<T>::value);
		if (!component)
		{
			static_assert(alignof(T) <= alignof(double), "Alignment greater than double not supported.");
			m_components.push_back(Component{ mp::type_id<T>::value, sizeof(T), [](void* ptr) { new (ptr) T{}; } });
		}
		add_components<I + 1, Ts...>();
	}

	// Base case.
	template <size_t I>
	void add_components() {}

	// Finds component with specified [component_id] ( O(n) complexity ).
	Component const* find_component(size_t component_id) const;

	// Finds component with specified [component_id] ( O(n) complexity ).
	Component*       find_component(size_t component_id) { return const_cast<Component*>(static_cast<Context const*>(this)->find_component(component_id)); }

	// Checks whether an entity type with specifier components has already been defined and if it
	// isn't it adds a new entity type and returns a pointer to it.
	Entity_type* find_create_entity_type(range<uintptr_t*> component_ids);

	// Fetches the component instance that belongs to specified [entity].
	void* get_component_instance(Entity entity, uint32_t component_id);

	// Defines a foreach statement with given components and returns its index in `m_foreaches`.
	uint32_t define_foreach(std::initializer_list<uint32_t> component_ids);
	
	// Runs the controlled foreach.
	template <typename Fn, typename... Components>
	void do_foreach_control(uint32_t foreach_index, Fn fn, uint32_t foreach_stmt_index, uint32_t iteration)
	{
		assert(is_setup());
		assert(foreach_index < m_foreaches.size() && "Executing undefined foreach.");

		auto& foreach = m_foreaches[foreach_index];
		std::tuple<Components*...> component_arrays;
		Foreach_control<Components...> control{ this, foreach_index, &foreach_stmt_index, &iteration };

		for (; foreach_stmt_index < foreach.foreach_stmt_count; ++foreach_stmt_index)
		{
			auto& foreach_stmt = m_foreach_stmts[foreach.foreach_stmt_first + foreach_stmt_index];
			auto& entity_type = m_entity_types[foreach_stmt.entity_type_index];
			assert(sizeof...(Components) == foreach_stmt.component_ref_index_count);
	
			unwrap_component_arrays<0, decltype(component_arrays), Components...>(component_arrays, entity_type.components_ref_first, foreach_stmt.component_ref_index_first);

			control.m_type = foreach_stmt.entity_type_index;

			for (; iteration < entity_type.alive_count; ++iteration)
			{
				control.m_flags = 0;
				
				invoke_foreach_fn(std::forward<Fn>(fn), control, component_arrays, iteration, mp::build_indices<sizeof...(Components)>{});

				if (control.is_flag_set(Entity_created))
				{
					unwrap_component_arrays<0, decltype(component_arrays), Components...>(component_arrays, entity_type.components_ref_first, foreach_stmt.component_ref_index_first);
				}
				else if (control.is_flag_set(Entity_destroyed) && !is_alive(control.entity()))
				{
					--iteration;
				}
			}

			iteration = 0;
		}
	}

	// Helper function that sets the typed component arrays into specified tuple [arrays].
	template <int I, typename Tuple, typename T, typename... Ts>
	void unwrap_component_arrays(Tuple& arrays, uint32_t component_ref_first, uint32_t component_ref_index_first)
	{
		auto& component_ref = m_component_refs[component_ref_first + m_ids[component_ref_index_first + I]];
		auto& component = m_components[component_ref.component_index];
		auto& range = m_component_ranges[component_ref.component_range_global_index];
		
		std::get<I>(arrays) = reinterpret_cast<T*>(component.array + range.first_physical_index * component.instance_size);
		
		unwrap_component_arrays<I + 1, Tuple, Ts...>(arrays, component_ref_first, component_ref_index_first);
	}

	// Base case.
	template <int I, typename Tuple>
	void unwrap_component_arrays(Tuple&, uint32_t, uint32_t)
	{
	}

	// Helper function that invokes a foreach function from specified component arrays [args] and
	// iteration index [i].
	template <typename Fn, typename Tuple, size_t... Is>
	static void invoke_foreach_fn(Fn fn, Tuple const& args, uint32_t i, mp::indices<Is...>)
	{
		return fn(std::get<Is>(args)[i]...);
	}

	// Helper function that invokes a foreach function from specified component arrays [args] and
	// iteration index [i].
	template <typename Fn, typename Control, typename Tuple, size_t... Is>
	static void invoke_foreach_fn(Fn fn, Control& control, Tuple const& args, uint32_t i, mp::indices<Is...>)
	{
		return fn(control, std::get<Is>(args)[i]...);
	}

	// Structure that contains helper functions defined in the cpp.
	struct Private;

private:
	template <typename...> friend class Foreach_control;

	// Array of indices, used to index heterogeneous arrays.
	std::vector<uintptr_t> m_ids;

	// Array of defined component types.
	std::vector<Component> m_components;

	// Array of component ranges in component instances arrays.
	std::vector<Component_range> m_component_ranges;

	// Array of defined entity types.
	std::vector<Entity_type> m_entity_types;
	
	// Array of component references from entity types.
	std::vector<Component_ref> m_component_refs;

	// Array of foreach instances.
	std::vector<Foreach> m_foreaches;

	// Array of foreach statements referenced by foreach instances.
	std::vector<Foreach_stmt> m_foreach_stmts;
};

// Lightweight class optionally used in entity components foreach iterations to fetch additional
// information about the current entity (e.g. the id) and to instruct the context that changes have
// been made, e.g. an entity was created or destroyed.
template <typename... Components>
class Foreach_control
{
public:
	// Returns the context that is executing the foreach.
	Context& context() { return *m_context; }

	// Returns the current iteration entity id.
	Entity entity() const
	{
		auto& entity_type = m_context->m_entity_types[m_type];
		auto& first_component_ref = m_context->m_component_refs[entity_type.components_ref_first];
		auto& first_component_range = m_context->m_component_ranges[first_component_ref.component_range_global_index];
		auto& physical_to_logical = m_context->m_components[first_component_ref.component_index].physical_to_logical;
		uint32_t logical_index = physical_to_logical[first_component_range.first_physical_index + *m_iteration];
		return { m_type, m_context->m_entity_types[m_type].generation[logical_index], logical_index };
	}

	// Sets a [flag].
	void set_flag(Flags flag) { m_flags |= flag; }

	// Returns whether [flag] is enabled.
	bool is_flag_set(Flags flag) { return (m_flags & flag) == flag; }

	// Helper function that destroys current entity.
	void destroy_entity()
	{
		m_context->destroy(entity());
		m_flags |= Entity_destroyed;
	}

	// Performs a nested foreach starting from the entity after current one.
	template <typename Fn>
	void nested_call(Fn fn)
	{
		m_context->do_foreach_control<Fn, Components...>(m_foreach_index, std::forward<Fn>(fn), *m_foreach_stmt_index, *m_iteration + 1);
	}
	
private:
	Foreach_control(Context* context, uint32_t foreach_index, uint32_t* foreach_stmt_index, uint32_t* iteration)
		: m_context(context), m_foreach_index(foreach_index), m_foreach_stmt_index(foreach_stmt_index), m_iteration(iteration)
	{}

	friend Context;

	// Context the control is associated to.
	Context* m_context;

	// Index of the foreach instance in Context::m_foreaches.
	uint32_t m_foreach_index;

	// Index of the current foreach statement.
	uint32_t* m_foreach_stmt_index;

	// Index of the current entity type.
	Type m_type;

	// Pointer to the current iteration.
	uint32_t* m_iteration;

	//
	int m_flags;
};

} // namespace entity
