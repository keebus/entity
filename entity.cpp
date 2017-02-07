
#include "entity.h"
#include <assert.h>
#include <algorithm>

namespace entity
{

Context::~Context()
{
	for (auto& component : m_components)
		free(component.array);
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
	}

	// #todo optimization: sort entity types by the number of components they have, and sort
	// components by frequency (maybe?).

	for (auto& entity_type : m_entity_types)
	{
		for (auto& component_ref : get_component_ref_range(entity_type))
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

Entity Context::create(Type_id type_id)
{
	auto& entity_type = m_entity_types[type_id];

	// The (component-local) index of the component about to be pushed.
	uint32_t component_instance_index = m_component_ranges[
		m_component_refs[entity_type.components_ref_first].component_range_global_index
	].size;

	// Generate a new local index for the new entity.
	uint32_t entity_index;
	if (component_instance_index < entity_type.component_to_entity.size())
	{
		// This component_to_entity position stores the index to a free entity index.
		entity_index = entity_type.component_to_entity[component_instance_index];
		entity_type.entity_to_component[entity_index] = component_instance_index;
	}
	else
	{
		// No reusable indices. Create a new entity index and map it to the component index.
		assert(component_instance_index == entity_type.component_to_entity.size());
		entity_index = component_instance_index;
		entity_type.entity_to_component.push_back(component_instance_index);
		entity_type.component_to_entity.push_back(entity_index);
		entity_type.generation.push_back(0);
	}

	// Push an instance to all components this entity has.
	for (auto& component_ref : get_component_ref_range(entity_type))
	{
		++ component_push_back(component_ref.component_index, component_ref.component_range_global_index);
	}

	return Entity{ (uint16_t)type_id, entity_type.generation[entity_index], entity_index };
}

void Context::destroy(Entity entity)
{
	//assert(is_alive(entity));

	auto& entity_type = m_entity_types[entity.type];

	// Map the entity index to the components instances index.
	uint32_t removed_component_instances_index = entity_type.entity_to_component[entity.index];

	// Move the last entity in the range in the position of the deleted component.
	for (auto& component_ref : get_component_ref_range(entity_type))
	{
		auto& component = m_components[component_ref.component_index];
		auto& range = m_component_ranges[component_ref.component_range_global_index];

		char* instance_ptr = component.array + (range.first + removed_component_instances_index) * component.instance_size;
		char* back_instance_ptr = component.array + (range.first + --range.size) * component.instance_size;

		memcpy(instance_ptr, back_instance_ptr, component.instance_size);
	}
	
	// Number of active entities is the number of component instances.
	uint32_t last_component_instance_index = m_component_ranges[
		m_component_refs[entity_type.components_ref_first].component_range_global_index
	].size;

	// Map the last components instances index (equal the instances count) to its entity index.
	uint32_t last_component_instance_entity_index = entity_type.component_to_entity[last_component_instance_index];

	// Map the last entity instance to map the the new components instances index (the one being removed).
	entity_type.entity_to_component[last_component_instance_entity_index] = removed_component_instances_index;

	// And viceversa.
	entity_type.component_to_entity[removed_component_instances_index] = last_component_instance_entity_index;

	// Use the now free component-to-entity slot to map to deleted entity index, effectively marking
	// it free for reuse.
	entity_type.component_to_entity[last_component_instance_index] = entity.index;

	// Increment generation count so that all entity ids like `entity` are now not alive.
	++entity_type.generation[entity.index];
}

void Context::clear()
{
	// Reset all entity lists.
	for (auto& entity_type : m_entity_types)
	{
		// Make all entity indices free.
		for (uint32_t i = 0; i < entity_type.generation.size(); ++i)
		{
			entity_type.component_to_entity[i] = i;
			++entity_type.generation[i];
		}
	}

	// Clear all ranges of components instances.
	for (auto& range : m_component_ranges)
	{
		range.size = 0;
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

Context::Entity_type* Context::find_create_entity_type(range<uintptr_t const*> component_ids)
{
	for (auto& entity_type : m_entity_types)
	{
		if (component_ids.size() != entity_type.components_ref_count)
			continue;

		auto components_ref_range = get_component_ref_range(entity_type);

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

uint32_t& Context::component_push_back(uint32_t component_index, uint32_t component_range_global_index)
{
	auto& component = m_components[component_index];
	auto& component_range = m_component_ranges[component_range_global_index];

	uint32_t back_index = component_range.first + component_range.size;
	char* back_ptr = component.array + back_index * component.instance_size;

	if (component_range_global_index + 1 < component.ranges_first + component.ranges_count)
	{
		// This range is followed by another range. See if the next range starts at least one
		// instance away from this range last, which means we can insert our new instance without
		// moving memory around.
		auto& next_component_range = m_component_ranges[component_range_global_index + 1];

		// If there's no space at the end of this range for one more instance...
		if (back_index >= next_component_range.first)
		{
			// Make room for one more at the end of next range.
			component_push_back(component_index, component_range_global_index + 1);
			
			// Move first element to the newly inserted element.
			memcpy(component.array + (next_component_range.first + next_component_range.size) * component.instance_size, back_ptr, component.instance_size);

			// Shift the next range up by one.
			++next_component_range.first;
		}
	}
	else 
	{
		// This is the last component range. We shall operate on the instances array itself.
		if (back_index >= component.array_capacity)
		{
			component.array_capacity *= 2;

			// Not enough space. Grow the instance array.
			component.array = (char*)realloc(component.array, component.array_capacity * component.instance_size);

			// Allocation changed, therefore update the back instance pointer.
			back_ptr = component.array + back_index * component.instance_size;
		}
	}

	// Clear the new component instance memory.
	memset(back_ptr, 0, component.instance_size);

	return component_range.size;
}

void* Context::get(Entity entity, uint32_t component_id)
{
	assert(is_alive(entity));

	auto& entity_type = m_entity_types[entity.type];
	auto components_ref_range = get_component_ref_range(entity_type);
	
	auto it = std::lower_bound(components_ref_range.begin(), components_ref_range.end(), component_id, [](Component_ref const& ref, uint32_t component_id)
	{
		return ref.component_id < component_id;
	});

	if (it < components_ref_range.end() && it->component_id == component_id)
	{
		auto& component = m_components[it->component_index];
		auto& range = m_component_ranges[it->component_range_global_index];
		return component.array + (range.first + entity_type.entity_to_component[entity.index]) * component.instance_size;
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
	m_foreaches.push_back({ m_ids.size(), num_components, m_foreach_entities.size(), 0 });

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
					m_ids.push_back(j);
					goto next_component;
				}
			}
			break;
		
		next_component:
			;
		}

		const uint32_t component_ref_index_count = m_ids.size() - component_ref_index_first;

		if (num_components == component_ref_index_count)
			m_foreach_entities.push_back({ (uint32_t)(&entity_type - m_entity_types.data()), component_ref_index_first, component_ref_index_count });
		else
			m_ids.resize(component_ref_index_first);
	}
	
	m_foreaches.back().foreach_entity_count = m_foreach_entities.size() - m_foreaches.back().foreach_entity_first;
	return m_foreaches.size() - 1;
}

} // entity