#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "willpower/application/resourcesystem/ResourceLocation.h"

class ZipResourceLocation : public wp::application::resourcesystem::ResourceLocation {
  struct FileEntry {
    std::string filename;
    std::uint32_t index;
    std::uint64_t compressedSize, uncompressedSize;
  };

private:
  std::string mRootPath, mDefinitionPath;

  void* mArchive{nullptr};

  std::map<std::string, FileEntry> mFileEntries;

private:
  /*
  wp::application::resourcesystem::DataStreamPtr getHardResourceDataStream(std::string const& file, std::string const& namesp) const;

  wp::application::resourcesystem::DataStreamPtr getHardResourceDataStreamProgressive(std::string const& file, std::string const& namesp, DataStreamFetchProgressCallback progress) const;
  */

  bool hardResourceExists(std::string const& file) const;

public:
  ZipResourceLocation(wp::Logger* logger, std::string const& file, std::string const& definitionFile);
  ZipResourceLocation(ZipResourceLocation const&) = delete;
  ZipResourceLocation& operator=(ZipResourceLocation const&) = delete;

  ~ZipResourceLocation() override;

  std::string const& getRootPath() const;

  uint8_t* readData(std::string const& source, uint32_t* dataSize);

  std::string const& getDefinitionFile() const;
};
