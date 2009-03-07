/* <!-- copyright */
/*
 * aria2 - The high speed download utility
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "MultiDiskAdaptor.h"

#include <cassert>
#include <algorithm>
#include <map>

#include "DefaultDiskWriter.h"
#include "message.h"
#include "Util.h"
#include "FileEntry.h"
#include "MultiFileAllocationIterator.h"
#include "DefaultDiskWriterFactory.h"
#include "DlAbortEx.h"
#include "File.h"
#include "StringFormat.h"
#include "Logger.h"
#include "SimpleRandomizer.h"

namespace aria2 {

DiskWriterEntry::DiskWriterEntry(const SharedHandle<FileEntry>& fileEntry):
  fileEntry(fileEntry), _open(false), _directIO(false),
  _needsFileAllocation(false) {}

DiskWriterEntry::~DiskWriterEntry() {}

std::string DiskWriterEntry::getFilePath(const std::string& storeDir) const
{
  return storeDir+"/"+fileEntry->getPath();
}

void DiskWriterEntry::initAndOpenFile(const std::string& storeDir)
{
  if(!diskWriter.isNull()) {
    diskWriter->initAndOpenFile(getFilePath(storeDir), fileEntry->getLength());
    if(_directIO) {
      diskWriter->enableDirectIO();
    }
    _open = true;
  }
}

void DiskWriterEntry::openFile(const std::string& storeDir)
{
  if(!diskWriter.isNull()) {
    diskWriter->openFile(getFilePath(storeDir), fileEntry->getLength());
    if(_directIO) {
      diskWriter->enableDirectIO();
    }
    _open = true;
  }
}

void DiskWriterEntry::openExistingFile(const std::string& storeDir)
{
  if(!diskWriter.isNull()) {
    diskWriter->openExistingFile(getFilePath(storeDir), fileEntry->getLength());
    if(_directIO) {
      diskWriter->enableDirectIO();
    }
    _open = true;
  }
}

bool DiskWriterEntry::isOpen() const
{
  return _open;
}

void DiskWriterEntry::closeFile()
{
  if(_open) {
    diskWriter->closeFile();
    _open = false;
  }
}

bool DiskWriterEntry::fileExists(const std::string& storeDir)
{
  return File(getFilePath(storeDir)).exists();
}

uint64_t DiskWriterEntry::size() const
{
  assert(!diskWriter.isNull());
  return diskWriter->size();
}

SharedHandle<FileEntry> DiskWriterEntry::getFileEntry() const
{
  return fileEntry;
}

void DiskWriterEntry::setDiskWriter(const SharedHandle<DiskWriter>& diskWriter)
{
  this->diskWriter = diskWriter;
}

SharedHandle<DiskWriter> DiskWriterEntry::getDiskWriter() const
{
  return diskWriter;
}

bool DiskWriterEntry::operator<(const DiskWriterEntry& entry) const
{
  return fileEntry < entry.fileEntry;
}

void DiskWriterEntry::enableDirectIO()
{
  if(_open) {
    diskWriter->enableDirectIO();
  }
  _directIO = true;
}

void DiskWriterEntry::disableDirectIO()
{
  if(_open) {
    diskWriter->disableDirectIO();
  }
  _directIO = false;
}

bool DiskWriterEntry::needsFileAllocation() const
{
  return _needsFileAllocation;
}

void DiskWriterEntry::needsFileAllocation(bool f)
{
  _needsFileAllocation = f;
}

MultiDiskAdaptor::MultiDiskAdaptor():
  pieceLength(0),
  _maxOpenFiles(DEFAULT_MAX_OPEN_FILES),
  _directIOAllowed(false),
  _readOnly(false) {}

MultiDiskAdaptor::~MultiDiskAdaptor() {}

static SharedHandle<DiskWriterEntry> createDiskWriterEntry
(const SharedHandle<FileEntry>& fileEntry,
 bool needsFileAllocation)
{
  SharedHandle<DiskWriterEntry> entry(new DiskWriterEntry(fileEntry));
  entry->needsFileAllocation(needsFileAllocation);
  return entry;
}
 

void MultiDiskAdaptor::resetDiskWriterEntries()
{
  diskWriterEntries.clear();

  if(fileEntries.empty()) {
    return;
  }

  for(std::deque<SharedHandle<FileEntry> >::const_iterator i =
	fileEntries.begin(); i != fileEntries.end(); ++i) {
    diskWriterEntries.push_back
      (createDiskWriterEntry(*i, (*i)->isRequested()));
  }

  std::map<std::string, bool> dwreq;

  // TODO Currently, pieceLength == 0 is used for unit testing only.
  if(pieceLength > 0) {
    std::deque<SharedHandle<DiskWriterEntry> >::iterator done =
      diskWriterEntries.begin();
    for(std::deque<SharedHandle<DiskWriterEntry> >::iterator itr =
	  diskWriterEntries.begin(); itr != diskWriterEntries.end();) {
      const SharedHandle<FileEntry>& fileEntry = (*itr)->getFileEntry();

      if(!fileEntry->isRequested()) {
	++itr;
	continue;
      }
      off_t pieceStartOffset =
	(fileEntry->getOffset()/pieceLength)*pieceLength;
      if(itr != diskWriterEntries.begin()) {
	for(std::deque<SharedHandle<DiskWriterEntry> >::iterator i =
	      itr-1; true; --i) {
	  const SharedHandle<FileEntry>& fileEntry = (*i)->getFileEntry();
	  if(pieceStartOffset <= fileEntry->getOffset() ||
	     (uint64_t)pieceStartOffset <
	     fileEntry->getOffset()+fileEntry->getLength()) {
	    (*i)->needsFileAllocation(true);
	  } else {
	    break;
	  }
	  if(i == done) {
	    break;
	  }
	}
      }

      if(fileEntry->getLength() > 0) {
	off_t lastPieceStartOffset =
	  (fileEntry->getOffset()+fileEntry->getLength()-1)/pieceLength*pieceLength;
	logger->debug("Checking adjacent backward file to %s"
		      " whose lastPieceStartOffset+pieceLength=%lld",
		      fileEntry->getPath().c_str(),
		      lastPieceStartOffset+pieceLength);

	++itr;
	// adjacent backward files are not needed to be allocated. They
	// just requre DiskWriter
	for(; itr != diskWriterEntries.end() &&
 	      (!(*itr)->getFileEntry()->isRequested() ||
 	       (*itr)->getFileEntry()->getLength() == 0); ++itr) {
	  logger->debug("file=%s, offset=%lld",
			(*itr)->getFileEntry()->getPath().c_str(),
			(*itr)->getFileEntry()->getOffset());

	  if((*itr)->getFileEntry()->getOffset() <
	     static_cast<off_t>(lastPieceStartOffset+pieceLength)) {
	    logger->debug("%s needs diskwriter",
			  (*itr)->getFileEntry()->getPath().c_str());
	    dwreq[(*itr)->getFileEntry()->getPath()] = true;
	  } else {
	    break;
	  }
	}
	done = itr-1;
      } else {
	done = itr;
	++itr;
      }
    }
  }
  DefaultDiskWriterFactory dwFactory;
  for(std::deque<SharedHandle<DiskWriterEntry> >::iterator i =
	diskWriterEntries.begin(); i != diskWriterEntries.end(); ++i) {
    if((*i)->needsFileAllocation() ||
       dwreq.find((*i)->getFileEntry()->getPath()) != dwreq.end() ||
       (*i)->fileExists(storeDir)) {
      logger->debug("Creating DiskWriter for filename=%s",
		    (*i)->getFilePath(storeDir).c_str());
      (*i)->setDiskWriter(dwFactory.newDiskWriter());
      (*i)->getDiskWriter()->setDirectIOAllowed(_directIOAllowed);
      if(_readOnly) {
	(*i)->getDiskWriter()->enableReadOnly();
      }
    }
  }
}

std::string MultiDiskAdaptor::getTopDirPath() const
{
  return storeDir+"/"+topDir;
}

void MultiDiskAdaptor::mkdir(const std::string& storeDir) const
{
  for(std::deque<SharedHandle<DiskWriterEntry> >::const_iterator i =
	diskWriterEntries.begin(); i != diskWriterEntries.end(); ++i) {
    (*i)->getFileEntry()->setupDir(storeDir);
  }
}

void MultiDiskAdaptor::openIfNot
(const SharedHandle<DiskWriterEntry>& entry,
 void (DiskWriterEntry::*open)(const std::string&),
 const std::string& storeDir)
{
  if(!entry->isOpen()) {
//     logger->debug("DiskWriterEntry: Cache MISS. offset=%s",
// 		  Util::itos(entry->getFileEntry()->getOffset()).c_str());
 
    size_t numOpened = _openedDiskWriterEntries.size();
    (entry.get()->*open)(storeDir);
    if(numOpened >= _maxOpenFiles) {
      // Cache is full. 
      // Choose one DiskWriterEntry randomly and close it.
      size_t index = SimpleRandomizer::getInstance()->getRandomNumber(numOpened);
      std::deque<SharedHandle<DiskWriterEntry> >::iterator i =
	_openedDiskWriterEntries.begin();
      std::advance(i, index);
      (*i)->closeFile();
      (*i) = entry;
    } else {
      _openedDiskWriterEntries.push_back(entry);
    } 
  } else {
//     logger->debug("DiskWriterEntry: Cache HIT. offset=%s",
// 		  Util::itos(entry->getFileEntry()->getOffset()).c_str());
  }
}

void MultiDiskAdaptor::openFile()
{
  resetDiskWriterEntries();
  mkdir(storeDir);
  // Call DiskWriterEntry::openFile to make sure that zero-length files are
  // created.
  for(DiskWriterEntries::iterator itr = diskWriterEntries.begin();
      itr != diskWriterEntries.end(); ++itr) {
    openIfNot(*itr, &DiskWriterEntry::openFile, storeDir);
  }
}

void MultiDiskAdaptor::initAndOpenFile()
{
  resetDiskWriterEntries();
  mkdir(storeDir);
  // Call DiskWriterEntry::initAndOpenFile to make files truncated.
  for(DiskWriterEntries::iterator itr = diskWriterEntries.begin();
      itr != diskWriterEntries.end(); ++itr) {
    openIfNot(*itr, &DiskWriterEntry::initAndOpenFile, storeDir);
  }
}

void MultiDiskAdaptor::openExistingFile()
{
  resetDiskWriterEntries();
  // Not need to call openIfNot here.
}

void MultiDiskAdaptor::closeFile()
{
  for(DiskWriterEntries::iterator itr = diskWriterEntries.begin();
      itr != diskWriterEntries.end(); ++itr) {
    (*itr)->closeFile();
  }
}

void MultiDiskAdaptor::onDownloadComplete()
{
  closeFile();
  openFile();
}

static bool isInRange(const DiskWriterEntryHandle entry, off_t offset)
{
  return entry->getFileEntry()->getOffset() <= offset &&
    (uint64_t)offset < entry->getFileEntry()->getOffset()+entry->getFileEntry()->getLength();
}

static size_t calculateLength(const DiskWriterEntryHandle entry,
			      off_t fileOffset, size_t rem)
{
  size_t length;
  if(entry->getFileEntry()->getLength() < (uint64_t)fileOffset+rem) {
    length = entry->getFileEntry()->getLength()-fileOffset;
  } else {
    length = rem;
  }
  return length;
}

class OffsetCompare {
public:
  bool operator()(off_t offset, const SharedHandle<DiskWriterEntry>& dwe)
  {
    return offset < dwe->getFileEntry()->getOffset();
  }
};

static DiskWriterEntries::const_iterator
findFirstDiskWriterEntry(const DiskWriterEntries& diskWriterEntries, off_t offset)
{
  DiskWriterEntries::const_iterator first =
    std::upper_bound(diskWriterEntries.begin(), diskWriterEntries.end(),
		     offset, OffsetCompare());

  --first;

  // In case when offset is out-of-range
  if(!isInRange(*first, offset)) {
    throw DlAbortEx
      (StringFormat(EX_FILE_OFFSET_OUT_OF_RANGE,
		    Util::itos(offset, true).c_str()).str());
  }
  return first;
}

static void throwOnDiskWriterNotOpened(const SharedHandle<DiskWriterEntry>& e,
				       off_t offset,
				       const std::string& storeDir)
{
  throw DlAbortEx
    (StringFormat("DiskWriter for offset=%s, filename=%s is not opened.",
		  Util::itos(offset).c_str(),
		  e->getFilePath(storeDir).c_str()).str());  
}

void MultiDiskAdaptor::writeData(const unsigned char* data, size_t len,
				 off_t offset)
{
  DiskWriterEntries::const_iterator first = findFirstDiskWriterEntry(diskWriterEntries, offset);

  size_t rem = len;
  off_t fileOffset = offset-(*first)->getFileEntry()->getOffset();
  for(DiskWriterEntries::const_iterator i = first; i != diskWriterEntries.end(); ++i) {
    size_t writeLength = calculateLength(*i, fileOffset, rem);

    openIfNot(*i, &DiskWriterEntry::openFile, storeDir);

    if(!(*i)->isOpen()) {
      throwOnDiskWriterNotOpened(*i, offset+(len-rem), storeDir);
    }

    (*i)->getDiskWriter()->writeData(data+(len-rem), writeLength, fileOffset);
    rem -= writeLength;
    fileOffset = 0;
    if(rem == 0) {
      break;
    }
  }
}

ssize_t MultiDiskAdaptor::readData(unsigned char* data, size_t len, off_t offset)
{
  DiskWriterEntries::const_iterator first = findFirstDiskWriterEntry(diskWriterEntries, offset);

  size_t rem = len;
  size_t totalReadLength = 0;
  off_t fileOffset = offset-(*first)->getFileEntry()->getOffset();
  for(DiskWriterEntries::const_iterator i = first; i != diskWriterEntries.end(); ++i) {
    size_t readLength = calculateLength(*i, fileOffset, rem);

    openIfNot(*i, &DiskWriterEntry::openFile, storeDir);

    if(!(*i)->isOpen()) {
      throwOnDiskWriterNotOpened(*i, offset+(len-rem), storeDir);
    }

    totalReadLength +=
      (*i)->getDiskWriter()->readData(data+(len-rem), readLength, fileOffset);
    rem -= readLength;
    fileOffset = 0;
    if(rem == 0) {
      break;
    }
  }
  return totalReadLength;
}

bool MultiDiskAdaptor::fileExists()
{
  // TODO Use FileEntry::fileExists() here.

  // Don't use _cachedTopDirPath because they are initialized after opening files.
  // This method could be called before opening files.
  for(std::deque<SharedHandle<FileEntry> >::iterator i =
	fileEntries.begin(); i != fileEntries.end(); ++i) {
    if(File(storeDir+"/"+(*i)->getPath()).exists()) {
      return true;
    }
  }
  return false;
}

// TODO call DiskWriter::openFile() before calling this function.
uint64_t MultiDiskAdaptor::size()
{
  uint64_t size = 0;
  for(DiskWriterEntries::const_iterator itr = diskWriterEntries.begin();
      itr != diskWriterEntries.end(); ++itr) {
    openIfNot(*itr, &DiskWriterEntry::openFile, storeDir);
    size += (*itr)->size();
  }
  return size;
}

FileAllocationIteratorHandle MultiDiskAdaptor::fileAllocationIterator()
{
  return SharedHandle<FileAllocationIterator>(new MultiFileAllocationIterator(this));
}

void MultiDiskAdaptor::enableDirectIO()
{
  for(DiskWriterEntries::const_iterator itr = diskWriterEntries.begin();
      itr != diskWriterEntries.end(); ++itr) {
    (*itr)->enableDirectIO();
  }
}

void MultiDiskAdaptor::disableDirectIO()
{
  for(DiskWriterEntries::const_iterator itr = diskWriterEntries.begin();
      itr != diskWriterEntries.end(); ++itr) {
    (*itr)->disableDirectIO();
  }
}

void MultiDiskAdaptor::enableReadOnly()
{
  _readOnly = true;
}

void MultiDiskAdaptor::disableReadOnly()
{
  _readOnly = false;
}

void MultiDiskAdaptor::cutTrailingGarbage()
{
  for(std::deque<SharedHandle<DiskWriterEntry> >::const_iterator i =
	diskWriterEntries.begin(); i != diskWriterEntries.end(); ++i) {
    uint64_t length = (*i)->getFileEntry()->getLength();
    if(File((*i)->getFilePath(storeDir)).size() > length) {
      // We need open file before calling DiskWriter::truncate(uint64_t)
      openIfNot(*i, &DiskWriterEntry::openFile, storeDir);
      (*i)->getDiskWriter()->truncate(length);
    }
  }
}

void MultiDiskAdaptor::setMaxOpenFiles(size_t maxOpenFiles)
{
  _maxOpenFiles = maxOpenFiles;
}

size_t MultiDiskAdaptor::utime(const Time& actime, const Time& modtime)
{
  size_t numOK = 0;
  for(std::deque<SharedHandle<FileEntry> >::const_iterator i =
	fileEntries.begin(); i != fileEntries.end(); ++i) {
    if((*i)->isRequested()) {
      File f(storeDir+"/"+(*i)->getPath());
      if(f.isFile() && f.utime(actime, modtime)) {
	++numOK;
      }
    }
  }
  return numOK;
}

const std::deque<SharedHandle<DiskWriterEntry> >&
MultiDiskAdaptor::getDiskWriterEntries() const
{
  return diskWriterEntries;
}

const std::string& MultiDiskAdaptor::getStoreDir() const
{
  return storeDir;
}

} // namespace aria2
