#include "StaticPropAdditions.h"

#include "vbsp.h"
#include "gamebspfile.h"
#include "utlhashdict.h"
#include "utlmap.h"
#include "UtlBuffer.h"
#include "CollisionUtils.h"
#include "tier2/fileutils.h"
#include "tier1/tier1.h"
#include "vstdlib/iprocessutils.h"

#include "../motionmapper/motionmapper.h" // For SMD support

#ifdef STATIC_PROP_COMBINE_ENABLED

//---------------------------------------------------------------------//
//                                                                     //
//---------------------------------------------------------------------//

//---------------------------------------------------------------------//
//                              QC FILES                               //
//---------------------------------------------------------------------//


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
			int braceCount = 1;

			while (GetToken(true))
			{
				if (!V_strcmp(token, "{"))
					braceCount++;

				if (!V_strcmp(token, "}"))
				{
					braceCount--;
					if (braceCount == 0)
						break;
				}
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
			if (!GetToken(true)) // Group name
				goto invalidQC;

			if (!GetToken(true))
				goto invalidQC;

			if (V_strcmp(token, "{")) // String
			{
				if (m_pRefSMD)
					goto invalidQC;

				char *pRefSMD = new char[MAX_PATH];
				V_snprintf(pRefSMD, MAX_PATH, "%s/%s", pFileLocation, token);

				m_pRefSMD = pRefSMD;
				m_flRefScale = scaleFactor;
				continue;
			}

			// Brace open
			while (GetToken(true) && V_strcmp(token, "}"))
			{
				if (!V_stricmp(token, "studio"))
				{
					if (m_pRefSMD)
					{
						// goto invalidQC; // try to allow this by just using the first one
						continue;
					}

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

	if (pRetVal)
		*pRetVal = false;

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

void SearchQCs(CUtlHashDict<QCFile_t *> &dQCs, const char *szSearchDir /*= "modelsrc"*/)
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
				SearchQCs(dQCs, szFullPath);
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
						dQCs.Insert(newQC->m_pPath, newQC);
					else
						delete newQC;
				}
			}
		}
	}
}

void SaveQCFile(const char *filename, const char *modelname, const char *refpath, const char *phypath, const char *animpath, const char *surfaceprop, const char (*cdmaterials)[MAX_PATH], int contents, bool hasCollision)
{
	// TODO: Set bbox
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

	buf.Printf("$cdmaterials ");

	for (int i = 0; i < MAX_CDMATERIALS; ++i)
	{
		if (cdmaterials[i][0] == 0)
			break;

		buf.Printf("\"%s\" ", cdmaterials[i]);
	}

	buf.Printf("\n");

	if (hasCollision)
	{
		char phypath_file[MAX_PATH];
		V_FileBase(phypath, phypath_file, MAX_PATH);
		V_SetExtension(phypath_file, "smd", MAX_PATH);
		buf.Printf("$collisionmodel \"%s\" {\n", phypath_file);
		buf.Printf("$maxconvexpieces 2048\n");
		buf.Printf("$automass\n");
		buf.Printf("$concave\n}\n");
	}

	FileHandle_t fh = g_pFileSystem->Open(filename, "wb");
	if (FILESYSTEM_INVALID_HANDLE != fh)
	{
		g_pFileSystem->Write(buf.Base(), buf.TellPut(), fh);
		g_pFileSystem->Close(fh);
	}

	buf.Clear();
}

//---------------------------------------------------------------------//
//                             SMD FILES                               //
//---------------------------------------------------------------------//

void reallocMeshSource(s_source_t &combined, const s_source_t &addition)
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

		if (oldPos)
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

	buf.Clear();
}

void CombineMeshes(s_source_t &combined, const s_source_t &addition, const Vector &additionOrigin, const QAngle &additionAngles, float scale)
{
	if (addition.numvertices == 0) // Nothing to add
		return;

	matrix3x4_t rotMatrix;
	AngleMatrix(QAngle(0, 90, 0), rotMatrix);

	matrix3x4_t additionRotMatrix;
	AngleMatrix(additionAngles, additionRotMatrix);

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

			combined.texcoord[combinedVertNum] = addition.texcoord[meshVertNum];

			Vector yawRotatedVertex, rotatedVertex, yawRotatedNormal, rotatedNormal;

			VectorTransform(addition.vertex[meshVertNum], rotMatrix, yawRotatedVertex);
			VectorTransform(addition.normal[meshVertNum], rotMatrix, yawRotatedNormal);

			yawRotatedVertex *= scale;
			VectorTransform(yawRotatedNormal, additionRotMatrix, rotatedNormal);
			VectorTransform(yawRotatedVertex, additionRotMatrix, rotatedVertex);

			rotatedVertex += additionOrigin;

			combined.vertex[combinedVertNum] = rotatedVertex;
			combined.normal[combinedVertNum] = rotatedNormal;

			addedFace.verts[vert] = combinedVertNum;
		}

		combined.face[combinedFaceNum] = addedFace;
	}
}

