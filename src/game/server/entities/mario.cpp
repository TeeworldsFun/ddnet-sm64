#include "mario.h"
#include "character.h"
extern "C" {
	#include <decomp/include/sm64shared.h>
	#include <decomp/include/surface_terrains.h>
}

#include <stdlib.h>
#include <string.h>

#include <engine/shared/config.h>

#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>

#include "../quickhull/QuickHull.hpp"
#include "../ConvexHull/ConvexHull.h"

CMario::CMario(CGameWorld *pGameWorld, vec2 Pos, int owner) : CEntity(pGameWorld, CGameWorld::ENTTYPE_MARIO, Pos)
{
	GameWorld()->InsertEntity(this);
	m_Owner = owner;
	m_Scale = g_Config.m_MarioScale/100.f;

	// on teeworlds, up coordinate is Y-, SM64 is Y+. flip the Y coordinate
	// scale conversions:
	//    teeworlds -> sm64: divide
	//    sm64 -> teeworlds: multiply
	int spawnX = Pos.x/m_Scale;
	int spawnY = -Pos.y/m_Scale;

	geometry.position = (float*)malloc( sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES );
	geometry.normal   = (float*)malloc( sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES );
	geometry.color    = (float*)malloc( sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES );
	geometry.uv       = (float*)malloc( sizeof(float) * 6 * SM64_GEO_MAX_TRIANGLES );
	geometry.numTrianglesUsed = 0;
	memset(&input, 0, sizeof(SM64MarioInputs));
	memset(&state, 0, sizeof(SM64MarioState));

	//exportMap(spawnX, spawnY);

	memset(m_currSurfaces, -1, sizeof(uint32_t) * MAX_SURFACES);
	loadNewBlocks(Pos.x/32, -Pos.y/32);

	marioId = sm64_mario_create(spawnX, spawnY, 0, 0,0,0,0);
	if (marioId == -1)
	{
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "libsm64", "Failed to spawn Mario");
		GameServer()->SendChatTarget(owner, "Failed to spawn Mario");
		m_MarkedForDestroy = true;
		return;
	}

	//GameServer()->m_apPlayers[m_Owner]->Pause(CPlayer::PAUSE_SPEC, true);
	//GameServer()->m_apPlayers[m_Owner]->m_SpectatorID = m_Owner;

	// disable jumps, ground and air speed prediction
	CCharacter *character = GameServer()->GetPlayerChar(m_Owner);
	if (!character) return;
	character->SetJumping(false);

	CTuningParams TuningParams;
	CTuningParams oldTuning = GameServer()->TuningCopy();

	TuningParams.Set("ground_control_speed", 0);
	TuningParams.Set("ground_jump_impulse", 0);
	TuningParams.Set("air_control_speed", 0);
	TuningParams.Set("air_jump_impulse", 0);
	TuningParams.Set("gravity", 0);

	GameServer()->SetTuning(TuningParams);
	GameServer()->SendTuningParams(m_Owner);
	GameServer()->SetTuning(oldTuning);
}

void CMario::Destroy()
{
	if (marioId != -1)
	{
		free(geometry.position);
		free(geometry.normal);
		free(geometry.color);
		free(geometry.uv);
		deleteBlocks();
		sm64_mario_delete(marioId);
		marioId = -1;
	}
	m_MarkedForDestroy = true;
	if (GameServer()->m_apPlayers[m_Owner])
	{
		GameServer()->SendTuningParams(m_Owner);
		//GameServer()->m_apPlayers[m_Owner]->Pause(CPlayer::PAUSE_NONE, true);

		CCharacter *character = GameServer()->GetPlayerChar(m_Owner);
		if (character) character->SetJumping(true);
	}
}

void CMario::Reset()
{
	Destroy();
}

