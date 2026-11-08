#pragma once

// The launcher's own exit signal (#989).
//
// The editor keeps a separate copy at editor/include/Exceptions.h. The two
// are deliberately not shared: the editor is an executable the launcher
// cannot link, and neither has a library that could host a shared exit
// signal without putting GUI lifecycle into the World domain aggregate.
// Each copy serves only its own application; getMessage() is what this
// application's catch block logs.

#include <stdexcept>
#include <string>

class ExitApplicationException : public std::runtime_error {
  int mExitCode;

  std::string mMessage;

public:
  ExitApplicationException(int exitCode, std::string message)
      : std::runtime_error(message), mExitCode(exitCode), mMessage(message) {
  }

  int getExitCode() const {
    return mExitCode;
  }

  std::string const& getMessage() const {
    return mMessage;
  }
};