void SaveSampleAnimFile(const char *filename) 
{
	CUtlBuffer buf(0, 0, CUtlBuffer::TEXT_BUFFER);

	buf.Printf("version 1\nnodes\n0 \"static_prop\" -1\nend\nskeleton\ntime 0\n0 0 0 0 0 0 0\nend\n");

	FileHandle_t fh = g_pFileSystem->Open(filename, "wb");
	if (FILESYSTEM_INVALID_HANDLE != fh)
	{
		g_pFileSystem->Write(buf.Base(), buf.TellPut(), fh);
		g_pFileSystem->Close(fh);
	}

	buf.Clear();
}

//---------------------------------------------------------------------//
//                         MESH DECOMPILATION                          //
//---------------------------------------------------------------------//

const char *mdlExts[] = {
	".mdl",
	".phy",
	".dx90.vtx",
	".dx80.vtx",
	".sw.vtx",
	".vvd",
};

int DecompileModel(const StaticPropBuild_t &localBuild, const char *pDecompCache, const char *pCrowbarCMD, CUtlHashDict<QCFile_t *> &dQCs)
{
	int qcInd = dQCs.InvalidHandle();

	// Decompile the model
	const char *pLocalMDLName = localBuild.m_pModelName;

	char pMDLFileBase[MAX_PATH];
	V_FileBase(pLocalMDLName, pMDLFileBase, MAX_PATH);

	char pTempIntermediateName[MAX_PATH];
	char pTempMDLPath[MAX_PATH];
	scriptlib->MakeTemporaryFilename(gamedir, pTempIntermediateName, MAX_PATH);
	V_ExtractFilePath(pTempIntermediateName, pTempMDLPath, MAX_PATH);

	char pDecompMDLFullPath[MAX_PATH];
	V_snprintf(pDecompMDLFullPath, MAX_PATH, "%s%s", pTempMDLPath, pMDLFileBase);
	V_SetExtension(pDecompMDLFullPath, "mdl", MAX_PATH);


	for (int extInd = 0; extInd < sizeof(mdlExts) / sizeof(*mdlExts); extInd++)
	{
		char pLocalFileName[MAX_PATH];
		char pDecompFileFullPath[MAX_PATH];

		V_strcpy(pLocalFileName, pLocalMDLName);
		V_strcpy(pDecompFileFullPath, pDecompMDLFullPath);

		V_SetExtension(pLocalFileName, mdlExts[extInd], MAX_PATH);
		V_SetExtension(pDecompFileFullPath, mdlExts[extInd], MAX_PATH);

		CUtlBuffer fileBuf;
		if (g_pFullFileSystem->ReadFile(pLocalFileName, NULL, fileBuf))
			g_pFullFileSystem->WriteFile(pDecompFileFullPath, NULL, fileBuf);

		fileBuf.Clear();
	}

	char pMDLFileStem[MAX_PATH];
	V_StripExtension(pLocalMDLName, pMDLFileStem, MAX_PATH);

	char pDecompCacheForMDL[MAX_PATH];
	char pDecompCacheForMDLWithQuotes[MAX_PATH];
	V_snprintf(pDecompCacheForMDL, MAX_PATH, "%s%s\\", pDecompCache, pMDLFileStem);
	V_FixSlashes(pDecompCacheForMDL);
	V_snprintf(pDecompCacheForMDLWithQuotes, MAX_PATH, "\"%s\"", pDecompCacheForMDL);

	// Quotes needed for arg parser for some reason
	char pDecompMDLFullPathWithQuotes[MAX_PATH];
	V_snprintf(pDecompMDLFullPathWithQuotes, MAX_PATH, "\"%s\"", pDecompMDLFullPath);

	const char *crowbarArgv[] =
	{
		"NOTHING", // pad it out with random text so spaces don't matter
		"decompile",
		"-i",
		pDecompMDLFullPathWithQuotes,
		"-o",
		pDecompCacheForMDLWithQuotes,
		NULL
	};

	if (_spawnv(_P_WAIT, pCrowbarCMD, crowbarArgv) != 0) // error
	{
		// Console Spam :))
		// Warning("Unable to decompile %s.\n", localBuild.m_pModelName);
		return dQCs.InvalidHandle();
	}

	char pDecompiledQCPath[MAX_PATH];
	V_snprintf(pDecompiledQCPath, MAX_PATH, "%s%s.qc", pDecompCacheForMDL, pMDLFileBase);

	bool retVal = false;
	QCFile_t *newQC = new QCFile_t(pDecompCacheForMDL, pDecompiledQCPath, &retVal);

	if (retVal)
		qcInd = dQCs.Insert(newQC->m_pPath, newQC);
	else
	{
		delete newQC;
		Warning("Unable to load decompiled qc for %s.\n", localBuild.m_pModelName);
		return dQCs.InvalidHandle();
	}

	return qcInd;
}

//---------------------------------------------------------------------//
//                           LUMP ADDITION                             //
//---------------------------------------------------------------------//

