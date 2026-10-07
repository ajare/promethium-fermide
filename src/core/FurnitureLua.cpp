#include "FurnitureLua.h"
#include "ScriptSandbox.h"
#include <memory>
#include <set>

namespace core::script
{
	namespace
	{
		struct Conversion
		{
			std::string const& source;
			FurnitureValue root;
			std::string diagnostic;
			size_t values{};
		};

		void validateFunction(lua_State* state, int index, unsigned depth)
		{
			index = lua_absindex(state, index);
			if (!lua_isfunction(state, index) || lua_iscfunction(state, index) || depth > 16)
				throw SerializationException("Furniture lifecycle requires a Lua function with stateless helpers");
			for (int i = 1; ; ++i)
			{
				auto name = lua_getupvalue(state, index, i);
				if (!name) break;
				// Helpers may be shared, but never mutable captured values. Each later
				// invocation reconstructs this source in a fresh private environment.
				if (std::string_view(name) != "_ENV") validateFunction(state, -1, depth + 1);
				lua_pop(state, 1);
			}
		}

		// All owning records live in Conversion, outside the protected boundary.
		// Recursive frames borrow nodes and have no destructors crossed by Lua errors.
		void convert(lua_State* state, int index, FurnitureValue& value, Conversion& call, unsigned depth)
		{
			if (++call.values > 16384 || depth > 16)
				throw SerializationException("Furniture returned data exceeds conversion budget");
			index = lua_absindex(state, index);
			switch (lua_type(state, index))
			{
			case LUA_TSTRING:
			{
				size_t size{}; auto text = lua_tolstring(state, index, &size);
				if (size > 1024 || std::string_view(text, size).find('\0') != std::string_view::npos)
					throw SerializationException("Invalid Furniture string");
				value.kind = FurnitureValue::String; value.text.assign(text, size); return;
			}
			case LUA_TNUMBER: value.kind = FurnitureValue::Number; value.number = lua_tonumber(state, index); return;
			case LUA_TBOOLEAN: value.kind = FurnitureValue::Boolean; value.boolean = lua_toboolean(state, index); return;
			case LUA_TFUNCTION:
				validateFunction(state, index, 0); value.kind = FurnitureValue::Function; return;
			case LUA_TTABLE:
				if (lua_getmetatable(state, index)) throw SerializationException("Furniture tables cannot have metatables");
				value.kind = FurnitureValue::Array;
				lua_pushnil(state);
				while (lua_next(state, index))
				{
					if (lua_type(state, -2) == LUA_TSTRING)
					{
						if (!value.elements.empty()) throw SerializationException("Mixed Furniture table keys");
						value.kind = FurnitureValue::Object;
						size_t size{}; auto text = lua_tolstring(state, -2, &size);
						if (size > 128 || std::string_view(text, size).find('\0') != std::string_view::npos)
							throw SerializationException("Invalid Furniture field key");
						// Key ownership belongs to the host node before recursion allocates Lua values.
						auto* child = &value.fields[std::string(text, size)];
						convert(state, -1, *child, call, depth + 1);
					}
					else if (lua_isinteger(state, -2) && value.kind == FurnitureValue::Array)
					{
						auto key = lua_tointeger(state, -2);
						if (key < 1 || key > 4096) throw SerializationException("Invalid Furniture array index");
						if (value.elements.size() < static_cast<size_t>(key)) value.elements.resize(static_cast<size_t>(key));
						convert(state, -1, value.elements[static_cast<size_t>(key) - 1], call, depth + 1);
					}
					else throw SerializationException("Furniture tables require string fields or dense arrays");
					lua_pop(state, 1);
				}
				for (auto const& item : value.elements)
					if (!item) throw SerializationException("Furniture array contains holes");
				return;
			default: throw SerializationException("Unsupported Furniture value type");
			}
		}

