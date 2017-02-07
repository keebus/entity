
#include "entity.h"
#include "libs.h"
#include <assert.h>
#include <stdio.h>

struct Position
{
	int x;
	int y;
};

struct Velocity
{
	int x;
	int y;
};

int main()
{
	entity::Context context;

	entity::Type_id entity_position = context.define<Position>();
	entity::Type_id entity_position_velocity = context.define<Position, Velocity>();
	entity::Type_id entity_velocity = context.define<Velocity>();

	assert((context.define<Velocity>() == entity_velocity));
	assert((context.define<Position>() == entity_position));
	assert((context.define<Velocity, Position>() == entity_position_velocity));
	
	entity::Foreach<Position> foreach_position;
	context.define(foreach_position);

	entity::Foreach<Velocity> foreach_velocity;
	context.define(foreach_velocity);

	entity::Foreach<Velocity, Position> foreach_velocity_position;
	context.define(foreach_velocity_position);
	
	context.setup();

	for (int i = 0; i < 88; ++i)
	{
		auto p = context.create( entity_position);
		context.create(entity_velocity);
		*context.get<Position>(p) = { i, i * 10 + 2 };
	}

	foreach_position(context, [&](Position& p)
	{
		assert(p.y == p.x * 10 + 2);
	});
	
	std::vector<entity::Entity> es;
	for (int i = 0; i < 100; ++i)
	{
		context.clear();
		es.clear();
		for (int j = 0, n = rand() % 100 + 100; j < n; ++j)
		{
			switch (rand() % 3)
			{
				case 0: es.push_back(context.create(entity_position)); goto set_position;
				case 1: es.push_back(context.create(entity_position_velocity)); goto set_position;
				case 2: es.push_back(context.create(entity_velocity)); break;
				set_position:
				{
					int r = rand() % 1234;
					auto p = context.get<Position>(es.back());
					*p = { r, r * 10 + 2 };
					r = 0;
					break;
				}
				
			}
			assert(context.is_alive(es.back()));
		}

		foreach_position(context, [&](Position& p)
		{
			assert(p.y == p.x * 10 + 2);
		});

		for (int j = 0, n = es.size() / 3; j < n; ++j)
		{
			if (context.is_alive(es[j]))
				context.destroy(es[j]);
			assert(!context.is_alive(es[j]));
		}

		foreach_position(context, [&](Position& p)
		{
			p.x = rand() % 12345;
			p.y = p.x * 10 + 2;
		});

		foreach_velocity(context, [&](Velocity& v)
		{
			v.x =  rand() % 12345;
			v.y = v.x * 123;
		});

		foreach_velocity_position(context, [&](Velocity& v, Position& p)
		{
			assert(p.y == p.x * 10 + 2);
			assert(v.y == v.x * 123);

			printf("p (%d %d) v (%d %d)\n", p.x, p.y, v.x, v.y);
		});
	}
}