StaticPropBuild_t CompileAndAddToLump(s_source_t &combinedMesh, s_source_t &combinedCollisionMesh, const buildvars_t &buildVars, const StaticPropBuild_t &build, const char *pGameDirectory, const Vector &avgPos, const QAngle &angles, CRC32_t crc)
{
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

	if (combinedCollisionMesh.numvertices != 0)
	{
		Save_SMD_Static(pTempCollisionFilePath, &combinedCollisionMesh);
	}

	SaveSampleAnimFile(pTempAnimFilePath);

	char pQCFilePath[MAX_PATH];
	V_strcpy(pQCFilePath, pTempFilePath);
	V_SetExtension(pQCFilePath, "qc", MAX_PATH);

	char pModelName[MAX_PATH];
	V_snprintf(pModelName, MAX_PATH, "%x", crc);

	char pMapBase[MAX_PATH];
	V_FileBase(g_szMapFileName, pMapBase, MAX_PATH);

	char pFullModelPath[MAX_PATH];
	V_snprintf(pFullModelPath, MAX_PATH, "maps\\%s\\%s", pMapBase, pModelName);
	V_SetExtension(pFullModelPath, "mdl", MAX_PATH);

	SaveQCFile(pQCFilePath, pFullModelPath, pTempFilePath,
		pTempCollisionFilePath, pTempAnimFilePath, buildVars.surfaceProp, buildVars.cdMats, buildVars.contents, combinedCollisionMesh.numvertices != 0);
	//
	char pStudioMDLCmd[MAX_PATH];
	FileSystem_GetExecutableDir(pStudioMDLCmd, MAX_PATH);
	V_snprintf(pStudioMDLCmd, sizeof(pStudioMDLCmd), "%s\\studiomdl.exe", pStudioMDLCmd);

	// TODO: Nothing happens
	const char *argv[] =
	{
		"-game",
		pGameDirectory,
		pQCFilePath,
		NULL
	};

	if (_spawnv(_P_WAIT, pStudioMDLCmd, argv) != 0) // error
	{
		Error("Unable to combine prop! This will produce a warning only in future but I haven't gotten around to it yet\n");
	}

	char pModelBuildName[MAX_PATH];
	V_snprintf(pModelBuildName, sizeof(pModelBuildName), "models\\%s", pFullModelPath);

	StaticPropBuild_t newBuild = build;
	newBuild.m_pModelName = V_strdup(pModelBuildName);
	newBuild.m_Origin = avgPos;
	newBuild.m_Angles = angles;

	AddStaticPropToLump(newBuild);

	return newBuild;
}

void AddStaticPropToLumpWithScaling(const StaticPropBuild_t &build, const buildvars_t &buildVars, CUtlHashDict<QCFile_t *> &dQCs, CUtlHashDict<loaded_model_smds_t> &dLoadedSMDs, CUtlMap<CRC32_t, const char *> *mapCombinedProps)
{
	if (fabs(build.m_Scale - 1.0f) < 0.0001f)
	{
		AddStaticPropToLump(build);

		return;
	}

	CRC32_t crc;
	CRC32_Init(&crc);

	CRC32_ProcessBuffer(&crc, build.m_pModelName, sizeof(char) * V_strlen(build.m_pModelName));
	CRC32_ProcessBuffer(&crc, &build.m_Skin, sizeof(int));
	CRC32_ProcessBuffer(&crc, &build.m_Scale, sizeof(float));
	CRC32_ProcessBuffer(&crc, &build.m_Solid, sizeof(int));

	CRC32_Final(&crc);

	int combinedPropsInd = mapCombinedProps->Find(crc);
	if (mapCombinedProps->IsValidIndex(combinedPropsInd))
	{
		StaticPropBuild_t newBuild = build;
		newBuild.m_pModelName = mapCombinedProps->Element(combinedPropsInd);
		newBuild.m_Origin = build.m_Origin;
		newBuild.m_Angles = build.m_Angles;

		AddStaticPropToLump(newBuild);
		return;
	}

	ScalePropAndAddToLump(build, buildVars, dQCs, dLoadedSMDs, mapCombinedProps, crc);
}

