//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Places "detail" objects which are client-only renderable things
//
// $Revision: $
// $NoKeywords: $
//=============================================================================//

#include "vbsp.h"
#include "bsplib.h"
#include "utlvector.h"
#include "bspfile.h"
#include "gamebspfile.h"
#include "VPhysics_Interface.h"
#include "Studio.h"
#include "byteswap.h"
#include "utlhashdict.h"
#include "UtlBuffer.h"
#include "CollisionUtils.h"
#include <float.h>
#include "CModel.h"
#include "PhysDll.h"
#include "utlsymbol.h"
#include "tier1/strtools.h"
#include "KeyValues.h"
#include "scriplib.h"
#include "tier2/fileutils.h"
#include "tier1/tier1.h"
#include "vstdlib/iprocessutils.h"

#include "../motionmapper/motionmapper.h" // For SMD support

static void SetCurrentModel( studiohdr_t *pStudioHdr );
static void FreeCurrentModelVertexes();

IPhysicsCollision *s_pPhysCollision = NULL;

//-----------------------------------------------------------------------------
// These puppies are used to construct the game lumps
//-----------------------------------------------------------------------------
static CUtlVector<StaticPropDictLump_t>	s_StaticPropDictLump;
static CUtlVector<StaticPropLump_t>		s_StaticPropLump;
static CUtlVector<StaticPropLeafLump_t>	s_StaticPropLeafLump;


//-----------------------------------------------------------------------------
// Used to build the static prop
//-----------------------------------------------------------------------------
struct StaticPropBuild_t
{
	char const* m_pModelName;
	char const* m_pLightingOrigin;
	Vector	m_Origin;
	QAngle	m_Angles;
	int		m_Solid;
	int		m_Skin;
	int		m_Flags;
	float	m_FadeMinDist;
	float	m_FadeMaxDist;
	bool	m_FadesOut;
	float	m_flForcedFadeScale;
	unsigned short	m_nMinDXLevel;
	unsigned short	m_nMaxDXLevel;
	int		m_LightmapResolutionX;
	int		m_LightmapResolutionY;
};
 

//-----------------------------------------------------------------------------
// Used to cache collision model generation
//-----------------------------------------------------------------------------
struct ModelCollisionLookup_t
{
	CUtlSymbol m_Name;
	CPhysCollide* m_pCollide;
};

class QCFile_t
{
public:
	char const* m_pPath;
	char const* m_pRefSMD;
	char const* m_pPhySMD;
	float m_flRefScale;
	float m_flPhyScale;

	QCFile_t();
	QCFile_t(const char *pFileLocation, const char *pFilePath, bool *pRetVal = NULL);
	~QCFile_t();
};

QCFile_t::QCFile_t()
{
	m_pPath = m_pRefSMD = m_pPhySMD = NULL;
	m_flRefScale = m_flPhyScale = 1.0f;
}

QCFile_t::QCFile_t(const char *pFileLocation, const char *pFilePath, bool *pRetVal)
{
	m_pPath = m_pRefSMD = m_pPhySMD = NULL;
	m_flRefScale = m_flPhyScale = 1.0f;

	if (pRetVal)
		*pRetVal = false;

	FileHandle_t f = g_pFullFileSystem->Open(pFilePath, "rb");
	if (!f)
	{
		Warning("Invalid QC filepath supplied: %s - ignoring...\n", pFilePath);
		
		return;
	}

	// load file into a null-terminated buffer
	int fileSize = g_pFullFileSystem->Size(f);
	unsigned bufSize = g_pFullFileSystem->GetOptimalReadSize(f, fileSize + 2);

	char *buffer = (char*)g_pFullFileSystem->AllocOptimalReadBuffer(f, bufSize);
	Assert(buffer);

	// read into local buffer
	bool bRetOK = (g_pFullFileSystem->ReadEx(buffer, bufSize, fileSize, f) != 0);

	g_pFullFileSystem->Close(f);	// close file after reading	

	if (!bRetOK)
	{
		Warning("Error reading: %s - ignoring...\n", pFilePath);
		return;
	}

	float scaleFactor = 1.f;
	char modelName[MAXTOKEN];
	modelName[0] = '\0';

	ParseFromMemory(buffer, bufSize); // This should be replaced with a dedicated parser, as this causes includes to error out the program
	while (GetToken(true))
	{
		if (!V_strcmp(token, "{")) // Skip over irrelevant "compound" sections
		{
			while (GetToken(true))
			{
				if (!V_strcmp(token, "}"))
					break;
			}
		}

		if (!V_stricmp(token, "$scale"))
		{
			if (!GetToken(true))
				goto invalidQC;
			
			sscanf_s(token, "%f", &scaleFactor);
			continue;
		}

		if (!V_stricmp(token, "$modelname"))
		{
			if (!GetToken(true))
				goto invalidQC;

			sscanf_s(token, "%s", &modelName);
			continue;
		}

		if (!V_stricmp(token, "$bodygroup") || !V_stricmp(token, "$body") || !V_stricmp(token, "$model"))
		{
			if (!GetToken(true))
				goto invalidQC;

			if (V_strcmp(token, "{")) // String
			{
				if (m_pRefSMD || !GetToken(true))
					goto invalidQC;

				char *pRefSMD = new char[MAX_PATH];
				V_snprintf(pRefSMD, MAX_PATH, "%s/%s", pFileLocation, token);

				m_pRefSMD = pRefSMD;
				m_flRefScale = scaleFactor;
				continue;
			}

			// Brace open
			while (GetToken(true) && !V_strcmp(token, "}"))
			{
				if (!V_stricmp(token, "studio"))
				{
					if (m_pRefSMD)
						goto invalidQC;

					if (!GetToken(true))
						goto invalidQC;

					char *pRefSMD = new char[MAX_PATH];
					V_snprintf(pRefSMD, MAX_PATH, "%s/%s", pFileLocation, token);

					m_pRefSMD = pRefSMD;
					m_flRefScale = scaleFactor;
				}
			}

			
			continue;
		}

		if (!V_stricmp(token, "$collisionmodel"))
		{
			if (!GetToken(true))
				goto invalidQC;

			char *pPhySMD = new char[MAX_PATH];
			V_snprintf(pPhySMD, MAX_PATH, "%s/%s", pFileLocation, token);

			m_pPhySMD = pPhySMD;
			m_flPhyScale = scaleFactor;
			continue;
		}

		if (
			!V_stricmp(token, "$collisionjoints") ||
			!V_stricmp(token, "$ikchain") ||
			!V_stricmp(token, "$weightlist") ||
			!V_stricmp(token, "$poseparameter") ||
			!V_stricmp(token, "$proceduralbones") ||
			!V_stricmp(token, "$jigglebone")
			)
		{
			goto invalidQC;
		}
	}

	if (!modelName[0] || !m_pRefSMD)
		goto invalidQC;

	char *pPath = new char[MAX_PATH];
	if (V_strncmp(modelName, "models/", 7)) 
	{
		V_strncpy(pPath, "models/", 8);
		V_strncpy(pPath + 7, modelName, MAX_PATH - 7);
	}
	else
	{
		V_strncpy(pPath, modelName, MAX_PATH);
	}

	V_FixSlashes(pPath, '/');
	V_FixDoubleSlashes(pPath);
	
	m_pPath = pPath;

	g_pFullFileSystem->FreeOptimalReadBuffer(buffer);

	if (pRetVal)
		*pRetVal = true;

	return;

invalidQC:
	Warning("Invalid static prop QC: %s\n", pFilePath);

	// Reset the QC and get out
	if (m_pPath)
		delete m_pPath;
	if (m_pRefSMD)
		delete m_pRefSMD;
	if (m_pPhySMD)
		delete m_pPhySMD;

	m_pPath = m_pRefSMD = m_pPhySMD = NULL;
	m_flRefScale = m_flPhyScale = 1.0f;

	g_pFullFileSystem->FreeOptimalReadBuffer(buffer);

	return;
}

