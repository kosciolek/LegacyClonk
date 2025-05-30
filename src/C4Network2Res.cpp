/*
 * LegacyClonk
 *
 * Copyright (c) RedWolf Design
 * Copyright (c) 2017-2021, The LegacyClonk Team and contributors
 *
 * Distributed under the terms of the ISC license; see accompanying file
 * "COPYING" for details.
 *
 * "Clonk" is a registered trademark of Matthes Bender, used with permission.
 * See accompanying file "TRADEMARK" for details.
 *
 * To redistribute this file separately, substitute the full license texts
 * for the above references.
 */

#include <C4Include.h>
#include <C4Network2Res.h>

#include <C4Random.h>
#include <C4Config.h>
#include <C4Log.h>
#include <C4Group.h>
#include <C4Components.h>
#include <C4Game.h>
#include "StdAdaptors.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <errno.h>

// compile debug options
// #define C4NET2RES_LOAD_ALL
// #define C4NET2RES_DEBUG_LOG

// helper

class DirSizeHelper
{
	static uint32_t iSize, iMaxSize;
	static bool Helper(const char *szPath)
	{
		if (szPath[SLen(szPath) - 1] == '.')
			return false;
		if (iSize > iMaxSize)
			return false;
		if (DirectoryExists(szPath))
			ForEachFile(szPath, &Helper);
		else if (FileExists(szPath))
			iSize += FileSize(szPath);
		return true;
	}

public:
	static bool GetDirSize(const char *szPath, uint32_t *pSize, uint32_t inMaxSize = ~0)
	{
		// Security
		if (!pSize) return false;
		// Fold it
		iSize = 0; iMaxSize = inMaxSize;
		ForEachFile(szPath, &Helper);
		// Return
		*pSize = iSize;
		return true;
	}
};
uint32_t DirSizeHelper::iSize, DirSizeHelper::iMaxSize;

// *** C4Network2ResCore

C4Network2ResCore::C4Network2ResCore()
	: eType(NRT_Null),
	iID(-1), iDerID(-1),
	fLoadable(false),
	iFileSize(~0u), iFileCRC(~0u), iContentsCRC(~0u),
	iChunkSize(C4NetResChunkSize),
	fHasFileSHA(false) {}

void C4Network2ResCore::Set(C4Network2ResType enType, int32_t iResID, const char *strFileName, uint32_t inContentsCRC, const char *strAuthor)
{
	// Initialize base data
	eType = enType; iID = iResID; iDerID = -1;
	fLoadable = false;
	iFileSize = iFileCRC = ~0; iContentsCRC = inContentsCRC;
	iChunkSize = C4NetResChunkSize;
	FileName.Copy(strFileName);
	Author.Copy(strAuthor);
}

void C4Network2ResCore::SetLoadable(uint32_t iSize, uint32_t iCRC)
{
	fLoadable = true;
	iFileSize = iSize;
	iFileCRC = iCRC;
}

void C4Network2ResCore::Clear()
{
	eType = NRT_Null;
	iID = iDerID = -1;
	fLoadable = false;
	FileName.Clear();
	Author.Clear();
	iFileSize = iFileCRC = iContentsCRC = ~0;
	fHasFileSHA = false;
}

// C4PacketBase virtuals

void C4Network2ResCore::CompileFunc(StdCompiler *pComp)
{
	constexpr StdEnumEntry<C4Network2ResType> C4Network2ResType_EnumMap[] =
	{
		{ "Scenario",    NRT_Scenario },
		{ "Dynamic",     NRT_Dynamic },
		{ "Player",      NRT_Player },
		{ "Definitions", NRT_Definitions },
		{ "System",      NRT_System },
		{ "Material",    NRT_Material },
	};

	pComp->Value(mkNamingAdapt(mkEnumAdaptT<uint8_t>(eType, C4Network2ResType_EnumMap), "Type",     NRT_Null));
	pComp->Value(mkNamingAdapt(iID,                                                     "ID",       -1));
	pComp->Value(mkNamingAdapt(iDerID,                                                  "DerID",    -1));
	pComp->Value(mkNamingAdapt(fLoadable,                                               "Loadable", true));
	if (fLoadable)
	{
		pComp->Value(mkNamingAdapt(iFileSize,  "FileSize",  0U));
		pComp->Value(mkNamingAdapt(iFileCRC,   "FileCRC",   0U));
		pComp->Value(mkNamingAdapt(iChunkSize, "ChunkSize", C4NetResChunkSize));
		if (!iChunkSize) pComp->excCorrupt("zero chunk size");
	}
	pComp->Value(mkNamingAdapt(iContentsCRC,     "ContentsCRC", 0U));
	pComp->Value(mkNamingCountAdapt(fHasFileSHA, "FileSHA"));
	if (fHasFileSHA)
		pComp->Value(mkNamingAdapt(mkHexAdapt(FileSHA), "FileSHA"));
	pComp->Value(mkNamingAdapt(mkNetFilenameAdapt(FileName), "Filename", ""));
	pComp->Value(mkNamingAdapt(mkNetFilenameAdapt(Author),   "Author",   ""));
}

// *** C4Network2ResLoad

C4Network2ResLoad::C4Network2ResLoad(int32_t inChunk, int32_t inByClient)
	: iChunk(inChunk), iByClient(inByClient), Timestamp(time(nullptr)), pNext(nullptr) {}

C4Network2ResLoad::~C4Network2ResLoad() {}

bool C4Network2ResLoad::CheckTimeout()
{
	return difftime(time(nullptr), Timestamp) >= C4NetResLoadTimeout;
}

// *** C4Network2ResChunkData

C4Network2ResChunkData::C4Network2ResChunkData()
	: iChunkCnt(0), iPresentChunkCnt(0),
	pChunkRanges(nullptr), iChunkRangeCnt(0) {}

C4Network2ResChunkData::C4Network2ResChunkData(const C4Network2ResChunkData &Data2)
	: C4PacketBase(Data2),
	iChunkCnt(Data2.getChunkCnt()), iPresentChunkCnt(0),
	pChunkRanges(nullptr), iChunkRangeCnt(0)
{
	// add ranges
	Merge(Data2);
}

C4Network2ResChunkData::~C4Network2ResChunkData()
{
	Clear();
}

C4Network2ResChunkData &C4Network2ResChunkData::operator=(const C4Network2ResChunkData &Data2)
{
	// clear, merge
	SetIncomplete(Data2.getChunkCnt());
	Merge(Data2);
	return *this;
}

void C4Network2ResChunkData::SetIncomplete(int32_t inChunkCnt)
{
	Clear();
	// just set total chunk count
	iChunkCnt = inChunkCnt;
}

void C4Network2ResChunkData::SetComplete(int32_t inChunkCnt)
{
	Clear();
	// set total chunk count
	iPresentChunkCnt = iChunkCnt = inChunkCnt;
	// create one range
	ChunkRange *pRange = new ChunkRange;
	pRange->Start = 0; pRange->Length = iChunkCnt;
	pRange->Next = nullptr;
	pChunkRanges = pRange;
}

void C4Network2ResChunkData::AddChunk(int32_t iChunk)
{
	AddChunkRange(iChunk, 1);
}

void C4Network2ResChunkData::AddChunkRange(int32_t iStart, int32_t iLength)
{
	// security
	if (iStart < 0 || iStart + iLength > iChunkCnt || iLength <= 0) return;
	// find position
	ChunkRange *pRange, *pPrev;
	for (pRange = pChunkRanges, pPrev = nullptr; pRange; pPrev = pRange, pRange = pRange->Next)
		if (pRange->Start >= iStart)
			break;
	// create new
	ChunkRange *pNew = new ChunkRange;
	pNew->Start = iStart; pNew->Length = iLength;
	// add to list
	pNew->Next = pRange;
	(pPrev ? pPrev->Next : pChunkRanges) = pNew;
	// counts
	iPresentChunkCnt += iLength; iChunkRangeCnt++;
	// check merges
	if (pPrev && MergeRanges(pPrev))
		while (MergeRanges(pPrev));
	else
		while (MergeRanges(pNew));
}