void ScalePropAndAddToLump(const StaticPropBuild_t &propBuild, const buildvars_t &buildVars, CUtlHashDict<QCFile_t *> &dQCs, CUtlHashDict<loaded_model_smds_t> &dLoadedSMDs, CUtlMap<CRC32_t, const char *> *mapCombinedProps, CRC32_t crc)
{
	char pCrowbarCMD[MAX_PATH];
	{
		char pCrowbarDir[MAX_PATH];
		FileSystem_GetExecutableDir(pCrowbarDir, MAX_PATH);
		V_snprintf(pCrowbarCMD, sizeof(pCrowbarCMD), "%s\\crowbar.exe", pCrowbarDir);
	}

	char pGameDirectory[MAX_PATH];
	GetModSubdirectory("", pGameDirectory, sizeof(pGameDirectory));
	Q_RemoveDotSlashes(pGameDirectory);

	char pDecompCache[MAX_PATH];
	V_snprintf(pDecompCache, MAX_PATH, "%sdecomp_cache\\", pGameDirectory);

	int qcInd = dQCs.Find(propBuild.m_pModelName);
	if (!dQCs.IsValidIndex(qcInd))
	{
		qcInd = DecompileModel(propBuild, pDecompCache, pCrowbarCMD, dQCs);

		if (!dQCs.IsValidIndex(qcInd)) {
			Warning("Unable to decompile %s. Not scaling...\n", propBuild.m_pModelName);
			AddStaticPropToLump(propBuild);

			return;
		}
	}

	QCFile_t *correspondingQC = dQCs[qcInd];

	int smdInd = dLoadedSMDs.Find(propBuild.m_pModelName);
	if (!dLoadedSMDs.IsValidIndex(smdInd))
	{
		smdInd = dLoadedSMDs.Insert(propBuild.m_pModelName);

		loaded_model_smds_t &loadingSMD = dLoadedSMDs.Element(smdInd);

		V_memset(&loadingSMD.refSMD, 0, sizeof(s_source_t));
		V_memset(&loadingSMD.phySMD, 0, sizeof(s_source_t));

		// Load the mesh into the addition meshes
		V_snprintf(loadingSMD.refSMD.filename, MAX_PATH, "%s%s", pGameDirectory, correspondingQC->m_pRefSMD);

		Load_SMD(&loadingSMD.refSMD);

		if (correspondingQC->m_pPhySMD)
		{
			V_snprintf(loadingSMD.phySMD.filename, MAX_PATH, "%s%s", pGameDirectory, correspondingQC->m_pPhySMD);

			Load_SMD(&loadingSMD.phySMD);
		}
	}

	s_source_t scaledMesh;
	V_memset(&scaledMesh, 0, sizeof(scaledMesh));

	s_source_t scaledCollisionMesh;
	V_memset(&scaledCollisionMesh, 0, sizeof(scaledCollisionMesh));

	const s_source_t &loadedMesh = dLoadedSMDs.Element(smdInd).refSMD;
	CombineMeshes(scaledMesh, loadedMesh, Vector(0, 0, 0), QAngle(0, -90, 0), correspondingQC->m_flRefScale * propBuild.m_Scale);

	if (dLoadedSMDs.Element(smdInd).phySMD.numvertices != 0)
	{
		const s_source_t &loadedCollisionMesh = dLoadedSMDs.Element(smdInd).phySMD;

		CombineMeshes(scaledCollisionMesh, loadedCollisionMesh, Vector(0, 0, 0), QAngle(0, -90, 0), correspondingQC->m_flPhyScale * propBuild.m_Scale);
	}

	StaticPropBuild_t outBuild = CompileAndAddToLump(scaledMesh, scaledCollisionMesh, buildVars, propBuild, pGameDirectory, propBuild.m_Origin, propBuild.m_Angles, crc);
	WriteCacheLine(propBuild.m_pModelName, crc, outBuild.m_pModelName);

	mapCombinedProps->Insert(crc, outBuild.m_pModelName);
}

//---------------------------------------------------------------------//
//                       GROUPING MAIN FUNCTIONS                       //
//---------------------------------------------------------------------//

CRC32_t GetGroupingKeyAndSetNeededBuildVars(StaticPropBuild_t build, CUtlVector<buildvars_t> *vecBuildVars, bool *retVal)
{
	// Load the studio model file
	CUtlBuffer buf;
	if (!LoadStudioModel(build.m_pModelName, "prop_static", buf))
	{
		Warning("Error loading studio model \"%s\"!\n", build.m_pModelName);

		vecBuildVars->AddToTail(); // Add blank buildvars

		if (retVal)
			*retVal = false;

		return 0;
	}

	// Compute the convex hull of the model...
	studiohdr_t* pStudioHdr = (studiohdr_t*)buf.PeekGet();

	CRC32_t groupCRC;
	CRC32_Init(&groupCRC);

	// Create Build Vars
	buildvars_t buildVars = { 0 };
	buildVars.contents = pStudioHdr->contents;
	V_strcpy(buildVars.surfaceProp, pStudioHdr->pszSurfaceProp());

	for (int cdMatInd = 0; cdMatInd < pStudioHdr->numcdtextures; ++cdMatInd)
	{
		char *cdMat = pStudioHdr->pCdtexture(cdMatInd);
		V_FixSlashes(cdMat, '/');
		V_strcpy(buildVars.cdMats[cdMatInd], cdMat);

		CRC32_ProcessBuffer(&groupCRC, cdMat, V_strlen(cdMat));
	}

	for (int texInd = 0; texInd < pStudioHdr->numtextures; ++texInd)
	{
		mstudiotexture_t *tex = pStudioHdr->pTexture(texInd);
		char *texName = tex->pszName();
		V_FixSlashes(texName, '/');

		CRC32_ProcessBuffer(&groupCRC, texName, V_strlen(texName));
	}

	vecBuildVars->AddToTail(buildVars);

	// Add model flags
	CRC32_ProcessBuffer(&groupCRC, &pStudioHdr->flags, sizeof(pStudioHdr->flags));

	// Add prop flags
	const int relevantPropFlags = ~(STATIC_PROP_USE_LIGHTING_ORIGIN | STATIC_PROP_FLAG_FADES);

	int maskedFlags = build.m_Flags & relevantPropFlags;
	CRC32_ProcessBuffer(&groupCRC, &maskedFlags, sizeof(maskedFlags));

	// Add contents
	CRC32_ProcessBuffer(&groupCRC, &pStudioHdr->contents, sizeof(pStudioHdr->contents));

	// Add surfaceprop
	CRC32_ProcessBuffer(&groupCRC, &buildVars.surfaceProp, sizeof(buildVars.surfaceProp));

	// TODO: Add renderFX

	// TODO: Add tint
	buf.Clear();

	CRC32_Final(&groupCRC);

	if (retVal)
		*retVal = true;

	return groupCRC;
}

