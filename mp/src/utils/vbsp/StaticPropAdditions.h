#ifndef STATICPROPADDITIONS_H
#define STATICPROPADDITIONS_H

#ifdef _WIN32
#pragma once
#endif

#include "vbsp.h"
#include "utlhashdict.h"
#include "utlmap.h"
#include "VPhysics_Interface.h"
#include "../motionmapper/motionmapper.h" // For SMD support

#ifdef STATIC_PROP_COMBINE_ENABLED

IPhysicsCollision *s_pPhysCollision = NULL;

struct QCFile_t
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
	float	m_Scale;
};

void AddStaticPropToLump(StaticPropBuild_t const& build);
CPhysCollide* GetCollisionModel(char const* pModelName);

#define MAX_SURFACEPROP 32

struct buildvars_t {
	int contents;
	char surfaceProp[MAX_SURFACEPROP];
	char cdMats[MAX_PATH];
};

struct loaded_model_smds_t
{
	s_source_t refSMD;
	s_source_t phySMD;
};

void SearchQCs(CUtlHashDict<QCFile_t *> &dQCs, const char *szSearchDir = "modelsrc");
void ScalePropAndAddToLump(const StaticPropBuild_t &propBuild, const buildvars_t &buildVars, CUtlHashDict<QCFile_t *> &dQCs, CUtlHashDict<loaded_model_smds_t> &dLoadedSMDs, CUtlMap<CRC32_t, const char *> *mapCombinedProps, CRC32_t crc);
void AddStaticPropToLumpWithScaling(const StaticPropBuild_t &build, const buildvars_t &buildVars, CUtlHashDict<QCFile_t *> &dQCs, CUtlHashDict<loaded_model_smds_t> &dLoadedSMDs, CUtlMap<CRC32_t, const char *> *mapCombinedProps);

#define MAX_GROUPING_KEY 256

const char *GetGroupingKeyAndSetNeededBuildVars(StaticPropBuild_t build, CUtlVector<buildvars_t> *vecBuildVars);

struct reallocinfo_t
{
	void **pos;
	size_t element_size;
};

void reallocMeshSource(s_source_t &combined, const s_source_t &addition);

void CombineMeshes(s_source_t &combined, const s_source_t &addition, const Vector &additionOrigin, const QAngle &additionAngles, float scale);

void Save_SMD_Static(char const *filename, s_source_t *source); // Modified Save_SMD that actually saves the whole smd
void SaveSampleAnimFile(const char *filename);
void SaveQCFile(const char *filename, const char *modelname, const char *refpath, const char *phypath, const char *animpath, const char *surfaceprop, const char *cdmaterials, int contents, bool hasCollision);

int DecompileModel(const StaticPropBuild_t &localBuild, const char *pDecompCache, const char *pCrowbarCMD, CUtlHashDict<QCFile_t *> &dQCs);

// Global map file name
extern const char *g_szMapFileName;

StaticPropBuild_t CompileAndAddToLump(s_source_t &combinedMesh, s_source_t &combinedCollisionMesh, const buildvars_t &buildVars, const StaticPropBuild_t &build, const char *pGameDirectory, const Vector &avgPos, const QAngle &angles, CRC32_t crc);

void GroupPropsForVolume(bspbrush_t *pBSPBrushList, const CUtlVector<int> *keyGroupedProps, const CUtlVector<StaticPropBuild_t> *vecBuilds, CUtlVector<bool> *vecBuildAccountedFor,
	CUtlVector<buildvars_t> *vecBuildVars, CUtlHashDict<QCFile_t *> &dQCs, CUtlHashDict<loaded_model_smds_t> &dLoadedSMDs, CUtlMap<CRC32_t, const char *> *combinedProps);

void InitCache(CUtlMap<CRC32_t, const char *> *mapCombinedProps);
int ParseCacheLine(const char *cacheLine, CUtlMap<CRC32_t, const char *> *mapCombinedProps);
void WriteCacheLine(const char *modelName, CRC32_t processedCRC, const char *processedModelName);
void WriteCacheLine(const CUtlVector<StaticPropBuild_t> *vecBuilds, const CUtlVector<int> *localGroup, CRC32_t processedCRC, const char *processedModelName);
void CloseCache();


#endif // STATIC_PROP_COMBINE_ENABLED

#endif // STATICPROPADDITIONS_H