QCFile_t::~QCFile_t()
{
	if (m_pPath)
		delete m_pPath;
	if (m_pRefSMD)
		delete m_pRefSMD;
	if (m_pPhySMD)
		delete m_pPhySMD;
}

static bool ModelLess( ModelCollisionLookup_t const& src1, ModelCollisionLookup_t const& src2 )
{
	return src1.m_Name < src2.m_Name;
}

static CUtlRBTree<ModelCollisionLookup_t, unsigned short>	s_ModelCollisionCache( 0, 32, ModelLess );
static CUtlVector<int>	s_LightingInfo;


//-----------------------------------------------------------------------------
// Gets the keyvalues from a studiohdr
//-----------------------------------------------------------------------------
bool StudioKeyValues( studiohdr_t* pStudioHdr, KeyValues *pValue )
{
	if ( !pStudioHdr )
		return false;

	return pValue->LoadFromBuffer( pStudioHdr->pszName(), pStudioHdr->KeyValueText() );
}


//-----------------------------------------------------------------------------
// Makes sure the studio model is a static prop
//-----------------------------------------------------------------------------
enum isstaticprop_ret
{
	RET_VALID,
	RET_FAIL_NOT_MARKED_STATIC_PROP,
	RET_FAIL_DYNAMIC,
};

isstaticprop_ret IsStaticProp( studiohdr_t* pHdr )
{
	if (!(pHdr->flags & STUDIOHDR_FLAGS_STATIC_PROP))
		return RET_FAIL_NOT_MARKED_STATIC_PROP;

	// If it's got a propdata section in the model's keyvalues, it's not allowed to be a prop_static
	KeyValues *modelKeyValues = new KeyValues(pHdr->pszName());
	if ( StudioKeyValues( pHdr, modelKeyValues ) )
	{
		KeyValues *sub = modelKeyValues->FindKey("prop_data");
		if ( sub )
		{
			if ( !(sub->GetInt( "allowstatic", 0 )) )
			{
				modelKeyValues->deleteThis();
				return RET_FAIL_DYNAMIC;
			}
		}
	}
	modelKeyValues->deleteThis();

	return RET_VALID;
}


//-----------------------------------------------------------------------------
// Add static prop model to the list of models
//-----------------------------------------------------------------------------

static int AddStaticPropDictLump( char const* pModelName )
{
	StaticPropDictLump_t dictLump;
	strncpy( dictLump.m_Name, pModelName, DETAIL_NAME_LENGTH );

	for (int i = s_StaticPropDictLump.Size(); --i >= 0; )
	{
		if (!memcmp(&s_StaticPropDictLump[i], &dictLump, sizeof(dictLump) ))
			return i;
	}

	return s_StaticPropDictLump.AddToTail( dictLump );
}


//-----------------------------------------------------------------------------
// Load studio model vertex data from a file...
//-----------------------------------------------------------------------------
bool LoadStudioModel( char const* pModelName, char const* pEntityType, CUtlBuffer& buf )
{
	if ( !g_pFullFileSystem->ReadFile( pModelName, NULL, buf ) )
		return false;

	// Check that it's valid
	if (strncmp ((const char *) buf.PeekGet(), "IDST", 4) &&
		strncmp ((const char *) buf.PeekGet(), "IDAG", 4))
	{
		return false;
	}

	studiohdr_t* pHdr = (studiohdr_t*)buf.PeekGet();

	Studio_ConvertStudioHdrToNewVersion( pHdr );

	if (pHdr->version != STUDIO_VERSION)
	{
		return false;
	}

	isstaticprop_ret isStaticProp = IsStaticProp(pHdr);
	if ( isStaticProp != RET_VALID )
	{
		if ( isStaticProp == RET_FAIL_NOT_MARKED_STATIC_PROP )
		{
			Warning("Error! To use model \"%s\"\n"
				"      with %s, it must be compiled with $staticprop!\n", pModelName, pEntityType );
		}
		else if ( isStaticProp == RET_FAIL_DYNAMIC )
		{
			Warning("Error! %s using model \"%s\", which must be used on a dynamic entity (i.e. prop_physics). Deleted.\n", pEntityType, pModelName );
		}
		return false;
	}

	// ensure reset
	pHdr->pVertexBase = NULL;
	pHdr->pIndexBase  = NULL;

	return true;
}


//-----------------------------------------------------------------------------
// Computes a convex hull from a studio mesh
//-----------------------------------------------------------------------------
static CPhysConvex* ComputeConvexHull( mstudiomesh_t* pMesh )
{
	// Generate a list of all verts in the mesh
	Vector** ppVerts = (Vector**)stackalloc(pMesh->numvertices * sizeof(Vector*) );
	const mstudio_meshvertexdata_t *vertData = pMesh->GetVertexData();
	Assert( vertData ); // This can only return NULL on X360 for now
	for (int i = 0; i < pMesh->numvertices; ++i)
	{
		ppVerts[i] = vertData->Position(i);
	}

	// Generate a convex hull from the verts
	return s_pPhysCollision->ConvexFromVerts( ppVerts, pMesh->numvertices );
}


//-----------------------------------------------------------------------------
// Computes a convex hull from the studio model
//-----------------------------------------------------------------------------
CPhysCollide* ComputeConvexHull( studiohdr_t* pStudioHdr )
{
	CUtlVector<CPhysConvex*>	convexHulls;

	for (int body = 0; body < pStudioHdr->numbodyparts; ++body )
	{
		mstudiobodyparts_t *pBodyPart = pStudioHdr->pBodypart( body );
		for( int model = 0; model < pBodyPart->nummodels; ++model )
		{
			mstudiomodel_t *pStudioModel = pBodyPart->pModel( model );
			for( int mesh = 0; mesh < pStudioModel->nummeshes; ++mesh )
			{
				// Make a convex hull for each mesh
				// NOTE: This won't work unless the model has been compiled
				// with $staticprop
				mstudiomesh_t *pStudioMesh = pStudioModel->pMesh( mesh );
				convexHulls.AddToTail( ComputeConvexHull( pStudioMesh ) );
			}
		}
	}

	// Convert an array of convex elements to a compiled collision model
	// (this deletes the convex elements)
	return s_pPhysCollision->ConvertConvexToCollide( convexHulls.Base(), convexHulls.Size() );
}


