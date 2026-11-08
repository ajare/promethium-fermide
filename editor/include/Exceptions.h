#pragma once

// The editor's own exit signal (#989).
//
// This type used to live at core/include/Exceptions.h, where it was
// un-namespaced and carried no PF_CORE_API, so it never actually crossed the
// shared-core boundary despite sitting in the core include root. Every
// throw is in this application - the startup sequence in Main.cpp and the
// File > Exit action in UI.cpp - so the editor owns it here.
//
// The launcher keeps its own copy at
// launcher/include/ExitApplicationException.h. The two are deliberately
// separate rather than shared: the editor is an executable, so the launcher
// cannot link it, and neither application has a library that could host a
// shared exit signal without putting a GUI-lifecycle concern somewhere it
// does not belong. Each copy is used only by its own main() and its own
// state unwinding. If a third application ever needs one, that is the
// moment to give it a shared host rather than to add a third copy.

#include <stdexcept>
#include <string>

class ExitApplicationException : public std::runtime_error
{
	int mExitCode;

public:
	ExitApplicationException(int exitCode, std::string const& message)
		: std::runtime_error(message)
		, mExitCode(exitCode)
	{
	}

	int getExitCode() const
	{
		return mExitCode;
	}
};
