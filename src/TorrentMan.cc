/* <!-- copyright */
/*
 * aria2 - a simple utility for downloading files faster
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/* copyright --> */
#include "TorrentMan.h"
#include "Dictionary.h"
#include "List.h"
#include "ShaVisitor.h"
#include "Util.h"
#include "MetaFileUtil.h"
#include "DlAbortEx.h"
#include "File.h"
#include "message.h"
#include "PreAllocationDiskWriter.h"
#include "DefaultDiskWriter.h"
#include "MultiDiskWriter.h"
#include "prefs.h"
#include "CopyDiskAdaptor.h"
#include "DirectDiskAdaptor.h"
#include "MultiDiskAdaptor.h"
#include "LogFactory.h"
#include <errno.h>
#include <libgen.h>
#include <string.h>
#include <algorithm>

TorrentMan::TorrentMan():bitfield(NULL),
			 peerEntryIdCounter(0), cuidCounter(0),
			 downloadLength(0), uploadLength(0),
			 preDownloadLength(0), preUploadLength(0),
			 deltaDownloadLength(0), deltaUploadLength(0),
			 storeDir("."),
			 setupComplete(false),
			 halt(false),
			 interval(DEFAULT_ANNOUNCE_INTERVAL),
			 minInterval(DEFAULT_ANNOUNCE_MIN_INTERVAL),
			 complete(0), incomplete(0),
			 connections(0), trackers(0), diskAdaptor(NULL) {
  logger = LogFactory::getInstance();
}

TorrentMan::~TorrentMan() {
  if(bitfield != NULL) {
    delete bitfield;
  }
  for(Peers::iterator itr = peers.begin(); itr != peers.end(); itr++) {
    delete *itr;
  }
  if(diskAdaptor != NULL) {
    delete diskAdaptor;
  }
}

// TODO do not use this method in application code
void TorrentMan::updatePeers(const Peers& peers) {
  this->peers = peers;
}

bool TorrentMan::addPeer(Peer* peer, bool duplicate) {
  deleteOldErrorPeers(MAX_PEER_LIST_SIZE);
  if(duplicate) {
    for(Peers::iterator itr = peers.begin(); itr != peers.end(); itr++) {
      Peer* p = *itr;
      if(p->ipaddr == peer->ipaddr && p->port == peer->port && p->error > 0) {
	return false;
      }
    }
  } else {
    for(Peers::iterator itr = peers.begin(); itr != peers.end(); itr++) {
      Peer* p = *itr;
      if(p->ipaddr == peer->ipaddr && p->port == peer->port) {
	return false;
      }
    }
  }
  ++peerEntryIdCounter;
  peer->entryId = peerEntryIdCounter;
  peers.push_back(peer);
  return true;
}

/*
void TorrentMan::updatePeer(const Peer& peer) {
  for(Peers::iterator itr = peers.begin(); itr != peers.end(); itr++) {
    Peer& p = *itr;
    if(p.eid == peer.eid) {
      p = peer;
      break;
    }
  }
}
*/

bool TorrentMan::isPeerAvailable() const {
  return getPeer() != Peer::nullPeer;
}

int TorrentMan::deleteOldErrorPeers(int maxNum) {
  int counter = 0;
  for(Peers::iterator itr = peers.begin(); itr != peers.end();) {
    Peer* p = *itr;
    if(p->error >= MAX_PEER_ERROR && p->cuid == 0) {
      delete p;
      itr = peers.erase(itr);
      counter++;
      if(maxNum <= counter) {
	break;
      }
    } else {
      itr++;
    }
  }
  return counter;
}

Peer* TorrentMan::getPeer() const {
  if(connections > MAX_PEER_UPDATE) {
    return Peer::nullPeer;
  }
  for(Peers::const_iterator itr = peers.begin(); itr != peers.end(); itr++) {
    Peer* p = *itr;
    if(p->cuid == 0 && p->error < MAX_PEER_ERROR) {
      return p;
    }
  }
  return Peer::nullPeer;
}

bool TorrentMan::isEndGame() const {
  return bitfield->countMissingBlock() <= END_GAME_PIECE_NUM;
}

bool TorrentMan::hasMissingPiece(const Peer* peer) const {
  return bitfield->hasMissingPiece(peer->getBitfield(),
				   peer->getBitfieldLength());
}