void CMario::Tick()
{
	if (marioId == -1) return;

	CPlayer *player = GameServer()->m_apPlayers[m_Owner];
	CCharacter *character = GameServer()->GetPlayerChar(m_Owner);
	if (!player || !character)
	{
		Destroy();
		return;
	}

	player->m_SpectatorID = m_Owner;
	input.stickX = -character->GetLatestInput()->m_Direction;
	input.buttonA = character->GetLatestInput()->m_Jump;
	input.buttonB = character->GetLatestInput()->m_Fire & 1;
	input.buttonZ = character->GetLatestInput()->m_Hook;

	m_Tick += 1.f/Server()->TickSpeed();
	while (m_Tick >= 1.f/30)
	{
		m_Tick -= 1.f/30;

		sm64_reset_mario_z(marioId);
		sm64_mario_tick(marioId, &input, &state, &geometry);

		vec2 newPos(state.position[0]*m_Scale, -state.position[1]*m_Scale);
		if ((int)(newPos.x/32) != (int)(m_Pos.x/32) || (int)(newPos.y/32) != (int)(m_Pos.y/32))
			loadNewBlocks(newPos.x/32, newPos.y/32);

		m_Pos = newPos;

		player->m_ViewPos = vec2(m_Pos.x, m_Pos.y-48);
	}

	character->Core()->m_Pos = character->m_Pos = m_Pos;
	character->Core()->m_Vel = vec2(0,0);
	character->ResetHook();
}

void CMario::Snap(int SnappingClient)
{
	if (marioId == -1) return;
	if (!GameServer()->m_apPlayers[m_Owner] || !GameServer()->GetPlayerChar(m_Owner)) return;
	if (NetworkClipped(SnappingClient, m_Pos)) return;

	float drawScale = g_Config.m_MarioDrawScale / 100.f;

	std::vector<ivec2> verticesSnapped;
	for (int id : vertexIDs)
		Server()->SnapFreeID(id);
	vertexIDs.clear();

	// QuickHull
	std::vector<size_t> indexBuffer;
	quickhull::VertexDataSource<float> vertexBuffer;

	// ConvexHull
	std::vector<Coordinate> convexHull;

	size_t end = 3 * geometry.numTrianglesUsed;

	switch(g_Config.m_MarioDrawMode)
	{
		case 1:
			{
				quickhull::QuickHull<float> qh; // Could be double as well
				std::vector<quickhull::Vector3<float> > pointCloud;

				for (int i=0; i<3 * geometry.numTrianglesUsed; i++)
					pointCloud.push_back(quickhull::Vector3<float>(geometry.position[i*3+0], geometry.position[i*3+1], geometry.position[i*3+2]));

				quickhull::ConvexHull<float> hull = qh.getConvexHull(pointCloud, true, false);
				indexBuffer = hull.getIndexBuffer();
				vertexBuffer = hull.getVertexBuffer();
				end = (indexBuffer.empty()) ? 0 : indexBuffer.size()-1;
			}
			break;

		case 2:
			{
				std::vector<Coordinate> polygonPoints;

				for (int i=0; i<3 * geometry.numTrianglesUsed; i++)
					polygonPoints.push_back({geometry.position[i*3+0], geometry.position[i*3+1]});

				Polygon polygon(polygonPoints);
				convexHull = polygon.ComputeConvexHull();
				end = convexHull.size()-1;
			}
			break;
	}

	for (size_t i=0; i<end; i++)
	{
		ivec2 vertex, vertexTo;
		switch(g_Config.m_MarioDrawMode)
		{
			case 0:
				vertex = ivec2((int)geometry.position[i*3+0], -(int)geometry.position[i*3+1]);
				vertexTo = vertex;
				break;

			case 1:
				vertex = ivec2((int)vertexBuffer[indexBuffer[i]].x, -(int)vertexBuffer[indexBuffer[i]].y);
				vertexTo = ivec2((int)vertexBuffer[indexBuffer[i+1]].x, -(int)vertexBuffer[indexBuffer[i+1]].y);
				break;

			case 2:
				vertex = ivec2((int)convexHull[i].GetX(), -(int)convexHull[i].GetY());
				vertexTo = ivec2((int)convexHull[i+1].GetX(), -(int)convexHull[i+1].GetY());
				break;
		}

		vertex.x = ((vertex.x * m_Scale) - m_Pos.x) * drawScale + m_Pos.x;
		vertex.y = ((vertex.y * m_Scale) - m_Pos.y) * drawScale + m_Pos.y;
		vertexTo.x = ((vertexTo.x * m_Scale) - m_Pos.x) * drawScale + m_Pos.x;
		vertexTo.y = ((vertexTo.y * m_Scale) - m_Pos.y) * drawScale + m_Pos.y;

		vertex.y += 8;
		vertexTo.y += 8;

		bool repeated = false;

		for (size_t j=0; j<verticesSnapped.size(); j++)
		{
			if (verticesSnapped[j] == vertex)
			{
				repeated = true;
				break;
			}
		}

		if (repeated) continue;
		verticesSnapped.push_back(vertex);
		int id = Server()->SnapNewID();
		vertexIDs.push_back(id);

		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, id, sizeof(CNetObj_Laser)));
		if(!pObj)
			continue;

		pObj->m_FromX = vertex.x;
		pObj->m_FromY = vertex.y;
		pObj->m_X = vertex.x;
		pObj->m_Y = vertex.y;
		pObj->m_StartTick = Server()->Tick() + Server()->TickSpeed();
	}
}