void C4Network2ResChunkData::Merge(const C4Network2ResChunkData &Data2)
{
	// must have same basis chunk count
	assert(iChunkCnt == Data2.getChunkCnt());
	// add ranges
	for (ChunkRange *pRange = Data2.pChunkRanges; pRange; pRange = pRange->Next)
		AddChunkRange(pRange->Start, pRange->Length);
}

void C4Network2ResChunkData::Clear()
{
	iChunkCnt = iPresentChunkCnt = iChunkRangeCnt = 0;
	// remove all ranges
	while (pChunkRanges)
	{
		ChunkRange *pDelete = pChunkRanges;
		pChunkRanges = pDelete->Next;
		delete pDelete;
	}
}

int32_t C4Network2ResChunkData::GetChunkToRetrieve(const C4Network2ResChunkData &Available, int32_t iLoadingCnt, int32_t *pLoading) const
{
	// (this version is highly calculation-intensitive, yet the most satisfactory
	//  solution I could find)

	// find everything that should not be retrieved
	C4Network2ResChunkData ChData; Available.GetNegative(ChData);
	ChData.Merge(*this);
	for (int32_t i = 0; i < iLoadingCnt; i++)
		ChData.AddChunk(pLoading[i]);
	// nothing to retrieve?
	if (ChData.isComplete()) return -1;
	// invert to get everything that should be retrieved
	C4Network2ResChunkData ChData2; ChData.GetNegative(ChData2);
	// select chunk (random)
	int32_t iRetrieveChunk = SafeRandom(ChData2.getPresentChunkCnt());
	// return
	return ChData2.getPresentChunk(iRetrieveChunk);
}

bool C4Network2ResChunkData::MergeRanges(ChunkRange *pRange)
{
	// no next entry?
	if (!pRange || !pRange->Next) return false;
	// do merge?
	ChunkRange *pNext = pRange->Next;
	if (pRange->Start + pRange->Length < pNext->Start) return false;
	// get overlap
	int32_t iOverlap = (std::min)((pRange->Start + pRange->Length) - pNext->Start, pNext->Length);
	// set new chunk range
	pRange->Length += pNext->Length - iOverlap;
	// remove range
	pRange->Next = pNext->Next;
	delete pNext;
	// counts
	iChunkRangeCnt--; iPresentChunkCnt -= iOverlap;
	// ok
	return true;
}

void C4Network2ResChunkData::GetNegative(C4Network2ResChunkData &Target) const
{
	// clear target
	Target.SetIncomplete(iChunkCnt);
	// add all ranges that are missing
	int32_t iFreeStart = 0;
	for (ChunkRange *pRange = pChunkRanges; pRange; pRange = pRange->Next)
	{
		// add range
		Target.AddChunkRange(iFreeStart, pRange->Start - iFreeStart);
		// safe new start
		iFreeStart = pRange->Start + pRange->Length;
	}
	// add last range
	Target.AddChunkRange(iFreeStart, iChunkCnt - iFreeStart);
}

int32_t C4Network2ResChunkData::getPresentChunk(int32_t iNr) const
{
	for (ChunkRange *pRange = pChunkRanges; pRange; pRange = pRange->Next)
		if (iNr < pRange->Length)
			return iNr + pRange->Start;
		else
			iNr -= pRange->Length;
	return -1;
}

void C4Network2ResChunkData::CompileFunc(StdCompiler *pComp)
{
	bool fCompiler = pComp->isCompiler();
	if (fCompiler) Clear();
	// Data
	pComp->Value(mkNamingAdapt(mkIntPackAdapt(iChunkCnt),      "ChunkCnt",      0));
	pComp->Value(mkNamingAdapt(mkIntPackAdapt(iChunkRangeCnt), "ChunkRangeCnt", 0));
	const auto name = pComp->Name("Ranges");
	// Ranges
	if (!name)
		pComp->excCorrupt("ResChunk ranges expected!");
	ChunkRange *pRange = nullptr;
	for (int32_t i = 0; i < iChunkRangeCnt; i++)
	{
		// Create new range / go to next range
		if (fCompiler)
			pRange = (pRange ? pRange->Next : pChunkRanges) = new ChunkRange;
		else
			pRange = pRange ? pRange->Next : pChunkRanges;
		// Separate
		if (i) pComp->Separator();
		// Compile range
		pComp->Value(mkIntPackAdapt(pRange->Start));
		pComp->Separator(StdCompiler::SEP_PART2);
		pComp->Value(mkIntPackAdapt(pRange->Length));
	}
	// Terminate list
	if (fCompiler)
		(pRange ? pRange->Next : pChunkRanges) = nullptr;
}

// *** C4Network2Res

C4Network2Res::C4Network2Res(C4Network2ResList *pnParent)
	: fDirty(false),
	fTempFile(false), fStandaloneFailed(false),
	iRefCnt(0), fRemoved(false),
	iLastReqTime(0),
	fLoading(false),
	pCChunks(nullptr), iDiscoverStartTime(0), pLoads(nullptr), iLoadCnt(0),
	pNext(nullptr),
	pParent(pnParent)
{
	szFile[0] = szStandalone[0] = '\0';
}

C4Network2Res::~C4Network2Res()
{
	assert(!pNext);
	Clear();
}

bool C4Network2Res::SetByFile(const char *strFilePath, bool fTemp, C4Network2ResType eType, int32_t iResID, const char *szResName, bool fSilent)
{
	CStdLock FileLock(&FileCSec);
	// default ressource name: relative path
	if (!szResName) szResName = Config.AtExeRelativePath(strFilePath);
	SCopy(strFilePath, szFile, sizeof(szFile) - 1);
	// group?
	C4Group Grp;
	if (Grp.Open(strFilePath))
		return SetByGroup(&Grp, fTemp, eType, iResID, szResName, fSilent);
	// so it needs to be a file
	if (!FileExists(szFile))
	{
		if (!fSilent) pParent->logger->error("SetByFile: file {} not found!", strFilePath); return false;
	}
	// calc checksum
	uint32_t iCRC32;
	if (!C4Group_GetFileCRC(szFile, &iCRC32)) return false;
#ifdef C4NET2RES_DEBUG_LOG
	// log
	pParent->logger->trace("Resource: complete {}:{} is file {} ({})", iResID, szResName, szFile, fTemp ? "temp" : "static");
#endif
	// set core
	Core.Set(eType, iResID, szResName, iCRC32, "");
	// set own data
	fDirty = true;
	fTempFile = fTemp;
	fStandaloneFailed = false;
	fRemoved = false;
	iLastReqTime = time(nullptr);
	fLoading = false;
	local = true;
	// ok
	return true;
}

bool C4Network2Res::SetByGroup(C4Group *pGrp, bool fTemp, C4Network2ResType eType, int32_t iResID, const char *szResName, bool fSilent) // by main thread
{
	Clear();
	CStdLock FileLock(&FileCSec);
	// default ressource name: relative path
	StdStrBuf sResName;
	if (szResName)
		sResName.Ref(szResName);
	else
	{
		StdStrBuf sFullName = pGrp->GetFullName();
		sResName.Copy(Config.AtExeRelativePath(sFullName.getData()));
	}
	SCopy(pGrp->GetFullName().getData(), szFile, sizeof(szFile) - 1);
	// set core
	Core.Set(eType, iResID, sResName.getData(), pGrp->EntryCRC32(), pGrp->GetMaker());
#ifdef C4NET2RES_DEBUG_LOG
	// log
	pParent->logger->trace("Resource: complete {}:{} is file {} ({})", iResID, sResName.getData(), szFile, fTemp ? "temp" : "static");
#endif
	// set data
	fDirty = true;
	fTempFile = fTemp;
	fStandaloneFailed = false;
	fRemoved = false;
	iLastReqTime = time(nullptr);
	fLoading = false;
	local = true;
	// ok
	return true;
}