int TorrentMan::getMissingPieceIndex(const Peer* peer) const {
  int index = -1;
  if(isEndGame()) {
    index = bitfield->getMissingIndex(peer->getBitfield(),
				      peer->getBitfieldLength());
  } else {
    index = bitfield->getMissingUnusedIndex(peer->getBitfield(),
					    peer->getBitfieldLength());
  }
  return index;
}

int TorrentMan::getMissingFastPieceIndex(const Peer* peer) const {
  int index = -1;
  if(peer->isFastExtensionEnabled() && peer->countFastSet() > 0) {
    BitfieldMan tempBitfield(pieceLength, totalLength);
    for(Integers::const_iterator itr = peer->getFastSet().begin();
	itr != peer->getFastSet().end(); itr++) {
      if(!hasPiece(*itr) && peer->hasPiece(*itr)) {
	tempBitfield.setBit(*itr);
      }
    }
    if(isEndGame()) {
      index = bitfield->getMissingIndex(tempBitfield.getBitfield(),
					tempBitfield.getBitfieldLength());
    } else {
      index = bitfield->getMissingUnusedIndex(tempBitfield.getBitfield(),
					      tempBitfield.getBitfieldLength());
    }
  }
  return index;
}

Piece TorrentMan::getMissingFastPiece(const Peer* peer) {
  int index = getMissingFastPieceIndex(peer);
  return checkOutPiece(index);
}

Piece TorrentMan::getMissingPiece(const Peer* peer) {
  int index = getMissingPieceIndex(peer);
  return checkOutPiece(index);
}

Piece TorrentMan::checkOutPiece(int index) {
  if(index == -1) {
    return Piece::nullPiece;
  }
  bitfield->setUseBit(index);

  Piece piece = findUsedPiece(index);
  if(Piece::isNull(piece)) {
    Piece piece(index, bitfield->getBlockLength(index));
    addUsedPiece(piece);
    return piece;
  } else {
    return piece;
  }
}

int TorrentMan::deleteUsedPiecesByFillRate(int fillRate, int toDelete) {
  int deleted = 0;
  for(UsedPieces::iterator itr = usedPieces.begin();
      itr != usedPieces.end() && deleted < toDelete;) {
    Piece& piece = *itr;
    if(!bitfield->isUseBitSet(piece.getIndex()) &&
       piece.countCompleteBlock() <= piece.countBlock()*(fillRate/100.0)) {
      logger->debug("Deleting used piece index=%d, fillRate(%%)=%d<=%d",
		    piece.getIndex(),
		    (piece.countCompleteBlock()*100)/piece.countBlock(),
		    fillRate);
      itr = usedPieces.erase(itr);
      deleted++;
    } else {
      itr++;
    }
  }
  return deleted;
}

void TorrentMan::reduceUsedPieces(int max) {
  int toDelete = usedPieces.size()-max;
  if(toDelete <= 0) {
    return;
  }
  int fillRate = 10;
  while(fillRate < 50) {
    int deleted = deleteUsedPiecesByFillRate(fillRate, toDelete);
    if(deleted == 0) {
      break;
    }
    toDelete -= deleted;
    fillRate += 10;
  }
}

void TorrentMan::addUsedPiece(const Piece& piece) {
  usedPieces.push_back(piece);
}

Piece TorrentMan::findUsedPiece(int index) const {
  for(UsedPieces::const_iterator itr = usedPieces.begin(); itr != usedPieces.end(); itr++) {
    const Piece& piece = *itr;
    if(piece.getIndex() == index) {
      return piece;
    }
  }
  return Piece::nullPiece;
}

void TorrentMan::deleteUsedPiece(const Piece& piece) {
  if(Piece::isNull(piece)) {
    return;
  }
  usedPieces.erase(std::remove(usedPieces.begin(), usedPieces.end(), piece));
}

void TorrentMan::completePiece(const Piece& piece) {
  if(Piece::isNull(piece)) {
    return;
  }
  if(!hasPiece(piece.getIndex())) {
    addDownloadLength(piece.getLength());
  }
  bitfield->setBit(piece.getIndex());
  bitfield->unsetUseBit(piece.getIndex());
  deleteUsedPiece(piece);
  if(!isEndGame()) {
    reduceUsedPieces(100);
  }
}

