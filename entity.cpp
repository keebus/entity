
#include "entity.h"
#include <assert.h>
#include <algorithm>

namespace entity
{

struct Context::Private
{
	static uint32_t get_range_capacity(Context const& ctx, Component const& component, uint32_t component_range_global_index)
	{
		if (component_range_global_index + 1 < component.ranges_first + component.ranges_count)
		{
			return ctx.m_component_ranges[component_range_global_index + 1].first - ctx.m_component_ranges[component_range_global_index].first;
		}
		else
		{
			return component.array_capacity;
		}
	}

	// Pushes an instance of specified component at the back of its range. After this function
	// returns, an empty instance is at the end of specified component array. The size of the range
	// is not effected and a reference to it is returned.
	static void component_push_back(Context& ctx, Component& component, uint32_t component_range_global_index)
	{
		auto& component_range = ctx.m_component_ranges[component_range_global_index];

		uint32_t back_index = component_range.first + ctx.m_entity_types[component_range.entity_type_index].alive_count;

		if (component_range_global_index + 1 < component.ranges_first + component.ranges_count)
		{
			// This range is followed by another range. See if the next range starts at least one
			// instance away from this range last, which means we can insert our new instance without
			// moving memory around.
			auto& next_component_range = ctx.m_component_ranges[component_range_global_index + 1];

			// If there's no space at the end of this range for one more instance...
			if (back_index >= next_component_range.first)
			{
				assert(back_index == next_component_range.first);

				// Make room for one more at the end of next range.
				component_push_back(ctx, component, component_range_global_index + 1);

				uint32_t next_component_back_index = next_component_range.first + ctx.m_entity_types[next_component_range.entity_type_index].alive_count;

				char* dest_ptr = component.array + next_component_back_index * component.instance_size;
				char* back_ptr = component.array + back_index * component.instance_size;

				// Move first element to the newly inserted element.
				memcpy(dest_ptr, back_ptr, component.instance_size);

				// Update logical to physical index mapping.
				if (ctx.m_entity_types[next_component_range.entity_type_index].alive_count)
				{
					uint32_t logical_index = component.physical_to_logical[back_index];
					next_component_range.logical_to_physical[logical_index] = next_component_back_index;
					component.physical_to_logical[next_component_back_index] = logical_index;
				}

				// Shift the next range up by one.
				++next_component_range.first;
			}
		}
		else
		{
			// This is the last component range. We shall operate on the instances array itself.
			if (back_index >= component.array_capacity)
			{
				// Not enough space. Grow the instance array.
				component.array_capacity *= 2;
				component.array = (char*)realloc(component.array, component.array_capacity * component.instance_size);
				component.physical_to_logical = (uint32_t*)realloc(component.physical_to_logical, component.array_capacity * sizeof(uint32_t));
			}
		}

		// Clear the new component instance memory.
		char* back_ptr = component.array + back_index * component.instance_size;
		memset(back_ptr, 0, component.instance_size);
	}