bool C4Network2Res::SetByCore(const C4Network2ResCore &nCore, bool fSilent, const char *szAsFilename, int32_t iRecursion) // by main thread
{
	// try open local file
	const char *szFilename = szAsFilename ? szAsFilename : GetC4Filename(nCore.getFileName());
	if (SetByFile(szFilename, false, nCore.getType(), nCore.getID(), nCore.getFileName(), fSilent))
	{
		// check contents checksum
		if (Core.getContentsCRC() == nCore.getContentsCRC())
		{
			// set core
			fDirty = true;
			Core = nCore;

			// to ensure correct file sorting
			GetStandalone(nullptr, 0, false, false, false);
			// ok then
			return true;
		}
	}
	// get and search for filename without specified folder (e.g., Castle.c4s when the opened game is Easy.c4f\Castle.c4s)
	const char *szFilenameOnly = GetFilename(szFilename);
	const char *szFilenameC4 = GetC4Filename(szFilename);
	if (szFilenameOnly != szFilenameC4)
	{
		if (SetByCore(nCore, fSilent, szFilenameOnly, Config.Network.MaxResSearchRecursion)) return true;
	}
	// if it could still not be set, try within all folders of root (ignoring special folders), and try as file outside the folder
	// but do not recurse any deeper than set by config (default: One folder)
	if (iRecursion >= Config.Network.MaxResSearchRecursion) return false;
	StdStrBuf sSearchPath; const char *szSearchPath;
	if (!iRecursion)
		szSearchPath = Config.General.ExePath;
	else
	{
		sSearchPath.Copy(szFilename, SLen(szFilename) - SLen(szFilenameC4));
		szSearchPath = sSearchPath.getData();
	}
	StdStrBuf sNetPath; sNetPath.Copy(Config.Network.WorkPath);
	char *szNetPath = sNetPath.GrabPointer();
	TruncateBackslash(szNetPath);
	sNetPath.Take(szNetPath);
	for (DirectoryIterator i(szSearchPath); *i; ++i)
		if (DirectoryExists(*i))
			if (!*GetExtension(*i)) // directories without extension only
				if (!szNetPath || !*szNetPath || !ItemIdentical(*i, szNetPath)) // ignore network path
				{
					// search for complete name at subpath (e.g. MyFolder\Easy.c4f\Castle.c4s)
					const std::string filename{std::format("{}" DirSep "{}", *i, szFilenameC4)};
					if (SetByCore(nCore, fSilent, filename.c_str(), iRecursion + 1))
						return true;
				}
	// file could not be found locally
	return false;
}

bool C4Network2Res::SetLoad(const C4Network2ResCore &nCore) // by main thread
{
	Clear();
	CStdLock FileLock(&FileCSec);
	// must be loadable
	if (!nCore.isLoadable()) return false;
	// save core, set chunks
	Core = nCore;
	Chunks.SetIncomplete(Core.getChunkCnt());
	// create temporary file
	if (!pParent->FindTempResFileName(Core.getFileName(), szFile))
		return false;
#ifdef C4NET2RES_DEBUG_LOG
	// log
	pParent->logger->trace("Resource: loading {}:{} to file {}", Core.getID(), Core.getFileName(), szFile);
#endif
	// set standalone (result is going to be binary-compatible)
	SCopy(szFile, szStandalone, sizeof(szStandalone) - 1);
	// set flags
	fDirty = false;
	fTempFile = true;
	fStandaloneFailed = false;
	fRemoved = false;
	iLastReqTime = time(nullptr);
	fLoading = true;
	// No discovery yet
	iDiscoverStartTime = 0;
	return true;
}

bool C4Network2Res::SetDerived(const char *strName, const char *strFilePath, bool fTemp, C4Network2ResType eType, int32_t iDResID)
{
	Clear();
	CStdLock FileLock(&FileCSec);
	// set core
	Core.Set(eType, C4NetResIDAnonymous, strName, ~0, "");
	Core.SetDerived(iDResID);
	// save file path
	SCopy(strFilePath, szFile, _MAX_PATH);
	*szStandalone = '\0';
	// set flags
	fDirty = false;
	fTempFile = fTemp;
	fStandaloneFailed = false;
	fRemoved = false;
	iLastReqTime = time(nullptr);
	fLoading = false;
	// Do not set any chunk data - anonymous ressources are very likely to change.
	// Wait for FinishDerived()-call.
	return true;
}

void C4Network2Res::ChangeID(int32_t inID)
{
	Core.SetID(inID);
}

bool C4Network2Res::IsBinaryCompatible()
{
	// returns wether the standalone of this ressource is binary compatible
	// to the official version (means: matches the file checksum)

	CStdLock FileLock(&FileCSec);
	// standalone set? ok then (see GetStandalone)
	if (szStandalone[0]) return true;
	// is a directory?
	if (DirectoryExists(szFile))
		// forget it - if the file is packed now, the creation time and author
		// won't match.
		return false;
	// try to create the standalone
	return GetStandalone(nullptr, 0, false, false, true);
}

bool C4Network2Res::GetStandalone(char *pTo, int32_t iMaxL, bool fSetOfficial, bool fAllowUnloadable, bool fSilent)
{
	// already set?
	if (szStandalone[0])
	{
		if (pTo) SCopy(szStandalone, pTo, iMaxL);
		return true;
	}
	// already tried and failed? No point in retrying
	if (fStandaloneFailed) return false;
	// not loadable? Wo won't be able to check the standalone as the core will lack the needed information.
	// the standalone won't be interesting in this case, anyway.
	if (!fSetOfficial && !Core.isLoadable()) return false;
	// set flag, so failure below will let future calls fail
	fStandaloneFailed = true;
	// lock file
	CStdLock FileLock(&FileCSec);

	// directory?
	SCopy(szFile, szStandalone, sizeof(szStandalone) - 1);
	if (DirectoryExists(szFile))
	{
		// size check for the directory, if allowed
		if (fAllowUnloadable)
		{
			uint32_t iDirSize;
			if (!DirSizeHelper::GetDirSize(szFile, &iDirSize, Config.Network.MaxLoadFileSize))
			{
				if (!fSilent) pParent->logger->error("could not get directory size of {}!", szFile); szStandalone[0] = '\0'; return false;
			}
			if (iDirSize > uint32_t(Config.Network.MaxLoadFileSize))
			{
				if (!fSilent) pParent->logger->error("{} over size limit, will be marked unloadable!", szFile); szStandalone[0] = '\0'; return false;
			}
		}
		// log - this may take a few seconds
		if (!fSilent) Log(C4ResStrTableKey::IDS_PRC_NETPACKING, GetFilename(szFile));
		// pack inplace?
		if (!fTempFile)
		{
			if (!pParent->FindTempResFileName(szFile, szStandalone))
			{
				if (!fSilent) pParent->logger->error("GetStandalone: could not find free name for temporary file!"); szStandalone[0] = '\0'; return false;
			}
			if (!C4Group_PackDirectoryTo(szFile, szStandalone, true))
			{
				if (!fSilent) pParent->logger->error("GetStandalone: could not pack directory!"); szStandalone[0] = '\0'; return false;
			}
		}
		else if (!C4Group_PackDirectory(szStandalone))
		{
			if (!fSilent) pParent->logger->error("GetStandalone: could not pack directory!"); if (!SEqual(szFile, szStandalone)) EraseDirectory(szStandalone); szStandalone[0] = '\0'; return false;
		}
		// make sure directory is packed
		if (DirectoryExists(szStandalone))
		{
			if (!fSilent) pParent->logger->error("GetStandalone: directory hasn't been packed!"); if (!SEqual(szFile, szStandalone)) EraseDirectory(szStandalone); szStandalone[0] = '\0'; return false;
		}
		strcpy(szFile, szStandalone);
		fTempFile = true;
		// fallthru
	}

	// doesn't exist physically?
	if (!FileExists(szStandalone))
	{
		// try C4Group (might be packed)
		if (!pParent->FindTempResFileName(szFile, szStandalone))
		{
			if (!fSilent) pParent->logger->error("GetStandalone: could not find free name for temporary file!"); szStandalone[0] = '\0'; return false;
		}
		if (!C4Group_CopyItem(szFile, szStandalone))
		{
			if (!fSilent) pParent->logger->error("GetStandalone: could not copy to temporary file!"); szStandalone[0] = '\0'; return false;
		}
	}

	// remains missing? give up.
	if (!FileExists(szStandalone))
	{
		if (!fSilent) pParent->logger->error("GetStandalone: file not found!"); szStandalone[0] = '\0'; return false;
	}

	// do optimizations (delete unneeded entries)
	if (!OptimizeStandalone(fSilent))
	{
		if (!SEqual(szFile, szStandalone)) remove(szStandalone); szStandalone[0] = '\0'; return false;
	}

	// get file size
	size_t iSize = FileSize(szStandalone);
	// size limit
	if (fAllowUnloadable)
		if (iSize > uint32_t(Config.Network.MaxLoadFileSize))
		{
			if (!fSilent) pParent->logger->info("{} over size limit, will be marked unloadable!", szFile); szStandalone[0] = '\0'; return false;
		}
	// check
	if (!fSetOfficial && iSize != Core.getFileSize())
	{
		// remove file
		if (!SEqual(szFile, szStandalone)) remove(szStandalone); szStandalone[0] = '\0';
		// sorry, this version isn't good enough :(
		return false;
	}

	// calc checksum
	uint32_t iCRC32;
	if (!C4Group_GetFileCRC(szStandalone, &iCRC32))
	{
		if (!fSilent) pParent->logger->error("GetStandalone: could not calculate checksum!"); return false;
	}
	// set / check
	if (!fSetOfficial && iCRC32 != Core.getFileCRC())
	{
		// remove file, return
		if (!SEqual(szFile, szStandalone)) remove(szStandalone); szStandalone[0] = '\0';
		return false;
	}

	// we didn't fail
	fStandaloneFailed = false;
	// mark resource as loadable and safe file information
	Core.SetLoadable(iSize, iCRC32);
	// set up chunk data
	Chunks.SetComplete(Core.getChunkCnt());
	// ok
	return true;
}