void TorrentMan::cancelPiece(const Piece& piece) {
  if(Piece::isNull(piece)) {
    return;
  }
  updatePiece(piece);
  bitfield->unsetUseBit(piece.getIndex());
  if(!isEndGame()) {
    if(piece.countCompleteBlock() == 0) {
      deleteUsedPiece(piece);
    }
  }
}

void TorrentMan::updatePiece(const Piece& piece) {
  if(Piece::isNull(piece)) {
    return;
  }
  UsedPieces::iterator itr = find(usedPieces.begin(), usedPieces.end(),
				  piece);
  if(itr != usedPieces.end()) {
    *itr = piece;
  }
}

void TorrentMan::syncPiece(Piece& piece) {
  if(Piece::isNull(piece)) {
    return;
  }
  UsedPieces::iterator itr = find(usedPieces.begin(), usedPieces.end(),
				  piece);
  if(itr != usedPieces.end()) {
    piece = *itr;
    return;
  } else {
    // hasPiece(piece.getIndex()) is true, then set all bit of
    // piece.bitfield to 1
    if(hasPiece(piece.getIndex())) {
      piece.setAllBlock();
    }
  }
}

void TorrentMan::initBitfield() {
  if(bitfield != NULL) {
    delete bitfield;
  }
  bitfield = new BitfieldMan(pieceLength, totalLength);
}

void TorrentMan::setBitfield(unsigned char* bitfield, int bitfieldLength) {
  if(this->bitfield == NULL) {
    initBitfield();
  }
  this->bitfield->setBitfield(bitfield, bitfieldLength);
}

bool TorrentMan::downloadComplete() const {
  return bitfield->isAllBitSet();
}

bool TorrentMan::hasAllPieces() const {
  return bitfield->getTotalLength() == downloadLength;
}

void TorrentMan::readFileEntry(FileEntries& fileEntries, Directory** pTopDir, const Dictionary* infoDic, const string& defaultName) {
  Data* topName = (Data*)infoDic->get("name");
  if(topName != NULL) {
    name = topName->toString();
  } else {
    char* basec = strdup(defaultName.c_str());
    name = string(basename(basec))+".file";
    free(basec);
  }
  List* files = (List*)infoDic->get("files");
  if(files == NULL) {
    // single-file mode;
    setFileMode(SINGLE);
    Data* length = (Data*)infoDic->get("length");
    totalLength = length->toLLInt();
    FileEntry fileEntry(name, totalLength, 0);
    fileEntries.push_back(fileEntry);
  } else {
    long long int length = 0;
    long long int offset = 0;
    // multi-file mode
    setFileMode(MULTI);
    *pTopDir = new Directory(name);
    const MetaList& metaList = files->getList();
    for(MetaList::const_iterator itr = metaList.begin(); itr != metaList.end();
	itr++) {
      Dictionary* fileDic = (Dictionary*)(*itr);
      Data* lengthData = (Data*)fileDic->get("length");
      length += lengthData->toLLInt();
      List* path = (List*)fileDic->get("path");
      const MetaList& paths = path->getList();
      Directory* parentDir = *pTopDir;
      string filePath = name;
      for(int i = 0; i < (int)paths.size()-1; i++) {
	Data* subpath = (Data*)paths.at(i);
	Directory* dir = new Directory(subpath->toString());
	parentDir->addFile(dir);
	parentDir = dir;
	filePath.append("/").append(subpath->toString());
      }
      Data* lastpath = (Data*)paths.back();
      filePath.append("/").append(lastpath->toString());
      FileEntry fileEntry(filePath, lengthData->toLLInt(), offset);
      fileEntries.push_back(fileEntry);
      offset += fileEntry.length;
    }
    totalLength = length;
  }
}

