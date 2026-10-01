#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <stdexcept>
#include <string_view>

namespace smoke
{
	class Failure : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// Only unavailable optional external capabilities may skip a check.
	class OptionalCapabilityUnavailable : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	void require(bool condition, std::string_view diagnostic);
	void setupProcess();

	class Context
	{
	public:
		Context();
		~Context();
		Context(Context const&) = delete;
		Context& operator=(Context const&) = delete;

		std::filesystem::path const& temporaryRoot() const { return temporaryRoot_; }
		// Repository-relative required fixture; missing fixtures fail, never skip.
		std::filesystem::path fixture(std::filesystem::path const& relative) const;

	private:
		std::filesystem::path temporaryRoot_;
	};

	struct Check
	{
		std::string_view name;
		void (*run)(Context const&);
	};

	// Arguments exclude argv[0]. Results go to stdout; invocation errors to stderr.
	int run(std::string_view module, std::span<Check const> checks,
		std::span<std::string_view const> arguments, std::ostream& out, std::ostream& err);
	int main(std::string_view module, std::span<Check const> checks, int argc, char** argv);
}