bool C4Network2Res::CalculateSHA()
{
	// already present?
	if (Core.hasFileSHA()) return true;
	// get the file
	char szStandalone[_MAX_PATH + 1];
	if (!GetStandalone(szStandalone, _MAX_PATH, false))
		SCopy(szFile, szStandalone, _MAX_PATH);
	// get the hash
	uint8_t hash[StdSha1::DigestLength];
	if (!C4Group_GetFileSHA1(szStandalone, hash))
		return false;
	// save it back
	Core.SetFileSHA(hash);
	// okay
	return true;
}

C4Network2Res::Ref C4Network2Res::Derive()
{
	// Called before the file is changed. Rescues all files and creates a
	// new ressource for the file. This ressource is flagged as "anonymous", as it
	// has no official core (no res ID, to be exact).
	// The resource gets its final core when FinishDerive() is called.

	// For security: This doesn't make much sense if the resource is currently being
	// loaded. So better assume the caller doesn't know what he's doing and check.
	if (isLoading()) return nullptr;

	CStdLock FileLock(&FileCSec);
	// Save back original file name
	char szOrgFile[_MAX_PATH + 1];
	SCopy(szFile, szOrgFile, _MAX_PATH);
	bool fOrgTempFile = fTempFile;

	// Create a copy of the file, if neccessary
	if (!*szStandalone || SEqual(szStandalone, szFile))
	{
		if (!pParent->FindTempResFileName(szOrgFile, szFile))
		{
			pParent->logger->error("Derive: could not find free name for temporary file!"); return nullptr;
		}
		if (!C4Group_CopyItem(szOrgFile, szFile))
		{
			pParent->logger->error("Derive: could not copy to temporary file!"); return nullptr;
		}
		// set standalone
		if (*szStandalone)
			SCopy(szFile, szStandalone, _MAX_PATH);
		fTempFile = true;
	}
	else
	{
		// Standlone exists: Just set szFile to point on the standlone. It's
		// assumed that the original file isn't of intrest after this point anyway.
		SCopy(szStandalone, szFile, _MAX_PATH);
		fTempFile = true;
	}

	pParent->logger->info("Resource: deriving from {}:{}, original at {}", getResID(), Core.getFileName(), szFile);

	// (note: should remove temp file if something fails after this point)

	// create new ressource
	C4Network2Res::Ref pDRes = new C4Network2Res(pParent);
	if (!pDRes) return nullptr;

	// initialize
	if (!pDRes->SetDerived(Core.getFileName(), szOrgFile, fOrgTempFile, getType(), getResID()))
		return nullptr;

	// add to list
	pParent->Add(pDRes);

	// return new ressource
	return pDRes;
}

bool C4Network2Res::FinishDerive() // by main thread
{
	// All changes have been made. Register this ressource with a new ID.

	// security
	if (!isAnonymous()) return false;

	CStdLock FileLock(&FileCSec);
	// Save back data
	int32_t iDerID = Core.getDerID();
	char szName[_MAX_PATH + 1]; SCopy(Core.getFileName(), szName, _MAX_PATH);
	char szFileC[_MAX_PATH + 1]; SCopy(szFile, szFileC, _MAX_PATH);
	// Set by file
	if (!SetByFile(szFileC, fTempFile, getType(), pParent->nextResID(), szName))
		return false;
	// create standalone
	if (!GetStandalone(nullptr, 0, true))
		return false;
	// Set ID
	Core.SetDerived(iDerID);
	// announce derive
	pParent->getIOClass()->BroadcastMsg(MkC4NetIOPacket(PID_NetResDerive, Core));
	// derivation is dirty bussines
	fDirty = true;
	// ok
	return true;
}

bool C4Network2Res::FinishDerive(const C4Network2ResCore &nCore)
{
	// security
	if (!isAnonymous()) return false;
	// Set core
	Core = nCore;
	// Set chunks (assume the ressource is complete)
	Chunks.SetComplete(Core.getChunkCnt());

	// Note that the Contents-CRC is /not/ checked. Derivation needs to be
	// synchronized outside of C4Network2Res.

	// But note that the ressource /might/ be binary compatible (though very
	// unlikely), so do not set fNotBinaryCompatible.

	// ok
	return true;
}

void C4Network2Res::Remove()
{
	// schedule for removal
	fRemoved = true;
}

bool C4Network2Res::SendStatus(C4Network2IOConnection *pTo)
{
	// pack status
	C4NetIOPacket Pkt = MkC4NetIOPacket(PID_NetResStat, C4PacketResStatus(Core.getID(), Chunks));
	// to one client?
	if (pTo)
		return pTo->Send(Pkt);
	else
	{
		// reset dirty flag
		fDirty = false;
		// broadcast status
		assert(pParent && pParent->getIOClass());
		return pParent->getIOClass()->BroadcastMsg(Pkt);
	}
}

bool C4Network2Res::SendChunk(uint32_t iChunk, int32_t iToClient)
{
	assert(pParent && pParent->getIOClass());
	if (!szStandalone[0] || iChunk >= Core.getChunkCnt()) return false;
	// find connection for given client (one of the rare uses of the data connection)
	C4Network2IOConnection *pConn = pParent->getIOClass()->GetDataConnection(iToClient);
	if (!pConn) return false;
	// save last request time
	iLastReqTime = time(nullptr);
	// create packet
	CStdLock FileLock(&FileCSec);
	C4Network2ResChunk ResChunk;
	ResChunk.Set(this, iChunk);
	// send
	bool fSuccess = pConn->Send(MkC4NetIOPacket(PID_NetResData, ResChunk));
	pConn->DelRef();
	return fSuccess;
}

void C4Network2Res::AddRef()
{
	++iRefCnt;
}

void C4Network2Res::DelRef()
{
	if (--iRefCnt == 0) delete this;
}

void C4Network2Res::OnDiscover(C4Network2IOConnection *pBy)
{
	if (!IsBinaryCompatible()) return;
	// discovered
	iLastReqTime = time(nullptr);
	// send status back
	SendStatus(pBy);
}