	static range<Component_ref*> get_component_ref_range(Context& ctx, Entity_type const& etype)
	{
		auto begin = ctx.m_component_refs.data() + etype.components_ref_first;
		return{ begin, begin + etype.components_ref_count };
	}
};

Context::~Context()
{
	for (auto& component : m_components)
	{
		free(component.array);
		free(component.physical_to_logical);
	}
}

void Context::setup()
{
	// Temporary array that holds the number of ranges pushed per component (index).
	std::vector<uint32_t> component_range_end;
	component_range_end.resize(m_components.size(), 0U);

	for (auto& component : m_components)
	{
		// Reserve required ranges from the array and resolve the 'first' index.
		component.ranges_first = m_component_ranges.size();
		m_component_ranges.resize(m_component_ranges.size() + component.ranges_count);

		// Allocate initial component instances memory.
		component.array_capacity = 16;
		component.array = (char*)malloc(component.array_capacity * component.instance_size);
		component.physical_to_logical = (uint32_t*)malloc(component.array_capacity * sizeof(*component.physical_to_logical));
	}

	// #todo optimization: sort entity types by the number of components they have, and sort
	// components by frequency (maybe?).

	for (auto& entity_type : m_entity_types)
	{
		for (auto& component_ref : Private::get_component_ref_range(*this, entity_type))
		{
			// Resolve component index from component id.
			component_ref.component_index = find_component(component_ref.component_id) - m_components.data();

			// Setup the appropriate component range with this entity type index.
			component_ref.component_range_global_index = m_components[component_ref.component_index].ranges_first + component_range_end[component_ref.component_index]++;

			// Set range entity type index to this entity type index.
			m_component_ranges[component_ref.component_range_global_index].entity_type_index = &entity_type - m_entity_types.data();

#ifdef _DEBUG
			m_component_ranges[component_ref.component_range_global_index].component_index = component_ref.component_index;
#endif
		}
	}
}

Entity Context::create(Entity_type_id type_id)
{
	auto& entity_type = m_entity_types[type_id];

	// Generate a new local index for the new entity.
	uint32_t logical_index;

	// #todo
	if (!entity_type.free_indices.empty())
	{
		// #todo
		logical_index = entity_type.free_indices.front();
		entity_type.free_indices.pop_front();
	}
	else
	{
		// No reusable indices. Create a new logical index and map it to the component index.
		logical_index = entity_type.alive_count;
		for (auto& component_ref : Private::get_component_ref_range(*this, entity_type))
		{
			m_component_ranges[component_ref.component_range_global_index].logical_to_physical.emplace_back();
		}
		entity_type.generation.push_back(0);
	}

	for (auto& component_ref : Private::get_component_ref_range(*this, entity_type))
	{
		auto& component = m_components[component_ref.component_index];
		auto& range = m_component_ranges[component_ref.component_range_global_index];

		Private::component_push_back(*this, component, component_ref.component_range_global_index);

		uint32_t physical_index = range.first + entity_type.alive_count;
		range.logical_to_physical[logical_index] = physical_index;
		component.physical_to_logical[physical_index] = logical_index;
	}

	++entity_type.alive_count;

	return Entity{ (uint16_t)type_id, entity_type.generation[logical_index], logical_index };
}

void Context::destroy(Entity entity)
{
	// Entity destruction involves two steps: moving the last instance of each entity component range
	// into delete component location reducing by one the range of components and updating the
	// mapping between entity indices and component indices to affect this change. By adding a level
	// of indirection between entity index and component index we effectively make deletion not
	// terribly expensive as we don't need to update all existing entities but only one.

	assert(is_alive(entity));
	auto& entity_type = m_entity_types[entity.type];
	
	entity_type.free_indices.push_back(entity.index);

	// Increment generation count so that all entity ids like `entity` are now not alive.
	++entity_type.generation[entity.index];

	--entity_type.alive_count;

	// Move the last entity in the range in the position of the deleted component.
	for (auto& component_ref : Private::get_component_ref_range(*this, entity_type))
	{
		auto& component = m_components[component_ref.component_index];
		auto& range = m_component_ranges[component_ref.component_range_global_index];

		const uint32_t destroyed_physical_index = range.logical_to_physical[entity.index];
		const uint32_t back_physical_index = range.first + entity_type.alive_count;

		char* destroyed_instance_ptr = component.array + destroyed_physical_index * component.instance_size;
		char* back_instance_ptr = component.array + back_physical_index * component.instance_size;

		memcpy(destroyed_instance_ptr, back_instance_ptr, component.instance_size);

		// #todo
		uint32_t back_logical_index = component.physical_to_logical[back_physical_index];
		component.physical_to_logical[destroyed_physical_index] = back_logical_index;
		range.logical_to_physical[back_logical_index] = destroyed_physical_index;
	}
}

void Context::clear()
{
	// Reset all entity lists.
	for (auto& entity_type : m_entity_types)
	{
#ifdef _DEBUG
		for (auto& component_ref : Private::get_component_ref_range(*this, entity_type))
		{
			auto& component = m_components[component_ref.component_index];
			auto& range = m_component_ranges[component_ref.component_range_global_index];
			assert(range.logical_to_physical.size() == entity_type.generation.size());
		}
#endif

		entity_type.alive_count = 0;

		// Make all entity indices free.
		entity_type.free_indices.clear();
		for (uint32_t i = 0; i < entity_type.generation.size(); ++i)
		{
			entity_type.free_indices.push_back(i);
			++entity_type.generation[i];
		}
	}
}

bool Context::is_alive(Entity entity) const
{
	return	entity.type < m_entity_types.size() &&
			m_entity_types[entity.type].generation[entity.index] == entity.generation;
}

Context::Component const* Context::find_component(size_t component_id) const
{
	for (auto& component : m_components)
		if (component.id == component_id)
			return &component;
	return nullptr;
}

Context::Entity_type* Context::find_create_entity_type(range<uintptr_t*> component_ids)
{
	// Sort the component ids to simplify search.
	std::sort(component_ids.begin(), component_ids.end());

	// See if there's a previously defined entity type matching specified components. If so, just
	// return that, no need to create the same entity type twice.
	for (auto& entity_type : m_entity_types)
	{
		if (component_ids.size() != entity_type.components_ref_count)
			continue;

		auto components_ref_range = Private::get_component_ref_range(*this, entity_type);

		uint32_t i = 0;
		for (; i < component_ids.size(); ++i)
			if (component_ids.begin()[i] != components_ref_range.begin()[i].component_id)
				break;

		if (i == component_ids.size())
			return &entity_type;
	}

	// Entity type not found, create one.
	auto components_ref_first = m_component_refs.size();
		
	for (auto component_id : component_ids)
	{
		m_component_refs.push_back({ component_id });
		++find_component(component_id)->ranges_count;
	}

	m_entity_types.push_back({ components_ref_first, m_component_refs.size() - components_ref_first });
	
	return &m_entity_types.back();
}

void* Context::get_component_instance(Entity entity, uint32_t component_id)
{
	assert(is_alive(entity));

	auto& entity_type = m_entity_types[entity.type];
	auto components_ref_range = Private::get_component_ref_range(*this, entity_type);
	
	auto it = std::lower_bound(components_ref_range.begin(), components_ref_range.end(), component_id, [](Component_ref const& ref, uint32_t component_id)
	{
		return ref.component_id < component_id;
	});

	if (it < components_ref_range.end() && it->component_id == component_id)
	{
		auto& component = m_components[it->component_index];
		auto& range = m_component_ranges[it->component_range_global_index];
		
		// Get the physical index of the entity component instance within the component range.
		const uint32_t physical_index = range.logical_to_physical[entity.index];

		return component.array + physical_index * component.instance_size;
	}

	return nullptr;
}

uint32_t Context::define_foreach(std::initializer_list<uint32_t> component_ids)
{
	// See if we have already defined this combination of components.
	const uint32_t num_components = component_ids.size();
	for (auto& foreach : m_foreaches)
	{
		if (num_components != foreach.component_id_count)
			continue;

		for (uint32_t i = 0; i < foreach.component_id_count; ++i)
			if (m_ids[foreach.component_id_first + i] != component_ids.begin()[i])
				goto next_foreach;

		return &foreach - m_foreaches.data();

	next_foreach:
		;
	}

	// No matching foreach found, create a new one.
	m_foreaches.push_back({ m_ids.size(), num_components, m_foreach_stmts.size(), 0 });

	// Insert covered component ids into the array of ids.
	m_ids.insert(m_ids.end(), component_ids.begin(), component_ids.end());

	// Scan the list of entity types and see which ones match this foreach component list.
	for (auto& entity_type : m_entity_types)
	{
		const uint32_t component_ref_index_first = m_ids.size();

		for (uint32_t i = 0; i < num_components; ++i)
		{
			for (uint32_t j = 0; j < entity_type.components_ref_count; ++j)
			{
				if (component_ids.begin()[i] == m_component_refs[entity_type.components_ref_first + j].component_id)
				{
					// Component found in entity type, carry one with the next component in foreach's
					// component list.
					m_ids.push_back(j);
					goto next_component;
				}
			}

			// Component not found in entity type, stop search.
			break;
		
		next_component:
			;
		}

		const uint32_t component_ref_index_count = m_ids.size() - component_ref_index_first;

		// If all components have been mached, create the foreach statements otherwise undo any
		// change made.
		if (num_components == component_ref_index_count)
			m_foreach_stmts.push_back({ (uint32_t)(&entity_type - m_entity_types.data()), component_ref_index_first, component_ref_index_count });
		else
			m_ids.resize(component_ref_index_first);
	}
	
	m_foreaches.back().foreach_stmt_count = m_foreach_stmts.size() - m_foreaches.back().foreach_stmt_first;

	return m_foreaches.size() - 1;
}

} // entity