//-----------------------------------------------------------------------------
// Add, find collision model in cache
//-----------------------------------------------------------------------------
static CPhysCollide* GetCollisionModel( char const* pModelName )
{
	// Convert to a common string
	char* pTemp = (char*)_alloca(strlen(pModelName) + 1);
	strcpy( pTemp, pModelName );
	_strlwr( pTemp );

	char* pSlash = strchr( pTemp, '\\' );
	while( pSlash )
	{
		*pSlash = '/';
		pSlash = strchr( pTemp, '\\' );
	}

	// Find it in the cache
	ModelCollisionLookup_t lookup;
	lookup.m_Name = pTemp;
	int i = s_ModelCollisionCache.Find( lookup );
	if (i != s_ModelCollisionCache.InvalidIndex())
		return s_ModelCollisionCache[i].m_pCollide;

	// Load the studio model file
	CUtlBuffer buf;
	if (!LoadStudioModel(pModelName, "prop_static", buf))
	{
		Warning("Error loading studio model \"%s\"!\n", pModelName );

		// This way we don't try to load it multiple times
		lookup.m_pCollide = 0;
		s_ModelCollisionCache.Insert( lookup );

		return 0;
	}

	// Compute the convex hull of the model...
	studiohdr_t* pStudioHdr = (studiohdr_t*)buf.PeekGet();

	// necessary for vertex access
	SetCurrentModel( pStudioHdr );

	lookup.m_pCollide = ComputeConvexHull( pStudioHdr );
	s_ModelCollisionCache.Insert( lookup );

	if ( !lookup.m_pCollide )
	{
		Warning("Bad geometry on \"%s\"!\n", pModelName );
	}

	// Debugging
	if (g_DumpStaticProps)
	{
		static int propNum = 0;
		char tmp[128];
		sprintf( tmp, "staticprop%03d.txt", propNum );
		DumpCollideToGlView( lookup.m_pCollide, tmp );
		++propNum;
	}

	FreeCurrentModelVertexes();

	// Insert into cache...
	return lookup.m_pCollide;
}


//-----------------------------------------------------------------------------
// Tests a single leaf against the static prop
//-----------------------------------------------------------------------------

static bool TestLeafAgainstCollide( int depth, int* pNodeList, 
	Vector const& origin, QAngle const& angles, CPhysCollide* pCollide )
{
	// Copy the planes in the node list into a list of planes
	float* pPlanes = (float*)_alloca(depth * 4 * sizeof(float) );
	int idx = 0;
	for (int i = depth; --i >= 0; ++idx )
	{
		int sign = (pNodeList[i] < 0) ? -1 : 1;
		int node = (sign < 0) ? - pNodeList[i] - 1 : pNodeList[i];
		dnode_t* pNode = &dnodes[node];
		dplane_t* pPlane = &dplanes[pNode->planenum];

		pPlanes[idx*4] = sign * pPlane->normal[0];
		pPlanes[idx*4+1] = sign * pPlane->normal[1];
		pPlanes[idx*4+2] = sign * pPlane->normal[2];
		pPlanes[idx*4+3] = sign * pPlane->dist;
	}

	// Make a convex solid out of the planes
	CPhysConvex* pPhysConvex = s_pPhysCollision->ConvexFromPlanes( pPlanes, depth, 0.0f );

	// This should never happen, but if it does, return no collision
	Assert( pPhysConvex );
	if (!pPhysConvex)
		return false;

	CPhysCollide* pLeafCollide = s_pPhysCollision->ConvertConvexToCollide( &pPhysConvex, 1 );

	// Collide the leaf solid with the static prop solid
	trace_t	tr;
	s_pPhysCollision->TraceCollide( vec3_origin, vec3_origin, pLeafCollide, vec3_angle,
		pCollide, origin, angles, &tr );

	s_pPhysCollision->DestroyCollide( pLeafCollide );

	return (tr.startsolid != 0);
}

//-----------------------------------------------------------------------------
// Find all leaves that intersect with this bbox + test against the static prop..
//-----------------------------------------------------------------------------

static void ComputeConvexHullLeaves_R( int node, int depth, int* pNodeList,
	Vector const& mins, Vector const& maxs,
	Vector const& origin, QAngle const& angles,	CPhysCollide* pCollide,
	CUtlVector<unsigned short>& leafList )
{
	Assert( pNodeList && pCollide );
	Vector cornermin, cornermax;

	while( node >= 0 )
	{
		dnode_t* pNode = &dnodes[node];
		dplane_t* pPlane = &dplanes[pNode->planenum];

		// Arbitrary split plane here
		for (int i = 0; i < 3; ++i)
		{
			if (pPlane->normal[i] >= 0)
			{
				cornermin[i] = mins[i];
				cornermax[i] = maxs[i];
			}
			else
			{
				cornermin[i] = maxs[i];
				cornermax[i] = mins[i];
			}
		}

		if (DotProduct( pPlane->normal, cornermax ) <= pPlane->dist)
		{
			// Add the node to the list of nodes
			pNodeList[depth] = node;
			++depth;

			node = pNode->children[1];
		}
		else if (DotProduct( pPlane->normal, cornermin ) >= pPlane->dist)
		{
			// In this case, we are going in front of the plane. That means that
			// this plane must have an outward normal facing in the oppisite direction
			// We indicate this be storing a negative node index in the node list
			pNodeList[depth] = - node - 1;
			++depth;

			node = pNode->children[0];
		}
		else
		{
			// Here the box is split by the node. First, we'll add the plane as if its
			// outward facing normal is in the direction of the node plane, then
			// we'll have to reverse it for the other child...
			pNodeList[depth] = node;
			++depth;

			ComputeConvexHullLeaves_R( pNode->children[1], 
				depth, pNodeList, mins, maxs, origin, angles, pCollide, leafList );
			
			pNodeList[depth - 1] = - node - 1;
			ComputeConvexHullLeaves_R( pNode->children[0],
				depth, pNodeList, mins, maxs, origin, angles, pCollide, leafList );
			return;
		}
	}

	Assert( pNodeList && pCollide );

	// Never add static props to solid leaves
	if ( (dleafs[-node-1].contents & CONTENTS_SOLID) == 0 )
	{
		if (TestLeafAgainstCollide( depth, pNodeList, origin, angles, pCollide ))
		{
			leafList.AddToTail( -node - 1 );
		}
	}
}

//-----------------------------------------------------------------------------
// Places Static Props in the level
//-----------------------------------------------------------------------------

static void ComputeStaticPropLeaves( CPhysCollide* pCollide, Vector const& origin, 
				QAngle const& angles, CUtlVector<unsigned short>& leafList )
{
	// Compute an axis-aligned bounding box for the collide
	Vector mins, maxs;
	s_pPhysCollision->CollideGetAABB( &mins, &maxs, pCollide, origin, angles );

	// Find all leaves that intersect with the bounds
	int tempNodeList[1024];
	ComputeConvexHullLeaves_R( 0, 0, tempNodeList, mins, maxs,
		origin, angles, pCollide, leafList );
}


//-----------------------------------------------------------------------------
// Computes the lighting origin
//-----------------------------------------------------------------------------
static bool ComputeLightingOrigin( StaticPropBuild_t const& build, Vector& lightingOrigin )
{
	for (int i = s_LightingInfo.Count(); --i >= 0; )
	{
		int entIndex = s_LightingInfo[i];

		// Check against all lighting info entities
		char const* pTargetName = ValueForKey( &entities[entIndex], "targetname" );
		if (!Q_strcmp(pTargetName, build.m_pLightingOrigin))
		{
			GetVectorForKey( &entities[entIndex], "origin", lightingOrigin );
			return true;
		}
	}

	return false;
}