void C4Network2Res::OnStatus(const C4Network2ResChunkData &rChunkData, C4Network2IOConnection *pBy)
{
	// discovered a source: reset timeout
	iDiscoverStartTime = 0;
	// check if the chunk data is valid
	if (rChunkData.getChunkCnt() != Chunks.getChunkCnt())
		return;
	// add chunk data
	ClientChunks *pChunks;
	for (pChunks = pCChunks; pChunks; pChunks = pChunks->Next)
		if (pChunks->ClientID == pBy->getClientID())
			break;
	// not found? add
	if (!pChunks)
	{
		pChunks = new ClientChunks();
		pChunks->Next = pCChunks;
		pCChunks = pChunks;
	}
	pChunks->ClientID = pBy->getClientID();
	pChunks->Chunks = rChunkData;
	// load?
	if (fLoading) StartLoad(pChunks->ClientID, pChunks->Chunks);
}

void C4Network2Res::OnChunk(const C4Network2ResChunk &rChunk)
{
	if (!fLoading) return;
	// correct ressource?
	if (rChunk.getResID() != getResID()) return;
	// add ressource data
	CStdLock FileLock(&FileCSec);
	bool fSuccess = rChunk.AddTo(this, pParent->getIOClass());
#ifdef C4NET2RES_DEBUG_LOG
	// log
	pParent->logger->trace("Res: {} chunk {} to resource {} ({}){}", fSuccess ? "added" : "could not add", rChunk.getChunkNr(), Core.getFileName(), szFile, fSuccess ? "" : "!");
#endif
	if (fSuccess)
	{
		// status changed
		fDirty = true;
		// remove load waits
		for (C4Network2ResLoad *pLoad = pLoads, *pNext, *pPrev = nullptr; pLoad; pPrev = pLoad, pLoad = pNext)
		{
			pNext = pLoad->Next();
			if (pLoad->getChunk() == rChunk.getChunkNr())
				RemoveLoad(pLoad);
		}
	}
	// complete?
	if (Chunks.isComplete())
		EndLoad();
	// check: start new loads?
	else
		StartNewLoads();
}

bool C4Network2Res::DoLoad()
{
	if (!fLoading) return true;
	// any loads currently active?
	if (iLoadCnt)
	{
		// check for load timeouts
		int32_t iLoadsRemoved = 0;
		for (C4Network2ResLoad *pLoad = pLoads, *pNext; pLoad; pLoad = pNext)
		{
			pNext = pLoad->Next();
			if (pLoad->CheckTimeout())
			{
				RemoveLoad(pLoad);
				iLoadsRemoved++;
			}
		}
		// start new loads
		if (iLoadsRemoved) StartNewLoads();
	}
	else
	{
		// discover timeout?
		if (iDiscoverStartTime)
			if (difftime(time(nullptr), iDiscoverStartTime) > C4NetResDiscoverTimeout)
				return false;
	}
	// ok
	return true;
}

bool C4Network2Res::NeedsDiscover()
{
	// set timeout, if this is the first discover
	if (!iDiscoverStartTime)
		iDiscoverStartTime = time(nullptr);
	// do discover
	return true;
}

void C4Network2Res::Clear()
{
	CStdLock FileLock(&FileCSec);
	// delete files
	if (fTempFile)
		if (FileExists(szFile))
			if (remove(szFile))
				pParent->logger->error("Could not delete temporary resource file ({})", strerror(errno));
	if (szStandalone[0] && !SEqual(szFile, szStandalone))
		if (FileExists(szStandalone))
			if (remove(szStandalone))
				pParent->logger->error("Could not delete temporary resource file ({})", strerror(errno));
	szFile[0] = szStandalone[0] = '\0';
	fDirty = false;
	fTempFile = false;
	Core.Clear();
	Chunks.Clear();
	fRemoved = false;
	ClearLoad();
}

int32_t C4Network2Res::OpenFileRead()
{
	CStdLock FileLock(&FileCSec);
	if (!GetStandalone(nullptr, 0, false, false, true)) return -1;
	return open(szStandalone, _O_BINARY | O_RDONLY);
}

int32_t C4Network2Res::OpenFileWrite()
{
	CStdLock FileLock(&FileCSec);
	return open(szStandalone, _O_BINARY | O_CREAT | O_WRONLY, S_IREAD | S_IWRITE);
}

void C4Network2Res::StartNewLoads()
{
	if (!pCChunks) return;
	// count clients
	int32_t iCChunkCnt = 0; ClientChunks *pChunks;
	for (pChunks = pCChunks; pChunks; pChunks = pChunks->Next)
		iCChunkCnt++;
	// create array
	ClientChunks **pC = new ClientChunks *[iCChunkCnt];
	// initialize
	int32_t i;
	for (i = 0; i < iCChunkCnt; i++) pC[i] = nullptr;
	// create shuffled order
	for (pChunks = pCChunks, i = 0; i < iCChunkCnt; i++, pChunks = pChunks->Next)
	{
		// determine position
		int32_t iPos = SafeRandom(iCChunkCnt - i);
		// find & set
		for (int32_t j = 0; ; j++)
			if (!pC[j] && !iPos--)
			{
				pC[j] = pChunks;
				break;
			}
	}
	// start new load until maximum count reached
	while (iLoadCnt + 1 <= C4NetResMaxLoad)
	{
		int32_t ioLoadCnt = iLoadCnt;
		// search someone
		for (i = 0; i < iCChunkCnt; i++)
			if (pC[i])
			{
				// try to start load
				if (!StartLoad(pC[i]->ClientID, pC[i]->Chunks))
				{
					pC[i] = nullptr; continue;
				}
				// success?
				if (iLoadCnt > ioLoadCnt) break;
			}
		// not found?
		if (i >= iCChunkCnt)
			break;
	}
	// clear up
	delete[] pC;
}

bool C4Network2Res::StartLoad(int32_t iFromClient, const C4Network2ResChunkData &Available)
{
	assert(pParent && pParent->getIOClass());
	// all slots used? ignore
	if (iLoadCnt + 1 >= C4NetResMaxLoad) return true;
	int32_t loadsAtClient = 0;
	// are there already enough loads by this client? ignore
	for (C4Network2ResLoad *pPos = pLoads; pPos; pPos = pPos->Next())
	{
		if (pPos->getByClient() == iFromClient)
		{
			if (++loadsAtClient >= C4NetResMaxLoadPerPeerPerFile)
				return true;
		}
	}
	// find chunk to retrieve
	int32_t iLoads[C4NetResMaxLoad]; int32_t i = 0;
	for (C4Network2ResLoad *pLoad = pLoads; pLoad; pLoad = pLoad->Next())
		iLoads[i++] = pLoad->getChunk();
	int32_t iRetrieveChunk = Chunks.GetChunkToRetrieve(Available, i, iLoads);
	// nothing? ignore
	if (iRetrieveChunk < 0 || static_cast<uint32_t>(iRetrieveChunk) >= Core.getChunkCnt())
		return true;
	// search message connection for client
	C4Network2IOConnection *pConn = pParent->getIOClass()->GetMsgConnection(iFromClient);
	if (!pConn) return false;
	// send request
	if (!pConn->Send(MkC4NetIOPacket(PID_NetResReq, C4PacketResRequest(Core.getID(), iRetrieveChunk))))
	{
		pConn->DelRef(); return false;
	}
	pConn->DelRef();
#ifdef C4NET2RES_DEBUG_LOG
	// log
	pParent->logger->trace("Res: requesting chunk {} of {}:{} ({}) from client {}",
		iRetrieveChunk, Core.getID(), Core.getFileName(), szFile, iFromClient);
#endif
	// create load class
	C4Network2ResLoad *pnLoad = new C4Network2ResLoad(iRetrieveChunk, iFromClient);
	// add to list
	pnLoad->pNext = pLoads;
	pLoads = pnLoad;
	iLoadCnt++;
	// ok
	return true;
}

