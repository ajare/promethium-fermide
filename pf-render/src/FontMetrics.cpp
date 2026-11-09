#include "FontMetrics.h"

#include <stdexcept>

namespace pf::render
{
namespace
{
	// Constant-initialised, so an application that installs its metrics from
	// static start-up can never read this before it exists.
	IFontMetrics const* gFontMetrics{ nullptr };
}

IFontMetrics const* installFontMetrics(IFontMetrics const* metrics)
{
	auto const previous = gFontMetrics;
	gFontMetrics = metrics;
	return previous;
}

IFontMetrics const& requireFontMetrics()
{
	if (!gFontMetrics)
		throw std::runtime_error("No font metrics installed: the shared renderer "
			"measures and tessellates text through the application's IFontMetrics (#999).");
	return *gFontMetrics;
}

FontMetricsScope::FontMetricsScope(IFontMetrics const* metrics)
	: mPrevious(installFontMetrics(metrics))
{
}

FontMetricsScope::~FontMetricsScope()
{
	installFontMetrics(mPrevious);
}

} // namespace pf::render