//-----------------------------------------------------------------------------
// Places Static Props in the level
//-----------------------------------------------------------------------------
static void AddStaticPropToLump( StaticPropBuild_t const& build )
{
	// Get the collision model
	CPhysCollide* pConvexHull = GetCollisionModel( build.m_pModelName );
	if (!pConvexHull)
		return;

	// Compute the leaves the static prop's convex hull hits
	CUtlVector< unsigned short > leafList;
	ComputeStaticPropLeaves( pConvexHull, build.m_Origin, build.m_Angles, leafList );

	if ( !leafList.Count() )
	{
		Warning( "Static prop %s outside the map (%.2f, %.2f, %.2f)\n", build.m_pModelName, build.m_Origin.x, build.m_Origin.y, build.m_Origin.z );
		return;
	}
	// Insert an element into the lump data...
	int i = s_StaticPropLump.AddToTail( );
	StaticPropLump_t& propLump = s_StaticPropLump[i];
	propLump.m_PropType = AddStaticPropDictLump( build.m_pModelName ); 
	VectorCopy( build.m_Origin, propLump.m_Origin );
	VectorCopy( build.m_Angles, propLump.m_Angles );
	propLump.m_FirstLeaf = s_StaticPropLeafLump.Count();
	propLump.m_LeafCount = leafList.Count();
	propLump.m_Solid = build.m_Solid;
	propLump.m_Skin = build.m_Skin;
	propLump.m_Flags = build.m_Flags;
	if (build.m_FadesOut)
	{
		propLump.m_Flags |= STATIC_PROP_FLAG_FADES;
	}
	propLump.m_FadeMinDist = build.m_FadeMinDist;
	propLump.m_FadeMaxDist = build.m_FadeMaxDist;
	propLump.m_flForcedFadeScale = build.m_flForcedFadeScale;
	propLump.m_nMinDXLevel = build.m_nMinDXLevel;
	propLump.m_nMaxDXLevel = build.m_nMaxDXLevel;
	
	if (build.m_pLightingOrigin && *build.m_pLightingOrigin)
	{
		if (ComputeLightingOrigin( build, propLump.m_LightingOrigin ))
		{
			propLump.m_Flags |= STATIC_PROP_USE_LIGHTING_ORIGIN;
		}
	}

	propLump.m_nLightmapResolutionX = build.m_LightmapResolutionX;
	propLump.m_nLightmapResolutionY = build.m_LightmapResolutionY;

	// Add the leaves to the leaf lump
	for (int j = 0; j < leafList.Size(); ++j)
	{
		StaticPropLeafLump_t insert;
		insert.m_Leaf = leafList[j];
		s_StaticPropLeafLump.AddToTail( insert );
	}

}


//-----------------------------------------------------------------------------
// Places static props in the lump
//-----------------------------------------------------------------------------

static void SetLumpData( )
{
	GameLumpHandle_t handle = g_GameLumps.GetGameLumpHandle(GAMELUMP_STATIC_PROPS);
	if (handle != g_GameLumps.InvalidGameLump())
		g_GameLumps.DestroyGameLump(handle);

	int dictsize = s_StaticPropDictLump.Size() * sizeof(StaticPropDictLump_t);
	int objsize = s_StaticPropLump.Size() * sizeof(StaticPropLump_t);
	int leafsize = s_StaticPropLeafLump.Size() * sizeof(StaticPropLeafLump_t);
	int size = dictsize + objsize + leafsize + 3 * sizeof(int);

	handle = g_GameLumps.CreateGameLump( GAMELUMP_STATIC_PROPS, size, 0, GAMELUMP_STATIC_PROPS_VERSION );

	// Serialize the data
	CUtlBuffer buf( g_GameLumps.GetGameLump(handle), size );
	buf.PutInt( s_StaticPropDictLump.Size() );
	if (dictsize)
		buf.Put( s_StaticPropDictLump.Base(), dictsize );
	buf.PutInt( s_StaticPropLeafLump.Size() );
	if (leafsize)
		buf.Put( s_StaticPropLeafLump.Base(), leafsize );
	buf.PutInt( s_StaticPropLump.Size() );
	if (objsize)
		buf.Put( s_StaticPropLump.Base(), objsize );
}


void SearchQCs(CUtlHashDict<QCFile_t *> &vecQCs, const char *szSearchDir = "modelsrc")
{
	FileFindHandle_t handle;
	
	char szQCPattern[MAX_PATH] = { 0 };
	Q_snprintf(szQCPattern, MAX_PATH, "%s/*.*", szSearchDir);

	for (const char *szFindResult = g_pFullFileSystem->FindFirst(szQCPattern, &handle);
		szFindResult;
		szFindResult = g_pFullFileSystem->FindNext(handle))
	{
		if (szFindResult[0] != '.')
		{
			char szFullPath[MAX_PATH] = { 0 };
			V_snprintf(szFullPath, sizeof(szFullPath), "%s/%s", szSearchDir, szFindResult);
			V_FixDoubleSlashes(szFullPath);

			if (g_pFullFileSystem->FindIsDirectory(handle))
			{
				SearchQCs(vecQCs, szFullPath);
			}
			else
			{
				char szExtension[MAX_PATH] = { 0 };
				V_ExtractFileExtension(szFindResult, szExtension, MAX_PATH);

				if (!V_stricmp(szExtension, "qc"))
				{
					bool retVal = false;
					QCFile_t *newQC = new QCFile_t(szSearchDir, szFullPath, &retVal);

					if (retVal)
						vecQCs.Insert(newQC->m_pPath, newQC);
					else
						delete newQC;
				}
			}
		}
	}
}

#ifdef STATIC_PROP_COMBINE_ENABLED

#define MAX_GROUPING_KEY 256

#define MAX_SURFACEPROP 32

struct buildvars_t {
	int contents;
	char surfaceProp[MAX_SURFACEPROP];
	char cdMats[MAX_PATH];
};

inline const char *GetGroupingKeyAndSetNeededBuildVars(StaticPropBuild_t build, CUtlVector<buildvars_t> *vecBuildVars)
{
	// Load the studio model file
	CUtlBuffer buf;
	if (!LoadStudioModel(build.m_pModelName, "prop_static", buf))
	{
		Warning("Error loading studio model \"%s\"!\n", build.m_pModelName);

		return NULL;
	}

	// Compute the convex hull of the model...
	studiohdr_t* pStudioHdr = (studiohdr_t*)buf.PeekGet();

	// Memory leak. In the spirit of valve, TOO BAD!
	char *groupingKey = new char[MAX_GROUPING_KEY];
	int curGroupingKeyInd = 0;

	// Create Build Vars
	buildvars_t buildVars = { 0 };
	buildVars.contents = pStudioHdr->contents;
	V_strcpy(buildVars.surfaceProp, pStudioHdr->pszSurfaceProp());

	for (int cdMatInd = 0; cdMatInd < pStudioHdr->numcdtextures; ++cdMatInd)
	{
		const char *cdMat = pStudioHdr->pCdtexture(cdMatInd);
		V_strcpy(buildVars.cdMats, cdMat);

		// TODO: Sort this stuff
		// Add textures to key
		for (int texInd = 0; texInd < pStudioHdr->numtextures; ++texInd)
		{
			V_strcpy(&groupingKey[curGroupingKeyInd], cdMat);
			curGroupingKeyInd += V_strlen(cdMat);

			mstudiotexture_t *tex = pStudioHdr->pTexture(cdMatInd);
			const char *texName = tex->pszName();

			V_strcpy(&groupingKey[curGroupingKeyInd], texName);
			curGroupingKeyInd += V_strlen(texName);
		}
	}

	vecBuildVars->AddToTail(buildVars);

	V_FixSlashes(groupingKey, '/');

	// Add model flags
	V_snprintf(&groupingKey[curGroupingKeyInd], 9, "%08X", pStudioHdr->flags);
	curGroupingKeyInd += 8;

	// Add prop flags
	const int relevantPropFlags = ~(STATIC_PROP_USE_LIGHTING_ORIGIN | STATIC_PROP_FLAG_FADES);

	V_snprintf(&groupingKey[curGroupingKeyInd], 9, "%08X", build.m_Flags & relevantPropFlags);
	curGroupingKeyInd += 8;

	// Add contents
	V_snprintf(&groupingKey[curGroupingKeyInd], 9, "%08X", pStudioHdr->contents);
	curGroupingKeyInd += 8;

	// Add surfaceprop
	V_strcpy(&groupingKey[curGroupingKeyInd], pStudioHdr->pszSurfaceProp());
	curGroupingKeyInd += V_strlen(pStudioHdr->pszSurfaceProp());

	// TODO: Add renderFX

	// TODO: Add tint

	return groupingKey;
}

