#pragma once

// The editor's "Open Recent" list (#988).
//
// This is a File-menu concern, not World-domain state: core never referenced
// it, so it has no reason to cross the shared-core DLL boundary (#962) and it
// carries no export macro. It is header-only so the headless verification tier
// can compile the same production code without linking the editor - the smoke
// modules cannot link an executable, and a compiled library here would have to
// be linked by them. headless/CMakeLists.txt already appends editor/include to
// PF_INCLUDE_DIR, so the checks pick this up with no link change at all.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

class RecentFiles
{
	std::filesystem::path mFilepath;
	std::deque<std::string> mEntries;
	std::size_t mMaximumEntries;

	void save() const
	{
		std::ofstream output(mFilepath, std::ios::trunc);
		if (!output)
			throw std::runtime_error("Could not write recent-file list: " + mFilepath.string());
		for (auto const& entry : mEntries) output << entry << '\n';
		if (!output)
			throw std::runtime_error("Could not write recent-file list: " + mFilepath.string());
	}

public:
	explicit RecentFiles(std::size_t maximumEntries = 5)
		: mMaximumEntries(maximumEntries)
	{
		if (maximumEntries == 0)
			throw std::invalid_argument("RecentFiles requires a positive entry limit");
	}

	// Loads an existing list, or creates an empty file when none exists.
	void initialize(std::filesystem::path filepath)
	{
		mFilepath = std::move(filepath);
		mEntries.clear();

		std::ifstream input(mFilepath);
		if (!input)
		{
			if (std::filesystem::exists(mFilepath))
				throw std::runtime_error("Could not read recent-file list: " + mFilepath.string());
			save();
			return;
		}

		for (std::string line; std::getline(input, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()
				|| std::find(mEntries.begin(), mEntries.end(), line) != mEntries.end()) continue;
			mEntries.push_back(std::move(line));
			if (mEntries.size() == mMaximumEntries) break;
		}
		if (input.bad())
			throw std::runtime_error("Could not read recent-file list: " + mFilepath.string());
	}

	void add(std::string filepath)
	{
		if (mFilepath.empty())
			throw std::logic_error("RecentFiles must be initialized before use");
		if (filepath.empty()) return;
		mEntries.erase(std::remove(mEntries.begin(), mEntries.end(), filepath), mEntries.end());
		mEntries.push_front(std::move(filepath));
		while (mEntries.size() > mMaximumEntries) mEntries.pop_back();
		save();
	}

	// Removes and persists a selected entry only when it no longer resolves to
	// a regular file. Inspection errors leave the entry intact.
	bool removeUnavailable(std::string const& filepath)
	{
		if (mFilepath.empty())
			throw std::logic_error("RecentFiles must be initialized before use");
		std::error_code error;
		auto const status = std::filesystem::status(filepath, error);
		if ((!error && std::filesystem::is_regular_file(status))
			|| (error && error != std::errc::no_such_file_or_directory)) return false;

		auto const previousSize = mEntries.size();
		mEntries.erase(std::remove(mEntries.begin(), mEntries.end(), filepath), mEntries.end());
		if (mEntries.size() == previousSize) return false;
		save();
		return true;
	}

	[[nodiscard]] bool empty() const { return mEntries.empty(); }
	[[nodiscard]] std::deque<std::string> const& entries() const { return mEntries; }
	[[nodiscard]] std::filesystem::path const& filepath() const { return mFilepath; }
};