void TorrentMan::setupInternal1(const string& metaInfoFile) {
  peerId = "-A2****-";
  for(int i = 0; i < 12; i++) {
    peerId += Util::itos((int)(((double)10)*random()/(RAND_MAX+1.0)));
  }

  uploadLength = 0;
  downloadLength = 0;
  Dictionary* topDic = (Dictionary*)MetaFileUtil::parseMetaFile(metaInfoFile);
  const Dictionary* infoDic = (const Dictionary*)topDic->get("info");
  ShaVisitor v;
  infoDic->accept(&v);
  unsigned char md[20];
  int len;
  v.getHash(md, len);
  setInfoHash(md);

  FileEntries fileEntries;
  Directory* topDir = NULL;
  readFileEntry(fileEntries, &topDir, infoDic, metaInfoFile);

  announce = ((Data*)topDic->get("announce"))->toString();
  pieceLength = ((Data*)infoDic->get("piece length"))->toInt();
  pieces = totalLength/pieceLength+(totalLength%pieceLength ? 1 : 0);
  Data* piecesHashData = (Data*)infoDic->get("pieces");
  if(piecesHashData->getLen() != pieces*20) {
    throw new DlAbortEx("The number of pieces is wrong.");
  }
  for(int index = 0; index < pieces; index++) {
    string hex = Util::toHex((unsigned char*)&piecesHashData->getData()[index*20], 20);
    pieceHashes.push_back(hex);
    logger->debug("piece #%d, hash:%s", index, hex.c_str());
  }

  initBitfield();
  delete topDic;

  if(option->get(PREF_DIRECT_FILE_MAPPING) == V_TRUE) {
    if(fileMode == SINGLE) {
      diskAdaptor = new DirectDiskAdaptor(new DefaultDiskWriter(totalLength));
    } else {
      diskAdaptor = new MultiDiskAdaptor(new MultiDiskWriter(pieceLength));
    }
  } else {
    diskAdaptor = new CopyDiskAdaptor(new PreAllocationDiskWriter(totalLength));
    ((CopyDiskAdaptor*)diskAdaptor)->setTempFilename(name+".a2tmp");
  }
  diskAdaptor->setStoreDir(storeDir);
  diskAdaptor->setTopDir(topDir);
  diskAdaptor->setFileEntries(fileEntries);
}

void TorrentMan::setupInternal2() {
  if(segmentFileExists()) {
    load();
    diskAdaptor->openExistingFile();
  } else {
    diskAdaptor->initAndOpenFile();
  }
  setupComplete = true;
}

void TorrentMan::setup(const string& metaInfoFile, const Integers& targetFileIndexes) {
  setupInternal1(metaInfoFile);
  Strings targetFilePaths;
  const FileEntries& entries = diskAdaptor->getFileEntries();
  for(int i = 0; i < (int)entries.size(); i++) {
    if(find(targetFileIndexes.begin(), targetFileIndexes.end(), i+1) != targetFileIndexes.end()) {
      logger->debug("index=%d is %s", i+1, entries.at(i).path.c_str());
      targetFilePaths.push_back(entries.at(i).path);
    }
  }
  setFileFilter(targetFilePaths);
  setupInternal2();
}

void TorrentMan::setup(const string& metaInfoFile, const Strings& targetFilePaths) {
  setupInternal1(metaInfoFile);
  setFileFilter(targetFilePaths);
  setupInternal2();
}

void TorrentMan::setFileFilter(const Strings& filePaths) {
  if(fileMode != MULTI || filePaths.empty()) {
    return;
  }
  diskAdaptor->removeAllDownloadEntry();
  for(Strings::const_iterator pitr = filePaths.begin();
      pitr != filePaths.end(); pitr++) {
    if(!diskAdaptor->addDownloadEntry(*pitr)) {
      throw new DlAbortEx("No such file entry %s", (*pitr).c_str());
    }
    FileEntry fileEntry = diskAdaptor->getFileEntryFromPath(*pitr);
    bitfield->addFilter(fileEntry.offset, fileEntry.length);
  }
  bitfield->enableFilter();
}

FileEntries TorrentMan::readFileEntryFromMetaInfoFile(const string& metaInfoFile) {
  Dictionary* topDic = (Dictionary*)MetaFileUtil::parseMetaFile(metaInfoFile);
  const Dictionary* infoDic = (const Dictionary*)topDic->get("info");
  FileEntries fileEntries;
  Directory* topDir = NULL;
  readFileEntry(fileEntries, &topDir, infoDic, metaInfoFile);
  if(topDir != NULL) {
    delete topDir;
  }
  return fileEntries;
}

string TorrentMan::getName() const {
  return name;
}

bool TorrentMan::hasPiece(int index) const {
  return bitfield->isBitSet(index);
}

string TorrentMan::getPieceHash(int index) const {
  return pieceHashes.at(index);
}

string TorrentMan::getSegmentFilePath() const {
  return storeDir+"/"+name+".aria2";
}