struct reallocinfo_t
{
	void **pos;
	size_t element_size;
};

inline void reallocMeshSource(s_source_t &combined, const s_source_t &addition)
{
	int oldCombinedVerts = combined.numvertices;
	int oldCombinedFaces = combined.numfaces;

	combined.numvertices += addition.numvertices;
	combined.numfaces += addition.numfaces;

	reallocinfo_t vertallocinfo[] = {
		{(void **)&combined.vertex,				sizeof(Vector)},
		{(void **)&combined.normal,				sizeof(Vector)},
		{(void **)&combined.texcoord,			sizeof(Vector2D)},

	};

	for (int i = 0; i < (sizeof(vertallocinfo) / sizeof(*vertallocinfo)); ++i) 
	{
		void *oldPos = *vertallocinfo[i].pos;

		*vertallocinfo[i].pos = calloc(combined.numvertices, vertallocinfo[i].element_size);

		V_memcpy(*vertallocinfo[i].pos, oldPos, oldCombinedVerts * vertallocinfo[i].element_size);

		if(oldPos)
			free(oldPos);
	}

	s_face_t *oldFaces = combined.face;

	combined.face = (s_face_t *)calloc(combined.numfaces, sizeof(*oldFaces));

	V_memcpy(combined.face, oldFaces, oldCombinedFaces * sizeof(*oldFaces));

	if (oldFaces)
		free(oldFaces);
}

inline void SaveNodes_Static(s_source_t *source, CUtlBuffer& buf)
{
	// Static, 1 node
	buf.Printf("nodes\n0 \"static_prop\" - 1\nend\n");
}

inline void SaveSkeleton_Static(s_source_t *source, CUtlBuffer& buf)
{
	// Static, no animation
	buf.Printf("skeleton\ntime 0\n0 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000\nend\n");
}

inline void SaveTriangles_Static(s_source_t *source, CUtlBuffer& buf)
{
	buf.Printf("triangles\n");

	// Face
	for (int face = 0; face < source->numfaces; ++face)
	{
		int texIndex = g_material[source->face[face].material];
		const char *texName = g_texture[texIndex].name;
		buf.Printf("%s\n", texName);

		// Vertex
		for (int vert = 0; vert < 3; vert++)
		{
			unsigned long vertInd = source->face[face].verts[vert];

			// No parent bone (Might be wrong)
			buf.Printf("0\t\t");

			// Position
			buf.Printf("%f %f %f\t", source->vertex[vertInd].x, source->vertex[vertInd].y, source->vertex[vertInd].z);

			// Normal
			buf.Printf("%f %f %f\t", source->normal[vertInd].x, source->normal[vertInd].y, source->normal[vertInd].z);

			// UV
			buf.Printf("%f %f\n", source->texcoord[vertInd].x, 1 - source->texcoord[vertInd].y); // needs 1 - for some reason???
		}

	}
}

// Modified Save_SMD that actually saves the whole smd
void Save_SMD_Static(char const *filename, s_source_t *source)
{
	// Text buffer
	CUtlBuffer buf(0, 0, CUtlBuffer::TEXT_BUFFER);

	// Header
	buf.Printf("version 1\n");

	// Nodes
	SaveNodes_Static(source, buf);

	// Skeleton
	SaveSkeleton_Static(source, buf);

	// Triangles
	SaveTriangles_Static(source, buf);

	FileHandle_t fh = g_pFileSystem->Open(filename, "wb");
	if (FILESYSTEM_INVALID_HANDLE != fh)
	{
		g_pFileSystem->Write(buf.Base(), buf.TellPut(), fh);
		g_pFileSystem->Close(fh);
	}
}

inline void CombineMeshes(s_source_t &combined, const s_source_t &addition, const Vector &additionOrigin, const QAngle &additionAngles, float scale)
{
	if (addition.numvertices == 0) // Nothing to add
		return;

	QAngle correctionAngle = QAngle(0, -90, 0);
	Vector correctedOffset;
	VectorRotate(additionOrigin, correctionAngle, correctedOffset);

	QAngle correctedAngle = QAngle(-additionAngles.z, additionAngles.y, additionAngles.x);

	matrix3x4_t rotMatrix;
	AngleMatrix(correctedAngle, rotMatrix);

	matrix3x4_t transformMatrix = rotMatrix;
	MatrixScaleBy(scale, transformMatrix);
	MatrixSetColumn(correctedOffset, 3, transformMatrix);

	int oldNumVertices = combined.numvertices;
	int oldNumFaces = combined.numfaces;

	// Might be slow :)
	reallocMeshSource(combined, addition);

	for (int face = 0; face < addition.numfaces; face++)
	{
		int combinedFaceNum = oldNumFaces + face;

		s_face_t addedFace = addition.face[face];

		for (int vert = 0; vert < 3; vert++)
		{
			int meshVertNum = addedFace.verts[vert];

			int combinedVertNum = oldNumVertices + meshVertNum;

			combined.texcoord[combinedVertNum]			= addition.texcoord[meshVertNum];

			VectorTransform(addition.vertex[meshVertNum], transformMatrix, combined.vertex[combinedVertNum]);
			VectorTransform(addition.normal[meshVertNum], rotMatrix, combined.normal[combinedVertNum]);
			
			addedFace.verts[vert] = combinedVertNum;
		}

		combined.face[combinedFaceNum] = addedFace;
	}
}

inline void SaveQCFile(const char *filename, const char *modelname, const char *refpath, const char *phypath, const char *animpath, const char *surfaceprop, const char *cdmaterials, int contents)
{
	CUtlBuffer buf(0, 0, CUtlBuffer::TEXT_BUFFER);

	buf.Printf("$staticprop\n");

	char modelname_file[MAX_PATH];
	V_strcpy(modelname_file, modelname);
	V_SetExtension(modelname_file, "mdl", MAX_PATH);

	buf.Printf("$modelname \"%s\"\n", modelname_file);
	buf.Printf("$surfaceprop \"%s\"\n", surfaceprop);

	char refpath_file[MAX_PATH];
	V_FileBase(refpath, refpath_file, MAX_PATH);
	V_SetExtension(refpath_file, "smd", MAX_PATH);
	buf.Printf("$body body \"%s\"\n", refpath_file);

	buf.Printf("$contents %d\n", contents);

	char animpath_file[MAX_PATH];
	V_FileBase(animpath, animpath_file, MAX_PATH);
	V_SetExtension(animpath_file, "smd", MAX_PATH);
	buf.Printf("$sequence idle %s act_idle 1\n", animpath_file);

	buf.Printf("$cdmaterials \"%s\"\n", cdmaterials);

	char phypath_file[MAX_PATH];
	V_FileBase(phypath, phypath_file, MAX_PATH);
	V_SetExtension(phypath_file, "smd", MAX_PATH);
	buf.Printf("$collisionmodel \"%s\" {\n", phypath_file);
	buf.Printf("$maxconvexpieces 2048\n");
	buf.Printf("$automass\n");
	buf.Printf("$concave\n}\n");

	FileHandle_t fh = g_pFileSystem->Open(filename, "wb");
	if (FILESYSTEM_INVALID_HANDLE != fh)
	{
		g_pFileSystem->Write(buf.Base(), buf.TellPut(), fh);
		g_pFileSystem->Close(fh);
	}
}