void C4Network2Res::EndLoad()
{
	// clear loading data
	ClearLoad();
	// set complete
	fLoading = false;
	// call handler
	assert(pParent);
	pParent->OnResComplete(this);
}

void C4Network2Res::ClearLoad()
{
	// remove client chunks and loads
	fLoading = false;
	while (pCChunks) RemoveCChunks(pCChunks);
	while (pLoads) RemoveLoad(pLoads);
	iDiscoverStartTime = iLoadCnt = 0;
}

void C4Network2Res::RemoveLoad(C4Network2ResLoad *pLoad)
{
	if (pLoad == pLoads)
		pLoads = pLoad->Next();
	else
	{
		// find previous entry
		C4Network2ResLoad *pPrev;
		for (pPrev = pLoads; pPrev && pPrev->Next() != pLoad; pPrev = pPrev->Next());
		// remove
		if (pPrev)
			pPrev->pNext = pLoad->Next();
	}
	// delete
	delete pLoad;
	iLoadCnt--;
}

void C4Network2Res::RemoveCChunks(ClientChunks *pChunks)
{
	if (pChunks == pCChunks)
		pCChunks = pChunks->Next;
	else
	{
		// find previous entry
		ClientChunks *pPrev;
		for (pPrev = pCChunks; pPrev && pPrev->Next != pChunks; pPrev = pPrev->Next);
		// remove
		if (pPrev)
			pPrev->Next = pChunks->Next;
	}
	// delete
	delete pChunks;
}

bool C4Network2Res::OptimizeStandalone(bool fSilent)
{
	CStdLock FileLock(&FileCSec);
	// for now: player files only
	if (Core.getType() == NRT_Player)
	{
		// log - this may take a few seconds
		if (!fSilent) Log(C4ResStrTableKey::IDS_PRC_NETPREPARING, GetFilename(szFile));
		// copy to temp file, if needed
		if (!fTempFile && SEqual(szFile, szStandalone))
		{
			char szNewStandalone[_MAX_PATH + 1];
			if (!pParent->FindTempResFileName(szStandalone, szNewStandalone))
			{
				if (!fSilent) pParent->logger->error("OptimizeStandalone: could not find free name for temporary file!"); return false;
			}
			if (!C4Group_CopyItem(szStandalone, szNewStandalone))
			{
				if (!fSilent) pParent->logger->error("OptimizeStandalone: could not copy to temporary file!"); return false;
			} /* TODO: Test failure */
			SCopy(szNewStandalone, szStandalone, sizeof(szStandalone) - 1);
		}
		// open as group
		C4Group Grp;
		if (!Grp.Open(szStandalone))
		{
			if (!fSilent) pParent->logger->error("OptimizeStandalone: could not open player file!"); return false;
		}
		// remove portrais
		Grp.Delete(C4CFN_Portraits, true);
		// remove bigicon, if the file size is too large
		size_t iBigIconSize = 0;
		if (Grp.FindEntry(C4CFN_BigIcon, nullptr, &iBigIconSize))
			if (iBigIconSize > C4NetResMaxBigicon * 1024)
				Grp.Delete(C4CFN_BigIcon);
		Grp.Close();
	}
	return true;
}

bool C4Network2Res::GetClientProgress(int32_t clientID, int32_t &presentChunkCnt, int32_t &chunkCnt)
{
	// Try to find chunks for client ID
	ClientChunks *chunks;
	for (chunks = pCChunks; chunks; chunks = chunks->Next)
	{
		if (chunks->ClientID == clientID) break;
	}

	if (!chunks) return false; // Not found?

	presentChunkCnt = chunks->Chunks.getPresentChunkCnt();
	chunkCnt = Chunks.getChunkCnt();
	return true;
}

// *** C4Network2ResChunk

C4Network2ResChunk::C4Network2ResChunk() {}

C4Network2ResChunk::~C4Network2ResChunk() {}

bool C4Network2ResChunk::Set(C4Network2Res *pRes, uint32_t inChunk)
{
	const auto &logger = pRes->pParent->GetLogger();
	const C4Network2ResCore &Core = pRes->getCore();
	iResID = pRes->getResID();
	iChunk = inChunk;
	// calculate offset and size
	int32_t iOffset = iChunk * Core.getChunkSize(),
		iSize = std::min<int32_t>(Core.getFileSize() - iOffset, C4NetResChunkSize);
	if (iSize < 0) { logger->error("could not get chunk from offset {} from resource file {}: File size is only {}!", iOffset, pRes->getFile(), Core.getFileSize()); return false; }
	// open file
	int32_t f = pRes->OpenFileRead();
	if (f == -1) { logger->error("could not open resource file {}!", pRes->getFile()); return false; }
	// seek
	if (iOffset)
		if (lseek(f, iOffset, SEEK_SET) != iOffset)
		{
			close(f); logger->error("could not read resource file {}!", pRes->getFile()); return false;
		}
	// read chunk of data
	char *pBuf = static_cast<char *>(malloc(sizeof(char) * iSize));
	if (read(f, pBuf, iSize) != iSize)
	{
		free(pBuf); close(f); logger->error("could not read resource file {}!", pRes->getFile()); return false;
	}
	// set
	Data.Take(pBuf, iSize);
	// close
	close(f);
	// ok
	return true;
}

bool C4Network2ResChunk::AddTo(C4Network2Res *pRes, C4Network2IO *pIO) const
{
	assert(pRes); assert(pIO);
#ifdef C4NET2RES_DEBUG_LOG
	const auto &logger = pRes->pParent->GetLogger();
#endif
	const C4Network2ResCore &Core = pRes->getCore();
	// check
	if (iResID != pRes->getResID())
	{
#ifdef C4NET2RES_DEBUG_LOG
		logger->trace("C4Network2ResChunk({})::AddTo({} [{}]): Ressource ID mismatch!", iResID, Core.getFileName(), pRes->getResID());
#endif
		return false;
	}
	// calculate offset and size
	int32_t iOffset = iChunk * Core.getChunkSize();
	if (iOffset + Data.getSize() > Core.getFileSize())
	{
#ifdef C4NET2RES_DEBUG_LOG
		logger->trace("C4Network2ResChunk({})::AddTo({} [{}]): Adding {} bytes at offset {} exceeds expected file size of {}!", iResID, Core.getFileName(), pRes->getResID(), Data.getSize(), iOffset, Core.getFileSize());
#endif
		return false;
	}
	// open file
	int32_t f = pRes->OpenFileWrite();
	if (f == -1)
	{
#ifdef C4NET2RES_DEBUG_LOG
		logger->trace("C4Network2ResChunk({})::AddTo({} [{}]): Open write file error: {}!", iResID, Core.getFileName(), pRes->getResID(), strerror(errno));
#endif
		return false;
	}
	// seek
	if (iOffset)
		if (lseek(f, iOffset, SEEK_SET) != iOffset)
		{
#ifdef C4NET2RES_DEBUG_LOG
			logger->trace("C4Network2ResChunk({})::AddTo({} [{}]): lseek file error: {}!", iResID, Core.getFileName(), pRes->getResID(), strerror(errno));
#endif
			close(f);
			return false;
		}
	// write
	if (write(f, Data.getData(), Data.getSize()) != int32_t(Data.getSize()))
	{
#ifdef C4NET2RES_DEBUG_LOG
		logger->trace("C4Network2ResChunk({})::AddTo({} [{}]): write error: {}!", iResID, Core.getFileName(), pRes->getResID(), strerror(errno));
#endif
		close(f);
		return false;
	}
	// ok, add chunks
	close(f);
	pRes->Chunks.AddChunk(iChunk);
	return true;
}

void C4Network2ResChunk::CompileFunc(StdCompiler *pComp)
{
	// pack header
	pComp->Value(mkNamingAdapt(iResID, "ResID", -1));
	pComp->Value(mkNamingAdapt(iChunk, "Chunk", ~0U));
	// Data
	pComp->Value(mkNamingAdapt(Data, "Data"));
}

// *** C4Network2ResList

