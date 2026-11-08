#include "Checks.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "EditorDocument.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// Each check opens the document it needs and closes it again, so the
	// single-document invariant is left as it was found.
	struct CloseDocument
	{
		~CloseDocument() { editor::closeDocument(); }
	};
}

void registerEditorPreviewDocument(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "theEditorHoldsNoWorldOutsideItsRuntime", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Preview", 8, 2);
			auto const document = editor::currentDocument();
			require(document != nullptr,
				"opening a World left the editor with no document");
			// The World the UI holds and the World the runtime drives are one
			// object. There is no second World anywhere in the arrangement.
			require(world.get() == &document->runtime().world(),
				"the World the editor holds is not the World its runtime drives");
			require(&document->authoring().world() == &document->runtime().world(),
				"the authoring handle reaches a different World than the runtime drives");
		} });

	checks.push_back({ "theRuntimeIsTakenFromTheAuthoringHandle", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Authoring route", 8, 2);
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after opening a World");
			require(&document->runtime() == &document->authoring().runtime(),
				"the document's runtime is not the one its authoring handle wraps");
			require(&editor::previewRuntime(*world) == &document->runtime(),
				"previewRuntime does not resolve the open document's runtime");
		} });

	checks.push_back({ "previewRuntimeRefusesAWorldItDoesNotDrive", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Open", 8, 2);
			require(world != nullptr, "no World after opening a document");
			core::World foreign("Foreign", 4, 1);
			bool refused = false;
			try
			{
				(void)editor::previewRuntime(foreign);
			}
			catch (std::logic_error const&)
			{
				refused = true;
			}
			require(refused,
				"a World the editor does not own was accepted as its preview");
		} });

	checks.push_back({ "previewRuntimeRefusesWhenNothingIsOpen", [](smoke::Context const&)
		{
			editor::closeDocument();
			core::World loose("Loose", 4, 1);
			bool refused = false;
			try
			{
				(void)editor::previewRuntime(loose);
			}
			catch (std::logic_error const&)
			{
				refused = true;
			}
			require(refused,
				"a preview was driven with no document open");
		} });

	checks.push_back({ "adoptWorldRefusesAWorldSomeoneElseHolds", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const before = editor::currentDocument();
			auto loaded = std::make_shared<core::World>("Shared", 4, 1);
			auto const other = loaded;
			require(loaded.use_count() == 2, "fixture is not shared as intended");
			bool refused = false;
			try
			{
				(void)editor::adoptWorld(loaded);
			}
			catch (std::runtime_error const&)
			{
				refused = true;
			}
			require(refused,
				"a World held by someone else was adopted as the editor's document");
			require(loaded.use_count() == 2,
				"the refused adoption still changed who holds the World");
			require(editor::currentDocument() == before,
				"the refused adoption replaced the open document");
		} });

	checks.push_back({ "theWorldViewKeepsTheRuntimeAliveWhenTheDocumentCloses",
		[](smoke::Context const&)
		{
			auto world = editor::openNewWorld("Closing", 4, 1);
			auto& driven = editor::previewRuntime(*world);
			driven.update(1.0f);
			require(driven.getSimulationTick() == 60,
				"the preview did not advance before the document closed");
			editor::closeDocument();
			require(editor::currentDocument() == nullptr,
				"the document is still open after closeDocument()");
			// The view shares the runtime's control block rather than the
			// World's ownership, so the World it names is still alive and
			// still the one that was driven.
			require(world->getName() == "Closing",
				"the held World view stopped naming the closed document's World");
			require(world.get() != nullptr, "the held World view went null");
			world.reset();
		} });

	checks.push_back({ "replacingTheDocumentReplacesTheRuntime", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const first = editor::openNewWorld("First", 4, 1);
			auto const* const firstRuntime = &editor::previewRuntime(*first);
			auto const second = editor::openNewWorld("Second", 4, 1);
			auto const* const secondRuntime = &editor::previewRuntime(*second);
			require(firstRuntime != secondRuntime,
				"opening a second document kept driving the first document's runtime");
			require(editor::previewRuntime(*second).getSimulationTick() == 0,
				"the new document's runtime carried the old document's tick");
			bool staleRefused = false;
			try
			{
				(void)editor::previewRuntime(*first);
			}
			catch (std::logic_error const&)
			{
				staleRefused = true;
			}
			require(staleRefused,
				"a stale World view still resolved a runtime to drive");
		} });
}