void CMario::deleteBlocks()
{
	for (int j=0; j<MAX_SURFACES; j++)
	{
		if (m_currSurfaces[j] == (uint32_t)(-1)) continue;
		sm64_surface_object_delete(m_currSurfaces[j]);
		m_currSurfaces[j] = -1;
	}
}

bool CMario::addBlock(int x, int y, int *i)
{
	if ((*i) >= MAX_SURFACES) return false;
	bool block = GameServer()->Collision()->CheckPoint(x*32, y*32);
	if (!block) return false;

	struct SM64SurfaceObject obj;
	memset(&obj.transform, 0, sizeof(struct SM64ObjectTransform));
	obj.transform.position[0] = x*32 / m_Scale;
	obj.transform.position[1] = (-y*32-16) / m_Scale;
	obj.transform.position[2] = 0;
	obj.surfaceCount = 0;
	obj.surfaces = (struct SM64Surface*)malloc(sizeof(struct SM64Surface) * 4*2);

	bool up =		GameServer()->Collision()->CheckPoint(x*32, y*32-32);
	bool down =		GameServer()->Collision()->CheckPoint(x*32, y*32+32);
	bool left =		GameServer()->Collision()->CheckPoint(x*32-32, y*32);
	bool right =	GameServer()->Collision()->CheckPoint(x*32+32, y*32);

	// block ground face
	if (!up)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][2] = 64 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][2] = -64 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][2] = 64 / m_Scale;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = 0 / m_Scale; 		obj.surfaces[obj.surfaceCount+1].vertices[0][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[0][2] = -64 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][2] = 64 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][2] = -64 / m_Scale;

		obj.surfaceCount += 2;
	}

	// left (Z+)
	if (!left)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][2] = -64 / m_Scale;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = 0 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[1][2] = 64 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = 0 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[2][2] = -64 / m_Scale;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = 0 / m_Scale;

		obj.surfaces[obj.surfaceCount+1].vertices[0][2] = 64 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[0][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = 0 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[1][2] = -64 / m_Scale;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = 0 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[2][2] = 64 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = 0 / m_Scale;

		obj.surfaceCount += 2;
	}

	// right (Z-)
	if (!right)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][2] = 64 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = 32 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[1][2] = -64 / m_Scale;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = 32 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[2][2] = 64 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = 32 / m_Scale;

		obj.surfaces[obj.surfaceCount+1].vertices[0][2] = -64 / m_Scale;	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = 32 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[1][2] = 64 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = 32 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[2][2] = -64 / m_Scale;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = 32 / m_Scale;

		obj.surfaceCount += 2;
	}

	// block bottom face
	if (!down)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[0][2] = 64 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[1][2] = -64 / m_Scale;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+0].vertices[2][2] = 64 / m_Scale;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[0][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[0][2] = -64 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = 32 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[1][2] = 64 / m_Scale;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][1] = 0 / m_Scale;		obj.surfaces[obj.surfaceCount+1].vertices[2][2] = -64 / m_Scale;

		obj.surfaceCount += 2;
	}

	for (uint32_t ind=0; ind<obj.surfaceCount; ind++)
	{
		obj.surfaces[ind].type = SURFACE_DEFAULT;
		obj.surfaces[ind].force = 0;
		obj.surfaces[ind].terrain = TERRAIN_STONE;
	}

	if (obj.surfaceCount)
		m_currSurfaces[(*i)++] = sm64_surface_object_create(&obj);

	free(obj.surfaces);
	return true;
}

