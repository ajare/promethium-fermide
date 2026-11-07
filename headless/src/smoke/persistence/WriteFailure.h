#pragma once

#include "core/TransactionalFileWriter.h"

namespace persistence
{
	// A failed check must not leave fault injection enabled for the next check.
	struct ResetWriteFailure
	{
		ResetWriteFailure() { core::setTransactionalWriteFailureAfterBytesForTesting(0); }
		~ResetWriteFailure() { core::setTransactionalWriteFailureAfterBytesForTesting(0); }
	};
}