C4Network2ResList::C4Network2ResList()
	: iClientID(-1),
	iNextResID((-1) << 16),
	pFirst(nullptr),
	ResListCSec(this),
	iLastDiscover(0), iLastStatus(0),
	pIO(nullptr) {}

C4Network2ResList::~C4Network2ResList()
{
	Clear();
}

bool C4Network2ResList::Init(std::shared_ptr<spdlog::logger> logger, int32_t inClientID, C4Network2IO *pIOClass) // by main thread
{
	// clear old list
	Clear();
	this->logger = std::move(logger);
	// safe IO class
	pIO = pIOClass;
	// set client id
	iNextResID = iClientID = 0;
	SetLocalID(inClientID);
	// create network path
	if (!CreateNetworkFolder()) return false;
	// ok
	return true;
}

void C4Network2ResList::SetLocalID(int32_t inClientID)
{
	CStdLock ResIDLock(&ResIDCSec);
	int32_t iOldClientID = iClientID;
	int32_t iIDDiff = (inClientID - iClientID) << 16;
	// set new counter
	iClientID = inClientID;
	iNextResID += iIDDiff;
	// change ressource ids
	CStdLock ResListLock(&ResListCSec);
	for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
		if (pRes->getResClient() == iOldClientID)
			pRes->ChangeID(pRes->getResID() + iIDDiff);
}

int32_t C4Network2ResList::nextResID() // by main thread
{
	CStdLock ResIDLock(&ResIDCSec);
	assert(iNextResID >= (iClientID << 16));
	if (iNextResID >= ((iClientID + 1) << 16) - 1)
		iNextResID = std::max<int32_t>(0, iClientID) << 16;
	// find free
	while (getRes(iNextResID))
		iNextResID++;
	return iNextResID++;
}

C4Network2Res *C4Network2ResList::getRes(int32_t iResID)
{
	CStdShareLock ResListLock(&ResListCSec);
	for (C4Network2Res *pCur = pFirst; pCur; pCur = pCur->pNext)
		if (pCur->getResID() == iResID)
			return pCur;
	return nullptr;
}

C4Network2Res *C4Network2ResList::getRes(const char *szFile, bool fLocalOnly)
{
	CStdShareLock ResListLock(&ResListCSec);
	for (C4Network2Res *pCur = pFirst; pCur; pCur = pCur->pNext)
		if (!pCur->isAnonymous())
			if (SEqual(pCur->getFile(), szFile))
				if (!fLocalOnly || pCur->getResClient() == iClientID)
					return pCur;
	return nullptr;
}

C4Network2Res::Ref C4Network2ResList::getRefRes(int32_t iResID)
{
	CStdShareLock ResListLock(&ResListCSec);
	return getRes(iResID);
}

C4Network2Res::Ref C4Network2ResList::getRefRes(const char *szFile, bool fLocalOnly)
{
	CStdShareLock ResListLock(&ResListCSec);
	return getRes(szFile, fLocalOnly);
}

C4Network2Res::Ref C4Network2ResList::getRefNextRes(int32_t iResID)
{
	CStdShareLock ResListLock(&ResListCSec);
	C4Network2Res *pRes = nullptr;
	for (C4Network2Res *pCur = pFirst; pCur; pCur = pCur->pNext)
		if (!pCur->isRemoved() && pCur->getResID() >= iResID)
			if (!pRes || pRes->getResID() > pCur->getResID())
				pRes = pCur;
	return pRes;
}

void C4Network2ResList::Add(C4Network2Res *pRes)
{
	// get locks
	CStdShareLock ResListLock(&ResListCSec);
	CStdLock ResListAddLock(&ResListAddCSec);
	// reference
	pRes->AddRef();
	// add
	pRes->pNext = pFirst;
	pFirst = pRes;
}

C4Network2Res::Ref C4Network2ResList::AddByFile(const char *strFilePath, bool fTemp, C4Network2ResType eType, int32_t iResID, const char *szResName, bool fAllowUnloadable)
{
	// already in list?
	if (C4Network2Res::Ref pRes = getRefRes(strFilePath); pRes)
	{
		return pRes;
	}

	// get ressource ID
	if (iResID < 0) iResID = nextResID();
	if (iResID < 0) { logger->error("AddByFile: no more ressource IDs available!"); return nullptr; }
	// create new
	auto res = std::make_unique<C4Network2Res>(this);
	// initialize
	if (!res->SetByFile(strFilePath, fTemp, eType, iResID, szResName)) { return nullptr; }
	// create standalone for non-system files
	// system files shouldn't create a standalone; they should never be marked loadable!
	if (eType != NRT_System)
		if (!res->GetStandalone(nullptr, 0, true, fAllowUnloadable))
			if (!fAllowUnloadable)
			{
				return nullptr;
			}

	// add to list
	const auto resPtr = res.release();
	Add(resPtr);
	return resPtr;
}

C4Network2Res::Ref C4Network2ResList::AddByCore(const C4Network2ResCore &Core, bool fLoad) // by main thread
{
	// already in list?
	C4Network2Res::Ref pRes = getRefRes(Core.getID());
	if (pRes) return pRes;
#ifdef C4NET2RES_LOAD_ALL
	// load without check (if possible)
	if (Core.isLoadable()) return AddLoad(Core);
#endif
	// create new
	pRes = new C4Network2Res(this);
	// try set by core
	if (!pRes->SetByCore(Core, true))
	{
		pRes.Clear();
		// try load (if specified)
		return fLoad ? AddLoad(Core) : nullptr;
	}
	// log
	logger->info("Found identical {}. Not loading.", pRes->getCore().getFileName());
	// add to list
	Add(pRes);
	// ok
	return pRes;
}

C4Network2Res::Ref C4Network2ResList::AddLoad(const C4Network2ResCore &Core) // by main thread
{
	// marked unloadable by creator?
	if (!Core.isLoadable())
	{
		// show error msg
		logger->error("Cannot load {} (marked unloadable)", Core.getFileName());
		return nullptr;
	}
	// create new
	C4Network2Res::Ref pRes = new C4Network2Res(this);
	// initialize
	pRes->SetLoad(Core);
	// log
	logger->info("loading {}...", Core.getFileName());
	// add to list
	Add(pRes);
	return pRes;
}

void C4Network2ResList::RemoveAtClient(int32_t iClientID) // by main thread
{
	CStdShareLock ResListLock(&ResListCSec);
	for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
		if (pRes->getResClient() == iClientID)
			pRes->Remove();
}

void C4Network2ResList::Clear()
{
	CStdShareLock ResListLock(&ResListCSec);
	for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
	{
		pRes->Remove();
		pRes->iLastReqTime = 0;
	}
	iClientID = C4ClientIDUnknown;
	iLastDiscover = iLastStatus = 0;

	// Make sure the logger is not reset as OnShareFree() and
	// C4GameRes::~C4GameRes() will destroy the C4Network2Res objects
}

void C4Network2ResList::ClearLogger()
{
	assert(iClientID == C4ClientIDUnknown);
	logger.reset();
}

void C4Network2ResList::OnClientConnect(C4Network2IOConnection *pConn) // by main thread
{
	// discover ressources
	SendDiscover(pConn);
}