void CMario::loadNewBlocks(int x, int y)
{
	deleteBlocks();
	int yadd = 0;

	int arrayInd = 0;
	for (int xadd=-7; xadd<=7; xadd++)
	{
		// get block at floor
		for (yadd=0; y+yadd<=GameServer()->Collision()->GetHeight(); yadd++)
		{
			if (addBlock(x+xadd, y+yadd, &arrayInd)) break;
		}

		for (yadd=6; yadd>=0; yadd--)
		{
			addBlock(x+xadd, y-yadd, &arrayInd);
		}
	}
}

void CMario::exportMap(int spawnX, int spawnY)
{
	FILE *f = fopen("level.c", "w");

	fprintf(f, "#include \"level.h\"\n#include \"../src/decomp/include/surface_terrains.h\"\nconst struct SM64Surface surfaces[] = {\n");

	for (int y=0; y<GameServer()->Collision()->GetHeight(); y++)
	{
		for (int x=0; x<GameServer()->Collision()->GetWidth(); x++)
		{
			bool block = GameServer()->Collision()->CheckPoint(x*32, y*32);
			if (!block) continue;

			vec3 pos(x*32 / m_Scale, (-y*32-16) / m_Scale, 0);

			bool up =		GameServer()->Collision()->CheckPoint(x*32, y*32-32);
			bool down =		GameServer()->Collision()->CheckPoint(x*32, y*32+32);
			bool left =		GameServer()->Collision()->CheckPoint(x*32-32, y*32);
			bool right =	GameServer()->Collision()->CheckPoint(x*32+32, y*32);

			int vertices1[3][3], vertices2[3][3];

			// block ground face
			if (!up)
			{
				vertices1[0][0] = pos.x + (32 / m_Scale);	vertices1[0][1] = pos.y + (32 / m_Scale);	vertices1[0][2] = pos.z + (64 / m_Scale);
				vertices1[1][0] = pos.x + (0 / m_Scale);	vertices1[1][1] = pos.y + (32 / m_Scale);	vertices1[1][2] = pos.z + (-64 / m_Scale);
				vertices1[2][0] = pos.x + (0 / m_Scale);	vertices1[2][1] = pos.y + (32 / m_Scale);	vertices1[2][2] = pos.z + (64 / m_Scale);

				vertices2[0][0] = pos.x + (0 / m_Scale); 	vertices2[0][1] = pos.y + (32 / m_Scale);	vertices2[0][2] = pos.z + (-64 / m_Scale);
				vertices2[1][0] = pos.x + (32 / m_Scale);	vertices2[1][1] = pos.y + (32 / m_Scale);	vertices2[1][2] = pos.z + (64 / m_Scale);
				vertices2[2][0] = pos.x + (32 / m_Scale);	vertices2[2][1] = pos.y + (32 / m_Scale);	vertices2[2][2] = pos.z + (-64 / m_Scale);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices1[0][0], vertices1[0][1], vertices1[0][2],
					vertices1[1][0], vertices1[1][1], vertices1[1][2],
					vertices1[2][0], vertices1[2][1], vertices1[2][2]);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices2[0][0], vertices2[0][1], vertices2[0][2],
					vertices2[1][0], vertices2[1][1], vertices2[1][2],
					vertices2[2][0], vertices2[2][1], vertices2[2][2]);
			}

			// left (X-)
			if (!left)
			{
				vertices1[0][2] = pos.z + (-64 / m_Scale);	vertices1[0][1] = pos.y + (0 / m_Scale);	vertices1[0][0] = pos.x + (0 / m_Scale);
				vertices1[1][2] = pos.z + (64 / m_Scale);	vertices1[1][1] = pos.y + (32 / m_Scale);	vertices1[1][0] = pos.x + (0 / m_Scale);
				vertices1[2][2] = pos.z + (-64 / m_Scale);	vertices1[2][1] = pos.y + (32 / m_Scale);	vertices1[2][0] = pos.x + (0 / m_Scale);

				vertices2[0][2] = pos.z + (64 / m_Scale);	vertices2[0][1] = pos.y + (32 / m_Scale);	vertices2[0][0] = pos.x + (0 / m_Scale);
				vertices2[1][2] = pos.z + (-64 / m_Scale);	vertices2[1][1] = pos.y + (0 / m_Scale);	vertices2[1][0] = pos.x + (0 / m_Scale);
				vertices2[2][2] = pos.z + (64 / m_Scale);	vertices2[2][1] = pos.y + (0 / m_Scale);	vertices2[2][0] = pos.x + (0 / m_Scale);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices1[0][0], vertices1[0][1], vertices1[0][2],
					vertices1[1][0], vertices1[1][1], vertices1[1][2],
					vertices1[2][0], vertices1[2][1], vertices1[2][2]);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices2[0][0], vertices2[0][1], vertices2[0][2],
					vertices2[1][0], vertices2[1][1], vertices2[1][2],
					vertices2[2][0], vertices2[2][1], vertices2[2][2]);
			}

			// right (X+)
			if (!right)
			{
				vertices1[0][2] = pos.z + (64 / m_Scale);	vertices1[0][1] = pos.y + (0 / m_Scale);	vertices1[0][0] = pos.x + (32 / m_Scale);
				vertices1[1][2] = pos.z + (-64 / m_Scale);	vertices1[1][1] = pos.y + (32 / m_Scale);	vertices1[1][0] = pos.x + (32 / m_Scale);
				vertices1[2][2] = pos.z + (64 / m_Scale);	vertices1[2][1] = pos.y + (32 / m_Scale);	vertices1[2][0] = pos.x + (32 / m_Scale);

				vertices2[0][2] = pos.z + (-64 / m_Scale);	vertices2[0][1] = pos.y + (32 / m_Scale);	vertices2[0][0] = pos.x + (32 / m_Scale);
				vertices2[1][2] = pos.z + (64 / m_Scale);	vertices2[1][1] = pos.y + (0 / m_Scale);	vertices2[1][0] = pos.x + (32 / m_Scale);
				vertices2[2][2] = pos.z + (-64 / m_Scale);	vertices2[2][1] = pos.y + (0 / m_Scale);	vertices2[2][0] = pos.x + (32 / m_Scale);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices1[0][0], vertices1[0][1], vertices1[0][2],
					vertices1[1][0], vertices1[1][1], vertices1[1][2],
					vertices1[2][0], vertices1[2][1], vertices1[2][2]);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices2[0][0], vertices2[0][1], vertices2[0][2],
					vertices2[1][0], vertices2[1][1], vertices2[1][2],
					vertices2[2][0], vertices2[2][1], vertices2[2][2]);
			}

			// front
			{
				vertices1[0][0] = pos.x + (32 / m_Scale);	vertices1[0][1] = pos.y + (0 / m_Scale);	vertices1[0][2] = pos.z + (-64 / m_Scale);
				vertices1[1][0] = pos.x + (0 / m_Scale);	vertices1[1][1] = pos.y + (32 / m_Scale);	vertices1[1][2] = pos.z + (-64 / m_Scale);
				vertices1[2][0] = pos.x + (32 / m_Scale);	vertices1[2][1] = pos.y + (32 / m_Scale);	vertices1[2][2] = pos.z + (-64 / m_Scale);

				vertices2[0][0] = pos.x + (0 / m_Scale);	vertices2[0][1] = pos.y + (32 / m_Scale);	vertices2[0][2] = pos.z + (-64 / m_Scale);
				vertices2[1][0] = pos.x + (32 / m_Scale);	vertices2[1][1] = pos.y + (0 / m_Scale);	vertices2[1][2] = pos.z + (-64 / m_Scale);
				vertices2[2][0] = pos.x + (0 / m_Scale);	vertices2[2][1] = pos.y + (0 / m_Scale);	vertices2[2][2] = pos.z + (-64 / m_Scale);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices1[0][0], vertices1[0][1], vertices1[0][2],
					vertices1[1][0], vertices1[1][1], vertices1[1][2],
					vertices1[2][0], vertices1[2][1], vertices1[2][2]);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices2[0][0], vertices2[0][1], vertices2[0][2],
					vertices2[1][0], vertices2[1][1], vertices2[1][2],
					vertices2[2][0], vertices2[2][1], vertices2[2][2]);
			}

			// back
			{
				vertices1[0][0] = pos.x + (0 / m_Scale);	vertices1[0][1] = pos.y + (0 / m_Scale);	vertices1[0][2] = pos.z + (64 / m_Scale);
				vertices1[1][0] = pos.x + (32 / m_Scale);	vertices1[1][1] = pos.y + (32 / m_Scale);	vertices1[1][2] = pos.z + (64 / m_Scale);
				vertices1[2][0] = pos.x + (0 / m_Scale);	vertices1[2][1] = pos.y + (32 / m_Scale);	vertices1[2][2] = pos.z + (64 / m_Scale);

				vertices2[0][0] = pos.x + (32 / m_Scale);	vertices2[0][1] = pos.y + (32 / m_Scale);	vertices2[0][2] = pos.z + (64 / m_Scale);
				vertices2[1][0] = pos.x + (0 / m_Scale);	vertices2[1][1] = pos.y + (0 / m_Scale);	vertices2[1][2] = pos.z + (64 / m_Scale);
				vertices2[2][0] = pos.x + (32 / m_Scale);	vertices2[2][1] = pos.y + (0 / m_Scale);	vertices2[2][2] = pos.z + (64 / m_Scale);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices1[0][0], vertices1[0][1], vertices1[0][2],
					vertices1[1][0], vertices1[1][1], vertices1[1][2],
					vertices1[2][0], vertices1[2][1], vertices1[2][2]);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices2[0][0], vertices2[0][1], vertices2[0][2],
					vertices2[1][0], vertices2[1][1], vertices2[1][2],
					vertices2[2][0], vertices2[2][1], vertices2[2][2]);
			}

			// block bottom face
			if (!down)
			{
				vertices1[0][0] = pos.x + (0 / m_Scale);	vertices1[0][1] = pos.y + (0 / m_Scale);	vertices1[0][2] = pos.z + (64 / m_Scale);
				vertices1[1][0] = pos.x + (0 / m_Scale);	vertices1[1][1] = pos.y + (0 / m_Scale);	vertices1[1][2] = pos.z + (-64 / m_Scale);
				vertices1[2][0] = pos.x + (32 / m_Scale);	vertices1[2][1] = pos.y + (0 / m_Scale);	vertices1[2][2] = pos.z + (64 / m_Scale);

				vertices2[0][0] = pos.x + (32 / m_Scale);	vertices2[0][1] = pos.y + (0 / m_Scale);	vertices2[0][2] = pos.z + (-64 / m_Scale);
				vertices2[1][0] = pos.x + (32 / m_Scale);	vertices2[1][1] = pos.y + (0 / m_Scale);	vertices2[1][2] = pos.z + (64 / m_Scale);
				vertices2[2][0] = pos.x + (0 / m_Scale);	vertices2[2][1] = pos.y + (0 / m_Scale);	vertices2[2][2] = pos.z + (-64 / m_Scale);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices1[0][0], vertices1[0][1], vertices1[0][2],
					vertices1[1][0], vertices1[1][1], vertices1[1][2],
					vertices1[2][0], vertices1[2][1], vertices1[2][2]);

				fprintf(f, "{SURFACE_DEFAULT,0,TERRAIN_STONE,{{%d,%d,%d},{%d,%d,%d},{%d,%d,%d}}},\n",
					vertices2[0][0], vertices2[0][1], vertices2[0][2],
					vertices2[1][0], vertices2[1][1], vertices2[1][2],
					vertices2[2][0], vertices2[2][1], vertices2[2][2]);
			}
		}
	}

	fprintf(f, "};\nconst size_t surfaces_count = sizeof( surfaces ) / sizeof( surfaces[0] );\nconst int32_t spawn[3] = {%d, %d, 0};\n", spawnX, spawnY);
	fclose(f);
}