inline void SaveSampleAnimFile(const char *filename) {
	CUtlBuffer buf(0, 0, CUtlBuffer::TEXT_BUFFER);

	buf.Printf("version 1\nnodes\n0 \"static_prop\" -1\nend\nskeleton\ntime 0\n0 0 0 0 0 0 0\nend\n");

	FileHandle_t fh = g_pFileSystem->Open(filename, "wb");
	if (FILESYSTEM_INVALID_HANDLE != fh)
	{
		g_pFileSystem->Write(buf.Base(), buf.TellPut(), fh);
		g_pFileSystem->Close(fh);
	}
}

const char *g_szMapFileName;

inline void GroupPropsForVolume(bspbrush_t *pBSPBrushList, const CUtlVector<int> *keyGroupedProps, const CUtlVector<StaticPropBuild_t> *vecBuilds, CUtlVector<bool> *vecBuildAccountedFor, CUtlVector<buildvars_t> *vecBuildVars, CUtlHashDict<QCFile_t *> &dQCs)
{
	CUtlVector<int> localGroup;

	for (int propInd = 0; propInd < keyGroupedProps->Count(); ++propInd)
	{
		int buildInd = keyGroupedProps->Element(propInd);
		if (vecBuildAccountedFor->Element(buildInd))
			continue;

		StaticPropBuild_t propBuild = vecBuilds->Element(buildInd);

		// Get the collision model
		CPhysCollide* pConvexHull = GetCollisionModel(propBuild.m_pModelName);
		if (!pConvexHull)
		{
			AddStaticPropToLump(propBuild);
			vecBuildAccountedFor->Element(buildInd) = true;

			continue;
		}


		Vector mins, maxs;
		s_pPhysCollision->CollideGetAABB(&mins, &maxs, pConvexHull, propBuild.m_Origin, propBuild.m_Angles);

		bspbrush_t *testBrush = BrushFromBounds(mins, maxs);

		for (bspbrush_t *pBrush = pBSPBrushList; pBrush; pBrush = pBrush->next)
		{
			if (IsBoxIntersectingBox(testBrush->mins, testBrush->maxs, pBrush->mins, pBrush->maxs))
			{
				bspbrush_t *pIntersect = IntersectBrush(testBrush, pBrush);
				if (pIntersect)
				{
					// Locally group the prop
					localGroup.AddToTail(buildInd);
					vecBuildAccountedFor->Element(buildInd) = true;

					FreeBrush(pIntersect);
				}
			}
		}
	}

	if (localGroup.Count() < 1)
		return;

	Msg("Group:\n");

	s_source_t combinedMesh;
	V_memset(&combinedMesh, 0, sizeof(combinedMesh));

	s_source_t combinedCollisionMesh;
	V_memset(&combinedCollisionMesh, 0, sizeof(combinedCollisionMesh));

	// TODO: actually combine the local group by writing a qc and building the model
	for (int localGroupInd = 0; localGroupInd < localGroup.Count(); localGroupInd++)
	{
		int buildInd = localGroup[localGroupInd];

		int qcInd = dQCs.Find(vecBuilds->Element(buildInd).m_pModelName);
		QCFile_t *correspondingQC = dQCs[qcInd];

		{
			// Add to mesh

			s_source_t additionMesh;
			V_memset(&additionMesh, 0, sizeof(additionMesh));

			s_source_t additionCollisionMesh;
			V_memset(&additionCollisionMesh, 0, sizeof(additionCollisionMesh));

			// Load the mesh into the addition meshes
			V_strcpy(additionMesh.filename, "../");
			V_strcpy(additionCollisionMesh.filename, "../");

			V_strcpy(additionMesh.filename + 3, correspondingQC->m_pRefSMD);
			V_strcpy(additionCollisionMesh.filename + 3, correspondingQC->m_pPhySMD);

			Load_SMD(&additionMesh);
			Load_SMD(&additionCollisionMesh);

			Vector offsetOrigin = vecBuilds->Element(buildInd).m_Origin - vecBuilds->Element(localGroup[0]).m_Origin;

			CombineMeshes(combinedMesh, additionMesh, offsetOrigin, vecBuilds->Element(buildInd).m_Angles, correspondingQC->m_flRefScale);
			CombineMeshes(combinedCollisionMesh, additionCollisionMesh, offsetOrigin, vecBuilds->Element(buildInd).m_Angles, correspondingQC->m_flPhyScale);

			//TODO: Free addition meshes
		}


		// In here, get the SMD of the model somehow using the QC. use motionmapper's Load_SMD by setting filename in the input
		// This updates the s_source thing 
		// Then combine these into 1 somehow
		// Then, after all of this, compile into a mdl and add to map
		vecBuildAccountedFor->Element(buildInd) = true;

		const Vector &buildOrigin = vecBuilds->Element(buildInd).m_Origin;

		Msg("\t%s : %f %f %f : %d\n", vecBuilds->Element(buildInd).m_pModelName, buildOrigin.x, buildOrigin.y, buildOrigin.z, buildInd);
	}

	char pTempFilePath[MAX_PATH];
	scriptlib->MakeTemporaryFilename(gamedir, pTempFilePath, MAX_PATH);
	V_SetExtension(pTempFilePath, "smd", MAX_PATH);

	char pTempCollisionFilePath[MAX_PATH];
	scriptlib->MakeTemporaryFilename(gamedir, pTempCollisionFilePath, MAX_PATH);
	V_SetExtension(pTempCollisionFilePath, "smd", MAX_PATH);

	char pTempAnimFilePath[MAX_PATH];
	scriptlib->MakeTemporaryFilename(gamedir, pTempAnimFilePath, MAX_PATH);
	V_SetExtension(pTempAnimFilePath, "smd", MAX_PATH);

	Save_SMD_Static(pTempFilePath, &combinedMesh);
	Save_SMD_Static(pTempCollisionFilePath, &combinedCollisionMesh);

	SaveSampleAnimFile(pTempAnimFilePath);

	char pQCFilePath[MAX_PATH];
	V_strcpy(pQCFilePath, pTempFilePath);
	V_SetExtension(pQCFilePath, "qc", MAX_PATH);

	buildvars_t &buildVars = vecBuildVars->Element(localGroup[0]);

	char pModelName[MAX_PATH];
	V_FileBase(pQCFilePath, pModelName, MAX_PATH);
	
	char pMapBase[MAX_PATH];
	V_FileBase(g_szMapFileName, pMapBase, MAX_PATH);

	char pFullModelPath[MAX_PATH];
	V_snprintf(pFullModelPath, MAX_PATH, "maps\\%s\\%s", pMapBase, pModelName);
	V_SetExtension(pFullModelPath, "mdl", MAX_PATH);

	SaveQCFile(pQCFilePath, pFullModelPath, pTempFilePath,
		pTempCollisionFilePath, pTempAnimFilePath, buildVars.surfaceProp, buildVars.cdMats, buildVars.contents);
	//
	char pStudioMDLCmd[MAX_PATH];
	FileSystem_GetExecutableDir(pStudioMDLCmd, MAX_PATH);
	V_snprintf(pStudioMDLCmd, sizeof(pStudioMDLCmd), "%s\\studiomdl.exe", pStudioMDLCmd);

	char pGameDirectory[MAX_PATH];
	GetModSubdirectory("", pGameDirectory, sizeof(pGameDirectory));
	Q_RemoveDotSlashes(pGameDirectory);

	// TODO: Nothing happens
	char *argv[] =
	{
		"-game",
		pGameDirectory,
		pQCFilePath,
		NULL
	};

	_spawnv(_P_WAIT, pStudioMDLCmd, argv);

	char pModelBuildName[MAX_PATH];
	V_snprintf(pModelBuildName, sizeof(pModelBuildName), "models\\%s", pFullModelPath);

	StaticPropBuild_t newBuild = vecBuilds->Element(localGroup[0]);
	newBuild.m_pModelName = V_strdup(pModelBuildName);
	newBuild.m_Angles = QAngle(0, 0, 0); // TODO: Change base model (maybe just by combining with nothing?)

	AddStaticPropToLump(newBuild);
}