#define PROPPOS_ROUND_NUM 100.f

int GroupPropsForVolume(combinevolume_t &combineVolume, const CUtlVector<int> *keyGroupedProps, const CUtlVector<StaticPropBuild_t> *vecBuilds, CUtlVector<bool> *vecBuildAccountedFor,
	CUtlVector<buildvars_t> *vecBuildVars, CUtlHashDict<QCFile_t *> &dQCs, CUtlHashDict<loaded_model_smds_t> &dLoadedSMDs, CUtlMap<CRC32_t, const char *> *combinedProps)
{
	CUtlVector<int> localGroup;

	Vector avgPos = Vector(0, 0, 0);
	float avgYaw = 0.f;

	int numPropsMade = 0;

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
			AddStaticPropToLumpWithScaling(propBuild, vecBuildVars->Element(buildInd), dQCs, dLoadedSMDs, combinedProps);
			vecBuildAccountedFor->Element(buildInd) = true;

			numPropsMade++;

			continue;
		}


		Vector mins, maxs;
		s_pPhysCollision->CollideGetAABB(&mins, &maxs, pConvexHull, propBuild.m_Origin, propBuild.m_Angles);

		// Make sure to fix for scale
		mins = propBuild.m_Origin + (mins - propBuild.m_Origin) * propBuild.m_Scale;
		maxs = propBuild.m_Origin + (maxs - propBuild.m_Origin) * propBuild.m_Scale;

		bspbrush_t *testBrush = BrushFromBounds(mins, maxs);

		for (bspbrush_t *pBrush = combineVolume.pBSPBrushList; pBrush; pBrush = pBrush->next)
		{
			if (IsBoxIntersectingBox(testBrush->mins, testBrush->maxs, pBrush->mins, pBrush->maxs))
			{
				bspbrush_t *pIntersect = IntersectBrush(testBrush, pBrush);
				if (pIntersect)
				{
					// Locally group the prop
					localGroup.AddToTail(buildInd);

					avgPos += propBuild.m_Origin;
					avgYaw += propBuild.m_Angles[YAW];

					vecBuildAccountedFor->Element(buildInd) = true;

					FreeBrush(pIntersect);
				}
			}
		}
	}

	if (localGroup.Count() < 1)
		return numPropsMade;

	if (localGroup.Count() == 1)
	{
		AddStaticPropToLumpWithScaling(vecBuilds->Element(localGroup[0]), vecBuildVars->Element(localGroup[0]), dQCs, dLoadedSMDs, combinedProps);
		vecBuildAccountedFor->Element(localGroup[0]) = true;
		return numPropsMade++;
	}

	avgPos /= localGroup.Count();
	avgYaw /= localGroup.Count(); // TODO: Round to nearest 15 degrees

	avgYaw = RoundInt(avgYaw / 15.f) * 15.f;

	matrix3x4_t yawRot;
	AngleMatrix(QAngle(0, -avgYaw, 0), yawRot);

	CRC32_t propPosCRC;
	CRC32_Init(&propPosCRC);
	for (int localGroupInd = 0; localGroupInd < localGroup.Count(); localGroupInd++)
	{
		int buildInd = localGroup[localGroupInd];
		const StaticPropBuild_t &localBuild = vecBuilds->Element(buildInd);

		Vector additionOrigin;
		VectorRotate(localBuild.m_Origin - avgPos, yawRot, additionOrigin);

		QAngle additionAngle = localBuild.m_Angles;
		additionAngle[YAW] -= avgYaw;

		int additionOriginDivisions[3];
		int additionAngleDivisions[3];

		for (int i = 0; i < 3; ++i)
		{
			additionOriginDivisions[i] = RoundInt(additionOrigin.Base()[i] * PROPPOS_ROUND_NUM);
			additionAngleDivisions[i] = RoundInt(additionAngle.Base()[i] * PROPPOS_ROUND_NUM);

			additionOrigin.Base()[i] = additionOriginDivisions[i] / PROPPOS_ROUND_NUM;
			additionAngle.Base()[i] = additionAngleDivisions[i] / PROPPOS_ROUND_NUM;
		}

		float buildScale = 1;

		CRC32_ProcessBuffer(&propPosCRC, additionOriginDivisions, sizeof(additionOriginDivisions));
		CRC32_ProcessBuffer(&propPosCRC, additionAngleDivisions, sizeof(additionAngleDivisions));
		CRC32_ProcessBuffer(&propPosCRC, localBuild.m_pModelName, sizeof(char) * V_strlen(localBuild.m_pModelName));
		CRC32_ProcessBuffer(&propPosCRC, &localBuild.m_Skin, sizeof(int));
		CRC32_ProcessBuffer(&propPosCRC, &buildScale, sizeof(float));
		CRC32_ProcessBuffer(&propPosCRC, &localBuild.m_Solid, sizeof(int));
	}
	CRC32_Final(&propPosCRC);


	int combinedPropsInd = combinedProps->Find(propPosCRC);
	if (combinedProps->IsValidIndex(combinedPropsInd))
	{
		StaticPropBuild_t newBuild = vecBuilds->Element(localGroup[0]); // Use first prop to get the build info
		newBuild.m_pModelName = combinedProps->Element(combinedPropsInd);
		newBuild.m_Origin = avgPos;
		newBuild.m_Angles = QAngle(0, avgYaw - 90, 0);

		AddStaticPropToLump(newBuild);
		return numPropsMade++;
	}

	s_source_t combinedMesh;
	V_memset(&combinedMesh, 0, sizeof(combinedMesh));

	s_source_t combinedCollisionMesh;
	V_memset(&combinedCollisionMesh, 0, sizeof(combinedCollisionMesh));


	char pCrowbarCMD[MAX_PATH];
	{
		char pCrowbarDir[MAX_PATH];
		FileSystem_GetExecutableDir(pCrowbarDir, MAX_PATH);
		V_snprintf(pCrowbarCMD, sizeof(pCrowbarCMD), "%s\\crowbar.exe", pCrowbarDir);
	}

	char pGameDirectory[MAX_PATH];
	GetModSubdirectory("", pGameDirectory, sizeof(pGameDirectory));
	Q_RemoveDotSlashes(pGameDirectory);

	char pDecompCache[MAX_PATH];
	V_snprintf(pDecompCache, MAX_PATH, "%sdecomp_cache\\", pGameDirectory);

	

	// TODO: actually combine the local group by writing a qc and building the model
	for (int localGroupInd = 0; localGroupInd < localGroup.Count(); localGroupInd++)
	{
		int buildInd = localGroup[localGroupInd];
		const StaticPropBuild_t &localBuild = vecBuilds->Element(buildInd);

		int qcInd = dQCs.Find(vecBuilds->Element(buildInd).m_pModelName);
		if (!dQCs.IsValidIndex(qcInd))
		{
			qcInd = DecompileModel(localBuild, pDecompCache, pCrowbarCMD, dQCs);

			if (!dQCs.IsValidIndex(qcInd)) {
				Warning("Unable to decompile %s. This prop will not be in the map!\n", localBuild.m_pModelName);

				continue;
			}
		}

		QCFile_t *correspondingQC = dQCs[qcInd];

		// Add to mesh

		int smdInd = dLoadedSMDs.Find(localBuild.m_pModelName);
		if (!dLoadedSMDs.IsValidIndex(smdInd))
		{
			smdInd = dLoadedSMDs.Insert(localBuild.m_pModelName);

			loaded_model_smds_t &loadingSMD = dLoadedSMDs.Element(smdInd);

			V_memset(&loadingSMD.refSMD, 0, sizeof(s_source_t));
			V_memset(&loadingSMD.phySMD, 0, sizeof(s_source_t));

			// Load the mesh into the addition meshes
			V_snprintf(loadingSMD.refSMD.filename, MAX_PATH, "%s%s", pGameDirectory, correspondingQC->m_pRefSMD);

			Load_SMD(&loadingSMD.refSMD);

			if (correspondingQC->m_pPhySMD)
			{
				V_snprintf(loadingSMD.phySMD.filename, MAX_PATH, "%s%s", pGameDirectory, correspondingQC->m_pPhySMD);

				Load_SMD(&loadingSMD.phySMD);
			}
		}

		const s_source_t &additionMesh = dLoadedSMDs.Element(smdInd).refSMD;

		Vector additionOrigin;
		VectorRotate(localBuild.m_Origin - avgPos, yawRot, additionOrigin);

		QAngle additionAngle = localBuild.m_Angles;
		additionAngle[YAW] -= avgYaw;

		int additionOriginDivisions[3];
		int additionAngleDivisions[3];

		for (int i = 0; i < 3; ++i) // TODO: CACHE THIS STUFF MAYBE?
		{
			additionOriginDivisions[i] = RoundInt(additionOrigin.Base()[i] * PROPPOS_ROUND_NUM);
			additionAngleDivisions[i] = RoundInt(additionAngle.Base()[i] * PROPPOS_ROUND_NUM);

			additionOrigin.Base()[i] = additionOriginDivisions[i] / PROPPOS_ROUND_NUM;
			additionAngle.Base()[i] = additionAngleDivisions[i] / PROPPOS_ROUND_NUM;
		}

		CombineMeshes(combinedMesh, additionMesh, additionOrigin, additionAngle, correspondingQC->m_flRefScale * localBuild.m_Scale);

		if (dLoadedSMDs.Element(smdInd).phySMD.numvertices != 0)
		{
			const s_source_t &additionCollisionMesh = dLoadedSMDs.Element(smdInd).phySMD;

			CombineMeshes(combinedCollisionMesh, additionCollisionMesh, additionOrigin, additionAngle, correspondingQC->m_flPhyScale * localBuild.m_Scale);
		}

		// In here, get the SMD of the model somehow using the QC. use motionmapper's Load_SMD by setting filename in the input
		// This updates the s_source thing 
		// Then combine these into 1 somehow
		// Then, after all of this, compile into a mdl and add to map
		vecBuildAccountedFor->Element(buildInd) = true;
	}

	StaticPropBuild_t createdBuild = CompileAndAddToLump(combinedMesh, combinedCollisionMesh, vecBuildVars->Element(localGroup[0]), vecBuilds->Element(localGroup[0]), pGameDirectory, avgPos, QAngle(0, avgYaw - 90, 0), propPosCRC);

	WriteCacheLine(vecBuilds, &localGroup, propPosCRC, createdBuild.m_pModelName);
	combinedProps->Insert(propPosCRC, createdBuild.m_pModelName);

	return numPropsMade++;
}