void C4Network2ResList::HandlePacket(char cStatus, const C4PacketBase *pPacket, C4Network2IOConnection *pConn)
{
	// security
	if (!pConn) return;

#define GETPKT(type, name) \
	assert(pPacket); \
	const type &name = static_cast<const type &>(*pPacket);

	switch (cStatus)
	{
	case PID_NetResDis: // ressource discover
	{
		if (!pConn->isOpen()) break;
		GETPKT(C4PacketResDiscover, Pkt);
		// search matching ressources
		CStdShareLock ResListLock(&ResListCSec);
		for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
			if (Pkt.isIDPresent(pRes->getResID()))
				// must be binary compatible
				if (pRes->IsBinaryCompatible())
					pRes->OnDiscover(pConn);
	}
	break;

	case PID_NetResStat: // ressource status
	{
		if (!pConn->isOpen()) break;
		GETPKT(C4PacketResStatus, Pkt);
		// matching ressource?
		CStdShareLock ResListLock(&ResListCSec);
		C4Network2Res *pRes = getRes(Pkt.getResID());
		// present / being loaded? call handler
		if (pRes)
			pRes->OnStatus(Pkt.getChunks(), pConn);
	}
	break;

	case PID_NetResDerive: // ressource derive
	{
		GETPKT(C4Network2ResCore, Core);
		if (Core.getDerID() < 0) break;
		// Check if there is a anonymous derived ressource with matching parent.
		CStdShareLock ResListLock(&ResListCSec);
		for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
			if (pRes->isAnonymous() && pRes->getCore().getDerID() == Core.getDerID())
				pRes->FinishDerive(Core);
	}
	break;

	case PID_NetResReq: // ressource request
	{
		GETPKT(C4PacketResRequest, Pkt);
		// find ressource
		CStdShareLock ResListLock(&ResListCSec);
		C4Network2Res *pRes = getRes(Pkt.getReqID());
		// send requested chunk
		if (pRes && pRes->IsBinaryCompatible()) pRes->SendChunk(Pkt.getReqChunk(), pConn->getClientID());
	}
	break;

	case PID_NetResData: // a chunk of data is coming in
	{
		GETPKT(C4Network2ResChunk, Chunk);
		// find ressource
		CStdShareLock ResListLock(&ResListCSec);
		C4Network2Res *pRes = getRes(Chunk.getResID());
		// send requested chunk
		if (pRes) pRes->OnChunk(Chunk);
	}
	break;
	}
#undef GETPKT
}

void C4Network2ResList::OnTimer()
{
	CStdShareLock ResListLock(&ResListCSec);
	C4Network2Res *pRes;
	// do loads, check timeouts
	for (pRes = pFirst; pRes; pRes = pRes->pNext)
		if (pRes->isLoading() && !pRes->isRemoved())
			if (!pRes->DoLoad())
				pRes->Remove();
	// discovery time?
	if (!iLastDiscover || difftime(time(nullptr), iLastDiscover) >= C4NetResDiscoverInterval)
	{
		// needed?
		bool fSendDiscover = false;
		for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
			if (!pRes->isRemoved())
				fSendDiscover |= pRes->NeedsDiscover();
		// send
		if (fSendDiscover)
			SendDiscover();
	}
	// status update?
	if (!iLastStatus || difftime(time(nullptr), iLastStatus) >= C4NetResStatusInterval)
	{
		// any?
		bool fStatusUpdates = false;
		for (pRes = pFirst; pRes; pRes = pRes->pNext)
			if (pRes->isDirty() && !pRes->isRemoved())
				fStatusUpdates |= pRes->SendStatus();
		// set time accordingly
		iLastStatus = fStatusUpdates ? time(nullptr) : 0;
	}
}

void C4Network2ResList::OnShareFree(CStdCSecEx *pCSec)
{
	if (pCSec == &ResListCSec)
	{
		// remove entries
		for (C4Network2Res *pRes = pFirst, *pNext, *pPrev = nullptr; pRes; pRes = pNext)
		{
			pNext = pRes->pNext;
			if (pRes->isRemoved() && (!pRes->getLastReqTime() || difftime(time(nullptr), pRes->getLastReqTime()) > C4NetResDeleteTime))
			{
				// unlink
				(pPrev ? pPrev->pNext : pFirst) = pNext;
				// remove
				pRes->pNext = nullptr;
				pRes->DelRef();
			}
			else
				pPrev = pRes;
		}
	}
}

bool C4Network2ResList::SendDiscover(C4Network2IOConnection *pTo) // by both
{
	// make packet
	C4PacketResDiscover Pkt;
	// add special retrieves
	CStdShareLock ResListLock(&ResListCSec);
	for (C4Network2Res *pRes = pFirst; pRes; pRes = pRes->pNext)
		if (!pRes->isRemoved())
			Pkt.AddDisID(pRes->getResID());
	ResListLock.Clear();
	// empty?
	if (!Pkt.getDisIDCnt()) return false;
	// broadcast?
	if (!pTo)
	{
		// save time
		iLastDiscover = time(nullptr);
		// send
		return pIO->BroadcastMsg(MkC4NetIOPacket(PID_NetResDis, Pkt));
	}
	else
		return pTo->Send(MkC4NetIOPacket(PID_NetResDis, Pkt));
}

void C4Network2ResList::OnResComplete(C4Network2Res *pRes)
{
	// log
	logger->info("{} received.", pRes->getCore().getFileName());
	// call handler (ctrl might wait for this ressource)
	Game.Control.Network.OnResComplete(pRes);
}

bool C4Network2ResList::CreateNetworkFolder()
{
	// get network path without trailing backslash
	char szNetworkPath[_MAX_PATH + 1];
	SCopy(Config.Network.WorkPath, szNetworkPath, _MAX_PATH);
	TruncateBackslash(szNetworkPath);
	// but make sure that the configured path has one
	AppendBackslash(Config.Network.WorkPath);
	// does not exist?
	if (access(szNetworkPath, 00))
	{
		if (!MakeDirectory(szNetworkPath, nullptr))
		{
			LogFatalNTr("could not create network path!"); return false;
		}
		return true;
	}
	// stat
	struct stat s;
	if (stat(szNetworkPath, &s))
	{
		LogFatalNTr("could not stat network path!"); return false;
	}
	// not a subdir?
	if (!(s.st_mode & S_IFDIR))
	{
		LogFatalNTr("could not create network path: blocked by a file!"); return false;
	}
	// ok
	return true;
}

bool C4Network2ResList::FindTempResFileName(const char *szFilename, char *pTarget)
{
	static constexpr auto newFileCreated = [](const char *const filename)
	{
		FILE *const file{fopen(filename, "wxb")};
		if (file)
		{
			fclose(file);
		}

		return file != nullptr;
	};

	char safeFilename[_MAX_PATH];
	char *safePos = safeFilename;
	while (*szFilename)
	{
		if ((*szFilename >= 'a' && *szFilename <= 'z') ||
			(*szFilename >= 'A' && *szFilename <= 'Z') ||
			(*szFilename >= '0' && *szFilename <= '9') ||
			(*szFilename == '.') || (*szFilename == '/'))
			*safePos = *szFilename;
		else
			*safePos = '_';

		++safePos;
		++szFilename;
	}
	*safePos = 0;
	szFilename = safeFilename;

	// create temporary file
	SCopy(Config.AtNetworkPath(GetFilename(szFilename)), pTarget, _MAX_PATH);
	// file name is free?
	if (newFileCreated(pTarget)) return true;
	// find another file name
	char szFileMask[_MAX_PATH + 1];
	SCopy(pTarget, szFileMask, GetExtension(pTarget) - 1 - pTarget);
	char *const end{szFileMask + std::strlen(szFileMask) + 1};
	end[-1] = '_';

	for (int32_t i = 2; i < 1000; i++)
	{
		char *const extPtr{std::to_chars(end, szFileMask + std::size(szFileMask) - 1, i).ptr};
		SCopy(GetExtension(pTarget) - 1, extPtr, _MAX_PATH - (extPtr - szFileMask));
		SCopy(szFileMask, pTarget, _MAX_PATH);
		// doesn't exist?
		if (newFileCreated(pTarget))
			return true;
	}
	// not found
	return false;
}

int32_t C4Network2ResList::GetClientProgress(int32_t clientID)
{
	int32_t sumPresentChunkCnt = 0, sumChunkCnt = 0;
	CStdLock ResListLock(&ResListCSec);
	for (C4Network2Res *res = pFirst; res; res = res->pNext)
	{
		int32_t presentChunkCnt, chunkCnt;
		if (res->isRemoved() || !res->GetClientProgress(clientID, presentChunkCnt, chunkCnt)) continue;
		sumPresentChunkCnt += presentChunkCnt;
		sumChunkCnt += chunkCnt;
	}
	return sumChunkCnt == 0 ? 100 : sumPresentChunkCnt * 100 / sumChunkCnt;
}