#endif // STATIC_PROP_COMBINE_ENABLED


//-----------------------------------------------------------------------------
// Places Static Props in the level
//-----------------------------------------------------------------------------

#ifdef STATIC_PROP_COMBINE_ENABLED
void EmitStaticProps(const char *szMapName)
#else
void EmitStaticProps()
#endif
{

#ifdef STATIC_PROP_COMBINE_ENABLED
	g_szMapFileName = szMapName;
#endif


	CreateInterfaceFn physicsFactory = GetPhysicsFactory();
	if ( physicsFactory )
	{
		s_pPhysCollision = (IPhysicsCollision *)physicsFactory( VPHYSICS_COLLISION_INTERFACE_VERSION, NULL );
		if( !s_pPhysCollision )
			return;
	}

	// Generate a list of lighting origins, and strip them out
	int i;
	for ( i = 0; i < num_entities; ++i)
	{
		char* pEntity = ValueForKey(&entities[i], "classname");
		if (!Q_strcmp(pEntity, "info_lighting"))
		{
			s_LightingInfo.AddToTail(i);
		}
	}

#ifdef STATIC_PROP_COMBINE_ENABLED
	CUtlVector<int> vecPropCombineVolumes;
	CUtlVector<StaticPropBuild_t> vecBuilds;
	CUtlVector<bool> vecBuildAccountedFor;

#endif // STATIC_PROP_COMBINE_ENABLED

	// Emit specifically specified static props
	for ( i = 0; i < num_entities; ++i)
	{
		char* pEntity = ValueForKey(&entities[i], "classname");
#ifdef STATIC_PROP_COMBINE_ENABLED
		if (!strcmp(pEntity, "comp_propcombine_volume"))
		{
			vecPropCombineVolumes.AddToTail(i);
		} else
#endif // STATIC_PROP_COMBINE_ENABLED
		if (!strcmp(pEntity, "static_prop") || !strcmp(pEntity, "prop_static"))
		{
			StaticPropBuild_t build;

			GetVectorForKey( &entities[i], "origin", build.m_Origin );
			GetAnglesForKey( &entities[i], "angles", build.m_Angles );
			build.m_pModelName = ValueForKey( &entities[i], "model" );
			build.m_Solid = IntForKey( &entities[i], "solid" );
			build.m_Skin = IntForKey( &entities[i], "skin" );
			build.m_FadeMaxDist = FloatForKey( &entities[i], "fademaxdist" );
			build.m_Flags = 0;//IntForKey( &entities[i], "spawnflags" ) & STATIC_PROP_WC_MASK;
			if (IntForKey( &entities[i], "ignorenormals" ) == 1)
			{
				build.m_Flags |= STATIC_PROP_IGNORE_NORMALS;
			}
			if (IntForKey( &entities[i], "disableshadows" ) == 1)
			{
				build.m_Flags |= STATIC_PROP_NO_SHADOW;
			}
			if (IntForKey( &entities[i], "disablevertexlighting" ) == 1)
			{
				build.m_Flags |= STATIC_PROP_NO_PER_VERTEX_LIGHTING;
			}
			if (IntForKey( &entities[i], "disableselfshadowing" ) == 1)
			{
				build.m_Flags |= STATIC_PROP_NO_SELF_SHADOWING;
			}

			if (IntForKey( &entities[i], "screenspacefade" ) == 1)
			{
				build.m_Flags |= STATIC_PROP_SCREEN_SPACE_FADE;
			}

			if (IntForKey( &entities[i], "generatelightmaps") == 0)
			{
				build.m_Flags |= STATIC_PROP_NO_PER_TEXEL_LIGHTING;			
				build.m_LightmapResolutionX = 0;
				build.m_LightmapResolutionY = 0;
			}
			else
			{
				build.m_LightmapResolutionX = IntForKey( &entities[i], "lightmapresolutionx" );
				build.m_LightmapResolutionY = IntForKey( &entities[i], "lightmapresolutiony" );
			}

			const char *pKey = ValueForKey( &entities[i], "fadescale" );
			if ( pKey && pKey[0] )
			{
				build.m_flForcedFadeScale = FloatForKey( &entities[i], "fadescale" );
			}
			else
			{
				build.m_flForcedFadeScale = 1;
			}
			build.m_FadesOut = (build.m_FadeMaxDist > 0);
			build.m_pLightingOrigin = ValueForKey( &entities[i], "lightingorigin" );
			if (build.m_FadesOut)
			{			  
				build.m_FadeMinDist = FloatForKey( &entities[i], "fademindist" );
				if (build.m_FadeMinDist < 0)
				{
					build.m_FadeMinDist = build.m_FadeMaxDist; 
				}
			}
			else
			{
				build.m_FadeMinDist = 0;
			}
			build.m_nMinDXLevel = (unsigned short)IntForKey( &entities[i], "mindxlevel" );
			build.m_nMaxDXLevel = (unsigned short)IntForKey( &entities[i], "maxdxlevel" );

#ifdef STATIC_PROP_COMBINE_ENABLED
			vecBuilds.AddToTail(build);

			vecBuildAccountedFor.AddToTail(false);


#else
			AddStaticPropToLump( build );
#endif // STATIC_PROP_COMBINE_ENABLED

			// strip this ent from the .bsp file
			entities[i].epairs = 0;
		}
	}

#ifdef STATIC_PROP_COMBINE_ENABLED

	if (vecPropCombineVolumes.Count() > 0)
	{
		Msg("\nCombining static props to reduce drawcalls...\n\n");

		//int nOriginalCount = vecBuilds.Count();
		//int nCombineIndex = 0;

		

		// Combining algorithm:

		// Load all QCs
		CUtlHashDict<QCFile_t *> dQCs;
		dQCs.Purge();
		SearchQCs(dQCs);

		// Find prop materials by decompiling or finding qc


		// RIGHT NOW: ASSUME WE HAVE THE MODEL SRC - QC FOUND

		// Pair key to group, by index in vecBuilds
		CUtlHashDict<CUtlVector<int> *> dPropGroups;
		dPropGroups.Purge();

		CUtlVector<buildvars_t> vecBuildVars;
		
		// Create grouping key for each
		// TODO: Allow disabling some of the more unnecessary things (surfaceprop etc)
		for (i = 0; i < vecBuilds.Size(); ++i) 
		{
			// This probably won't ever happen but just in case
			if (vecBuildAccountedFor[i])
				continue;

			const char *groupingKey = GetGroupingKeyAndSetNeededBuildVars(vecBuilds[i], &vecBuildVars);

			if (!groupingKey)
			{
				// Add prop as it was not grouped
				AddStaticPropToLump(vecBuilds[i]);
				vecBuildAccountedFor[i] = true;

				continue;
			}

			int groupInd = dPropGroups.Find(groupingKey);

			// Add to groups
			if (!dPropGroups.IsValidIndex(groupInd))
			{
				// FIXME: DELETE THESE
				CUtlVector<int> *newGroup = new CUtlVector<int>();

				groupInd = dPropGroups.Insert(groupingKey, newGroup);
			}

			dPropGroups[groupInd]->AddToTail(i);
		}

		Msg("GROUPS PRE-VOLUME:\n");
		for (i = 0; i < dPropGroups.Count(); ++i) 
		{
			Msg("Group %d:\n", i);

			CUtlVector<int> *propGroup = dPropGroups[i];
			for (int propInd = 0; propInd < propGroup->Count(); ++propInd)
			{
				Vector &buildOrigin = vecBuilds[propGroup->Element(propInd)].m_Origin;

				Msg("\t%s : %f %f %f : %d\n", vecBuilds[propGroup->Element(propInd)].m_pModelName, buildOrigin.x, buildOrigin.y, buildOrigin.z, propGroup->Element(propInd));
			}
		}

		// Setup motionmapper globals
		g_currentscale = g_defaultscale = 1.0;
		g_defaultrotation = RadianEuler(0, 0, M_PI / 2);

		flip_triangles = 0;
		normal_blend = 2.0f; // Never blend


		//, then split these groups by the propcombine volume
		for (i = 0; i < vecPropCombineVolumes.Count(); ++i) 
		{
			entity_t propVolumeEnt = entities[vecPropCombineVolumes[i]];

			Vector clipMins, clipMaxs;
			clipMins[0] = clipMins[1] = clipMins[2] = MIN_COORD_INTEGER;
			clipMaxs[0] = clipMaxs[1] = clipMaxs[2] = MAX_COORD_INTEGER;

			bspbrush_t *pBSPBrushList = MakeBspBrushList(propVolumeEnt.firstbrush, propVolumeEnt.firstbrush + propVolumeEnt.numbrushes,
				clipMins, clipMaxs, NO_DETAIL);
			ChopBrushes(pBSPBrushList);

			for (int group = 0; group < dPropGroups.Count(); group++)
			{
				CUtlVector<int> *groupVec = dPropGroups.Element(group);

				GroupPropsForVolume(pBSPBrushList, groupVec, &vecBuilds, &vecBuildAccountedFor, &vecBuildVars, dQCs);
			}
		}

		// TODO: Delete temp files, can't do it with how processes are spawned currently
// 		scriptlib->DeleteTemporaryFiles("*.smd");
// 		scriptlib->DeleteTemporaryFiles("*.qc");

		// Make sure to add ungrouped props
		for (i = 0; i < vecBuildAccountedFor.Count(); ++i)
		{
			if (!vecBuildAccountedFor[i])
			{
				AddStaticPropToLump(vecBuilds[i]);
			}
		}

		// Then combine these props, create new builds and add to lump

		for (i = vecPropCombineVolumes.Count(); --i >= 0; )
		{
			// Strip the ent, hard to tell if this removes the faces but I think it does
			entities[vecPropCombineVolumes[i]].epairs = 0;
			entities[vecPropCombineVolumes[i]].numbrushes = 0;
		}
	}
	else 
	{
		for (i = 0; i < vecBuilds.Count(); ++i) 
		{
			AddStaticPropToLump(vecBuilds[i]);
		}
	}

#endif // STATIC_PROP_COMBINE_ENABLED

	// Strip out lighting origins; has to be done here because they are used when
	// static props are made
	for ( i = s_LightingInfo.Count(); --i >= 0; )
	{
		// strip this ent from the .bsp file
		entities[s_LightingInfo[i]].epairs = 0;
	}


	SetLumpData( );
}