//---------------------------------------------------------------------//
//                               CACHING                               //
//---------------------------------------------------------------------//

#define CACHE_LOCATION "modelsrc\\vbspaddon_mdls.cache"
#define SEPARATOR_CHAR 0x1b
#define MAX_CHECKSUM_LENGTH 11

static const int MAX_CACHE_LINE = MAX_PATH * 6 + MAX_CHECKSUM_LENGTH * 6 + 12; // 6 Paths, 6 CRC32s, 11 seperator characters

FileHandle_t cacheFileHandle;

void InitCache(CUtlMap<CRC32_t, const char *> *mapCombinedProps) {

	Msg("Initializing vbsp addon cache...\n");

	cacheFileHandle = g_pFullFileSystem->Open(CACHE_LOCATION, "rb");
	if (FILESYSTEM_INVALID_HANDLE == cacheFileHandle)
	{
		cacheFileHandle = g_pFullFileSystem->Open(CACHE_LOCATION, "ab");
		if (FILESYSTEM_INVALID_HANDLE == cacheFileHandle)
		{
			Warning("Unable to create cache at " CACHE_LOCATION "\n");
		}

		return;
	}

	char tempFile[MAX_PATH];
	scriptlib->MakeTemporaryFilename(gamedir, tempFile, MAX_PATH);
	FileHandle_t tempFileHandle = g_pFullFileSystem->Open(tempFile, "wb+");
	if (FILESYSTEM_INVALID_HANDLE == tempFileHandle)
	{
		Warning("Unable to read temp file... not deleting incorrect lines.\n");
		return;
	}

	char cacheLine[MAX_CACHE_LINE];

	while (g_pFullFileSystem->ReadLine(cacheLine, MAX_CACHE_LINE, cacheFileHandle))
	{
		int lineLength = 0;
		if (lineLength = ParseCacheLine(cacheLine, mapCombinedProps))
		{
			g_pFullFileSystem->Write(cacheLine, lineLength, tempFileHandle);
		}
	}

	g_pFullFileSystem->Close(tempFileHandle);
	tempFileHandle = g_pFullFileSystem->Open(tempFile, "rb");
	if (FILESYSTEM_INVALID_HANDLE == tempFileHandle)
	{
		Warning("Unable to read temp file... not deleting incorrect lines.\n");
		return;
	}

	g_pFullFileSystem->Close(cacheFileHandle);
	cacheFileHandle = g_pFullFileSystem->Open(CACHE_LOCATION, "wb");
	if (FILESYSTEM_INVALID_HANDLE == cacheFileHandle)
	{
		Warning("Unable to create cache at " CACHE_LOCATION "\n");
		return;
	}

	g_pFullFileSystem->Seek(tempFileHandle, 0, FILESYSTEM_SEEK_HEAD);
	while (g_pFullFileSystem->ReadLine(cacheLine, MAX_CACHE_LINE, tempFileHandle))
	{
		int lineLength = V_strlen(cacheLine);
		g_pFullFileSystem->Write(cacheLine, lineLength, cacheFileHandle);
	}

	g_pFullFileSystem->Close(tempFileHandle);
}

