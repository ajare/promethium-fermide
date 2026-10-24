#include "core/StationConfigurationDocument.h"
#include "core/StationReference.h"
#include "core/SerializationException.h"
#include "core/TransactionalFileWriter.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>

namespace core
{
	std::string serializeStationConfiguration(StationConfigurationDocument const& document)
	{
		auto output = YamlSerializer::toString();
		output->beginMap("stationConfiguration");
		// Keep feature-absent documents byte-identical; schema 112 is required
		// only when the new opt-in contract actually travels on the wire, and
		// schema 113 only when a loop carries the Condition model (#840).
		bool const faults = std::any_of(document.settings.coolantLoops.begin(), document.settings.coolantLoops.end(),
			[](auto const& loop) { return loop.coolantLeak.has_value(); });
		bool const condition = std::any_of(document.settings.coolantLoops.begin(), document.settings.coolantLoops.end(),
			[](auto const& loop) { return loop.condition.has_value(); });
		auto const trips = std::any_of(document.settings.sabatierReactors.begin(), document.settings.sabatierReactors.end(),
			[](auto const& s) { return s.overtempRunaway.has_value(); })
			|| std::any_of(document.settings.methaneProcessors.begin(), document.settings.methaneProcessors.end(),
				[](auto const& s) { return s.overtempRunaway.has_value(); });
		output->writeUint32("schemaVersion", trips ? 114 : condition ? 113 : faults ? 112 : 111);
		writeStationSystemsSettingsMap(*output, document.settings);
		output->beginMap("layout");
		output->writeFloat("panX", document.panX);
		output->writeFloat("panY", document.panY);
		output->writeFloat("zoom", document.zoom);
		output->beginArray("nodes");
		for (auto const& [key, position] : document.positions)
		{
			output->beginMap("");
			output->writeString("key", key);
			output->writeFloat("x", position.first);
			output->writeFloat("y", position.second);
			output->endMap();
		}
		output->endArray();
		output->endMap();
		// #814: the provenance block. Written only when units were placed -
		// a document without prefabs keeps the pre-814 shape verbatim.
		if (!document.prefabs.empty())
		{
			output->beginArray("prefabs");
			for (auto const& provenance : document.prefabs)
			{
				output->beginMap("");
				output->writeString("prefabId", provenance.prefabId);
				output->writeString("instanceId", provenance.instanceId);
				output->beginArray("overrides");
				for (auto const& [name, value] : provenance.overrides)
				{
					output->beginMap("");
					output->writeString("name", name);
					if (auto const* number = std::get_if<double>(&value))
					{
						output->writeString("kind", "number");
						output->writeDouble("value", *number);
					}
					else
					{
						output->writeString("kind", "boolean");
						output->writeBool("value", std::get<bool>(value));
					}
					output->endMap();
				}
				output->endArray();
				output->endMap();
			}
			output->endArray();
		}
		output->endMap();
		output->serialize();
		return output->getSerializedString();
	}