static studiohdr_t *g_pActiveStudioHdr;
static void SetCurrentModel( studiohdr_t *pStudioHdr )
{
	// track the correct model
	g_pActiveStudioHdr = pStudioHdr;
}

static void FreeCurrentModelVertexes()
{
	Assert( g_pActiveStudioHdr );

	if ( g_pActiveStudioHdr->pVertexBase )
	{
		free( g_pActiveStudioHdr->pVertexBase );
		g_pActiveStudioHdr->pVertexBase = NULL;
	}
}

const vertexFileHeader_t * mstudiomodel_t::CacheVertexData( void * pModelData )
{
	char				fileName[260];
	FileHandle_t		fileHandle;
	vertexFileHeader_t	*pVvdHdr;

	Assert( pModelData == NULL );
	Assert( g_pActiveStudioHdr );

	if ( g_pActiveStudioHdr->pVertexBase )
	{
		return (vertexFileHeader_t *)g_pActiveStudioHdr->pVertexBase;
	}

	// mandatory callback to make requested data resident
	// load and persist the vertex file
	strcpy( fileName, "models/" );	
	strcat( fileName, g_pActiveStudioHdr->pszName() );
	Q_StripExtension( fileName, fileName, sizeof( fileName ) );
	strcat( fileName, ".vvd" );

	// load the model
	fileHandle = g_pFileSystem->Open( fileName, "rb" );
	if ( !fileHandle )
	{
		Error( "Unable to load vertex data \"%s\"\n", fileName );
	}

	// Get the file size
	int size = g_pFileSystem->Size( fileHandle );
	if (size == 0)
	{
		g_pFileSystem->Close( fileHandle );
		Error( "Bad size for vertex data \"%s\"\n", fileName );
	}

	pVvdHdr = (vertexFileHeader_t *)malloc(size);
	g_pFileSystem->Read( pVvdHdr, size, fileHandle );
	g_pFileSystem->Close( fileHandle );

	// check header
	if (pVvdHdr->id != MODEL_VERTEX_FILE_ID)
	{
		Error("Error Vertex File %s id %d should be %d\n", fileName, pVvdHdr->id, MODEL_VERTEX_FILE_ID);
	}
	if (pVvdHdr->version != MODEL_VERTEX_FILE_VERSION)
	{
		Error("Error Vertex File %s version %d should be %d\n", fileName, pVvdHdr->version, MODEL_VERTEX_FILE_VERSION);
	}
	if (pVvdHdr->checksum != g_pActiveStudioHdr->checksum)
	{
		Error("Error Vertex File %s checksum %d should be %d\n", fileName, pVvdHdr->checksum, g_pActiveStudioHdr->checksum);
	}

	g_pActiveStudioHdr->pVertexBase = (void*)pVvdHdr;
	return pVvdHdr;
}