bool GetModelChecksum(const char *modelPath, int &checksum)
{
	FileHandle_t fh = g_pFullFileSystem->Open(modelPath, "rb");
	if (FILESYSTEM_INVALID_HANDLE == fh)
	{
		Warning("Unable to open model for checksum test\n");
		return false;
	}

	int dataBuf[3];
	g_pFullFileSystem->Read(dataBuf, sizeof(dataBuf), fh);

	g_pFullFileSystem->Close(fh);

	checksum = dataBuf[2];
	return true;
}

void WriteCacheLine(const char *modelName, CRC32_t processedCRC, const char *processedModelName)
{
	unsigned int fileLoc = g_pFullFileSystem->Tell(cacheFileHandle);
	g_pFullFileSystem->Seek(cacheFileHandle, 0, FILESYSTEM_SEEK_TAIL);

	char cacheLine[MAX_CACHE_LINE] = { 0 };

	int modelChecksum;
	if (!GetModelChecksum(modelName, modelChecksum))
		return;

	V_snprintf(cacheLine, MAX_CACHE_LINE, "%s%c0x%x%c%c0x%x%c%s\n", modelName, SEPARATOR_CHAR, modelChecksum, SEPARATOR_CHAR, SEPARATOR_CHAR, processedCRC, SEPARATOR_CHAR, processedModelName);

	g_pFullFileSystem->Write(cacheLine, V_strlen(cacheLine), cacheFileHandle);
	g_pFullFileSystem->Seek(cacheFileHandle, fileLoc, FILESYSTEM_SEEK_HEAD);
}

