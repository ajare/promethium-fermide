#include "core/PermissionSet.h"

#include <utility>
#include "core/AccessPermission.h"

namespace core
{
	std::unique_ptr<PermissionSet> PermissionSet::create(std::string name)
	{
		return std::unique_ptr<PermissionSet>(new PermissionSet(std::move(name)));
	}

	std::string PermissionSet::trimName(std::string const& value)
	{
		return AccessPermission::trimName(value);
	}

	bool PermissionSet::nameIsValid(std::string const& value, std::string* diagnostic)
	{
		std::string accessDiagnostic;
		if (AccessPermission::nameIsValid(value, &accessDiagnostic))
		{
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (diagnostic)
		{
			*diagnostic = std::move(accessDiagnostic);
			auto position = diagnostic->find("Access permission");
			if (position != std::string::npos)
				diagnostic->replace(position, 17, "Permission set");
		}
		return false;
	}
}