		int evaluate(lua_State* state)
		{
			auto& call = *static_cast<Conversion*>(lua_touserdata(state, 1));
			lua_settop(state, 0);
			pushPrivateEnvironment(state);
			if (luaL_loadbufferx(state, call.source.data(), call.source.size(), "@furniture.lua", "t") != LUA_OK)
				return lua_error(state);
			lua_pushvalue(state, 1); lua_setupvalue(state, -2, 1);
			lua_call(state, 0, 1);
			bool failed = false;
			try { convert(state, -1, call.root, call, 0); }
			catch (std::exception const& error) { call.diagnostic = error.what(); failed = true; }
			if (failed) { pushBorrowedDiagnostic(state, call.diagnostic); return lua_error(state); }
			return 0;
		}

		void fields(FurnitureValue const& value, std::initializer_list<std::string_view> allowed)
		{
			if (value.kind != FurnitureValue::Object) throw SerializationException("Furniture record must be a table");
			for (auto const& [key, child] : value.fields)
			{
				(void)child;
				if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
					throw SerializationException("Unknown Furniture field: " + key);
			}
		}
		void records(FurnitureValue const& array, std::initializer_list<std::string_view> allowed)
		{
			if (!array.IsSequence()) throw SerializationException("Furniture list must be an ordered array");
			for (auto const& value : array) fields(value, allowed);
		}
		void key(FurnitureValue const& value)
		{
			auto text = value.as<std::string>();
			if (text.empty() || text.size() > 128 || text.find_first_not_of(
				"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
				throw SerializationException("Invalid stable Furniture key");
		}
	}

	FurnitureValue readFurnitureLua(std::string const& source)
	{
		ScratchBudget budget(2 * 1024 * 1024, 100'000);
		std::unique_ptr<lua_State, StateCloser> state(lua_newstate(budgetedAllocate, &budget));
		if (!state) throw SerializationException("Cannot allocate Furniture sandbox");
		lua_atpanic(state.get(), luaPanic);
		ModuleLoader loader;
		loader.hostModuleVersions.emplace("promethium.actions.v1", 1);
		std::string diagnostic;
		if (runScratchSetup(state.get(), &loader, nullptr, diagnostic) != LUA_OK)
			throw SerializationException(diagnostic);
		Conversion call{source, {}, {}, 0};
		lua_settop(state.get(), 0);
		lua_pushcfunction(state.get(), evaluate); lua_pushlightuserdata(state.get(), &call);
		auto result = protectedCall(state.get(), budget, 1, 0);
		if (!result.succeeded) throw SerializationException(result.diagnostic.substr(0, 2048));
		return std::move(call.root);
	}

	void validateFurnitureLua(FurnitureValue const& root)
	{
		fields(root, {"api_version", "uuid", "definitions"});
		if (root["api_version"].as<int>() != 1) throw SerializationException("Furniture requires api_version = 1");
		records(root["definitions"], {"key", "label", "tiles", "usablePoints", "sideRoutes", "vertices", "edges", "use", "finish_use"});
		if (root["definitions"].size() > 256) throw SerializationException("Too many Furniture definitions");
		for (auto const& definition : root["definitions"])
		{
			key(definition["key"]);
			records(definition["tiles"], {"x", "y", "imageSet", "image"});
			records(definition["usablePoints"], {"key", "label", "x", "y", "blocksPathing", "supportElevation"});
			for (auto const& point : definition["usablePoints"]) key(point["key"]);
			if (definition["vertices"])
			{
				records(definition["vertices"], {"key", "x", "y", "usablePoint", "external"});
				for (auto const& vertex : definition["vertices"]) key(vertex["key"]);
			}
			if (definition["edges"]) records(definition["edges"], {"from", "to", "depthOffset"});
			bool use = static_cast<bool>(definition["use"]), finish = static_cast<bool>(definition["finish_use"]);
			if (use != finish || (use && (definition["use"].kind != FurnitureValue::Function
				|| definition["finish_use"].kind != FurnitureValue::Function || definition["usablePoints"].size() == 0)))
				throw SerializationException("Furniture use and finish_use must both be Lua functions, or both absent; use requires a usable point");
		}
	}
}