void WriteCacheLine(const CUtlVector<StaticPropBuild_t> *vecBuilds, const CUtlVector<int> *localGroup, CRC32_t processedCRC, const char *processedModelName)
{
	unsigned int fileLoc = g_pFullFileSystem->Tell(cacheFileHandle);
	g_pFullFileSystem->Seek(cacheFileHandle, 0, FILESYSTEM_SEEK_TAIL);

	char cacheLine[MAX_CACHE_LINE] = { 0 };
	int position = 0;

	CUtlHashDict<int8> localGroupModelsSet; // Not sure of a better way to do hash set with utl so this will have to do
	localGroupModelsSet.Purge();

	for (int i = 0; i < localGroup->Count(); ++i)
	{
		const char *modelName = vecBuilds->Element(localGroup->Element(i)).m_pModelName;

		int hashIndex = localGroupModelsSet.Find(modelName);
		if (localGroupModelsSet.IsValidIndex(hashIndex))
			continue;

		localGroupModelsSet.Insert(modelName);

		int modelChecksum;
		if (!GetModelChecksum(modelName, modelChecksum))
			return;

		position += V_snprintf(cacheLine + position, MAX_CACHE_LINE, "%s%c0x%x%c", modelName, SEPARATOR_CHAR, modelChecksum, SEPARATOR_CHAR);
	}
	
	V_snprintf(cacheLine + position, MAX_CACHE_LINE, "%c0x%x%c%s\n", SEPARATOR_CHAR, processedCRC, SEPARATOR_CHAR, processedModelName);

	g_pFullFileSystem->Write(cacheLine, V_strlen(cacheLine), cacheFileHandle);
	g_pFullFileSystem->Seek(cacheFileHandle, fileLoc, FILESYSTEM_SEEK_HEAD);
}

void CloseCache()
{
	g_pFullFileSystem->Close(cacheFileHandle);
}

inline int GetNextSeperatorPos(const char *cacheLine, int lastPos)
{
	for (int i = lastPos; i < MAX_CACHE_LINE; ++i)
	{
		if (cacheLine[i] == SEPARATOR_CHAR || cacheLine[i] == 0 || cacheLine[i] == '\n')
		{
			return i;
		}
	}

	return MAX_CACHE_LINE;
}

int ParseCacheLine(const char *cacheLine, CUtlMap<CRC32_t, const char *> *mapCombinedProps)
{
	char originalMDLPath[MAX_PATH];
	int originalMDLChecksum;
	uint32 groupedCRC;
	char combinedMDLPath[MAX_PATH];

	int lastPos = 0;
	int curPos;

	// Original models end when double separator character found
	while (cacheLine[lastPos] != SEPARATOR_CHAR) {
		curPos = GetNextSeperatorPos(cacheLine, lastPos);
		V_strncpy(originalMDLPath, cacheLine + lastPos, curPos - lastPos + 1);

		int actualChecksum;
		if (!GetModelChecksum(originalMDLPath, actualChecksum))
			return 0;

		lastPos = curPos + 1;
		curPos = GetNextSeperatorPos(cacheLine, lastPos);
		{
			char tmpString[MAX_CHECKSUM_LENGTH];
			V_strncpy(tmpString, cacheLine + lastPos, curPos - lastPos + 1);
			originalMDLChecksum = V_atoi64(tmpString);
		}

		if (actualChecksum != originalMDLChecksum)
			return 0;

		lastPos = curPos + 1;
	}

	lastPos++; // Get past double escape char

	curPos = GetNextSeperatorPos(cacheLine, lastPos);
	{
		char tmpString[MAX_CHECKSUM_LENGTH];
		V_strncpy(tmpString, cacheLine + lastPos, curPos - lastPos + 1);
		groupedCRC = V_atoi64(tmpString);
	}

	lastPos = curPos + 1;
	curPos = GetNextSeperatorPos(cacheLine, lastPos);
	V_strncpy(combinedMDLPath, cacheLine + lastPos, curPos - lastPos + 1);

	if (!g_pFullFileSystem->FileExists(combinedMDLPath))
		return 0;

	int mdlPathLen = V_strlen(combinedMDLPath);
	char *mdlPathPersistent = new char[mdlPathLen + 1];
	V_strcpy(mdlPathPersistent, combinedMDLPath);
	
	mapCombinedProps->Insert(groupedCRC, mdlPathPersistent);

	return curPos + 1;
}

#endif