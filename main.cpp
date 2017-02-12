
#include "entity.h"
#include "libs.h"
#include <assert.h>
#include <stdio.h>
#include <ctime>

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

	entity::Type entity_position = context.define<Position>();
	entity::Type entity_position_velocity = context.define<Position, Velocity>();
	entity::Type entity_velocity = context.define<Velocity>();

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
		context.get<Position>(p) = { i, i * 10 + 2 };
	}

	context.foreach(foreach_position, [&](Position& p)
	{
		assert(p.y == p.x * 10 + 2);
	});

	context.foreach_control(foreach_position, [&](auto& control, Position& p)
	{
		assert(p.y == p.x * 10 + 2);
		control.destroy_entity();
	});
	
	for (int i = 0; i < 88; ++i)
	{
		auto p = context.create(entity_position);
		context.get<Position>(p) = { i, i * 10 + 2 };
	}

	context.foreach(foreach_position, [&](Position& p)
	{
		assert(p.y == p.x * 10 + 2);
	});

	std::vector<entity::Entity> es;
	srand(time(0));
	for (int i = 0; i < 1000; ++i)
	{
		context.clear();
		es.clear();
		for (int j = 0, n = rand() % 1000 + 100; j < n; ++j)
		{
			switch (rand() % 3)
			{
				case 0: es.push_back(context.create(entity_position)); goto set_position;
				case 1:
					es.push_back(context.create(entity_position_velocity));
					context.get<Velocity>(es.back()) = { (int)es.size(), (int)es.size() * 2};
					goto set_position;

				case 2:
					es.push_back(context.create(entity_velocity));
					context.get<Velocity>(es.back()) = { (int)es.size(), (int)es.size() * 2};
					break;

				set_position:
				{
					int r = rand() % 1234;
					context.get<Position>(es.back()) = { r, r * 10 + 2 };
					break;
				}
				
			}
			assert(context.is_alive(es.back()));
		}

		context.foreach(foreach_position, [&](Position& p)
		{
			assert(p.y == p.x * 10 + 2);
		});

		for (int j = 0, n = es.size() / 3; j < n; ++j)
		{
			if (context.is_alive(es[j]))
				context.destroy(es[j]);
			assert(!context.is_alive(es[j]));
		}

		context.foreach(foreach_position, [&](Position& p)
		{
			p.x = rand() % 12345;
			p.y = p.x * 10 + 2;
		});

		context.foreach(foreach_velocity, [&](Velocity& v)
		{
			v.x =  rand() % 12345;
			v.y = v.x * 123;
		});

		context.foreach(foreach_velocity_position, [&](Velocity& v, Position& p)
		{
			assert(p.y == p.x * 10 + 2);
			assert(v.y == v.x * 123);

			printf("p (%d %d) v (%d %d)\n", p.x, p.y, v.x, v.y);
		});
	}
}