	bool parseStationConfiguration(std::string const& yaml, StationConfigurationDocument& document,
		std::string* diagnostic)
	{
		try
		{
			auto input = YamlSerializer::fromString(yaml);
			input->deserialize();
			if (!input->hasField("stationSystems"))
				throw SerializationException("Missing stationSystems map");
			input->beginMap("stationConfiguration");
			auto const version = input->readUint32("schemaVersion");
			if (version < 96 || version > 114) throw SerializationException("Unsupported Station configuration schemaVersion");
			StationConfigurationDocument candidate;
			readStationSystemsSettingsMap(*input, version, candidate.settings, false);
			// Incomplete wiring is valid draft state, but all surviving entries
			// still undergo the authoritative physical/settings validation.
			auto validated = candidate.settings;
			std::set<std::string> transferIds;
			if (validated.transfers.size() > MaxStationResourceTransfers)
				throw SerializationException("Too many Station transfers");
			for (auto it = validated.transfers.begin(); it != validated.transfers.end(); )
			{
				auto exists = [&](std::string const& id) {
					return std::any_of(validated.stores.begin(), validated.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == id; });
				};
				if (!transferIds.insert(it->id).second)
					throw SerializationException("Duplicate Station transfer id");
				if (!exists(it->sourceId) || !exists(it->destinationId))
				{
					std::string diagnosticForTransfer;
					if (it->id.empty() || it->id.size() > MaxStationStoreIdLength
						|| (!it->sourceId.empty() && it->sourceId == it->destinationId)
						|| !validateStationTransferSettings(*it, diagnosticForTransfer)
						|| std::find(StationStoreUnits.begin(), StationStoreUnits.end(), it->unit)
							== StationStoreUnits.end())
						throw SerializationException("Invalid broken Station transfer");
					auto partial = *it;
					if (!exists(partial.sourceId)) partial.sourceId.clear();
					if (!exists(partial.destinationId)) partial.destinationId.clear();
					std::string wiringError;
					if (!validateStationTransferWiring(validated, partial, wiringError, true))
						throw SerializationException(wiringError);
					it = validated.transfers.erase(it);
				}
				else ++it;
			}
			std::string validationError;
			// Draft-tolerant validation (#642): entries whose references do
			// not resolve are recorded and skipped rather than refusing the
			// file; the draft keeps them and the Flow canvas shows them as
			// broken. Every other rule still refuses, and Apply settings
			// re-validates the whole configuration strictly against the World.
			std::vector<std::string> brokenReferences;
			if (!validateStationSystemsSettings(validated, validationError, &brokenReferences))
				throw SerializationException(validationError);
			if (input->hasField("layout"))
			{
				input->beginMap("layout");
				candidate.panX = input->readFloat("panX");
				candidate.panY = input->readFloat("panY");
				candidate.zoom = input->readFloat("zoom");
				if (!std::isfinite(candidate.panX) || !std::isfinite(candidate.panY)
					|| !std::isfinite(candidate.zoom) || candidate.zoom < 0.001f || candidate.zoom > 1.5f)
					throw SerializationException("Invalid Station configuration view state");
				if (input->hasField("nodes"))
				{
					input->beginArray("nodes");
					while (input->nextArrayItem())
					{
						input->beginMap("");
						auto key = input->readString("key");
						auto x = input->readFloat("x"), y = input->readFloat("y");
						if (key.empty() || !std::isfinite(x) || !std::isfinite(y)
							|| !candidate.positions.emplace(std::move(key), std::pair{x, y}).second)
							throw SerializationException("Invalid or duplicate Station layout node");
						input->endMap();
					}
					input->endArray();
				}
				input->endMap();
			}
			// #814: the provenance block. Boundary discipline: a document
			// stamped before schema 111 that carries the block is refused as
			// conversion-required rather than silently dropping the
			// provenance the author placed.
			if (input->hasField("prefabs"))
			{
				if (version < 111)
					throw SerializationException(
						"Prefab provenance requires World schema 111; conversion required");
				input->beginArray("prefabs");
				std::set<std::string> provenanceInstances;
				while (input->nextArrayItem())
				{
					input->beginMap("");
					PrefabProvenance provenance;
					provenance.prefabId = input->readString("prefabId");
					provenance.instanceId = input->readString("instanceId");
					if (provenance.prefabId.empty() || provenance.instanceId.empty()
						|| !provenanceInstances.insert(provenance.instanceId).second)
						throw SerializationException("Invalid or duplicate prefab provenance");
					if (input->hasField("overrides"))
					{
						input->beginArray("overrides");
						std::set<std::string> overrideNames;
						while (input->nextArrayItem())
						{
							input->beginMap("");
							auto const name = input->readString("name");
							auto const kind = input->readString("kind");
							if (name.empty() || !overrideNames.insert(name).second)
								throw SerializationException(
									"Invalid or duplicate prefab override in instance '"
									+ provenance.instanceId + "'");
							if (kind == "number")
							{
								auto const value = input->readDouble("value");
								if (!std::isfinite(value))
									throw SerializationException("Non-finite prefab override '"
										+ name + "'");
								provenance.overrides.emplace(name, PrefabOverrideValue{value});
							}
							else if (kind == "boolean")
								provenance.overrides.emplace(name,
									PrefabOverrideValue{input->readBool("value")});
							else
								throw SerializationException("Unknown prefab override kind '" + kind
									+ "' for parameter '" + name + "'");
							input->endMap();
						}
						input->endArray();
					}
					candidate.prefabs.push_back(std::move(provenance));
					input->endMap();
				}
				input->endArray();
			}
			input->endMap();
			document = std::move(candidate);
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (std::exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
	}

	void saveStationConfiguration(std::string const& path, StationConfigurationDocument const& document)
	{
		writeFileTransactionally(path, serializeStationConfiguration(document));
	}

	bool loadStationConfiguration(std::string const& path, StationConfigurationDocument& document,
		std::string* diagnostic)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			if (diagnostic) *diagnostic = "Could not open Station configuration: " + path;
			return false;
		}
		std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		if (file.bad())
		{
			if (diagnostic) *diagnostic = "Could not read Station configuration: " + path;
			return false;
		}
		return parseStationConfiguration(bytes, document, diagnostic);
	}
}
