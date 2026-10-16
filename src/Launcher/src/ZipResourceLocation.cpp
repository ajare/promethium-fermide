#include <stdexcept>
#include <limits>
#include <memory>

#include "utils/FileSystem.h"

#include "willpower/common/Exceptions.h"

#include "miniz.c"
#include "ZipResourceLocation.h"

using namespace std;

using namespace wp::application;

ZipResourceLocation::ZipResourceLocation(wp::Logger* logger, string const& file, string const& definitionFile)
    : resourcesystem::ResourceLocation(logger, file, "ZipFile", definitionFile), mRootPath(file), mDefinitionPath(file + ":" + definitionFile) {
  auto archiveStorage = make_unique<mz_zip_archive>();

  if (!mz_zip_reader_init_file(archiveStorage.get(), mRootPath.c_str(), 0)) {
    throw wp::Exception("Could not open Zip archive '" + mRootPath + "'.");
  }

  auto archive = unique_ptr<mz_zip_archive, void (*)(mz_zip_archive*)>(
      archiveStorage.release(), [](mz_zip_archive* value) {
        mz_zip_reader_end(value);
        delete value;
      });

  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(archive.get()); ++i) {
    mz_zip_archive_file_stat fileStat;

    if (!mz_zip_reader_file_stat(archive.get(), i, &fileStat)) {
      throw wp::Exception("Could not read member " + to_string(i) + " from Zip archive '" + mRootPath + "'.");
    }

    FileEntry entry;
    entry.filename = utils::FileSystem::standardisePath(fileStat.m_filename);
    entry.index = i;
    entry.compressedSize = fileStat.m_comp_size;
    entry.uncompressedSize = fileStat.m_uncomp_size;

    mFileEntries[entry.filename] = entry;
  }

  // miniz keeps the archive's FILE and central-directory state in this object.
  // It must remain live until the resource location is destroyed.
  mArchive = archive.release();
}

ZipResourceLocation::~ZipResourceLocation() {
  auto archive = static_cast<mz_zip_archive*>(mArchive);
  if (archive) {
    mz_zip_reader_end(archive);
    delete archive;
    mArchive = nullptr;
  }
}

string const& ZipResourceLocation::getRootPath() const {
  return mRootPath;
}
/*
resourcesystem::DataStreamPtr ZipResourceLocation::getHardResourceDataStream(string const& file, string const& namesp) const
{
        auto it = mFileEntries.find(utils::FileSystem::standardisePath(file));
        FileEntry const& e = it->second;

        size_t fileSize;
        char* buffer = (char*)mz_zip_reader_extract_to_heap(static_cast<mz_zip_archive*>(mArchive), e.index, &fileSize, 0);

        return resourcesystem::DataStreamPtr(new resourcesystem::DataStream(buffer, fileSize, namesp));
}

resourcesystem::DataStreamPtr ZipResourceLocation::getHardResourceDataStreamProgressive(string const& file, string const& namesp, DataStreamFetchProgressCallback progress) const
{
        throw runtime_error("ZipResourceLocation::getHardResourceDataStreamProgressive() not yet implemented.");
}
*/
bool ZipResourceLocation::hardResourceExists(string const& file) const {
  return mFileEntries.contains(utils::FileSystem::standardisePath(file));
}

uint8_t* ZipResourceLocation::readData(string const& source, uint32_t* dataSize) {
  string const memberName = utils::FileSystem::standardisePath(source);
  auto const it = mFileEntries.find(memberName);
  if (it == mFileEntries.end()) {
    throw wp::Exception("Member '" + memberName + "' was not found in Zip archive '" + mRootPath + "'.");
  }

  FileEntry const& entry = it->second;
  if (entry.uncompressedSize > numeric_limits<uint32_t>::max()) {
    throw wp::Exception("Member '" + memberName + "' in Zip archive '" + mRootPath + "' is " +
                        to_string(entry.uncompressedSize) + " bytes, which exceeds the DataStream size limit of " +
                        to_string(numeric_limits<uint32_t>::max()) + " bytes.");
  }

  uint32_t const size = static_cast<uint32_t>(entry.uncompressedSize);
  auto data = make_unique<uint8_t[]>(size);
  if (size != 0 && !mz_zip_reader_extract_to_mem(static_cast<mz_zip_archive*>(mArchive), entry.index, data.get(), size, 0)) {
    throw wp::Exception("Could not extract member '" + memberName + "' from Zip archive '" + mRootPath + "'.");
  }

  *dataSize = size;
  return data.release();
}

string const& ZipResourceLocation::getDefinitionFile() const {
  return mDefinitionPath;
}