bool TorrentMan::segmentFileExists() const {
  string segFilename = getSegmentFilePath();
  File f(segFilename);
  if(f.isFile()) {
    logger->info(MSG_SEGMENT_FILE_EXISTS, segFilename.c_str());
    return true;
  } else {
    logger->info(MSG_SEGMENT_FILE_DOES_NOT_EXIST, segFilename.c_str());
    return false;
  }
}

FILE* TorrentMan::openSegFile(const string& segFilename, const string& mode) const {
  FILE* segFile = fopen(segFilename.c_str(), mode.c_str());
  if(segFile == NULL) {
    throw new DlAbortEx(EX_SEGMENT_FILE_OPEN,
			segFilename.c_str(), strerror(errno));
  }
  return segFile;
}

void TorrentMan::load() {
  string segFilename = getSegmentFilePath();
  logger->info(MSG_LOADING_SEGMENT_FILE, segFilename.c_str());
  FILE* segFile = openSegFile(segFilename, "r+");
  try {
    read(segFile);
    fclose(segFile);
  } catch(string ex) {
    fclose(segFile);
    throw new DlAbortEx(EX_SEGMENT_FILE_READ,
			segFilename.c_str(), strerror(errno));
  }
  logger->info(MSG_LOADED_SEGMENT_FILE);
}

void TorrentMan::read(FILE* file) {
  assert(file != NULL);
  unsigned char savedInfoHash[INFO_HASH_LENGTH];
  if(fread(savedInfoHash, INFO_HASH_LENGTH, 1, file) < 1) {
    throw string("readError");
  }
  if(Util::toHex(savedInfoHash, INFO_HASH_LENGTH) != Util::toHex(infoHash, INFO_HASH_LENGTH)) {
    throw new DlAbortEx("Incorrect infoHash.");
  }
  unsigned char* savedBitfield = new unsigned char[bitfield->getBitfieldLength()];
  try {
    if(fread(savedBitfield, bitfield->getBitfieldLength(), 1, file) < 1) {
      throw string("readError");
    }
    setBitfield(savedBitfield, bitfield->getBitfieldLength());
    if(fread(&downloadLength, sizeof(downloadLength), 1, file) < 1) {
      throw string("readError");
    }
    if(fread(&uploadLength, sizeof(uploadLength), 1, file) < 1) {
      throw string("readError");
    }
    preDownloadLength = downloadLength;
    preUploadLength = uploadLength;
    delete [] savedBitfield;
  } catch(...) {
    delete [] savedBitfield;
    throw;
  }
}

void TorrentMan::save() const {
  if(!setupComplete) {
    return;
  }
  string segFilename = getSegmentFilePath();
  logger->info(MSG_SAVING_SEGMENT_FILE, segFilename.c_str());
  FILE* file = openSegFile(segFilename, "w");
  try {
    if(fwrite(infoHash, INFO_HASH_LENGTH, 1, file) < 1) {
      throw string("writeError");
    }
    if(fwrite(bitfield->getBitfield(), bitfield->getBitfieldLength(), 1, file) < 1) {
      throw string("writeError");
    }
    if(fwrite(&downloadLength, sizeof(downloadLength), 1, file) < 1) {
      throw string("writeError");
    }
    if(fwrite(&uploadLength, sizeof(uploadLength), 1, file) < 1) {
      throw string("writeError");
    }
    fclose(file);
    logger->info(MSG_SAVED_SEGMENT_FILE);
  } catch(string ex) {
    fclose(file);
    throw new DlAbortEx(EX_SEGMENT_FILE_WRITE,
			segFilename.c_str(), strerror(errno));
  }
}

void TorrentMan::remove() const {
  if(segmentFileExists()) {
    File f(getSegmentFilePath());
    f.remove();
  }
}

bool TorrentMan::isSelectiveDownloadingMode() const {
  return bitfield->isFilterEnabled();
}

void TorrentMan::finishSelectiveDownloadingMode() {
  bitfield->clearFilter();
  diskAdaptor->addAllDownloadEntry();
}

long long int TorrentMan::getCompletedLength() const {
  return bitfield->getCompletedLength();
}

long long int TorrentMan::getSelectedTotalLength() const {
  return bitfield->getFilteredTotalLength();
}

void TorrentMan::onDownloadComplete() {
  save();
  diskAdaptor->onDownloadComplete();
  if(isSelectiveDownloadingMode()) {
    finishSelectiveDownloadingMode();
  }
}
